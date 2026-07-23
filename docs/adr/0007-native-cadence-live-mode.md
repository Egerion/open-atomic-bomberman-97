# ADR-0007: Native-cadence live mode + the Video Settings screen

**Status:** Accepted
**Date:** 2026-07-23
**Deciders:** Ege

## Context

ADR-0006 pins a CANONICAL display rate (~180 fps, nine sub-frames per 20 Hz
tick) so the deterministic sim recovers the original's per-frame movement/AI
granularity. The renderer then interpolates the fixed 20 Hz tick up to the
display refresh. In practice this still felt subtly different from the original
running natively: side-by-side, the original's in-match motion is "creamier"
and its AI more frantic/responsive. The gap is not the sim CONTENT (movement
speed is frame-rate-invariant by the `speed × delta` design, and ADR-0006's
sub-frames already match the AI's decision granularity) — it is the DISPLAY
path. The original free-runs its whole gameplay+render loop per displayed frame
off the measured wall-clock delta (`sub_42A191`), so input→screen latency is
~one frame (~5 ms at ~180 fps). The port's fixed-tick + inter-tick
interpolation renders ~1 tick (up to 50 ms) behind the input it sampled.

Ege asked to reproduce that feel 1:1 and A/B it before committing. Two facts
shaped the decision:

1. The original's real mechanic (consume the wall-clock delta every frame) is
   inherently NON-DETERMINISTIC — the exact per-frame timing varies run to run.
   Even the RE oracle (`bm_native --oracle`) must PIN the delta to 50 ms to be
   reproducible; the whole clean-room validation strategy (golden hashes +
   native-vs-mirror lockstep, ADR-0003) depends on that determinism.
2. The feel Ege was missing was the DISPLAY latency, not the sim — confirmed by
   a pixel/latency A/B. So matching it does not require making the sim itself
   non-deterministic everywhere; only the LIVE render path needs the per-frame
   drive.

## Decision

Add a per-frame, wall-clock-driven LIVE path alongside the deterministic tick,
gated behind an off-by-default toggle — never on the tests/oracle path.

**Sim (`libs/sim`).** `run_tick` is split into phases (`Full` / `Players` /
`Systems`) and `Simulation::frame(inputs, delta_ms)` is added:

- The **movement/AI pass** (`player_turn`) runs ONCE per displayed frame with
  the measured `delta_ms` as a single sub-frame — low latency, fps-scaled
  granularity, exactly the original's per-frame driver.
- The **50 ms-quantized systems** (bombs, flames, fuses, enclosure, diseases,
  the tick counter) drain off an internal real-time accumulator, so those
  timers stay on their native 50 ms grid regardless of fps.
- `Simulation::tick()` (the `Full` phase) is UNCHANGED and stays the ONLY entry
  the tests, golden, and oracle use. All 49 suites remain byte-identical.

**Tick-cadence gating.** Anything that is a per-TICK duration but happens to
live on the per-frame path must not count down ~9× too fast at 180 fps:

- Sim: `pickup_pause`, and the trampoline/warp state timers
  (`tick_bounce`/`tick_warp`) advance only when `player_turn`'s `advance_timers`
  is set — always true on the tick path, true on the live path only on the
  frame that crosses a 50 ms boundary.
- Renderer: the action-pose countdowns (kick/punch/pickup, `on_events`'s
  `tick_advanced`), the cornerhead fidget, the carry arc, and the gold-sparkle
  age all gate on a real tick advance. The walk leg phase, by contrast, DOES
  advance per frame (it is per-frame in the original); its sub-pixel budget is
  carried in 1/16-px units so the speed matches the tick path and is
  fps-independent.
- Everything already keyed to `s.tick` (death animation, bomb pulse, conveyor
  belt, flame/brick aging, disease flash) needs no change — `s.tick` still
  advances at 20 Hz on the live path.

**Presentation.** Movement renders directly (no interpolation) since the sim is
already at render rate; the slower 50 ms-stepped entities (flying/sliding
bombs, rovers) interpolate across their step using the systems accumulator
fraction so they glide instead of stuttering.

**Custom settings — the Video Settings screen.** The three levers are exposed
as a SMALL PORT-ONLY screen (`present_video_settings`, F10 from the menu),
deliberately kept SEPARATE from the RE'd Options screen so the latter stays a
faithful 18-row reproduction (the `no-invented-visuals` rule):

| Setting | Effect | Live key | options.ini key | Default |
|---|---|---|---|---|
| VSync | on = vsync-locked ~60 fps; off = uncapped ~180 fps | F8 | `vsync=` | on |
| Native cadence | the per-frame live path above | F9 | `native_cadence=` | off |
| Show FPS | the corner fps/cadence readout | F7 | `show_fps=` | off |

The keys are PORT-ONLY (no 1997 equivalent), round-tripped through the same
read-modify-write `options.ini` as `fullscreen=` (`libs/assets` `Options`).
Faithful defaults keep a fresh install deterministic and chrome-clean; once set,
the choice persists. The fps readout is drawn via `FontTextures::draw`'s scale
parameter (a self-contained dst-rect scale — never `SDL_SetRenderScale`, which
perturbs the whole render transform).

## Consequences

- **Determinism intact where it matters.** Golden + oracle run on `tick()`
  only; the live path's non-determinism is confined to real-time play, which
  was never hashed. No golden recapture (all 49 tests byte-identical).
- **Live feel matches the original** (Ege: "birebir oldu"): ~1-frame input
  latency, native AI friskiness, correct animation speeds.
- **Off by default.** A default launch is the ADR-0006 deterministic build; the
  live mode is an opt-in the player enables in Video Settings (persisted).
- **The live path is a display/UX lever, not a new source of truth.** It must
  never be used to derive or validate gameplay — that remains `tick()` + the
  oracle. Do not port RE findings against the live path.
- **Not yet fully per-frame.** The systems (esp. kicked-bomb *movement*) still
  step at 50 ms on the live path; bombs/rovers are interpolated to hide it.
  Moving system movement fully per-frame — while keeping fuse/flame timers on
  the 50 ms grid and the tick path deterministic — is possible future work.

## Alternatives considered

- **Literal wall-clock everywhere (no phase split).** Rejected: kills the
  golden concept and the native-vs-mirror oracle — the project's whole
  validation backbone — for a feel gain the confined live path delivers anyway.
- **Raise the fixed tick to 180 Hz.** Rejected: movement/AI already run at that
  granularity (ADR-0006); the change would mainly re-cadence the systems' RNG
  draws (re-opening proven determinism) for little observable gain.
- **Expose the toggles as rows on the RE'd Options screen.** Rejected: that
  screen is a faithful 18-row reproduction; port-only video settings belong in
  a separate screen (`no-invented-visuals`).

See also ADR-0003 (deterministic sim), ADR-0006 (canonical frame cadence),
`docs/re/facts.md` "Canonical frame cadence".
