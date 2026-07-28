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
//   v2 -> v3: added MsgType::Drop (the peer-drop -> AI handoff control message,
//             ADR-0011 Risks "Dropped/late peers").
//   v3 -> v4: added MsgType::SetupPreview / SetupChunk / SetupAck — the
//             host-authoritative match-setup layer (setup_session.hpp) and its
//             chunked sim::MatchConfig payload (match_config_codec.hpp).
//   v4 -> v5: SetupAck grew a `seat` byte (9 -> 10 bytes). It is what turns the
//             setup layer from two-peer into N-peer: the host tracks a per-seat
//             ack mask and only reaches Phase::Final once EVERY guest has
//             acknowledged the current revision, instead of latching on the
//             first ack to arrive over the star (setup_session.hpp).
//   v5 -> v6: the MatchConfig blob lost its 13-byte `born_with` block
//             (kMatchConfigLayout 1 -> 2). The scheme's "-P born with" field is
//             a COUNT that REPLACES the VALUELST starting inventory, which the
//             original expresses by writing the value table id the baseline is
//             read from — so it now lands in tuning.start_with[] (already on
//             the wire) instead of a channel of its own (docs/re/facts.md "The
//             .SCH -P row's 2nd field is a COUNT that REPLACES the starting
//             inventory"). Refusing pre-v6 peers at the lobby door is the point:
//             a v5 peer decoding a v6 blob would read every field after
//             `forbidden` shifted by 13 bytes.
inline constexpr std::uint32_t kWireProtocolVersion = 6;

}  // namespace bomber::net
