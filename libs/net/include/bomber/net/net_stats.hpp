#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

#include "bomber/net/net_path.hpp"
#include "bomber/sim/constants.hpp"  // sim::kMaxPlayers

// IN-MATCH NETPLAY INSTRUMENTATION — the numbers behind the overlay, and behind
// the line a dead session leaves in netdiag.log.
//
// WHY THIS EXISTS. The game showed nothing at all about the network while a
// match ran, so three different failures in one day had to be diagnosed from the
// server side or not at all: "it stutters" (unmeasurable), "it suddenly cut out"
// (nobody could say whether the server was even in the path), and a relay bug
// that had to be inferred from matchmaker logs because the client was silent.
//
// TWO RULES SHAPED THE DESIGN, and both are load-bearing:
//
//  1. NO NEW WIRE MESSAGE. Every number here is derived from traffic that
//     already flows. `kWireProtocolVersion` is at 8 and the unmerged
//     host-migration branch is earmarked for 9; taking 9 for a diagnostic would
//     push that branch to 10 and invalidate every build in the wild — the
//     build_hash door refuses a mismatch, so each bump means hand-delivering
//     exes. Nothing in this file encodes or decodes anything, so build_hash is
//     untouched and a machine running this build still plays a machine that is
//     not. See the derivations on each field for how far that gets us, and where
//     it costs accuracy (the ack-RTT is the one real compromise).
//
//  2. THE COUNTING MUST NOT PERTURB WHAT IT MEASURES. Every buffer here is a
//     fixed-size array owned by value: no allocation on any packet path, no
//     logging in the hot path, no map lookups. The whole tracker is a few
//     hundred bytes and every per-packet operation is O(1) integer work.
//
// SDL-FREE and CLOCK-INJECTED, exactly like LobbyFlow/Rendezvous: the caller
// hands in a monotonic millisecond stamp, so the whole thing is unit-testable
// headless with a synthetic clock, and no wall clock is created inside libs/net.
// Nothing here ever reaches libs/sim — no counter, no timer, no hashed field —
// so no golden hash can move (determinism rule 1).

namespace bomber::net {

// The sim's fixed tick rate. A peer pumps its session once per tick and sends a
// datagram every pump (RollbackSession::send_local runs on every path through
// advance(), including the stalled-at-the-cap and round-ended ones), so this is
// also the EXPECTED inbound datagram rate per remote seat — which is what makes
// the loss estimate below possible without a sequence number.
inline constexpr int kPumpHz = 20;

// tick -> "when did our input for this tick first leave" ring. 512 ticks is
// ~25 s at 20 Hz, far longer than any RTT worth reporting, so an entry is never
// overwritten before its acknowledgement could arrive. Indexed by tick % size
// with the tick stored alongside, so a wrapped slot is detected rather than
// mistaken for a hit.
inline constexpr std::size_t kSendRing = 512;

// The RTT sparkline: 48 buckets of 100 ms = 4.8 s of history. A single RTT
// number is much less useful than seeing it spike, and each bucket keeps the
// WORST sample in its 100 ms so decimation cannot hide a spike.
inline constexpr std::size_t kRttHistory = 48;
inline constexpr std::int64_t kRttBucketMs = 100;

// Rates (datagrams/s, rollbacks/s, re-sim ticks/s) are recomputed on this
// boundary from counters reset each time. Not a moving average: a plain
// completed-window count, so the number on screen is a fact about the last
// second rather than a blend of an unknown span.
inline constexpr std::int64_t kRateWindowMs = 1000;

// A sample above this is discarded rather than reported. It can only come from a
// wrapped ring slot or a clock that jumped; either way it is not an RTT.
inline constexpr int kRttSaneMaxMs = 10000;

// Per REMOTE seat. Everything is exact unless the comment says otherwise; the
// two estimates are called out explicitly because the owner reads these as fact.
struct PeerStats {
    // EXACT. This seat is a network peer whose input we exchange.
    bool tracked = false;
    // EXACT. False once the seat has been handed to the AI (or its loss ended
    // the match). A dropped seat stops producing traffic, so its rates go to
    // zero legitimately — this flag is what stops that reading as loss.
    bool live = true;

    // EXACT, and the most honest "late arrival" number this architecture has.
    // How many ticks the newest input we hold from this seat trails our own
    // speculative head. In a healthy rollback session it hovers around the true
    // one-way delay in ticks; it climbs the moment the peer's input stops
    // keeping up, and it is what the prediction cap eventually clamps.
    //
    // (There is deliberately no "packets that arrived after their tick was
    // needed" counter: with zero input delay EVERY remote input necessarily
    // arrives after we speculated its tick — that is what rollback IS — so such
    // a counter would read 100% on a perfectly healthy link and mean nothing.
    // Lag, the misprediction count and the stall count are the real signals.)
    int lag_ticks = 0;
    int worst_lag_ticks = 0;

    // ESTIMATED — an UPPER BOUND on the true network round trip, never below it.
    // Derived with no new message, from the one acknowledgement the protocol
    // already contains: a peer's InputRange always starts at ITS confirmed
    // frontier, and it cannot confirm tick T until our input for T has arrived.
    // So the first datagram from this seat whose first_tick rises above T is a
    // proof that our input for T-1 completed a round trip, and the interval from
    // "our input for T-1 first left" to "that datagram arrived" is measured
    // exactly.
    //
    // It is an upper bound, not the network RTT, because it also contains:
    //   * up to one of the peer's 50 ms pumps (it can only answer on a pump);
    //   * any time the peer spent BEHIND tick T-1 before it could confirm it;
    //   * in a >2-seat match, the wait for the SLOWEST other seat, since a
    //     peer's confirmed frontier is gated by every seat it awaits;
    //   * `local_lead` * 50 ms, whenever the arrival-variance absorber has a lead
    //     in force — our input then reaches the peer before the peer reaches that
    //     tick, so the frontier rises on the peer's schedule and not on arrival.
    //     That case sets `rtt_offset_bound` unconditionally, below.
    // Read `rtt_min_ms` as the best estimate of the real path and `rtt_ms` /
    // the sparkline as the thing that spikes — BUT ONLY WHEN `rtt_offset_bound`
    // below is clear. Getting a tight pairwise RTT unconditionally would need a
    // ping message, and that costs a wire version — see the file header.
    int rtt_ms = -1;             // most recent sample; -1 until one exists
    int rtt_smooth_ms = -1;      // EWMA (1/4 weight) — the steady reading
    int rtt_min_ms = -1;         // best sample this session: the tightest bound on the path
    int rtt_max_ms = -1;         // worst sample this session
    int rtt_recent_max_ms = -1;  // worst within the sparkline window: "is it spiking NOW"
    int jitter_ms = 0;           // EWMA of |sample - previous sample|

    // EXACT, and the reading above is USELESS AS A PATH MEASUREMENT when this is
    // set. MEASURED, not theorised: two peers over UDP loopback (a ~25 ms path)
    // reported 33 ms and 333 ms respectively — because in rollback netcode the
    // peers settle into a WALL-CLOCK PHASE OFFSET, bounded only by the prediction
    // cap, and the peer that is AHEAD is measuring that offset rather than the
    // wire. Writing out the ack sample as max(offset, one_way) + one_way makes it
    // exact: while the offset exceeds the one-way delay the sample collapses onto
    // the offset, which is also what `lag_ticks` measures — so the two numbers
    // become the same number and neither says anything about the network. That
    // gives the detector for free: a healthy reading is ~2x the lag, a
    // contaminated one is ~1x (net_stats.cpp derives both).
    //
    // Hence this flag rather than a "corrected" figure. There is no correction:
    // once the offset dominates, the one-way delay is genuinely not observable
    // from this side without a message that carries a timestamp — and that costs
    // a wire version. Saying "not measurable right now" is the honest output, and
    // `lag_ticks` is the actionable number in that state anyway, since it is the
    // lag (not the path) that drives the stalls the player feels.
    bool rtt_offset_bound = false;

    // EXACT. Datagrams carrying this seat's input that we accepted, and those
    // whose content we already held. The duplicate count is EXPECTED to be large
    // and is not a fault: every peer re-sends its whole unconfirmed window every
    // pump, which is exactly how a lost datagram heals itself. It is here as a
    // liveness sanity check ("traffic is flowing"), not as a loss signal.
    std::uint32_t input_packets = 0;
    std::uint32_t dup_inputs = 0;

    // EXACT: datagrams from this seat counted over the last completed second.
    int recv_per_sec = 0;
    // ESTIMATED. 100 * (1 - recv_per_sec / kPumpHz), floored at 0. A peer sends
    // one datagram per pump, so a shortfall is either datagrams lost on the path
    // or the PEER'S OWN LOOP stalling (an alt-tab, a window drag, a slow frame).
    // This number cannot tell those two apart — nothing on the wire can, without
    // a sequence number we would have to add. Treat it as "the peer's voice is
    // thinner than it should be", not as a measured drop rate.
    int loss_pct_est = 0;

    // Newest LAST, oldest first; `rtt_history_len` entries are valid. One entry
    // per kRttBucketMs holding that bucket's WORST sample, so a spike survives.
    std::array<std::uint16_t, kRttHistory> rtt_history{};
    std::size_t rtt_history_len = 0;
};

// The whole live picture. A plain aggregate so the overlay, the log formatter
// and the tests all read the same snapshot.
struct NetStats {
    // EXACT, straight off the Transport actually carrying the bytes.
    NetPath path = NetPath::Unknown;

    // EXACT. The speculative head and the confirmed frontier, and the distance
    // between them — which IS "how deep predictions are running" and "how far
    // the confirmed frontier is behind the local head": one number, two names.
    std::uint32_t tick = 0;
    std::uint32_t confirmed = 0;
    int prediction_depth = 0;
    int worst_prediction_depth = 0;
    int max_prediction = 0;  // the cap the session was built with, for context

    // EXACT. Pumps on which the cap held the display instead of simulating. THIS
    // is what a netcode stutter actually is: a frame the game could not advance
    // because a peer's input had not arrived. If this is zero, a reported stutter
    // is not the netcode.
    std::uint32_t stall_pumps = 0;
    int stalls_per_sec = 0;

    // EXACT. Pumps this peer held DELIBERATELY, to give back clock skew its
    // partner had lost (rollback_session.hpp's re-phasing note). Distinct from a
    // stall on purpose: a stall is the cap refusing to let us run, a re-phase hold
    // is us choosing not to. A healthy match shows a burst of these after each
    // hitch and none in between; a permanently rising count means the two machines
    // cannot hold the same tick rate at all, which is a different fault.
    //
    // NOTE FOR READING A LOG: a re-phase hold RETURNS BEFORE the prediction-cap
    // check, so a pump spent holding is never also counted as a stall. `stalls=0`
    // on a line whose `rephase` is large therefore does NOT mean the cap was out
    // of the picture — add the two together to get "pumps that could not advance
    // the display", which is the number the player actually feels.
    std::uint32_t rephase_holds = 0;

    // --- THE ARRIVAL-VARIANCE FILTER (rollback_session.hpp's jitter note) -----
    //
    // EXACT. `frame_advantage` is the raw per-pump comparison the re-phase
    // controller is built on — our own prediction depth minus the peer's, which
    // cancels the path delay and leaves twice the clock skew. `sustained` is the
    // part of it that has held for the WHOLE filter window, and is what the
    // controller now acts on.
    //
    // On a clean path the two are the same number every pump. The GAP between them
    // is arrival variance, i.e. exactly the quantity that separated the silky live
    // sessions from the laggy ones, made visible instead of inferred.
    int frame_advantage = 0;
    int sustained_advantage = 0;

    // EXACT. Pumps where the RAW reading asked for a hold and the filter refused
    // — displayed ticks that jitter used to cost and no longer does. Zero on a
    // clean path, by construction: the filtered reading can never exceed the raw
    // one, so a pump the filter holds is a pump the raw reading would have held
    // too. This is the one number that says how much the absorber is doing.
    std::uint32_t rephase_suppressed = 0;

    // EXACT. The LOCAL INPUT LEAD in ticks (rollback_session.hpp's jitter note):
    // how far ahead of our own head we are filing and sending our input, so that
    // the peer has it before it needs it. This is the only thing here the player
    // PAYS for — each tick is 50 ms of local input lag — so it is on the overlay
    // rather than inferred, and it is 0 on any path that does not need it.
    int local_lead = 0;
    // EXACT. Pumps spent with a lead in force, i.e. how much of this session the
    // player paid input lag for. Zero on a clean path.
    std::uint32_t lead_pumps = 0;
    // EXACT. The arrival variance that set it: the spread (max minus min over the
    // window) of the peer's OWN prediction depth, which every InputRange carries
    // for free. A steady path reads 0 here however SLOW it is — which is what
    // stops a merely-distant peer from being charged input lag.
    int peer_depth_spread = 0;

    // EXACT. A rollback is one mispredicted remote input; `resim_ticks` is the
    // total number of ticks replayed to correct them. Re-sims per second is the
    // CPU cost of the correction, and a high count with a low rollback count
    // means corrections are landing deep.
    std::uint32_t rollbacks = 0;
    std::uint32_t resim_ticks = 0;
    int rollbacks_per_sec = 0;
    int resim_ticks_per_sec = 0;

    // EXACT. Every datagram the session pulled off the transport, and those that
    // failed decode() — a non-zero `rx_malformed` means something on the path is
    // corrupting or injecting, which no other number here would reveal.
    std::uint32_t rx_packets = 0;
    std::uint32_t rx_malformed = 0;
    int rx_per_sec = 0;

    // EXACT. The session's own loud states, mirrored here so the log line and the
    // overlay read one struct.
    bool desynced = false;
    std::uint32_t desync_tick = 0;
    bool aborted = false;
    std::uint16_t dropped_seats = 0;

    // True once a real clock has been supplied. Without it every time-derived
    // field above stays at its "unavailable" value rather than reading as zero.
    bool clocked = false;
    std::int64_t elapsed_ms = 0;

    std::array<PeerStats, sim::kMaxPlayers> peers{};
};

// The accumulator. Fed by RollbackSession at a handful of points; owns no heap.
//
// Every method is safe to call without a clock: pass -1 for `now_ms` (the
// default everywhere) and the tick-derived statistics still work exactly, while
// the time-derived ones stay unavailable. That is what lets the existing headless
// tests and any embedder keep calling advance() with no timing source.
class NetStatsTracker {
public:
    // Called once, when the session is built.
    void begin(NetPath path, std::uint16_t remote_seats, std::uint32_t start_tick,
               int max_prediction);

    // Called at the top of every pump. `now_ms` is the caller's monotonic clock,
    // or -1 for "no clock this session".
    void begin_pump(std::int64_t now_ms);

    // Our own input for `tick` has just gone out for the FIRST time. Starts the
    // round trip the ack-RTT closes.
    void on_local_tick(std::uint32_t tick);

    // One datagram came off the transport; `decoded` is false when decode()
    // rejected it.
    void on_datagram(bool decoded);

    // An InputRange/Input datagram attributed to `seat`. `first_tick` is the
    // peer's confirmed frontier (InputRange always starts there) — the whole
    // basis of the ack-RTT.
    void on_input_from(int seat, std::uint32_t first_tick);

    // One tick's input from `seat` that we already held.
    void on_dup_input(int seat);

    // A misprediction was found: ticks [from, head) will be replayed.
    void on_rollback(std::uint32_t from, std::uint32_t head);

    // This pump could not simulate — the prediction cap held it.
    void on_stall();

    // This pump WOULD have simulated, and chose not to, to shed clock skew.
    void on_rephase_hold();

    // The whole input-timing decision for this pump, in one call because it is one
    // decision taken at one point: the re-phase controller's raw and filtered
    // readings, whether the filter refused a hold the raw reading asked for, and
    // the local input lead in force with the arrival variance that set it. Called
    // every pump, so the overlay shows a live reading rather than a stale
    // last-decision one.
    void on_timing(int raw_advantage, int sustained, bool suppressed, int lead, int spread);

    // Called at the bottom of every pump with the session's live state.
    // `remote_next[s]` is the first tick no input is held for from seat s, which
    // is what makes `lag_ticks` exact.
    void end_pump(std::uint32_t tick, std::uint32_t confirmed,
                  const std::array<std::uint32_t, sim::kMaxPlayers>& remote_next,
                  std::uint16_t dropped, bool desynced, std::uint32_t desync_tick, bool aborted);

    const NetStats& stats() const { return s_; }

private:
    void roll_windows();  // fold the per-second counters into rates
    void push_rtt(int seat, int sample);

    NetStats s_;
    std::uint16_t remote_seats_ = 0;
    std::uint32_t start_tick_ = 0;
    std::int64_t now_ms_ = -1;
    std::int64_t begin_ms_ = -1;
    std::int64_t window_start_ms_ = -1;
    std::int64_t bucket_start_ms_ = -1;

    // tick -> first-send instant, and the tick that owns the slot.
    std::array<std::int64_t, kSendRing> sent_at_{};
    std::array<std::uint32_t, kSendRing> sent_tick_{};
    bool sent_any_ = false;

    // Highest confirmed frontier each peer has reported. A sample is only taken
    // when this RISES — otherwise every redundant re-send at an unchanged
    // frontier would be measured as a longer and longer round trip.
    std::array<std::uint32_t, sim::kMaxPlayers> peer_acked_{};
    std::array<bool, sim::kMaxPlayers> peer_acked_any_{};
    std::array<int, sim::kMaxPlayers> rtt_prev_{};      // previous sample, for jitter
    std::array<int, sim::kMaxPlayers> bucket_worst_{};  // worst sample in the open bucket

    // Per-second window counters, zeroed on every roll.
    std::array<int, sim::kMaxPlayers> win_recv_{};
    int win_rx_ = 0;
    int win_rollbacks_ = 0;
    int win_resim_ = 0;
    int win_stalls_ = 0;
};

// --- end of session ----------------------------------------------------------

// WHY THE SESSION ENDED. "It suddenly cut out" is the report this whole feature
// exists to answer, and the answer has to survive the window closing — so every
// one of these lands both on screen and in netdiag.log.
enum class SessionEndReason : std::uint8_t {
    Unknown = 0,
    MatchCompleted,  // somebody clinched; the normal end
    RoundAbandoned,  // Esc — the host scheduled an end tick and both peers stopped there
    Desync,       // the confirmed-hash exchange disagreed: the peers are simulating different games
    PeerDropped,  // a seat went silent past the hard timeout with Options row 12 OFF
    PeerLostBetweenRounds,  // the between-rounds config exchange never completed
    WindowClosed,           // the local player closed the window mid-match
    LeftSession,            // the local player walked out (Ctrl+Q / Escape at an outcome screen)
    // The double-Esc BAIL-OUT: the local player left a match that had stopped
    // responding. Deliberately its own reason rather than another LeftSession —
    // the whole point of the bail-out is that it is used when something is
    // already wrong, so a log full of these says the netcode is failing people,
    // where a log full of LeftSession says only that players leave. The note
    // carries the session's depth/stall numbers at the moment they gave up.
    LeftStalled,
};

const char* end_reason_name(SessionEndReason r);

// Everything one log line carries. `timestamp` is supplied by the CALLER so this
// whole formatter stays pure and testable — libs/net creates no wall clock.
struct SessionSummary {
    NetStats stats;
    std::string timestamp;  // e.g. "2026-07-29 14:03:11"
    std::string note;       // free-form context; may be empty
    SessionEndReason reason = SessionEndReason::Unknown;
    int round = 0;
    std::uint16_t local_seats = 0;
    std::uint16_t all_seats = 0;
    bool is_host = false;
};

// ONE LINE, key=value, newline-terminated. Deliberately flat and greppable
// rather than pretty: it is read after the fact, usually pasted into a chat, and
// it has to survive being quoted. Never throws; every field has a defined
// rendering even when nothing was ever measured.
std::string format_session_log_line(const SessionSummary& s);

}  // namespace bomber::net
