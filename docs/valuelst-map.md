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

Powerup kind order (matches scheme `-P` rows and the id blocks above): extra bomb, flame, disease, kick, skate, punch, grab, spooger, goldflame, trigger, jelly, super-disease, random.

## Mapped, not yet consumed

| id(s) | area |
|---|---|
| 91 | clogs (speed-down roulette effect) |
| 101, 102 | "hurry" timing and late-game powerup gating |
| 120–138 | disease behavior flags and durations (300 frames each) |
| 190–192 | conveyor belt speeds |
| 320–324 | dud-bomb timing and chance |
| 330 | idle "cornerhead" fidget duration spread — original rolls `20 + rand()%getvalue(330)` ticks while boxed in (`sub_41F29B`); presentation-only, renderer's `kPanicSpread` stands in (=40) until the value is read |
| 46 | closing wall detonates (1) vs destroys (0) bombs |
| 660/661, 665, 667 | punched-bomb arcs, pickup pause, jelly craziness |
| 670/671 | powers lost when a bomb lands on your head |
| 680/681 | trampoline timing |
| 340–350 | per-level tile regeneration |
| 450–460 | per-level ice (input lag) in ms |
| 900–920 | AI behavior knobs |
| 1100–1110 | net protocol retransmit timing |

## Not in VALUELST (hardcoded in BM95.EXE — our own tunables)

Flame linger duration (`flame_frames`, we use 10) and the corner-assist threshold (`corner_threshold`, we use 900 = 9 px). Tune against original feel.

Parsing notes: `;` starts a comment; the file ends with a DOS EOF byte (0x1A); a few ids hold coordinate pairs (`id,x,y`) — the current parser keeps the first value only, which is fine for the ids the sim reads.
