# ADR-0004: Front-end as an SDL-free screen/state machine

**Status:** Accepted
**Date:** 2026-07-04
**Deciders:** Ege

## Context

Phase 3 opens the front-end (`docs/ROADMAP.md`). Today `bomber_game` boots
straight into a match (`GameApp::run` → `start_match` → `run_interactive`);
there is no title, no menu, no results screen. The original game (RE'd in
`docs/re/frontend-flow.md`) boots **IPLOGO → HSLOGO → TITLE → main-menu loop**,
where every full-screen image goes through one primitive `sub_42A088(name,
wait)` — a name-derived palette + image that waits for a keypress or the
`getvalue(12)` attract timeout — and screen changes are covered by
`HEADWIPE.ANI` played on the standard `counter % statecnt` ANI pacer. Crucially
the whole front-end is PCX + ANI + RSS + `.BM` — **all formats the pipeline
already parses** (no FMV ships) — and the RSS tracks are just SOUNDLST ids the
existing `AudioEngine` already plays (title 1000, menu 1010, menuexit 10).

Several later agents will build the *polished* individual screens (real options
UI, campaign, network, credits) in parallel. They need a shared, minimal,
extensible spine to plug into — not a finished menu system, and not something
that risks the determinism contract (ADR-0003). The presentation layer lives in
`libs/game`; `libs/sim` is SDL-free and must not be touched by any of this.

## Decision

Model the application as a **screen/state machine with an SDL-free logic core**,
layered exactly like the sim's testable-core ethos:

1. **Pure transition core (SDL-free, header-only, doctestable).** An `AppState`
   enum (`Boot, Logo, Title, Menu, Match, Results, Quit`) plus a pure step
   function `next(state, AppInput) -> AppState` over a tiny event alphabet
   (`Advance` = key/timeout accept, `Back` = escape, `MatchOver`, `Quit`). No
   SDL, no globals; it is unit-tested without a window
   (`libs/game/include/bomber/game/app_flow.hpp`, `tests/test_frontend.cpp`).
   This mirrors the RE'd top-level flow (`sub_42B060` → `sub_42B9CE`).

2. **A generic asset-driven `Screen` primitive.** Data, not code: a `ScreenDef`
   = `{ background PCX name, optional overlay ANI sequences, music SOUNDLST id,
   dwell ticks, skippable, next AppState }`. On enter it blits the full-screen
   PCX, starts the music track, advances overlay ANIs by a frame counter, and
   reports `Advance` on keypress **or** dwell timeout — a direct port of
   `sub_42A088`'s "keypress OR getvalue(12)" wait. Missing art/audio logs and is
   skipped (never aborts). `libs/game/.../screen.hpp` + `src/screen.cpp`.

3. **A generic transition primitive.** `HEADWIPE.ANI` played as an overlay wipe,
   its step chosen `counter % statecnt` (the confirmed universal ANI driver,
   `sub_41DAA7`); a wall-clock alpha fade is the fallback when the ANI is
   absent. `libs/game/.../transition.hpp` + `src/transition.cpp`.

4. **`GameApp` drives the machine around the existing match loop.** `run()`
   walks Boot→Logo→Title→Menu, hands the existing `start_match`/tick loop the
   `Match` state, then Results, then back to Menu — instead of jumping straight
   into a match. A `BOMBER_BOOT_MATCH` env (and `--match`) still boots straight
   to a match for fast iteration.

The Menu and Results *screens are stubs* for now (placeholder backgrounds that
accept a key); the point is a working spine — see logos, hear the title sting,
land on a menu, play, return — with clean seams for the polished screens.

### Why presentation-only (no determinism impact)

The state machine, screens, and transitions live entirely in `libs/game` and
run on the **wall clock / frame time**. Any front-end randomness (attract
variety, wipe jitter) uses a presentation-side LCG (as `Renderer::panic_lcg_` /
`flash_lcg_` do), **never `State::rng`**. `libs/sim` is untouched: no gameplay
field, no hash input, no RNG draw changes — the golden hashes
(`tests/test_golden.cpp`) are unaffected by construction.

## Options Considered

**Screen/state machine, SDL-free core (chosen)** — matches the original's
one-primitive design and the repo's testable-core pattern; the pure `next()` is
doctested; polished screens slot in as new `ScreenDef`s / states without
touching the core or the sim.

**Ad-hoc `if`-ladder inside `GameApp::run_interactive`** — fastest to hack, but
untestable, entangles menu logic with the render loop, and gives parallel
agents no clean seam. Rejected.

**Full menu framework now (widgets, focus, layout)** — out of scope for a
spine and would collide with the parallel screen work. Deferred to the
individual-screen agents.

## Consequences

- Easier: each polished screen is an isolated task (a `ScreenDef` + an
  `AppState` handler); the flow is unit-testable; boot/attract behaviour is
  faithful to the RE.
- Harder: the spine must stay minimal — resist growing it into the menu
  framework; that belongs in the per-screen work.
- Determinism: none. `libs/sim` unchanged; golden suite unaffected.

## Extension points (for the parallel screen agents)

1. **Add a state**: extend the `AppState` enum + the `next()` switch, add a
   doctest edge in `tests/test_frontend.cpp`.
2. **Add a screen**: author a `ScreenDef` (bg / overlays / music id / dwell /
   next) and register it; no core change needed for a plain image screen.
3. **Replace a stub**: swap the Menu / Results placeholder for a real
   interactive screen behind the same `AppState` — the transition and audio
   plumbing already exist.
4. **`.BM` text screens** (options/credits/manual) need a new
   `libs/assets` parser for the `<IMGxxx>`-tagged ASCII markup — flagged as the
   one unbuilt front-end format (see `docs/re/frontend-flow.md`).

## Action Items

1. [x] SDL-free `app_flow.hpp` core + `tests/test_frontend.cpp`.
2. [x] `Screen` + `Transition` primitives; front-end PCX / HEADWIPE loaders.
3. [x] Wire Boot→Logo→Title→Menu(stub)→Match→Results(stub)→Menu in `GameApp`.
4. [ ] Parallel agents: polished Title/attract, Menu, Results, `.BM` viewer.
