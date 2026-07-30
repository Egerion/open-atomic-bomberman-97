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
constexpr std::size_t kPunchFrameBytes = 1 + 4 + 1;  // tag + nonce u32 + is_pong u8
constexpr std::size_t kProbeFrameBytes = 1 + 4 + 1;  // tag + nonce u32 + seen_peer u8
constexpr std::size_t kDropFrameBytes = 1 + 1 + 4;   // tag + seat u8 + at_tick u32
constexpr std::size_t kMatchCtlFrameBytes = 1 + 1 + 4;  // tag + kind u8 + at_tick u32
constexpr std::size_t kHostLostFrameBytes = 1 + 1 + 4;  // tag + seat u8 + at_tick u32
constexpr std::size_t kAckFrameBytes = 1 + 4 + 4 + 1;  // tag + revision + checksum + seat u8
// tag + revision u32 + level_index u8 + rounds u8 + name_len u8
constexpr std::size_t kPreviewHeaderBytes = 1 + 4 + 1 + 1 + 1;
constexpr std::size_t kPreviewRosterBytes = 2 * static_cast<std::size_t>(sim::kMaxPlayers);

// The number of chunks a blob of `total_len` bytes MUST be cut into. Both the
// encoder and the decoder derive it, so a sender cannot claim a different
// split than the one its total_len implies.
constexpr std::size_t chunks_for(std::size_t total_len) {
    return (total_len + kSetupChunkPayloadBytes - 1) / kSetupChunkPayloadBytes;
}

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

std::vector<std::uint8_t> encode_punch(std::uint32_t nonce, bool is_pong) {
    std::vector<std::uint8_t> b;
    b.push_back(static_cast<std::uint8_t>(MsgType::Punch));
    put_u32_le(b, nonce);
    b.push_back(is_pong ? 1U : 0U);
    return b;
}

std::vector<std::uint8_t> encode_probe(std::uint32_t nonce, bool seen_peer) {
    std::vector<std::uint8_t> b;
    b.push_back(static_cast<std::uint8_t>(MsgType::Probe));
    put_u32_le(b, nonce);
    b.push_back(seen_peer ? 1U : 0U);
    return b;
}

std::vector<std::uint8_t> encode_drop(std::uint8_t seat, std::uint32_t at_tick) {
    std::vector<std::uint8_t> b;
    b.push_back(static_cast<std::uint8_t>(MsgType::Drop));
    b.push_back(seat);
    put_u32_le(b, at_tick);
    return b;
}

std::vector<std::uint8_t> encode_match_ctl(MatchCtlKind kind, std::uint32_t at_tick) {
    std::vector<std::uint8_t> b;
    b.push_back(static_cast<std::uint8_t>(MsgType::MatchCtl));
    b.push_back(static_cast<std::uint8_t>(kind));
    put_u32_le(b, at_tick);
    return b;
}

std::vector<std::uint8_t> encode_host_lost(std::uint8_t seat, std::uint32_t at_tick) {
    std::vector<std::uint8_t> b;
    b.push_back(static_cast<std::uint8_t>(MsgType::HostLost));
    b.push_back(seat);
    put_u32_le(b, at_tick);
    return b;
}

std::vector<std::uint8_t> encode_setup_preview(const SetupPreviewFrame& preview) {
    std::vector<std::uint8_t> b;
    b.push_back(static_cast<std::uint8_t>(MsgType::SetupPreview));
    put_u32_le(b, preview.revision);
    b.push_back(preview.level_index);
    b.push_back(preview.rounds);
    // Caller-supplied names longer than the cap are TRUNCATED here (the host
    // owns this string, so it is not untrusted); decode still rejects an
    // over-long one, which can only come from a peer that disagrees with us.
    const std::size_t name_len = preview.level_name.size() < kSetupLevelNameMax
                                     ? preview.level_name.size()
                                     : kSetupLevelNameMax;
    b.push_back(static_cast<std::uint8_t>(name_len));
    for (std::size_t i = 0; i < name_len; ++i)
        b.push_back(static_cast<std::uint8_t>(preview.level_name[i]));
    for (const SetupSlotKind k : preview.slots) b.push_back(static_cast<std::uint8_t>(k));
    for (const std::uint8_t t : preview.team) b.push_back(t);
    return b;
}

std::vector<std::uint8_t> encode_setup_chunk(const SetupChunkFrame& chunk) {
    std::vector<std::uint8_t> b;
    b.push_back(static_cast<std::uint8_t>(MsgType::SetupChunk));
    put_u32_le(b, chunk.revision);
    put_u32_le(b, chunk.total_len);
    put_u32_le(b, chunk.checksum);
    b.push_back(chunk.chunk_count);
    b.push_back(chunk.chunk_index);
    b.insert(b.end(), chunk.payload.begin(), chunk.payload.end());
    return b;
}

std::vector<std::uint8_t> encode_setup_ack(std::uint32_t revision, std::uint32_t checksum,
                                           std::uint8_t seat) {
    std::vector<std::uint8_t> b;
    b.push_back(static_cast<std::uint8_t>(MsgType::SetupAck));
    put_u32_le(b, revision);
    put_u32_le(b, checksum);
    b.push_back(seat);
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
    if (tag == MsgType::Punch) {
        if (size != kPunchFrameBytes) return false;
        out->type = MsgType::Punch;
        out->punch.nonce = get_u32_le(data + 1);
        out->punch.is_pong = data[1 + 4] != 0;
        return true;
    }
    if (tag == MsgType::Probe) {
        if (size != kProbeFrameBytes) return false;
        out->type = MsgType::Probe;
        out->probe.nonce = get_u32_le(data + 1);
        out->probe.seen_peer = data[1 + 4] != 0;
        return true;
    }
    if (tag == MsgType::Drop) {
        if (size != kDropFrameBytes) return false;
        if (data[1] >= sim::kMaxPlayers) return false;  // untrusted: seat must index a real slot
        out->type = MsgType::Drop;
        out->drop.seat = data[1];
        out->drop.at_tick = get_u32_le(data + 2);
        return true;
    }
    if (tag == MsgType::MatchCtl) {
        if (size != kMatchCtlFrameBytes) return false;
        if (data[1] > static_cast<std::uint8_t>(MatchCtlKind::Rematch)) return false;
        out->type = MsgType::MatchCtl;
        out->match_ctl.kind = static_cast<MatchCtlKind>(data[1]);
        out->match_ctl.at_tick = get_u32_le(data + 2);
        return true;
    }
    if (tag == MsgType::HostLost) {
        if (size != kHostLostFrameBytes) return false;
        if (data[1] >= sim::kMaxPlayers) return false;  // untrusted: seat must index a real slot
        out->type = MsgType::HostLost;
        out->host_lost.seat = data[1];
        out->host_lost.at_tick = get_u32_le(data + 2);
        return true;
    }
    if (tag == MsgType::SetupAck) {
        if (size != kAckFrameBytes) return false;
        if (data[9] >= sim::kMaxPlayers) return false;  // untrusted: seat indexes a per-seat mask
        out->type = MsgType::SetupAck;
        out->setup_ack.revision = get_u32_le(data + 1);
        out->setup_ack.checksum = get_u32_le(data + 1 + 4);
        out->setup_ack.seat = data[9];
        return true;
    }
    if (tag == MsgType::SetupPreview) {
        if (size < kPreviewHeaderBytes) return false;
        const std::size_t name_len = data[7];
        if (name_len > kSetupLevelNameMax) return false;
        if (size != kPreviewHeaderBytes + name_len + kPreviewRosterBytes) return false;

        SetupPreviewFrame pf;
        pf.revision = get_u32_le(data + 1);
        pf.level_index = data[5];
        pf.rounds = data[6];
        pf.level_name.reserve(name_len);
        for (std::size_t i = 0; i < name_len; ++i) {
            const std::uint8_t c = data[kPreviewHeaderBytes + i];
            // Printable ASCII only: the GUI draws this string, and a remote peer
            // has no business smuggling NULs or control codes into it.
            if (c < 0x20U || c > 0x7EU) return false;
            pf.level_name.push_back(static_cast<char>(c));
        }
        std::size_t off = kPreviewHeaderBytes + name_len;
        for (std::size_t s = 0; s < static_cast<std::size_t>(sim::kMaxPlayers); ++s) {
            const std::uint8_t k = data[off++];
            if (k > static_cast<std::uint8_t>(SetupSlotKind::Remote)) return false;
            pf.slots[s] = static_cast<SetupSlotKind>(k);
        }
        for (std::size_t s = 0; s < static_cast<std::size_t>(sim::kMaxPlayers); ++s)
            pf.team[s] = data[off++];
        out->type = MsgType::SetupPreview;
        out->setup_preview = std::move(pf);
        return true;
    }
    if (tag == MsgType::SetupChunk) {
        if (size < kSetupChunkHeaderBytes) return false;
        const std::uint32_t revision = get_u32_le(data + 1);
        const std::uint32_t total_len = get_u32_le(data + 5);
        const std::uint32_t checksum = get_u32_le(data + 9);
        const std::size_t count = data[13];
        const std::size_t index = data[14];
        if (total_len == 0 || total_len > kMaxMatchConfigBytes) return false;
        if (count != chunks_for(total_len) || index >= count) return false;
        const std::size_t begin = index * kSetupChunkPayloadBytes;
        const std::size_t remaining = static_cast<std::size_t>(total_len) - begin;
        const std::size_t payload_len =
            remaining < kSetupChunkPayloadBytes ? remaining : kSetupChunkPayloadBytes;
        if (size != kSetupChunkHeaderBytes + payload_len) return false;

        SetupChunkFrame cf;
        cf.revision = revision;
        cf.total_len = total_len;
        cf.checksum = checksum;
        cf.chunk_count = static_cast<std::uint8_t>(count);
        cf.chunk_index = static_cast<std::uint8_t>(index);
        cf.payload.assign(data + kSetupChunkHeaderBytes,
                          data + kSetupChunkHeaderBytes + payload_len);
        out->type = MsgType::SetupChunk;
        out->setup_chunk = std::move(cf);
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
