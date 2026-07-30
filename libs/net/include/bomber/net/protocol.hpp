#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "bomber/net/input_codec.hpp"
#include "bomber/net/match_config_codec.hpp"

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
    Drop = 5,
    SetupPreview = 6,
    SetupChunk = 7,
    SetupAck = 8,
    Probe = 9,
    MatchCtl = 10,
    HostLost = 11
};

// THE MATCH-SHELL CONTROL MESSAGE (wire v8). Everything above is about the
// CONTENT of a match — inputs, hashes, the config it is built from. This one is
// about the SHELL around it: when a round stops and when the peers leave the
// outcome screens. Both used to be LOCAL decisions on each machine, which is a
// desync by construction — one peer's Esc froze its own sim while the other kept
// ticking, and "the match is over" was reached (and the socket dropped) whenever
// each peer independently felt like it.
//
// HOST-AUTHORITATIVE, exactly like the original's network screens
// (docs/re/network-screens.md §7): the machine driving the game decides and
// broadcasts; a client (`sub_40C06A() == 1`) may only ask. That is what the four
// kinds encode — see MatchCtlKind.
enum class MatchCtlKind : std::uint8_t {
    // HOST -> everyone. "Stop this round; `at_tick` is the first tick NOBODY
    // simulates." The outcome is a DRAW by DECREE, not by inspecting the frozen
    // state: an abandoned round tallies nothing, so the two peers cannot
    // disagree about who won it. Idempotent and order-free like MsgType::Drop —
    // the EARLIEST announced tick wins — and re-sent every pump, because a peer
    // that misses it keeps simulating into a round the host has already left.
    //
    // `at_tick` is in the FUTURE (the host's own speculative tick plus a lead
    // above the prediction cap), unlike DropFrame's deliberately retroactive
    // one. The reasons are opposite: a drop tick must be reachable when a seat's
    // input will NEVER arrive, whereas here every seat is live and still
    // sending, so a future tick is reachable by definition — and it has to be
    // future, because a peer that has already speculated past it would stop
    // having simulated (and tallied) more ticks than the host did.
    EndRound = 0,
    // GUEST -> host. "The player at this machine pressed Esc." A REQUEST, never
    // an act: a guest that ended its own round would be simulating a different
    // number of ticks than the host, which is the whole class of bug this
    // message exists to remove. Carries no tick (the host owns that). Re-sent
    // every pump until the host's EndRound comes back.
    EndRoundRequest = 1,
    // HOST -> everyone. Pure LIVENESS while the host is reading the post-match
    // RESULTS/VICTORY screens. Without it a guest waiting on the host cannot
    // tell "still deciding" from "gone", and the only safe reading of silence is
    // "gone" — which is precisely the connection drop this whole change removes.
    RematchWait = 2,
    // HOST -> everyone. "I am leaving the outcome screens for the setup screens
    // now." The guests follow, and the SAME transport carries the next
    // SetupSession — so finishing a match returns both peers to map selection
    // with the session intact instead of tearing it down.
    Rematch = 3,
};

// One match-shell control datagram. `at_tick` is meaningful for EndRound only;
// every other kind sends 0 and ignores it.
struct MatchCtlFrame {
    std::uint32_t at_tick = 0;
    MatchCtlKind kind = MatchCtlKind::EndRound;
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

// One PATH-VERIFICATION probe (link_probe.hpp). The punch proves a path to the
// peer that RECEIVED the pong; it proves nothing about whether the peer reached
// the same conclusion before its own deadline, and a peer that decides
// differently ends up on a transport the other one is not on. The probe is the
// mutual half: it runs over the CHOSEN Transport (direct socket or relay) and
// both ends must agree before the match layer is handed anything.
//
// `nonce` is the sender's per-seat punch nonce, so a reflected copy of our own
// datagram (the star hub reflects) is recognised and ignored. `seen_peer` is the
// whole protocol: "I have already received a datagram from you on this path".
// Receiving a probe proves peer→me; receiving one with seen_peer set proves
// me→peer as well, which is the mutual proof neither side can derive alone.
struct ProbeFrame {
    std::uint32_t nonce = 0;
    bool seen_peer = false;
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

// HOST MIGRATION's detection message (wire v9, ADR-0011 decision 5,
// docs/online-multiplayer-design.md §8.1): the drop->AI handoff of the HUB's own
// seat. It is a SEPARATE message from DropFrame despite carrying identical
// fields, and the difference is authority, not payload:
//
//   * a Drop is the hub's DECREE — exactly one machine ever sends it, and every
//     peer obeys it unconditionally;
//   * a HostLost is a survivor's OBSERVATION about the machine that would
//     otherwise have decreed it. Nobody has authority here, because the only
//     peer that had it is the one that died, so EVERY survivor announces and
//     every survivor re-sends (broadcast_host_lost is not hub-only).
//
// Keeping them apart is what lets a peer accept "the hub is gone" from a
// non-hub, while still refusing an ordinary guest-drop decree from one. Sharing
// the Drop tag would have made any guest able to hand any seat to the AI.
//
// `at_tick` is RETROACTIVE for exactly DropFrame's reason (see above), plus one
// of its own: survivors DISAGREE about it. It is a local quantity — peers hold
// different amounts of a dying hub's final output — so proposals differ by
// however many of its last datagrams each happened to lose. The LOWEST proposal
// wins everywhere, which is monotone and therefore needs no agreement protocol;
// adopting one below our own confirmed frontier is the single case that makes
// the session un-confirm (RollbackSession::rewind_for_migration).
struct HostLostFrame {
    std::uint8_t seat = 0;      // the seat that was the hub (< sim::kMaxPlayers)
    std::uint32_t at_tick = 0;  // announcer's first tick with no input from it
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

// --- host-driven match setup (setup_session.hpp) ------------------------------
//
// The original's shape, reproduced over our own P2P transport
// (docs/re/network-screens.md §7): after the connect screens every peer lands in
// the ORDINARY roster and level screens, the HOST drives them and broadcasts
// each change (roster slot kind 40, team kind 58, level kind 43, rounds kind
// 44), and guests are strictly read-only. Our three messages collapse that into
// a LIVE PREVIEW (everything a read-only display needs, re-sent for UDP loss)
// plus the FINAL authoritative config (the whole resolved sim::MatchConfig,
// chunked) and its acknowledgement.

// The longest level name a preview may carry. Longer names are rejected on
// decode rather than truncated — a name that does not fit is a sender that
// disagrees with us about the protocol.
inline constexpr std::size_t kSetupLevelNameMax = 32;

// One slot's role as the HOST is showing it on the roster screen. Enough for a
// read-only display and nothing more — the AUTHORITATIVE roster is the `ai` /
// `active` / `team` arrays inside the final MatchConfig. `Remote` mirrors the
// original's input type 4, the only marker of "someone else's player"
// (sub_40D372 applies a kind-40 upload as sub_421E33(slot, 4, 0)); type 0 = OFF.
enum class SetupSlotKind : std::uint8_t {
    Off = 0,
    Human = 1,
    Ai = 2,
    Remote = 3,
};

// The live preview the host re-broadcasts while the user edits the roster and
// level screens. DISPLAY ONLY — never build a Simulation from this (see
// setup_session.hpp's header comment for why). `level_index` is deliberately
// unvalidated beyond its byte range: it indexes the HOST's level list, whose
// length is a GUI concern, so the display must bounds-check it itself.
struct SetupPreviewFrame {
    std::string level_name;
    std::uint32_t revision = 0;
    std::array<SetupSlotKind, sim::kMaxPlayers> slots{};
    std::array<std::uint8_t, sim::kMaxPlayers> team{};
    std::uint8_t level_index = 0;
    std::uint8_t rounds = 1;
};

// One slice of an encode_match_config() blob. `revision` identifies the
// confirmed setup, `checksum` is over the WHOLE blob (not this slice), so a
// receiver can only apply a config it reassembled completely and intact.
struct SetupChunkFrame {
    std::vector<std::uint8_t> payload;
    std::uint32_t revision = 0;
    std::uint32_t total_len = 0;
    std::uint32_t checksum = 0;
    std::uint8_t chunk_count = 0;
    std::uint8_t chunk_index = 0;
};

// "Seat `seat` reassembled and decoded revision `revision`, whose blob checksums
// to `checksum`." Carrying the checksum (not just the revision) means the host
// learns the guest latched the SAME bytes, not merely something with the same
// label; carrying the SEAT is what makes a >2-peer lobby possible at all. Over a
// StarHubTransport the host's chunks reach every guest, so an unattributed ack
// would only ever mean "somebody has it" — the host would start the match while
// another guest was still reassembling. With the seat in the frame the host
// keeps a per-seat mask and reaches Phase::Final only when every expected guest
// has acked the CURRENT revision (setup_session.hpp).
//
// A machine that owns several seats sends one ack PER SEAT, so the host's mask
// is satisfied whatever the machine↔seat grouping is — the host never has to
// know which seats share a machine.
struct SetupAckFrame {
    std::uint32_t revision = 0;
    std::uint32_t checksum = 0;
    std::uint8_t seat = 0;  // < sim::kMaxPlayers; bounds-checked on decode
};

// Bytes of config carried per chunk. 1024 keeps the whole datagram at 1039
// bytes — under the ~1200-byte safe UDP payload even after RelayedTransport's
// 17-byte header, so nothing depends on IP fragmentation surviving the path.
inline constexpr std::size_t kSetupChunkPayloadBytes = 1024;
inline constexpr std::size_t kSetupChunkHeaderBytes = 1 + 4 + 4 + 4 + 1 + 1;  // 15
// Derived, so the two bounds can never drift apart.
inline constexpr std::size_t kMaxSetupChunks =
    (kMaxMatchConfigBytes + kSetupChunkPayloadBytes - 1) / kSetupChunkPayloadBytes;

// One decoded datagram: exactly one of `input` / `range` / `hash` / `hello` /
// `punch` / `drop` / `setup_preview` / `setup_chunk` / `setup_ack` /
// `match_ctl` / `host_lost` is meaningful per `type`.
struct Message {
    MsgType type = MsgType::Input;
    InputFrame input;
    InputRangeFrame range;
    HashFrame hash;
    HelloFrame hello;
    PunchFrame punch;
    ProbeFrame probe;
    DropFrame drop;
    SetupPreviewFrame setup_preview;
    SetupChunkFrame setup_chunk;
    SetupAckFrame setup_ack;
    MatchCtlFrame match_ctl;
    HostLostFrame host_lost;
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

// [MsgType::Probe][nonce u32-LE][seen_peer u8] — 6 bytes. A path-verification
// probe over the chosen Transport (link_probe.hpp).
std::vector<std::uint8_t> encode_probe(std::uint32_t nonce, bool seen_peer);

// [MsgType::Drop][seat u8][at_tick u32-LE] — 6 bytes. Decode rejects a seat
// index outside [0, sim::kMaxPlayers).
std::vector<std::uint8_t> encode_drop(std::uint8_t seat, std::uint32_t at_tick);

// [MsgType::MatchCtl][kind u8][at_tick u32-LE] — 6 bytes. Decode rejects a kind
// above Rematch, exactly as MsgType::Drop rejects an out-of-range seat: an
// unknown kind means a peer that disagrees with us about the protocol, and the
// build_hash door is what is supposed to have caught that.
std::vector<std::uint8_t> encode_match_ctl(MatchCtlKind kind, std::uint32_t at_tick);

// [MsgType::HostLost][seat u8][at_tick u32-LE] — 6 bytes. Byte-identical in
// shape to MsgType::Drop and deliberately NOT the same tag; see HostLostFrame on
// why the authority difference needs its own opcode. Decode rejects a seat index
// outside [0, sim::kMaxPlayers), exactly as Drop does.
std::vector<std::uint8_t> encode_host_lost(std::uint8_t seat, std::uint32_t at_tick);

// [MsgType::SetupPreview][revision u32-LE][level_index u8][rounds u8]
//   [name_len u8][name_len bytes][10 * slot_kind u8][10 * team u8] — 28..60
// bytes. Decode rejects a name longer than kSetupLevelNameMax, a name byte
// outside printable ASCII (a hostile peer must not be able to push control
// characters into the host's level label), a slot kind above Remote, and any
// length that disagrees with name_len.
std::vector<std::uint8_t> encode_setup_preview(const SetupPreviewFrame& preview);

// [MsgType::SetupChunk][revision u32-LE][total_len u32-LE][checksum u32-LE]
//   [chunk_count u8][chunk_index u8][payload] — 15 + up to 1024 bytes. Decode
// rejects total_len of 0 or above kMaxMatchConfigBytes, a chunk_count that is
// not exactly ceil(total_len / kSetupChunkPayloadBytes), an index outside that
// count, and a payload whose length disagrees with the slice it claims to be —
// so a receiver can never be talked into a short or overlapping reassembly.
std::vector<std::uint8_t> encode_setup_chunk(const SetupChunkFrame& chunk);

// [MsgType::SetupAck][revision u32-LE][checksum u32-LE][seat u8] — 10 bytes.
// Decode rejects a seat outside [0, sim::kMaxPlayers), exactly as MsgType::Drop
// does: the field indexes a per-seat mask, so it is bounds-checked at the wire.
std::vector<std::uint8_t> encode_setup_ack(std::uint32_t revision, std::uint32_t checksum,
                                           std::uint8_t seat);

// Decode a datagram produced by encode_input/encode_hash. Returns false (leaving
// *out untouched) on an unknown tag, a short buffer, or a malformed payload.
bool decode(const std::uint8_t* data, std::size_t size, Message* out);

}  // namespace bomber::net
