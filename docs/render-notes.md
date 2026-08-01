# Renderer notes — the reference material behind `libs/render`

The long-form rationale for `bomber::render`. Every section here used to be a
comment block inside `renderer.hpp`, `renderer.cpp`, `asset_store.hpp` or
`sprites.hpp`, where it was reference material a reader consults once rather
than something to re-read on every visit (coding-standards §10). The code keeps
the `sub_XXXX` citations and the one-line consequence, and points here for the
argument.

Companion documents: `docs/re/facts.md` ("Draw order", "Flame draw offset",
"Walk leg-cycle pacing", "In-match colour quantization"),
`docs/re/audit/renderer.md` (the 2026-07-20 fidelity audit and its four fixes),
`docs/re/stage-actors.md`, `docs/re/goldman-roulette.md`, `docs/re/player-colour.md`,
`docs/frame-pacing.md` and `docs/hd-assets.md`.

---

## 1. Inter-tick interpolation

ADR-0003 fixes the sim at 20 Hz. The ORIGINAL has no such step: it runs its whole
gameplay driver once per DISPLAYED frame with a millisecond frame delta
(`sub_42A191`; the movement budget is `baseSpeed × frameDelta / 50`, facts.md
"Speed = a spent budget"), so at 60-70 fps positions advance a few pixels every
~16 ms. To reproduce that fluidity without touching the determinism contract the
renderer blends the moving entities — players, bombs, rovers — between the
previous and the current tick by `alpha`, the match loop's accumulator fraction
in [0,1). Purely cosmetic; the sim state is never touched. `alpha = 1.0` draws
raw current-tick positions, which is what the demo screenshots, the frozen
end-of-round frames and the visual goldens use.

**Players do not lerp between endpoints.** `player_interp` plays back
`State::sub_trace`, the sim's per-sub-frame samples, across the tick interval.
An endpoint lerp filters out exactly the motion that makes the original look
alive: the AI's stutter-step direction flips (up to `kSubFrames` per tick) and
the human wall-vibrate. Segment `f` runs from sample `f-1` — or the previous
tick's endpoint when `f == 0` — to sample `f`, and sample `kSubFrames-1` is
pinned to the tick's true endpoint by the sim (`run_tick` step 12), so
`alpha → 1` converges on exactly the position the old endpoint lerp produced.
`out_dir` carries the active segment's facing, so the sprite flips mid-tick too.

**The snap threshold.** `kInterpSnapDelta` is 32 px in fixed point. Anything a
moving entity legitimately covers in ONE tick stays well under it: a max-skate
hyper walker is ~30 px, a kicked slide is `getvalue(300)/100` = 10 px, a
punched or thrown bomb's flight leg is under a tile. Warps, trampoline landings
and the flying-bomb field wrap move a full tile (36/40 px) or more in one tick,
and those must SNAP rather than smear across the screen. The snap applies to
BOTH axes together: lerping the small axis of a mostly-teleport move would draw
one frame at a position the entity never occupied. `player_interp` applies the
same rule per SEGMENT, so a relocation that lands inside one sub-frame snaps
there instead of smearing across the tick.

**`advance_tick` must be called once per `sim_.tick()`, inside the match loop's
catch-up loop.** This is an ordering whose violation is silent. When a slow
displayed frame advances the sim two ticks at once, calling it once leaves
`prev` two ticks back — a 2-tick lerp span that snaps or double-speeds for about
three frames, which is the "jump/hitch" jitter. Both callees are tick-keyed
no-ops on repeat, so `draw_frame`'s own calls stay correct for the demo and
screenshot path that ticks-then-draws without such a loop.

---

## 2. The F9 native-cadence path, and what stays on the 20 Hz grid

`set_native_cadence` switches the live game to the original's per-frame
animation clock (ADR-0007). It changes the CADENCE of the renderer's own
bookkeeping and nothing else — the deterministic path is untouched, which is why
it can exist at all.

What advances **per displayed frame** in that mode:

- the walk leg phase, because the sim's `PlayerWalking` events now arrive per
  frame; at 20 Hz against a 180 fps render the walk cycle looks frozen.

What deliberately stays **per sim tick**, gated on `new_tick`:

- the cornerhead fidget and the carry arc. Advancing them per frame plays them
  about nine times too fast — the reported "cornerhead fidget too fast" bug.
- the kick/punch/pickup countdowns in `on_events`, which is why that function
  takes `tick_advanced` and the F9 caller passes false on frames that did not
  cross a 50 ms tick — the reported throw/pickup speed-up.
- bombs and rovers, which step on the 20 Hz systems grid in BOTH modes. On the
  F9 path the players are drawn direct (`interp_alpha_ == 1`), so those slower
  entities interpolate on `entity_alpha_` — the systems accumulator fraction —
  instead. Without that, a flying bomb stutters at 20 Hz against a per-frame
  world.

---

## 3. The pose fallback chain

`select_player_pose` (game_util `carry_pose.hpp`) is a straight encoding of
`sub_41F29B`'s name-build order: the walk/idle flag and the carried-bomb pointer
(+148) choose the BASE name, then the action-state word (+78) overwrites it.

`Renderer::pick_pose` wraps it in a retry loop because a partial install can be
missing any given ANI. Each pass drops the flag whose sequence turned out to be
absent and re-runs the choice, so the player degrades to the next pose down
rather than blanking out. At most one flag is dropped per pass, so the loop
always terminates well inside its guard of eight.

The frame INDEX differs per family, and the differences are load-bearing:

| pose | frame source | why |
|---|---|---|
| Spin | `kWarpTicks - warp` | states 6/7 advance by the per-phase counter (+80) |
| Kick, Punch | `steps.size() - countdown` | states 1/2 `goto LABEL_239` immediately, keeping the +80 frame |
| Pickup | the +48 body phase | the shared tail recomputes it unconditionally (0x420350-0x420379), discarding the elapsed-based frame the pickup block computed |
| Cornerhead | `panic_elapsed_` | elapsed-since-entry, so each rolled variant plays one clean cycle |
| WalkBomb, StandBomb | the +48 body phase | +48 is pinned to 0 for the whole carry, so this is a HELD frame, not a cycle |
| Walk, Stand | `walk_phase_ / 3` | `(u16)player[+48] / 3 % statecnt` (pseudo.c 23410) |

The Pickup row is the subtle one: because +48 is pinned to 0 while the bomb is
held, the pickup sequence is a still frame during the carry and only PLAYS once
the throw clears the carry link — which IS what a throw looks like in the
original, and what a walk-phase-driven pickup pose could never show (it animated
through the carry and then, standing still, showed nothing at the release).

---

## 4. Draw order

The pass order in `draw_frame` IS `sub_42A191`'s per-frame call sequence
(facts.md "Draw order"), and it is pinned by `tests/visual` — a pass that ran
out of order would move a pin while drawing the same art.

1. **Static solid/brick tiles.** The original never draws these per frame at
   all: `sub_425D22` STAMPS "tile %u solid"/"tile %u brick" into the background
   surface whenever a cell changes (`sub_425EFC`/`sub_425E9B`), so every sprite
   pass composites OVER them. A single pass that painted them after the bombs
   buried an airborne bomb behind any tile it crossed. A burning brick draws NO
   static brick here — the ignition stamp's blank-then-revert dance erases it
   from the background, leaving bare floor for the crumble frames, even though
   the CELL stays `Brick` (and blocking) until `burning` expires.
2. **Stage actors** (`sub_4056CA`), the lowest floor layer above the field.
3. **Bombs.** `sub_4245B9` → `sub_42331C` draws inline as it ticks, and it runs
   before the powerup drawer and the flame animator. A bomb sitting on a powerup
   tile, or in a just-ignited flame tile (the one-tick window before a chain
   reaction detonates it), must be drawn UNDER those.
4. **Floor powerups** (`sub_424F89`).
5. **Brick crumble and flames** (`sub_426D06`, kind 9 and kinds 0-8).
6. **Players** (`sub_420F07` → `sub_41F29B`), in FIXED SLOT ORDER 0..9. No
   Y-sort, no depth buffer, no re-ordering: the higher SLOT always wins an
   overlap regardless of screen position. A prior pseudo-3D Y-sort was removed
   here — it silently changed which sprite won an overlap on every multiplayer
   round (`docs/re/audit/renderer.md` finding F1).
7. **Rovers**, **death animations**, then the **gold twinkle** post-pass
   (`sub_420E39`, called after `sub_420F07`'s per-player loop) so the sparkles
   read on top of the player sprite.

Within a pass, `FieldCells` fixes the cell order at row-major, y ascending then
x ascending. That is observable for the same reason the pass order is:
overlapping sprites composite in visit order.

---

## 5. The gold twinkle pool

`docs/re/goldman-roulette.md` §6; VALUELST id 1010 ("how many seconds the
twinkling of goldman lasts", 0 = indefinitely per the file's own legend). Ported
from `sub_420D4E` (spawn) and `sub_420E39` (age/draw), called once per player
per tick by `sub_420F07` while the goldman option is on and a gold player is
pending.

The split between the two halves is the part that was wrong before and is easy
to get wrong again:

- **Spawn** runs once per SIM TICK (`update_gold_sparkles`, from
  `sample_movement`). It scans for the FIRST empty slot in the fixed 100-entry
  pool — exactly one attempt per matching player per tick, never a fresh scan
  per particle — and places a spark with 5-in-6 probability at
  `rand()%40 + player_x - 20`, `rand()%50 + player_y - 48`. The position is
  fixed at spawn; the particle does not track the player afterwards.
- **Ageing and retirement** runs once per ENGINE FRAME, which is why it lives in
  `draw_gold_sparkles` and not beside the spawn. Ageing at 20 Hz made the
  sparkles about nine times too slow and left them lingering.

`gold_player_` carries `dword_46492C`'s dual encoding: a player SLOT index in
solo play, a TEAM id in team play, matched against each player's own team byte.
All three rolls come off `gold_lcg_`, never `State::rng` (determinism rule 6).

---

## 6. The asset load pipeline

`AssetStore::load` walks the groups in the order MASTER.ALI lists them. The
distinction that matters: **`load_core_anis` is the only group whose failure
fails the store.** Everything below it is optional and isolates its own failures
so a partial install still boots — a missing POWERS.ANI falls back to the static
`POW*.PCX` tiles, a missing CORNER file falls back to the stand pose, a missing
ALIENS1.ANI leaves the renderer's plain marker, a missing MISC.ANI leaves the
editor on outline-box markers.

Three files are deliberately NOT loaded because MASTER.ALI does not list them,
so the original engine never loads them either: `HEADWIPE.ANI`, `FLAME.ANI` and
`TRIGBOMB.ANI` are dead art (facts.md "ANI sequence-name audit"). The
menu→setup screen change is a CUT in the original, not a missing feature.

The boot LOADING progress is the port's stand-in for `sub_412E33(100*read/total)`.
Its denominator is the count of `step()` points, and the trailing DATA_HD
overlay block's 34 are counted only when `DATA_HD` exists, so a classic install
fills the bar exactly as XPLODE finishes rather than stalling short.

**Memory.** Once every frame is a GPU texture the CPU-side pixels are dead
weight, but they cannot all be freed at the same moment: the player-coloured
BASE sets are the recolour SOURCE, so their classic pixels survive until
`build_player_sets` has consumed them for every player, and their HD source
frames survive until the per-player HD sets exist. HD is off at boot and
`build_player_sets` skips the 16x-heavier per-player HD recolor; the first Tab
calls `ensure_player_hd_sets`, which builds them and then releases the source.

---

## 7. Two loaders for the same PCX files

`frontend_pcx` and `bm_inline_pcx` read the same `DATA/RES/*.PCX` and cache them
separately, because `sub_41302D` (the `.BM` viewer) differs from the backdrop
loader in both respects that matter:

- **Transparency.** The inline `<IMGname>` blit is `sub_4428E4` → `sub_44AED5`,
  which SKIPS source bytes equal to 0, so palette index 0 is a key colour
  (`key_color.hpp`). Backdrops are copied opaquely and must not be keyed.
  Without this, CREDITS.BM's CREDBAR/QALOGO/BOMBDUDE draw as black rectangles.
- **Palette.** `sub_41302D` loads via `sub_4150F0` (pseudo.c 16370), i.e. raw
  decode plus `sub_41BBBD`, the master-palette SNAP — the same snapping loader
  MAINMENU and GLUE<n> use, and NOT the own-palette `sub_415120` path
  TITLE/DRAW/WINZ use. It cannot be otherwise: the viewer never uploads a
  palette, so on 8-bit hardware these images are physically displayable only
  through the palette already active. Corroboration: JERM.PCX and KURT.PCX are
  100% master-palette colours already, so the snap is the identity for them,
  while the other three shift (CREDBAR by up to 39/255 on its yellows) — which
  is what the original shows too.

So the snap rule for front-end art is name-keyed and exact: MAINMENU and
GLUE<n> snap (`sub_42B9CE` 30760 and `sub_4148E5` 17342-17343 load them through
`sub_4151CC` → `sub_4150F0` with COLOR.PAL left active), everything else —
TITLE, DRAW, WIN, WINZ, the logos — brings its own palette via `sub_42A088` →
`sub_41522D` and stays raw.

---

## 8. Why the scaling filter needs a texture registry

The F10 SOFT SCALING toggle has to be live: no reload, no window recreate.

SDL3 (3.4.10) implements `SDL_SetRenderLogicalPresentation` as a viewport +
scale transform on the draw calls themselves, NOT as an intermediate
render-target texture (`SDL_render.c`: the logical mode only feeds
`GetRenderViewportInPixels`/`logical_scale`; there is no logical target to give
a scale mode to). So there is no single "logical-presentation scaler" knob — the
upscale is filtered PER TEXTURE, by each texture's own
`SDL_SetTextureScaleMode`.

`SDL_SetDefaultTextureScaleMode` is no substitute: it only seeds
`texture->scaleMode` at CREATION and does nothing to the thousands of textures
the boot load already uploaded. Setting it would smooth only later-created
textures — the classic silently-half-works bug.

A live toggle therefore has to reach every EXISTING texture, so `make_texture`
(the single texture-creation funnel in the whole codebase) keeps a registry of
the classic ones and `set_scale_filter` re-stamps them. A registry rather than a
walk over `AssetStore`, because the owners are spread across ~40 containers
there plus `FontTextures`' glyph atlas, `BmScreen`'s inline images and the
viewer app: a per-container walk would have to be extended by hand for every
future container, and forgetting one is invisible except as a patch of the
screen that did not smooth. The registry is an `unordered_set` rather than a
vector because teardown destroys tens of thousands of textures (2327 ANI frames
× 10 recoloured player sets alone) and a linear erase per destroy would make
quitting quadratic.


---

## 9. Three details the code cites but does not spell out

### The brick-crumble clamp

`sub_426D06`'s kind-9 (brick crumble) draw HOLDS the final disintegration frame
once its counter passes `statecnt-1`, instead of the generic `% statecnt` wrap
`draw_anim` otherwise applies. Most tilesets' XBRICK has FEWER cels than
`brick_burn_frames` — 9 vs 10 for FIELD0/1/5 (Green Acres and friends) — so on
the LAST burn tick (`burning == 1`, elapsed == 9) a wrap gives `9 % 9 == 0`, the
FULL fresh brick cel: a one-frame DARK "brick re-forms" flash. FIELD10 has a
10-cel XBRICK and never wraps, which is exactly why the visual golden — captured
on FIELD10's demo field — never caught this. The clamp fixes the rest and leaves
FIELD10 byte-identical.

### The carry arc's two branches

`sub_42331C`'s "carried" state-3 branch (pseudo.c ~25488-25497) reads the
CARRIER's state word +78:

- **+78 == 4** (the pickup animation is playing): curve step
  `k = clamp((+80 elapsed frames) - 1, 0, 3)`, then
  `x = carrier_x + dx * (10 + getvalue(2k+500))` and
  `y = carrier_y + dy * 10 - getvalue(2k+501)` — a 10 px forward nudge plus the
  curve's own forward reach, and a vertical lift growing through the curve's Y
  column (10/20/30/40 px) as the animation plays.
- **any other state** (the steady carry): NO curve at all —
  `x = carrier_x + dx * 10`, `y = carrier_y + dy * 10 - 40`. The bomb sits
  straight above the head.

The port used to run the arc for the whole carry with `k` saturated at 3, which
left the bomb permanently 12 px too far forward — half a tile off to the side
whenever the carrier faced west or east.

### The disease strobe

`sub_41F29B` ~23252, traced 2026-07-09. After the shadow blit, the per-player
draw-colour byte (+0x3C) — which normally selects the FRAME within the current
pose sequence, one frame per player colour 0-9 — is replaced by a fresh
`rand() % 10` whenever bit 3 of the disease-timer WORD (+120, distinct from
+0x3C) is set. So the body is redrawn in a RANDOM one of the ten real player
colours, not tinted with an arbitrary colour, and every pose branch keys off the
swapped-in index.

The `& 8` gate pulses it 8 ticks on, 8 ticks off — 0.4 s of buzz, 0.4 s of calm
at 20 Hz. That clustered pulse is what makes the strobe read as a distinct
"I am diseased" state, and it is the SOLE ongoing cue for the no-bomb
Constipation disease, whose placement block otherwise just looks like "I can't
place bombs for no reason." A prior port gated on `s.tick & 1` instead — a
documented deviation that produced a ~10 Hz shimmer easily dismissed as a render
artifact.
