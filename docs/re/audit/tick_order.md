# Fidelity audit — `simulation.cpp` per-tick orchestration (system #8)

**Verdict: PASS — no order-fidelity bugs found.** `run_tick`'s 11 gameplay
phases (players → input-freeze → rovers → clock → chain-drain → bomb
advance → fuse tick → flame/brick age → tile-regen/enclosure → head checks →
disease spread/age → bomb compaction) reproduce the original's gap-order
exactly as reconstructed in `docs/re/facts.md` "Per-tick call order —
END-TO-END". The four divergences that entry fixed on 2026-07-11 are all
still correctly placed in the current code (verified line-by-line below); the
documented rotation cut (tick starts at the player pass, original's frame
starts at clock/bomb) is rotation-identical and was **not** flagged, per the
audit brief. Independently re-derived against the native transliteration
(`native/src/game/batch_0x4293E5.cpp::sub_42A191`, `batch_0x41F29B.cpp`) and
`pseudo.c` — no discrepancy from what facts.md already documents.

Findings: **0 bugs**. 7 verification entries below (all "no deviation").
Highest severity: none.

## Method

1. Read `sub_42A191` (native transliteration + pseudo.c 29488-29557) to
   confirm the original's own frame-local call order.
2. Read `sub_41F29B` (native transliteration `batch_0x41F29B.cpp`) to confirm
   the per-player turn's internal phase order (head check → disease →
   contagion → key-shuffle → stun → input → movement → the end-of-turn
   action tail, i.e. `sub_41F29B`'s bomb-action block at
   `batch_0x41F29B.cpp:642-759`).
3. Built the "gap" order (player-pass → player-pass) per facts.md's rotation
   section, cross-checked against my own read of (1)/(2) rather than trusting
   the doc blindly.
4. Read current `libs/sim/src/simulation.cpp` `run_tick` end to end and
   matched every phase to its gap-order counterpart, file:line cited.
5. Re-derived the input-freeze decrement-boundary arithmetic from scratch
   (not just the code comment) to confirm the post-pass decrement lands the
   1000 ms gate-open boundary correctly at tick granularity.

## Side-by-side gap-order table (coverage proof)

One "gap" = from one player pass to the next player pass, matching facts.md's
"### The rotation" section.

| # | Original (gap position, `sub_42A191` line) | Port (`run_tick` step, file:line) | Match |
|---|---|---|---|
| 1 | Player pass `sub_420F07` (29527): head(flame+pickup) → disease age/expire → contagion → key-shuffle → stun → input → movement(in-move flame+pickup) → end-of-turn action tail, per player slot 0..9 | Step 1: `player_turn` loop, `simulation.cpp:611-633` — movement w/ in-move flame+pickup (`on_move_pixel`/`resolve_player_field`, `:126-129,481-494`) + the end-of-turn-action-tail equivalent `bomb_actions` (`:242-281`) | Rotation-shifted (documented, not a bug) — this step is the *second half* of a player's turn; the *first half* (head+disease) is step 9/10 of the **previous** tick |
| 1b | `dword_4621E0` decrement at TOP of `sub_420F07` (23642-23645), same frame it gates | Step 1b: `s.input_freeze` decrement AFTER the player loop, `:645` | Deliberately shifted one half-step to land the same **1000 ms boundary** at tick granularity (re-derived below) — matches |
| 2 | Rover/ghost mover `sub_401F76`, called from `sub_4016DA` immediately after the player pass (29528-29529) | Step 2: `rovers.tick()`, `:656` | Matches — directly after players, before clock |
| — | Carried-bomb pass `sub_42459A` (29530) — draw/position-sync only, no gameplay | *(none — carried bomb has no separate entity to sync)* | Accepted deviation, already documented (facts.md "Carried-bomb pass") |
| — | Hurry banner check (29531-29549) — draw only | *(none)* | No gameplay effect, correctly omitted |
| 3 | Match clock `sub_4105D2` (29518, next frame) | Step 3: `--s.ticks_left`, `:662-663` | Matches — clock is the first phase of the next frame/gap |
| 4 | Chain-queue drain, top of bomb pass `sub_4245B9`→`sub_42331C` (25331-25346, 29522) | Step 4: `flames.drain_chain_queue()`, `:679` | Matches — first bomb-phase after clock |
| 5 | Per-slot motion (slide/fly), interleaved with fuse/explosion per slot (25350-25742) | Step 5: `bombs.advance_bombs()`, `:682` | Phase-split (accepted deviation, "Within-batch slot interleave", already documented) |
| 6 | Per-slot fuse tick + explosion, interleaved with motion (25605-25681) | Step 6: `bombs.tick_fuses()`, `:685` — explosion via `flames_.explode()` inline | Phase-split, same accepted deviation as #5 |
| 7 | Flame/brick-burn aging `sub_426D06` (29525, after bomb pass) | Step 7: `flames.age_flames_and_bricks()`, `:697` | Matches — same gap, after the batch |
| 8 | Tile regen `sub_426704` (called from WITHIN `sub_426818`, before its own wall logic) → enclosure walls `sub_426818` (29526, after flame age, before player pass) | Step 8: `tile_regen.update()` then `enclosure.update()`, `:708-709` | Matches — regen immediately before the wall stepper, both before the head checks |
| 9 | Player-pass head: flame death (22915-22917) → pickup (22919-22926), per player slot, at the START of next turn | Step 9: `field_vs_players()` (batched over all players), `:714` | Batched-vs-interleaved (accepted deviation, cross-referenced to the diseases audit — see below); relative order vs. steps 3-8 matches |
| 10 | Disease freshness/age/expire/contagion (22927-22975), same per-player turn, immediately after head | Step 10: `diseases.spread_and_age()` (batched over all players), `:719` | Same batched-vs-interleaved deviation as #9; runs AFTER step 9, matching "head pickup before disease aging" |
| 11 | *(no counterpart — 100-slot array, in-place slot clear at explosion)* | Step 11: bomb vector compaction, `:722-724` | Architecture-only; verified `grid::bomb_at` already filters `.active` (`grid.hpp:53`), so the deferred erase has no observable effect on any system that runs between explosion and compaction |

## Verification entries (previously-fixed divergences — confirmed still correctly placed)

**1. Flame-death/pickup runs inside the mover, not just as a post-batch pass**
- **Original:** `sub_41EC84` post-commit tail, pseudo.c 22699-22717 — flame
  check then pickup, every pixel step.
- **Port:** `on_move_pixel`/`resolve_player_field` (`simulation.cpp:116-129`),
  wired into `stage.move_on_actor` at `simulation.cpp:489`.
- **Visible effect:** none (correct) — a walking player still dies/picks up
  mid-move, before that tick's bomb actions.
- **Severity:** none. **Confidence:** high.

**2. Clock/regen/enclosure run BEFORE the head checks**
- **Original:** clock (29518) → … → tile regen/enclosure (29526) → player
  pass head (29527, part of `sub_420F07`).
- **Port:** clock step 3 (`:662`) → tile-regen/enclosure step 8 (`:708-709`)
  → `field_vs_players` step 9 (`:714`).
- **Visible effect:** none (correct) — a wall dropping on a player standing
  on flame still crushes with no flame-kill credit; a wall dropping on a
  token still destroys it before pickup.
- **Severity:** none. **Confidence:** high.

**3. Rovers run immediately after the player pass, before the next bomb pass**
- **Original:** `sub_4016DA`/`sub_401F76` at 29528-29529, directly after
  `sub_420F07` (29527), before the next frame's `sub_4245B9` (29522).
- **Port:** `rovers.tick()` step 2 (`:656`), directly after the player loop
  (step 1) and before the clock/bomb-pass phases (steps 3-6).
- **Visible effect:** none (correct) — a rover never reacts to flames lit
  later in the same gap.
- **Severity:** none. **Confidence:** high.

**4. Flame-death head check exempts bounce/warp immunity**
- **Original:** `sub_41DE63` (called from both the head check at 22917 and
  the in-move check at 22699-22708) early-outs for player states 5/6/7
  (bounce/warp).
- **Port:** `resolve_player_field`'s `if (p.bounce == 0 && p.warp == 0)` gate,
  `simulation.cpp:55`.
- **Visible effect:** none (correct) — a mid-hop/mid-warp player standing
  over flame survives, matching the wall-crush and rover-landing guards.
- **Severity:** none. **Confidence:** high.

**5. Chain-queue drain sits at the first bomb-phase slot after the player pass**
- **Original:** drain at the top of `sub_42331C`, the FIRST bomb-related call
  after the player pass in the rotated stream (25331-25346).
- **Port:** `flames.drain_chain_queue()` step 4 (`:679`), immediately after
  clock (step 3) and before bomb motion (step 5)/fuses (step 6) — the first
  bomb-phase step in the gap.
- **Visible effect:** none (correct) — a trigger-press queued during step 1
  is caught with no player move in between; an arm-hit/landing/slide push
  queued during steps 5-6 waits a full extra gap (one chain link per tick).
- **Severity:** none. **Confidence:** high.

**6. Input-freeze decrement placement (boundary re-derived independently)**
- **Original:** `dword_4621E0` decrements by the measured **display-frame**
  delta at the top of every `sub_420F07` call (23642-23645) — i.e.
  decrement-then-use, same frame, at sub-tick granularity (the freeze is a
  continuous-ms countdown, not a tick counter).
- **Port:** `s.input_freeze` (armed to 20 ticks = 1000 ms) is used
  (`frozen = s.input_freeze > 0`) inside step 1's player loop, then
  decremented AFTER the loop, step 1b (`:645`) — use-then-decrement.
- **Independent check:** with N = the 1-indexed tick-call counter, the port's
  *used* value on call N is `21-N` (before that call's own decrement). Calls
  1-20 all read a positive value → frozen for exactly 20 ticks × 50 ms =
  1000 ms; call 21 reads 0 → first unfrozen tick, landing the boundary at
  exactly t=1000ms elapsed. Swapping to decrement-BEFORE-use (a literal
  transliteration of "decrement at the top") would make call 20 the first to
  read 0, cutting the window to 950 ms — one tick short, exactly the failure
  mode the code comment (`:635-644`) warns against. The chosen order is
  correct, not merely asserted.
- **Visible effect:** none (correct, and provably so).
- **Severity:** none. **Confidence:** high (independently re-derived, not
  just trusting the comment/facts.md prose).

**7. Sub-frame loop bounds / cadence**
- **Original:** the whole per-player turn's input/AI/movement-accrual section
  runs once per DISPLAYED frame (`sub_42A191`, ~184 fps measured — ADR-0006
  amendment).
- **Port:** `kSubFrames = 9`, `kSubFrameMs = {6,5,6,5,6,5,6,5,6}`
  (`constants.hpp:50-51`, sums to 50 ms), looped `for (int sub = 0; sub <
  kSubFrames; ++sub)` in `player_turn` (`simulation.cpp:376`).
- **Visible effect:** none (correct) — matches ADR-0006's canonical-rate
  decision; not a per-tick orchestration bug, just confirming the loop bound
  itself is well-formed (no off-by-one, no stale `kSubFrames` reference
  found elsewhere in `simulation.cpp`).
- **Severity:** none. **Confidence:** high.

## Cross-referenced, not re-litigated here (owned by other systems in the ledger)

- **Disease batching (head-check/disease phases 9-10 batched over all
  players vs. the original's per-player interleave):** facts.md's rotation
  entry already marks this ORDER-EQUIVALENT ("our step order preserves the
  per-player age-then-spread relation... see the disease audit"). Owned by
  ledger system #6 (`diseases.cpp`), still TODO — re-verify there, not here.
- **Bomb slot interleave (steps 5/6 phase-split vs. the original's per-slot
  interleave of motion and fuse/explosion):** explicitly an ACCEPTED
  DEVIATION in facts.md ("Within-batch slot interleave"), owned by ledger
  system #2 (`bombs.cpp`).
- **AI decide slot / danger-grid snapshot timing:** facts.md marks this
  ORDER-EQUIVALENT already; owned by ledger system #13 (`ai.cpp`, flagged
  "re-verify only").
- **Tick-rotation cut itself** (tick starts at players, original frame starts
  at clock/bomb): intentional and rotation-identical per the audit brief —
  not evaluated as a deviation.

## Citations

- `libs/sim/src/simulation.cpp` (full file read; step comments and line
  numbers cited above are from the version audited).
- `libs/sim/src/systems/bombs.cpp:532-567` (`advance_bombs`/`tick_fuses`
  split, confirms explosion fires inside the fuse-tick phase).
- `libs/sim/src/grid.hpp:51-55` (`bomb_at` filters `.active`, confirms the
  deferred bomb-vector compaction is observably inert).
- `libs/sim/include/bomber/sim/constants.hpp:50-51` (`kSubFrames`/
  `kSubFrameMs`).
- `native/src/game/batch_0x4293E5.cpp:776-846` (`sub_42A191`, read directly —
  confirms facts.md's call sequence: clock → backdrop/clip → stage-actor
  sprites → bomb pass → powerup draw → sprite-list → flame age → enclosure →
  player pass → campaign rovers → carried pass → hurry → overlay/flush).
- `native/src/game/batch_0x41F29B.cpp:224-419` (`sub_41F29B` body, read
  directly — confirms per-player order: flame check → pickup →
  freshness/age/expire → contagion-proximity scan → key-shuffle → stun-like
  counters → surround/cornerhead → input-gate → godir clamp/reversed).
- `docs/re/facts.md:1829-2082` ("Per-tick call order — END-TO-END", the
  authority this audit verifies against) and `:4949-4972` ("Round-start
  input freeze").
- `docs/adr/0006-canonical-frame-cadence.md` (sub-frame cadence rationale).
- `docs/re/fidelity-audit.md` (audit process/ledger this entry reports into).
