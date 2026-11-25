#include <cstring>
#include <iostream>
#include <string_view>

#include "can/frame_packet.h"

namespace {

bool expect_empty(const can::frame_packet& packet, std::string_view label) {
    std::cout << "Iterating " << label << std::endl;
    for (const auto& sample : packet) {
        std::cerr << label << " yielded unexpected frame " << sample.second.can_id << '\n';
        return false;
    }
    return true;
}

}  // namespace

int main() {
    const can::frame_packet empty;
    if (!expect_empty(empty, "a default packet")) {
        return 1;
    }

    can::frame_packet packet;
    constexpr std::uint32_t utc = 1234;
    packet.prepare(utc);
    if (!expect_empty(packet, "a header-only packet")) {
        return 1;
    }

    can_frame frame{};
    frame.can_id = 7;
    frame.len = 1;
    frame.data[0] = 42;
    packet.append(std::int32_t{25});
    packet.append(frame);

    const auto expected_time = can::can_time{std::chrono::seconds{utc} +
                                             std::chrono::milliseconds{25}};
    std::size_t count = 0;
    for (const auto& [time, actual] : packet) {
        if (time != expected_time || actual.can_id != 7 ||
            actual.len != 1 || actual.data[0] != 42) {
            std::cerr << "Wrong timestamp or frame data in populated packet\n";
            return 1;
        }
        ++count;
    }
    if (count != 1) {
        std::cerr << "Expected one frame, found " << count << '\n';
        return 1;
    }
}
