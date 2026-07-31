#pragma once

#include <array>
#include <bit>  // popcount, for the star-only host-migration gate
#include <cstddef>
#include <cstdint>
#include <unordered_map>
#include <vector>

#include "bomber/net/net_stats.hpp"  // NetStatsTracker (diagnostics; nothing the sim sees)
#include "bomber/net/time_sync.hpp"  // TimeSyncController (the pacing half; no hashed state)
#include "bomber/net/transport.hpp"
#include "bomber/sim/constants.hpp"  // kMaxPlayers
#include "bomber/sim/event.hpp"
#include "bomber/sim/simulation.hpp"
#include "bomber/sim/state.hpp"

// GGPO-style ROLLBACK netcode over an abstract Transport (ADR-0010 §3.3 step 4).
// Where LockstepSession STALLS until every seat's input has arrived, this never
// stalls the display: it PREDICTS an absent remote input (repeat the peer's last
// known one), simulates ahead speculatively, and when the real input disagrees,
// RESTORES a snapshot and RE-SIMULATES the affected ticks. Only CONFIRMED ticks
// are hashed and exchanged, so a genuine divergence is still caught loudly even
// though speculative frames legitimately differ moment to moment.
//
// Separate from LockstepSession on purpose: input-delay lockstep is the simpler
// shipped strategy, rollback the low-latency upgrade, selectable per match. Both
// share the wire codec (protocol.hpp) and the Transport seam.
//
// The evidence behind the parts that are not obvious from the code:
//   * pacing (re-phase, jitter filter, local input lead) — time_sync.hpp and
//     docs/net-rollback.md §1; `sync_` owns that half;
//   * the confirmed event stream — docs/net-rollback.md §2;
//   * host migration, and what is NOT built — design §8;
//   * the round-abandon authority model — design §10.
//
// AUTHORITY, in one line, because getting it wrong is SILENT: every
// host-authoritative decision asks hosting(), never DropPolicy::is_host. After a
// migration the old host is a corpse and the ELECTED hub carries is_host ==
// false, so a decision, its broadcast and the refusal to obey an inbound echo
// must all ask the same question or the authority lands nowhere.

namespace bomber::net {

// One decoded datagram (protocol.hpp). Forward-declared so the wire codec stays
// off the include path of every consumer of the session.
struct Message;

// What the session does when a remote seat goes SILENT for a hard window. Within
// `max_prediction` the seat is merely predicted; past the cap the session stalls;
// past this timeout it is DROPPED, and the RE'd Options row 12 "Lost net players
// revert to AI" (`dword_464928`, docs/re/results-and-options.md §row 12) decides
// which way: ON, the host broadcasts MsgType::Drop and every peer hands the seat
// to the deterministic AISystem at the same tick; OFF, `aborted()` latches and
// the caller ends the match. Never a hang.
//
// libs/net cannot see CFG.INI or the Options screen, so the CALLER copies the
// option in. Detection is local to every peer, but SCHEDULING is host-only: a
// guest never mutates hashed `State` on its own authority.
struct DropPolicy {
    bool revert_to_ai = false;  // Options row 12; see above

    // "This user pressed Host" — the SEED of the role, never the answer to "am I
    // driving the game". READING THIS FIELD FOR AN AUTHORITY DECISION IS A BUG:
    // ask hosting(). Kept separate precisely because it cannot move — it records
    // what the user asked for, not what is true now.
    bool is_host = false;

    // Pumps of TOTAL SILENCE before a seat is declared dropped; 0 (the default)
    // disables detection entirely. At 20 Hz a pump is 50 ms.
    //
    // SIZE THIS GENEROUSLY. The counter resets on any input from the seat, so a
    // peer that comes back inside the window costs nothing — but crossing it is
    // IRREVERSIBLE with row 12 off. An early value of 50 (2.5 s) killed matches
    // whenever a player merely dragged their window, because Windows blocks the
    // message pump for the whole drag. Alt-tab, a stalled disk and a laptop
    // sleeping all blow past a few seconds too: the threshold must mean
    // "genuinely gone", not "briefly busy".
    int timeout_ticks = 0;

    // The seat that is the HUB, or -1 to disable migration entirely — what every
    // pre-existing caller gets, so their behaviour is byte-identical.
    //
    // LAST IN THE STRUCT DELIBERATELY. Callers build a DropPolicy by POSITIONAL
    // aggregate init (`{revert_to_ai, is_host, timeout}`), so a field inserted
    // above `timeout_ticks` silently rebinds that third argument and turns drop
    // detection off everywhere. That is what happened when this field was first
    // added in the middle. Append here.
    int host_seat = -1;
};

class RollbackSession {
public:
    // `sim` is BORROWED and seeded identically on both peers (ADR-0010 parity).
    // `max_prediction` bounds how far the display may run ahead of the confirmed
    // frontier before advance() stalls (typical GGPO values are ~8).
    //
    // `start_tick` is the session's FIRST tick, 0 for a single-round match. A
    // MULTI-ROUND match builds one session per round over the SAME socket and the
    // wire carries no round id, so basing each round above anything the previous
    // one could have used (round_rotation.hpp) is what makes a straggling
    // datagram land below confirmed_, where the existing guards drop it.
    RollbackSession(sim::Simulation& sim, std::uint16_t local_seats, std::uint16_t all_seats,
                    int max_prediction, Transport& transport, const DropPolicy& drop = {},
                    std::uint32_t start_tick = 0);

    // Feed the local player's input and advance the DISPLAYED (speculative) state
    // by one tick, unless the cap is reached and the peer has not caught up.
    // Applies any pending rollback first. Never blocks.
    //
    // `now_ms` is a MONOTONIC WALL CLOCK that timestamps the diagnostics and
    // NEVER reaches the sim (determinism rule 1). -1 means "no clock": the
    // tick-derived statistics still work exactly, the time-derived ones do not.
    void advance(const sim::TickInputs& local_input, std::int64_t now_ms = -1);

    // The cap this session was built with; the match shell sizes its own
    // wall-clock catch-up allowance against the same bound.
    int max_prediction() const { return max_prediction_; }

    std::uint32_t predicted_tick() const { return tick_; }       // next speculative tick
    std::uint32_t confirmed_tick() const { return confirmed_; }  // highest all-inputs-known tick
    const sim::Simulation& sim() const { return *sim_; }
    std::uint64_t hash() const { return sim_->hash(); }

    bool desynced() const { return desynced_; }
    std::uint32_t desync_tick() const { return desync_tick_; }

    // --- the confirmed event stream (docs/net-rollback.md §2) -----------------

    // APPEND, in tick order, the `State::events` of every tick that has become
    // CONFIRMED since the last drain, and forget them. THE ONLY SAFE INPUT for
    // anything that sums per-tick events across a match: a confirmed tick is
    // never re-simulated, so each tick's events are handed out exactly once.
    //
    // Drain every pump — the buffer is otherwise unbounded.
    void drain_confirmed_events(std::vector<sim::Event>& out);

    // drain_confirmed_events() PLUS every tick simulated but not yet confirmed.
    // Call it ONCE, at the moment the round stops, so two peers whose frontiers
    // sit at different ticks still close the round having covered the IDENTICAL
    // tick range. Advancing the session after this would double-count the tail.
    void drain_remaining_events(std::vector<sim::Event>& out);

    // The live diagnostic snapshot (net_stats.hpp). Read-only and side-effect
    // free; the session behaves identically whether anyone calls it or not.
    const NetStats& stats() const { return stats_.stats(); }

    // Seats declared dropped so far (0 while everyone is live).
    std::uint16_t dropped_seats() const { return dropped_; }
    // The agreed handoff tick, or kNoHandoff for a still-live peer. Every peer
    // holds the same value.
    std::uint32_t handoff_tick(int seat) const {
        return handoff_[static_cast<std::size_t>(seat)];
    }
    // LOUD match-ending state: a peer dropped while Options row 12 was OFF, so
    // there is no legal way to keep simulating. advance() is a no-op from here
    // on and the caller polls this — it never hangs.
    bool aborted() const { return aborted_; }

    // --- round abandon (Esc), wire v8 ----------------------------------------

    // "Stop this round." Only on the machine that is hosting(); a NO-OP anywhere
    // else, because a guest has no say in what both machines simulate.
    // Idempotent, so a caller may offer Esc unconditionally and let the session
    // decide — which is what MatchRunner does.
    void request_end_round();

    // An end tick is agreed (on either peer). The shell reads this to tell an
    // ABANDONED round — a DRAW by decree, tallying nothing — from one that ended
    // on its own terms.
    bool end_round_scheduled() const { return end_tick_ != kNoEndRound; }
    std::uint32_t end_round_tick() const { return end_tick_; }  // first tick NOBODY simulates
    // Stop the round loop. advance() still receives and re-sends from here on, so
    // a peer that has not caught up still can.
    bool round_ended() const { return end_tick_ != kNoEndRound && tick_ >= end_tick_; }

    // --- host migration (wire v9, design §8) ---------------------------------

    int current_hub() const { return hub_; }  // the elected hub, or -1 when off
    // "This machine is driving the game." Equal to DropPolicy::is_host while
    // migration is off; recomputed from the election once it is on.
    bool hosting() const;
    // Seats recorded as a LOST HUB rather than an ordinary dropped guest —
    // the signal a caller would poll to start rewiring the star. No such caller
    // is built (design §8.3), so today only the tests read it.
    std::uint16_t host_lost_seats() const { return host_lost_; }

    // THE MIGRATION STALL. While held, advance() receives and re-sends but
    // simulates nothing and runs no drop detection, so the confirmed frontier —
    // and the floor of the retained rewind window with it — stays where the hub's
    // death left it.
    //
    // Releasing also re-arms every silence counter: by then every remote seat is
    // past the drop timeout, because that silence is HOW the loss was detected,
    // so a naive release would declare the whole surviving table dropped one pump
    // later. (Belt-and-braces rather than the mechanism the suite proves; §3.)
    void set_migration_hold(bool held);
    bool migration_held() const { return migration_hold_; }

    static constexpr std::uint32_t kNoHandoff = 0xFFFFFFFFU;
    static constexpr std::uint32_t kNoEndRound = 0xFFFFFFFFU;

private:
    struct Slot {
        sim::TickInputs inputs;       // seat inputs used to simulate this tick
        std::uint16_t confirmed = 0;  // which seats are CONFIRMED (rest are predicted)
    };

    // The whole of advance() except the diagnostics bracket, so the tracker's
    // begin/end pair is written ONCE and cannot be missed on an early return.
    void advance_impl(const sim::TickInputs& local_input);
    // Diagnostics only. `first_tick` is the sender's confirmed frontier (the
    // ack-RTT's basis), or 0 for a frame that carries none.
    void note_input_seats(std::uint16_t seats, std::uint32_t first_tick);

    void receive();                     // drain transport -> slots + peer hashes
    void on_message(const Message& m);  // ONE decoded datagram, dispatched
    void on_input_range(const Message& m);
    void on_single_input(const Message& m);
    void on_drop(const Message& m);
    void on_host_lost(const Message& m);
    void on_match_ctl(const Message& m);

    void apply_remote(std::uint32_t tick, std::uint16_t seats, const sim::TickInputs& in);
    void count_duplicate_input(std::uint16_t seats);
    // The EARLIEST tick wins, so several corrections in one pump collapse into
    // one replay.
    void request_rollback(std::uint32_t tick);
    sim::TickInputs assemble(std::uint32_t tick);  // confirmed seats + predicted remote seats
    void resimulate(std::uint32_t from);           // restore snapshot[from], replay to tick_
    void advance_confirmed();                      // raise confirmed_ over the all-known prefix
    std::uint64_t hash_after(std::uint32_t tick) const;
    void emit_confirmed_events(std::uint32_t tick);
    void exchange_confirmed_hash(std::uint32_t tick, std::uint64_t h);
    void send_local(std::uint32_t from);  // redundant range of recent local inputs
    void note_peer_hash(std::uint32_t tick, std::uint64_t peer_hash);
    void prune();

    // May this pump advance the simulation? Asks, strongest first: the migration
    // stall, the agreed round end, the re-phase hold, the prediction cap. Takes
    // the pacing decision on the way, which is why it is not const.
    bool simulation_allowed(bool eligible);
    void simulate_one_tick(const sim::TickInputs& local_input);

    // The events produced BY simulating `tick`, or nullptr if it is unsimulated
    // or pruned. Read off the snapshot taken before `tick + 1` — the same lookup
    // advance_confirmed() takes the hash from, which is what ties the two.
    const std::vector<sim::Event>* events_of(std::uint32_t tick) const;

    // Is a re-phase decision even meaningful right now? Separated from the
    // advantage test so raw and filtered readings meet the same preconditions,
    // and so the local lead inherits every one of them.
    bool rephase_eligible() const;
    // File the local sample up to the lead's head, NEVER rewriting a filed tick
    // (docs/net-rollback.md §1.4).
    void file_local(const sim::TickInputs& local_input);

    // Seats whose input is still EXCHANGED at `tick`: the single tick-keyed
    // predicate behind both "what does the sim get fed" and "whose input must
    // still arrive".
    std::uint16_t seats_awaited(std::uint32_t tick) const;
    std::uint16_t awaited_remote_seats() const;  // ...of those, the remote ones, at tick_
    // Re-assert the drop schedule on the hashed State before simulating `tick`.
    // IDEMPOTENT, and run on the first pass AND every re-simulation — that is
    // what makes a handoff survive a rollback to an earlier snapshot.
    void apply_handoffs(std::uint32_t tick);
    void heard(std::uint16_t seats);  // a packet carrying these seats arrived
    // Order-free and idempotent: the EARLIEST tick wins, so a duplicate, a
    // re-send and an out-of-order copy all reduce to a no-op.
    void schedule_handoff(int seat, std::uint32_t at_tick);
    // Hard-timeout scan: the host schedules, but any peer can abort.
    void detect_drops();
    // ADVANCES the seat's silence counter, so it runs once per seat per pump.
    bool silence_exceeded(int seat);
    // The three outcomes detect_drops() can reach for one silent seat.
    void abort_on_drop(int seat);       // Options row 12 OFF: the match is over
    void announce_host_lost(int seat);  // the HUB's own seat went silent
    void announce_drop(int seat);       // an ordinary guest drop, by the elected hub

    // Host-side redundant re-sends, on send_local()'s reasoning.
    void broadcast_handoffs();
    void broadcast_end_round();
    // Earliest tick wins, exactly as schedule_handoff does.
    void schedule_end_round(std::uint32_t at_tick);

    // --- host migration internals (design §8.1/§8.2) --------------------------

    // Migration arms ONLY for a star, and the two-seat exclusion is a SAFETY GATE
    // rather than a missing feature: with two seats the data plane cannot tell a
    // dead peer from a dead path, so electing on that guess makes each side hand
    // the OTHER's seat to the AI and play on inside a private divergent game
    // (design §8.1). Pinned in tests/net/test_host_migration.cpp.
    bool migration_enabled() const {
        return drop_.host_seat >= 0 && std::popcount(all_seats_) > 2;
    }
    // Decided but not yet paid off. Gates the wide re-send, the re-phase
    // suppression, and — correctness rather than tuning — the refusal to declare
    // a SECOND host loss (detect_drops).
    bool migration_healing() const { return host_lost_ != 0 && confirmed_ < heal_until_; }
    int hub_of(std::uint16_t live) const;  // THE ELECTION: pure, no vote, no coordinator
    // The survivor set the election runs over, derived from the SCHEDULE and not
    // from hashed State — a player who has been blown up still runs a machine —
    // which is what keeps it identical on every peer and rollback-proof.
    std::uint16_t live_seats() const;
    std::uint32_t resend_from() const;  // normally confirmed_; widened while healing
    // Un-confirm back to `at_tick` and purge every hash at or above it on BOTH
    // sides. False if `at_tick` is older than the retained window. THE ONE PLACE
    // THIS CLASS MOVES ITS FRONTIER BACKWARDS.
    bool rewind_for_migration(std::uint32_t at_tick);
    void adopt_host_lost(int seat, std::uint32_t at_tick);  // lowest tick wins
    // NOT hub-only, unlike every other broadcast here: the announcement is ABOUT
    // the hub, so the machine the redundancy discipline leans on is the dead one.
    void broadcast_host_lost();

    sim::Simulation* sim_;  // BORROWED
    Transport* transport_;
    std::uint16_t local_seats_;
    std::uint16_t all_seats_;
    std::uint16_t remote_seats_;
    int max_prediction_;
    DropPolicy drop_;

    std::uint32_t tick_ = 0;       // next speculative tick (starts at start_tick)
    std::uint32_t confirmed_ = 0;  // highest tick whose inputs are ALL confirmed
    std::uint32_t rollback_to_ = 0;
    bool rollback_pending_ = false;

    std::unordered_map<std::uint32_t, Slot> slots_;            // per-tick inputs
    std::unordered_map<std::uint32_t, sim::State> snapshots_;  // State BEFORE that tick
    // Prediction source: each seat's most recent CONFIRMED input and the tick it
    // came from, so a later confirmation wins.
    sim::TickInputs last_remote_input_;
    std::array<std::uint32_t, sim::kMaxPlayers> last_remote_tick_{};
    std::unordered_map<std::uint32_t, std::uint64_t> hash_;  // ours, per CONFIRMED tick
    std::unordered_map<std::uint32_t, std::uint64_t> peer_hash_;
    // Confirmed-but-not-yet-drained events, in tick order. Filled ONLY by
    // advance_confirmed(), the same place the confirmed hash is taken, so the two
    // can never disagree about which ticks are final.
    std::vector<sim::Event> confirmed_events_;
    // High-water mark of ticks already in that stream. `confirmed_` cannot serve,
    // because migration moves it backwards; this only rises (§2.2).
    std::uint32_t events_through_ = 0;

    // The peer-drop SCHEDULE: durable, tick-keyed, rollback-proof. Deliberately
    // NOT a one-shot mutation of State — `Player::ai` is hashed, so restoring a
    // pre-handoff snapshot would silently erase a flag set once.
    std::array<std::uint32_t, sim::kMaxPlayers> handoff_;  // kNoHandoff = live peer
    // Pumps since anything at all was heard carrying that seat. LIVENESS, not
    // input progress: a peer stalled behind a THIRD peer's silence still sends
    // its window every pump and must not be accused of dropping.
    std::array<int, sim::kMaxPlayers> silence_{};
    // First tick we hold NO input for from a seat; the host uses it as the
    // handoff tick (see DropFrame on why that is retroactive).
    std::array<std::uint32_t, sim::kMaxPlayers> remote_next_{};
    std::uint16_t dropped_ = 0;
    bool aborted_ = false;

    // The agreed round-abandon tick. OUTSIDE sim::State, like the handoff
    // schedule, because it must survive a rollback untouched — and unlike it,
    // never reaching the sim at all.
    std::uint32_t end_tick_ = kNoEndRound;

    // --- host migration state (all outside sim::State; none of it is hashed) --

    int hub_ = -1;                 // the elected hub; -1 while migration is off
    std::uint16_t host_lost_ = 0;  // seats recorded as a LOST HUB
    // Where the WIDE re-send may stop: a full rewind window past the migration
    // tick. Raised, never lowered, so a second host loss extends it.
    std::uint32_t heal_until_ = 0;
    // The oldest tick still retained, and it only ever RISES — prune() keeps a
    // window below confirmed_ so rewind_for_migration() can un-confirm into it.
    std::uint32_t oldest_slot_ = 0;
    bool migration_hold_ = false;  // the caller-driven stall across the rewire

    // THE PACING HALF (time_sync.hpp). A private COLLABORATOR rather than a base
    // class: it is asked questions and never asks any, so nothing in it can reach
    // the hashed state above it.
    TimeSyncController sync_;

    // The tick our next local sample will be filed against. MONOTONE — the one
    // invariant that makes moving the lead mid-match safe.
    std::uint32_t local_next_ = 0;

    bool desynced_ = false;
    std::uint32_t desync_tick_ = 0;

    // INSTRUMENTATION ONLY, consulted by no decision here: a build that never
    // looks at it plays exactly the same match.
    NetStatsTracker stats_;
};

}  // namespace bomber::net
