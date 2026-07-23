# ADR-0008: Modernization — package structure, abstractions, metrics

**Status:** Accepted — in progress (staged, golden-verified)
**Date:** 2026-07-23
**Deciders:** Ege

## Context

The clean-room port is now trustworthy (49 golden-pinned suites, oracle
lockstep). Ege wants to modernize the code to explicit engineering standards:
≤300 lines/class, ≤4 params/method, stronger abstractions, a package structure
where a developer can add a *horizontal* feature (screen, powerup, stage actor,
map) with minimal, localized edits. Concrete debts identified together:

- `game_app.cpp` is a **4809-line God object** (menu + match loop + every
  `present_*` screen + options + editor entry + input + window). The one
  genuine SRP violation by any rubric.
- **Engine-base is scattered**: window/render-device setup, frame pacing
  (3 hand-copied loops), event polling (**27** hand-rolled `while (SDL_PollEvent)`
  loops), audio, resources — all live inside `game_app`.
- **831 static_casts**; ~212 are Fixed→float render conversions because `Fixed`
  is a bare `using Fixed = int32_t` typedef.
- `tests/` is flat (49 files in one dir).

## Decision

Modernize on a **two-tier** principle so the standards apply where they help and
the determinism/faithful-port contract (ADR-0003) is never broken:

| Tier | Modernization language | Constraint |
|---|---|---|
| Presentation / app | full OOP — `Screen` interface, `ScreenStack`, small classes, ≤300L/≤4P | none (not hashed) |
| Deterministic sim (`libs/sim`) | data-oriented — feature **tables** + context structs, **no virtuals** | golden byte-identical; `State` stays a plain aggregate |

Virtuals in `State` would break hashing/determinism; faithful-port arithmetic
can't be paraphrased. So sim "abstraction" = registry/table, not interface
hierarchy — the *correct* abstraction for a deterministic sim.

### Target package structure

```
libs/
  core/          value types (Direction, Fixed, ids)
  platform/      ENGINE BASE: Window · FrameClock · InputPump · FrameLoop · ResourceCache
  assets/  sim/  match/
  render/ audio/ input/ ui/     (extracted from game/)
  screens/       Screen interface + one class per screen
  app/           Application · ScreenStack · MatchRunner
apps/game/       thin main → App().run()
tests/           mirrored: tests/{core,platform,assets,sim,match,render,ui,game,screens}/ + helpers/
```

### Key abstractions

- **`Screen`** (presentation, virtual): `handle_event/update/draw`; a `ScreenStack`
  the `FrameLoop` drives — dissolves the `present_*` God methods; `MatchRunner`
  is a Screen that also steps the sim.
- **`TurnContext`/`Systems`** (sim): collapse the 12-param `player_turn` /
  `run_tick` calls to ≤4 by bundling the system references + cadence — a
  reference bundle only, so hashing/determinism is untouched.
- **Feature tables** (sim): `powerups`/`diseases`/`stage_actors` as `constexpr`
  arrays of `{tag, data, free-fn}` — deterministic (fixed order), extensible
  (one entry to add one).

### Metrics policy

≤300L/≤4P enforced in presentation/app/systems. **Documented exception**:
faithful-port functions that mirror one binary `sub_XXXX` (`player_turn`,
`run_tick`) stay whole (splitting fragments the fidelity mapping); their param
count is solved by `TurnContext`, not by splitting.

### Cast-reduction policy

Reduce the *reducible* ~350-400, keep the ~235 hashed-int8 narrowings (explicit
narrowing is correct under `/W4`). Highest-leverage, zero-risk: a render-side
`to_px(Fixed)` / `frect(...)` helper (~200 casts → one home). Optional/later: a
real `struct Fixed` (byte-identical layout → hashable, but golden-sensitive).

## Execution — staged, golden-verified, batched on one branch

All work on `modernize`; every stage keeps 49 tests + golden green.

0. **`platform/FrameClock`** — DONE (this ADR's first commit): the 3 pacing loops
   consolidated; the engine-base layer scaffolded.
1. `platform/InputPump` + `FrameLoop` — the 27 event loops → one engine loop.
2. `Screen` interface + `ScreenStack`; migrate `present_*` to screen classes.
3. Extract `render/ audio/ input/ ui/` modules from `game/`.
4. Sim `TurnContext`/`Systems` + feature tables.
5. Cast reduction (`to_px`/`frect`), dead-code sweep, test restructure.

Parallelism (Ege's request): a big architectural refactor is **coupled** —
every stream edits `game_app.cpp` + CMake — so naive N-way parallel worktrees
merge-conflict. The effective shape is **serial spine + parallel leaves**:
parallel analysis up front → serial module carve → parallel per-module polish
(disjoint files, worktree-isolated) → build/golden verify → single merge.
Realistic speedup ~2-3×, not N×.

See ADR-0003 (determinism), ADR-0006 (cadence), ADR-0007 (native-cadence).
