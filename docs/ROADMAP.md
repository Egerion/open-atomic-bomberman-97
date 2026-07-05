# Roadmap — autonomous "devam" loop

Protocol per item (see CLAUDE.md for the hard rules): RE the mechanic from the
binary first (facts into `docs/re/facts.md`, no guessing) → faithful port into
the owning system under `libs/sim/src/systems/` → doctest suite → full build +
ctest green → golden hashes updated deliberately in the same change when
behaviour changes (cite the facts.md entry) → tick the box here.

## Phase 1 — 1:1 mechanic fidelity

- [x] 1. Jelly bombs — DONE 2026-07-03. RE'd from `sub_42331C` (facts.md "Bomb
      machine"): kicked jelly reverses off obstacles and keeps rolling (sound
      135), flying jelly rolls a 1-in-getvalue(667)=3 ±90° veer per landing
      boundary, kind is exclusive at creation (trigger overrides jelly).
      Implemented in BombSystem + events BombStopped(130)/JellyBounced(135);
      `tests/test_jelly.cpp`; golden B refreshed + new golden E pins the
      ping-pong/veer choreography.
- [x] 2. Random powerup — DONE 2026-07-03. RE'd from `sub_41E21E` case 0xC
      (facts.md "Powerup pickup dispatcher"): rerolls uniformly over the 12
      real kinds (never Random), retries up to 200 times against the scheme's
      forbidden table, then dispatches as the rolled kind (skulls included).
      `State::forbidden` added (config, unhashed); `tests/test_random.cpp`.
      Bonus fixes from the same read: AWESOME cadence is every 5th after the
      7th with a wrap past 50 (was every 3rd), jelly pickup plays 135.
- [x] 3. Dud bombs — DONE 2026-07-03 (facts.md "Dud bombs"). Roll at
      creation for regular bombs only, behind a global gate re-armed
      180 + rand(180) ticks ahead (ids 320/321); 1-in-3 roll (id 322);
      fizzle 120 ticks (id 323) with the fuse frozen, then relight.
      DUDS.ANI wired for rendering; `tests/test_dud.cpp`; golden fully
      recaptured (hash layout + setup arm draw).
- [x] 4. Airborne fuse pause — CONFIRMED 2026-07-03 via `sub_42331C`: the
      fuse only advances when not dud, not flying (+46==2), not carried
      (+46==3), not trigger kind. Our sim already did exactly this; no code
      change. "Still guessed" table is now empty.
- [x] 5. Head-stun — DONE 2026-07-03 (facts.md "Head hit"). Stun is a
      hardcoded 16-tick countdown (was our 20 guess); drop count modulus
      fixed to getvalue(671) itself; drops picked by kind-roll (rand % 15,
      surplus vs start-with) and scattered to RANDOM tiles (sub_4255B2 —
      replaced our nearest-spiral guess). Golden verified unchanged.
- [x] 6. ANI STAT HEAD timing u16 (0x001E vs 0xFFFF) — DONE 2026-07-04
      (facts.md "ANI per-step timing — CONFIRMED INERT"). The loader
      `sub_41CD03` stores the field at step record +0, but **no engine code
      ever reads it** — across the whole decompile the only offset read from a
      step is +16 (the frame pointer). Pacing is `counter % statecnt`
      (`sub_41DAA7`), the counter advanced by game logic per entity type
      (movers per pixel-step, arrows per frame, conveyors counter/3, etc.).
      Data confirms it is not even a terminal marker (0xFFFF never first,
      mostly mid-sequence). Our `Renderer::draw_anim` already matched; made it
      explicit via `game/anim_pace.hpp` (`anim_step_index`, mirrors
      `sub_41DAA7`), documented `SeqStep::head0` as inert, `tests/test_anim.cpp`.
- [x] 7. Stage specials: conveyors, trampolines, dirarrows, warpholes +
      bomb interactions — DONE 2026-07-04, CORRECTED+COMPLETED (strict 1:1),
      (docs/re/stage-actors.md). Actor placement is NOT a .SCH flag — it comes
      from text `DATA/RES/EXTRA<N>.RES` (`sub_404E99` → registry `dword_45E0A8`,
      type at +4: 0=dirarrow,1=warphole,2=conveyor,3=trampoline; godir at +44).
      CONFIRMED constants (no tunables/guesses): VALUELST 30=20 (tick), 42=923
      (walk speed), 189=3, 190/191/192=250/350/450; the belt and the walk share
      the SAME 1/100-px budget units (both scaled by `dword_464958/dword_46494C`
      ≈1 at 20 Hz), so the belt's per-tick budget IS getvalue(190+idx). CONVEYOR
      SPEED is a game OPTION (`dword_464930`, default 1=medium hardcoded pseudo.c
      14652; options.ini `conveyor_speed=` overrides — this install = 2=high);
      the prior default-0 (=250) was the "too fast/slow" bug → now defaults to 1
      (=350). TRAMPOLINE bounce = VALUELST id 680 = 30 frames ("how many frames
      do you bounce on a trampoline?", read by `sub_41F29B` ~23160; id 681=35 is
      the hop px/frame), wired via `Tuning::apply(680)`, replacing the OUR-TUNABLE
      20 (and a mistaken ANI-derived 12, which was the cosmetic belt-frame count).
      DIRARROWS are
      BOMB-ONLY (the player mover `sub_41F29B` has no type-0 branch; confirmed);
      a sliding bomb turns to the arrow godir at a tile centre (`sub_42331C`
      ~25532). WARPHOLES teleport via `sub_405A81` (idno/linkto scan, ZERO RNG),
      pre-resolved to a hashed `warp_dest` grid at setup, sound 1330; player &
      bomb both warp, latched against ping-pong. Bomb-on-conveyor slides at belt
      speed (`sub_42331C` case 0). Bombs do NOT bounce on trampolines (confirmed:
      sound 350 fires from the player stepper only). Ported: hashed
      `actor_type`/`actor_dir`/`warp_dest_*` + `Player::warp_latch`/`Bomb::
      warp_latch`; `StageActorSystem::{move_on_actor,trampoline_after_move,
      warphole_after_move}`; `BombSystem::{slide(budget),conveyor_carry}`;
      `match::apply_actors` warphole link resolution; sound_director 350/1330;
      `Renderer::draw_actors` (dirarrow/warp art + trampoline gated on bounce
      state). Tests: `test_conveyor.cpp`, `test_trampoline.cpp`,
      `test_stage_actors.cpp` (new; register in tests/CMakeLists.txt). GOLDEN:
      NO recapture needed — the hash for actor-free / no-warp / no-latch states
      is byte-identical (verified) and no new RNG draws, so golden A-E and their
      RNG-stream assertions are unchanged.
      CORRECTION 2026-07-04 (fly + random land, warp anim): the trampoline is
      NOT an in-place bounce — RE'd `sub_41F29B` state 5 (raw disasm) shows a
      FLIGHT that at the APEX (frame counter == getvalue(680)/2 == 15) teleports
      the player to a RANDOM nearby open tile (loop @0x4203a7: 2×`rand()%5` per
      attempt, both always drawn, up to 100, must differ on BOTH axes, `!solid`
      = `!sub_425FB9`≡`grid::tile_open`, `!bomb` = `!sub_422E48`≡`grid::bomb_at`).
      Hop arc CONFIRMED linear tent `35*min(c,30-c)` (peak 525 px, id 681=35).
      Ported in `StageActorSystem::tick_bounce` (relocation on `State::rng`) +
      renderer lift/shadow-skip. WARP animation: states 6/7 draw the `strcpy`'d
      literal `"spin"` (@0x45a213, in WALK.ANI) — `SequenceSet::spin[player]`,
      drawn while `Player::warp>0`. GOLDEN STILL UNCHANGED: the relocation is the
      only new RNG and runs only in the state-5 gate (needs a trampoline); golden
      places no actors → never entered → byte-identical. No new hashed field.
- [x] 8. Kick nuances audit — DONE 2026-07-04 (facts.md "Kick nuances").
      (1) A bomb sliding onto a lit tile now EXPLODES (`sub_42331C` runs the
      `sub_42708D` flame check per pixel-step) — ported in `BombSystem::slide`
      (now index-based, detonates via FlameSystem). (2) The mid-slide re-steer
      reads a DIRARROW/conveyor stage actor's godir (`sub_405654` scans the
      level-actor registry `dword_45E0A8`, NOT the player array) — there is no
      player re-steer in the original, so nothing added; deferred to item 7.
      (3) Kicked speed = fixed getvalue(300) (`sub_42464B` sets bomb +112);
      the base+190 seen in `sub_41F29B` is the conveyor bonus to *player*
      speed, unrelated — our id 300 was already faithful. `tests/
      test_kick_nuances.cpp`; golden recaptured with #9/#10.
- [x] 9. Trigger allowance — DONE 2026-07-04 (facts.md "Trigger allowance").
      `sub_41EB13` gates the trigger kind on `+85 < +86 (max_bombs)` and
      consumes one (`++85`); `+85` is refilled to 0 ONLY by a Trigger pickup
      (`sub_41E21E` case 9) and is never decremented — so it is a per-pickup
      lifetime budget of `max_bombs` placements, after which bombs downgrade to
      normal timed bombs (placement is not blocked). Ported `Player::
      trigger_placed` (hashed) in `BombSystem::place` + Trigger pickup case.
      `tests/test_trigger_allowance.cpp`; golden recaptured.
- [x] 10. Goldflame literalness — DONE 2026-07-04 (facts.md "Goldflame
      literalness"). Goldflame is flag +94 (`sub_41E21E` case 8); reach is
      computed at drop time in `sub_41EB13` as max(gridW,gridH)=15, OVERRIDING
      short-flame (which sets 1 first). Ported `Player::goldflame` (hashed),
      set in the pickup case, applied in `BombSystem::place` (replaces the old
      flame=99 sentinel). `tests/test_goldflame.cpp`; golden recaptured
      (flame 99 → 15 + flag). (The deferred head-hit goldflame drop is now DONE
      in item 15 §4.)
- [x] 11. Powerup mutual exclusions — DONE 2026-07-04 (`sub_41E21E` via
      `sub_41E16A`): punch↔trigger, grab↔spooger, trigger↔jelly evict each
      other (trigger also drops punch). Added `remove()` calls in
      `powerups.apply`. No RNG draws; hashed flags change → golden recaptured.
- [x] 12. Trigger-bomb rendering — DONE 2026-07-04 (render only). Trigger bombs
      (`Bomb::trigger`) now draw with TRIGBOMB.ANI "bomb trigger green"
      (owner-recoloured, 7-frame animated) instead of the regular pulse; regular
      pulse fallback if the ANI/sequence is missing. `AssetStore::trigbomb` +
      `SequenceSet::bomb_trigger[]`; no golden impact.
- [x] 13. Walk-animation cadence — DONE 2026-07-04 (render only). `Renderer::
      sample_movement` now advances `walk_phase_` by Manhattan pixels travelled
      this tick (`/kScale`, 1 frame ≈ 1 pixel) instead of once per moved tick,
      matching the original's distance-driven 16.16 phase (`sub_41F29B` indexes
      `phase >> 16`; rover `sub_401B5C` bumps per pixel-step). Cosmetic; no
      golden impact.
- [x] 14. Animated powerups — DONE 2026-07-04 (render only). Floor powerups now
      draw POWERS.ANI `"power <name>"` (bottom-anchored at tile-centre-x /
      tile-bottom-y, advanced by `s.tick`), static POW*.PCX fallback. NOTE: in
      this install only "power random" is multi-frame; the other 13 sequences
      are single-frame, so most powerups still look static — the pipeline is
      correct, the data is 1-frame. `AssetStore::powers` (shared, uncoloured) +
      `SequenceSet::powerup_anim[]`; no golden impact.
- [x] 15. Final in-game 1:1 gaps — DONE 2026-07-04 (facts.md "Final in-game 1:1
      gaps"; strict RE, no guesses). Closes the last four in-game fidelity gaps:
      **(1) Warphole knockout** (`sub_4056CA` case 1, `+146` latch): on first
      activation a warphole clears its own tile AND one RANDOM adjacent tile
      (`rand()%4` cardinal, retry-until-in-bounds, set to Blank). Ported into
      `match::apply_actors` off the SETUP-only LCG (never `State::rng`); mutates
      only `cfg.cells`. **(2) options.ini `conveyor_speed=`** (`sub_406238`
      reader → `dword_464930`, clamp `[0,getvalue(189)-1]`; this install = 2 high
      = 450). New `assets::load_options()` (install.hpp/cpp), wired through
      `game_app` init + `start_match`; absent key ⇒ keeps the confirmed default 1.
      **(3) getvalue(330) idle-fidget spread = 13** (`sub_41F29B` ~23011, "how
      many cornerhead animations there are"): renderer `kPanicSpread` 40-stub →
      13. Presentation `panic_lcg_`, not `State::rng`. **(4) Goldflame on a head
      hit** (`sub_421F7E`, kind 8 = byte +94, start-with id 58 = 0): added to
      `head_hit`'s `surplus()`. Tests: `test_match.cpp` (warp knockout — register
      unchanged, same suite), `test_options.cpp` (NEW — register in
      tests/CMakeLists.txt), `test_sim.cpp` (goldflame head-hit case). GOLDEN:
      gaps 1/2/3 no impact (setup-LCG / config / presentation); gap 4 shifts the
      head-hit RNG draw count ONLY for a goldflame victim ⇒ **scenario B must be
      recaptured** (already required for items 9/10 + diarrhea-throw), A/C/D/E
      byte-identical (A no players; C no punch/grab; D no actions; E hides no
      powerups so no goldflame). REMAINING guess: none new.

## Phase 2 — AI

- [ ] Port ai.c behaviour (VALUELST 900-series). AI must produce PlayerInput
      through the normal TickInputs path so determinism holds.

## Phase 3 — Front-end

- [~] Screen-flow SPINE — DONE 2026-07-04 (docs/adr/0004-frontend-screen-flow.md,
      docs/re/frontend-flow.md). RE'd the real boot path: `sub_42B060` runs
      IPLOGO -> HSLOGO -> TITLE (each `sub_42A088(name, wait)`: name-derived
      `.plt` palette + full-screen image, waits for a key OR the getvalue(12)
      attract timeout), then falls through to the `sub_42B9CE` (noreturn) menu
      loop; results via `sub_42A088(aDraw, 0)`; screen changes covered by
      HEADWIPE.ANI on the standard `counter % statecnt` pacer (no `headwipe`
      string — loaded generically). No FMV ships; the whole front-end is
      PCX+ANI+RSS the pipeline already parses (RSS tracks are SOUNDLST ids:
      title 1000, menu 1010, menuexit 10). Built an SDL-free flow core
      (`libs/game/app_flow.hpp`, pure `next(state,input)`) + a data-driven
      `Screen` primitive + a `Transition` (HEADWIPE wipe / fade fallback);
      `GameApp` now boots Boot->Logo->Title->Menu(stub)->Match->Results(stub)
      ->Menu (dev fast-path `--match` / `BOMBER_BOOT_MATCH`). AssetStore gained
      front-end PCX + HEADWIPE loaders (guarded). `tests/test_frontend.cpp`
      pins the flow graph. libs/sim untouched — no golden impact.
- [~] Polished screens — Title/attract + Results + navigable Menu DONE
      2026-07-04 (docs/re/frontend-flow.md "main-menu items" + "results flow").
      RE'd the real menu loop `sub_42B9CE`: seven rows (v10 0..6 = Play, two
      setup screens, Editor, Credits `.BM`, Roulette, Quit) with an animated
      bomb-trigger cursor over MAINMENU.PCX; the results tail of the Play handler
      `sub_42A3F6` is three-tier — DRAW.PCX (no survivor, sting 1700) / RESULTS
      tally (a survivor) / VICTORY%u.PCX (match winner, voice 2000); BONUS.PCX is
      NOT in that path (spine's round=BONUS guess corrected to VICTORY<player>).
      Built: attract loop (title timeout re-runs IPLOGO->HSLOGO->TITLE rather
      than dead-ending, `run_boot_attract`); a navigable menu (up/down highlight
      wrap, Enter select, Esc quit; nav blip 20 / accept 10; `present_menu`);
      real Results (`round_winner()` -> DRAW or VICTORY<player>). Added four
      `.BM`-backed leaf AppStates (Options/Controllers/Network/Credits) as the
      hub's stub leaves + edges; `tests/test_frontend.cpp` pins the hub graph.
      libs/sim untouched — no golden impact.
- [x] `.BM` text-screen leaves + menu/results polish (#40) — the four leaves now
      render their real text via the `.BM` viewer (`sub_41302D`, `bmscreen.cpp`):
      Credits→CREDITS.BM (inline CREDBAR/JERM/KURT/BOMBDUDE/QALOGO images),
      Options/Network/Controllers→OPTIONS/NETWORK/INPUT.BM help. Font =
      **FONT6.FON** (active font pinned by `sub_431E9C(6)`), decoded by the new
      `bomber::assets::bmfont` parser (1bpp glyphs, `docs/formats/fon.md`). Layout
      is faithful: 34px top/left inset, `344/line_height` visible rows, **keyboard
      line/page scroll (no auto-scroll)**, Enter/Esc dismiss. Menu cursor PINNED to
      the confirmed `getvalue(700/701/702)` = row-700 columns `{332,140,38}`
      (x=332, y=140+38·row), drawn as the animated "bomb trigger green" sprite
      (`ValueList::column_or`, VALUELST multi-column support added parallel to the
      sim's first-column `values`). Draw sting fixed to a one-shot 1700 group pick
      (was looping via music_id). libs/sim untouched — no golden impact.
- [ ] Interactive front-end (DEFERRED, hooks in place): the real Options screen
      (Team Play/Random Start/Conveyor Speed → options.ini) + controller key-remap
      UI (`sub_42B0CE`/`sub_42B47D`) — the `.BM` help overlays render now, the
      settings/remap widgets are the next chunk. Also the RESULTS.PCX cumulative
      tally tier (needs a multi-round match loop + scoreboard) and the map
      editor/roulette screens (menu rows are inert documented stubs).
- [ ] Match settings; campaign later.

## Done (highlights)

Asset pipeline · deterministic sim core · exact movement port (sub_41EC84) ·
bombs/kick/punch/grab/throw/spooger · 9 diseases (contagion/cure/visual) ·
HURRY enclosement · head-stun scatter · owner-coloured bombs/flames · music +
voice lines · stage rotation · component architecture + golden-hash suite.
