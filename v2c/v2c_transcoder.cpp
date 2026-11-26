#include <unordered_map>
#include <chrono>
#include <bit>
#include <charconv>

#include "can/can_codec.h"
#include "v2c_transcoder.h"

namespace can {

template <typename T>
class sig_last {
	const tr_signal _sig;
	sig_calc_type<T> _val { 0 };
public:
	sig_last(const tr_signal sig) : _sig(sig) {}

	friend uint64_t tag_invoke(assemble_cpo, sig_last& self, uint64_t mux_val, uint64_t fd) {
		if (!self._sig.is_active(mux_val))
			return 0;

		sig_calc_type<T> val(self._sig.decode(fd));
		self._val = val;
		return self._sig.encode(val.get_raw());
	}

	friend void tag_invoke(reset_cpo, sig_last&) { }
};

template <typename T>
class sig_avg {
	const tr_signal _sig;
	sig_calc_type<T> _val { 0 };
	uint64_t _num_samples { 0 };
public:
	sig_avg(const tr_signal sig) : _sig(sig) {}

	friend uint64_t tag_invoke(assemble_cpo, sig_avg& self, uint64_t mux_val, uint64_t fd) {
		if (!self._sig.is_active(mux_val))
			return 0;

		sig_calc_type<T> val(self._sig.decode(fd));
		self._val = (self._num_samples == 0) ? val : self._val + val;
		++self._num_samples;

		return self._sig.encode(self._val.idivround(self._num_samples).get_raw());
	}

	friend void tag_invoke(reset_cpo, sig_avg& self) {
		self._val = 0;
		self._num_samples = 0;
	}
};

static int32_t millis_diff(can_time tp, uint32_t utc) {
	using namespace std::chrono;
	return duration_cast<milliseconds>(tp - can_time(seconds(utc))).count();
}

void tx_group::try_publish(can_time up_to, frame_packet& fp) {
	const auto end = window_end();
	if (_assemble_freq <= std::chrono::milliseconds::zero() || up_to < end)
		return;

	if (all_collected())
		publish(end, fp);

	// There were no new samples in the intervening windows.
	_group_origin += _assemble_freq * ((up_to - _group_origin) / _assemble_freq);
}

void tx_group::publish(can_time tp, frame_packet& fp) {
	// Use the latest frame for common signals of a multiplexed message,
	// with receive order breaking timestamp ties.

	std::sort(_msg_clumps.begin(), _msg_clumps.end(), [](const auto& a, const auto& b) {
		if (a.message_id != b.message_id) // primarily by message_ids
			return a.message_id < b.message_id;
		if (a.stamp != b.stamp)
			return a.stamp > b.stamp;
		return a.receive_order > b.receive_order;
	});

	std::optional<canid_t> prev_id;
	for (const auto& smsg : _msg_clumps) {
		can_frame cf { 0 };
		cf.can_id = smsg.message_id;
		cf.len = smsg.len;
		can::use_non_muxed(cf, prev_id != cf.can_id);
		prev_id = cf.can_id;
		auto raw_data = smsg.mdata;
		std::memcpy(cf.data, &raw_data, CAN_MAX_DLEN);
		fp.append(millis_diff(tp, fp.utc()));
		fp.append(cf);
	}
}

bool tx_group::all_collected() const {
	return std::ranges::all_of(_msg_clumps, [this](const auto& smsg) {
		return smsg.stamp && within_interval(*smsg.stamp);
	});
}

// used only to assemble messages

void tx_group::assign(canid_t message_id, int64_t message_mux) {
	_msg_clumps.push_back({ .message_id = message_id, .message_mux = message_mux });
}

bool tx_group::within_interval(can_time stamp) const {
	return stamp >= _group_origin && stamp < window_end();
}

void tx_group::add_clumped(can_time stamp, canid_t message_id, int64_t message_mux, uint64_t cval, uint8_t len) {
	for (auto& smsg : _msg_clumps) {
		if (smsg.message_id == message_id && smsg.message_mux == message_mux) {
			smsg.stamp = stamp;
			smsg.receive_order = _next_receive_order++;
			smsg.mdata = cval;
			smsg.len = len;
			break;
		}
	}
}

template <template <typename> typename sig_agg>
static sig_asm make_sig_agg(const tr_signal& sig) {
	switch (sig.value_type()) {
		case i64: return sig_asm(sig_agg<int64_t>(sig));
		case u64: return sig_asm(sig_agg<uint64_t>(sig));
		case f32: return sig_asm(sig_agg<float>(sig));
		case f64: return sig_asm(sig_agg<double>(sig));
	}
	fprintf(stderr, "Signal %s doesn't have a valid value_type\n", sig.name().c_str());
	return { sig_avg<int64_t>(sig) };
}

std::vector<uint64_t> tr_message::distinct_mux_vals() const {
	std::vector<uint64_t> mux_vals;

	for (const auto& sig : _signals) {
		if (sig.mux_val().has_value())
			mux_vals.push_back(sig.mux_val().value());
	}

	std::sort(mux_vals.begin(), mux_vals.end());
	mux_vals.erase(std::unique(mux_vals.begin(), mux_vals.end()), mux_vals.end());

	return mux_vals;
}

void tr_message::assign_group(tx_group* txg, uint32_t message_id) {
	_tx_group = txg;
	_sig_asms.reset();

	if (_mux.has_value())
		for (const auto mux_val : distinct_mux_vals())
			_tx_group->assign(message_id, mux_val);
	else
		_tx_group->assign(message_id, -1);
}

void tr_message::assemble(can_time stamp, can_frame frame) {
	if (!_tx_group) return;
	if (!_sig_asms)
		make_sig_assemblers();

	if (!_tx_group->within_interval(_last_stamp))
		reset_sig_asms();

	uint64_t clumped_val = 0;
	uint64_t fd = std::bit_cast<uint64_t>(frame.data);
	int64_t mux_val = _mux.has_value() ? _mux->decode(fd) : -1;

	for (auto& sc : *_sig_asms)
		clumped_val |= sig_assemble(sc, mux_val, fd);

	if (_mux.has_value())
		clumped_val |= _mux->encode(mux_val);

	_tx_group->add_clumped(stamp, frame.can_id, mux_val, clumped_val, frame.len);

	_last_stamp = stamp;
}

void tr_message::make_sig_assemblers() {
	auto& assemblers = _sig_asms.emplace();
	for (const tr_signal& sig : _signals) {
		std::string_view atype = sig.agg_type();
		sig_asm sasm;

		if (atype == "LAST")
			sasm = make_sig_agg<sig_last>(sig);
		else if (atype == "AVG")
			sasm = make_sig_agg<sig_avg>(sig);
		else continue;

		assemblers.emplace_back(std::move(sasm));
	}
}

void tr_message::reset_sig_asms() {
	for (auto& sc : *_sig_asms)
		sig_reset(sc);
}

tr_signal* tr_message::find_signal(const std::string& sig_name) {
	auto sig_it = std::find_if(_signals.begin(), _signals.end(), [&](const auto& s) { return s.name() == sig_name; });
	return sig_it == _signals.end() ? nullptr : &(*sig_it);
}

void tr_message::sig_agg_type(const std::string& sig_name, const std::string& agg_type) {
	if (auto sig = find_signal(sig_name); sig)
		sig->agg_type(agg_type);
}

void tr_message::sig_val_type(const std::string& sig_name, unsigned sig_ext_val_type) {
	if (auto sig = find_signal(sig_name); sig)
		sig->value_type(sig_ext_val_type);
}

void tr_message::add_signal(tr_signal sig) {
	_signals.push_back(std::move(sig));
}

void tr_message::add_muxer(tr_muxer mux) {
	_mux = std::move(mux);
}

bool vin_assembler::decode_some(const can::tr_message& msg, can_frame frame) {
	if (!_vin_msg_id || frame.can_id != *_vin_msg_id)
		return false;
	auto old_bits = _cbits;
	uint64_t fd = std::bit_cast<uint64_t>(frame.data);
	for (const auto& sig : msg.signals(fd)) {
		int chidx = vin_char(sig.name());
		if (chidx == 0) continue;
		_vin[chidx - 1] = char(sig.decode(fd));
		_cbits |= (1 << (chidx - 1));
	}
	return _cbits != old_bits;
}

int vin_assembler::vin_char(std::string_view sig_name) {
	if (!sig_name.starts_with("VIN"))
		return 0;

	const auto digits = sig_name.substr(sig_name.find_last_not_of("0123456789") + 1);
	const auto* end = digits.data() + digits.size();
	unsigned position = 0;
	const auto parsed = std::from_chars(digits.data(), end, position);
	if (parsed.ec != std::errc{} || parsed.ptr != end || position > vin_len)
		return 0;
	return static_cast<int>(position);
}

frame_packet v2c_transcoder::transcode(can_time stamp, can_frame frame) {
	using namespace std::chrono;

	setup_timers(stamp);

	if (_next_update_tp <= stamp)
		store_assembled(stamp);

	frame_packet rv {};

	if (_publish_freq <= 0ms || stamp >= *_packet_origin + _publish_freq) {
		if (!_frame_packet.empty())
			rv = std::move(_frame_packet);
		if (_publish_freq <= 0ms)
			_packet_origin = stamp;
		else
			*_packet_origin += _publish_freq * ((stamp - *_packet_origin) / _publish_freq);
		_frame_packet.prepare(duration_cast<seconds>(_packet_origin->time_since_epoch()).count());
	}

	auto mi = _msgs.find(frame.can_id);
	if (mi != _msgs.end()) {
		_vin.decode_some(mi->second, frame);
		mi->second.assemble(stamp, frame);
	}

	return rv;
}

frame_packet v2c_transcoder::drain() {
	if (_frame_packet.empty())
		return {};

	auto packet = std::move(_frame_packet);
	_frame_packet.prepare(packet.utc());
	return packet;
}

void v2c_transcoder::setup_timers(can_time first_stamp) {
	using namespace std::chrono;

	if (_packet_origin)
		return;

	_frame_packet.prepare(duration_cast<seconds>(first_stamp.time_since_epoch()).count());
	_packet_origin = first_stamp;

	for (auto& txg : _tx_groups) {
		txg->time_begin(first_stamp);
		_next_update_tp = std::min(_next_update_tp, txg->window_end());
	}
}

void v2c_transcoder::store_assembled(can_time up_to) {
	std::vector<tx_group*> due;
	_next_update_tp = can_time::max();
	for (const auto& txg : _tx_groups) {
		if (txg->window_end() <= up_to)
			due.push_back(txg.get());
		else
			_next_update_tp = std::min(_next_update_tp, txg->window_end());
	}
	std::ranges::stable_sort(due, {}, &tx_group::window_end);
	for (auto* txg : due) {
		txg->try_publish(up_to, _frame_packet);
		_next_update_tp = std::min(_next_update_tp, txg->window_end());
	}
}

// helper methods for dbc_parser, to initialize the transcoder structures:

tr_message* v2c_transcoder::find_message(canid_t message_id) {
	auto msg_it = _msgs.find(message_id);
	return msg_it == _msgs.end() ? nullptr : &(msg_it->second);
}

void v2c_transcoder::assign_tx_group(
	const std::string& object_type, unsigned message_id, const std::string& tx_group
) {
	auto grp_it = std::find_if(_tx_groups.begin(), _tx_groups.end(),
		[&](const auto& g) { return g->name() == tx_group; });

	if (grp_it == _tx_groups.end())
		return;

	if (auto msg_ptr = find_message(message_id); msg_ptr)
		msg_ptr->assign_group(grp_it->get(), message_id);
}

void v2c_transcoder::add_signal(canid_t message_id, tr_signal sig) {
	if (auto msg_ptr = find_message(message_id); msg_ptr)
		msg_ptr->add_signal(std::move(sig));
}

void v2c_transcoder::add_muxer(canid_t message_id, tr_muxer mux) {
	if (auto msg_ptr = find_message(message_id); msg_ptr)
		msg_ptr->add_muxer(std::move(mux));
}

void v2c_transcoder::add_message(canid_t message_id, std::string_view message_name) {
	if (message_name == "VIN")
		_vin.vin_message_id(message_id);
	_msgs.try_emplace(message_id);
}

void v2c_transcoder::set_env_var(const std::string& name, int64_t ev_value) {
	using namespace std::chrono;

	if (name == "V2CTxTime") {
		_publish_freq = milliseconds(ev_value);
	}
	else if (name.ends_with("GroupTxFreq") && ev_value > 0 && ev_value <= UINT32_MAX) {
		_tx_groups.emplace_back(new tx_group(name, ev_value));
	}
}

void v2c_transcoder::set_sig_val_type(canid_t message_id, const std::string& sig_name, unsigned sig_ext_val_type) {
	if (auto msg_ptr = find_message(message_id); msg_ptr)
		msg_ptr->sig_val_type(sig_name, sig_ext_val_type);
}

void v2c_transcoder::set_sig_agg_type(canid_t message_id, const std::string& sig_name, const std::string& agg_type) {
	if (auto msg_ptr = find_message(message_id); msg_ptr)
		msg_ptr->sig_agg_type(sig_name, agg_type);
}

} // end namespace can
