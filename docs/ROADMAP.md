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

- [x] Port ai.c behaviour (VALUELST 900-series). AI must produce PlayerInput
      through the normal TickInputs path so determinism holds. Staged per
      docs/adr/0005-ai-architecture.md §8 / docs/re/ai.md. COMPLETE 2026-07-05 —
      all 8 behaviours live (Stages 2-5) in libs/sim/src/systems/ai.{hpp,cpp}.
      - [x] Stage 1 — exhaustive RE (docs/re/ai.md) + determinism ADR (0005).
      - [x] Stage 2 — `Player::ai` + hashed `State::brains`, `AISystem`
            dispatcher (draws A/B), danger+obstacle grids, flee BFS (sub_40970B),
            flame veto (sub_40A76E), wander (sub_40A81F); wired before each
            player's `player_turn`. One-time golden hash-layout recapture.
      - [x] Stage 3 — directed BFS (sub_4092A1) + behaviour 2 directed branch,
            powerup scan (sub_409C1F) + seek-powerup (behaviour 5 sub_40BAF5,
            getvalue(920)=4). Corrected sub_40A59D (does NOT reject powerup
            tiles). NO new hashed field ⇒ golden FROZEN. tests/test_ai.cpp +3.
      - [x] Stage 4 — blast-bricks (sub_40AD8D, getvalue(915)=5) + grab-glove
            (sub_40BD44). Both DROP via the bomb-key edge (action1) → normal
            BombSystem in player_turn. RE corrections: sub_423188 = drop-tile
            CLEARANCE (not escape search); grab-carry = grab-then-LOB (not hold).
            NO new hashed field ⇒ golden FROZEN. tests/test_ai.cpp +5.
      - [x] Stage 5 — DONE 2026-07-05 (AI COMPLETE). enemy targeting
            (sub_422718 two-pass rand%10 / sub_40B8C2 behaviour 6), bomb-near-
            enemy (sub_40ABED behaviour 4, with the byte-confirmed OOB cross-table
            {-1,0,0,0,1}+{0,-1,0,1,0} and the stale +20/+24 Manhattan gate), punch
            (sub_40BE02 behaviour 1), and the safe-branch remote-detonation whim
            (sub_40B20F trigger && !punch && rand%10). TEAM reduces to slot!=self
            (no Player::team; team wiring = documented follow-up). NO new hashed
            field ⇒ golden FROZEN (proven byte-identical via a Stage-5-disabled
            differential build). tests/test_ai.cpp +6; one Stage-3 emergent seed
            refreshed for the new draw stream.

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
- [x] Pre-match SETUP screens + authentic player colour — DONE 2026-07-05
      (docs/re/setup-screens.md, docs/re/player-colour.md). RE'd the real Play
      path `sub_42A3F6` → `sub_410F81` (PLAYER INPUT TYPE, getvalue 705-723) →
      `sub_406DDE` (LEVEL & ROUNDS, getvalue 730-738): `present_setup` (10-slot
      roster, Right cycles OFF→CPU→KBD0→KBD1, Left/'0' off, 'T' team toggle,
      random GLUE<n> backdrop via getvalue(16), music 1020) + `present_map_select`
      (RANDOM + 11 named levels via getstring(150+n), wins 1..100, PgUp/PgDn ±5).
      Strings come from the install's MESSAGES.TXT via the new `assets::messages`
      parser; roster/team/level/wins feed `MatchConfig` (`active[]`, non-hashed
      `team[]`) and `start_match`. Player colour replaced the truecolour-tint
      guess with the original's `.RMP` palette-INDEX remap (`sub_414A65` apply/
      backfill + `sub_415A1C` blit): new `assets::load_rmp` (259-byte format),
      `recolor_image_rmp`/`AniTextures::recolored(rmp)`, tint kept only as the
      missing-file fallback; setup slots inked via `slot_color` (= `sub_41672F`).
      Tests: `test_messages.cpp`, `test_rmp.cpp` (registered). GOLDEN: no impact —
      config/presentation only, `active` defaults all-true so hand-built configs
      are unchanged; sim hash byte-identical.
- [x] Interactive Options screen (Team Play / Conveyor Speed → options.ini) —
      `libs/game/src/options_screen.cpp`. The menu's Options row now opens a
      real editable screen (random `GLUE<n>` backdrop via the same `pick_glue`
      convention as `present_setup`, FONT6 text, Up/Down select, Left/Right
      change, Enter/Esc leave; SFX 20 nav / 10 accept) in place of the `.BM`
      help overlay; F1 on the screen still reaches OPTIONS.BM.
      `bomber::assets::save_options` is a read-modify-write writer (preserves
      unknown lines) added alongside `load_options`, and writes ONLY when a
      setting actually changed. Random Start was left OUT — it is not
      RE'd/documented anywhere in `docs/re/`, and the task rules said not to
      guess it. Team Play is the confirmed screen-level team-mode GATE
      (`dword_464964`, docs/re/setup-screens.md: "Team mode is toggled on the
      OPTIONS game-type screen, OFF by default") layered on top of
      `present_setup`'s existing per-slot `team[]`/'T' toggle: turning it OFF
      zeroes every slot's `MatchConfig::team[]` at `start_match` (so a stray
      'T' press has no effect until Team Play is back ON), turning it ON lets
      each slot's own team stand. Persisted via a `team_play=` key (this
      install's shipped `options.ini` already carries that exact key).
      libs/sim untouched — no golden impact (`MatchConfig` is never read by
      `state_hash()`; full team MODE — a hashed `Player::team` + sim win/
      friendly-fire/AI-target logic — remains the documented follow-up in
      docs/re/ai.md).
- [x] SDL3 gamepad support — DONE 2026-07-08. `GamepadMapper` (enumeration,
      hotplug, d-pad/left-stick + south/east buttons), the setup screen's
      JOYSTICK type-3 slots + joystick pane (getvalue 715/720, msgs 40/41/42),
      the pure `cycle_slot_input_type` helper (sub_421E80's confirmed wrap
      order, unit-tested), and per-slot `collect_inputs()` (keyboard sub 0/1 +
      pad slots) feeding `sim_.tick`. Mid-match disconnect degrades to neutral
      input. libs/sim untouched.
- [x] Multi-round best-of-N loop + RESULTS tally 1:1 — DONE 2026-07-08.
      `AppInput::RoundContinue` keeps the flow graph pure (no side-channel
      state); round 2+ reuses the same roster/level/win-target; and
      `present_scoreboard` now matches docs/re/results-and-options.md §1
      exactly: header getstring(30) @ getvalue(780/781/783), rows getstring(31)/
      (38) @ getvalue(785-788) in per-slot ink, outcome line strings 120/121 vs
      35/36 @ getvalue(800/801/803), voice 2000 under the scoreboard, 6 s idle
      auto-advance. Kills column + win_by_kills clinch = in-flight follow-up
      (needs PlayerDied killer attribution — events are unhashed, golden-safe).
- [x] RE: RESULTS/options/key-remap/roulette truth — DONE 2026-07-08
      (docs/re/results-and-options.md, from pseudo.c): RESULTS layout ids
      780-803 + the two packed counters (sub_421AC8 wins / sub_421B0F kills);
      the real Options screen `sub_4080DC` (19 rows incl. Team Play/Random
      Start; NOT the map editor as previously mislabelled) + ALL 22 options.ini
      keys and the write-on-EXIT semantics (sub_405DE3 via sub_410EBF); the
      key-remap UI `sub_407B9D` (2×6 scancode grid, keydef=); menu row 5 is a
      generic .BM help browser, and the Goldman Roulette wheel (sub_4034BC)
      runs at round setup under goldman=1, not from the menu. BONUS.PCX
      confirmed DEAD (zero references in the whole decompile).
- [x] Options screen + key-remap aligned to the RE — DONE 2026-07-08.
      options_screen.cpp now carries the sub_4080DC rows our port can back
      (Team Play, Random Start, Conveyor Speed, Stomped-Bombs/Win-By-Kills/
      Goldman/Diseases-Destroyable toggles persisted, Enclosement Depth +
      Play Time + Disable-Music with real Tuning/audio consumers, "Define
      keyboard layouts" → the new KeyRemapScreen; net/modem/memory rows
      omitted, documented per row). All 22 options.ini keys typed in
      assets::Options (KeyDef 2×10 grid for keydef=); the write moved to app
      EXIT (`flush_options`) matching sub_405DE3/sub_410EBF; KeyboardMapper is
      data-driven from the bindings. num_to_win_match seeds win_target_.
- [x] TEAM MODE, sim side — DONE 2026-07-08 (the docs/re/ai.md follow-up).
      Hashed `Player::team` (+84 byte; one-time golden hash-layout recapture,
      RNG streams proven identical), AI enemy scans (sub_422718/sub_40ABED/
      sub_40B8C2) honour the documented team filter, round-end generalised to
      "one SIDE left" (`sides_remaining`/`winning_side`, our semantics — team 0
      = solo). Frontend maps the setup 0/1 byte to sim teams 1/2 under Team
      Play (both +84 values are real teams, sub_4141F8). tests/test_team.cpp
      (9 cases); suite 28/28.
- [x] Kill attribution + win_by_kills — DONE 2026-07-08. PlayerDied events
      carry the killer slot (unhashed Event::data, contract rule 4 — golden
      byte-identical, proven); frontend kill tally (self-kills excluded, "our
      semantics") + the §1 v73 win_by_kills clinch (unique-leader tie-break)
      shared between run_app and present_scoreboard via results.hpp.
- [x] Goldman Roulette wheel — DONE 2026-07-08 (docs/re/goldman-roulette.md,
      port 1:1: 5-draw setup, 420/6×70 Lissajous, boundary-decel landing,
      prize table incl. the clogs booby slot as a documented no-op gap;
      per-player born_with_extra config overlay, golden untouched). Gold-player
      assignment = the RESULTS tier's v73 clinch write (§2, pinned + ported).
- [x] Hidden scheme editor — DONE 2026-07-08 (results-and-options.md §5):
      Ctrl+E ×6 menu trigger, chooser + 15×11 mouse editor + powerup-rules
      sub-editor, .SCH writer with parse(write(s))==s round-trip; detail pass
      pinned the brush truth (single-cell), override widget, picker layout,
      new-scheme defaults, and the real TILES0/MISC.ANI canvas art.
- [x] In-round shell — RE'd + ported 2026-07-08 (docs/re/in-match-shell.md):
      NO pause exists (Ctrl+Q instant forfeit added; Esc kept as a documented
      port convenience), MM:SS clock HUD (getvalue 110-112, msg 281, KFONT
      "numeric font", ∞ when untimed, ≤30 s warning ink), hurry flash/2700
      reconciled, stage music 1100+level (fallback 1120), results tier all
      under track 1130 (1020 = setup screens only). F1 mid-round help browser
      = TODO (needs the generic .BM glob browser, same as menu row 5).
- [x] Options toggles into the sim — DONE 2026-07-08 (facts.md "Options
      toggles"): stomped_bombs_detonate = the CLOSING WALL detonates (default
      getvalue(46)=1; airborne exempt) — wall-vs-bomb behaviour aligned;
      diseases_destroyable (getvalue(120)=1; OFF ⇒ skull relocates via
      sub_4255B2) incl. the sliding-bomb powerup squash; random_start's real
      200-swap shuffle replaced our Fisher-Yates guess. Golden byte-identical
      (defaults match the original; proven before/after).
- [x] Faithful screen inks — DONE 2026-07-08: the ink "globals" are RGB555
      LUT offsets (0x495390 table); decoded to exact RGBs (white/grey/cyan/
      team-red) and applied in present_scoreboard incl. the real two-ink team
      split (results-and-options.md §1).
- [x] The generic .BM help BROWSER — DONE 2026-07-08: menu row 5 and the
      in-round F1 modal both open the real sub_41431C/sub_414235 browser
      (HelpBrowser, bmscreen.hpp), gated on getvalue(15) ("manual enabled")
      ahead of the *.BM glob with the pinned getstring(5)/(4)+(95) error
      pair in the real error ink (byte_49D0DA, RGB (252,80,80) — the SAME
      LUT element as sub_4141F8's team-1 ink, results-and-options.md §1/§4).
      The Options screen's F1 and the editor chooser's F1 were corrected to
      open this SAME generic browser too (sub_4080DC/sub_403184 both call
      sub_41431C directly, not a fixed OPTIONS.BM/EDITOR.BM cut — confirmed
      against pseudo.c's F1/315 dispatch in both functions).
- [ ] Remaining front-end: attract-mode demo match (partial work parked in a
      worktree; task was user-stopped), Network screen (netplay deferred per
      ADR-0003), win_by_kills Options row already live.
- [ ] Known parked fidelity gaps (need their own golden recaptures, flagged as
      task chips): flame-arm stops, flying-bomb landing on powerups, scatter
      occupancy test.
- [x] Campaign mode — DONE-with-scope 2026-07-09, consumers RE'd + banner/
      AI-seeding corrected same day (`docs/re/campaign.md`,
      `sub_401085`/`sub_4015C6`/`sub_410F81`/`sub_40151B`/`sub_4016DA`/
      `sub_40133F`). `.CAM` parser (`libs/assets/campaign.hpp/.cpp`,
      `bomber::assets::res`): `;` comments, `-C`-marked 9-field stage lines
      (case-insensitive marker), lenient per-line malformed-line warnings,
      mirrors `messages.hpp`'s parse/load split (`tests/test_campaign.cpp`,
      registered). Trigger: present_setup's 'C'×5 same-key counter
      (`campaign_trigger_count_`, mirrors the menu's Ctrl+E×6
      `editor_trigger_count_`) opens `CampaignFilePicker` (`libs/game/
      campaign_screen.hpp/.cpp`, same glob+list-dialog shape as
      `SchemeFilePicker`) globbing `*.cam` in the install root; a confirmed
      pick loads the file, seeds the roster from stage 0's AI count, and
      arms `campaign_active_`. Flow: campaign SKIPS `present_map_select`
      (`sub_406DDE`'s `if (!dword_46489C)` gate) going straight from
      `present_setup` to the match; a clinched match (VICTORY) advances
      `campaign_stage_index_` and loads the next stage's scheme/roster
      instead of returning to the menu, via a new `AppInput::
      CampaignContinue` (`app_flow.hpp`, `Results -> Match`, doctested in
      `test_frontend.cpp`) — kept distinct from `RoundContinue` since
      campaign stage-advance changes the roster/scheme, breaking that
      event's "same roster/settings" contract. Esc on present_setup clears
      the campaign flag (port convenience — the doc does not pin the
      original's own campaign-exit key). GOLDEN: no impact — zero libs/sim
      changes; campaign only sequences which `match::build_match_config`
      runs next, same anti-corruption boundary `start_match` already
      crosses for a manual game.
      **2026-07-09 consumer RE + correction** (`docs/re/campaign.md`'s
      "sub_40151B", "Rover/ghost/AI roster", "Round pacing", "Stage banner"
      sections): the prior TODO list guessed at consumer sites; all four are
      now pinned. (1) **AI count seeding CORRECTED**: the real seeder is
      `sub_40151B` (not `sub_4015C6`/`sub_42288C` — that only clears a
      per-slot UI latch), which activates the AI count via `sub_422928`'s
      RANDOM slot pick (`rand()%10` + retry-on-occupied), not a sequential
      fill; ported as `bomber::game::seed_campaign_ai_slots` (`libs/game/
      results.hpp`, doctested in `test_frontend.cpp`), driven by the
      existing presentation-only `setup_lcg_`. (2) **Rovers/ghosts are NOT
      player slots** — CORRECTED, the prior "folded into COMPUTER slots"
      roster fill was a mislabelling and has been REMOVED (not replaced):
      they are autonomous roaming map-hazard actors in a third, separate
      particle-actor table (`sub_401AAE`/`sub_401B05` spawn, `sub_401B5C`
      per-tick mover: wander + human-avoidance bias, flame-death with
      kill-score award, player knockback). This is a genuine new `libs/sim`
      actor kind (spawn/movement-AI/flame-interaction/hashed-state/art) —
      confirmed real but deliberately NOT ported here (out of scope for a
      doc-and-small-port pass; flagged as follow-up, not silently dropped).
      `rover_speed`/`ghost_speed` (fields 4/6) are real original inputs (an
      actor's move-budget) with no effect until that actor kind exists.
      (3) **AI difficulty (field 8) CONFIRMED dead** — grepped the whole
      decompile for every read of the campaign record's field-8 slot: none
      exists beyond the loader's own write. Not a guess anymore; nothing
      left to wire up, and no dormant AI-personality link either (VALUELST
      900=1 in the shipped file ⇒ `rand()%900` always yields personality 0
      regardless of any campaign input). (4) **`sub_4016DA` round pacing
      PINNED** (5 clauses: rover/ghost mover driver, the SAME survivor-count
      check the normal round-end already uses, a campaign-only 2s grace
      timer once every rover/ghost is dead, a human/network-alive early-out
      guard, and a mutual-wipeout stage-replay fallback) — clause 2 is
      already exactly our port's existing best-of-N `sides_remaining<=1`
      check, so no change there; clauses 1/3 need rovers/ghosts to exist
      sim-side (deferred with them, now for a pinned reason instead of an
      unpinned guess); clause 5 (mutual-wipeout replay) is independently
      portable but left for a follow-up (edge case, no test pressure yet).
      **Stage banner PORTED**: `GameApp::present_campaign_banner()` shows
      `"(<stage name>)"` (getstring 1235) over `"Prepare to begin
      Campaign!"` (getstring 1230) at every stage transition (first stage
      via the picker, subsequent stages via the Results handler); the
      stage-list-exhausted variant (getstring 1220/1225) is NOT ported,
      consistent with the port's existing dialog-less exhaustion path.
      **Remaining scope calls** (unchanged from before, still deliberate):
      mid-round abandon (Esc/Ctrl+Q inside `run_match`) is indistinguishable
      from a real draw/time-up and so does not itself clear campaign state
      — it replays the same stage like any other draw, avoiding a widened
      `run_match` return contract for this port.

## Done (highlights)

Asset pipeline · deterministic sim core · exact movement port (sub_41EC84) ·
bombs/kick/punch/grab/throw/spooger · 9 diseases (contagion/cure/visual) ·
HURRY enclosement · head-stun scatter · owner-coloured bombs/flames · music +
voice lines · stage rotation · component architecture + golden-hash suite.
