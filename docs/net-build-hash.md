# `build_hash` — what each scenario covers, and what it is measured to catch

`libs/net/src/build_hash.cpp` computes the cross-build compatibility digest two
online peers compare before a match (`CLAUDE.md` determinism rule 7, ADR-0011).
Its job is to stop two peers whose **simulations disagree** from ever reaching
tick 0 together: any behaviour change shifts the digest, the lobby door refuses
the mismatch, and nobody discovers the divergence halfway through a match.

**It is only as good as what the scenarios actually EXECUTE.** This page is the
evidence register for that claim — every "this scenario discriminates X" below
was produced by building the digest with the fix reverted and confirming it
moved. Coverage by coincidence is not coverage.

## Why one scenario per mechanic class

Measured, not theorised. A single 60-tick full-clock run never armed the
enclosure and had no AI seat, so **two real behaviour fixes** — facts.md's "AI
never bombs a warphole", and the enclosure arm sweep that clears
warpholes/trampolines — both left the digest **byte-identical**. A peer on the
old build was still admitted and only diverged mid-match.

Widening that single scenario made it worse in a new way: shortening the clock
so the walls arm also crushed the AI seat before it could act, so the AI path
went dark again. The settings that reach one mechanic suppress another. Hence
one scenario per mechanic class, each free to pick settings that suit it.

When you add a system to `libs/sim`, add or extend a scenario **and check it**:
build the digest before and after and confirm it moved.

## The scenarios

### 1. Core

Movement, bombs, flames, brick destruction, powerup pickups. Four seats on the
`pillar_arena` board, 60 ticks of canned input.

### 2. Enclosure, stage actors, round end

A deliberately **short** match clock (`game_seconds = 8`), so the run reaches the
hurry window: the walls arm, the arm sweep clears the actor types it clears
(warphole and trampoline, but *not* conveyor or dirarrow), players get crushed,
and the round-end freeze engages. None of that is reachable on a full clock,
which is why it went untested.

### 3. The AI brain

A **full** clock on purpose: the enclosure must not arm here, or it crushes the
AI seat before the brain has done anything worth hashing. (Not hypothetical —
that is what a combined scenario actually did.) The AI seat spawns *on* a
warphole whose destination is a second warphole.

**Known gap, still open for this scenario**: it does *not* discriminate the "AI
never bombs a warphole" fix — the digest is byte-identical with and without it.
Seating the AI on a warphole was not enough; the likely reason is that a player
on a warphole spends its time in the warp movement states rather than deciding to
drop, so the guarded branch is never reached. That diagnosis is corroborated by
scenario 5, which covers the brain by seating the AI on plain floor and
discriminates its fix immediately. AI *decisions* are therefore no longer
uncovered wholesale — but the warphole drop-refusal specifically still is, and
closing it wants the same treatment (drive an AI onto a warphole with a reason to
drop).

### 4. Stage actors, driven

Scenarios 2 and 3 both **place** warpholes and trampolines and neither
discriminates a change to them: #2's four seats walk to the centre and never
reach the actor tiles, #3's AI seat sits in the warp states rather than deciding
anything. Measured: the "trigger is the −1 APPROACH, not arrival" fix plus the
removal of the `tramp_latch`/`warp_latch` player fields left the digest
**byte-identical across all three**, so a peer without that fix was still
admitted and would desync on any board carrying an actor — which every stock
warphole map does.

This scenario walks a player *onto* a trampoline and *through* a warphole whose
exit is itself a warphole (the ping-pong case the removed latch used to
suppress). Seat 0 walks, then **stops and idles**, then walks again: the idle is
the point, because the trigger predicate and the re-entry guard only diverge for
a player that comes to rest on an actor tile.

### 5. The AI's key presses — the glove path

Built against the decision path itself. The AI seat is **born holding** the grab
and punch gloves, so a run drives behaviour 3's drop, then behaviour 0's grab of
the bomb it is standing on, then behaviour 0's carrying release (the throw), then
behaviour 1's punch — every one of the four AI key-write sites, each of which
manufactures its own input edge (facts.md "AI key presses manufacture their own
edge").

Every constant was **measured**, because getting a brain to exercise a branch is
exactly what this file's history says goes wrong. The obvious version — spare
bomb, one brick, 300 ticks — reaches only two drops and grabs on neither, because
the AI leaves its own bomb's tile before the once-per-tick action tail evaluates.
The **brick pocket** fixes that: it keeps behaviour 3 supplied with adjacent
targets and, with the spare bomb, keeps the AI penned close enough to still be on
the bomb at tail time. As tuned the run makes 3 drops, 3 grabs and 3 throws, the
first grab at tick 10. (It was 4/3/3 plus a punch, first grab at tick 11, before
the per-frame bomb-action tail of 2026-07-30 changed the trajectory — a grab now
lands on the frame that decided it, so the AI is elsewhere by the tick's end.)

If you edit this scenario, **re-measure those**: `tests/sim/test_ai.cpp`
"build_hash scenario 5 really drives the glove path" replicates the board and
asserts the grab, so it fails loudly if a future edit makes the brain idle.

Verified three times over:

| reverted fix | scenario hash | digest |
|---|---|---|
| the 2026-07-28 edge fix, with this scenario present | — | 1599681701 → 150405641 |
| the same revert with only scenarios 1–4 | — | byte-identical at 3780851729 |
| the per-frame bomb-action tail (against 977392888) | → 7293458409330077548 | → 3366107864 (scenario 3 moves with it) |
| the grab pause's `getvalue(665)+1` window | → 12594943270607026772 | → 4051077992 (this scenario ALONE) |

Scenarios 1, 2, 4 and 6 are byte-identical under both of the last two reverts.

**Do not fold this into scenario 3**: a full clock and a spawn *off* an actor
tile are both load-bearing (the warp states and the wall crush each starve the
brain in their own way).

### 6. Diseases — infection, contagion and expiry

Scenarios 1–5 leave `Tuning::diseases_time_limited` at its default `true`, and
until 2026-07-30 the port gated disease expiry on that flag — so the whole
"VALUELST id 121 is dead in the original, stop consuming it" fix was invisible to
the digest: every scenario took the same branch either way. **A default is not
coverage.** This scenario turns the flag off, the only setting the fix changes
anything for, and the setting a scheme authored with `121,0` hands a peer.

It needs its own board and its own inputs, both arrived at by measurement. The
obvious version — `pillar_arena` plus the shared `canned_inputs` — reaches
**zero** infections in 400 ticks: those inputs walk the four seats into each
other's bombs, and three are dead by tick 100 with the skulls still under
unbroken bricks. The corridor board instead guarantees the pickup: every brick in
the wall hides a skull (Disease is the only kind with a nonzero count, and a
positive count places unconditionally), and seat 0 has to blast through the wall
to continue.

Measured: with the id-121 gate restored the scenario hash changes
(10041317218023902939 → 2524117031108598899) and the digest with it (2695214498 →
977392888), so a peer still honouring the flag is refused at the door — nothing
else in scenarios 1–5 moves. As tuned the run infects seat 0 at tick 129 and
passes it to seat 1 on the way back; on a time-limited build both diseases have
expired by tick 400, on a gate-honouring one both still read the full duration.

## Folding

Order matters and is part of the digest: **append** new scenarios, never
reorder, or every existing build looks incompatible for no reason. The 64-bit
scenario hashes are folded pairwise, reduced to 32 bits, and mixed with
`kWireProtocolVersion`.
