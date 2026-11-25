#include "v2c/v2c_transcoder.h"

#include <chrono>
#include <iostream>
#include <vector>

namespace {

using namespace std::chrono;
using namespace can;

struct sample {
	milliseconds stamp;
	canid_t id;
	uint8_t value;
	bool operator==(const sample&) const = default;
};

bool expect(std::string_view step, const frame_packet& packet, std::initializer_list<sample> wanted) {
	std::vector<sample> actual;
	if (!packet.empty()) {
		for (const auto& [stamp, frame] : packet)
			actual.push_back({ duration_cast<milliseconds>(stamp.time_since_epoch()), frame.can_id, frame.data[0] });
	}
	std::cout << step << " emitted " << actual.size() << " frame(s)";
	for (const auto& row : actual)
		std::cout << " [" << row.stamp.count() << "ms id=" << row.id << " data=" << unsigned(row.value) << ']';
	std::cout << '\n';
	if (std::ranges::equal(actual, wanted))
		return true;

	std::cerr << step << " expected " << wanted.size() << " frame(s)";
	for (const auto& row : wanted)
		std::cerr << " [" << row.stamp.count() << "ms id=" << row.id << " data=" << unsigned(row.value) << ']';
	std::cerr << '\n';
	return false;
}

can_frame frame(canid_t id, uint8_t data = 0) {
	can_frame cf{};
	cf.can_id = id;
	cf.len = 1;
	cf.data[0] = data;
	return cf;
}

tr_signal signal() {
	return tr_signal{ "Value", sig_codec{ 0, 8, '1', '+' }, {} };
}

void assign(v2c_transcoder& transcoder, std::string group, canid_t id) {
	transcoder.add_message(id, "Status");
	transcoder.add_signal(id, signal());
	transcoder.assign_tx_group("BO_", id, group);
}

bool epoch_origin() {
	v2c_transcoder transcoder{};
	transcoder.set_env_var("V2CTxTime", 100);
	transcoder.set_env_var("FastGroupTxFreq", 100);
	assign(transcoder, "FastGroupTxFreq", 1);
	transcoder.transcode(can_time{0ms}, frame(1, 7));
	return expect("0ms data, 100ms boundary", transcoder.transcode(can_time{100ms}, frame(99)), {{100ms, 1, 7}});
}

bool subsecond_packet_start() {
	v2c_transcoder transcoder{};
	transcoder.set_env_var("V2CTxTime", 600);
	transcoder.set_env_var("FastGroupTxFreq", 100);
	assign(transcoder, "FastGroupTxFreq", 1);
	transcoder.transcode(can_time{1250ms}, frame(1, 7));
	const bool before = expect("1250ms data, 1799ms before publication boundary",
		transcoder.transcode(can_time{1799ms}, frame(99)), {});
	const bool at = expect("1850ms publication boundary",
		transcoder.transcode(can_time{1850ms}, frame(99)), {{1350ms, 1, 7}});
	return before && at;
}

bool sparse_groups() {
	v2c_transcoder transcoder{};
	transcoder.set_env_var("V2CTxTime", 500);
	transcoder.set_env_var("SlowGroupTxFreq", 250);
	transcoder.set_env_var("FastGroupTxFreq", 100);
	assign(transcoder, "FastGroupTxFreq", 1);
	assign(transcoder, "SlowGroupTxFreq", 2);
	transcoder.transcode(can_time{1000ms}, frame(1, 7));
	transcoder.transcode(can_time{1000ms}, frame(2, 9));
	const bool first = expect("1000ms data, 1500ms jump crosses both group deadlines",
		transcoder.transcode(can_time{1500ms}, frame(1, 5)), {{1100ms, 1, 7}, {1250ms, 2, 9}});
	const bool second = expect("1500ms data, 2000ms jump crosses empty group windows",
		transcoder.transcode(can_time{2000ms}, frame(99)), {{1600ms, 1, 5}});
	return first && second;
}

bool missing_member_at_epoch() {
	tx_group group{ "Fast", 100 };
	tr_message a, b;
	a.add_signal(signal());
	b.add_signal(signal());
	a.assign_group(&group, 1);
	b.assign_group(&group, 2);
	group.time_begin(can_time{0ms});
	frame_packet packet;
	packet.prepare(0);
	a.assemble(can_time{0ms}, frame(1, 7));
	group.try_publish(can_time{100ms}, packet);
	return expect("0ms data for id=1 only, 100ms group boundary", packet, {});
}

bool previous_window_sample() {
	const can_time start{1000125us};
	tx_group group{ "Fast", 1 };
	tr_message message;
	message.add_signal(signal());
	message.assign_group(&group, 1);
	group.time_begin(start);
	frame_packet packet;
	packet.prepare(1);
	message.assemble(start + 500us, frame(1, 9));
	group.try_publish(start + 1ms, packet);
	group.try_publish(start + 2ms, packet);
	return expect("1000625us data, two group boundaries at 1001125us and 1002125us", packet, {{1001ms, 1, 9}});
}

bool no_active_group(bool configure_zero) {
	v2c_transcoder transcoder{};
	transcoder.set_env_var("V2CTxTime", 100);
	if (configure_zero) {
		transcoder.set_env_var("FastGroupTxFreq", 0);
		assign(transcoder, "FastGroupTxFreq", 1);
	}
	std::cerr << "1000ms id=1 received with " << (configure_zero ? "a zero-period group" : "no groups") << '\n';
	return expect("inactive groups", transcoder.transcode(can_time{1000ms}, frame(1, 7)), {});
}

} // namespace

int main(int argc, char** argv) {
	if (argc > 1)
		return no_active_group(std::string_view{argv[1]} == "zero-group") ? 0 : 1;

	const bool epoch = epoch_origin();
	const bool subsecond = subsecond_packet_start();
	const bool sparse = sparse_groups();
	const bool missing = missing_member_at_epoch();
	const bool previous = previous_window_sample();
	return epoch && subsecond && sparse && missing && previous ? 0 : 1;
}
