#include "can/can_codec.h"
#include "dbc/dbc_parser.h"

#include <algorithm>
#include <array>
#include <bit>
#include <charconv>
#include <fstream>
#include <functional>
#include <iostream>
#include <iterator>
#include <sstream>
#include <stdexcept>
#include <unordered_map>

namespace {

struct Signal {
    std::string name;
    can::sig_codec codec;
    std::optional<unsigned> multiplex_value;
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
                    char order, char sign, std::optional<unsigned> value) {
        auto& msg = messages.at(id);
        const unsigned first_byte = start / 8;
        const unsigned offset = order == '1' ? start % 8 : 7 - start % 8;
        if (first_byte >= msg.size || bits == 0 || bits > 64 ||
            (bits + offset + 7) / 8 > msg.size - first_byte) {
            throw std::runtime_error("signal lies outside the payload of " + msg.name + "." + name);
        }
        msg.signals.push_back({std::move(name), can::sig_codec{start, bits, order, sign}, value});
    }

    friend void tag_invoke(can::def_sg_cpo, Database& db, std::uint32_t id,
                           std::optional<unsigned> value, std::string name, unsigned start,
                           unsigned bits, char order, char sign, double, double, double,
                           double, std::string, std::vector<std::size_t>) {
        db.add_signal(id, std::move(name), start, bits, order, sign, value);
    }

    friend void tag_invoke(can::def_sg_mux_cpo, Database& db, std::uint32_t id,
                           std::string name, unsigned start, unsigned bits, char order,
                           char sign, std::string, std::vector<std::size_t>) {
        auto& msg = db.messages.at(id);
        if (msg.multiplexor || sign != '+') {
            throw std::runtime_error("expected one unsigned multiplexor for " + msg.name);
        }
        msg.multiplexor = msg.signals.size();
        db.add_signal(id, std::move(name), start, bits, order, sign, {});
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

void replay(const Database& db, const char* path) {
    std::ifstream input{path};
    if (!input) {
        throw std::runtime_error(std::string{"cannot open frame log: "} + path);
    }

    std::size_t line_number = 0;
    std::uint64_t last_stamp = 0;
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
        std::string stamp_text, id_text, bytes, extra;
        std::uint64_t stamp = 0;
        std::uint32_t id = 0;
        if (!(row >> stamp_text >> id_text >> bytes) || row >> extra ||
            !parse_number(stamp_text, stamp, 10) ||
            (id_text.size() != 3 && id_text.size() != 8) ||
            !parse_number(id_text, id, 16) ||
            id > (id_text.size() == 3 ? CAN_SFF_MASK : CAN_EFF_MASK)) {
            fail("expected milliseconds, a 3- or 8-digit hexadecimal CAN identifier, and a payload");
        }
        if (stamp < last_stamp) {
            fail("timestamps must be nondecreasing");
        }
        last_stamp = stamp;
        if (id_text.size() == 8) {
            id |= CAN_EFF_FLAG;
        }
        if (bytes == "-") {
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
            std::cout << stamp << ' ' << id_text << ' ' << msg.name << '.' << signal.name << '=';
            if (signal.codec.sign_type() == '-') {
                std::cout << std::bit_cast<std::int64_t>(raw);
            } else {
                std::cout << raw;
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
    constexpr auto usage = "Usage: can-telemetry-replay <database.dbc> <frames.log>\n";
    if (argc == 2 && std::string_view{argv[1]} == "--help") {
        std::cout << usage;
        return 0;
    }
    if (argc != 3) {
        std::cerr << usage;
        return 2;
    }
    try {
        replay(load_database(argv[1]), argv[2]);
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
