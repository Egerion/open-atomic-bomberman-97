#pragma once

#include <array>
#include <bit>  // popcount, for the star-only host-migration gate
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
//
// ARRIVAL VARIANCE (jitter), and why the controller above needed a filter.
// MEASURED 2026-07-30 from 13 real Turkey<->Lithuania sessions in netdiag.log
// (the readings not flagged [OFFSET-BOUND,NOT-PATH]): the sessions the owner
// called silky and the ones he called badly laggy are separated by exactly ONE
// variable. Base RTT barely moved — 80-156 ms in both groups — while JITTER went
// from <=6 ms to 85-99 ms. Anything keyed to mean latency would be aimed at the
// wrong number.
//
// Jitter's damage was not the packets, it was THIS CONTROLLER reading them. Both
// halves of the comparison are instantaneous samples: our own lag is where the
// newest arrival happens to have left the frontier, and the peer's is the length
// of whichever InputRange landed last. Under 90 ms of arrival variance each
// swings a tick or two on its own, so their DIFFERENCE crosses a threshold of 2
// on noise alone — and a hold costs a displayed tick. Reproduced in
// tests/net/test_jitter_absorb.cpp over the ms-clock rig at the live conditions:
// at ~80 ms jitter the pair spent 12% of its pumps holding and ran the match at
// 17.6 ticks a second instead of 20, ON BOTH MACHINES. That is not a stutter, it
// is the whole game in slow motion, and it is self-inflicted. Corrections got
// deeper with it (4.6 re-simulated ticks per rollback against 2.0 on a clean
// path) because the two peers were chasing each other's phase.
//
// THE CURE IS TO MEASURE THE SUSTAINED ADVANTAGE, not an instant of it: the
// controller acts on the MINIMUM of the last kRephaseWindowPumps samples. A clock
// skew is permanent, so it survives a minimum; a jitter burst is not, so it does
// not. The same reasoning `rtt_min_ms` is built on — variance can only ever add
// delay, so the floor of a window is the excursion-free reading.
//
// A window buys noise immunity with REACTION TIME, though, and a full second of
// it is far too slow for the very case the re-phasing was built for: a peer whose
// frame loop freezes hands the pair several ticks of skew at once, and waiting the
// window out lets that skew spend the entire prediction budget first. (Measured,
// not feared — with the window alone both freeze scenarios in
// test_rollback_pacing.cpp went straight back to reaching the cap.) So the wait is
// required only where jitter is a PLAUSIBLE explanation, and there is a measured
// yardstick for that: an advantage larger than `peer_depth_spread()` plus the
// threshold is acted on at once. On a steady path the spread is 0 and the rule
// reduces to the unfiltered one this controller shipped with — which is why the
// whole clock-skew half of tests/net/test_rollback_pacing.cpp is numerically
// IDENTICAL to the build before this. Under the live 90 ms condition the spread is
// ~6, so an advantage would have to exceed the entire prediction cap to skip the
// wait, and arrival variance cannot manufacture that.
//
// Two properties make this safe to ship rather than merely plausible:
//   * BOTH arms imply the old predicate — a minimum is never above the current
//     sample, and a spread is never negative — so a pump this holds is a pump the
//     unfiltered controller would also have held. It can only ever hold LESS. On a
//     path that never triggered it, every branch is taken identically and the two
//     builds are the same program.
//   * it is self-limiting. Each hold moves this peer one tick back, which lowers
//     the next sample, which lowers the minimum — so a skew of N is shed in N
//     holds and the controller stops. No integrator, no gain to tune.
//
// Filtering the controller stops the game running in slow motion, but it does not
// make a single late packet arrive any sooner: with the phase left alone, the
// CORRECTIONS a burst causes are exactly the path's own. Measured by ablation at
// the live condition — the filter alone took held pumps from 71 to 33 and the rate
// from 17.6 to 18.9 t/s, while re-simulated ticks went UP, from 1298 to ~1408. The
// spurious holds had been buying a little re-sim work with a lot of the player's
// frame rate. Absorbing the arrivals themselves is the second half, and it is a
// different mechanism.
//
// THE LOCAL LEAD — adaptive input delay that needs NO AGREEMENT and NO WIRE
// CHANGE. The received wisdom is that input delay is a shared constant: both
// peers must apply the same one or they desync, `input_delay` is the SERVER's at
// match start, and changing it costs a kWireProtocolVersion bump and a new
// executable in every player's hands. THAT IS TRUE OF LOCKSTEP AND FALSE HERE,
// and the difference is worth stating precisely.
//
// LockstepSession's `input_delay` is a SCHEDULE: it decides WHICH TICK a sampled
// input applies to, so two peers with different values file the same keypress
// against different ticks and simulate different games. This session has no such
// schedule. It sends "seat s's input for tick T" and every peer feeds that value
// to tick T, whatever it is. How the owner of seat s CHOSE that value — from the
// keyboard as of tick T, or as of two ticks earlier — is invisible to everyone
// else and cannot make them disagree. So a peer may lead its own input by any
// amount, change it mid-match, and do it while its partner does something else
// entirely, without a single byte of new protocol.
//
// What the lead buys is real: filing our input k ticks ahead of our own head puts
// it on the wire k*50 ms before the peer needs it, so up to k*50 ms of arrival
// variance costs that peer NOTHING — no prediction, no misprediction, no re-sim.
// Unlike the phase, this is NOT zero-sum: we pay for it in our OWN input
// responsiveness, not out of our partner's budget.
//
// It is therefore held at ZERO unless arrival variance is actually being seen,
// because a lead is exactly the input lag rollback exists to avoid, and the owner's
// condition on this work was that a clean path must behave as it does today. The
// trigger is the SPREAD of the peer's own prediction depth (max minus min across
// the window) — which arrives free in the length of every InputRange, and which a
// steady path leaves at zero however SLOW it is. A 300 ms path with no jitter
// holds a constant depth and gets no lead; a 100 ms path that bursts gets one.
// ADR-0011 already argued for a small delay of 1-2 ticks on this evidence; the cap
// here is 2, and it is spent only where the measurements say it is earned.
//
// THE ONE RULE THAT MAKES CHANGING IT SAFE: `local_next_` — the tick our next
// local sample will be filed against — only ever moves FORWARD, and a tick that
// has been filed is never re-decided. The peer may already hold, and have
// confirmed and hashed, an input we filed three ticks ago; rewriting it would be
// a genuine desync. Raising the lead therefore files the current sample TWICE
// (the player's input is held one extra tick, 50 ms, unnoticeable) and lowering it
// files NOTHING for one pump and lets the head catch up. Neither rewrites
// anything, so the lead can move at any moment in a live match.
//
// WHAT WAS DELIBERATELY NOT BUILT, because the measurements rule it out:
//   * a RECEIVE-SIDE JITTER BUFFER. Delaying an input we already hold is strictly
//     worse than using it: it converts a confirmed tick back into a predicted one.
//     A buffer belongs on the SEND side, which is what the lead is.
//   * BIASING THE PHASE TARGET so this peer deliberately runs late enough to
//     absorb a burst. Prediction depth between two peers IS zero-sum — ours is
//     (d+skew)/tick, theirs is (d-skew)/tick, and the sum is fixed by the path —
//     so the slack we gain that way is charged to the partner, twice over, and
//     with both peers doing it they ratchet each other backwards, which is the
//     slow motion above with extra steps.
//   * a kWireProtocolVersion bump. Nothing here encodes or decodes anything new.
//     A build carrying this still plays a build that does not: the lead is
//     invisible to the peer except as input arriving early, which every version
//     of this session has always accepted (apply_remote files a future tick
//     without comment). The one asymmetry is that a peer on an older build reads
//     our InputRange length as our prediction depth and so over-reads it by our
//     lead — which makes IT hold LESS, the safe direction, and by at most 2.

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
// HOST MIGRATION ADDS A SECOND RESIDUAL of the same shape and the same bound.
// A migration can move `confirmed_` BACKWARDS (rewind_for_migration), which is
// the only thing in this class that does. The stream is therefore emitted at
// most once per tick (`events_through_`) — without that guard the rewound window
// would be drained twice and every kill in it double-counted, which is the exact
// failure above, reintroduced.
//
// COVERAGE, HONESTLY: the un-confirm branch itself is NOT reached by
// tests/net/test_host_migration.cpp — measured, and pinned there as a negative so
// the gap cannot rot silently. Confirming a tick needs every awaited seat, so no
// peer gets far above the tick the hub's input stops at, and the adopted tick is
// the lowest such tick across peers. So `events_through_` and the un-confirm are
// both DEFENSIVE here, not proven. The hash purge beside them IS proven, because
// it runs on every adoption whether or not the frontier moves.
//
// What the guard CANNOT recover is agreement across that window. A survivor that
// had already confirmed past the adopted tick drained those ticks' events as
// computed WITH the dead hub's input; a survivor that never got that far drains
// them as re-simulated with the seat on AI. The two disagree, and the earlier
// peer cannot retract what the shell already consumed. The window is bounded by
// `max_prediction` (≤ 400 ms at the default cap of 8) and sits around the instant
// a host died, so it is the same "taken on trust" class as the speculative tail
// rather than a new one — but it is a real divergence, not a covered case, and a
// tally that must agree exactly across a host migration does not yet exist.
//
// The residual is the SPECULATIVE TAIL — the at-most-`max_prediction` ticks
// between confirmed_ and the head at the moment the round stops. Both peers stop
// at the same TICK, so drain_remaining_events() lets the shell close the range on
// both machines identically; the CONTENT of those last few ticks is the one part
// still taken on trust. It is bounded (400 ms at the default cap of 8) and, on
// the ordinary round-end path, empty of kills — the shell lingers 3 s (60 ticks)
// after the last side falls before it reads the tally, which is far longer than
// the confirmation frontier ever trails.

// HOST MIGRATION (wire v9, ADR-0011 decision 5, design §8). Everything above
// assumes the hub is alive: the drop->AI handoff is scheduled by the host, and
// detect_drops() below used to carry a LIMITATION saying so — if the seat that
// went silent was the HOST's, nobody scheduled and the guests stalled forever.
// This is the cure, and it has three parts that must be read together.
//
//   DETECTION (§8.1). MsgType::HostLost, announced by EVERY survivor rather
//   than by one designated peer, because the machine that would normally decree
//   it is the one that died. Survivors DISAGREE about the tick — it is a local
//   quantity, differing by however many of the hub's last datagrams each lost —
//   and the LOWEST proposal wins everywhere. Monotone, so no agreement protocol.
//
//   ELECTION (§8.2). A pure function of the drop schedule: the starting hub
//   keeps the role while live, else the lowest surviving seat, chaining if the
//   successor dies too. No vote is exchanged and none is needed, because every
//   peer computes it from state they already agree on.
//
//     THE ELECTION READS THE SCHEDULE, NOT THE CONFIRMED FRONTIER, and that is a
//     RETRACTION of the obvious design rather than an oversight. Evaluating the
//     role at the frontier — so a peer takes it only once the migration is
//     agreed history — is unimplementable over the very topology the role exists
//     for, and it DEADLOCKS: the frontier cannot cross the migration tick until
//     the survivors exchange input; their input only ever reached each other
//     through the dead hub's reflection; and it will not flow again until
//     somebody WITH THE ROLE rewires the star. The frontier gate gates the
//     migration on itself. Reading the schedule is safe for the reason the
//     schedule exists: an entry only ever comes from an announcement or a
//     hard-timeout detection, it is idempotent and earliest-wins, and the role
//     decides nothing hashed — only which peer emits Drop frames. The tick-keyed
//     seats_awaited()/apply_handoffs() pair the sim reads is untouched.
//
//   RE-ANCHORING is NOT here. Rebuilding the star, re-punching to the new hub
//   and moving the lobby anchor are the caller's (MigratingTransport +
//   LobbyFlow::begin_migration + HostMigrationDriver). This class only decides
//   WHEN the role moves and holds the sim still while it does.
//
// Two consequences that are easy to miss and were both measured:
//   * a migration un-confirms (rewind_for_migration) — the only place the
//     frontier moves backwards — so snapshots are retained BELOW confirmed_
//     while migration is enabled; and
//   * send_local's start tick widens while a migration heals (resend_from),
//     because a severed star breaks the invariant that a peer missing an old
//     input is still stalled at it.

namespace bomber::net {

// How many pumps of frame-advantage history the re-phase controller must see the
// advantage hold across before it acts on it (the jitter note above). One second
// at 20 Hz — long enough to outlast the correlated delay bursts a congested path
// produces (the modelled ones in tests/net/test_jitter_absorb.cpp run a few
// hundred milliseconds), short enough that a genuine skew is still shed inside
// about a second and a half, against a live report that lived with one for a
// whole round. A window is a SPAN OF PUMPS rather than of milliseconds on
// purpose: the thing being filtered is measured in ticks.
inline constexpr int kRephaseWindowPumps = 20;

// The most local input lead the absorber will ever take (the jitter note above).
// Two ticks is 100 ms of arrival variance absorbed, and 100 ms of input lag paid
// for it — the upper end of the 1-2 ticks ADR-0011 §"Keep a small shared
// input_delay" already argued for, and the value the lobby picks for the lockstep
// path. Deliberately small: past this the cure is worse than the disease, and the
// rest of the burst is what rollback is FOR.
//
// THIS IS THE KNOB. It is the only number here that costs the player something he
// can feel, and feel is the one thing a loopback harness cannot measure — so it is
// meant to be judged in a live match and turned down if 100 ms reads worse than
// the corrections it removes. 1 halves both; 0 disables the lead entirely and
// leaves the re-phase filter, which is inert on a clean path either way.
inline constexpr int kMaxLocalLeadTicks = 2;

// How much spread in the peer's own prediction depth is treated as ordinary
// rather than as arrival variance worth spending input lag on. A pump boundary
// alone moves that depth by one tick on ANY path, and a second tick of slop keeps
// a merely-unlucky sample from putting input lag on a link that does not need it.
// The measured clean and 5 ms-jitter conditions both sit at or below this, which
// is what makes "a clean path is untouched" a fact rather than a hope.
inline constexpr int kLeadDeadbandTicks = 2;

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

    // HOST MIGRATION (wire v9, design §8). The seat that is the HUB, or -1 (the
    // default) to disable migration entirely — which is what every pre-existing
    // caller and test gets, so their behaviour is byte-identical to before: the
    // role stays whatever `is_host` said, MsgType::HostLost is ignored on
    // arrival, and prune() keeps its old window.
    //
    // With it set, `is_host` stops being the answer to "am I driving the game"
    // and becomes only the SEED of it: the role is recomputed every pump from
    // the drop schedule (hosting()), so it can move to this machine mid-round
    // when the hub dies. The two are kept as separate fields rather than one
    // because they answer different questions — `is_host` is "did the user press
    // Host", `host_seat` is "which seat is the hub right now" — and only the
    // second can change.
    //
    // LAST IN THE STRUCT DELIBERATELY. Every existing caller and test builds a
    // DropPolicy by POSITIONAL aggregate init (`{revert_to_ai, is_host,
    // timeout}`), so a field inserted above `timeout_ticks` silently rebinds
    // that third argument and turns drop detection off everywhere. That is not
    // hypothetical — it is exactly what happened when this field was first added
    // in the middle, and tests/net/test_peer_drop.cpp caught it. Append here.
    int host_seat = -1;
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

    // --- host migration (wire v9, design §8) ---------------------------------

    // The seat currently holding the HUB role, or -1 when migration is off. A
    // pure function of (drop schedule, DropPolicy::host_seat) — see hosting().
    int current_hub() const { return hub_; }
    // "This machine is the one driving the game." Equal to DropPolicy::is_host
    // when migration is off; recomputed from the election when it is on.
    bool hosting() const;
    // Seats this session has recorded as a LOST HUB, as opposed to an ordinary
    // dropped guest. Non-zero means a migration has been decided locally; the
    // caller (HostMigrationDriver) polls it to start rewiring the star.
    std::uint16_t host_lost_seats() const { return host_lost_; }

    // THE MIGRATION STALL. While held, advance() receives and re-sends but
    // simulates nothing and runs no drop detection, so the confirmed frontier —
    // and with it the floor of the retained rewind window — stays exactly where
    // the hub's death left it. The caller holds this for the rewire window and
    // releases it once a path exists again.
    //
    // Releasing also re-arms every silence counter, because by the time a
    // migration starts every remote seat has been silent for longer than the drop
    // timeout — that is HOW the hub's loss was detected — so a naive release
    // would declare the whole surviving table dropped one pump later.
    //
    // HONEST COVERAGE NOTE: that re-arm is belt-and-braces, NOT the mechanism the
    // suite actually proves. Reverting it alone leaves tests/net/
    // test_host_migration.cpp green, because two other things already cover the
    // window: detect_drops() declares nothing at all while migration_healing(),
    // and heard() zeroes the counter the moment the rewired star delivers
    // anything. The cascade test discriminates against the HEALING GUARD — revert
    // that and it fails loudly. The re-arm is kept because it is free and makes
    // the property hold even if the guard's window is later narrowed, but do not
    // read the passing test as evidence for it.
    void set_migration_hold(bool held);
    bool migration_held() const { return migration_hold_; }

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
    // "How far ahead of its partner is this peer" — ours minus theirs, which
    // cancels the path delay and leaves twice the clock skew. One instantaneous
    // sample, and therefore full of arrival variance: see sustained_advantage().
    int frame_advantage() const;
    // The part of that advantage which has held for the WHOLE window — the
    // minimum of the last kRephaseWindowPumps samples, and the only reading the
    // controller acts on (the jitter note at the top of this file). Never above
    // the current sample, so it can only ever hold LESS than the raw reading.
    int sustained_advantage() const;
    // Is a re-phase decision even meaningful right now — a peer is present, the
    // match is still running, and no host migration is currently making both
    // readings lie? Separated from the advantage test so the raw and filtered
    // readings are compared against exactly the same preconditions, so the
    // diagnostic can say honestly which of the two refused, and so the local lead
    // (lead_target) inherits every one of those preconditions rather than
    // re-stating them.
    bool rephase_eligible() const;
    // Push this pump's raw advantage and the peer's reported depth into their
    // windows. Called EVERY pump including held ones — otherwise neither window
    // would span a fixed stretch of time.
    //
    // An INELIGIBLE pump does not contribute a sample, it DISCARDS the window.
    // The two situations that make a pump ineligible — no peer heard yet, and a
    // host migration healing — are both ones where the readings already taken
    // describe a world that no longer exists: a migration freezes `peer_depth_`
    // at whatever the hub last relayed, so the step back to live values would
    // register as arrival variance and buy a lead nobody asked for. Starting over
    // costs one window of inaction, which is the same state a session opens in.
    void note_advantage(bool eligible, int raw);

    // THE LOCAL LEAD (the jitter note at the top of this file).
    // How much arrival variance the peer is currently living with, in ticks: the
    // SPREAD of its own prediction depth across the window. Zero on any steady
    // path, however slow, which is what keeps the lead off a clean link.
    int peer_depth_spread() const;
    // The lead that spread justifies, deadbanded and capped. 0 unless the peer is
    // genuinely being hit by variance.
    int lead_target() const;
    // Move `lead_` at most one tick toward the target. One tick per pump is the
    // whole rate limit the mechanism needs: raising it holds the local sample for
    // one extra tick and lowering it re-uses the previous one, and neither is
    // visible at 50 ms, where a multi-tick jump would be.
    void update_lead();
    // File the local sample against every still-unfiled tick up to the lead's
    // head. NEVER rewrites a filed tick — see the header note's "one rule".
    void file_local(const sim::TickInputs& local_input);

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

    // --- host migration internals (design §8.1/§8.2) --------------------------

    // Migration arms ONLY for a star — a match of more than two seats. The seat
    // count is the match's, not the survivors', so a 4-seat match that has
    // already lost two players keeps the machinery it started with.
    //
    // THE TWO-SEAT CASE IS DELIBERATELY EXCLUDED, and it is a safety gate rather
    // than a missing feature. With two seats the data plane cannot tell a DEAD
    // PEER from a DEAD PATH — the peer that stopped arriving may be gone, or may
    // be alive, still playing, and merely unreachable (design §4.2, observed
    // live: RX 0/s, ~100% LOSS, BAD 0 on a direct match deep into a session) —
    // and with nobody else at the table there is no third party whose view could
    // settle it. Electing on that guess makes each side hand the OTHER's seat to
    // the AI and play on inside a private, divergent game that neither player
    // can distinguish from a real one. A freeze is worse gameplay and better
    // information, so until §4.2's oracle exists the two-seat case is left
    // exactly as it was. Pinned by "a TWO-PEER path death must NOT trigger an
    // election" in tests/net/test_host_migration.cpp.
    //
    // A star is different in the one way that matters: the survivors can still
    // hear EACH OTHER once rewired, so "the hub is unreachable from everyone"
    // is a conclusion the remaining peers reach together rather than a guess one
    // peer makes alone.
    bool migration_enabled() const {
        return drop_.host_seat >= 0 && std::popcount(all_seats_) > 2;
    }
    // A migration is decided but not yet paid off: the frontier has not climbed
    // clear of the tick the hub died at. Gates the WIDE re-send, the re-phase
    // suppression, and — the one that is a correctness matter rather than a
    // tuning one — the refusal to declare a SECOND host loss (see detect_drops).
    bool migration_healing() const { return host_lost_ != 0 && confirmed_ < heal_until_; }
    // THE ELECTION, and it is a pure function — no vote, no message, no
    // coordinator (design §8.2). The seat that STARTED as the hub keeps the role
    // while it is live (whoever pressed Host need not be seat 0); once it is
    // gone the LOWEST surviving seat takes over, and the rule CHAINS — if the
    // elected hub dies too, the next-lowest succeeds it by the same rule.
    int hub_of(std::uint16_t live) const;
    // The survivor set the election runs over: all_seats_ minus every seat with
    // a scheduled handoff. Deliberately derived from the SCHEDULE and not from
    // the hashed State — a player who has been blown up still runs a machine and
    // can still be the hub — which is also what makes it recomputable after any
    // rollback and identical on every peer.
    std::uint16_t live_seats() const;
    // Where the redundant local-input re-send starts. Normally confirmed_; while
    // a migration heals, widened down to the oldest retained tick. See the .cpp
    // for why the usual invariant does not hold across a severed star.
    std::uint32_t resend_from() const;
    // Un-confirm back to `at_tick` so ticks simulated with the dead hub's real
    // input can be redone with the seat on AI, and purge every hash at or above
    // it on BOTH sides. Returns false if `at_tick` is older than the retained
    // window, in which case the caller reports a desync rather than guessing.
    // THE ONE PLACE THIS CLASS MOVES ITS FRONTIER BACKWARDS.
    bool rewind_for_migration(std::uint32_t at_tick);
    // Adopt an announced (or locally detected) host loss. Lowest tick wins, so
    // concurrent announcements from survivors that disagree converge with no
    // agreement protocol.
    void adopt_host_lost(int seat, std::uint32_t at_tick);
    // Re-send of every recorded host loss. NOT hub-only, unlike every other
    // broadcast here: the announcement is ABOUT the hub, so the machine the
    // redundancy discipline normally leans on is the one that is gone.
    void broadcast_host_lost();

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
    // High-water mark of ticks whose events have ALREADY entered the stream, so
    // no tick can enter it twice. `confirmed_` alone cannot serve, because host
    // migration moves that backwards; this only ever rises. See the emit site in
    // advance_confirmed() for why the distinction is a correctness matter and
    // not bookkeeping.
    std::uint32_t events_through_ = 0;

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

    // --- host migration state (all outside sim::State; none of it is hashed) --

    // The seat currently elected hub, recomputed every pump from live_seats().
    // -1 while migration is off.
    int hub_ = -1;
    // Seats recorded as a LOST HUB rather than an ordinary dropped guest. Drives
    // broadcast_host_lost() (every holder re-sends) and the wide re-send window.
    std::uint16_t host_lost_ = 0;
    // The tick above which the WIDE redundant re-send may stop (resend_from()):
    // a full rewind window past the migration tick, i.e. once no survivor can
    // still be waiting on an input either side has already finalised. Raised,
    // never lowered, so a second host loss extends it.
    std::uint32_t heal_until_ = 0;
    // The oldest tick still retained. prune() keeps a fixed window BELOW
    // confirmed_ while migration is enabled, because rewind_for_migration() has
    // to be able to un-confirm into it; this is that floor, and it only rises.
    std::uint32_t oldest_slot_ = 0;
    // The caller-driven stall across the rewire. See set_migration_hold().
    bool migration_hold_ = false;

    // Each remote seat's OWN prediction depth, taken from the length of the last
    // InputRange it sent. Not hashed, not sent, and not part of any correctness
    // decision — it only ever decides whether THIS peer holds a tick to let its
    // partner catch up.
    std::array<int, sim::kMaxPlayers> peer_depth_{};
    // The highest confirmed frontier each seat has reported, and whether it has
    // reported one at all. A sender's frontier only ever RISES, so a datagram
    // whose frontier has not is one that overtook a newer one on the way — and
    // the stale window it carries reads as the peer having suddenly caught up,
    // which is precisely the reading that makes this peer decide it is ahead.
    // Reordering is rare on a quiet path and routine on a jittery one, so this
    // guard costs nothing where it is not needed and removes a noise source where
    // it is. (net_stats' ack-RTT already gates on the same rise, for the same
    // reason.)
    std::array<std::uint32_t, sim::kMaxPlayers> peer_frontier_{};
    std::array<bool, sim::kMaxPlayers> peer_frontier_any_{};
    bool peer_heard_ = false;  // nothing to compare against until a peer speaks
    // A re-phase hold is spread over alternate pumps so a skew is shed at half
    // rate rather than freezing the display outright.
    bool rephase_held_ = false;
    // The frame-advantage window (the jitter note at the top of this file). A
    // plain ring of the last kRephaseWindowPumps samples — the only query is a
    // MINIMUM over the whole window, so which slot is newest does not matter, and
    // the scan is twenty integer compares once per pump.
    std::array<int, kRephaseWindowPumps> advantage_{};
    std::size_t advantage_next_ = 0;
    int advantage_count_ = 0;  // samples so far; below a full window, no decision
    // The peer's own prediction depth over the same window. Its SPREAD is the
    // arrival variance the local lead exists to absorb (the jitter note); shares
    // advantage_next_/advantage_count_ because both are sampled in the same place
    // on the same pump and a second index could only ever disagree.
    std::array<int, kRephaseWindowPumps> peer_window_{};

    // THE LOCAL INPUT LEAD. `local_next_` is the tick our next local sample will
    // be filed against and is MONOTONE — the single invariant that makes moving
    // the lead mid-match safe (see the header). At a lead of 0 it equals `tick_`
    // at every point either is read, which is what makes a clean path byte-for-
    // byte the session it was before this existed.
    std::uint32_t local_next_ = 0;
    int lead_ = 0;

    bool desynced_ = false;
    std::uint32_t desync_tick_ = 0;

    // INSTRUMENTATION ONLY. Deliberately last, deliberately not consulted by any
    // decision in this class: nothing below reads stats_, so a build that never
    // looks at it plays exactly the same match. Owns no heap and allocates on no
    // packet path (net_stats.hpp), so counting cannot perturb what it counts.
    NetStatsTracker stats_;
};

}  // namespace bomber::net
