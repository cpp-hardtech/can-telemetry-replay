#include "can/can_codec.h"
#include "dbc/dbc_parser.h"

#include <algorithm>
#include <array>
#include <bit>
#include <charconv>
#include <cmath>
#include <fstream>
#include <functional>
#include <iostream>
#include <iterator>
#include <sstream>
#include <stdexcept>
#include <unordered_map>
#include <utility>

namespace {

struct EngineeringScale {
    double factor;
    double offset;
    std::string unit;
};

struct Signal {
    std::string name;
    can::sig_codec codec;
    std::optional<unsigned> multiplex_value;
    std::optional<EngineeringScale> scaling;
};

struct Message {
    std::string name;
    std::size_t size;
    std::vector<Signal> signals;
    std::optional<std::size_t> multiplexor;
};

bool valid_identifier(std::uint32_t id) {
    return id <= CAN_SFF_MASK || (id & ~CAN_EFF_MASK) == CAN_EFF_FLAG;
}

struct Database {
    std::unordered_map<std::uint32_t, Message> messages;

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
                               value, std::move(scaling)});
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
        auto& msg = db.messages.at(id);
        if (msg.multiplexor || sign != '+') {
            throw std::runtime_error("expected one unsigned multiplexor for " + msg.name);
        }
        msg.multiplexor = msg.signals.size();
        db.add_signal(id, std::move(name), start, bits, order, sign, {}, {});
    }

    friend void tag_invoke(can::def_sig_valtype_cpo, Database&, unsigned, std::string name,
                           unsigned type) {
        if (type != 0) {
            throw std::runtime_error("floating-point DBC signal is unsupported: " + name);
        }
    }

    friend void tag_invoke(can::def_sg_mul_val_cpo, Database&, unsigned, std::string name,
                           std::string, std::vector<std::pair<unsigned, unsigned>>) {
        throw std::runtime_error("extended DBC multiplexing is unsupported: " + name);
    }
};

Database load_database(const char* path) {
    std::ifstream input{path};
    if (!input) {
        throw std::runtime_error(std::string{"cannot open DBC file: "} + path);
    }
    std::string contents{std::istreambuf_iterator<char>{input}, {}};
    Database db;
    if (!can::parse_dbc(contents, std::ref(db)) || db.messages.empty()) {
        throw std::runtime_error(std::string{"expected DBC message definitions in "} + path);
    }
    for (const auto& [id, msg] : db.messages) {
        if (!msg.multiplexor && std::ranges::any_of(msg.signals, [](const Signal& signal) {
                return signal.multiplex_value.has_value();
            })) {
            throw std::runtime_error("missing DBC multiplexor for " + msg.name);
        }
    }
    return db;
}

template <typename T>
bool parse_number(std::string_view value, T& number, int base) {
    const auto* end = value.data() + value.size();
    const auto result = std::from_chars(value.data(), end, number, base);
    return result.ec == std::errc{} && result.ptr == end;
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

void replay(const Database& db, const char* path, bool physical, bool candump) {
    std::ifstream input{path};
    if (!input) {
        throw std::runtime_error(std::string{"cannot open frame log: "} + path);
    }

    std::size_t line_number = 0;
    Timestamp last_stamp{};
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
            std::uint64_t milliseconds = 0;
            if (!(row >> stamp_text >> id_text >> bytes) || (row >> extra) ||
                !parse_number(stamp_text, milliseconds, 10)) {
                fail("expected milliseconds, a hexadecimal CAN identifier, and a payload");
            }
            stamp = {milliseconds / 1000,
                     static_cast<std::uint32_t>((milliseconds % 1000) * 1000000)};
            stamp_text = std::to_string(milliseconds);
        }
        std::uint32_t id = 0;
        if ((id_text.size() != 3 && id_text.size() != 8) ||
            !parse_number(id_text, id, 16) ||
            id > (id_text.size() == 3 ? CAN_SFF_MASK : CAN_EFF_MASK)) {
            fail("expected a 3- or 8-digit hexadecimal CAN identifier");
        }
        if (stamp < last_stamp) {
            fail("timestamps must be nondecreasing");
        }
        last_stamp = stamp;
        if (id_text.size() == 8) {
            id |= CAN_EFF_FLAG;
        }
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
        if (it == db.messages.end()) {
            continue;
        }
        const auto& msg = it->second;
        if (count != msg.size) {
            fail("expected " + std::to_string(msg.size) + " payload bytes for " + msg.name);
        }
        const auto multiplex_value = msg.multiplexor ? msg.signals[*msg.multiplexor].codec(data.data()) : 0;
        for (const auto& signal : msg.signals) {
            if (signal.multiplex_value && *signal.multiplex_value != multiplex_value) {
                continue;
            }
            const auto raw = signal.codec(data.data());
            const auto signed_value = std::bit_cast<std::int64_t>(raw);
            const bool is_signed = signal.codec.sign_type() == '-';
            const auto prefix = [&] {
                std::cout << stamp_text;
                if (candump) {
                    std::cout << ' ' << interface_name;
                }
                std::cout << ' ' << id_text << ' ' << msg.name << '.' << signal.name << '=';
            };
            if (physical && signal.scaling) {
                const auto value = physical_value(
                    is_signed ? static_cast<double>(signed_value) : static_cast<double>(raw),
                    *signal.scaling);
                prefix();
                std::cout << value;
                if (!signal.scaling->unit.empty()) {
                    std::cout << ' ' << signal.scaling->unit;
                }
            } else {
                prefix();
                if (is_signed) {
                    std::cout << signed_value;
                } else {
                    std::cout << raw;
                }
            }
            std::cout << '\n';
        }
    }
    if (!input.eof()) {
        fail("could not read frame log");
    }
}

}  // namespace

int main(int argc, char* argv[]) {
    constexpr auto usage =
        "Usage: can-telemetry-replay [--physical] [--candump] <database.dbc> <frames.log>\n";
    if (argc == 2 && std::string_view{argv[1]} == "--help") {
        std::cout << usage;
        return 0;
    }
    bool physical = false;
    bool candump = false;
    int input = 1;
    for (; input < argc; ++input) {
        const std::string_view option{argv[input]};
        if (option == "--physical" && !physical) {
            physical = true;
        } else if (option == "--candump" && !candump) {
            candump = true;
        } else {
            break;
        }
    }
    if (argc != input + 2) {
        std::cerr << usage;
        return 2;
    }
    try {
        replay(load_database(argv[input]), argv[input + 1], physical, candump);
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
