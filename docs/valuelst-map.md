# VALUELST.RES — gameplay value map

`DATA/RES/VALUELST.RES` is a commented, plain-text `id,value` list containing the game's tuning constants. This is our own condensed map of the ids the sim consumes or will consume; open the original file for the full authored commentary.

Units: speeds are hundredths of a pixel per frame; probabilities are 1-in-N; frame counts assume the nominal frame rate below.

## Consumed by `sim::Tuning` today

| id | meaning | original value |
|---|---|---|
| 25 / 30 | nominal/target frame rate — defines our tick rate | 20 |
| 41 | bomb fuse length, frames | 40 (= 2 s) |
| 42 | starting walk speed | 923 |
| 90 | speed added per skate | 150 |
| 300 | kicked bomb speed | 1000 |
| 301 | punched bomb speed | 1300 |
| 100 | match length, seconds | 150 |
| 20 | brick disintegration animation, frames | 10 |
| 50–62 | starting inventory per powerup kind | 1 bomb, 2 flame, rest 0 |
| 400–412 | powerups hidden under bricks per match (negative N = \|N\| tries at 1-in-10) | 10,10,3,4,8,2,2,1,−2,−4,1,−4,−2 |
| 550–562 | per-player accumulation caps (0 = uncapped) | 8,8,0,1,4,1,1,1,1,1,1,0,0 |
| 31 | elapsed-ms clamp per frame (`dword_464958`) — defines the ~1.0 frame/tick budget ratio at 20 Hz | 150 |
| 189 | number of conveyor speeds (count) | 3 |
| 190–192 | conveyor belt speeds low/med/high (1/100 px, same budget units as id 42); selected by the "Conveyor Speed" game option (default 1=medium; this install's options.ini=2) | 250 / 350 / 450 |
| 680 | trampoline bounce length, frames ("how many frames do you bounce"; `sub_41F29B` ~23160) | 30 |
| 900 | number of predefined AI personalities (brain-init spread `rand()%900`; =1 ⇒ every brain is personality 0) — `docs/re/ai.md` §1/§6 | 1 |
| 910 | closing-wall ("fire-god") danger look-ahead, tiles — the AI danger grid marks the next 910 spiral bricks with a decaying threat (`docs/re/ai.md` §4.3) | 15 |
| 915 | blast-bricks drop chance, 1-in-N (behaviour 3 `sub_40AD8D`, Stage 4) — `docs/re/ai.md` §3.3 | 5 |
| 920 | powerup-seek range/BFS depth (behaviour 5 `sub_40BAF5`, Stage 3) — how close a powerup must be for an AI to chase it | 4 |

Powerup kind order (matches scheme `-P` rows and the id blocks above): extra bomb, flame, disease, kick, skate, punch, grab, spooger, goldflame, trigger, jelly, super-disease, random.

## Mapped, not yet consumed

| id(s) | area |
|---|---|
| 91 | clogs (speed-down roulette effect) |
| 101, 102 | "hurry" timing and late-game powerup gating |
| 120–138 | disease behavior flags and durations (300 frames each) |
| 320–324 | dud-bomb timing and chance |
| 330 | **= 13 (CONFIRMED).** File comment: "how many cornerhead animations there are" — id 330 is BOTH the number of cornerhead sequences AND the idle "cornerhead" fidget duration spread (`sub_41F29B` ~23011 rolls `20 + rand()%getvalue(330)`, guarded so the modulus ≥1). Presentation-only (renderer `panic_lcg_`, never `State::rng`); renderer's `kPanicSpread` now = 13 (was the 40 stub). Equals `kCornerheadVariants` by construction, not coincidence |
| 46 | closing wall detonates (1) vs destroys (0) bombs |
| 660/661, 665, 667 | punched-bomb arcs, pickup pause, jelly craziness |
| 670/671 | powers lost when a bomb lands on your head |
| 681 | trampoline hop arc height (px/frame) — presentation-only; id 680 (bounce frames) is now consumed above |
| 340–350 | per-level tile regeneration |
| 450–460 | per-level ice (input lag) in ms |
| 905 | reserved/unused AI slot — no `getvalue(905)` call exists in the binary and VALUELST has no `905,<n>` line; only the editor's label writer touches it (`docs/re/ai.md` §9.5) |
| 1100–1110 | net protocol retransmit timing |

## Not in VALUELST (hardcoded in BM95.EXE — our own tunables)

Flame linger duration (`flame_frames`, we use 10) and the corner-assist threshold (`corner_threshold`, we use 900 = 9 px). Tune against original feel.

Parsing notes: `;` starts a comment; the file ends with a DOS EOF byte (0x1A); a few ids hold coordinate pairs (`id,x,y`) — the current parser keeps the first value only, which is fine for the ids the sim reads.
