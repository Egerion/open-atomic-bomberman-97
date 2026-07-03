# Open Bomberman

Clean-room, modern C++20 rewrite of **Atomic Bomberman** (Interplay, 1997), using the assets from your own copy of the original game.

Status: playable 2P local build — deterministic sim (movement, bombs, kick/punch/grab/throw, spooger, diseases, HURRY enclosement, head hits), original art/sound/music, random stage rotation. See `docs/RE-NOTES.md`, `docs/re/facts.md` and `docs/adr/` for research notes and decisions, and `CLAUDE.md` for the architecture and project rules.

## Layout

```
libs/assets   loaders for the original formats (ANI, PCX, SCH, RES, RSS) — SDL-free
libs/sim      deterministic 20 Hz gameplay core (state + systems) — dependency-free
libs/match    scheme + VALUELST -> MatchConfig glue (header-only)
libs/game     SDL3 presentation: asset store, renderer, audio, input, app shell
apps/         bomber_game, bomber_viewer, abtool (thin mains)
tests/        doctest suites incl. golden-hash behaviour pins
```

## Requirements

- An original Atomic Bomberman installation (the game data is **not** included and must never be committed — see `.gitignore`)
- CMake ≥ 3.25
- Windows: Visual Studio 2022; SDL3 comes via FetchContent (`windows-fetch` preset, no vcpkg) or vcpkg (`windows-msvc` preset, `VCPKG_ROOT` set)
- Linux/macOS: any C++20 compiler (SDL3 via FetchContent or system)

## Build

Windows (game + viewer + tools, no vcpkg needed):

```
cmake --preset windows-fetch
cmake --build --preset windows-fetch
```

Headless tools only (no SDL required):

```
cmake --preset headless
cmake --build --preset headless
```

Convenience wrapper: `make play`, `make viewer`, `make test`, `make survey` (Windows needs GNU make: `winget install ezwinports.make`).

## Running

`bomber_game` — playable local-multiplayer build:

```
bomber_game                       # auto-detects the game install, BASIC scheme
bomber_game <game_dir> <scheme>   # explicit install + scheme
```

Player 0: arrows + Right Ctrl/Space (bomb), Right Shift (throw/grab/trigger/punch) · Player 1: WASD + Left Ctrl/E, Left Shift · Esc: quit. When one player remains the match restarts after 3 s. `--demo <ticks> <out.bmp>` renders a scripted match headlessly (CI/verification). Install auto-detection: `BOMBER_GAME_DIR` env var, `gamedir.txt`, or the standard install paths.

`abtool` — headless asset inspector/extractor:

```
abtool survey "D:\Program Files (x86)\INTRPLAY\BOMBRMAN"   # parse & validate every asset
abtool ani DATA\ANI\WALK.ANI out\                          # list + dump frames as BMP
abtool sch DATA\SCHEMES\BASIC.SCH                          # print arena as ASCII
abtool simrun DATA\SCHEMES\BASIC.SCH <game_dir> 400        # headless sim demo (ASCII)
abtool pcx DATA\RES\FIELD0.PCX out\field0.bmp
abtool rss DATA\SOUND\ZEN1.RSS out\zen1.wav
```

`bomber_viewer` — SDL3 animation viewer:

```
bomber_viewer "D:\...\BOMBRMAN\DATA\ANI"        # Up/Down: file · Left/Right: sequence
bomber_viewer <game_dir> --selftest [shot_dir]  # headless CI mode (SDL_VIDEODRIVER=dummy ok)
```

## Tests

`ctest` runs the doctest suites: determinism (10k-tick lockstep), gameplay rules, movement (faithful sub_41EC84 port), diseases, spooger, and the golden-hash pins that freeze sim behaviour against accidental change. Full verification against an original install: `abtool survey` and `bomber_viewer --selftest` — both exit non-zero on any failure.

## Legal

This project contains no Interplay/Konami code or assets. It is a from-scratch reimplementation based on observing data formats and behaviour. You need to own the original game to use it.
