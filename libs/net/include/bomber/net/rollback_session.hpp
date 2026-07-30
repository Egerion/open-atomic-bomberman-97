#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <unordered_map>
#include <vector>

#include "bomber/net/net_stats.hpp"  // NetStatsTracker (diagnostics; nothing the sim sees)
#include "bomber/net/transport.hpp"
#include "bomber/sim/constants.hpp"  // kMaxPlayers
#include "bomber/sim/event.hpp"
#include "bomber/sim/simulation.hpp"
#include "bomber/sim/state.hpp"

// GGPO-style ROLLBACK netcode over an abstract Transport (ADR-0010 §3.3 step 4).
// Where LockstepSession STALLS until every seat's input for a tick has arrived,
// RollbackSession never stalls the display: it PREDICTS an absent remote input
// (repeat the peer's last known input), simulates ahead speculatively, and when
// the real input arrives and disagrees, RESTORES a snapshot and RE-SIMULATES the
// affected ticks with the corrected input. This hides latency — the local player
// sees no input delay — at the cost of an occasional visible correction.
//
// The port has exactly the two things rollback usually makes hard, for free:
//   * cheap full-state snapshots — `State` is a plain value type, so save is
//     `State s = sim.state()` and restore is `sim.state() = s`; and
//   * a fast pure re-sim — `Simulation::tick(inputs)` is a deterministic pure
//     function of (State, inputs).
// `state_hash` still guards correctness: CONFIRMED ticks (no prediction) are
// exchanged and compared, so a genuine divergence is caught loudly even though
// speculative frames legitimately differ between peers moment to moment.
//
// This is a SEPARATE class from LockstepSession on purpose: input-delay lockstep
// is the simpler, already-shipped strategy; rollback is the low-latency upgrade,
// selectable per match without disturbing the other. Both share the wire codec
// (protocol.hpp) and the Transport seam.
//
// PEER DROP -> AI HANDOFF (ADR-0011 Risks, "Dropped/late peers"). Prediction
// covers a briefly late peer and the `max_prediction` cap covers a very late
// one, but a peer that never comes back would stall everyone forever. Past a
// hard silence timeout the host announces MsgType::Drop and every peer, at the
// SAME tick, sets `Player::ai` for that seat: the deterministic AISystem then
// derives its input from the shared `State`, identically everywhere, and the
// confirmed-hash exchange keeps agreeing. See DropPolicy below (gated on the
// RE'd Options row 12) and the `handoff_` schedule at the bottom of the class
// (the rollback-safe part).
//
// RE-PHASING (the standing-lag cure; no wire change). MEASURED 2026-07-30 with a
// two-peer harness on INDEPENDENT wall clocks (tests/net/test_rollback_pacing
// .cpp): any freeze of a peer's frame loop longer than MatchRunner's 200 ms
// catch-up clamp has its excess wall time DISCARDED, so that peer falls
// permanently behind its partner — a 400 ms window drag costs a standing 4 ticks
// and nothing ever gives them back. The prediction cap was the only re-phasing
// mechanism there was, and it only engages once the WHOLE budget is spent: the
// peer that is ahead stalls at 8/8 for the rest of the round, so every ordinary
// packet-timing wobble becomes a visible stutter. A live Turkey<->Lithuania
// record showed exactly that end state — depth=8/8, lag=lag_max=8, rollbacks=0
// (the peer was never WRONG, only late) on a ~100 ms path where the healthy
// depth is 2.
//
// The cure is GGPO's frame-advantage time-sync, and the numbers it needs are
// already on the wire. An InputRange spans [sender's confirmed, sender's head),
// so its LENGTH is the sender's own prediction depth — i.e. the peer's own lag,
// measured on the peer. Ours minus theirs cancels the path delay (which both
// contain equally) and leaves twice the CLOCK SKEW, which is the part that should
// not be there. Past a small threshold the peer that is ahead holds one tick per
// pump until the skew is gone, so depth returns to the path baseline instead of
// parking at the cap. Purely a decision about WHEN this peer simulates — never
// about WHAT it simulates — so no hashed state and no golden can move.

// ROUND ABANDON -> DRAW (wire v8, MatchCtlKind::EndRound). Esc during an online
// round is a LOCAL keypress and therefore must not be a local ACT: a peer that
// stopped its own sim would have simulated — and tallied — a different number of
// ticks than its partner, which is the same divergence class the drop handoff
// exists to avoid. So it routes through request_end_round(): an end tick is
// scheduled and broadcast, every peer stops at exactly that tick, and the round
// is a DRAW BY DECREE (nobody inspects the frozen state for a winner), so the
// match shell above can replay a round instead of tearing the connection down.
//
// STOPPING THE MATCH IS THE HOST'S ALONE, and this NARROWED on 2026-07-30.
// request_end_round() used to have a guest branch: a guest sent
// MatchCtlKind::EndRoundRequest and the host converted it into the decision. That
// made any guest able to force-end any round at will with no host confirmation —
// a griefing lever, and the owner independently asked for the authority model
// that removes it. So a guest's request_end_round() is now a no-op, and a host
// IGNORES an inbound EndRoundRequest. The second half is the one that matters:
// kWireProtocolVersion is unchanged at 8, so a peer running the previous build
// still connects, and refusing the message is what stops it still driving us.
// The kind stays in the enum and decode() still accepts it precisely so that
// older peer is turned away rather than disconnected.
//
// LEAVING is a different act and is NOT host-only — it is not in this class at
// all. Stopping changes what BOTH machines simulate and so needs one authority;
// leaving only removes yourself, must never depend on a peer answering, and is
// the match shell's own business (MatchRunner's double-Esc bail-out, which tears
// the transport down locally and never sends anything).
//
// THE CONFIRMED EVENT STREAM (drain_confirmed_events, added 2026-07-30). Rollback
// makes `State::events` a stream that REPLAYS: a tick simulated speculatively and
// then corrected produces its events twice, and the two passes need not agree.
// Anything that ACCUMULATES those events across ticks therefore accumulates a
// per-machine number — and because events are excluded from state_hash by design
// (determinism rule 4), the per-tick desync check, the goldens and `build_hash`
// are all blind to it. That is not hypothetical: the front-end's per-match kill
// tally (libs/game's results.hpp `tally_kills`) was fed straight from
// `sim.state().events` once per pump, so two peers with different rollback
// histories reached different kill totals from an IDENTICAL, agreed simulation —
// and with Team Play + "win by kills" that is a different match VERDICT on the
// two machines.
//
// The cure is to hand such a consumer only the events of CONFIRMED ticks. A
// confirmed tick's inputs can never change again, so it is simulated exactly once
// more than the goldens are: whatever the speculative passes did, the events
// emitted here are the ones the final, agreed history produced. They need no
// storage of their own — `snapshots_[t+1]` is the State AFTER tick t and carries
// its `events`, which is the SAME lookup advance_confirmed() already does for the
// hash. So the stream a consumer sums is, tick for tick, the stream whose hash
// both peers have compared and agreed on: an accumulator fed from it is guarded
// by the desync check that could not see the old one.
//
// The residual is the SPECULATIVE TAIL — the at-most-`max_prediction` ticks
// between confirmed_ and the head at the moment the round stops. Both peers stop
// at the same TICK, so drain_remaining_events() lets the shell close the range on
// both machines identically; the CONTENT of those last few ticks is the one part
// still taken on trust. It is bounded (400 ms at the default cap of 8) and, on
// the ordinary round-end path, empty of kills — the shell lingers 3 s (60 ticks)
// after the last side falls before it reads the tally, which is far longer than
// the confirmation frontier ever trails.

namespace bomber::net {

// What the session does when a remote seat goes SILENT for a hard window
// (ADR-0011 Risks, "Dropped/late peers"). Within `max_prediction` the seat is
// merely predicted; past the cap the session stalls; past this timeout it is
// declared DROPPED and one of two things happens, decided by the RE'd Options
// row 12 "Lost net players revert to AI" (`dword_464928`,
// docs/re/results-and-options.md §row 12) that `revert_to_ai` carries:
//
//   ON  — the host broadcasts MsgType::Drop and EVERY peer hands the seat to
//         the deterministic AISystem at the same tick; the match plays on.
//   OFF — the drop is a match-ending condition: `aborted()` latches, advance()
//         becomes a no-op, and the caller ends the match. Never a hang.
//
// The option lives in CFG.INI (`assets::Options::lost_net_revert_ai`) and
// on the Options screen; libs/net cannot see either (it depends on bomber::sim
// only), so the CALLER copies it in here. Detection is local to every peer, but
// SCHEDULING is host-only: a guest must never mutate the hashed `State` on its
// own authority, it only obeys the host's Drop message.
struct DropPolicy {
    bool revert_to_ai = false;  // Options row 12; see above
    // Only the hub/host schedules + broadcasts a handoff — and, since wire v8,
    // the round-end abandon (request_end_round). It is the session's one bit of
    // "am I the machine driving the game", so both host-authoritative decisions
    // read it rather than carrying two copies of the same flag.
    bool is_host = false;
    // Pumps of TOTAL SILENCE from a seat before it is declared dropped. 0 (the
    // default) disables drop detection entirely, so existing callers and every
    // pre-existing scenario behave exactly as before. At 20 Hz a pump is 50 ms.
    //
    // Size this GENEROUSLY. The counter resets the moment any input from the
    // seat arrives, so a peer that comes back inside the window costs nothing —
    // but crossing it is IRREVERSIBLE with Options row 12 off (`aborted_`
    // latches and the match is over). An early value of 50 (2.5 s) killed
    // matches whenever a player merely dragged their window: Windows blocks the
    // message pump for the whole drag, the peer sees silence, and the match died
    // with no way back. Alt-tab, a stalled disk, a laptop sleeping for a moment
    // and ordinary network hiccups all blow past a few seconds too, so the
    // threshold must mean "genuinely gone", not "briefly busy".
    int timeout_ticks = 0;
};

class RollbackSession {
public:
    // `sim` is BORROWED and seeded identically on both peers (ADR-0010 parity).
    // `local_seats` / `all_seats` as in LockstepSession (AI seats are simulated,
    // not exchanged). `max_prediction` bounds how many un-confirmed ticks the
    // display may run ahead before it must wait (a safety cap; typical GGPO
    // values are ~8): past it advance() stalls rather than predict unboundedly.
    // `drop` defaults to "detection off" — additive, so nothing changes for a
    // caller that does not opt in.
    //
    // `start_tick` is the session's FIRST tick number, 0 for a single-round match
    // (every pre-existing caller and test). A MULTI-ROUND match builds one session
    // per round over the SAME socket, and the wire carries no round id: a
    // straggling INPUT/HASH datagram from round N would land inside round N+1's
    // tick space and be filed as a future input (apply_remote only rejects ticks
    // BELOW confirmed_) or compared as a peer hash — a stale-input bug or a
    // phantom desync. Giving each round a base above anything the previous one
    // could have used (round_rotation.hpp's round_tick_base) makes those stale
    // ticks unconditionally < confirmed_, so the existing guards drop them.
    // Purely a numbering offset: every tick comparison in this class is relative.
    RollbackSession(sim::Simulation& sim, std::uint16_t local_seats, std::uint16_t all_seats,
                    int max_prediction, Transport& transport, const DropPolicy& drop = {},
                    std::uint32_t start_tick = 0);

    // Feed the local player's input for the next frame and advance the DISPLAYED
    // (speculative) state by one tick, unless the prediction cap is reached and
    // the peer has not caught up (then it holds). Applies any pending rollback
    // first, so on return sim() reflects the best current estimate. Never blocks.
    //
    // `now_ms` is a MONOTONIC WALL CLOCK, and it exists only to timestamp the
    // diagnostics (net_stats.hpp): RTT, jitter and the per-second rates cannot be
    // derived from tick numbers alone. It NEVER reaches the sim — no branch below
    // reads it, `Simulation::tick` is not handed it, and nothing hashed depends
    // on it (determinism rule 1). The default of -1 means "no clock": every
    // tick-derived statistic still works exactly and the time-derived ones stay
    // at their unavailable values, which is what keeps every existing headless
    // caller and test byte-identical.
    void advance(const sim::TickInputs& local_input, std::int64_t now_ms = -1);

    // The cap this session was built with. The match shell reads it to size its
    // own wall-clock catch-up allowance against the same bound.
    int max_prediction() const { return max_prediction_; }

    std::uint32_t predicted_tick() const { return tick_; }   // next tick to simulate speculatively
    std::uint32_t confirmed_tick() const { return confirmed_; }  // highest all-inputs-known tick
    const sim::Simulation& sim() const { return *sim_; }
    std::uint64_t hash() const { return sim_->hash(); }

    bool desynced() const { return desynced_; }
    std::uint32_t desync_tick() const { return desync_tick_; }

    // --- the confirmed event stream (see the note at the top of this file) ----

    // APPEND, in tick order, the `State::events` of every tick that has become
    // CONFIRMED since the last drain, and forget them. This is the ONLY safe
    // input for anything that sums per-tick events across a match: a confirmed
    // tick is never re-simulated, so each tick's events are handed out exactly
    // once and are the ones the agreed history produced.
    //
    // The caller is expected to drain every pump — the buffer is otherwise
    // unbounded (it is a few events per tick, so a whole round is on the order of
    // a hundred kilobytes even if nobody ever reads it, but it is not free).
    void drain_confirmed_events(std::vector<sim::Event>& out);

    // drain_confirmed_events() PLUS the events of every tick simulated but not
    // yet confirmed, i.e. the whole range [.., predicted_tick()). Call it ONCE,
    // at the moment the round stops, so that two peers whose confirmation
    // frontiers sit at different ticks still close the round having covered the
    // IDENTICAL tick range — the tail's content is the documented residual, its
    // extent is not. Advancing the session after this would double-count the
    // tail, so don't.
    void drain_remaining_events(std::vector<sim::Event>& out);

    // THE LIVE DIAGNOSTIC SNAPSHOT (net_stats.hpp). Everything the in-match
    // overlay draws and the end-of-session log line records — path, per-peer
    // RTT/lag, prediction depth, re-sim rate, stalls — derived entirely from
    // traffic this session already exchanges. Read-only and side-effect free;
    // the session behaves identically whether anyone calls it or not.
    const NetStats& stats() const { return stats_.stats(); }

    // Seats declared dropped so far (0 while everyone is live). With
    // DropPolicy::revert_to_ai these are the seats now driven by the AI; without
    // it, the seats whose loss ended the match.
    std::uint16_t dropped_seats() const { return dropped_; }
    // The agreed handoff tick for a dropped seat, or kNoHandoff if that seat is
    // still a live network peer. Every peer holds the same value.
    std::uint32_t handoff_tick(int seat) const {
        return handoff_[static_cast<std::size_t>(seat)];
    }
    // LOUD match-ending state: a peer dropped while Options row 12 was OFF, so
    // there is no legal way to keep simulating. advance() is a no-op from here
    // on — the caller polls this and ends the match (it never hangs).
    bool aborted() const { return aborted_; }

    // --- round abandon (Esc), wire v8 ----------------------------------------

    // "Stop this round." HOST ONLY — see the authority note at the top of this
    // file. On the host an end tick is scheduled and broadcast at once; on a
    // guest this is a NO-OP, because a guest has no say in what both machines
    // simulate. Idempotent: a second call while an end is already scheduled does
    // nothing.
    void request_end_round();

    // An end tick is agreed (on either peer). The match shell reads this to tell
    // an ABANDONED round — which is a DRAW by decree and tallies nothing — from
    // a round that ended on its own terms.
    bool end_round_scheduled() const { return end_tick_ != kNoEndRound; }
    // The agreed first tick NOBODY simulates, or kNoEndRound. Equal on every
    // peer once the announcement has propagated.
    std::uint32_t end_round_tick() const { return end_tick_; }
    // The sim has reached the agreed end: stop the round loop. advance() still
    // receives, re-announces and re-sends local input from here on, so a peer
    // that has not caught up yet still can.
    bool round_ended() const { return end_tick_ != kNoEndRound && tick_ >= end_tick_; }

    static constexpr std::uint32_t kNoHandoff = 0xFFFFFFFFU;
    static constexpr std::uint32_t kNoEndRound = 0xFFFFFFFFU;

private:
    struct Slot {
        sim::TickInputs inputs;         // seat inputs used to simulate this tick (confirmed or predicted)
        std::uint16_t confirmed = 0;    // which seats are CONFIRMED (rest are predicted)
    };

    // The whole of advance() except the diagnostics bracket around it. Split so
    // the tracker's begin/end pair is written ONCE and cannot be missed on any of
    // advance()'s several early-return paths (aborted, round ended, at the cap).
    void advance_impl(const sim::TickInputs& local_input);

    // Attribute one arriving input datagram to every seat it carries, for the
    // diagnostics only. `first_tick` is the sender's confirmed frontier (the
    // ack-RTT's basis), or 0 for a frame that carries none.
    void note_input_seats(std::uint16_t seats, std::uint32_t first_tick);

    void receive();  // drain transport -> input slots + peer hashes, flag rollbacks
    void apply_remote(std::uint32_t tick, std::uint16_t seats, const sim::TickInputs& in);
    sim::TickInputs assemble(std::uint32_t tick);   // confirmed seats + predicted (last-known) remote seats
    void resimulate(std::uint32_t from);            // restore snapshot[from], replay to tick_
    void advance_confirmed();                       // raise confirmed_ over the contiguous all-known prefix
    void send_local(std::uint32_t from);            // redundant range of recent local inputs [from, tick_)
    void note_peer_hash(std::uint32_t tick, std::uint64_t peer_hash);
    void prune();

    // The events produced BY simulating `tick`, or nullptr if that tick has not
    // been simulated (or its snapshot has already been pruned). Reads them off
    // the snapshot taken BEFORE `tick + 1` — the same place, and the same
    // lookup, advance_confirmed() takes the confirmed hash from, which is what
    // ties the two together.
    const std::vector<sim::Event>* events_of(std::uint32_t tick) const;

    // How far the newest input we hold from any still-awaited remote seat trails
    // our own speculative head — the same quantity the overlay calls `lag`.
    int local_lag() const;
    // The worst of the peers' OWN lags, read off the length of the InputRanges
    // they send (see the re-phasing note at the top of this file).
    int peer_lag() const;
    // "This peer is ahead of its partner by more than the path alone explains."
    bool should_rephase() const;

    // Seats whose input is still EXCHANGED at `tick`: all_seats_ minus every
    // seat already handed to the AI by then. The single tick-keyed predicate
    // behind both "what does the sim get fed" (assemble) and "whose input must
    // still arrive" (advance_confirmed / apply_remote / drop detection).
    std::uint16_t seats_awaited(std::uint32_t tick) const;
    // Re-assert the schedule on the hashed State at the START of simulating
    // `tick`. IDEMPOTENT (`ai = true` twice is `ai = true`), and called on the
    // first pass AND on every re-simulation of that tick — that is what makes
    // the handoff survive a rollback to a snapshot taken before it.
    void apply_handoffs(std::uint32_t tick);
    void heard(std::uint16_t seats);  // a packet carrying these seats arrived: they are alive
    void schedule_handoff(int seat, std::uint32_t at_tick);
    void detect_drops();        // hard-timeout scan; host schedules, everyone can abort
    void broadcast_handoffs();  // host-side redundant re-send, like send_local's
    // Adopt an announced end tick. Order-free and idempotent exactly as
    // schedule_handoff is: the EARLIEST tick wins, so a duplicate, a re-send and
    // an out-of-order copy all reduce to a no-op and every peer converges.
    void schedule_end_round(std::uint32_t at_tick);
    // HOST-only redundant re-send of a scheduled EndRound. A guest owes the
    // shell nothing here: it has no say in when a round stops.
    void broadcast_end_round();

    sim::Simulation* sim_;  // BORROWED
    Transport* transport_;
    std::uint16_t local_seats_;
    std::uint16_t all_seats_;
    std::uint16_t remote_seats_;
    int max_prediction_;
    DropPolicy drop_;

    std::uint32_t tick_ = 0;        // next speculative tick to simulate (starts at start_tick)
    std::uint32_t confirmed_ = 0;   // highest tick whose inputs are ALL confirmed (+1 = next unconfirmed)
    std::uint32_t rollback_to_ = 0;
    bool rollback_pending_ = false;

    std::unordered_map<std::uint32_t, Slot> slots_;            // per-tick inputs (confirmed/predicted)
    std::unordered_map<std::uint32_t, sim::State> snapshots_;  // State BEFORE simulating that tick
    // Prediction source: the most recent CONFIRMED input for each seat, and the
    // tick it came from (so a later confirmation wins). Predicting a remote
    // seat's next input = "repeat its last confirmed input".
    sim::TickInputs last_remote_input_;  // neutral until a seat is first confirmed
    std::array<std::uint32_t, sim::kMaxPlayers> last_remote_tick_{};
    std::unordered_map<std::uint32_t, std::uint64_t> hash_;  // our hash after each CONFIRMED tick
    std::unordered_map<std::uint32_t, std::uint64_t> peer_hash_;
    // Events of confirmed-but-not-yet-drained ticks, in tick order. Filled ONLY
    // by advance_confirmed(), which is also the only place the confirmed hash is
    // taken — so the two can never disagree about which ticks are final.
    std::vector<sim::Event> confirmed_events_;

    // The peer-drop SCHEDULE — the durable, tick-keyed record that survives
    // rollback. Deliberately NOT a one-shot mutation of State: `Player::ai` is
    // hashed, so restoring a pre-handoff snapshot would silently erase a flag
    // set once. Kept here (outside State, like the input slots) and re-applied
    // by apply_handoffs() at the head of EVERY simulation of a tick >= at_tick,
    // first pass or re-sim alike, so the flag is a pure function of the tick.
    std::array<std::uint32_t, sim::kMaxPlayers> handoff_;  // kNoHandoff = live peer
    // Pumps since anything at all was heard carrying that seat. LIVENESS, not
    // input progress: a peer stalled behind a THIRD peer's silence still sends
    // its redundant window every pump, and must not be accused of dropping.
    std::array<int, sim::kMaxPlayers> silence_{};
    // First tick for which we hold NO input from a seat (0 = never heard from
    // it). The host uses it as the handoff tick — see DropFrame on why that is
    // retroactive rather than in the future.
    std::array<std::uint32_t, sim::kMaxPlayers> remote_next_{};
    std::uint16_t dropped_ = 0;
    bool aborted_ = false;

    // The agreed round-abandon tick (kNoEndRound = the round runs to its own
    // end). Deliberately OUTSIDE sim::State — like the handoff schedule, it is
    // shell bookkeeping and must survive a rollback untouched; unlike it, it
    // never reaches the sim at all, so no golden hash can move because of it.
    std::uint32_t end_tick_ = kNoEndRound;

    // Each remote seat's OWN prediction depth, taken from the length of the last
    // InputRange it sent. Not hashed, not sent, and not part of any correctness
    // decision — it only ever decides whether THIS peer holds a tick to let its
    // partner catch up.
    std::array<int, sim::kMaxPlayers> peer_depth_{};
    bool peer_heard_ = false;  // nothing to compare against until a peer speaks
    // A re-phase hold is spread over alternate pumps so a skew is shed at half
    // rate rather than freezing the display outright.
    bool rephase_held_ = false;

    bool desynced_ = false;
    std::uint32_t desync_tick_ = 0;

    // INSTRUMENTATION ONLY. Deliberately last, deliberately not consulted by any
    // decision in this class: nothing below reads stats_, so a build that never
    // looks at it plays exactly the same match. Owns no heap and allocates on no
    // packet path (net_stats.hpp), so counting cannot perturb what it counts.
    NetStatsTracker stats_;
};

}  // namespace bomber::net
