#include "bomber/net/protocol.hpp"

namespace bomber::net {

namespace {

void put_u32_le(std::vector<std::uint8_t>& b, std::uint32_t v) {
    for (int i = 0; i < 4; ++i) b.push_back(static_cast<std::uint8_t>((v >> (8 * i)) & 0xFFU));
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

constexpr std::size_t kHashFrameBytes = 1 + 4 + 8;  // tag + tick u32 + hash u64

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
    return false;  // unknown tag
}

}  // namespace bomber::net
