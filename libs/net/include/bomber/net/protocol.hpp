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

enum class MsgType : std::uint8_t {
    Input = 0,
    Hash = 1,
    InputRange = 2,
    Hello = 3,
    Punch = 4,
    Drop = 5
};

// A peer's claimed Simulation::hash() at the end of tick `tick_index`. The
// receiver compares it against its OWN hash for that tick; a mismatch is an
// immediate, loud desync (there is no silent correction — unlike the 1997
// host-authoritative model, docs/re/multiplayer.md §1.5).
struct HashFrame {
    std::uint32_t tick_index = 0;
    std::uint64_t hash = 0;
};

// The pre-match seed handshake (handshake.hpp): the ONE datagram exchanged
// before any INPUT/HASH flows, so both peers agree on the shared match seed
// without a --seed on each command line. The HOST announces its seed
// (is_ack=false); the GUEST replies with an ACK (is_ack=true, seed unused) once
// it has adopted that seed. Both re-send every pump so a dropped packet
// self-heals (SeedHandshake).
struct HelloFrame {
    std::uint32_t seed = 0;
    bool is_ack = false;
};

// One NAT hole-punch probe (ADR-0011 §3, docs/online-multiplayer-design.md §3):
// the Rendezvous sends a PING (is_pong=false) with a fresh `nonce` to each of the
// peer's candidate addresses; a peer that receives a PING echoes it back as a
// PONG (is_pong=true, same nonce). The first candidate whose PING→PONG→PING round
// completes becomes the chosen match path, and its round-trip is the first RTT
// sample. Rides the SAME UDP socket the match then borrows, so it needs a MsgType
// tag to sit alongside Input/Hash/Hello.
struct PunchFrame {
    std::uint32_t nonce = 0;
    bool is_pong = false;
};

// The peer-drop control message (ADR-0011 Risks, "Dropped/late peers"): the
// HOST announces "seat `seat` produced no input from tick `at_tick` on, hand it
// to the AI". Every peer applies it at that exact tick, so the deterministic
// AISystem derives identical inputs everywhere and the hash stays equal.
//
// `at_tick` is RETROACTIVE — the first tick for which the host holds no input
// from that seat, which is at or below every peer's `confirmed_tick()`. It is
// deliberately NOT a tick in the future: ticks between the seat's last input and
// a future handoff tick could never be CONFIRMED (their missing input never
// arrives), so the session would keep speculating and never unstall — exactly
// the hang this message exists to cure. Placing it at the first missing tick
// means a peer that has not received the message yet is simply still stalled
// there, so a late arrival always lands inside the rollback window.
struct DropFrame {
    std::uint8_t seat = 0;       // the dropped seat index (< sim::kMaxPlayers)
    std::uint32_t at_tick = 0;   // first tick simulated with that seat on AI
};

// A CONTIGUOUS run of input frames sharing one seat_mask — the redundancy the
// lockstep session sends every tick so a dropped UDP packet is recovered by the
// next one (each packet re-carries the whole un-confirmed local-input window).
// per_tick[i] is tick `first_tick + i`; only `seat_mask` seats are meaningful.
struct InputRangeFrame {
    std::uint32_t first_tick = 0;
    std::uint16_t seat_mask = 0;
    std::vector<sim::TickInputs> per_tick;
};

// One decoded datagram: exactly one of `input` / `range` / `hash` / `hello` /
// `punch` / `drop` is meaningful per `type`.
struct Message {
    MsgType type = MsgType::Input;
    InputFrame input;
    InputRangeFrame range;
    HashFrame hash;
    HelloFrame hello;
    PunchFrame punch;
    DropFrame drop;
};

// [MsgType::Input][input_codec frame] — the seats in `seat_mask`, stamped `tick`.
std::vector<std::uint8_t> encode_input(std::uint32_t tick_index, std::uint16_t seat_mask,
                                       const sim::TickInputs& inputs);

// [MsgType::InputRange][first_tick u32-LE][count u8][seat_mask u16-LE]
//   [count * (one packed byte per set seat)] — `per_tick.size()` consecutive
// ticks from `first_tick`, each carrying `seat_mask`'s seats. count is capped at
// 255 (the input-delay window is tiny); an empty range encodes nothing useful
// and is rejected on decode.
std::vector<std::uint8_t> encode_input_range(std::uint32_t first_tick, std::uint16_t seat_mask,
                                             const std::vector<sim::TickInputs>& per_tick);

// [MsgType::Hash][tick u32-LE][hash u64-LE] — 13 bytes.
std::vector<std::uint8_t> encode_hash(std::uint32_t tick_index, std::uint64_t hash);

// [MsgType::Hello][seed u32-LE][is_ack u8] — 6 bytes. The host sends its seed
// (is_ack=false); the guest replies with is_ack=true (seed field ignored).
std::vector<std::uint8_t> encode_hello(std::uint32_t seed, bool is_ack);

// [MsgType::Punch][nonce u32-LE][is_pong u8] — 6 bytes. A hole-punch PING
// (is_pong=false) or the PONG echo of one (is_pong=true, same nonce).
std::vector<std::uint8_t> encode_punch(std::uint32_t nonce, bool is_pong);

// [MsgType::Drop][seat u8][at_tick u32-LE] — 6 bytes. Decode rejects a seat
// index outside [0, sim::kMaxPlayers).
std::vector<std::uint8_t> encode_drop(std::uint8_t seat, std::uint32_t at_tick);

// Decode a datagram produced by encode_input/encode_hash. Returns false (leaving
// *out untouched) on an unknown tag, a short buffer, or a malformed payload.
bool decode(const std::uint8_t* data, std::size_t size, Message* out);

}  // namespace bomber::net
