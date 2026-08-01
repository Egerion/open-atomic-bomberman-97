# The in-match frame loop

`MatchRunner::run()` (`libs/frontend/src/match_runner.cpp`) — the
presentation-side driver of the deterministic sim. This page holds the rationale
that used to sit inline; `docs/coding-standards.md` §10 puts anything longer
than ~10 lines of reasoning here with a one-line pointer from the code.

Related: `docs/frame-pacing.md` (the `FramePacer` decision itself, which is
SDL-free and headlessly pinned), `docs/re/in-match-shell.md` (the RE source),
`docs/net-rollback.md` (the session this loop drives online).

## Why the loop paces presents explicitly

The original is a DirectDraw flip loop (`sub_42A191` / `sub_41E61E`): one input
read plus at most one tick per DISPLAYED frame, and the flip block IS the
throttle. `GameApp::init()` requests vsync, but on Windows windowed mode
`SDL_RenderPresent` does NOT reliably block — DWM gives the swapchain a
multi-frame flip queue, so presents return instantly in bursts (measured 4-12 ms
frame deltas) until the queue fills, then stall (20-25 ms).

The sim accumulator's crossings then land on that jerky CPU-side train and ticks
get assigned to frames in 2/4-frame beats instead of the steady
3-frames-per-tick a 20 Hz sim on a 60 Hz display needs. Measured with the same
live-run rig as the 2026-07-10 input-latency audit: **4-41% of tick-to-tick gaps
were a frame off** (visible micro-stutter), whether or not the old blind
`SDL_Delay(2)` throttle ran after present.

The fix is explicit pacing: sleep until the next display-refresh boundary after
each present. When present genuinely blocks on vblank the target is already
reached and the sleep is a no-op (the resync branch keeps the target
phase-locked to the real vblank train); when it doesn't block, the sleep supplies
exactly the cadence vsync failed to. Input latency is unchanged versus a
truly-blocking vsync — one `SDL_PollEvent` plus one `collect_inputs()` sample per
displayed frame either way, the original's own acquisition bound — and no fixed
extra delay sits on that path. A refresh-rate mismatch (59.94 Hz reported as 60,
VRR) only drifts the target phase and the resync branch absorbs it. Unknown
refresh falls back to 60 Hz, which still bounds the loop on drivers where
`SDL_SetRenderVSync` is a no-op (e.g. dummy video).

## F8 (uncapped) targets the sub-frame lattice, not a period

The useful ceiling is the sim's interpolation quantum: `renderer.cpp` floors
`interp_alpha * kSubFrames`, so a tick has exactly `kSubFrames` distinct
on-screen positions and presenting any faster only re-shows one.

The old code aimed at "now + sub-frame period" and dropped the whole overrun
whenever a present ran long, which on this renderer is routine: measured
2026-07-28, `SDL_RenderPresent` costs 0.3 ms at p50 but **11-18 ms at p99** as
the swapchain backs up against a 60 Hz panel, so the resync branch fired on
7-25% of frames and the achieved rate sat at 151-171 Hz instead of 180.

Targeting the next LATTICE point cannot lose phase (the target is read off the
tick clock, not off "now") and cannot present a sub-frame twice. The lattice
origin is `last - acc`: `acc` only ever moves by measured deltas and whole ticks,
so that is an exact point on the 50 ms tick clock rather than a per-frame
estimate, and the presents stay welded to the boundaries the interpolator
quantises onto. The F9 native path zeroes `acc` every frame, so there is no tick
clock to anchor to there — the lattice is left free-running rather than
re-anchored onto `last`, which would silently turn the absolute target back into
a relative one.

The coarse sleep overshoots by ~0.5 ms on Win11 and has a floor of about the
same, which at a 5.556 ms period is the difference between hitting the boundary
and missing it — hence the spin tail, asked for only by the uncapped path.

## Frame-cadence action-key capture

The ORIGINAL samples its live key-state array `byte_4A2BA0` (maintained by
`sub_433E14` from DirectInput BUFFERED records) once per DISPLAYED FRAME:
`sub_41F29B` shuffles the key bytes (+54=+56, then +56=0; pseudo.c 22976-22979)
and re-reads them via `sub_41E61E` (23037) in the same per-frame callback. Its
edge-gated bomb drop can therefore only miss a tap shorter than ONE display frame
(~14-16 ms).

Feeding the sim a state sample taken only once per 50 ms tick widened that loss
window ~3x — a normal human tap (~30-40 ms) could fall entirely between two tick
samples and the bomb press silently vanished. So the loop samples the mapped
inputs once per rendered frame and latches action-key downs until the next tick
consumes them.

**Directions are deliberately NOT latched.** They are level-driven (the original
integrates held time in ms, so a sub-tick tap moved a few px at most; stretching
it to a full 50 ms tick budget would overshoot the original far more than
dropping it does), while action1/2 are EDGE-consumed — capture-or-lose — which is
exactly what the frame sampling exists to capture.

The latch is consumed on the FIRST tick of a catch-up burst only. A later tick in
the same burst re-reads the live state, matching the original's
one-edge-check-per-update under a slow frame: its clamped ms delta produces
exactly one `sub_41E61E` read per displayed frame too.

## The long-stall guard

A window drag, alt-tab, asset stall or debugger break can hand the loop a
multi-hundred-ms delta; without a cap the catch-up `while` fires that many ticks
in one frame — the sim lurches (entities snap-teleport past the 32 px interp snap
threshold) and, worse, the loop can wedge trying to out-run real time. The queue
is capped at a few ticks' worth and the excess wall-time is DROPPED (the match
briefly runs in slow motion) rather than fast-forwarded. Determinism is
untouched: the sim still advances one deterministic tick per crossing; only how
many crossings a single hitch produces is bounded.

## When a round ends

`over_ticks` is a plain countdown (the campaign hazard-clear grace, and the
netplay fallback); `await_death_fx` is the animation-driven wait the deciding
kill arms instead. The two differ because:

**Normally**, when the results screen takes over is not a timer in the original,
and this used to be a flat 3 s. The round driver `sub_42A3F6` ends each pass of
its loop with two guards: the player count `sub_421947` (call at 0x42A6B2, keep
looping while > 1) and the clock-expired predicate `sub_41087D` (0x42A6BE) — no
sleep sits between them and the outcome tier. That count still includes a player
mid-DEATH-ANIMATION: the kill routine `sub_41DCB2` only raises the dying flag,
and the per-player pass `sub_41F29B` clears the slot's in-play flag only once the
death sequence has played its last step (its length coming from `sub_41DA5C`). So
the round is over one frame after the LAST corpse finishes animating. The shipped
"die green" sequences run 12-93 steps (0.6-4.65 s at 20 Hz), so a fixed 60-tick
linger truncated half of them and sat on an already-finished field after the
short ones. The clock-expired exit, by contrast, has no linger at all.

**Netplay keeps the old fixed linger.** The death-sequence pool and each
sequence's step count come from the LOCAL install's DATA/ANI files, which
`build_hash` does not cover, so an animation-driven handoff could land on a
different tick on each peer and leave one of them ticking a round the other has
already walked out of. A fixed count is the same tick on both.

**Campaign rounds end by `sub_4016DA`'s pacing verdict and NOTHING else**
(`docs/re/campaign.md` "Round end" / "Round pacing"). The survivor-count guard is
genuinely unreachable there: `sub_421969` @0x421977 returns a CONSTANT 2 while
`dword_46489C` is set, so the round loop's own player-count test at 0x42A62C
never trips, and the loop tail at 0x42A644 looks at `dword_464894` alone. Killing
every AI opponent does not end a campaign stage — clearing the monsters
(verdict 1) or running the clock out (verdict 2) does. The port used to apply the
normal `sides_remaining()` rule here, which ended EIGHT of the seventeen shipped
stages on their first tick: those have `ai_count` 0, so a lone human is already
the only side standing before anyone has moved.

**Once an abandon is agreed (Esc online), the agreed tick is the ONLY exit.** The
per-machine round-end reading is a reading of a SPECULATIVE state — it would let
one peer leave a tick or two before the other, having tallied a different slice
of the round's events — so it is skipped entirely while an abandon is in flight.

## Esc online is two different things

**Faithfulness first**: literal Esc (27) is INERT mid-round in the original
(`docs/re/in-match-shell.md` "Esc negative finding"); only Ctrl+Q (raw 0x11)
aborts, with no confirm prompt. The port keeps an Esc binding anyway as a
familiar "quit to menu" key, functionally standing in for Ctrl+Q.

**FIRST PRESS online — "stop the round", and HOST ONLY.** Esc must not be a local
act: a peer that stopped its own sim would have simulated, and tallied, a
different number of ticks than its partner. So it routes through the session, the
host schedules an agreed end tick and broadcasts it, and both peers stop there
with the round declared a DRAW. A GUEST's first press does nothing to the match —
deliberately, because the guest branch that used to send `EndRoundRequest` let
any guest force-end any round with no host confirmation.

**SECOND PRESS — "leave", and available to EVERYONE.** This is the escape hatch
the MatchCtl change removed by accident: with the peer stalled the host's
decision never comes back, so Esc looked dead and there was no way out of a
broken match at all. Note WHY it looked dead — the loop is fine. `SDL_PollEvent`
runs at the top of every frame, and a session held at the prediction cap returns
from `advance()` immediately, so the key was always being read; it simply had
nothing to do but wait on a peer that was not answering. The fix is not to revive
the loop, it is to give the key a meaning that needs no peer: `leave()` is local,
sends nothing, and the transport closes on the way out.

**Why a second press and not a timer or a confirm dialog.** Esc during a round
already means "stop", and a modal would have to be dismissed — with what, on a
machine where the player is already convinced input is being ignored? Two presses
need no new input vocabulary and cannot be reached accidentally. The window is
deliberately short and, crucially, VISIBLE: the first press puts a line on screen
that says a second one leaves, which is what makes this discoverable at the
moment it is needed rather than a secret. The two sides must NOT read the same —
a guest shown "press Enter" or "ending round" would be watching for something
that is never going to happen, which is exactly the confusion this started from.

## The online kill tally does not come from the live event stream

Under rollback that stream REPLAYS, so two peers with different prediction
histories accumulated different totals from the same agreed simulation and could
clinch the match for different players. Both tally calls go through
`net_tally.hpp`, which takes each tick's events from the session's CONFIRMED
stream exactly once.

## The F1 help modal resets the clock

`sub_42A16F(1)`/`(0)` bracket the modal — the whole game freezes under the help
overlay: sim, rendering, HUD, everything. On resume the port resets the
accumulator and clock instead of reproducing the original's documented bug (the
round clock silently absorbs the whole modal duration in one lump, §1's "Pause
negative finding" point 2). A DELIBERATE deviation, so closing the browser does
not fire a tick burst or eat round time.

## The fps overlay reports what is IN EFFECT

F9 stays pressable online — the toggle lives in `GameApp`'s global event filter,
shared with every screen, and silently swallowing it there would be a second
special case to keep in sync — but a netplay match ignores it. So the row would
otherwise read NATIVE while the sim ran fixed-tick. "(NET)" says WHY it is
ignored, which is the difference between an honest overlay and a player pressing
F9 repeatedly wondering what is broken.
