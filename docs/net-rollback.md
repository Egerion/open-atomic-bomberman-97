# Rollback netcode — pacing and the confirmed event stream

The two bodies of measured evidence behind `libs/net`'s rollback path. Both used
to live as file-header essays in `time_sync.hpp` and `rollback_session.hpp`;
they are reference material a reader consults once, not something to re-read on
every visit to the code, so they live here and the headers point at them.

Companion documents: `docs/adr/0010` (lockstep over UDP), `docs/adr/0011`
(online multiplayer), `docs/online-multiplayer-design.md` §6 (RTT → rollback
parameters), §8 (host migration) and §10 (the match shell).

---

## 1. Pacing — `TimeSyncController` (`time_sync.hpp`)

### 1.1 Why it is a separate class

Nothing in the pacing half touches hashed state. It reads peer frame numbers and
prediction depths and answers two questions — should this pump be **held**, and
how far ahead of its own head should this peer **file** its input — both of which
decide *when* this peer simulates and never *what* it simulates. The other half
of `RollbackSession` is the exact opposite: entirely about hashed state, and
bit-exact. So a change here cannot move a golden hash or `build_hash`, and a
change there is not read through this arithmetic.

It reads no clock, exactly as `libs/platform`'s `FramePacer` reads none: the two
samples it needs arrive from the session, so the decision is a pure function of
numbers already measured and is pinned by a headless unit test
(`tests/net/test_time_sync.cpp`) rather than by running two peers at each other.

Eligibility ("is a comparison even meaningful right now") stays with the session,
because only the session knows whether a peer is present, whether the round is
ending, and whether a host migration is making both readings lie.

### 1.2 Re-phasing: the standing-lag cure

**Measured 2026-07-30** with a two-peer harness on independent wall clocks
(`tests/net/test_rollback_pacing.cpp`). Any freeze of a peer's frame loop longer
than `MatchRunner`'s 200 ms catch-up clamp has its excess wall time *discarded*,
so that peer falls permanently behind its partner — a 400 ms window drag costs a
standing 4 ticks and nothing ever gives them back.

The prediction cap was the only re-phasing mechanism there was, and it only
engages once the whole budget is spent: the peer that is ahead stalls at 8/8 for
the rest of the round, so every ordinary packet-timing wobble becomes a visible
stutter. A live **Turkey↔Lithuania** record showed exactly that end state —
`depth=8/8`, `lag=lag_max=8`, `rollbacks=0` (the peer was never *wrong*, only
late) on a ~100 ms path where the healthy depth is 2.

The cure is GGPO's frame-advantage time-sync, and the numbers it needs are
already on the wire. An `InputRange` spans `[sender's confirmed, sender's head)`,
so its **length is the sender's own prediction depth** — the peer's own lag,
measured on the peer. Ours minus theirs cancels the path delay (both contain it
equally) and leaves twice the **clock skew**, which is the part that should not
be there. Past a small threshold the peer that is ahead holds one tick per pump
until the skew is gone, so depth returns to the path baseline instead of parking
at the cap.

### 1.3 Arrival variance, and why the controller needed a filter

**Measured 2026-07-30** from 13 real Turkey↔Lithuania sessions in `netdiag.log`
(the readings not flagged `[OFFSET-BOUND,NOT-PATH]`): the sessions the owner
called silky and the ones he called badly laggy are separated by exactly one
variable. Base RTT barely moved — 80–156 ms in both groups — while **jitter went
from ≤6 ms to 85–99 ms**. Anything keyed to mean latency would be aimed at the
wrong number.

Jitter's damage was not the packets, it was *this controller reading them*. Both
halves of the comparison are instantaneous samples: our own lag is where the
newest arrival happens to have left the frontier, and the peer's is the length of
whichever `InputRange` landed last. Under 90 ms of arrival variance each swings a
tick or two on its own, so their difference crosses a threshold of 2 on noise
alone — and a hold costs a displayed tick.

Reproduced in `tests/net/test_jitter_absorb.cpp` over the ms-clock rig at the
live conditions: at ~80 ms jitter the pair spent **12% of its pumps holding** and
ran the match at **17.6 ticks a second instead of 20, on both machines**. That is
not a stutter, it is the whole game in slow motion, and it is self-inflicted.
Corrections got deeper with it (4.6 re-simulated ticks per rollback against 2.0
on a clean path) because the two peers were chasing each other's phase.

**The cure is to measure the sustained advantage, not an instant of it**: the
controller acts on the *minimum* of the last `kRephaseWindowPumps` samples. A
clock skew is permanent, so it survives a minimum; a jitter burst is not, so it
does not. Same reasoning `rtt_min_ms` is built on — variance can only ever add
delay, so the floor of a window is the excursion-free reading.

A window buys noise immunity with **reaction time**, and a full second of it is
far too slow for the very case re-phasing was built for: a peer whose frame loop
freezes hands the pair several ticks of skew at once, and waiting the window out
lets that skew spend the entire prediction budget first. (Measured, not feared —
with the window alone both freeze scenarios in `test_rollback_pacing.cpp` went
straight back to reaching the cap.) So the wait is required only where jitter is
a *plausible* explanation, and there is a measured yardstick for that: an
advantage larger than `peer_depth_spread()` plus the threshold is acted on at
once. On a steady path the spread is 0 and the rule reduces to the unfiltered one
this controller shipped with — which is why the whole clock-skew half of
`test_rollback_pacing.cpp` is numerically *identical* to the build before it.
Under the live 90 ms condition the spread is ~6, so an advantage would have to
exceed the entire prediction cap to skip the wait, and arrival variance cannot
manufacture that.

Two properties make this safe to ship rather than merely plausible:

- **Both arms imply the old predicate** — a minimum is never above the current
  sample, and a spread is never negative — so a pump this holds is a pump the
  unfiltered controller would also have held. It can only ever hold *less*. On a
  path that never triggered it, every branch is taken identically and the two
  builds are the same program.
- **It is self-limiting.** Each hold moves this peer one tick back, which lowers
  the next sample, which lowers the minimum — so a skew of N is shed in N holds
  and the controller stops. No integrator, no gain to tune.

Filtering the controller stops the game running in slow motion, but it does not
make a single late packet arrive any sooner: with the phase left alone, the
corrections a burst causes are exactly the path's own. Measured by ablation at
the live condition — the filter alone took held pumps from 71 to 33 and the rate
from 17.6 to 18.9 t/s, while re-simulated ticks went *up*, from 1298 to ~1408.
The spurious holds had been buying a little re-sim work with a lot of the
player's frame rate.

### 1.4 The local lead — adaptive input delay needing no agreement

The received wisdom is that input delay is a shared constant: both peers must
apply the same one or they desync, `input_delay` is the server's at match start,
and changing it costs a `kWireProtocolVersion` bump and a new executable in every
player's hands. **That is true of lockstep and false here.**

`LockstepSession`'s `input_delay` is a *schedule*: it decides which tick a
sampled input applies to, so two peers with different values file the same
keypress against different ticks and simulate different games. `RollbackSession`
has no such schedule. It sends "seat s's input for tick T" and every peer feeds
that value to tick T, whatever it is. How the owner of seat s *chose* that value —
from the keyboard as of tick T, or as of two ticks earlier — is invisible to
everyone else and cannot make them disagree. So a peer may lead its own input by
any amount, change it mid-match, and do so while its partner does something else
entirely, without a single byte of new protocol.

What the lead buys is real: filing our input k ticks ahead of our own head puts
it on the wire k×50 ms before the peer needs it, so up to k×50 ms of arrival
variance costs that peer nothing — no prediction, no misprediction, no re-sim.
Unlike the phase, this is **not zero-sum**: we pay for it in our own input
responsiveness, not out of our partner's budget.

It is therefore held at **zero** unless arrival variance is actually being seen,
because a lead is exactly the input lag rollback exists to avoid, and the owner's
condition on this work was that a clean path must behave as it does today. The
trigger is the *spread* of the peer's own prediction depth (max minus min across
the window) — which arrives free in the length of every `InputRange`, and which a
steady path leaves at zero however slow it is. A 300 ms path with no jitter holds
a constant depth and gets no lead; a 100 ms path that bursts gets one. ADR-0011
already argued for a small delay of 1–2 ticks on this evidence; the cap here is
2, and it is spent only where the measurements say it is earned.

**The one rule that makes changing it safe**: `local_next_` — the tick our next
local sample will be filed against — only ever moves *forward*, and a tick that
has been filed is never re-decided. The peer may already hold, and have confirmed
and hashed, an input we filed three ticks ago; rewriting it would be a genuine
desync. Raising the lead therefore files the current sample **twice** (the
player's input is held one extra tick, 50 ms, unnoticeable) and lowering it files
**nothing** for one pump and lets the head catch up. Neither rewrites anything,
so the lead can move at any moment in a live match.

### 1.5 What was deliberately not built

The measurements rule these out; do not re-propose them without new ones.

- **A receive-side jitter buffer.** Delaying an input we already hold is strictly
  worse than using it: it converts a confirmed tick back into a predicted one. A
  buffer belongs on the *send* side, which is what the lead is.
- **Biasing the phase target** so this peer deliberately runs late enough to
  absorb a burst. Prediction depth between two peers *is* zero-sum — ours is
  `(d+skew)/tick`, theirs is `(d−skew)/tick`, and the sum is fixed by the path —
  so slack gained that way is charged to the partner, twice over, and with both
  peers doing it they ratchet each other backwards, which is the slow motion
  above with extra steps.
- **A `kWireProtocolVersion` bump.** Nothing in the pacing half encodes or
  decodes anything. A build carrying it still plays a build that does not: the
  lead is invisible to the peer except as input arriving early, which every
  version of this session has always accepted (`apply_remote` files a future tick
  without comment). The one asymmetry is that a peer on an older build reads our
  `InputRange` length as our prediction depth and so over-reads it by our lead —
  which makes *it* hold *less*, the safe direction, and by at most 2.

### 1.6 The four tuning constants

| constant | value | why |
|---|---|---|
| `kRephaseWindowPumps` | 20 | One second at 20 Hz: long enough to outlast the correlated delay bursts a congested path produces (the modelled ones in `test_jitter_absorb.cpp` run a few hundred ms), short enough that a genuine skew is shed inside about a second and a half. A span of *pumps* rather than of milliseconds because the thing being filtered is measured in ticks. |
| `kMaxLocalLeadTicks` | 2 | 100 ms of arrival variance absorbed, 100 ms of input lag paid for it — the upper end of ADR-0011's 1–2 ticks. **This is the knob**: the only number here that costs the player something he can feel, and feel is the one thing a loopback harness cannot measure. Judge it in a live match; 1 halves both, 0 disables the lead and leaves the re-phase filter (inert on a clean path either way). |
| `kLeadDeadbandTicks` | 2 | A pump boundary alone moves the peer's depth by one tick on *any* path, and a second tick of slop keeps a merely-unlucky sample from putting input lag on a link that does not need it. The measured clean and 5 ms-jitter conditions both sit at or below this, which is what makes "a clean path is untouched" a fact rather than a hope. |
| `kRephaseAdvantageTicks` | 2 | Our lag minus the peer's is *twice* the skew, so this engages once this peer is a full tick (50 ms) ahead. Small on purpose: the failure being cured is skew accumulating unnoticed until the prediction cap is the only thing holding it, so it must be shed while there is budget to spare. At 2 the margin is a factor of two over the ±1 tick a pump boundary can contribute. |

---

## 2. The confirmed event stream (`rollback_session.hpp`)

### 2.1 The divergence it fixes

Rollback makes `State::events` a stream that **replays**: a tick simulated
speculatively and then corrected produces its events twice, and the two passes
need not agree. Anything that *accumulates* those events across ticks therefore
accumulates a per-machine number — and because events are excluded from
`state_hash` by design (determinism rule 4), the per-tick desync check, the
goldens and `build_hash` are all blind to it.

That is not hypothetical. The front-end's per-match kill tally (`libs/game`'s
`results.hpp` `tally_kills`) was fed straight from `sim.state().events` once per
pump, so two peers with different rollback histories reached different kill
totals from an *identical, agreed* simulation — and with Team Play + "win by
kills" that is a different match **verdict** on the two machines.

The cure is to hand such a consumer only the events of **confirmed** ticks. A
confirmed tick's inputs can never change again, so it is simulated exactly once
more than the goldens are. They need no storage of their own: `snapshots_[t+1]`
is the `State` *after* tick t and carries its `events`, which is the same lookup
`advance_confirmed()` already does for the hash. So the stream a consumer sums
is, tick for tick, the stream whose hash both peers have compared and agreed on.

### 2.2 The two residuals

**Host migration** can move `confirmed_` *backwards* (`rewind_for_migration`),
the only thing in the class that does. The stream is therefore emitted at most
once per tick (`events_through_`); without that guard the rewound window would be
drained twice and every kill in it double-counted — the same failure,
reintroduced through the one door its author could not have known about.

What the guard **cannot** recover is agreement across that window. A survivor
that had already confirmed past the adopted tick drained those ticks' events as
computed *with* the dead hub's input; a survivor that never got that far drains
them as re-simulated with the seat on AI. The two disagree, and the earlier peer
cannot retract what the shell already consumed. The window is bounded by
`max_prediction` (≤ 400 ms at the default cap of 8) and sits around the instant a
host died, so it is the same "taken on trust" class as the speculative tail — but
it is a real divergence, not a covered case, and a tally that must agree exactly
across a host migration does not yet exist.

**The speculative tail** is the at-most-`max_prediction` ticks between
`confirmed_` and the head at the moment the round stops. Both peers stop at the
same *tick*, so `drain_remaining_events()` lets the shell close the range on both
machines identically; the *content* of those last few ticks is the part still
taken on trust. It is bounded (400 ms at the default cap of 8) and, on the
ordinary round-end path, empty of kills — the shell lingers 3 s (60 ticks) after
the last side falls before it reads the tally, which is far longer than the
confirmation frontier ever trails.

---

## 3. Honest coverage

Three mechanisms in `RollbackSession` are **defensive rather than proven**.
Reverting any one of them alone leaves `tests/net/test_host_migration.cpp` green.
They are recorded here so a future reader does not mistake a passing suite for
evidence.

| mechanism | why it is not covered | what *is* covered |
|---|---|---|
| the un-confirm branch of `rewind_for_migration`, and `events_through_` | Confirming a tick needs every awaited seat, so no peer gets far above the tick the hub's input stops at, and the adopted tick is the lowest such tick across peers. The suite never constructs a survivor that is ahead of it. | The hash purge beside them, which runs on every adoption whether or not the frontier moves. |
| the silence re-arm in `set_migration_hold(false)` | `detect_drops()` declares nothing at all while `migration_healing()`, and `heard()` zeroes the counter the moment the rewired star delivers anything. Two other mechanisms already cover the window. | The **healing guard**: revert that and the cascade test fails loudly. |
| the wide re-send window in `resend_from()` | Carried from an earlier branch's measurement. The suite's divergence case does not reach the wedge, because the peer that is ahead gets rewound to the adopted tick and re-sending from a rewound frontier already covers what the peer behind needs. The documented wedge needs a survivor behind on a tick *below* the migration tick. | Nothing. Kept because the failure was observed on the earlier branch (a 3-seat star wedged eight ticks apart with the migration otherwise perfectly converged) and the cost is a few dozen bytes per datagram for a second or two. |

The whole of what `RollbackSession` owns across a migration — detection,
election, un-confirm, stall — is proven against a **modelled** rewire
(`StarBus::set_hub`), never a real one. See
`docs/online-multiplayer-design.md` §8.3 "What is implemented, and what is not".

---

## 4. Diagnostics: what the ack-RTT actually measures (`net_stats.hpp`)

Two rules shaped `NetStatsTracker`, and both are load-bearing:

1. **No new wire message.** Every number is derived from traffic that already
   flows. A `kWireProtocolVersion` bump invalidates every build in the wild — the
   `build_hash` door refuses a mismatch, so each one means hand-delivering exes —
   and the version has always been somebody else's to spend.
2. **The counting must not perturb what it measures.** Every buffer is a
   fixed-size array owned by value: no allocation on any packet path, no logging
   in the hot path, no map lookups. The whole tracker is a few hundred bytes and
   every per-packet operation is O(1) integer work.

### 4.1 The derivation

An `InputRange` always starts at the sender's **confirmed frontier**, and a peer
cannot confirm tick T until our input for T has arrived. So the first datagram
from a seat whose `first_tick` **rises** above T proves our input for T−1
completed a round trip, and the interval from "our input for T−1 first left" to
"that datagram arrived" is measured exactly.

Sampling only on a *rise* is essential: the peer re-sends its whole unconfirmed
window every pump, so measuring every datagram at an unchanged frontier would
report a round trip that grows without bound.

It is an **upper bound**, never the network RTT, because it also contains up to
one of the peer's 50 ms pumps; any time the peer spent behind T−1 before it could
confirm; in a >2-seat match the wait for the slowest other seat; and
`local_lead × 50 ms` whenever the absorber has a lead in force.

### 4.2 `rtt_offset_bound` — when the reading means nothing

**Measured, not theorised**: two peers over UDP loopback (a ~25 ms path) reported
**33 ms and 333 ms** respectively. In rollback netcode the peers settle into a
wall-clock **phase offset**, bounded only by the prediction cap, and the peer
that is *ahead* is measuring that offset rather than the wire.

Write the one-way delay as `d` and the peer's wall-clock lead over us as `G`,
both in pumps. The ack sample is `max(G, d) + d`:

- **peers level** (`G = 0`) — our input reaches the peer `d` after we sent it and
  it answers at once, so the sample is `2d`, while the lag is only the `d` it
  takes the peer's own input to reach us. **`rtt = 2 × lag`.**
- **peer behind** (`G > d`) — the peer cannot answer until it reaches T, so the
  sample is `G + d`, and the lag is `G + d` too, because its newest input is
  exactly that far back. **`rtt = lag`**, and the reading has stopped containing
  any information about the wire.

So a healthy reading sits near 2× the lag and a contaminated one near 1×. Cutting
at 1.5× separates them with a factor of two of margin on both sides. Judged on
the *latest* sample, not the session minimum: an offset builds up over a match,
and a good sample from before it did says nothing about what the numbers mean
now.

There is no correction, only the flag. Once the offset dominates, the one-way
delay is genuinely not observable from this side without a message carrying a
timestamp — and that costs a wire version. "Not measurable right now" is the
honest output, and `lag_ticks` is the actionable number in that state anyway.

**A local lead breaks the same premise, and more completely.** While we file our
input k ticks early it reaches the peer *before* the peer gets to that tick, so
its frontier stops rising on our input's arrival and starts rising on its own
progress: the sample becomes `max(one_way, lead + offset) + one_way`, still an
upper bound on the path but no longer an estimate of it. No ratio test can see
that — the lead inflates the sample and *deflates* the lag at the same time — so
it is **declared rather than detected**. The alternative was to let the number
silently change meaning, which is precisely what this file exists not to do: the
owner's whole jitter table (§1.3) was built by discarding the readings this flag
marks. `peer_depth_spread` is the arrival-variance reading that survives a lead,
and is what to read instead.
