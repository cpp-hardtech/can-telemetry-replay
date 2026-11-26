#include "can/can_codec.h"
#include "dbc/dbc_parser.h"
#include "v2c/v2c_transcoder.h"

#include <algorithm>
#include <array>
#include <bit>
#include <charconv>
#include <chrono>
#include <cmath>
#include <fstream>
#include <functional>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace {

struct EngineeringScale {
    double factor;
    double offset;
    std::string unit;
};

using SelectorRanges = std::vector<std::pair<unsigned, unsigned>>;
using SignalValues = std::vector<std::optional<std::uint64_t>>;
using IdentifierFilter = std::unordered_set<std::uint32_t>;

struct SelectorCondition {
    std::size_t selector;
    SelectorRanges ranges;

    bool matches(const SignalValues& values) const {
        const auto& value = values[selector];
        return value && std::ranges::any_of(ranges, [&](const auto& range) {
            return range.first <= *value && *value <= range.second;
        });
    }
};

struct Signal {
    std::string name;
    can::sig_codec codec;
    std::optional<unsigned> inline_mux_value;
    std::optional<EngineeringScale> scaling;
    std::vector<SelectorCondition> conditions;

    bool is_selector() const { return !scaling; }
};

struct Message {
    std::string name;
    std::size_t size;
    std::vector<Signal> signals;
    std::vector<std::size_t> decode_order;

    std::size_t signal_index(const std::string& signal_name) const {
        const auto it = std::ranges::find(signals, signal_name, &Signal::name);
        if (it == signals.end()) {
            throw std::runtime_error("unknown DBC signal " + name + "." + signal_name);
        }
        return it - signals.begin();
    }

    void add_ranges(const std::string& signal_name, const std::string& selector_name,
                    SelectorRanges ranges) {
        auto& signal = signals[signal_index(signal_name)];
        const auto selector = signal_index(selector_name);
        if (!signals[selector].is_selector()) {
            throw std::runtime_error("expected DBC multiplexor for " + name + "." + signal_name);
        }
        if (std::ranges::any_of(ranges, [](const auto& range) { return range.first > range.second; })) {
            throw std::runtime_error("reversed DBC multiplex range for " + name + "." + signal_name);
        }
        const auto condition = std::ranges::find(signal.conditions, selector, &SelectorCondition::selector);
        if (condition == signal.conditions.end()) {
            signal.conditions.push_back({selector, std::move(ranges)});
        } else {
            condition->ranges.insert(condition->ranges.end(), ranges.begin(), ranges.end());
        }
    }

    void resolve_multiplexing() {
        std::optional<std::size_t> root;
        bool multiple_roots = false;
        for (std::size_t i = 0; i < signals.size(); ++i) {
            const auto& signal = signals[i];
            if (!signal.is_selector()) {
                continue;
            }
            if (signal.codec.sign_type() != '+') {
                throw std::runtime_error("expected unsigned DBC multiplexor for " + name + "." + signal.name);
            }
            if (!signal.inline_mux_value && signal.conditions.empty()) {
                multiple_roots = root.has_value();
                root = i;
            }
        }
        for (auto& signal : signals) {
            if (signal.inline_mux_value && signal.conditions.empty()) {
                if (!root || multiple_roots) {
                    throw std::runtime_error("expected one root DBC multiplexor for " + name + "." + signal.name);
                }
                const auto value = *signal.inline_mux_value;
                signal.conditions.push_back({*root, {{value, value}}});
            }
        }

        // Resolve selectors before their dependents, independently of DBC signal order.
        std::vector<std::vector<std::size_t>> dependents(signals.size());
        std::vector<std::size_t> dependencies(signals.size());
        for (std::size_t i = 0; i < signals.size(); ++i) {
            dependencies[i] = signals[i].conditions.size();
            if (dependencies[i] == 0) {
                decode_order.push_back(i);
            }
            for (const auto& condition : signals[i].conditions) {
                dependents[condition.selector].push_back(i);
            }
        }
        for (std::size_t next = 0; next < decode_order.size(); ++next) {
            for (const auto dependent : dependents[decode_order[next]]) {
                if (--dependencies[dependent] == 0) {
                    decode_order.push_back(dependent);
                }
            }
        }
        if (decode_order.size() != signals.size()) {
            throw std::runtime_error("cyclic DBC multiplexors for " + name);
        }
    }

    void decode_signals(const std::uint8_t* data, SignalValues& values) const {
        values.assign(signals.size(), std::nullopt);
        for (const auto index : decode_order) {
            const auto& signal = signals[index];
            if (std::ranges::all_of(signal.conditions, [&](const auto& condition) {
                    return condition.matches(values);
                })) {
                values[index] = signal.codec(data);
            }
        }
    }
};

bool valid_identifier(std::uint32_t id) {
    return id <= CAN_SFF_MASK || (id & ~CAN_EFF_MASK) == CAN_EFF_FLAG;
}

struct Database {
    std::unordered_map<std::uint32_t, Message> messages;

    struct MultiplexRule {
        std::uint32_t id;
        std::string signal;
        std::string selector;
        SelectorRanges ranges;
    };
    std::vector<MultiplexRule> multiplex_rules;

    friend void tag_invoke(can::def_bo_cpo, Database& db, std::uint32_t id,
                           std::string name, std::size_t size, std::size_t) {
        if (!valid_identifier(id) || size > CAN_MAX_DLEN) {
            throw std::runtime_error("invalid Classic CAN message " + name);
        }
        if (!db.messages.emplace(id, Message{name, size, {}, {}}).second) {
            throw std::runtime_error("duplicate CAN identifier for " + name);
        }
    }

    void add_signal(std::uint32_t id, std::string name, unsigned start, unsigned bits,
                    char order, char sign, std::optional<unsigned> value,
                    std::optional<EngineeringScale> scaling) {
        auto& msg = messages.at(id);
        const unsigned first_byte = start / 8;
        const unsigned offset = order == '1' ? start % 8 : 7 - start % 8;
        if (first_byte >= msg.size || bits == 0 || bits > 64 ||
            (bits + offset + 7) / 8 > msg.size - first_byte) {
            throw std::runtime_error("signal lies outside the payload of " + msg.name + "." + name);
        }
        msg.signals.push_back({std::move(name), can::sig_codec{start, bits, order, sign},
                               value, std::move(scaling), {}});
    }

    friend void tag_invoke(can::def_sg_cpo, Database& db, std::uint32_t id,
                           std::optional<unsigned> value, std::string name, unsigned start,
                           unsigned bits, char order, char sign, double factor, double offset,
                           double, double, std::string unit, std::vector<std::size_t>) {
        db.add_signal(id, std::move(name), start, bits, order, sign, value,
                      EngineeringScale{factor, offset, std::move(unit)});
    }

    friend void tag_invoke(can::def_sg_mux_cpo, Database& db, std::uint32_t id,
                           std::string name, unsigned start, unsigned bits, char order,
                           char sign, std::string, std::vector<std::size_t>) {
        db.add_signal(id, std::move(name), start, bits, order, sign, {}, {});
    }

    friend void tag_invoke(can::def_sg_mux_value_cpo, Database& db, std::uint32_t id,
                           std::string, unsigned value) {
        db.messages.at(id).signals.back().inline_mux_value = value;
    }

    friend void tag_invoke(can::def_sig_valtype_cpo, Database&, unsigned, std::string name,
                           unsigned type) {
        if (type != 0) {
            throw std::runtime_error("floating-point DBC signal is unsupported: " + name);
        }
    }

    friend void tag_invoke(can::def_sg_mul_val_cpo, Database& db, unsigned id, std::string name,
                           std::string selector, SelectorRanges ranges) {
        db.multiplex_rules.push_back({id, std::move(name), std::move(selector), std::move(ranges)});
    }
};

Database load_database(const char* path, can::v2c_transcoder* transcoder) {
    std::ifstream input{path};
    if (!input) {
        throw std::runtime_error(std::string{"cannot open DBC file: "} + path);
    }
    std::string contents{std::istreambuf_iterator<char>{input}, {}};
    Database db;
    if (!can::parse_dbc(contents, std::ref(db)) || db.messages.empty()) {
        throw std::runtime_error(std::string{"expected DBC message definitions in "} + path);
    }
    if (transcoder && !db.multiplex_rules.empty()) {
        throw std::runtime_error("extended DBC multiplexing is unsupported in aggregate replay: " +
                                 db.multiplex_rules.front().signal);
    }
    for (auto& rule : db.multiplex_rules) {
        const auto message = db.messages.find(rule.id);
        if (message == db.messages.end()) {
            throw std::runtime_error("unknown DBC message in multiplex rule for " + rule.signal);
        }
        message->second.add_ranges(rule.signal, rule.selector, std::move(rule.ranges));
    }
    db.multiplex_rules.clear();
    for (auto& [id, msg] : db.messages) {
        if (transcoder && std::ranges::count_if(msg.signals, &Signal::is_selector) > 1) {
            throw std::runtime_error("expected one DBC multiplexor in aggregate replay for " + msg.name);
        }
        msg.resolve_multiplexing();
    }
    if (transcoder && !can::parse_dbc(contents, std::ref(*transcoder))) {
        throw std::runtime_error(std::string{"could not load V2C settings from "} + path);
    }
    return db;
}

template <typename T>
bool parse_number(std::string_view value, T& number, int base) {
    const auto* end = value.data() + value.size();
    const auto result = std::from_chars(value.data(), end, number, base);
    return result.ec == std::errc{} && result.ptr == end;
}

bool parse_identifier(std::string_view text, std::uint32_t& id) {
    const bool extended = text.size() == 8;
    if ((text.size() != 3 && !extended) || !parse_number(text, id, 16) ||
        id > (extended ? CAN_EFF_MASK : CAN_SFF_MASK)) {
        return false;
    }
    if (extended) {
        id |= CAN_EFF_FLAG;
    }
    return true;
}

using Timestamp = std::pair<std::uint64_t, std::uint32_t>;

bool parse_candump_timestamp(std::string_view text, Timestamp& stamp) {
    if (text.size() < 4 || text.front() != '(' || text.back() != ')') {
        return false;
    }
    text.remove_prefix(1);
    text.remove_suffix(1);
    const auto point = text.find('.');
    if (point == std::string_view::npos) {
        return false;
    }
    const auto fraction = text.substr(point + 1);
    if ((fraction.size() != 6 && fraction.size() != 9) ||
        !parse_number(text.substr(0, point), stamp.first, 10) ||
        !parse_number(fraction, stamp.second, 10)) {
        return false;
    }
    if (fraction.size() == 6) {
        stamp.second *= 1000;
    }
    return true;
}

std::string physical_value(double raw, const EngineeringScale& scaling) {
    const double value = std::fma(raw, scaling.factor, scaling.offset);
    if (!std::isfinite(value)) {
        throw std::runtime_error("DBC scaling produced a non-finite value");
    }
    std::array<char, 64> text;
    const auto result = std::to_chars(text.data(), text.data() + text.size(), value);
    if (result.ec != std::errc{}) {
        throw std::runtime_error("could not format a physical signal value");
    }
    return std::string{text.data(), result.ptr};
}

void write_csv_string(std::string_view value) {
    std::cout.put('"');
    for (const char character : value) {
        if (character == '"') {
            std::cout.put('"');
        }
        std::cout.put(character);
    }
    std::cout.put('"');
}

void write_raw_value(const Signal& signal, std::uint64_t raw) {
    if (signal.codec.sign_type() == '-') {
        std::cout << std::bit_cast<std::int64_t>(raw);
    } else {
        std::cout << raw;
    }
}

void print_packet(const Database& db, const can::frame_packet& packet,
                  const IdentifierFilter& identifiers) {
    if (packet.empty()) {
        return;
    }
    SignalValues values;
    for (auto [stamp, frame] : packet) {
        if (!identifiers.empty() && !identifiers.contains(frame.can_id)) {
            continue;
        }
        const auto& msg = db.messages.at(frame.can_id);
        const bool extended = (frame.can_id & CAN_EFF_FLAG) != 0;
        std::ostringstream id;
        id << std::uppercase << std::hex << std::setfill('0') << std::setw(extended ? 8 : 3)
           << (frame.can_id & CAN_EFF_MASK);
        const auto id_text = id.str();
        const auto stamp_ms =
            std::chrono::duration_cast<std::chrono::milliseconds>(stamp.time_since_epoch()).count();
        const bool show_common = can::use_non_muxed(frame);
        msg.decode_signals(frame.data, values);
        for (std::size_t i = 0; i < msg.signals.size(); ++i) {
            const auto& signal = msg.signals[i];
            if (!values[i] ||
                (!show_common && !signal.is_selector() && signal.conditions.empty())) {
                continue;
            }
            std::cout << stamp_ms << ' ' << id_text << ' ' << msg.name << '.' << signal.name << '=';
            write_raw_value(signal, *values[i]);
            std::cout << '\n';
        }
    }
}

void replay(const Database& db, const char* path, bool physical, bool candump, bool csv,
            can::v2c_transcoder* transcoder, const IdentifierFilter& identifiers) {
    std::ifstream input{path};
    if (!input) {
        throw std::runtime_error(std::string{"cannot open frame log: "} + path);
    }
    if (csv) {
        std::cout << "timestamp,timestamp_unit,interface,can_id,message,signal,value,unit\n";
    }

    std::size_t line_number = 0;
    Timestamp last_stamp{};
    SignalValues values;
    auto fail = [&](const std::string& reason) {
        throw std::runtime_error(std::string{path} + ":" + std::to_string(line_number) + ": " + reason);
    };

    for (std::string line; std::getline(input, line);) {
        ++line_number;
        const auto start = line.find_first_not_of(" \t\r\n");
        if (start == std::string::npos || line[start] == '#') {
            continue;
        }
        std::istringstream row{line};
        std::string stamp_text, interface_name, id_text, bytes, extra;
        Timestamp stamp{};
        std::uint64_t milliseconds = 0;
        if (candump) {
            std::string frame, trailing;
            if (!(row >> stamp_text >> interface_name >> frame) ||
                ((row >> extra) && ((extra != "R" && extra != "T") || (row >> trailing))) ||
                !parse_candump_timestamp(stamp_text, stamp)) {
                fail("expected a candump timestamp, interface name, and data frame");
            }
            stamp_text = stamp_text.substr(1, stamp_text.size() - 2);
            const auto separator = frame.find('#');
            if (separator == std::string::npos || frame.find('#', separator + 1) != std::string::npos) {
                fail("expected a Classic CAN data frame in ID#PAYLOAD form");
            }
            id_text = frame.substr(0, separator);
            bytes = frame.substr(separator + 1);
        } else {
            if (!(row >> stamp_text >> id_text >> bytes) || (row >> extra) ||
                !parse_number(stamp_text, milliseconds, 10)) {
                fail("expected milliseconds, a hexadecimal CAN identifier, and a payload");
            }
            stamp = {milliseconds / 1000,
                     static_cast<std::uint32_t>((milliseconds % 1000) * 1000000)};
            stamp_text = std::to_string(milliseconds);
        }
        std::uint32_t id = 0;
        if (!parse_identifier(id_text, id)) {
            fail("expected a 3- or 8-digit hexadecimal CAN identifier");
        }
        if (stamp < last_stamp) {
            fail("timestamps must be nondecreasing");
        }
        last_stamp = stamp;
        if (!candump && bytes == "-") {
            bytes.clear();
        }
        if (bytes.size() % 2 || bytes.size() > 2 * CAN_MAX_DLEN) {
            fail("expected up to 8 hexadecimal payload bytes");
        }

        std::array<std::uint8_t, CAN_MAX_DLEN> data{};
        const auto count = bytes.size() / 2;
        for (std::size_t byte = 0; byte < count; ++byte) {
            if (!parse_number(std::string_view{bytes}.substr(byte * 2, 2), data[byte], 16)) {
                fail("invalid hexadecimal payload");
            }
        }

        const auto it = db.messages.find(id);
        if (it != db.messages.end() && count != it->second.size) {
            fail("expected " + std::to_string(it->second.size) + " payload bytes for " + it->second.name);
        }
        if (transcoder) {
            // The packet stores epoch seconds as an unsigned 32-bit field.
            if (milliseconds / 1000 > std::numeric_limits<std::uint32_t>::max()) {
                fail("timestamp is outside the V2C packet range");
            }
            can_frame frame{};
            frame.can_id = id;
            frame.len = static_cast<std::uint8_t>(count);
            std::copy_n(data.begin(), count, frame.data);
            const auto packet = transcoder->transcode(
                can::can_time{std::chrono::milliseconds{milliseconds}}, frame);
            print_packet(db, packet, identifiers);
            continue;
        }
        if (it == db.messages.end() || (!identifiers.empty() && !identifiers.contains(id))) {
            continue;
        }
        const auto& msg = it->second;
        msg.decode_signals(data.data(), values);
        for (std::size_t i = 0; i < msg.signals.size(); ++i) {
            if (!values[i]) {
                continue;
            }
            const auto& signal = msg.signals[i];
            const auto raw = *values[i];
            const auto signed_value = std::bit_cast<std::int64_t>(raw);
            const bool is_signed = signal.codec.sign_type() == '-';
            std::string value;
            std::string_view unit;
            if (physical && signal.scaling) {
                value = physical_value(
                    is_signed ? static_cast<double>(signed_value) : static_cast<double>(raw),
                    *signal.scaling);
                unit = signal.scaling->unit;
            }
            if (csv) {
                const std::array<std::string_view, 6> fields{
                    stamp_text, candump ? "s" : "ms", interface_name, id_text, msg.name,
                    signal.name};
                for (const auto field : fields) {
                    write_csv_string(field);
                    std::cout.put(',');
                }
            } else {
                std::cout << stamp_text;
                if (candump) {
                    std::cout << ' ' << interface_name;
                }
                std::cout << ' ' << id_text << ' ' << msg.name << '.' << signal.name << '=';
            }
            if (!value.empty()) {
                std::cout << value;
            } else {
                write_raw_value(signal, raw);
            }
            if (csv) {
                std::cout.put(',');
                write_csv_string(unit);
            } else if (!unit.empty()) {
                std::cout << ' ' << unit;
            }
            std::cout << '\n';
        }
    }
    if (!input.eof()) {
        fail("could not read frame log");
    }
    if (transcoder) {
        print_packet(db, transcoder->drain(), identifiers);
    }
}

}  // namespace

int main(int argc, char* argv[]) {
    constexpr auto usage =
        "Usage: can-telemetry-replay [--physical] [--candump] [--csv] [--id HEX]... "
        "<database.dbc> <frames.log>\n"
        "       can-telemetry-replay --aggregate [--id HEX]... <database.dbc> <frames.log>\n"
        "Use three hex digits for a standard ID and eight for an extended ID.\n";
    if (argc == 2 && std::string_view{argv[1]} == "--help") {
        std::cout << usage;
        return 0;
    }
    bool physical = false;
    bool candump = false;
    bool csv = false;
    bool aggregate = false;
    IdentifierFilter identifiers;
    int input = 1;
    for (; input < argc; ++input) {
        const std::string_view option{argv[input]};
        if (option == "--physical" && !physical) {
            physical = true;
        } else if (option == "--candump" && !candump) {
            candump = true;
        } else if (option == "--csv" && !csv) {
            csv = true;
        } else if (option == "--aggregate" && !aggregate) {
            aggregate = true;
        } else if (option == "--id") {
            if (++input == argc) {
                std::cerr << usage;
                return 2;
            }
            std::uint32_t id = 0;
            if (!parse_identifier(argv[input], id)) {
                std::cerr << "invalid CAN identifier for --id: " << argv[input] << '\n';
                return 2;
            }
            identifiers.insert(id);
        } else {
            break;
        }
    }
    if (argc != input + 2 || (aggregate && (physical || candump || csv))) {
        std::cerr << usage;
        return 2;
    }
    try {
        can::v2c_transcoder transcoder{};
        auto* aggregator = aggregate ? &transcoder : nullptr;
        replay(load_database(argv[input], aggregator), argv[input + 1], physical, candump, csv,
               aggregator, identifiers);
        if (!std::cout.flush()) {
            throw std::runtime_error("could not write replay output");
        }
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
