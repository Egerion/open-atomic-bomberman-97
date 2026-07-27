# RE investigation — do stage actors vanish at enclosure ARM, or tile-by-tile?

> **RESOLVED 2026-07-26 — this file's Hypothesis 2 (a GLOBAL arm-time trigger)
> is CONFIRMED, and its geometry argument was right.** The mechanism it could
> not name (no binary in that worktree) is `sub_405D0C`, the 100-slot actor
> sweep the arm branch calls: it deactivates every warphole and every
> trampoline outright. See `docs/re/enclosure.md` §5.1 for the body, the call
> site and the citations.
>
> **One correction to what follows.** This file inherited from
> `audit/enclosure.md` Finding 0 the premise that the arm branch writes no
> actor registry, and therefore concluded that whatever the trigger was, it
> had to be **render-only**. Finding 0 is retracted (see its own banner): the
> sweep clears the slots' ACTIVE flag, which removes them from the tile
> lookup `sub_405654` as well as from the animator `sub_4056CA`. So it is a
> **gameplay** change — warpholes and trampolines stop *working*, not just
> stop drawing — and the fix landed in `libs/sim`, not only in the renderer.
> Read this file for the geometry (still correct and still the reason the
> per-tile hypothesis is dead); read §5.1 for the answer.

**Trigger.** Live-play report (Ege): on the **COAL** map the warpholes "really
disappear when the walls START closing," and a suspicion that all the
special-feature maps share the pattern. This re-examines `docs/re/enclosure.md`
§5.1's conclusion — that a stage actor under a closing wall is a **tile-by-tile
render hide** (`Renderer::draw_actors` skips a tile once `cells[y][x] !=
Blank`), never removed all-at-once.

**Scope of what's available here.** The binary and the `native/` transliteration
are NOT in this worktree, so this is a geometry + port-code + install-data
analysis. The original `EXTRA<N>.RES` actor files ARE readable from the runtime
install (`D:\...\BOMBRMAN\DATA\RES\`, never committed) and are the definitive
actor-placement source (`docs/re/stage-actors.md` §2). Every claim that needs a
disassembly read to settle is flagged **[NEEDS BINARY]**.

**Verdict (high confidence on the geometry; the mechanism needs a binary read).**
The COAL warpholes sit on **ring 2**, an INNER ring the spiral closes THIRD —
never on the outer ring it closes first. So the tile-by-tile render gate CANNOT
make them "disappear when the walls start closing": at the default depth they are
never covered at all, and at higher depths they are covered ~24-31 s into the
close, one at a time. **Hypothesis 1 (positional — outer-ring actors covered
early only LOOK like disappear-at-start) is FALSIFIED for COAL.** If the live
observation is accurate, the original hides the actors on a GLOBAL trigger at/near
arm time (Hypothesis 2), independent of wall position — which, per Finding 0 of
`audit/enclosure.md`, is a RENDER-only behaviour (the arm branch writes no actor
registry), NOT a sim change. The current port does not reproduce it. **Recommend
a render-only fix but do NOT implement it yet** — the exact trigger and scope live
inside `sub_4056CA`, which has never been read (§5.1's per-tile gate was INFERRED
from the sibling powerup drawer). See "Recommendation".

---

## 1. COAL's actual warphole layout (from `EXTRA4.RES`)

COAL MINE is level **index 4** (`libs/match/.../level_registry.hpp`, `kNames[4]`),
and per `docs/re/stage-actors.md` §2 level index 4 loads **`EXTRA4.RES`** =
warpholes. The install's `EXTRA4.RES` (Kurt Dekker, 04/02/97), verbatim actor
lines, grammar `-W,<type>,<idno>,<x>,<y>,<linkto>`:

```
-W,1, 0, 2, 2, 3
-W,1, 1,-3, 2, 0
-W,1, 2,-3,-3, 1
-W,1, 3, 2,-3, 2
```

Applying `stage-actors.md` §2's coordinate normalization (negatives wrap from the
far edge: `x += 15` while `< 0`, `y += 11` while `< 0`; board 15×11):

| idno | raw (x,y) | normalized tile | links to idno | partner tile |
|-----:|-----------|-----------------|--------------:|--------------|
| 0    | (2, 2)    | **(2, 2)**      | 3             | (2, 8)       |
| 1    | (−3, 2)   | **(12, 2)**     | 0             | (2, 2)       |
| 2    | (−3, −3)  | **(12, 8)**     | 1             | (12, 2)      |
| 3    | (2, −3)   | **(2, 8)**      | 2             | (12, 8)      |

The four warpholes are the four corners of the rectangle **[2,12] × [2,8]** — i.e.
the perimeter of the depth-2 box. Ring number of a tile is
`min(x, y, 14−x, 10−y)`; all four evaluate to **2**.

## 2. The spiral reaches ring 2 THIRD, not first

Reproducing `EnclosureSystem::spiral()` (a literal port of `sub_426818`'s
advance state machine, `docs/re/enclosure.md` §4) on the shipped 15×11 board
gives these cumulative drop-EVENT counts per ring:

```
ring_end = [52, 96, 132, 160, 180, 192]
  ring 0: events   0..51   (outer perimeter)
  ring 1: events  52..95
  ring 2: events  96..131   <-- the COAL warpholes live here
  ...
  ring 5: events 180..191   (centre tile 7,5 covered at event 182)
```

The spiral STARTS at (0,0) and closes the whole outer ring, then ring 1, before
its 3rd ring ever touches a warphole tile. First-cover event for each warphole
(cadence is a hard 250 ms = **5 ticks/event**, `docs/re/enclosure.md` §3):

| warphole | tile   | ring | first covered at event | ≈ ticks after 1st drop | ≈ seconds |
|----------|--------|-----:|-----------------------:|-----------------------:|----------:|
| idno 0   | (2, 2) | 2    | 96                     | 480                    | 24 s      |
| idno 1   | (12,2) | 2    | 106                    | 530                    | 26.5 s    |
| idno 2   | (12,8) | 2    | 113                    | 565                    | 28 s      |
| idno 3   | (2, 8) | 2    | 124                    | 620                    | 31 s      |

## 3. Depth gate — at the DEFAULT setting the warpholes are NEVER covered

`enclosement_depth` defaults to **1** (VALUELST id 27; `tuning.hpp`,
`docs/valuelst-map.md` id 27; options.ini `enclosement_depth=`, seeded 1). The
port closes `rings = 2 × depth` rings (`rings_for()` in `enclosure.cpp`):

| depth | rings closed | total events | ring 2 (events ≥ 96) reached? |
|------:|-------------:|-------------:|-------------------------------|
| 1 (default) | 2 (rings 0,1) | 96 (events 0..95) | **NO — walls stop one ring short** |
| 2     | 4 (rings 0..3) | 160          | yes, at events 96..124 (late)  |
| 3 ("all") | 6 (rings 0..5) | 192       | yes, at events 96..124 (late)  |

**So under the current port at the default depth, COAL's four warpholes stay
fully visible for the ENTIRE match** — the closing spiral never even reaches
their ring. At depth 2/3 they are covered one at a time, 24-31 s into the close,
in the order (2,2) → (12,2) → (12,8) → (2,8). Neither case is "disappear when the
walls start closing."

**This falsifies Hypothesis 1 for COAL.** The warpholes are not on the outer
ring; the tile-by-tile gate cannot produce the reported timing under any depth.

## 4. The shared pattern is real — all the special actors sit on ring 2

The user's suspicion of a shared pattern is confirmed quantitatively. Kurt
Dekker placed the fixed actors on the SAME ring-2 rectangle across the
special-feature maps (all coordinates normalized, all rings = 2):

- **`EXTRA9.RES` (trampolines, level index 9 = DEEP FOREST GREEN)** — the four
  FIXED trampolines are `-T,2,2 / -T,-3,2 / -T,2,-3 / -T,-3,-3` → tiles
  (2,2),(12,2),(2,8),(12,8): the **exact same ring-2 corners** as COAL's
  warpholes (first covered events 96/106/124/113). The file then adds **four
  `-T,H,H` RANDOM trampolines** placed on odd-parity open tiles at LOAD time
  (`stage-actors.md` §2) — these CAN land on any ring, including the outer one.
- **`EXTRA10.RES` (conveyors, level index 10 = INNER CITY TRASH)** — 34 belt
  tiles forming a rectangular loop: rows y=2 (x=2..12) and y=8 (x=3..12), columns
  x=2 (y=3..8) and x=12 (y=2..7). **Every one is on ring 2** (earliest first-cover
  event 96). None on the outer ring.
- **`EXTRA2.RES` / `EXTRA3.RES` (dirarrows, indices 2 HOCKEY RINK / 3 ANCIENT
  EGYPT)** — these do NOT follow the ring-2 pattern: they are scattered across
  many rings INCLUDING the outer ring 0 (e.g. EXTRA2 has arrows at (8,0),(0,4),
  (6,10),(14,6) — all ring 0; EXTRA3 blankets the board with ~44 arrows, many on
  x=0/14 or y=0/10). Outer-ring dirarrows ARE covered early by the spiral, so for
  those the two hypotheses are indistinguishable. But a dirarrow only re-steers a
  sliding bomb (no player interaction, `stage-actors.md` §5) and is visually
  minor, so its disappearance is rarely noticed — it is not a useful discriminator
  either way. The warphole/conveyor/fixed-trampoline maps (all pinned to ring 2)
  are where the behaviour is both noticeable and discriminating.

**Consequence of the shared placement.** The earlier "outer-ring trampoline
vanished as the walls close" report (cited in §5.1) is almost certainly one of
the **random `-T,H,H`** trampolines that happened to land on an outer ring — that
one IS covered early, so it is consistent with BOTH hypotheses and does not
discriminate. The COAL warphole report is the discriminating one, because those
actors are pinned to ring 2.

## 5. Why the tile-by-tile gate and the observation cannot both be right

- Actors are drawn AFTER the background/cell layer (draw order actors → bombs →
  powerups → flame → players, `audit/renderer.md` "Verified faithful"). So if
  `sub_4056CA` always drew, a covered actor would composite OVER the wall. The
  user sees the actor GONE ⇒ some gate hides it.
- §5.1 INFERRED that gate is per-tile (`!sub_425FB9`, cell-not-solid), mirroring
  the confirmed powerup drawer `sub_424F89`. **But the powerup drawer's gate keys
  off the powerup's OWN cell-state grid (its record's first dword must read 2
  — revealed/floor — AND `sub_425FB9` must say the cell is not solid); actors
  have no such per-tile "revealed" state — they live only in the registry
  `dword_45E0A8`.** The analogy is therefore weaker than §5.1 presents: it is not
  obvious `sub_4056CA` performs a per-actor `sub_425FB9` solid test at all.
- The ONLY reading that matches the COAL observation is a **global** gate: the
  actor animator (or its call site in `sub_42A191`) stops drawing actors once the
  enclosure is armed (`dword_45BE9C`) or once the hurry window opens
  (clock < `hurry_seconds`). That is render-only and fully consistent with
  Finding 0 (arm writes no actor registry) — it just isn't what §5.1 assumed.

## 6. [NEEDS BINARY] The exact functions a native trace must read to settle this

1. **`sub_4056CA` — the actor animator (THE decisive function).** Read its entry
   and per-actor loop. Which of these is its draw gate?
   - (a) a per-actor `sub_425FB9(x,y)` / collision-grid solid test → the current
     §5.1 per-tile inference is right, and the port's `cells != Blank` gate is
     faithful (but then the COAL observation is unexplained — re-check it);
   - (b) a top-of-function early-out on the enclosure-armed flag `dword_45BE9C`
     (or on the hurry-clock predicate `sub_410578() < getvalue(101)`), skipping
     ALL actors → Hypothesis 2, port is wrong, render-only fix needed;
   - (c) neither (some third gate).
   Note whether the gate is per-TYPE (e.g. warpholes/trampolines only) or applies
   to every actor type — that decides the fix's scope.
2. **`sub_426818` arm branch** (`native/src/game/batch_0x42583B.cpp` ~686-705) —
   re-confirm (Finding 0 already did) that the ARM edge writes NOTHING to
   `dword_45E0A8`; in particular that `sub_405D0C` is the level-select lobby-table
   cleanup, and no OTHER registry write hangs off the time predicate.
3. **Every writer of `dword_45E0A8`** — grep the native for stores to the actor
   registry reachable while a match is live + the enclosure armed. If there are
   none (expected), the hide is definitively render-only and the sim registry
   must stay intact (matching the port's current no-sim-change stance).
4. **`sub_42A191`'s call to `sub_4056CA`** — check whether the OUTER per-frame
   loop conditionally SKIPS the actor-draw call during the hurry/armed window
   (an outer gate would be equivalent to (1b) but live one level up).

## 7. The precise live test to disambiguate (for Ege)

The whole ambiguity is **timing**: does an actor vanish at close-START (arm,
independent of wall position) or only when the spiral's wall physically reaches
its tile? COAL's ring-2 warpholes already give a ~24 s gap; a centre actor gives
the maximum possible gap.

**Test A — COAL as-is, depth 3.** Set `enclosement_depth = 3` ("all the way", via
the Options screen or `options.ini`) and play COAL. When the walls arm, the first
wall appears at the **top-left corner (0,0)** and spirals inward. Watch the four
warpholes at the inner corners (2,2),(12,2),(12,8),(2,8):
- **Vanish the instant the walls arm / the first outer-corner wall drops** (while
  the spiral is still way out on ring 0) → **original hides at arm ⇒ PORT WRONG**
  (needs the global render gate).
- **Each stays visible until the spiral's THIRD ring physically reaches its tile**
  — you will watch rings 0 and 1 fully close first, then the warpholes wink out
  one by one in the order (2,2)→(12,2)→(12,8)→(2,8), ~24-31 s after the first drop
  → **original hides tile-by-tile ⇒ PORT RIGHT** (§5.1's current gate is faithful;
  re-word the observation).

**Test B — centre warphole (cleanest; one reversible text edit).** In a COPY of
`EXTRA4.RES`, add a self-linked centre warphole and keep depth 3:

```
-W,1, 4, 7, 5, 4        ; idno 4 at the board centre (7,5), links to itself (harmless in-place hop)
```

Centre (7,5) is **ring 5**, covered at **event 182** — dead last. If that central
warphole disappears at arm/close-start (177 events / ~15 minutes of cadence
before the wall could ever reach it) the port is unambiguously wrong; if it
survives until the very end of the close, the port is right. Restore `EXTRA4.RES`
afterward. (A self-linked warphole's destination is its own tile, so it is a
no-op teleporter; the one-shot `+146` knockout still clears one neighbour brick
at load, which is fine.)

Either test settles it without the binary; the binary trace in §6 then confirms
the mechanism before any code lands.

## 8. Recommendation — do NOT change code yet; render-only fix is the likely direction

- **Confidence.** Geometry (§§1-4): **High** — repo/install-derived, reproducible.
  That the current port fails to reproduce the reported timing: **High** (follows
  directly from the geometry, at every depth). That the original hides at arm
  (Hypothesis 2) rather than tile-by-tile: **Medium** — rests on the live report
  plus the geometry, with the MECHANISM (`sub_4056CA`'s gate) unread.
- **Golden.** Any fix is **render-only / golden-neutral**: the arm branch writes
  no actor registry (Finding 0), a covered actor can never re-trigger in the sim
  anyway (a solid tile is impassable), and mutating hashed `State::actor_type`
  would DIVERGE from the registry-retaining original. So the fix lives entirely in
  `libs/game/src/renderer.cpp` — no `state_hash()` impact, golden untouched.
- **Why not implement now.** The current per-tile gate is ITSELF an unconfirmed
  inference; swapping it for an arm-time gate would replace one guess with
  another. The exact trigger (arm at `hurry−5` vs. hurry-banner at `< hurry` vs.
  first drop) and the scope (all actor types vs. a subset) are not determinable
  without reading `sub_4056CA`. Per the RE workflow ("mirror the original's
  arithmetic, don't paraphrase it"; "Do NOT guess"), the drawer must be read
  first.
- **The fix, once the trace confirms Hypothesis 2.** Gate `Renderer::draw_actors`
  on the port's existing armed/hurry flags instead of (or in addition to) the
  per-tile `cells != Blank` check:
  - if the trace shows the arm flag (`dword_45BE9C`): skip the whole actor draw
    once `s.enclose_interval > 0` (the port's "armed" proxy, set at
    `seconds_left <= hurry_seconds − 5`);
  - if it shows the hurry-clock predicate: skip once `s.hurry` is set
    (`seconds_left < hurry_seconds`, ~5 s earlier);
  - restrict to the actor TYPES the trace says are gated.
  Keep the current per-tile `cells != Blank` skip as a subordinate guard (it is
  correct for the level-7 brick-regen case regardless). Then update §5.1 and this
  file with the confirmed citation. No golden recapture needed (render-only), but
  the `tests/visual/` shots that include an armed enclosure over an actor map
  would want a re-pin.
- **If instead the trace shows the per-tile gate (1a).** The port is already
  faithful; re-examine the COAL observation (e.g. depth actually 2/3 and the user
  timed the late tile-by-tile cover generously) and correct §5.1's wording to
  stop over-claiming the powerup-drawer analogy.

**No code changed in this pass — docs only, so no build/golden run is required.**
