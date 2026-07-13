# ADR-0006: Canonical frame cadence — 60 fps sub-frames inside the 20 Hz tick

**Status:** Accepted
**Date:** 2026-07-12
**Deciders:** Ege

## Context

The original's gameplay driver (`sub_42A191`) runs once per DISPLAYED frame
with the measured integer-ms wall-clock delta — 60-70 fps on period hardware.
Its timers and animations quantize back to 50 ms through per-entity ms
accumulators (the 20 Hz layer ADR-0003 modelled correctly), but input
acquisition, the AI brain, the head-stun countdown, and every movement-budget
accrual (`speed × frameDelta / 50` — players, rovers, sliding bombs) genuinely
run at display rate. Consequences the 20 Hz-only port missed: the AI decides
3× more often (the original's "frantic" feel), the stock walker moves at
921/100 px per 50 ms (integer truncation), a head stun lasts ~267 ms rather
than 800 ms, and rovers get their flat +100/frame budget three times per
50 ms. Live A/B against the original running natively confirmed all of this
as perceptible. Full details and citations: `docs/re/facts.md` "Canonical
frame cadence".

A deterministic sim cannot consume measured deltas, and the original is
mildly frame-rate-dependent by construction (60 vs 70 fps machines genuinely
played differently), so bit-parity with "the" original does not exist — a
canonical rate must be pinned.

## Decision

- **Canonical display rate = 60 fps**, expressed as the fixed repeating
  integer-ms delta pattern `{17, 17, 16}` (sums to one 50 ms tick;
  `constants.hpp kSubFrames`/`kSubFrameMs`/`frame_budget`). 60 is what the
  reference Win11 install presents at and the closest round rate to period
  hardware.
- The **tick stays 20 Hz** and remains the determinism/replay/netplay unit
  (ADR-0003 unchanged): `Simulation::tick(inputs)` is still the only entry
  point, and the 50 ms-quantized counters (fuses, flames, diseases, anims)
  keep their tick units.
- Inside `player_turn`, the per-frame mechanics run as **three deterministic
  sub-frames per tick**: input decode, AI decide (with its RNG draws and
  ms-accrued pursuit timers), ice-buffer push, budget accrual, per-pixel
  mover, head-stun decrement. Rover and belt budgets accrue per frame with
  the same deltas (folded into one pass where provably equivalent).
- Deliberately tick-quantized leftovers (≤50 ms phase each, documented in the
  facts entry): the edge-gated bomb-action tail, human direction sampling
  (one sample per tick), bomb slide/fly single-pass integration, and
  entity-serialized interleaving within the tick.

## Options Considered

**Sub-frames inside the 20 Hz tick (chosen)** — restores everything
perceptible (AI temperature, movement arithmetic, stun/rover timing) with a
contained diff; the 50 ms counter layer, tuning constants, and tick contract
survive untouched.
**Full 60 Hz tick rebase** — philosophically purest, but the original's own
timers are 50 ms-quantized anyway, so it re-labels the same mechanics at ×3
constants while churning every system, test, and golden; the only extra
fidelity is sub-tick cross-entity phase (<50 ms), invisible in play.
**AI-only 3× decide** — cheapest, but movement would not follow each
decision, so it is not the original's mechanic at all.

## Consequences

- Easier: the port now matches the original's feel (verified by A/B); future
  per-frame findings have a natural slot (a sub-frame) to land in.
- Harder: "per tick" vs "per frame" must be pinned per mechanic when porting
  (the facts entry's two-clock model is the checklist); golden B/C/D/E and
  the visual goldens were recaptured once for the cadence switch.
- Revisit: per-sub-frame HUMAN direction sampling (needs a TickInputs
  contract extension and shell plumbing); the [VERIFY] on the disease-factor
  × delta truncation order.
