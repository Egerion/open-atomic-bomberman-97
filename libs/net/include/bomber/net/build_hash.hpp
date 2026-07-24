#pragma once

#include <cstdint>

namespace bomber::net {

// The cross-build compatibility digest (ADR-0011 "bit-identical sim builds"): a
// 32-bit fingerprint two online peers compare BEFORE a match to reject
// incompatible builds LOUDLY — the matchmaking server at lobby join, and the
// P2P Hello at tick 0 — instead of silently desyncing mid-game.
//
// It folds two cross-platform-STABLE inputs:
//   1. the state_hash of a fixed, asset-free reference scenario run through the
//      deterministic sim. It changes iff sim BEHAVIOUR changes — any added,
//      removed or reordered RNG draw, any movement/bomb/flame arithmetic shift,
//      any tick-order change. Because state_hash digests FIELDS (not raw bytes),
//      an MSVC build and a GCC build of the same source produce the SAME value,
//      so cross-platform peers still match.
//   2. kWireProtocolVersion — bumped by hand when the on-wire codec/protocol
//      (input_codec / protocol) changes shape without changing sim behaviour.
//
// It deliberately mixes in NOTHING padding-dependent (sizeof, raw layout): that
// would make two behaviour-compatible builds on different compilers disagree and
// wrongly refuse to play. Computed once, then cached.
std::uint32_t build_hash();

// Bump when the wire codec/protocol format changes but sim behaviour does not
// (e.g. a new MsgType, a widened field). A sim behaviour change is caught by the
// reference-scenario hash instead, so it need NOT bump this.
//   v1 -> v2: added MsgType::Punch (the NAT hole-punch PING/PONG, ADR-0011 §3).
inline constexpr std::uint32_t kWireProtocolVersion = 2;

}  // namespace bomber::net
