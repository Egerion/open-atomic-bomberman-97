#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "bomber/sim/constants.hpp"  // kMaxPlayers

// THE PACING HALF OF ROLLBACK NETCODE, lifted out of RollbackSession for the
// reason libs/platform's FramePacer was lifted out of the SDL frame loops: the
// DECISION is a pure function of numbers the caller has already measured, so it
// can be pinned by a headless unit test (tests/net/test_time_sync.cpp) instead of
// only being observable by running two peers at each other over a modelled path.
//
// WHERE THE SEAM FALLS, and why it is a real one rather than a tidy one: nothing
// in this file touches hashed state. It reads peer frame numbers and prediction
// depths and answers two questions — should this pump be HELD, and how far ahead
// of its own head should this peer FILE its input — both of which are decisions
// about WHEN this peer simulates and never about WHAT it simulates. The other
// half of RollbackSession is the exact opposite: it is entirely about hashed
// state and must stay bit-exact. So a change here cannot move a golden hash or
// `build_hash`, and a change there cannot be reasoned about with the arithmetic
// below in the way.
//
// IT READS NO CLOCK, exactly as FramePacer reads none. The two samples it needs
// arrive from the session — our own prediction depth, and the peer's own depth
// off the length of the InputRange it last sent — and the eligibility question
// ("is a comparison even meaningful right now") stays with the session, because
// only the session knows whether a peer is present, whether the round is ending
// and whether a host migration is currently making both readings lie.
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

// How much CLOCK SKEW (in ticks) this peer tolerates before it starts giving it
// back. Our lag minus the peer's own lag is twice the skew — the path delay is in
// both and cancels — so this is a threshold on 2x the skew, i.e. it engages once
// this peer is a full tick (50 ms) ahead of its partner. Small on purpose: the
// whole failure being cured is skew ACCUMULATING unnoticed until the prediction
// cap is the only thing left holding it, so it must be shed while there is still
// budget to spare. Too small and ordinary arrival noise would trip it: at 2 the
// margin is a factor of two over the +/-1 tick a pump-boundary can contribute.
inline constexpr int kRephaseAdvantageTicks = 2;

class TimeSyncController {
public:
    // What one pump's comparison came to. Everything here is a READING except
    // `hold`, which is the one thing the session must act on; the rest exists so
    // the diagnostics can show WHY it did or did not (net_stats.hpp).
    struct Decision {
        int raw = 0;        // this pump's instantaneous frame advantage
        int sustained = 0;  // the part of it that held for the whole window
        int spread = 0;     // the peer's own depth spread — the variance yardstick
        bool hold = false;  // give a tick back: do not simulate this pump
        // "The raw reading wanted this tick and the filter kept it" — the
        // absorber's own meter, and the only way to see from the outside that it
        // is doing anything (net_stats.hpp). Not a decision input: nothing below
        // reads it.
        bool suppressed = false;
    };

    // One arriving InputRange, whose LENGTH is the sender's own prediction depth
    // (the re-phasing note above) — which is why the remote half of the
    // comparison costs no wire message. `seats` is the remote seats the datagram
    // carried and `first_tick` the sender's confirmed frontier, which the
    // staleness guard on `peer_frontier_` needs.
    void note_peer_range(std::uint16_t seats, std::uint32_t first_tick, int depth);

    // A peer has spoken at least once, so there is something to compare against.
    // One of the session's eligibility preconditions, kept here because this is
    // where the evidence for it arrives.
    bool peer_heard() const { return peer_heard_; }

    // The worst of the peers' OWN lags, read off the length of the InputRanges
    // they send (see the re-phasing note at the top of this file). `awaited` is
    // the remote seats whose input is still exchanged at the current tick — the
    // session's question, so the session supplies it.
    int peer_lag(std::uint16_t awaited) const;

    // "How far ahead of its partner is this peer" — ours minus theirs, which
    // cancels the path delay and leaves twice the clock skew. One instantaneous
    // sample, and therefore full of arrival variance: see sustained_advantage().
    int frame_advantage(int local_depth, std::uint16_t awaited) const {
        return local_depth - peer_lag(awaited);
    }

    // The part of that advantage which has held for the WHOLE window — the
    // minimum of the last kRephaseWindowPumps samples, and the only reading the
    // controller acts on (the jitter note at the top of this file). Never above
    // the current sample, so it can only ever hold LESS than the raw reading.
    int sustained_advantage() const;

    // THE LOCAL LEAD (the jitter note at the top of this file).
    // How much arrival variance the peer is currently living with, in ticks: the
    // SPREAD of its own prediction depth across the window. Zero on any steady
    // path, however slow, which is what keeps the lead off a clean link.
    int peer_depth_spread() const;

    // THE WHOLE PER-PUMP DECISION, and a pure function of (this pump's samples,
    // the windows the previous pumps filled). `local_depth` is how far this
    // peer's own filing head leads its confirmed frontier; `eligible` is the
    // session's precondition (a peer is present, the match is still running, no
    // host migration is making both readings lie).
    //
    // MUST BE CALLED EVERY PUMP, including ones the caller is about to hold and
    // ones where nothing is eligible, or the windows below would span a variable
    // stretch of time rather than a fixed one.
    Decision pump(bool eligible, int local_depth, std::uint16_t awaited);

    // The local input lead currently in force, in ticks (the lead note above).
    int lead() const { return lead_; }

    // Move `lead_` at most one tick toward the target. One tick per pump is the
    // whole rate limit the mechanism needs: raising it holds the local sample for
    // one extra tick and lowering it re-uses the previous one, and neither is
    // visible at 50 ms, where a multi-tick jump would be.
    //
    // Called ONLY on a pump that actually simulates — a pump that does not must
    // not consume a sample either, or the lead would grow by one for every
    // stalled pump. That is also why the lead a diagnostic prints can outlive the
    // spread that bought it: `pump()` above runs on every pump and this does not.
    void update_lead(bool eligible);

private:
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
    void note_advantage(bool eligible, int raw, int lag);

    // The lead that spread justifies, deadbanded and capped. 0 unless the peer is
    // genuinely being hit by variance.
    int lead_target(bool eligible) const;

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
    // Ticks of local input lead in force. The session owns the FILING head this
    // moves (`local_next_`, and the monotonicity rule that makes moving it mid-
    // match safe); this owns only how far ahead of its own head that head sits.
    int lead_ = 0;
};

}  // namespace bomber::net
