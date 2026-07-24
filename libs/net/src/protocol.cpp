#include "bomber/net/protocol.hpp"

#include <bit>
#include <utility>

namespace bomber::net {

namespace {

void put_u16_le(std::vector<std::uint8_t>& b, std::uint16_t v) {
    b.push_back(static_cast<std::uint8_t>(v & 0xFFU));
    b.push_back(static_cast<std::uint8_t>((v >> 8) & 0xFFU));
}

void put_u32_le(std::vector<std::uint8_t>& b, std::uint32_t v) {
    for (int i = 0; i < 4; ++i) b.push_back(static_cast<std::uint8_t>((v >> (8 * i)) & 0xFFU));
}

std::uint16_t get_u16_le(const std::uint8_t* d) {
    return static_cast<std::uint16_t>(static_cast<unsigned>(d[0]) | (static_cast<unsigned>(d[1]) << 8));
}

void put_u64_le(std::vector<std::uint8_t>& b, std::uint64_t v) {
    for (int i = 0; i < 8; ++i) b.push_back(static_cast<std::uint8_t>((v >> (8 * i)) & 0xFFU));
}

std::uint32_t get_u32_le(const std::uint8_t* d) {
    std::uint32_t v = 0;
    for (int i = 0; i < 4; ++i) v |= static_cast<std::uint32_t>(d[i]) << (8 * i);
    return v;
}

std::uint64_t get_u64_le(const std::uint8_t* d) {
    std::uint64_t v = 0;
    for (int i = 0; i < 8; ++i) v |= static_cast<std::uint64_t>(d[i]) << (8 * i);
    return v;
}

constexpr std::size_t kHashFrameBytes = 1 + 4 + 8;   // tag + tick u32 + hash u64
constexpr std::size_t kRangeHeaderBytes = 1 + 4 + 1 + 2;  // tag + first_tick u32 + count u8 + mask u16
constexpr std::size_t kHelloFrameBytes = 1 + 4 + 1;  // tag + seed u32 + is_ack u8

}  // namespace

std::vector<std::uint8_t> encode_input(std::uint32_t tick_index, std::uint16_t seat_mask,
                                       const sim::TickInputs& inputs) {
    std::vector<std::uint8_t> b;
    b.push_back(static_cast<std::uint8_t>(MsgType::Input));
    const std::vector<std::uint8_t> payload = serialize(tick_index, seat_mask, inputs);
    b.insert(b.end(), payload.begin(), payload.end());
    return b;
}

std::vector<std::uint8_t> encode_hash(std::uint32_t tick_index, std::uint64_t hash) {
    std::vector<std::uint8_t> b;
    b.push_back(static_cast<std::uint8_t>(MsgType::Hash));
    put_u32_le(b, tick_index);
    put_u64_le(b, hash);
    return b;
}

std::vector<std::uint8_t> encode_hello(std::uint32_t seed, bool is_ack) {
    std::vector<std::uint8_t> b;
    b.push_back(static_cast<std::uint8_t>(MsgType::Hello));
    put_u32_le(b, seed);
    b.push_back(is_ack ? 1U : 0U);
    return b;
}

std::vector<std::uint8_t> encode_input_range(std::uint32_t first_tick, std::uint16_t seat_mask,
                                             const std::vector<sim::TickInputs>& per_tick) {
    std::vector<std::uint8_t> b;
    b.push_back(static_cast<std::uint8_t>(MsgType::InputRange));
    put_u32_le(b, first_tick);
    b.push_back(static_cast<std::uint8_t>(per_tick.size() & 0xFFU));  // count (caller keeps <= 255)
    put_u16_le(b, seat_mask);
    for (const sim::TickInputs& ti : per_tick)
        for (int s = 0; s < sim::kMaxPlayers; ++s)
            if ((seat_mask & (1U << s)) != 0)
                b.push_back(pack_input(ti.players[static_cast<std::size_t>(s)]));
    return b;
}

bool decode(const std::uint8_t* data, std::size_t size, Message* out) {
    if (size < 1) return false;
    const auto tag = static_cast<MsgType>(data[0]);
    if (tag == MsgType::Input) {
        InputFrame f;
        if (!deserialize(data + 1, size - 1, &f)) return false;
        out->type = MsgType::Input;
        out->input = f;
        return true;
    }
    if (tag == MsgType::Hash) {
        if (size != kHashFrameBytes) return false;
        out->type = MsgType::Hash;
        out->hash.tick_index = get_u32_le(data + 1);
        out->hash.hash = get_u64_le(data + 1 + 4);
        return true;
    }
    if (tag == MsgType::Hello) {
        if (size != kHelloFrameBytes) return false;
        out->type = MsgType::Hello;
        out->hello.seed = get_u32_le(data + 1);
        out->hello.is_ack = data[1 + 4] != 0;
        return true;
    }
    if (tag == MsgType::InputRange) {
        if (size < kRangeHeaderBytes) return false;
        const std::uint32_t first = get_u32_le(data + 1);
        const std::size_t count = data[5];
        const std::uint16_t mask = get_u16_le(data + 6);
        if (count == 0 || (mask >> sim::kMaxPlayers) != 0) return false;
        const std::size_t seats = static_cast<std::size_t>(std::popcount(mask));
        if (seats == 0 || size != kRangeHeaderBytes + count * seats) return false;

        InputRangeFrame rf;
        rf.first_tick = first;
        rf.seat_mask = mask;
        rf.per_tick.resize(count);
        std::size_t off = kRangeHeaderBytes;
        for (std::size_t i = 0; i < count; ++i)
            for (int s = 0; s < sim::kMaxPlayers; ++s)
                if ((mask & (1U << s)) != 0)
                    rf.per_tick[i].players[static_cast<std::size_t>(s)] = unpack_input(data[off++]);
        out->type = MsgType::InputRange;
        out->range = std::move(rf);
        return true;
    }
    return false;  // unknown tag
}

}  // namespace bomber::net
