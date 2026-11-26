#include "v2c/v2c_transcoder.h"

#include <bit>
#include <chrono>
#include <exception>
#include <iostream>
#include <limits>

namespace {

using namespace std::chrono;
using namespace can;

constexpr can_time start{1s};

can_frame input(canid_t id, uint8_t len, uint64_t raw) {
	can_frame frame{};
	frame.can_id = id;
	frame.len = len;
	std::memcpy(frame.data, &raw, sizeof(raw));
	std::cout << "  input id=" << id << " len=" << unsigned(len) << " raw=0x" << std::hex << raw << std::dec << '\n';
	return frame;
}

std::vector<can_frame> frames(const frame_packet& packet) {
	std::vector<can_frame> result;
	if (!packet.empty()) {
		for (const auto& [stamp, frame] : packet) {
			result.push_back(frame);
			std::cout << "  output id=" << frame.can_id << " len=" << unsigned(frame.len)
				<< " common=" << use_non_muxed(result.back())
				<< " raw=0x" << std::hex << std::bit_cast<uint64_t>(frame.data) << std::dec << '\n';
		}
	}
	return result;
}

void add_signal(tr_message& message, std::string name, unsigned start_bit, unsigned width, char sign,
	std::optional<int64_t> mux = {}) {
	message.add_signal(tr_signal{std::move(name), sig_codec{start_bit, width, '1', sign}, mux});
}

bool mux_at_bit_four() {
	std::cout << "selector in bits 4..5, expected output payloads 0x2b and 0x17\n";
	tx_group group{"Test", 100};
	group.time_begin(start);
	tr_message message;
	message.add_muxer(tr_muxer{sig_codec{4, 2, '1', '+'}});
	add_signal(message, "A", 0, 4, '+', 1);
	add_signal(message, "B", 0, 4, '+', 2);
	message.assign_group(&group, 1);
	message.assemble(start, input(1, 1, 0x17));
	message.assemble(start + 1ms, input(1, 1, 0x2b));
	frame_packet packet;
	packet.prepare(1);
	group.try_publish(start + 100ms, packet);
	const auto observed = frames(packet);
	return observed.size() == 2 && observed[0].data[0] == 0x2b && observed[1].data[0] == 0x17;
}

bool frame_metadata(canid_t id, bool check_common) {
	std::cout << (check_common ? "CAN ID 0 must carry common signals\n" : "output DLC must match the two-byte input\n");
	tx_group group{"Test", 100};
	group.time_begin(start);
	tr_message message;
	add_signal(message, "Value", 0, 8, '+');
	message.assign_group(&group, id);
	message.assemble(start, input(id, 2, 7));
	frame_packet packet;
	packet.prepare(1);
	group.try_publish(start + 100ms, packet);
	auto observed = frames(packet);
	return observed.size() == 1 && (check_common ? use_non_muxed(observed[0]) : observed[0].len == 2);
}

bool delayed_aggregation_attribute() {
	constexpr std::string_view dbc = R"dbc(VERSION ""
BS_:
BU_: Receiver
BO_ 1 Data: 1 Receiver
 SG_ Value : 0|8@1+ (1,0) [0|255] "" Receiver
EV_ V2CTxTime: 0 [0|60000] "ms" 200 1 DUMMY_NODE_VECTOR1 Receiver;
EV_ TestGroupTxFreq: 0 [0|60000] "ms" 100 2 DUMMY_NODE_VECTOR1 Receiver;
BA_DEF_ BO_ "TxGroupFreq" STRING ;
BA_DEF_ SG_ "AggType" STRING ;
BA_ "TxGroupFreq" BO_ 1 "TestGroupTxFreq";
BA_ "AggType" SG_ 1 Value "AVG";
)dbc";
	v2c_transcoder transcoder{};
	const bool parsed = parse_dbc(dbc, std::ref(transcoder));
	std::cout << "DBC parsed=" << parsed << " with group attribute before AVG, expected mean 6\n";
	if (!parsed)
		return false;
	transcoder.transcode(start, input(1, 1, 4));
	transcoder.transcode(start + 50ms, input(1, 1, 8));
	transcoder.transcode(start + 100ms, input(99, 0, 0));
	const auto observed = frames(transcoder.transcode(start + 200ms, input(99, 0, 0)));
	return observed.size() == 1 && observed[0].data[0] == 6;
}

bool average(std::string_view scenario, char sign, uint64_t a, uint64_t b, uint64_t expected) {
	std::cout << scenario << ", expected mean 0x" << std::hex << expected << std::dec << '\n';
	tx_group group{"Test", 100};
	group.time_begin(start);
	tr_message message;
	add_signal(message, "Value", 0, 64, sign);
	message.sig_agg_type("Value", "AVG");
	message.assign_group(&group, 1);
	message.assemble(start, input(1, 8, a));
	message.assemble(start + 1ms, input(1, 8, b));
	frame_packet packet;
	packet.prepare(1);
	group.try_publish(start + 100ms, packet);
	const auto observed = frames(packet);
	return observed.size() == 1 && std::bit_cast<uint64_t>(observed[0].data) == expected;
}

bool last_byte_signal() {
	constexpr uint64_t raw = 0xab00000000000000ull;
	const tr_signal signal{"LastByte", sig_codec{56, 8, '1', '+'}, {}};
	const auto actual = signal.decode(raw);
	std::cout << "8-byte payload, decode byte 7 expected=171 actual=" << actual << '\n';
	return actual == 171;
}

bool float_raw_conversion() {
	constexpr auto raw = std::bit_cast<uint32_t>(1.5f);
	const sig_calc_type<float> signal{raw};
	const auto actual = signal.get_raw();
	std::cout << "encode 1.5f expected=0x" << std::hex << raw << " actual=0x" << actual << std::dec << '\n';
	return actual == raw;
}

bool ignores_id_zero_without_vin() {
	std::cout << "unconfigured VIN assembly must ignore VehicleSpeed on CAN ID 0\n";
	vin_assembler assembler{};
	tr_message message;
	add_signal(message, "VehicleSpeed", 0, 8, '+');
	try {
		return !assembler.decode_some(message, input(0, 1, 42)) && assembler.empty();
	} catch (const std::exception& error) {
		std::cerr << "Unexpected VIN decode of VehicleSpeed: " << error.what() << '\n';
		return false;
	}
}

bool accepts_vin_character_with_other_fields() {
	std::cout << "VIN on CAN ID 0 must accept VIN_1 and ignore Index and VINChecksum\n";
	vin_assembler assembler{};
	assembler.vin_message_id(0);
	tr_message message;
	add_signal(message, "Index", 8, 8, '+');
	add_signal(message, "VINChecksum", 16, 8, '+');
	const auto frame = input(0, 3, 0xa00541);
	try {
		const bool metadata_ignored = !assembler.decode_some(message, frame);
		add_signal(message, "VIN_1", 0, 8, '+');
		const bool character_added = assembler.decode_some(message, frame);
		const bool repeated = assembler.decode_some(message, frame);
		return metadata_ignored && character_added && !repeated && assembler.empty();
	} catch (const std::exception& error) {
		std::cerr << "Unexpected VIN decode error: " << error.what() << '\n';
		return false;
	}
}

} // namespace

int main() {
	constexpr uint64_t largest_signed = std::numeric_limits<int64_t>::max();
	const bool mux = mux_at_bit_four();
	const bool zero = frame_metadata(0, true);
	const bool length = frame_metadata(1, false);
	const bool attributes = delayed_aggregation_attribute();
	const bool unsigned_type = average("unsigned DBC signal without SIG_VALTYPE_", '+', 0, 0x8000000000000002ull,
		0x4000000000000001ull);
	const bool wide_sum = average("sum of two largest signed samples", '-', largest_signed, largest_signed, largest_signed);
	const bool round = average("midpoint after a representable signed sum", '-', largest_signed, 0,
		0x4000000000000000ull);
	const bool last_byte = last_byte_signal();
	const bool float_raw = float_raw_conversion();
	const bool no_vin = ignores_id_zero_without_vin();
	const bool vin_fields = accepts_vin_character_with_other_fields();
	return mux && zero && length && attributes && unsigned_type && wide_sum && round && last_byte && float_raw &&
	       no_vin && vin_fields ? 0 : 1;
}
