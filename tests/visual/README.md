# Visual golden harness

`tests/test_golden.cpp` pins the **sim's** state hashes. Nothing pinned the
**renderer's** output, so presentation regressions (flame offsets, wrong draw
order, a missing animation) could land and ship without any test noticing.
This directory gives the renderer the same pin-and-recapture discipline —
CLAUDE.md's determinism contract, applied to pixels instead of `state_hash()`.

## How it works

1. `bomber_game --demo-shots <label:tick,label:tick,...> <outdir>` (added to
   `apps/game/main.cpp`) runs ONE headless, scripted match — the same fixed
   input script (`bomber::game::demo_inputs`, `libs/game/src/input.cpp`) and
   fixed sim seed (`0xB0BB1E5`, `GameApp::run()`) the older single-shot
   `--demo <ticks> <out.bmp>` flag already used — and saves a `<label>.bmp`
   screenshot of the live backbuffer every time the tick counter reaches one
   of the requested ticks. All shots come from the SAME run, so they're
   mutually consistent (a shot at tick 68 sees everything that happened at
   ticks 1-67 too).
2. `shots.txt` is the manifest: one `<label> <tick> <sha256>` row per pinned
   frame. `run_visual_golden.cmake` reads it, builds the `--demo-shots`
   argument, runs `bomber_game`, and compares each produced BMP's SHA-256
   against the pinned hash.
3. The `visual_golden` ctest (registered in `apps/game/CMakeLists.txt`,
   since it needs the `bomber_game` target, which doesn't exist yet when
   `tests/CMakeLists.txt` runs) invokes that script via `cmake -P` — no
   bash/PowerShell dependency, same as every other CMake-driven step in this
   repo.

We store **hashes**, not the BMPs themselves, to keep the repo lean — a
1280x960 24-bit BMP is ~3.5 MB each, and we only need to detect *that*
pixels changed, not diff them from inside git. When a hash mismatches, the
freshly rendered BMPs are still on disk under the test's working directory
(`build/<preset>/apps/game/visual_shots/`) for a human to actually look at
and decide "regression" vs. "deliberate change, recapture."

## Determinism guarantees

Two runs of the exact same `--demo-shots` command are byte-for-byte
identical:

- **Sim**: `GameApp::run()`'s demo path calls `start_match(0xB0BB1E5)` — a
  fixed literal seed, not `next_seed_` (which is presentation-only and never
  read on this path) — so board layout, brick fill, and powerup placement
  are the same every time. The input script (`demo_inputs`) is a pure
  function of the tick index, no RNG at all.
- **Presentation-side LCGs**: the front end's four cosmetic LCGs
  (`setup_lcg_`, `attract_lcg_`, `goldman_lcg_`, `next_seed_` —
  `GameApp::init()`, `game_app.cpp`) are normally reseeded from real
  per-process entropy (`random_boot_seed()`, wall clock + `random_device`)
  on every launch, matching the original's own boot-time `time_(); srand_();`
  reseed. `--demo`/`--demo-shots` pins all four to a fixed literal
  (`0xD3701234u`) instead. The demo script doesn't currently reach any of
  the menu/attract/Goldman-wheel/campaign code that reads them, but pinning
  removes that as an assumption future demo scripts have to keep
  re-verifying as the scripted match grows to cover more of the front end.
- **Renderer's own cosmetic LCGs** (`panic_lcg_`, `flash_lcg_`, `gold_lcg_`
  — `renderer.hpp`, used for things like the panic-mode player-highlight
  flash) are fixed literals by default and are never reseeded from entropy
  at all, demo mode or not.
- **No wall-clock or frame-rate dependence**: `run_demo()` ticks the sim a
  fixed number of times and calls `renderer_->draw_frame` once per tick —
  no `SDL_Delay`, no vsync-paced frame count, nothing that varies with how
  fast the host machine runs.
- **`options.ini` cannot reach a capture.** The install-root `options.ini` is
  a *mutable* file: the game rewrites it whenever a real session touches an
  Options row, and the original BM95.EXE writes it too. Any value from it
  that reaches the sim or the frame is therefore a per-session variable, and
  a pinned frame must not depend on one. `GameApp::load_config` pins each
  such key on `capture_run()`:

  | key | pinned to | why it matters |
  |-----|-----------|----------------|
  | `random_start`   | `true` (`kCaptureRandomStart`) | shuffles which spawn slot each player index gets (`sub_421793`'s 200-pair swap) — rewrites the whole match from tick 1 |
  | `conveyor_speed` | `2` / High (`kCaptureConveyorSpeed`) | belt speed (ids 190-192); the demo's first bomb rides level 10's conveyor loop |
  | `team_play`      | `false` | team red/white vs each player's own slot colour |
  | `playtime`       | `150` s | the clock HUD is inside the hashed frame |
  | `show_fps`, `native_cadence`, `vsync` | off / off / on | an overlay over every frame; a wall-clock-driven sim |

  The remaining `options.ini` keys (`goldman`, `stomped_bombs_detonate`,
  `diseases_destroyable`, `win_by_kills`, `enclosement_depth`,
  `num_to_win_match`, `disable_game_music`, `levelno`, `smallmemory`) were
  swept 2026-07-28 across their whole ranges and move no pinned frame — the
  scripted match is over at tick 76, long before an enclosure or a second
  round. They are left reading the file. If the demo script ever grows past
  a round end, re-run that sweep before trusting them.

Proven empirically (2026-07-11): two independent `--demo-shots` runs against
the real install produced identical SHA-256 hashes for all 5 pinned shots.
Re-proven 2026-07-28 the harder way: the five hashes are now unchanged across
a full sweep of `random_start` × `conveyor_speed` × `team_play` in the
install's `options.ini`, which before the pins above produced twelve
different sets of five.

## The pinned shots

| label                  | tick | scenario |
|------------------------|------|----------|
| `walking`               | 10   | both players mid-stride, no bombs yet |
| `bomb_pulse`             | 40   | two live bombs on the board, pre-explosion |
| `explosion_mid_flame`   | 65   | a fresh explosion (bright) next to an older, decaying one (dark) — arms + centre visible |
| `brick_crumble`         | 68   | crumbling bricks at two different decay stages |
| `powerup_revealed`      | 76   | two revealed powerup tokens in their reveal-flash frame |

All five come from one scripted 2-player match on the real install's
first-rotation level (`BASIC.SCH`, fixed seed) — see `shots.txt` for the
exact hashes.

## Recapturing

Recapture whenever a renderer change is deliberate — do it in the SAME
commit that makes the change, exactly like `test_golden.cpp`'s hash
constants:

```sh
cmake --build --preset windows-fetch
cmake -DBOMBER_GAME_EXE=build/windows-fetch/Release/OPEN-BM95.exe \
      -DBOMBER_SHOTS_DIR=tests/visual \
      -DBOMBER_WORK_DIR=build/windows-fetch/apps/game/visual_shots \
      -DBOMBER_RECAPTURE=1 \
      -P tests/visual/run_visual_golden.cmake
```

This prints `RECAPTURE <label> <tick> <hash>` for every shot instead of
comparing. Eyeball the BMPs under `BOMBER_WORK_DIR` first (they're real
Windows BMPs — open with anything), THEN paste the printed rows over the
matching lines in `shots.txt`. Don't recapture blind: a hash mismatch is
either a deliberate change (recapture) or the exact class of regression this
harness exists to catch (fix the renderer instead).

## Deriving new shot ticks

Set `BOMBER_DEMO_TRACE=1` when running `--demo` or `--demo-shots` to dump
every sim event (`Event::Type`, tick, position) to stderr — bomb placements,
explosions, brick destructions, powerup reveals/picks, deaths, etc. — plus a
one-line tuning dump (`fuse`/`flame`/`brick_burn` frame counts) at the top.
Use it to find new tick numbers instead of hand-deriving the arithmetic:

```sh
BOMBER_DEMO_TRACE=1 BOMBER_GAME_DIR=<install> \
  build/windows-fetch/Release/OPEN-BM95.exe --demo 200 /tmp/scratch.bmp
```

Then render a few candidate ticks with `--demo-shots` and look at the BMPs
(e.g. via Python/Pillow, or any BMP viewer) before committing to a tick
number — event ticks tell you roughly when something happens, but only the
actual frame tells you whether it reads clearly on screen.

## Adding a new install path / CI

The `visual_golden` test SKIPs (CTest "Not Run", not a failure) whenever
`bomber_game` can't resolve a game directory — no `BOMBER_GAME_DIR` env var,
no `gamedir.txt`, none of the standard install paths
(`libs/assets/src/install.cpp`). That's the expected state on CI, which has
no copy of the original assets (CLAUDE.md: never commit exe-derived
material). `run_visual_golden.cmake` detects this via `GameApp::run()`'s
exit code 2 ("no game_dir resolved") and prints a `VISUAL_GOLDEN_SKIP:`
marker that the test's `SKIP_REGULAR_EXPRESSION` property matches. Any OTHER
non-zero exit is treated as a real failure (crash, bad args, SDL init
failure), not a skip.
