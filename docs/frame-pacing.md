# Frame pacing: why the uncapped path targets a lattice

Measured evidence behind `libs/platform/include/bomber/platform/frame_pacer.hpp`.
The header carries the two rules and the invariant; the numbers that justify them
live here, because a rationale this long stops being a comment
(`docs/coding-standards.md` §10).

## The two rules

`FramePacer` supplies the pacing DECISION with no clock of its own: the caller
passes the instant it measured after its present and gets back what to wait for.

- **Resync** — the default (vsync) path. The cadence comes from the display; the
  pacer only supplies the block `SDL_RenderPresent` does not reliably do in
  Windows windowed mode. A present that ran past the target means the hardware
  train moved, so the target re-phases onto it and the overrun is deliberately
  dropped — chasing it would fight vblank.
- **SubFrame** — F8's uncapped path. Nothing in hardware paces this one: the
  cadence is the sim's own interpolation quantum. `renderer.cpp` floors
  `interp_alpha * kSubFrames`, so a 50 ms tick has exactly `kSubFrames` DISTINCT
  on-screen positions and a present is only worth making at the instant that
  index changes. The target is therefore not "now + period" but the next point on
  a lattice anchored to the tick clock.

## Why a lattice and not deficit catch-up

Measured 2026-07-28 on the reference Win11 box: 640x480 windowed, direct3d11,
60 Hz panel, 6 s samples of a live match, target 180 Hz.

| rule | presents/s | DISTINCT/s | frame p99 | worst |
|---|---|---|---|---|
| shipped Resync rule | 171.4 | 167.9 | 16.3 ms | 19.5 ms |
| bounded catch-up + spin | 179.5 | 135.5 | 8.5 ms | 22.1 ms |
| SubFrame lattice | 179.3 | 179.3 | 8.5 ms | 11.8 ms |

The middle row is the trap: catch-up reaches the target RATE by re-presenting
sub-frames it had already shown (265 of its 1081 presents were byte-identical
repeats), so the number on the overlay improves and the motion does not. The
lattice presents 179.3 distinct images for 179.3 presents.

The dominant cost being paced around is `SDL_RenderPresent` itself, not the
frame's CPU work: the same rig measured 0.11 ms of work before the present and
0.3 ms (p50) / 4-10 ms (p90) / 11-18 ms (p99) inside it, the swapchain blocking
as it backs up against the 60 Hz display.

## Amendment, 2026-07-29: that present cost was the D3D11 backend

The table above was measured under a backend property, not a property of this
rule. SDL's D3D11 renderer pins `SetMaximumFrameLatency(1)`, so an uncapped
windowed present waits on the flip DWM is still holding at 60 Hz. The GL backend
does not, and `game_app.cpp`'s `init_video` now prefers it on Windows for exactly
that reason.

With GL the same rig holds 180.0 fps at 0.002 ms adjacent-frame jitter and the
two rules become indistinguishable (lattice 180.0 / 0.002 ms, resync 179.7 /
0.156 ms). The lattice is kept because it is still the better of the two and
because its no-duplicate-sub-frame argument stands on its own.

Re-measured across window sizes, roster size, audio on/off and fullscreen, the
frame's own CPU work never left 0.11-0.23 ms — so nothing here should be read as
a claim about how expensive the port's drawing is.

## Sizing `kSpinTailNs`

The tail of a SubFrame wait is spun rather than slept, sized from the measured
error of SDL3's high-resolution waitable-timer sleep on Win11: a 5.556 ms request
returns after 6.008 ms mean (+0.45), and ANY request has a ~0.5 ms floor (a
200 us request takes 0.54 ms). Sleeping the whole gap therefore lands late every
frame.

Spinning the last millisecond also keeps the core out of the deep idle states
that made the present call itself measurably slower (p90 4.2 ms -> 1.5 ms on the
rig above) — which is why the constant is 1 ms rather than a bare 0.5 ms. It
costs at most ~1 ms of one core per frame (~18% of a core at 180 Hz), which the
default vsync path deliberately does not pay: `plan_resync` never asks for a spin.
