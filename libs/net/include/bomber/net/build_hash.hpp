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
//   v6 -> v7: added MsgType::Probe — the mutual path verification both peers now
//             run over the CHOSEN transport before the match layer is handed
//             anything (link_probe.hpp). A v6 peer answers no probe, so its
//             partner would verify nothing and relay for no reason; refusing the
//             pair at the lobby door is cheaper than a match that connects and
//             then delivers nothing, which is the exact failure this closes.
//   v7 -> v8: added MsgType::MatchCtl — the host-authoritative MATCH SHELL
//             (protocol.hpp's MatchCtlKind): "this round is abandoned, it ends
//             at tick X and counts as a draw", a guest's request for the same,
//             and the post-match "back to the setup screens" handoff that keeps
//             the transport alive between matches. A v7 peer answers none of it:
//             it would keep simulating a round its partner has already left, and
//             would drop the socket the moment a match ended. Both are silent
//             divergences rather than loud ones, so the pair has to be refused
//             at the lobby door.
//   v8 -> v9: added MsgType::HostLost — HOST MIGRATION's detection message
//             (protocol.hpp's HostLostFrame, design §8.1): "the HUB's seat went
//             silent, hand it to the AI from tick X", announced by any survivor
//             because the one machine that could have decreed it is the one that
//             died. It is a distinct tag from MsgType::Drop despite an identical
//             payload, precisely so a peer can accept "the hub is gone" from a
//             non-hub without also accepting an ordinary seat-drop decree from
//             one.
//             A v8 peer is refused for a reason stronger than "misses a
//             feature": it answers the message with silence but keeps PLAYING.
//             Its hub dies, the v9 survivors adopt a handoff tick, re-elect and
//             re-simulate those ticks with the seat on AI — and the v8 peer,
//             having ignored the announcement, simulates the same ticks with the
//             hub's last predicted input. Both sides then run on happily with
//             DIFFERENT hashed State. That is a silent divergence that the hash
//             exchange reports as a desync a few ticks later, blaming the wrong
//             machine; and if the v8 peer is itself the elected successor it will
//             never take the role at all, so the survivors wait forever on a hub
//             that does not know it is one. Neither failure is recoverable
//             in-match, so the pair is refused at the lobby door.
inline constexpr std::uint32_t kWireProtocolVersion = 9;

}  // namespace bomber::net
