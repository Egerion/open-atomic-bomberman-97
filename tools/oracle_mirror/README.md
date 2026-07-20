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
t=<tick> P=<n> | <slot>:<tx>,<ty>,<alive>,<bombs> ... | B=<liveBombs> F=<flameCells>
```

- Only OBSERVABLE gameplay state is compared. **rng is deliberately NOT emitted**
  — the native LCG and the clean-room xorshift32 are different generators.
- Tile coordinates (`tx,ty`) are the robust cross-representation field; raw pixel
  / fixed-point positions differ in layout between the two ports.
- `bombs` = live bombs a player owns; `B` = total live bombs; `F` = flame cells.

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
cmake --build build/oracle_mirror
build/oracle_mirror/mirror --seed=7 --ticks=200 > cleanroom.txt
```

Then, once the native oracle exists (`bm_native --oracle --seed=7 --ticks=200 >
native.txt`), `diff native.txt cleanroom.txt | head` gives the first divergence.

## Status

The clean-room mirror is DONE and validated (compiles, runs, emits the format).
The **native oracle side is NOT yet built** — it is blocked on porting the
match-core stub `sub_41F29B` (players) and driving the native match with a fixed
seed + this script. See `native/docs/M3_NOTES.md` (Part B section) for exactly
where the native side stands.
