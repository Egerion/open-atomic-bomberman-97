#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "bomber/net/input_codec.hpp"
#include "bomber/net/match_config_codec.hpp"

// The message layer over the raw input codec: every packet carries a one-byte
// MsgType tag ahead of its payload. INPUT reuses input_codec's frame verbatim;
// HASH is a tick plus the state_hash digest the desync detector compares
// (ADR-0010: state_hash IS the on-wire integrity check). Untrusted-input safe
// throughout — decode() bounds-checks the tag and the payload.

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

// THE MATCH-SHELL CONTROL MESSAGE (wire v8, docs/online-multiplayer-design.md
// §10). Everything above is about the CONTENT of a match; this one is about the
// SHELL around it — when a round stops and when the peers leave the outcome
// screens. Both used to be LOCAL decisions, which is a desync by construction.
//
// HOST-AUTHORITATIVE, like the original's network screens
// (docs/re/network-screens.md §7), and "the host" means
// RollbackSession::hosting() — the ELECTED hub, not whoever pressed Host.
enum class MatchCtlKind : std::uint8_t {
    // HOST -> everyone. "Stop this round; `at_tick` is the first tick NOBODY
    // simulates." The outcome is a DRAW BY DECREE, not read out of the frozen
    // state, so the two peers cannot disagree about who won it. Idempotent and
    // order-free — the EARLIEST tick wins — and re-sent every pump.
    //
    // `at_tick` is in the FUTURE, unlike DropFrame's deliberately retroactive
    // one, and the reasons are opposite: a drop tick must be reachable when a
    // seat's input will NEVER arrive, whereas here every seat is live, so a
    // future tick is reachable by definition — and it MUST be future, because a
    // peer that had already speculated past it would stop having simulated (and
    // tallied) more of the round than the host did.
    EndRound = 0,
    // RETIRED, AND DELIBERATELY STILL HERE. It used to be a guest's request that
    // the host granted, i.e. a lever letting any guest force-end any round with
    // no host confirmation. Nothing sends it and nothing acts on an inbound one.
    //
    // THE ENUMERATOR STAYS and decode() still accepts it, because
    // kWireProtocolVersion did not move: a peer on the previous build still
    // connects and still sends this, and the point is to turn that peer AWAY
    // rather than disconnect it on an unknown kind. Removing the value would
    // also renumber RematchWait/Rematch, which is a wire break for no gain.
    EndRoundRequest = 1,
    // HOST -> everyone. Pure LIVENESS while the host reads the post-match
    // screens. Without it a guest cannot tell "still deciding" from "gone", and
    // the only safe reading of silence is "gone".
    RematchWait = 2,
    // HOST -> everyone. "I am leaving the outcome screens for the setup screens."
    // The guests follow and the SAME transport carries the next SetupSession.
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
// before any INPUT/HASH flows, so both peers agree on the shared match seed. The
// HOST announces its seed (is_ack=false); the GUEST replies with an ACK
// (is_ack=true, seed unused). Both re-send every pump so a drop self-heals.
struct HelloFrame {
    std::uint32_t seed = 0;
    bool is_ack = false;
};

// One NAT hole-punch probe (ADR-0011 §3, design §3): the Rendezvous sends a PING
// (is_pong=false) with a fresh `nonce` to each of the peer's candidate
// addresses; a peer receiving one echoes it as a PONG (same nonce). The first
// candidate whose round completes becomes the match path, and its round-trip is
// the first RTT sample. Rides the SAME UDP socket the match then borrows.
struct PunchFrame {
    std::uint32_t nonce = 0;
    bool is_pong = false;
};

// One PATH-VERIFICATION probe (link_probe.hpp, design §4.1). `nonce` is the
// sender's per-seat punch nonce, so a reflected copy of our own datagram is
// recognised and ignored. `seen_peer` is the whole protocol: receiving a probe
// proves peer->me, and receiving one with seen_peer set proves me->peer too —
// the mutual proof neither side can derive alone.
struct ProbeFrame {
    std::uint32_t nonce = 0;
    bool seen_peer = false;
};

// The peer-drop control message: the HOST announces "seat `seat` produced no
// input from tick `at_tick` on, hand it to the AI". Every peer applies it at that
// exact tick, so the deterministic AISystem derives identical inputs everywhere.
//
// `at_tick` is RETROACTIVE — the first tick the host holds no input for — and
// deliberately NOT in the future: ticks between the seat's last input and a
// future handoff tick could never be CONFIRMED, so the session would speculate
// and never unstall, which is the hang this message cures. At the first missing
// tick, a peer that has not received the message is simply still stalled there,
// so a late copy always lands inside the rollback window.
struct DropFrame {
    std::uint8_t seat = 0;      // the dropped seat index (< sim::kMaxPlayers)
    std::uint32_t at_tick = 0;  // first tick simulated with that seat on AI
};

// HOST MIGRATION's detection message (wire v9, design §8.1). A SEPARATE message
// from DropFrame despite identical fields, and the difference is AUTHORITY: a
// Drop is the hub's decree, a HostLost is a survivor's observation about the
// machine that would otherwise have decreed it. Sharing the tag would let any
// guest hand any seat to the AI.
//
// `at_tick` is RETROACTIVE for DropFrame's reason plus one of its own: survivors
// DISAGREE about it, holding different amounts of a dying hub's final output.
// The LOWEST proposal wins — monotone, so no agreement protocol — and adopting
// one below our own frontier is the single case that un-confirms the session.
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

// --- host-driven match setup (setup_session.hpp, design §9) -------------------
//
// The original's shape over our own P2P transport (docs/re/network-screens.md
// §7): the HOST drives the roster and level screens and broadcasts each change,
// and guests are strictly read-only. Our three messages collapse that into a
// LIVE PREVIEW (everything a read-only display needs, re-sent for UDP loss) plus
// the FINAL authoritative config (the whole resolved sim::MatchConfig, chunked)
// and its acknowledgement.

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
// to `checksum`." The CHECKSUM (not just the revision) tells the host the guest
// latched the SAME bytes, not merely something with the same label. The SEAT is
// what makes a >2-peer lobby possible: over a StarHubTransport the host's chunks
// reach every guest, so an unattributed ack would only mean "somebody has it"
// and the host would start while another guest was still reassembling. A machine
// owning several seats sends one ack PER SEAT, so the host never has to know
// which seats share a machine.
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

// --- the encoders -------------------------------------------------------------
//
// Byte layouts and the exact validation each decode arm performs are tabulated in
// docs/net-wire-format.md. The rule they all share: a field that indexes anything
// (a seat, a chunk, an enum) is bounds-checked AT THE WIRE, because an
// out-of-range value means a peer that disagrees with us about the protocol and
// the build_hash door is what was supposed to have caught that.

std::vector<std::uint8_t> encode_input(std::uint32_t tick_index, std::uint16_t seat_mask,
                                       const sim::TickInputs& inputs);
// `per_tick.size()` consecutive ticks from `first_tick`, each carrying
// `seat_mask`'s seats. The count rides in a u8, and an empty range is rejected.
std::vector<std::uint8_t> encode_input_range(std::uint32_t first_tick, std::uint16_t seat_mask,
                                             const std::vector<sim::TickInputs>& per_tick);
std::vector<std::uint8_t> encode_hash(std::uint32_t tick_index, std::uint64_t hash);
std::vector<std::uint8_t> encode_hello(std::uint32_t seed, bool is_ack);
std::vector<std::uint8_t> encode_punch(std::uint32_t nonce, bool is_pong);
std::vector<std::uint8_t> encode_probe(std::uint32_t nonce, bool seen_peer);
std::vector<std::uint8_t> encode_drop(std::uint8_t seat, std::uint32_t at_tick);
std::vector<std::uint8_t> encode_match_ctl(MatchCtlKind kind, std::uint32_t at_tick);
// Byte-identical in shape to encode_drop and deliberately NOT the same tag; see
// HostLostFrame on why the authority difference needs its own opcode.
std::vector<std::uint8_t> encode_host_lost(std::uint8_t seat, std::uint32_t at_tick);
std::vector<std::uint8_t> encode_setup_preview(const SetupPreviewFrame& preview);
std::vector<std::uint8_t> encode_setup_chunk(const SetupChunkFrame& chunk);
std::vector<std::uint8_t> encode_setup_ack(std::uint32_t revision, std::uint32_t checksum,
                                           std::uint8_t seat);

// Returns false — leaving *out untouched — on an unknown tag, a short buffer or
// a malformed payload.
bool decode(const std::uint8_t* data, std::size_t size, Message* out);

}  // namespace bomber::net
