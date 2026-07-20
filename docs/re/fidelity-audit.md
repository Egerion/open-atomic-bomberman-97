# Systematic fidelity audit — clean-room vs the transliterated original

Goal: eliminate the small remaining behavioural differences between the
clean-room port (`libs/`) and the original, **systematically**, one system at
a time. The method is a line-by-line DIFFERENTIAL read of each clean-room
system against (a) its faithful native transliteration under `native/src/game/`
(git-ignored; the original's logic at source level) and (b) the pseudo.c
decompilation, cross-checked against the binary via `native/tools/disasm.py`.

The oracle (`native --oracle` vs `tools/oracle_mirror`) is the mechanical
backstop: after fixes, a re-diff should keep the two byte-identical for longer.
Known oracle caveats (do NOT chase as clean-room bugs — see native/docs/
M3_NOTES "Oracle diff VERDICT"): the native flame-spread harness gap (F=
fields unreliable), and the tick-rotation offset at explosions (absolute tick
label differs by one; behaviour identical).

## Process

1. **Audit (read-only):** one agent per system reads clean-room + native +
   pseudo.c and writes findings to `docs/re/audit/<system>.md` — each finding
   = {what the original does (fn + pseudo.c line + arithmetic), what the port
   does (file:line), visible effect, severity, confidence, suggested fix}.
   Changes NO code.
2. **Triage:** real bug vs intentional-documented deviation vs harness
   artifact. Recorded in the per-system findings file.
3. **Fix:** confirmed bugs fixed in verified batches — RE citation in the
   code comment, a doctest, golden/visual recapture if hashes move, full
   suite green.

## Ledger

Status: TODO | AUDITING | AUDITED | FIXING | DONE

| # | System (clean-room) | Native counterpart(s) / pseudo.c | Status | Findings |
|---|---|---|---|---|
| 1 | `movement.cpp` (per-pixel mover, speed, ice) | `sub_41EC84`, `sub_41F29B` accrual (batch_0x41F29B) | AUDITED | **1 finding, LOW, dormant.** `ice_delay` resolves its ring-buffer slot via a constant-cadence formula (`ceil(delay_ms·9/50)`) instead of accumulating the real non-uniform per-sub-frame deltas the original ages each slot by. Exact at every shipped tuning value (Hockey Rink's 250 ms is a multiple of the 50 ms tick, so the approximation coincides with real accumulation) — only diverges for a hypothetical custom scheme with a non-tick-aligned `ice_delay_ms`. Everything else (budget order, disease/skate/clog factoring, corner/glide/settle, conveyor add/subtract, opposite-key filter, reversed-controls point, kick-probe handoff) re-verified against the native transliteration and found faithful, mostly already documented by the 2026-07-10 Core-feel audit. See audit/movement.md |
| 2 | `bombs.cpp` (drop/kick/punch/throw/slide/fly/fuse) | `sub_42331C`, `sub_422EDE`, `sub_41EB13`, `sub_424xxx` (batch_0x422DDD/0x42459A) | AUDITED | **4 findings (1 CRIT, 2 HIGH, 1 LOW).** F1 CRIT: whole fuse/explosion/chain block is gated `sub_421969()>1` (alive-side count) in `sub_42331C` (25603) — at ≤1 side left the original FREEZES all armed bombs (no countdown/chain/trigger); `tick_fuses` has no gate, keeps exploding. Reachable most elimination rounds. F2 HIGH: kicked (case 1) + conveyor (case 0) get an unconditional flat +100/FRAME budget bonus (LABEL_21, 25396); port adds nothing → kicked ~9% slow, belts ~18-29% slow. **[fix must be cadence-aware: +100 is per-FRAME; reconcile with the one-pass bomb-slide integration (facts.md "Canonical frame cadence")].** F3 HIGH: conveyor-carried bomb coasts at kicked speed after leaving the belt; original re-checks the tile each tick + never sets motion-state-1, freezing it off-belt (also fixes `stop_own_sliding` wrongly halting belt bombs). F4 LOW: `detonate_triggered` can target a same-tick-placed bomb; original (`sub_424B41` 26036) requires a strictly-earlier creation stamp. audit/bombs.md |
| 3 | `flames.cpp` (ignite/spread/chain/brick-burn/age) | `sub_42331C` arms, `sub_426FCC`/`sub_426D06` (batch_0x426C4C) | AUDITED | **2 findings.** F1 HIGH **CONFIRMED by me**: `explode()` casts arms in enum order {Up,Down,Left,Right}=godir 0,2,3,1; original casts godir 0,1,2,3 (`sub_42331C` k=0..3). RNG side-effects inside arms (relocate/scatter) → draw-order desync when ≥2 fire. Fix: loop `from_godir(0..3)`. Golden-affecting (D). F2 MED: `relocate_overpowered_here` gates on hidden[] only, can't re-fire post-reveal on re-hit (original gates on kind). **[batch 2: NOT A BUG — audit false positive. The flame arm's visible-powerup interceptor (`batch_0x422DDD.cpp:849-861`) destroys an already-revealed token BEFORE the brick branch calls `sub_425107`, so a re-hit never re-relocates; the port matches. No fix.]** See audit/flames.md |
| 4 | `stage_actors.cpp` (conveyor/trampoline/warphole) | `sub_41F29B` actor branches, `sub_405A81` (batch_0x404852) | AUDITED | **1 confirmed + 1 flagged.** F1 MED/high-conf: conveyor idle-push leaves `p.facing=belt_dir` on an unobstructed belt; original (`sub_41F29B` 779-796) ALWAYS reverts facing to the last chosen dir after the belt-forced move (stepper returns 0 → revert fires). facing drives punch/kick dir + pose → visible. Fix: revert facing unconditionally, drop the zero-displacement gate. F2 (low-conf, unconfirmed): possible extra warp/tramp re-trigger when relocated onto another actor tile. audit/stage_actors.md |
| 5 | `powerups.cpp` (pickup/head-hit scatter/reveal/caps) | `sub_42542D`/`sub_4254F3`/`sub_4255B2`/`sub_425107`/`sub_421F7E` | AUDITED | **faithful (RNG-order exact); 1 new finding.** F1 MED: `sub_4214BC` (round reset, `batch_0x420D4E:427`) seeds ALL 13 start_with counters from getvalue(50+j); port (`setup.cpp:112-116`) only direct-seeds ExtraBomb+Flame, the other 11 rely on C++ defaults. Silent under shipped VALUELST (baselines 0) but a real gap for a custom VALUELST setting nonzero start_with for Kick/Skate/... (ids 52-62). Caps/head-hit-scatter/random-reroll/eviction/death-scatter all verified faithful. audit/powerups.md |
| 6 | `diseases.cpp` (contagion/age/expire/effects) | `sub_41F29B` disease block, `sub_41DF4C` | AUDITED | **3 findings** (prior 2026-07-10 audit intact). F1 MED/high: skull/SuperDisease pickup goes SILENT when a rolled Swap finds no valid target — port `continue`s past `give()` skipping the announce sound; original (`sub_41DFB6`) plays the voice line BEFORE the target check. **[batch 2: FIXED — `give()` announces before the Swap target scan; RNG/hash-neutral.]** F2 LOW: `clear()` zeroes hashed `disease_fresh` but no original cure site touches it → oracle hash noise **[batch 1: fixed]**. F3 LOW/low: ShortFuse `max(1,fuse/3)` floor absent from `sub_41EB13`. audit/diseases.md |
| 7 | `enclosure.cpp` (hurry wall spiral) | `sub_426818`/`sub_4278F2` | DONE | **faithful; 2 findings, no gameplay bug.** F0 DOC (med): enclosure.md mislabels `sub_405D0C` (it's a lobby-screen cleanup, not an actor-grid clear) — doc fix only, port correctly does nothing here. F1 LOW FIXED: wall-crush ON-branch now `break`s after the first grounded bomb it detonates on the crushed tile (`sub_426818`'s bomb-crush loop, native batch_0x42583B.cpp:769-820, pseudo.c 27262-27263 `sub_423209((int)v6,-1)` then unconditional `goto LABEL_47`) instead of looping over every bomb sharing the tile; the OFF (silent-eat) branch's loop-to-exhaustion is untouched (matches `sub_424841`'s own fall-through re-search). White-box doctest (two grounded bombs forced onto one tile) in `tests/test_enclosure_stomp.cpp`. Golden-independent: confirmed byte-identical `test_golden` hash output with/without the fix (isolated A/B rebuild — the only golden drift present belongs to concurrent, unrelated WIP in `bombs.cpp`/`ai.cpp`/etc.). audit/enclosure.md |
| 8 | `simulation.cpp` (tick order / gap rotation) | `sub_42A191` sequence | AUDITED | **0 bugs — PASS.** Gap order re-derived from the transliteration matches; all four 2026-07-11 rotation fixes intact; input-freeze use-then-decrement boundary independently confirmed correct. audit/tick_order.md |
| 9 | `renderer.cpp` (pose/draw/anim pacing/offsets) | `sub_41F29B` draw tail, `sub_420F07`, `sub_426D06` draw | DONE (code); visual recapture DEFERRED | **4 findings fixed, all presentation-only (no golden/hash impact).** F1 FIXED: dropped the Y-sort, players now drawn in fixed slot order 0..9 (`sub_420F07`, native batch_0x420D4E:176). F2 FIXED: cornerhead fidget re-rolls a variant on ANI-cycle completion (elapsed >= that variant's own statecnt) with an elapsed-since-entry phase, dropping the fabricated `20+rand%13` tick spread and the raw-tick phase; VALUELST 330 used only as the variant-COUNT modulus. F3 FIXED: kick/punch pose duration now the KICK/PUNCH sequence's own frame count (set in on_events like the pickup pose), `kActionPoseTicks` removed. F4 FIXED (disasm-confirmed 0x420350-0x420379, `idiv ebx=3`): pickup-pose FRAME now walk-phase-driven (`walk_phase_/3`), pickup_pose_ kept only as the state's exit timer. **Visual goldens NOT recaptured:** an instrumented run of the full 200-tick scripted demo shows none of the four changed paths ever fire (no player overlap, no kick/punch/grab, no boxed-in idle), so the fixes move NO pinned frame (t10 walking still byte-identical, eyeballed). Any shots.txt drift seen while this landed is from the concurrent, still-uncommitted libs/sim fixes (t65/t68 flipped alive 1->2 between rebuilds) — recapture shots.txt against the FINAL sim once that work lands. audit/renderer.md |
| 10 | `setup.cpp` (board build / placement / scatter) | `sub_4260F5`/`sub_4214BC`/`sub_4258E5`/`sub_40551F` | AUDITED | **2 new findings + 1 confirmed cross-reference (audit/powerups.md §1).** F1 MED/high: the hidden-powerup-under-brick scatter (`sub_4258E5`) uses independent 2-draws/try rejection sampling (≤200 tries, silently under-places on a miss); port (`setup.cpp:150-174`) instead pre-lists all Brick cells and removes a uniform-random entry (1 draw/placement, never under-places while a Brick remains) — different RNG draw count/order and a different sparse-board failure mode. **[batch 2: FIXED — port now replicates `sub_4258E5`'s 2-draws/try ≤200-try rejection scan draw-for-draw, interleaved 1-in-10 negative-N gate included; goldens B/C recaptured, D/E unaffected (all counts 0).]** F2 LOW(stock)/MED(custom data): confirms `audit/powerups.md` §1 — only ExtraBomb/Flame starting-inventory baselines are ever applied to a fresh player; the other 11 VALUELST ids 52-62 baselines are parsed into `Tuning::start_with[]` but never written to `Player` at setup (silent under shipped data). Also resolves that audit's open question: scheme `-P born_with` and VALUELST `start_with` are confirmed two independent, additive mechanisms, not one. F3 LOW: spawn-coordinate range handling uses `std::clamp` (saturate both ends) vs. the original's wrap-negative/clamp-overflow asymmetric rule — unreachable with any shipped `.SCH`. Random Start spawn shuffle independently re-confirmed as once-per-MATCH (not per-round) by tracing its real caller; the tile-centre-vs-tile-bottom Y question this pass raised was already resolved (renderer adds the offset at blit time, `facts.md` "Screen geometry"). Spawn-pocket clear left as-is per brief (re-confirmed NOT PINNED, no new evidence). audit/setup.md |
| 11 | `tile_regen.cpp` (Haunted House regrow) | `sub_426704`/`sub_422351` | AUDITED | **0 bugs — FAITHFUL.** Line-for-line match: interval reset-before-attempt, 100-try/2-draws-each loop, eligibility order, clear-radius formula, tick placement before enclosure. audit/tileregen_rovers.md |
| 12 | `rovers.cpp` (campaign rover/ghost mover) | `sub_401B5C`/`sub_401F76` (batch_0x401010) | DONE | **mostly faithful; 1 MED (campaign-only) FIXED.** F1 FIXED: `step()` no longer `return`s on the first flame contact — it now mirrors `sub_401B5C` (raw disasm 0x401E24-0x401ED3, `native/tools/disasm.py 0x401B5C 0x401F76`): mark the actor dead, fall through to the same-tile landing-tile kill, and keep consuming the rest of the tick's move_budget (re-arming the flame-death kill-score event on every further flame tile/pixel crossed that same tick); reaping is deferred to the next `tick()` call as before (`step()`'s return value now reflects `r.alive` at the end of the whole budget loop, not the first hit). Two doctests in `tests/test_rover_flame.cpp`: a long flame-lit corridor produces >1 `RoverDied` events in one tick (was always exactly 1), and a human player standing on the exact tile a rover dies to flame on is now also killed by the landing-tile-kill path. 2 inert cosmetic notes (F2/F3) unchanged, no fix needed. Golden-independent: confirmed byte-identical `test_golden` hash output with/without the fix (isolated A/B rebuild — rovers never run in any golden scenario; the only golden drift present belongs to concurrent, unrelated WIP in `bombs.cpp`/`ai.cpp`/etc.). audit/tileregen_rovers.md |
| 13 | `ai.cpp` (8 behaviours + BFS + danger) | `sub_40A1C6`+behaviours (batch_0x40A140/0x40902A) | AUDITED | **prior fixes intact; 2 NEW (source-level-only).** F1 HIGH: behaviour-4 "Manhattan gate" (`sub_40ABED`, disasm-confirmed 0x40ABED) is DISTANCE TRAVELLED since spawn/punch snapshot (+20/+24) ≥3, NOT absolute-coord sum (corrects ai.md §3.4/§9.4 doc too). Original near-constant FALSE right after spawn/punch; port `abs(tx)+abs(ty)>=3` (ai.cpp:932) TRUE almost everywhere → ported AI bombs a nearby enemy immediately post-spawn/punch where original won't. RNG-order-affecting. F2 MED: `behave_walk_path` no-improve branch writes `path_target_cost=here`; original (`sub_40B20F`) leaves it stale → stale-target invalidation differs next tick → RNG-order in boxed-in edge case. audit/ai.md |

Waves (to bound session-limit blast radius): W1 = 1,2,3,4 (feel-critical);
W2 = 5,6,9; W3 = 7,8,10,11,12; W4 = 13 (light re-verify).

## Campaign status (2026-07-20)

**Audit: 13/13 systems done.** 5 fully faithful (tick-order, tile_regen,
movement, enclosure, powerups — 0 real bugs / dormant-only). ~19 real
deviations found across the rest.

**Fix batch 1 LANDED** — headless 49/49 green, goldens B/C/D/E recaptured
(A/D kExpectedRng byte-identical; E's rng moved deliberately — faster kicked
bomb), visual goldens 5/5 recaptured + eyeballed:
- bombs F1 (round-end freeze), F2 (kicked/conveyor +100/frame speed), F3
  (conveyor coast-stop), F4 (same-tick trigger)
- flames F1 (arm iteration order / RNG desync)
- ai F1 (behaviour-4 Manhattan gate = distance-travelled), F2 (stale cost)
- stage_actors F1 (belt facing revert)
- diseases F2 (disease_fresh on clear)
- setup/powerups F1 (all-13 start_with seeding)
- rovers F1 (flame-death mid-move), enclosure F1 (single-bomb crush) — golden-independent
- renderer F1-F4 (draw order slot 0..9, cornerhead/kick/punch/pickup pose pacing)
- enclosure.md sub_405D0C mislabel corrected (doc)

**Fix batch 2 LANDED (2026-07-20)** — headless 49/49 green:
- diseases F1 (Swap-with-no-target announce) — DONE. RNG- and hash-neutral
  (the announce is a derived, unhashed event and draws no `State::rng`); no
  golden moved. `test_disease.cpp` "a swap roll with no valid target still
  emits the pickup announce".
- setup F1 (hidden-powerup scatter → rejection sampling) — DONE, matched
  `sub_4258E5` draw-for-draw. Golden B (all 6 checkpoints + setup) and C
  recaptured; D/E zero all `spawn_counts` so their scatter draws nothing and
  they stayed byte-identical (kExpectedRng / bounces / final rng all
  UNCHANGED, verified before recapture); A never runs `build_state`.
  `test_sim.cpp` two scatter tests (draw-count on a brickless board, brick-only
  placement).
- flames F2 (relocate re-fire post-reveal) — **NOT APPLIED; audit false
  positive.** A re-hit of an already-revealed over-powerful token never reaches
  `sub_425107` a SECOND time: the flame arm's VISIBLE-powerup interceptor
  (`batch_0x422DDD.cpp:849-861`, `v49 && *v49==2` → burn & break) fires BEFORE
  the brick branch (line 865) that calls `sub_425107`, so the revealed token is
  destroyed, not relocated. The port's `spread_to` mirrors this exactly (the
  `s.floor[ty][tx] != None` check at flames.cpp:103 burns & stops before the
  brick branch). The audit analysed `sub_425107` in isolation and missed this
  interceptor — which the audit's own "Verified faithful" section documents
  ("visible floor powerup (destroy, no ignite) → … → brick"). No code change,
  no golden move. See `audit/flames.md` finding 2 resolution.
- Out-of-scope observation logged: `sub_425107` draws an UNCONDITIONAL
  `rand()%30` cure roll (→ `sub_42BE0B`) at its top, on EVERY brick reveal
  (`batch_0x42459A.cpp:587` / pseudo.c 26290). The port models no such
  per-brick-reveal cure draw — a pre-existing systematic omission on every
  brick ignite, NOT introduced by batch 2, and out of scope here (adding it
  would draw once per brick reveal and re-shuffle every golden). Flag for a
  future batch.

Low/dormant items intentionally left: movement ice_delay (custom-VALUELST
only), diseases F3 (ShortFuse clamp, custom fuse<3 only), stage_actors F2
(unconfirmed), setup F3 (unreachable clamp).

## Fixes applied (2026-07-20)

Batch of confirmed SIM-side fidelity fixes landed in `libs/sim` (each with an
`sub_XXXX`/pseudo.c citation in the code and a doctest in
`tests/test_fidelity_audit.cpp`, plus updated existing suites; golden
recaptured — see the 2026-07-20 UPDATE note in `tests/test_golden.cpp`).

| # | System | Finding | Status | Where |
|---|--------|---------|--------|-------|
| 2 | bombs | F1 round-end freeze (`sub_42331C` 25603 / `sub_421969`) | **DONE** | `simulation.cpp run_tick` (`bombs_frozen` gate on `sides_remaining<=1 && !campaign`) |
| 2 | bombs | F2 kicked/conveyor flat `+100*kSubFrames` (LABEL_21 25396) | **DONE** | `bombs.cpp advance_bombs`/`conveyor_carry` |
| 2 | bombs | F3 conveyor bomb freezes off-belt (case 0) | **DONE** | `bombs.cpp advance_bombs`/`conveyor_carry` (motion-state split) |
| 2 | bombs | F4 same-tick trigger exclusion (`sub_424B41` 26036) | **DONE** | `bomb.hpp created_tick`, `bombs.cpp`, `hash.cpp` |
| 3 | flames | F1 arm iteration order 0,1,2,3 (`sub_42331C` k=0..3) | **DONE** | `flames.cpp explode` (`from_godir(g)` loop) |
| 3 | flames | F2 relocate re-fire post-reveal | **NOT A BUG (audit false positive)** — port already faithful | see below |
| 4 | stage_actors | F1 conveyor idle-push facing revert (`sub_41F29B` 789-795) | **DONE** | `stage_actors.cpp move_on_actor` (unconditional revert) |
| 4 | stage_actors | F2 chained warp/tramp re-trigger (low-conf) | DEFERRED | — |
| 5/10 | powerups/setup | F1 seed all 13 `start_with` baselines (`sub_4214BC` 427-428) | **DONE** | `setup.cpp build_state` |
| 6 | diseases | F2 `clear()` leaves `disease_fresh` (`sub_41DF4C`) | **DONE** | `diseases.cpp clear` |
| 6 | diseases | F1 silent-swap announce (`sub_41DFB6` 308-315) | **DONE (batch 2)** | `diseases.cpp give` (announce before Swap target scan) + `test_disease.cpp` |
| 6 | diseases | F3 shortfuse floor | DEFERRED (dormant, custom fuse<3 only) | — |
| 10 | setup | F1 scatter rejection-sampling (`sub_4258E5` 222-255) | **DONE (batch 2)** — matched draw-for-draw (2/try, ≤200 tries, silent drop, interleaved 1-in-10 gate) | `setup.cpp build_state` + `test_sim.cpp` |
| 10 | setup | F3 spawn-coord wrap vs clamp | DEFERRED (unreachable, no shipped `.SCH` trips it) | — |
| 13 | ai | F1 behaviour-4 Manhattan gate = distance-from-snapshot (`sub_40ABED` 0x40AC24) | **DONE** | `ai.cpp behave_bomb_enemy` (reuses `warp_to_*` = the +20/+24 field, seeded to spawn in `setup.cpp`) |
| 13 | ai | F2 stale `path_target_cost` (`sub_40B20F` 607-615) | **DONE** | `ai.cpp behave_walk_path` (drop the write) |

Golden RNG safety net (verified before recapture): golden A final rng and
golden D `kExpectedRng` (all 4 checkpoints) **byte-identical**; golden E final
rng moved (bombs F2 repositions the detonation → different death scatter) —
recaptured. Full headless suite green (49/49).
