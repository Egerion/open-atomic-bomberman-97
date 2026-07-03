# ADR-0003: Deterministic fixed-timestep simulation; netplay deferred

**Status:** Accepted
**Date:** 2026-07-02
**Deciders:** Ege

## Context

Netplay is explicitly out of scope for now (decision: "defer"). But retrofitting networking into a non-deterministic sim is a rewrite, not a feature. The original game's own data pushes toward determinism anyway: `VALUELST.RES` expresses speeds as integer hundredths-of-a-pixel **per frame**, i.e. the 1997 engine itself ran a fixed-cadence integer simulation.

## Decision

Structure the game as a **deterministic, fixed-timestep simulation** from day one, without writing any netcode:

- Sim state advances only in `sim::tick(state, inputs)` — pure function of current state + per-player input commands
- Integer/fixed-point math in sim (hundredths-of-pixel units, matching VALUELST); no floats in game logic
- Seeded PRNG owned by sim state (powerup drops, roulette, AI dice)
- Rendering interpolates/reads sim state; never mutates it. Wall-clock, audio, UI live outside the sim
- Tick rate: **20 Hz** — resolved 2026-07-02: VALUELST ids 25/30 define a nominal/target frame rate of 20 fps, and every frame-count tuning value (fuse 40 = 2 s, disease 300 = 15 s) confirms it

Netcode (lockstep would fit this shape naturally) is a future ADR.

## Options Considered

**Deterministic fixed-timestep (chosen)** — costs discipline, not time; enables replays, headless AI tests, and future lockstep netplay for free.
**Variable-timestep / float sim** — marginally quicker to hack up, permanently closes the netplay door and makes original-feel replication (integer px/frame speeds) harder, not easier.

## Consequences

- Easier: replays, deterministic unit tests of game rules, AI development against headless sim, eventual netplay
- Harder: constant vigilance — no `rand()`, no float drift, no reading the clock inside sim
- Revisit: actual tick rate after measuring original; netcode design when netplay becomes in-scope

## Action Items

1. [ ] `sim/` module boundary with tick + input-command types in the skeleton
2. [ ] Determinism test: two sims, same seed+inputs, state hash equal over N ticks
