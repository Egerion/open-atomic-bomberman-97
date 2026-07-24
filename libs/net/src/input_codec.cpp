#include "bomber/net/input_codec.hpp"

#include <bit>

namespace bomber::net {

using sim::kMaxPlayers;
using sim::PlayerInput;
using sim::TickInputs;

std::uint8_t pack_input(const PlayerInput& in) {
    unsigned bits = 0;
    if (in.up) bits |= 1U << 0;
    if (in.down) bits |= 1U << 1;
    if (in.left) bits |= 1U << 2;
    if (in.right) bits |= 1U << 3;
    if (in.action1) bits |= 1U << 4;
    if (in.action2) bits |= 1U << 5;
    return static_cast<std::uint8_t>(bits);
}

PlayerInput unpack_input(std::uint8_t bits) {
    PlayerInput in;
    in.up = (bits & (1U << 0)) != 0;
    in.down = (bits & (1U << 1)) != 0;
    in.left = (bits & (1U << 2)) != 0;
    in.right = (bits & (1U << 3)) != 0;
    in.action1 = (bits & (1U << 4)) != 0;
    in.action2 = (bits & (1U << 5)) != 0;
    return in;
}

namespace {

void put_u16_le(std::vector<std::uint8_t>& b, std::uint16_t v) {
    b.push_back(static_cast<std::uint8_t>(v & 0xFFU));
    b.push_back(static_cast<std::uint8_t>((v >> 8) & 0xFFU));
}

void put_u32_le(std::vector<std::uint8_t>& b, std::uint32_t v) {
    b.push_back(static_cast<std::uint8_t>(v & 0xFFU));
    b.push_back(static_cast<std::uint8_t>((v >> 8) & 0xFFU));
    b.push_back(static_cast<std::uint8_t>((v >> 16) & 0xFFU));
    b.push_back(static_cast<std::uint8_t>((v >> 24) & 0xFFU));
}

std::uint16_t get_u16_le(const std::uint8_t* d) {
    return static_cast<std::uint16_t>(static_cast<unsigned>(d[0]) | (static_cast<unsigned>(d[1]) << 8));
}

std::uint32_t get_u32_le(const std::uint8_t* d) {
    return static_cast<std::uint32_t>(d[0]) | (static_cast<std::uint32_t>(d[1]) << 8) |
           (static_cast<std::uint32_t>(d[2]) << 16) | (static_cast<std::uint32_t>(d[3]) << 24);
}

constexpr std::size_t kHeaderBytes = 6;  // tick_index u32 + seat_mask u16

}  // namespace

std::vector<std::uint8_t> serialize(std::uint32_t tick_index, std::uint16_t seat_mask,
                                    const TickInputs& inputs) {
    std::vector<std::uint8_t> b;
    put_u32_le(b, tick_index);
    put_u16_le(b, seat_mask);
    for (int s = 0; s < kMaxPlayers; ++s)
        if ((seat_mask & (1U << s)) != 0)
            b.push_back(pack_input(inputs.players[static_cast<std::size_t>(s)]));
    return b;
}

bool deserialize(const std::uint8_t* data, std::size_t size, InputFrame* out) {
    if (size < kHeaderBytes) return false;
    const std::uint16_t mask = get_u16_le(data + 4);
    // Reject any seat bit at or beyond kMaxPlayers: a peer must never name a
    // seat this build has no slot for.
    if ((mask >> kMaxPlayers) != 0) return false;
    const std::size_t seats = static_cast<std::size_t>(std::popcount(mask));
    // Exact framing: the payload is one byte per named seat, no more, no less.
    if (size != kHeaderBytes + seats) return false;

    InputFrame f;
    f.tick_index = get_u32_le(data);
    f.seat_mask = mask;
    std::size_t off = kHeaderBytes;
    for (int s = 0; s < kMaxPlayers; ++s)
        if ((mask & (1U << s)) != 0)
            f.inputs.players[static_cast<std::size_t>(s)] = unpack_input(data[off++]);
    *out = f;
    return true;
}

void merge_seats(const InputFrame& frame, TickInputs* dst) {
    for (int s = 0; s < kMaxPlayers; ++s)
        if ((frame.seat_mask & (1U << s)) != 0)
            dst->players[static_cast<std::size_t>(s)] = frame.inputs.players[static_cast<std::size_t>(s)];
}

}  // namespace bomber::net
