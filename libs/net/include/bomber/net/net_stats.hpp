#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

#include "bomber/net/net_path.hpp"
#include "bomber/sim/constants.hpp"  // sim::kMaxPlayers

// IN-MATCH NETPLAY INSTRUMENTATION — the numbers behind the overlay and behind
// the line a dead session leaves in netdiag.log.
//
// Every number is derived from traffic that ALREADY FLOWS (no new wire message,
// because a kWireProtocolVersion bump invalidates every build in the wild), and
// nothing here allocates on a packet path, so the counting cannot perturb what
// it counts. The derivations, and the one place a reading stops meaning what its
// name says (`rtt_offset_bound`), are in docs/net-rollback.md §4.
//
// Clock-INJECTED, so this is unit-testable headless. Nothing here ever reaches
// libs/sim, so no golden hash can move (determinism rule 1).

namespace bomber::net {

// The sim's fixed tick rate, and so also the EXPECTED inbound datagram rate per
// remote seat — which is what makes the loss estimate possible with no sequence
// number.
inline constexpr int kPumpHz = 20;

// tick -> "when did our input first leave" ring. 512 ticks is ~25 s, far longer
// than any RTT worth reporting, so a slot is never overwritten before its
// acknowledgement could arrive; the tick is stored alongside so a wrapped slot
// is detected rather than mistaken for a hit.
inline constexpr std::size_t kSendRing = 512;

// The RTT sparkline: 48 buckets x 100 ms = 4.8 s, each keeping its bucket's
// WORST sample so decimation cannot hide a spike.
inline constexpr std::size_t kRttHistory = 48;
inline constexpr std::int64_t kRttBucketMs = 100;

// Rates are a plain COMPLETED-WINDOW count rather than a moving average, so the
// number on screen is a fact about the last second and not a blend.
inline constexpr std::int64_t kRateWindowMs = 1000;

// Above this a "sample" can only be a wrapped ring slot or a clock that jumped.
inline constexpr int kRttSaneMaxMs = 10000;

// Per REMOTE seat. EXACT unless marked ESTIMATED — the two estimates are called
// out because the owner reads these as fact.
struct PeerStats {
    bool tracked = false;  // a network peer whose input we exchange
    // False once the seat is the AI's. A dropped seat legitimately stops
    // producing traffic, and this is what stops that reading as loss.
    bool live = true;

    // How many ticks the newest input from this seat trails our own speculative
    // head — the most honest "late arrival" number this architecture has. There
    // is deliberately no "arrived after its tick was needed" counter: with zero
    // input delay EVERY remote input does, which is what rollback IS.
    int lag_ticks = 0;
    int worst_lag_ticks = 0;

    // ESTIMATED — an UPPER BOUND on the true round trip, never below it. Read
    // `rtt_min_ms` as the best estimate of the path and `rtt_ms` / the sparkline
    // as the thing that spikes, BUT ONLY WHILE `rtt_offset_bound` is clear
    // (docs/net-rollback.md §4).
    int rtt_ms = -1;             // most recent sample; -1 until one exists
    int rtt_smooth_ms = -1;      // EWMA (1/4 weight) — the steady reading
    int rtt_min_ms = -1;         // best this session: the tightest bound on the path
    int rtt_max_ms = -1;         // worst this session
    int rtt_recent_max_ms = -1;  // worst in the sparkline window: "is it spiking NOW"
    int jitter_ms = 0;           // EWMA of |sample - previous sample|

    // The readings above are USELESS AS A PATH MEASUREMENT while this is set: the
    // peers have settled into a wall-clock phase offset and the peer that is
    // ahead is measuring that rather than the wire. There is no correction, only
    // the flag (§4.2); read `lag_ticks` instead.
    bool rtt_offset_bound = false;

    // A large `dup_inputs` is EXPECTED and not a fault: every peer re-sends its
    // whole unconfirmed window every pump, which is how a lost datagram heals
    // itself. A liveness check, not a loss signal.
    std::uint32_t input_packets = 0;
    std::uint32_t dup_inputs = 0;

    int recv_per_sec = 0;  // datagrams over the last completed second
    // ESTIMATED. A peer sends one datagram per pump, so a shortfall is either
    // path loss or the PEER'S OWN LOOP stalling, and nothing on the wire can tell
    // those apart without a sequence number. Read it as "the peer's voice is
    // thinner than it should be".
    int loss_pct_est = 0;

    // Newest LAST, `rtt_history_len` valid. One entry per kRttBucketMs holding
    // that bucket's WORST sample, so a spike survives decimation.
    std::array<std::uint16_t, kRttHistory> rtt_history{};
    std::size_t rtt_history_len = 0;
};

// The whole live picture. A plain aggregate so the overlay, the log formatter
// and the tests all read the same snapshot. Every field is EXACT.
struct NetStats {
    NetPath path = NetPath::Unknown;  // straight off the Transport carrying the bytes

    // The speculative head, the confirmed frontier, and the distance between them
    // — which IS both "how deep predictions are running" and "how far the
    // confirmed frontier is behind the local head": one number, two names.
    std::uint32_t tick = 0;
    std::uint32_t confirmed = 0;
    int prediction_depth = 0;
    int worst_prediction_depth = 0;
    int max_prediction = 0;  // the cap the session was built with, for context

    // Pumps on which the cap held the display instead of simulating. THIS is what
    // a netcode stutter actually is. If this is zero, a reported stutter is not
    // the netcode.
    std::uint32_t stall_pumps = 0;
    int stalls_per_sec = 0;

    // Pumps this peer held DELIBERATELY, to give back clock skew. Distinct from a
    // stall on purpose: a stall is the cap refusing to let us run, a re-phase
    // hold is us choosing not to.
    //
    // NOTE FOR READING A LOG: a re-phase hold returns BEFORE the cap check, so a
    // pump spent holding is never also counted as a stall. `stalls=0` on a line
    // whose `rephase` is large does NOT mean the cap was out of the picture — add
    // the two to get "pumps that could not advance the display".
    std::uint32_t rephase_holds = 0;

    // --- the arrival-variance filter (docs/net-rollback.md §1.3) --------------

    // The raw per-pump comparison the re-phase controller is built on, and the
    // part of it that held for the WHOLE window. On a clean path they are the
    // same number every pump; the GAP is arrival variance, made visible.
    int frame_advantage = 0;
    int sustained_advantage = 0;
    // Pumps where the RAW reading asked for a hold and the filter refused —
    // displayed ticks jitter used to cost. Zero on a clean path by construction,
    // and the one number saying how much the absorber is doing.
    std::uint32_t rephase_suppressed = 0;

    // The LOCAL INPUT LEAD in ticks — the only thing here the player PAYS for, at
    // 50 ms each, so it is shown rather than inferred, and 0 wherever it is not
    // needed.
    int local_lead = 0;
    std::uint32_t lead_pumps = 0;  // pumps spent with a lead in force
    // The variance that set it: the spread of the peer's OWN prediction depth. A
    // steady path reads 0 however SLOW it is, which stops a merely-distant peer
    // being charged input lag.
    int peer_depth_spread = 0;

    // A rollback is one mispredicted remote input; `resim_ticks` is the total
    // replayed. Many re-sims against few rollbacks means corrections land deep.
    std::uint32_t rollbacks = 0;
    std::uint32_t resim_ticks = 0;
    int rollbacks_per_sec = 0;
    int resim_ticks_per_sec = 0;

    // A non-zero `rx_malformed` means something on the path is corrupting or
    // injecting, which no other number here would reveal.
    std::uint32_t rx_packets = 0;
    std::uint32_t rx_malformed = 0;
    int rx_per_sec = 0;

    // The session's own loud states, mirrored so the log line and the overlay
    // read one struct.
    bool desynced = false;
    std::uint32_t desync_tick = 0;
    bool aborted = false;
    std::uint16_t dropped_seats = 0;

    // True once a real clock has been supplied. Without it every time-derived
    // field stays at its "unavailable" value rather than reading as zero.
    bool clocked = false;
    std::int64_t elapsed_ms = 0;

    std::array<PeerStats, sim::kMaxPlayers> peers{};
};

// One pump's whole input-timing decision, reported in one call because it is one
// decision taken at one point (TimeSyncController::Decision plus the lead in
// force).
struct TimingSample {
    int raw_advantage = 0;
    int sustained = 0;
    int spread = 0;
    int lead = 0;
    bool suppressed = false;
};

// The session state a pump ends in. A parameter object rather than seven
// arguments (coding-standards §3): every field is session state that already
// travels together, and a positional list this long is the shape that lets a
// caller transpose two of them unnoticed.
struct PumpSnapshot {
    std::uint32_t tick = 0;
    std::uint32_t confirmed = 0;
    // First tick no input is held for from each seat — what makes `lag_ticks`
    // exact rather than inferred.
    std::array<std::uint32_t, sim::kMaxPlayers> remote_next{};
    std::uint16_t dropped = 0;
    bool desynced = false;
    std::uint32_t desync_tick = 0;
    bool aborted = false;
};

// The accumulator. Fed by RollbackSession at a handful of points; owns no heap.
//
// Every method is safe to call without a clock: pass -1 for `now_ms` (the default
// everywhere) and the tick-derived statistics still work exactly, while the
// time-derived ones stay unavailable.
class NetStatsTracker {
public:
    // Once, when the session is built.
    void begin(NetPath path, std::uint16_t remote_seats, std::uint32_t start_tick,
               int max_prediction);
    // Top of every pump. `now_ms` is the caller's monotonic clock, or -1.
    void begin_pump(std::int64_t now_ms);
    // Our input for `tick` has gone out for the FIRST time: starts the round trip
    // the ack-RTT closes.
    void on_local_tick(std::uint32_t tick);
    void on_datagram(bool decoded);  // `decoded` false when decode() rejected it
    // `first_tick` is the peer's confirmed frontier — the whole basis of the
    // ack-RTT — or 0 for a frame that carries none.
    void on_input_from(int seat, std::uint32_t first_tick);
    void on_dup_input(int seat);  // one tick's input from `seat` that we held already
    void on_rollback(std::uint32_t from, std::uint32_t head);  // [from, head) will be replayed
    void on_stall();         // this pump could not simulate: the cap held it
    void on_rephase_hold();  // this pump chose not to, to shed clock skew
    // Called EVERY pump, so the overlay shows a live reading rather than a stale
    // last-decision one.
    void on_timing(const TimingSample& t);

    void end_pump(const PumpSnapshot& s);  // bottom of every pump

    const NetStats& stats() const { return s_; }

private:
    void roll_windows();  // fold the per-second counters into rates
    void push_rtt(int seat, int sample);
    void close_rtt_bucket();
    void update_peer(int seat, std::uint32_t tick, std::uint32_t next, bool live);

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
    // responding. Its own reason rather than another LeftSession, because a log
    // full of these says the netcode is failing people where a log full of
    // LeftSession says only that players leave.
    LeftStalled,
};

const char* end_reason_name(SessionEndReason r);

// Everything one log line carries. `timestamp` is supplied by the CALLER so this
// formatter stays pure and testable — libs/net creates no wall clock.
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

// ONE LINE, key=value, newline-terminated. Deliberately flat and greppable rather
// than pretty: it is read after the fact, usually pasted into a chat, and it has
// to survive being quoted. Never throws.
std::string format_session_log_line(const SessionSummary& s);

}  // namespace bomber::net
