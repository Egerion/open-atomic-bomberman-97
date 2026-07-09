# VALUELST/SOUNDLST id audit — data-driven fidelity cross-check

Snapshot 2026-07-09 (worktree `worktree-agent-a65ca16f71c171b97`, after merge
with `main` @ `f80f1f9`). Unlike `docs/re/coverage-audit.md` (function/subsystem
level), this pass cross-checks **every id** in `VALUELST.RES` and
`SOUNDLST.RES` against every read site in the binary and every consumer in
the port, to catch small fidelity gaps a function-level audit misses.

## Method

1. Enumerated every `id,value[,...]` line in the install's
   `DATA/RES/VALUELST.RES` (251 top-level ids) and `DATA/RES/SOUNDLST.RES`
   (every `id,name` pair) with their author comments.
2. Extracted **every** call site of the VALUELST reader `sub_4124A4`
   (`getvalue`, pinned in `facts.md` "VALUELST lookup mechanism") from
   `pseudo.c` — 228 call sites, both literal-id and computed-offset
   (`getvalue(i+910)`, `getvalue(dword_XXX+25)`, etc.) — and the sound-play
   family `sub_427961`/`sub_427BFB` — 55 call sites.
3. Cross-referenced the hit ids against `docs/valuelst-map.md`,
   `libs/sim/include/bomber/sim/tuning.hpp`'s `Tuning::apply` switch/range
   table, and every `VALUELST`/`getvalue`-cited comment in `libs/game`
   (`grep -rn "VALUELST\|getvalue"`).
4. Cross-referenced SOUNDLST ids against `libs/game/src/sound_director.cpp`
   and every `audio_.play*`/`start_music` call site in `libs/game/src/game_app.cpp`.
5. Reproduced the same grep-and-diff mechanically (Python scratch script,
   not committed) to avoid missing ids by eye; every finding below was then
   read back against the raw `pseudo.c` context before being recorded (no
   decompiled code is copied here — addresses + one-line behaviour only, per
   CLAUDE.md's clean-room policy).

### A load-bearing side-finding: id-namespace collisions

`sub_4124A4` is **not exclusively** a VALUELST reader. Several call sites
(`sub_41456C`'s Yes/No dialog: `getvalue(26)`/`getvalue(25)`, cast to a text
pointer) resolve to strings that only exist in `MESSAGES.TXT` — id 25 there
is `" No "`, 26 is `" Yes "`, 27 is `" Ok "`, none of which match VALUELST's
own id 25 (`20`, frame rate) / 27 (`1`, enclosement depth). `frontend-flow.md`
already disambiguates this correctly (calling it "getstring" in that
context) — this audit independently reproduces and confirms the collision
from the raw call sites, and flags it so a future naive "grep getvalue(N)"
pass doesn't conflate the two id spaces. A second collision exists between
VALUELST and **SOUNDLST**: id 1010 is `MENU.RSS` (music) in SOUNDLST but "gold
twinkle duration, seconds" in VALUELST — both genuinely read, in unrelated
functions (see gap table below). 102 of the 228 `getvalue` call-site ids do
not exist in VALUELST at all; most are MESSAGES.TXT ids read through the
same function (UI label/heading lookups), not VALUELST misses.

## Part A — VALUELST.RES

### A(i) — Gaps: original reads it, port does not consume it anywhere

| id(s) | meaning (file comment) | original consumer | behaviour | in-scope? |
|---|---|---|---|---|
| 650, 651 | "for the many-dropped bomb powerup, this determines the chance (1 in N) of the Clear/Fire In The Hole audio playing" (650=4) / "what constitutes many dropped bombs" (651=4) | `sub_41F29B` pseudo.c ~23358-23372: `v60=getvalue(651)`, `v65=getvalue(650)`; when a player's bomb-drop counter reaches the 651 threshold, `rand()%max(1,650)==0` fires `sub_427961(1200)` (SOUNDLST 1200 group, "Clear"/"Fire In The Hole") | Voice line playing when a player has laid an unusually long string of bombs. Zero port surface: no `Tuning` field, no `sound_director.cpp` case, no counter. | Yes — self-contained gameplay-feel feature, no dependency on unported systems |
| 500, 502, 504, 506 | "the curve (upwards) of a bomb being picked up" — 4-point arc `(12,10)/(25,20)/(25,30)/(12,40)` | presentation arc for the grab-carry bob (distinct from the punched-bomb arc, ids 660/661, which IS ported) | A carried bomb bobs along this 4-point curve while held | Yes — presentation-only, same shape as the already-ported 660/661 punch arc |
| 340–350 | per-level tile regeneration: seconds between regen attempts, 0 = none. Ten of eleven levels are 0; **level index 7 (id 347) = 4** ("cemetary/mortuary") — a real, non-zero feature | **RESOLVED 2026-07-09**: `sub_426704`, called from the enclosure stepper `sub_426818` (`docs/re/facts.md` "Per-level tile regeneration") | Haunted House (the "cemetary/mortuary" dev codename): destroyed bricks slowly regrow every 4 s within a clear radius (id 695) of no player | Ported — `TileRegenSystem`, `tests/test_regen.cpp` |
| 695 | "clear cell radius that must exist around a potentially-regenerating tile spot; nobody can be within this radius" | companion to 340-350, `sub_422351` | gates brick regrowth on player proximity | Ported — same system |
| 449–460 | "ice delay" per level, ms — control-input lag while standing on ice. Only level 2 (id 452, hockey rink) is non-zero (250 ms) | **RESOLVED 2026-07-09**: `sub_41F29B` ~23058-23078's per-player history ring buffer (`docs/re/facts.md` "Ice / input-lag") | Hockey Rink slows/delays control response (a level-wide input-lag, not localized to specific tiles) | Ported — `MovementSystem::ice_delay`, `tests/test_ice.cpp` |
| 1010 (VALUELST sense — collides with the SOUNDLST music id of the same number) | "how many seconds the twinkling of goldman lasts" | `sub_420D4E`/`sub_420F07` (pseudo.c 23549-23716, pinned in `docs/re/goldman-roulette.md` §6): while round-elapsed < getvalue(1010) seconds, spawn sparkle particles (`rand()%6` spawn-5-in-6) over the current gold-wheel-winning player | Cosmetic sparkle overlay on the gold player for the first ~5 s of each round they hold the prize | Yes — presentation-only, address and mechanism already pinned in goldman-roulette.md §6, just never wired into `Renderer`/`GameApp` |
| 43 | default bomb type (0 regular / 1 trigger / 2 jelly) | no `getvalue(43)` call site found anywhere in 228 sites | Likely superseded by per-player powerup-driven kind selection; candidate for **dead**, not a functional gap — listed here rather than in (iii) only because it's plausible a scheme/editor path reads it that this pass's `getvalue`-only scan wouldn't catch | Low priority, unconfirmed |
| 106 | death anim #9 ("the angel") upward px/frame | not located; port's `deaths_for()` discovers the DIE*.ANI pool from the install directly rather than trusting a VALUELST id for count/behaviour | If DIE9.ANI's own frame offsets already encode the upward drift, this id may be redundant with the asset; unresolved without a targeted disasm pass | Low priority, cosmetic |
| 113–119 | "two vertical (Y) coordinates of each player row across the top" / "left (X) coordinates of each player column across the top" | **no `getvalue` call reaches these ids anywhere** (checked literal AND `i+113..119` computed-offset patterns) | Re-classified to (iii) dead data below — the original itself never wires this up | N/A — not a port gap |

### A(ii) — Mismatches: doc says "consumed", code disagrees (or vice versa)

| id(s) | issue | resolution |
|---|---|---|
| 25, 30, 31 | `docs/valuelst-map.md`'s top table lists these under "Consumed by `sim::Tuning` today" (25/30 "defines our tick rate", 31 "elapsed-ms clamp"). **`Tuning::apply` has no `case 25`, `case 30`, or `case 31`.** The port hardcodes `kTicksPerSecond = 20` (`libs/sim/include/bomber/sim/constants.hpp`) and has no elapsed-ms clamp consumer at all (the fixed-tick lockstep sim doesn't need a wall-clock disk-hit clamp the way the original's real-time loop did). The VALUELST *values* are faithfully mirrored, but not through a live `Tuning::apply(25/30/31, …)` path — a modified install changing these ids would have no effect on the port, contrary to what the table implies. | Doc-only fix applied below (valuelst-map.md). Not a functional gap — id 25's own comment explicitly says "do not change this value" and no install is expected to. |
| 95, 101, 27, 660, 661, 665, 667, 670, 671, 1200, 1300, 1310, 1320 | valuelst-map.md's "Consumed by `sim::Tuning` today" table is **missing** these ids even though `Tuning::apply` has a live `case` for every one of them (verified by direct read of `tuning.hpp`). Conversely, the "Mapped, not yet consumed" section **still lists 101/102 and 660/661/665/667/670/671** as unconsumed. | Stale — the table lagged behind `tuning.hpp`'s own evolution. Fixed below. |
| 450–460 ("ice delay") | `libs/game/src/game_app.cpp`'s level-name comment reads `// The 11 built-in level names (VALUELST 450-460 / getvalue(150+n))` — conflating the **ice-delay** ids (450-460, a real, distinct, still-unconsumed VALUELST block) with the unrelated MESSAGES.TXT level-name ids (`getstring(150+n)`). The code itself is correct (it only calls `getstring`, never touches 450-460), but the comment's "VALUELST 450-460" citation next to "level names" is misleading. | Comment-only staleness; noted for a future cleanup pass, not fixed in this docs-only audit to avoid touching unrelated code paths. |
| 1002, 1006 (goldman wheel radii/lissajous) | This audit's mechanical id-diff initially flagged these as unconsumed; a direct read of `game_app.cpp:1981-1984` shows both ARE read live (`values_.column_or(1002,...)`, `values_.column_or(1006,...)`). False positive from the automated pass — recorded here so a future audit doesn't need to re-derive this. | Not a gap — confirmed consumed. |

### A(iii) — Dead data: no getvalue call site found, not consumed

Ids present in VALUELST.RES with **no** literal or computed-offset
`sub_4124A4` call site found in the 228-site scan, and not consumed anywhere
in the port. Not exhaustive proof of dead-in-the-original (a handful could be
read via a call-site shape this scan's regex didn't catch), but a strong
candidate list:

**63 ids**: 3, 4, 5, 6, 7, 8, 9, 13, 14, 43, 52, 53, 54, 56, 58, 59, 61, 64,
102, 106, 113, 114, 115, 116, 117, 118, 119, 122, 202, 206, 207, 212, 215,
216, 217, 225, 226, 227, 231, 232, 235, 236, 237, 240, 241, 242, 245, 246,
247, 324, 690, 705, 715, 745, 765, 770, 775, 795, 1101, 1102, 1103, 1104,
1205.

Notable sub-groups:
- **113–119** ("player row across the top" HUD coords) — genuinely dead in
  the original per the check above; not a port gap.
- **202, 206, 207, 212, 215-217, 225-227, 231, 232, 235-237, 240-242,
  245-247** — the "4th/5th column" slots of the color-remap 5-stride blocks
  (200-247). `Tuning::apply`'s own `if (k % 5 < 3)` guard already documents
  why: the file only ever populates 3 of every 5 slots (R/G/B), so these are
  VALUELST's own unused stride padding, not a port omission.
- **1101–1104** (net protocol per-transport retransmit timing: IPX/modem/
  serial/TCP) — netplay, out of scope per ADR-0003, listed for completeness
  only (matching `coverage-audit.md` §4's netplay boundary).
- **1205** ("human-avoidance bias") — already independently confirmed dead
  in the original by `docs/re/campaign.md` (cited in `tuning.hpp`'s own
  comment); this scan's negative result corroborates that finding rather
  than discovering it fresh.
- **3-9** (sound pre-caching toggles/cache sizing) — plausibly consumed by
  a DirectSound cache-management path this text-decompile scan can't easily
  trace (memory/perf tuning, not gameplay); out of scope for a gameplay port
  regardless.

## Part B — SOUNDLST.RES

### B(i) — Gaps: original plays it, port does not

| id(s) | trigger (file comment / call site) | status |
|---|---|---|
| 1200–1203 ("clear"/"fireinh"/"lookout"/"litemup", + 1204-1279 the "runaway"/DMB/ZAE/JMB voice pool) | `sub_427961(1200)` in `sub_41F29B` — see VALUELST 650/651 above; this is the SAME gap viewed from the sound side | Unimplemented — no `Tuning` counter, no `sound_director.cpp` case |
| 700–999 ("after a player death" taunt pool — the file's own comment block starts at id 701, ends "999 is the last possible death taunt") | `sub_41F29B` pseudo.c ~23481-23485: `v107 = max(1,getvalue(95))`; on `rand()%v107==0`, `sub_427961(700)` | **Was implemented with the WRONG range** — see B(iii) below; fixed in this pass |
| 3450 ("leprosy"), 3500 ("invisible"), 3550 ("duds") | no `sub_427961(3450/3500/3550)` call site anywhere in `pseudo.c` | Not a port gap — confirmed dead in the original (see B(iii)); listed here only because they sit inside the disease-sound id space and could be mistaken for a gap without the negative-result check |
| 1010 (twinkle, VALUELST sense — id collision, see above) | N/A, this is the VALUELST-side gap; SOUNDLST's own 1010 = MENU.RSS music, already ported (`kMenuMusicId`) | Not a SOUNDLST gap; cross-referenced here only to avoid double-counting the collision |

### B(ii) — Inventions: port plays something original doesn't (fidelity violation candidates)

**None found.** Every `audio_.play`/`play_random_in_range`/`play_one_of`/
`start_music` id and range in `sound_director.cpp` and `game_app.cpp` was
checked against a real SOUNDLST.RES block: 10, 20, 40 (net-only, correctly
gated off), 100/101, 120-123, 130, 135, 140-142 (+143-146 correctly excluded
per the "hard-coded to 3" comment), 150/151, 160, 170, 200-299, 300-309,
350, 360-362, 401-499, 550-554, 1000, 1010, 1020, 1100+stage/1120, 1130,
1300, 1310, 1320, 1330, 1400-1699, 1700-1999, 2000-2299, 2300 (+3000-3449
per-disease), 2600-2699, 2700-2799, 2800-2899 all resolve to real,
contiguously-authored SOUNDLST blocks. No id or range in the port falls
outside an authored block.

### B(iii) — Mismatched triggers

| id(s) | issue | fix |
|---|---|---|
| 700–999 taunt pool | `sound_director.cpp`'s `PlayerDied` handler used range `{500, 999}`. SOUNDLST 500-549 and 555-699 are **completely unauthored** (the file jumps from the powerup-get block, "499 is the last standard get-a-powerup sound", straight to the unrelated "ploppy poop" splat group at 550-554, then nothing until the real taunt block at 701-982). The old range therefore had a live chance of firing a **"poops" bomb-drop-splat sound as a post-death taunt** — `AudioEngine::play_random_in_range` only samples ids that are actually loaded (`names_.lower_bound`), so 550-554 (the poops group) WAS a reachable outcome of the old `{500,999}` roll, alongside the real 701-982 taunt lines. | **Fixed in this pass** (`libs/game/src/sound_director.cpp`): range narrowed to `{700, 999}`, matching the literal `sub_427961(700)` call site and the file's own "after a player death" comment block. One-line, presentation-only (cosmetic RNG per CLAUDE.md determinism-contract rule 6), no hashed-state/golden-test impact. |

## Verdict / work queue

Ranked by evidence strength × player-facing value:

1. **VALUELST 650/651 + SOUNDLST 1200 group — "Fire In The Hole" taunt on a long bomb string.** Fully pinned call site (`sub_41F29B`), self-contained (a per-player drop counter + a chance roll + a group-play), no dependency on any unported subsystem. Highest-value gap in this audit.
2. **VALUELST 1010 (twinkle) + `sub_420D4E`/`sub_420F07` — gold-player sparkle.** Already pinned with addresses in `goldman-roulette.md` §6; only wiring into `Renderer`/`GameApp` remains. Purely cosmetic, zero determinism risk.
3. **SOUNDLST 700-999 taunt range bug** — fixed in this pass (see B(iii)).
4. **VALUELST 500/502/504/506 — bomb-pickup arc curve.** Same shape as the already-ported 660/661 punch arc; low effort.
5. **VALUELST 449-460 (ice delay) — Hockey Rink control lag.** **RESOLVED 2026-07-09** (`docs/re/facts.md` "Ice / input-lag", `MovementSystem::ice_delay`, `tests/test_ice.cpp`). Pinned `sub_41F29B`'s 30-slot per-player history ring buffer (~23058-23078): a fixed input-response lag (`ceil(delay_ms/50)` ticks), AI-exempt, ported faithfully. No golden impact (level_index defaults to 0, whose ice_delay_ms is 0) — one-time hash-layout growth from the new `Player::ice_history` field, recaptured in the same commit.
6. **VALUELST 340-350/695 (tile regen) — Cemetery/Mortuary brick regrowth.** **RESOLVED 2026-07-09** (`docs/re/facts.md` "Per-level tile regeneration", `TileRegenSystem`, `tests/test_regen.cpp`). Pinned `sub_426704` (called from the enclosure stepper `sub_426818`) and its companion 695 clear-radius check together: 100-attempt random-tile loop, blank+unoccupied+player-clear eligibility, exactly one regrowth per successful attempt cycle, no dedicated sound/animation (confirmed `sub_40FDE8` is a netplay packet send, not a visual trigger). Also confirms "cemetary/mortuary" (id 347's own VALUELST comment) IS Haunted House (level index 7) by cross-checking against the ice-delay block's own level-name comments at the same index offset. No golden impact — one-time hash-layout growth from the new `State::regen_timer` field, recaptured in the same commit.
7. **Doc staleness in `docs/valuelst-map.md`'s "Consumed" table** (ids 25/30/31 wrongly claimed live-consumed; 95/101/27/660/661/665/667/670/671/1200/1300/1310/1320 wrongly omitted) — fixed in this pass, zero functional risk, but was actively misleading for future audits.
8. **113-119 dead-data reclassification** — no functional impact, corrects a possible future "why don't we read this" false start.
9. **43 (default bomb type), 106 (angel death-anim speed)** — low-confidence, low-value; flagged for a future targeted disasm pass rather than the work queue proper.
10. **3450/3500/3550 (leprosy/invisible/duds sounds), 1101-1104 (net timing)** — confirmed dead/out-of-scope, listed for completeness so a future pass doesn't re-open them.

Nothing in this audit contradicts the sim's determinism contract or requires
a golden-hash update: every genuine gap found is either presentation-only
(cosmetic RNG) or a self-contained new mechanic (the 650/651 taunt) that
hasn't been touched yet, not a behavioural correction to already-ported
gameplay math.
