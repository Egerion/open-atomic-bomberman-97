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
- [x] 7. Stage specials: conveyors and trampolines — DONE 2026-07-04
      (docs/re/stage-actors.md). Key RE: actor placement is NOT a .SCH grid
      flag — it comes from text `DATA/RES/EXTRA<N>.RES` files parsed by
      `sub_404E99` into the actor registry `dword_45E0A8` (type at +4:
      0=dirarrow,1=warphole,2=conveyor,3=trampoline; dir at +44). Conveyor
      (`sub_41F29B`, VALUELST 190/191/192 = 250/350/450) is a move-budget
      contribution: pushes a standing player along the belt, and adds/subtracts
      a bonus for walking with/against it (never overrides input). Trampoline
      (`sub_41EC84` step-on, `sub_41DE63` guard) launches an in-place bounce
      that ignores input and can't be pushed until it ends (sound 350). Ported:
      hashed `State::actor_type`/`actor_dir` grids, `StageActorSystem` folded
      into the player turn, `libs/assets` EXTRA<N>.RES parser wired through
      `match::apply_actors`, `CONVEYOR.ANI`/`EXTRAS.ANI` floor rendering
      (`Renderer::draw_actors`). Tests: `tests/test_conveyor.cpp`,
      `tests/test_trampoline.cpp`. Follow-ups documented (bombs.cpp:
      bomb-on-conveyor/dirarrow re-steer/tramp/warp; sound_director: 350/1330;
      dirarrow + warphole PLAYER paths). GOLDEN RECAPTURE NEEDED (hash layout
      grew by the actor grids; behaviour unchanged for actor-free scenarios so
      RNG-stream assertions stay).
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
      (flame 99 → 15 + flag). (Deferred: the original also drops goldflame on a
      head hit — left out to avoid an unverifiable RNG-stream shift; see facts.)
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

## Phase 2 — AI

- [ ] Port ai.c behaviour (VALUELST 900-series). AI must produce PlayerInput
      through the normal TickInputs path so determinism holds.

## Phase 3 — Front-end

- [ ] Menus (title/menu music 1000/1010, MAINMENU ANIs), win/draw screens,
      match settings; campaign later.

## Done (highlights)

Asset pipeline · deterministic sim core · exact movement port (sub_41EC84) ·
bombs/kick/punch/grab/throw/spooger · 9 diseases (contagion/cure/visual) ·
HURRY enclosement · head-stun scatter · owner-coloured bombs/flames · music +
voice lines · stage rotation · component architecture + golden-hash suite.
