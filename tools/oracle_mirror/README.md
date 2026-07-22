# Oracle diff harness — clean-room mirror (M3 Part B)

`mirror.cpp` runs `bomber::sim::Simulation` with a fixed seed + a fixed,
documented scripted per-tick input sequence and prints one canonical **digest
line per tick** to stdout. It is the clean-room half of the M3 oracle diff.

The native port (`native/`, ground truth by construction — it mirrors the
original BM95.EXE arithmetic) must emit the **same digest format** for the
**same seed + same script**. A plain `diff` of the two output files then points
at the first tick where the two implementations diverge = the first mechanic the
clean-room sim gets wrong.

## Digest format (both sides MUST match)

```
t=<tick> P=<n> | <slot>:<tx>,<ty>,<alive>,<bombs> ... | B=<liveBombs>[ bomb:<tx>,<ty>,<moving>]* F=<flameCells>
```

- Only OBSERVABLE gameplay state is compared. **rng is deliberately NOT emitted**
  — the native LCG and the clean-room xorshift32 are different generators.
- Tile coordinates (`tx,ty`) are the robust cross-representation field; raw pixel
  / fixed-point positions differ in layout between the two ports.
- `bombs` = live bombs a player owns; `B` = total live bombs; `F` = flame cells.
- After `B=`, one ` bomb:<tx>,<ty>,<moving>` token per live bomb, in creation
  order (native: bomb slots 0..99; mirror: `s.bombs` vector). `moving` = the
  kicked/redirected SLIDE state (native motion word +46==1; mirror `Bomb::moving`);
  a resting, belt-carried, or flying bomb reads 0 on both sides. Bomb POSITIONS
  are the pre-explosion field that validates bombs F2 (kicked/conveyor speed) and
  F3 (coast-stop) — unaffected by the known native flame gap / tick-rotation until
  a bomb explodes or stops.

## Scenarios

- **base** (default): scripted human slot 0 sweeps + drops a bomb (see below).
- **kick** (`--scenario=kick` mirror / `--oracle <out> <ticks> kick` native):
  idle players; one bomb injected at tile (7,0) already kicked +x toward the
  col-14 wall with a huge fuse. Validates F3 (both sides slide to col 14 and
  STOP, `moving`->0, B stays 1) and the F2 kicked-speed comparison. This
  scenario is what CAUGHT the F2 false positive: on 2026-07-20 it showed a
  ~1.9x speed DIVERGENCE (mirror faster) because the clean-room had folded a
  `+100*kSubFrames` "ground bonus" that the native slide loop cancels with a
  paired per-frame position backoff (the native slide is base-speed and
  cadence-invariant — same ~0.25 tile/tick at 1x and 9x). RESOLVED 2026-07-20:
  the +100 fold was reverted (see `audit/bombs.md` Finding 2); the mirror now
  matches the native kicked/belt speed.
- **conveyor** (`--scenario=conveyor` mirror / `--oracle <out> <ticks> conveyor`
  native): idle players; a short EAST belt on tiles (2,0)+(3,0) with open floor
  at (4,0)+, and a RESTING bomb (motion 0, NOT kicked) on the belt's first tile.
  Native injects the belt as two real stage-actor records (`sub_404E3C` alloc +
  the `sub_404E99` conveyor fill: present=1 @+0, type=2 @+4, godir @+44, tile
  x/y @+28/+32) and the bomb via `sub_422EDE` (no kick). Validates **F2 belt
  speed** (the reverted base `conveyor_speed()`, no +100) and **F3 belt-exit
  freeze** (the bomb stops the instant it steps off the belt, never coasting).
  Result 2026-07-21: native and mirror agree tile-for-tile across all 41 ticks
  EXCEPT a single 1-tick lag at the second tile boundary (native reaches (4,0)
  at t17, mirror at t18) — the known tick-rotation offset (native frame-start
  leads), NOT a speed error: it does NOT compound (the first crossing at t6 is
  identical), whereas a residual +100 would race the mirror progressively ahead.
  Both freeze permanently at (4,0). F2 belt + F3 CONFIRMED against the native.
- **jelly** (`--scenario=jelly` / `--oracle <out> <ticks> jelly`): the kick
  scenario with a KIND-2 (jelly) bomb. It slides to the col-14 wall and REVERSES
  (ping-pongs back) instead of stopping (`sub_42331C` case 1 jelly branch).
  Result 2026-07-22: native and mirror agree tile-for-tile on the way out
  (7,0)->(14,0) and ping-pong back to (6,0); only the known ~1-tick rotation
  offset appears on the return leg. Jelly bounce + ping-pong speed CONFIRMED.
- **flame** (`--scenario=flame` / `--oracle <out> <ticks> flame`): injects a
  flame-2 bomb at interior tile (6,4) and explodes it, comparing the flame cross
  (F count + cell tiles). Mirror casts the correct cross F=9. NATIVE-BLOCKED as
  of 2026-07-22: the native transliteration casts a DIAGONAL (cells (2,2)..(6,6),
  centre (6,4) unlit) — a native-port geometry bug in the flame cast, NOT a
  clean-room bug. Scenario is ready for when the native cast is fixed.

## Scripted input (reproduce identically on the native side)

Pure function of the tick index (`scripted_inputs` in `mirror.cpp`):
- slot 0 (scripted "human"): `right` for 20 ticks then `left` for 20 (period 40);
  `action1` (drop bomb) on every tick where `t % 25 == 24`.
- slots 1..N-1: AI.

Setup: classic 15x11 open arena (odd/odd Solid pillars), corner spawns,
`tuning.input_freeze_ticks = 0`, `spawn_counts = 0` (no hidden powerups).

## Build & run

```
cmake -S tools/oracle_mirror -B build/oracle_mirror
cmake --build build/oracle_mirror --config Debug
build/oracle_mirror/Debug/mirror --ticks=80 --players=2 > cleanroom.txt
# native (ground truth); CLI is positional: --oracle <out.txt> <ticks> [scenario]
native/build32/Debug/bm_native.exe --oracle native.txt 80
diff native.txt cleanroom.txt | head
# kick scenario:
build/oracle_mirror/Debug/mirror --ticks=40 --players=2 --scenario=kick > ck.txt
native/build32/Debug/bm_native.exe --oracle cn.txt 40 kick
diff cn.txt ck.txt | head
```

## Status

BOTH sides are built and diffed. `native --oracle` sets up the same match
directly and emits the same format; `sub_41F29B` (players) is ported. Current
findings (see `native/docs/M3_NOTES.md`):
- **base**: ticks 0-62 BYTE-IDENTICAL; first divergence t=63 is two documented
  non-bugs (tick-rotation offset + native flame-spread gap).
- **kick**: F3 coast-stop VALIDATED (both stop at the wall); bomb persistence
  (B=1) OK; F2 kicked-SPEED shows a ~1.9x divergence (mirror faster) — an open
  finding for RE adjudication (session 4 notes), not a resolved bug.
