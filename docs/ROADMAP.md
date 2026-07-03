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
- [ ] 2. Random powerup (id 12): currently a no-op on pickup — RE the
      sub_41E21E dispatch case, implement, tests (+ golden).
- [ ] 3. Dud bombs: locate the dud/fizzle mechanic — pointers already in
      facts.md: bomb state 2 = dud, duration getvalue(323)=120, VALUELST
      322=3 next to it (chance candidate); find the roll site, implement,
      tests (+ golden).
- [x] 4. Airborne fuse pause — CONFIRMED 2026-07-03 via `sub_42331C`: the
      fuse only advances when not dud, not flying (+46==2), not carried
      (+46==3), not trigger kind. Our sim already did exactly this; no code
      change. "Still guessed" table is now empty.
- [ ] 5. Head-stun duration: replace our tunable `head_stun_frames = 20` with
      the binary's real value.
- [ ] 6. ANI STAT HEAD timing u16 (0x001E vs 0xFFFF): resolve via the exe's
      ANI player; apply to Renderer animation pacing.
- [ ] 7. Stage specials: conveyors and trampolines (RE stage/scheme flags,
      then sim systems + rendering).
- [ ] 8. Kick nuances audit: diff our try_kick/slide against the binary's
      kicked-bomb code (alignment, timing, edge cases). Known gaps found
      during item 1: a bomb sliding into flame explodes (`sub_42708D` check);
      a resting player on the bomb's tile can re-steer it mid-slide; verify
      the kicked-speed value source (getvalue(base+190) with a per-player
      base) against our id 300.
- [ ] 9. Trigger allowance: the original caps trigger bombs per pickup
      (player counters +85/+86 in `sub_41EB13`) — ours are unlimited. RE the
      allowance value and the exhaustion behaviour, implement (+ golden).
- [ ] 10. Goldflame literalness: the original sets a player FLAG (+94) and
      uses max(gridW, gridH) as reach at drop time; ours sets flame = 99
      (observably equal, hash-different). Align for literal fidelity.

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
