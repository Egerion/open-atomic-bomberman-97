#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "bomber/net/input_codec.hpp"

// The message layer over the raw input codec (input_codec.hpp): a lockstep peer
// exchanges exactly two kinds of datagram — per-tick INPUT and per-tick HASH —
// so every packet carries a one-byte MsgType tag ahead of its payload. INPUT
// reuses input_codec's frame verbatim; HASH is a tick + the state_hash digest
// the desync detector compares (ADR-0010: state_hash IS the on-wire integrity
// check). Everything stays untrusted-input safe: decode() bounds-checks the tag
// and the payload and rejects anything malformed.

namespace bomber::net {

enum class MsgType : std::uint8_t { Input = 0, Hash = 1 };

// A peer's claimed Simulation::hash() at the end of tick `tick_index`. The
// receiver compares it against its OWN hash for that tick; a mismatch is an
// immediate, loud desync (there is no silent correction — unlike the 1997
// host-authoritative model, docs/re/multiplayer.md §1.5).
struct HashFrame {
    std::uint32_t tick_index = 0;
    std::uint64_t hash = 0;
};

// One decoded datagram: exactly one of `input` / `hash` is meaningful per `type`.
struct Message {
    MsgType type = MsgType::Input;
    InputFrame input;
    HashFrame hash;
};

// [MsgType::Input][input_codec frame] — the seats in `seat_mask`, stamped `tick`.
std::vector<std::uint8_t> encode_input(std::uint32_t tick_index, std::uint16_t seat_mask,
                                       const sim::TickInputs& inputs);

// [MsgType::Hash][tick u32-LE][hash u64-LE] — 13 bytes.
std::vector<std::uint8_t> encode_hash(std::uint32_t tick_index, std::uint64_t hash);

// Decode a datagram produced by encode_input/encode_hash. Returns false (leaving
// *out untouched) on an unknown tag, a short buffer, or a malformed payload.
bool decode(const std::uint8_t* data, std::size_t size, Message* out);

}  // namespace bomber::net
