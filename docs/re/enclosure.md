# Enclosure — the HURRY wall-closing spiral

RE of the wall-closing ("enclosure" / HURRY) mechanic in BM95.EXE (Watcom,
imagebase 0x400000). Distilled here per the RE workflow. Roadmap item #37.

When the match clock runs low, solid wall tiles drop in a clockwise spiral from
the top-left corner inward, crushing whatever they land on. This is the
pressure mechanic that ends drawn-out rounds.

## 1. The stepper — `sub_426818`

`sub_426818` is called once per rendered frame from the in-game main loop. It
owns the whole enclosure: arm/disarm, the preview animation, and the actual
tile drops.

### State (file-scope dwords)

| addr        | as    | meaning                                                   |
|-------------|-------|-----------------------------------------------------------|
| dword_45BE9C | flag | armed (1 = walls closing)                                |
| dword_462230 | int  | current wall tile X (`v11`)                              |
| dword_462234 | int  | current wall tile Y (`v12`)                              |
| dword_462238 | int  | current spiral direction (`v13`), indexes cos/sin below  |
| dword_462240 | int  | current ring depth (`v14`)                               |
| dword_46223C | DWORD| last-drop timestamp, ms (`timeGetTime()`)                |
| dword_462244 | int  | `rand()%3` — which of 3 drop SOUNDS (presentation)       |
| dword_464974 | int  | `getvalue(27)` = enclosement_depth, clamped to getvalue(28) |
| dword_4648AC / dword_4648B4 | int | board width 15 / height 11               |

`dword_45BECC[4] = {0,1,0,-1}` (cos) and `dword_45BEDC[4] = {-1,0,1,0}` (sin),
indexed by direction in **GODIR order** (0=Up,1=Right,2=Down,3=Left). So a step
`(x += cos[dir], y += sin[dir])` walks one tile in that godir.

## 2. Trigger time — `getvalue(101) - 5` seconds remaining  [CONFIRMED 2026-07-04]

```
v1     = getvalue(101);        // hurry_seconds
result = sub_410578();         // = SECONDS REMAINING (dword_4601A4)
if ( result <= v1 - 5 ) {      // remaining <= hurry_seconds - 5
    if ( !dword_45BE9C ) {     // ARM
        dword_462244 = rand() % 3;
        dword_45BE9C = 1;
        dword_46223C = sub_43ACF8();   // = timeGetTime(), the drop clock
        sub_405D0C();          // clear warpholes+trampolines from the actor grid
    }
} else if ( dword_45BE9C ) {   // DISARM (only if time somehow went back up)
    dword_45BE9C = 0;
    dword_462230 = 0; dword_462234 = 0; dword_462240 = 0; dword_462238 = 1;
}
```

- `sub_410578()` returns `dword_4601A4`, set in `sub_4105D2` as
  `(dword_4601AC - dword_4601B8) / 1000` = **whole seconds remaining** (total
  match ms minus elapsed ms, over 1000, clamped ≥ 0). Confirmed also by
  `sub_...` returning `dword_4601A4 <= 0` as the "time up?" predicate.
- So the walls START closing when **remaining ≤ hurry_seconds − 5**, i.e.
  **5 seconds AFTER** the "HURRY!" banner/sound (which shows during the window
  `hurry_seconds − 5 < remaining < hurry_seconds`, a separate check in the HUD
  routine at ~29533, `sub_427961(2700)` + the "hurry" ANI — presentation).
- The disarm branch resets the spiral to **(x=0, y=0, depth=0, dir=1=Right)**.
  Because a real match clock only counts down, disarm never fires mid-round;
  the reset just seeds the spiral origin for the next arm.

## 3. Cadence — 250 ms per wall tile = 5 ticks  [CONFIRMED 2026-07-04]

The drop loop (`LABEL_26`, ~27225):

```
LABEL_26:
  if ( dword_46223C + 250 >= now /*timeGetTime()*/ )  return;  // <250ms since last: wait
  if ( v22-- <= 0 )                                    return;  // cap 5 drops per frame
  dword_46223C += 250;                                          // advance drop clock 250ms
  sub_4278F2(dword_462244 + 140);          // play wall-drop sound (140 + rand%3)
  sub_425E9B(dword_462230, dword_462234, 1);  // DROP the wall at (x,y)
  ...clear player/flame/powerup/bomb on that tile...
  ...advance the spiral to the next (x,y)...  (section 4)
  goto LABEL_26;                            // loop: catch up any further 250ms buckets
```

- **The interval is a HARDCODED 250 ms** (`+= 250`), gated by `timeGetTime()`.
  It is NOT a VALUELST getvalue — the only enclosure getvalues are id 27
  (depth) and id 101 (threshold). `dword_46494C = 1000/getvalue(30) = 1000/20
  = 50` ms/tick, so 250 ms = **exactly 5 ticks** at the locked 20 Hz rate.
- `v22 = 5` caps drops at 5 per frame — a wall-clock catch-up for dropped
  frames. In deterministic lockstep every frame is 50 ms, so at most one 250 ms
  bucket elapses per 5 ticks and the cap never engages: a clean **1 wall / 5
  ticks**.
- The first wall drops 250 ms (5 ticks) AFTER the arm frame: on the arm frame
  `dword_46223C == now`, so `dword_46223C + 250 >= now` is true and the loop
  returns without dropping.
- `dword_462244 + 140` is a SOUND id (three drop-sound variants); `rand()%3`
  is drawn once at arm time, on the presentation stream — the sim draws NO RNG
  for the enclosure.

## 4. Spiral geometry — clockwise from (0,0), `2*depth` rings

Advance after each drop (`LABEL_47`, ~27267):

```
v3 = cos[dir] + x;  v4 = sin[dir] + y;                    // next tile ahead
if ( width-depth > v3 && height-depth > v4 && v3 >= depth && v4 >= depth )
    accept (x,y) = (v3,v4);                               // still inside the ring box
else {
    dir = (dir + 1) & 3;                                  // turn clockwise
    if ( dir == 1 ) {                                     // completed a full loop
        if ( 2*getvalue(27) <= depth ) return;            // reached centre: STOP
        ++depth; ++x; ++y;                                // step inward one ring
    }
    // (x,y) unchanged this step; walk resumes next drop in the new dir
}
```

- Starts at **(0,0)** with **dir = 1 (Right)**, walks the top edge, turns
  clockwise (Right→Down→Left→Up), steps one ring inward each full loop.
- Stops when `depth >= 2*getvalue(27)`, so **rings closed = 2 × enclosement_depth**
  (id 27): depth 1 → 2 rings, depth 2 → 4, depth 3 → 6 (all).
- `sub_425E9B(x,y,1)` sets the tile solid; the surrounding cleanup kills any
  player standing there, detonates/eats a bomb on it (per id 46), and clears
  flame/powerups. (Our `drop_wall` mirrors this; the bomb branch honours
  `wall_detonates` = getvalue(46).)

## 5. Our port (libs/sim EnclosureSystem)

The two moments (§2) are kept SEPARATE, so the presentation (banner + voice)
still fires at moment 1 while the tile drops start at moment 2:

- `enclose_order()` builds the clockwise-from-top-left ring path (matches §4).
- `total(depth)` / `enclose_rings(depth) = 2*depth` (cap 6) matches `2*getvalue(27)`.
- **Banner/sound (moment 1)**: `s.hurry` flips and the `Hurry` EVENT fires at
  `ticks_left <= hurry_seconds * kTicksPerSecond` (unchanged from before — the
  sound_director rides this event, so its timing is preserved). This models the
  HUD `dword_464984` latch, not the enclosure arm.
- **Wall drops (moment 2)**: gated on `ticks_left <= (hurry_seconds - 5) *
  kTicksPerSecond` (was conflated with moment 1 — walls closed 5 s too early).
  `enclose_interval` (0 until armed) doubles as the drop-armed flag.
  `ticks_left/20` is our seconds-remaining; the −5/−0 are whole-second offsets.
  (Floor-division makes the exact tick ±19 vs the original's per-frame ms
  recompute; well within the original's own ±1 s wall-clock rounding.)
- **Cadence**: `enclose_interval = 250 / (1000/kTicksPerSecond) = 5` ticks
  (was a spread-to-fit `hurry_seconds*20 / (n+1)`, wrong). First wall at
  wall-arm + 5 ticks; one wall every 5 ticks thereafter.
- **RNG**: none — the sim enclosure draws no `State::rng` (the `rand()%3` in the
  original is only the drop-sound pick, a presentation concern).

## 6. Determinism / golden impact

The banner and the walls are decoupled, so their hash contributions differ:
- `s.hurry` (hashed) flips at moment 1 — SAME tick as the old code, so the
  `hurry` bool contribution and the `Hurry`-event/sound timing are UNCHANGED.
- `enclose_index` / `enclose_timer` (hashed) and the `cells` the walls solidify
  now start at moment 2 with the 5-tick cadence — so ONLY the wall drops move
  the hash, on scenarios that reach the wall phase within their pinned ticks:

- **golden B** (game_seconds 150 → 3000 ticks, runs 3000): `hurry` still flips
  ~tick 1800 (checkpoints 500/1000/1500 UNCHANGED), but walls now start ~tick
  1900 at 5-tick cadence (was ~1800 at ~13) → **checkpoints 2000/2500/3000
  MOVE**.
- **golden C** ("fast hurry phase", game_seconds 70 → 1400 ticks, runs 1500):
  `hurry` still flips ~tick 200, walls now start ~tick 300 at 5-tick cadence
  (was ~200 at ~13) → the single **tick-1500 hash MOVES**.
- **goldens A/D/E**: A has no clock (empty sim); D/E run 800/300 ticks and never
  reach even moment 1 (~tick 1800). UNCHANGED, including their RNG-stream
  assertions (the enclosure draws no RNG, so no rng CHECK moves anywhere).

Recapture B (indices 3–5: ticks 2000/2500/3000) and C in the same commit, citing
this file. No test_golden.cpp RNG assertion changes.

## 7. Addresses (evidence)

| addr        | role                                                       |
|-------------|------------------------------------------------------------|
| sub_426818  | enclosure stepper: arm/disarm (§2), 250 ms cadence (§3), spiral (§4) |
| sub_410578  | seconds-remaining accessor (`dword_4601A4`)                |
| sub_4105D2  | clock update: `dword_4601A4 = (total_ms - elapsed_ms)/1000`|
| sub_43ACF8  | `timeGetTime()` — the ms drop clock                        |
| sub_425E9B  | set a tile solid (the wall drop)                           |
| sub_405D0C  | clear warpholes(type≤1)+trampolines(type 3) from actor grid on arm |
| VALUELST 27 | enclosement_depth (rings = 2×)                             |
| VALUELST 101| hurry_seconds — banner at this; walls at this − 5          |
| VALUELST 30 | tick rate 20 ⇒ 50 ms/tick ⇒ 250 ms = 5 ticks              |
| dword_45BECC/45BEDC | cos {0,1,0,-1} / sin {-1,0,1,0}, godir-indexed      |
| dword_462244 + 140 | wall-drop sound (3 variants, rand%3 at arm)         |
