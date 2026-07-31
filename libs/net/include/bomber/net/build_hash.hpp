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

// Bump when the wire codec/protocol changes shape but sim BEHAVIOUR does not (a
// behaviour change is caught by the reference-scenario hash instead).
//
//   v2  MsgType::Punch — the NAT hole-punch PING/PONG (ADR-0011 §3)
//   v3  MsgType::Drop — the peer-drop -> AI handoff
//   v4  MsgType::SetupPreview/SetupChunk/SetupAck — host-driven match setup
//   v5  SetupAck grew a `seat` byte: the setup layer became N-peer
//   v6  the MatchConfig blob lost `born_with` (kMatchConfigLayout 1 -> 2)
//   v7  MsgType::Probe — mutual path verification (link_probe.hpp)
//   v8  MsgType::MatchCtl — the host-authoritative match shell (design §10)
//   v9  MsgType::HostLost — host migration's detection message (design §8.1)
//
// EVERY ONE OF THESE REFUSES THE OLDER PEER AT THE LOBBY DOOR, and the bar is
// SILENT DIVERGENCE rather than "it misses a feature". v9 is the clearest case:
// a v8 peer ignores the announcement but keeps PLAYING, so the v9 survivors
// re-simulate the migration ticks with the seat on AI while it re-simulates the
// same ticks with the hub's last predicted input. Both run on happily with
// DIFFERENT hashed State, and the hash exchange reports a desync a few ticks
// later blaming the wrong machine.
inline constexpr std::uint32_t kWireProtocolVersion = 9;

}  // namespace bomber::net
