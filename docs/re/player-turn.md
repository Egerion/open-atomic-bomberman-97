# The per-frame player turn (sub_41F29B)

Rationale for `player_turn` in `libs/sim/src/simulation.cpp`, moved out of the
source per `docs/coding-standards.md` §10 (a rationale longer than ~10 lines is a
docs page with a one-line pointer). Every citation below is load-bearing; the
code carries pointers back to the sections here.

`player_turn` is one player's pass through `sub_41F29B`. The original runs it
once per **displayed frame**, not once per 50 ms tick, so the port's body is a
loop over `kSubFrames` canonical frames (`docs/re/facts.md` "Canonical frame
cadence", ADR-0006). Everything the loop reads per sub-frame — the AI re-decide
delta, the movement-budget accrual, the stun burn, the bomb-action tail — comes
off the cadence schedule, so the same body serves the deterministic 20 Hz tick
and the F9 native-cadence pass (ADR-0007) unchanged.

## §1 The two input blockers: head-hit stun and pickup-pause

`Player::stun` is the original's WORD +58 (`sub_421F7E`). `Player::pickup_pause`
is the state `+78 == 4` window, gated by `getvalue(665)`. They are two
independent counters; they shared one field until 2026-07-11, which let a grab
clobber an in-progress head-stun countdown and vice versa.

Either counter clears the original's new-input flag (`sub_41F29B` ~22981-23027),
which blocks **only new-input acquisition** — the `sub_41E61E` / AI-decide call
that would set a new direction (+46) and the bomb-key bytes (+56/+57), plus one
cosmetic standing-anim pick (~23086). Neither gates the mover. A
blocked-but-alive player leaves +46 at its per-tick -1 reset (22980), so it takes
the IDLE movement branch (23413) — but the per-pixel stepper still runs whenever
a **stage actor** drives it: a conveyor keeps carrying it (23417 sets +46 to the
belt dir before calling `sub_41EC84`, whose body is gated on `+46 != -1` at
22572), the belt-forced kick still probes, and a warphole/trampoline step-on
still fires. An off-belt player simply stands; Bomberman has no coasting
momentum.

Both counters decrement independently and unconditionally every alive tick
(22982-90 for +58; the state-4 anim-frame counter for pickup-pause). So:
decrement both and fall through, but force the resolved input to neutral
(`want_godir = -1`) and skip the bomb-action block if **either** is active —
exactly a skipped `sub_41E61E`, which leaves +46 = -1 and the reset key bytes 0
so no edge-gated action can fire.

**Different cadences.** +58 decrements once per displayed frame (22982-22984 runs
at the top of every `sub_41F29B` call, before the state dispatch), so it lives in
the sub-frame loop and in the flight branches, whose frames still execute 22982:
a 16-frame stun lasts ~89 ms at the canonical ~180 fps, not 800 ms.
`pickup_pause` mirrors the state-4 +80 window, which advances on the 50 ms
ms-accumulator like every anim counter — per tick — and it blocks the whole tick
it is decremented on, so the movement gate reads the **pre-decrement** value.

**But the gate itself is per frame** (0x41FA42 sits in the per-frame acquisition
block), and now that the bomb-action tail is per frame too, that bites: a grab
taken on sub-frame *f* arms the pause and the original blocks input from frame
*f+1*, whereas a tick-granular snapshot leaves the rest of the tick unblocked —
long enough for the AI's behaviour 0 to reach its carrying branch on the very
next frame and throw the bomb it just picked up. Hence `paused_now()` is
recomputed per sub-frame as `paused_entering || pickup_pause > 0`: the snapshot
keeps an already-running pause covering the whole tick it is decremented on, and
the live field catches one armed mid-tick.

## §2 The bomb-action tail (23277-23380)

Four blocks, in the original's exact order: auto-drop force (diarrhea +135 /
super +137) → carried-bomb throw (+37) → action2 edge (kick-stop +89 / punch +91
/ trigger +95) → drop edge (grab +92 / spooge +93 / plain drop), gated `!+134`.

The tail runs on **every alive frame regardless of the +78 state** (facts.md
"Player state machine (+78) — COMPLETE" and "Diarrhea/super auto-drop x
grab-glove"). State 5 (bounce) reaches it via an explicit jump straight into it
(23198); states 6/7 (warp out/in) and 20-39 fall through the state-dispatch join
just above it; and nothing in the new-input gate touches the tail itself — that
gate only guards the earlier acquisition call that would set the raw key bytes
+56/+57 from the controller.

`blocked` mirrors that. While blocked, this frame's effective key bytes start at
their per-frame reset of 0 (22976-22979, which the original runs unconditionally
every alive frame, so a fresh edge never materialises while acquisition is
skipped) — unless the auto-drop disease force overrides +56 = 1, and that
override lives *inside* the tail, so it fires regardless of `blocked`.
Consequences, both intended: a carried bomb is released on the very first blocked
frame (the throw's "key not down" test on +56 passes immediately — the
release-throw a user triggers by walking into a head-stun while carrying), and a
diarrhea/super auto-drop keeps cycling grab/throw/drop straight through a stun or
a bounce/warp flight, while a genuine new manual action cannot fire, its edge
needing +56 or +57 actually freshly down.

**`pickup_pause` is deliberately not modelled by `blocked`.** For that state the
original forces +56 = 1 *sustained* rather than leaving it at 0 (23017-23025) — a
materially different "held" rule from `blocked`'s zero-default. Every call site
therefore **skips the tail entirely** while `paused_now()`, and only routes
through it for `stun > 0` / bounce / warp.

`prev_action1`/`prev_action2` (the original's +54/+55) are latched to the
**effective** key values just used — post auto-drop-force, post blocked-zeroing —
not the raw controller input, mirroring the original's literal copy of +56 into
+54 at the top of the next frame. Latching the raw sample instead only matched
whenever auto-drop was inactive, and can diverge on the tick a disease is cured
with an un-pressed button (no scenario exercises it yet; see facts.md).

## §3 The tail is per FRAME, not per tick

It used to run once per tick, after the sub-frame loop. That was wrong in two
ways, and the second is not cosmetic (facts.md "The bomb-action tail is
per-FRAME"):

- its **position-dependent** tests — the grab/spooge "own bomb underfoot" probe
  and the drop tile — read the end-of-tick position, up to `kSubFrames - 1`
  frames after the frame whose decision set the key. An AI that won behaviour 0's
  grab roll early in a tick and stepped off the tile on a later frame found no
  bomb underfoot at tail time and the grab was silently discarded (**measured**:
  ~6 grabs per 7 drops instead of essentially all of them);
- the auto-drop diseases force their own edge *inside* the tail, so the original
  re-attempts a drop every frame, not once per tick.

Running the tail inside the loop closes both. Edge-gating still means a held
human key drops exactly once per press: frame 0 sees the edge and latches +54,
the remaining frames see none.

## §4 Flights: trampoline (state 5) and warp (states 6/7)

Both are state-gated flights (`sub_41F29B` state dispatch / `sub_41DE63`):
movement input is ignored until the flight finishes and the player cannot be
pushed. `tick_bounce` counts the hop down and, at the apex, teleports the player
to a random nearby open tile — it is a "fly + random land", not an in-place
bounce (`docs/re/stage-actors.md` §4). The apex relocation **draws RNG**, so it
must run inside the state gate, before any other per-tick draw. A warp likewise
relocates at the out→in midpoint over 18 ticks and is invulnerable throughout
(§5 of the same doc); the prior instantaneous teleport was the "stuck on entering
a warp" report.

Movement and input are fully skipped for the whole flight, but the bomb-action
tail is not (§2) — and it runs **once per frame** of the flight, so an auto-drop
disease cycles at frame density in the air exactly as on the ground. Because
input is fully blocked the whole time, the throw's "+56 clear" test passes from
the very first frame: a player who enters a flight *while carrying* has the bomb
thrown at their current tile almost immediately rather than holding it through
the flight.

If a flight is entered mid pickup-pause (a conveyor-carried grab pushed onto a
trampoline/warphole — vanishingly rare, no current scenario reaches it) the tail
stays fully skipped per §2's carve-out, so the narrow release-on-entry guard is
kept for exactly that case; otherwise a carried bomb would ride the flight
untouched.

22982 (`--+58`) and the ice block (23058-23078) both sit **above** the state
dispatch, so a flight frame still burns stun *and* pushes this frame's -1 godir
into the ice buffer — landing on an icy level replays neutral input, not a stale
pre-flight direction burst (facts.md "Ice / input-lag"; flight-push fix
2026-07-12).

The two flights run the identical frame body and differ only in which per-phase
timer they tick down, so they share one body rather than two byte-identical
copies.

## §5 The trigger frame already burns a state frame

`sub_41F29B` runs the mover — and therefore `sub_41EC84`'s step-on trigger —
**before** the animation/state dispatch (the mover's exit is a jump to the
dispatch head at LABEL_155), so the frame that sets state 5/6/7 falls straight
into the matching state block and its per-phase frame counter (+80) already reads
1 by the end of it. The port's flight gate sits at the *top* of `player_turn`, so
without an explicit post-loop tick the hop/warp would land one tick late and last
one tick longer than the original's. The state block runs before the tail
(LABEL_246), and the port matches.

There is deliberately **no** post-tick "player is standing on one" step-on check.
The original's only trigger site is the mover's -1 test; a standing-on fallback
let this port take players it should not (facts.md "Warphole/trampoline entry
predicate").

## §6 Retraction — "+46 is sticky / never reset" was a false positive

Rebuilding `want_godir` as -1 fresh each sub-frame is faithful. `sub_41F29B`
resets the native input direction to -1 **unconditionally** at the top of every
per-frame pass (`batch_0x41F29B.cpp:329` — the write is indexed as word 23 of the
player record, i.e. byte offset +46) *before* the input gate re-writes it. A
human holding no key, or an AI whose behaviour chain commits no direction, is
left at -1 and stops (the movement gate at :394 requires `+46 != -1`).

The W3-A audit's claim that +46 is never reset was a **false positive**
(2026-07-22): its grep for a byte-offset write to +46 missed this index-notation
reset. The port's stop-when-the-chain-writes-nothing is exact, not a divergence.

## §7 Round-start input freeze

`dword_4621E0`. `sub_41F29B`'s acquisition gate at 23028 demands the new-input
flag be set **and** `dword_4621E0` be zero. While it runs, the AI brain and the
human input read are both skipped — the same slot as the stun gate, but without
consuming stun (the countdowns are independent). Stage actors, the ice-buffer
flow and the bomb-action tail all run normally underneath it, with the tail's
inputs dead so only auto-drop can act.

The counter is armed to `50 ms x getvalue(30)` = 1000 ms by round init
(`sub_4214BC`) and decremented by the measured frame delta at the top of the
player-pass entry `sub_420F07` (pseudo.c 23642-23645). The original's gate opens
on the frame at `t >= 1000 ms` — exactly 1000 ms of dead input. At tick
granularity that boundary needs the decrement **after** the pass (ticks 0..19
read 20..1 and stay frozen; tick 20 reads 0); decrementing before would cut the
window one tick short. facts.md "Round-start input freeze".

## §8 The PlayerWalking event's unit

The pose/leg-cycle keys off the walking **dispatch**, not off displacement, so
the event carries the tick's summed per-frame budget accruals (event.hpp).
Events are unhashed derived outputs — no golden impact.

The deterministic path (`n_sub == kSubFrames`) emits whole px: the summed
9-sub-frame budget is several px, the int8 truncation is negligible, and the
renderer's `/3` leg divisor is unchanged. The F9 per-frame path (`n_sub == 1`)
emits a **sub-pixel** budget every frame; truncated to whole px it clamps up to
1, inflating the leg cycle ~3x — the reported "walk too fast". That path emits
1/16-px units instead (no clamp-up) and the renderer's native-cadence divisor
(`/48 = /3 * 16`) undoes the scale, so walk speed matches the tick path and is
frame-rate-independent, like the original's fixed-point per-frame leg phase.
