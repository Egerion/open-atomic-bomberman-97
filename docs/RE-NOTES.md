# Atomic Bomberman (1997) — Reverse Engineering Notes

Initial feasibility survey of the original install at `D:\Program Files (x86)\INTRPLAY\BOMBRMAN`.
Goal: clean-room rewrite in modern C++ using the original assets.

## Install inventory

| Item | Notes |
|---|---|
| `BM95.EXE` | PE32 i386, only 424 KB — game logic. DirectX-era Win95 title. |
| `DATA/ANI` | 95 `.ANI` files — all sprite animations (chunked `CHFILEANI` format) |
| `DATA/SCHEMES` | 67 `.SCH` files — arena maps, **plain text, self-documenting** |
| `DATA/SOUND` | 2027 `.RSS` files — raw PCM audio |
| `DATA/RES` | `.PCX` backgrounds/UI (standard PCX), `VALUELST.RES`, `SOUNDLST.RES`, `.CAM` files |
| `TOOLS/` | Interplay's own modding tools + official docs (FREDIT, FREDSPIT, PSS) |

## File formats — status

| Format | Type | Status |
|---|---|---|
| `.SCH` schemes | Text | ✅ Trivial. Grid (`#` solid, `:` brick, `.` blank), spawns, powerup table — all commented inline. |
| `VALUELST.RES` | Text | ✅ Gold mine: all gameplay tuning values with original dev comments (Kurt W. Dekker, 1997). Speeds in 1/100 px/frame, probabilities as 1-in-N. Default walk speed = 923, skate bonus = 150, etc. |
| `SOUNDLST.RES` | Text | ✅ Sound ID → file mapping. Header documents RSS: **raw 22 kHz stereo 16-bit signed LE PCM** (headerless). |
| `.RSS` audio | Binary | ✅ Raw PCM per above. Trivial loader. |
| `.PCX` images | Binary | ✅ Standard PCX, 640×480 8-bit palettized. Palette: `TOOLS/BOMBPAL.PCX` / `COLOR.PAL`. |
| `.ANI` animations | Binary | ✅ Custom chunked format (`CHFILEANI` / HEAD / PAL / FRAM / SEQ), fully parsed natively (`libs/assets/src/ani.cpp`, `docs/formats/ani.md`) — the community references below were consulted early on but the port doesn't depend on them. |
| `.CAM` | Text | ✅ Campaign/stage info. Parsed (`libs/assets/src/campaign.hpp/.cpp`) and fully wired: hidden 'C'×5 trigger, stage sequencing, AI-count roster seeding, rover/ghost hazard actors — see `docs/re/campaign.md`. |
| `MESSAGES.TXT` | Text | ✅ All UI strings. |
| `.FON`, `.RMP` | Binary | ✅ Both fully RE'd and ported — `.FON` bitmap fonts (`docs/formats/fon.md`, `bmfont.hpp`), `.RMP` player-colour palette remap (`docs/re/player-colour.md`, `rmp.hpp/.cpp`). Stale "minor/low priority" note from the initial survey removed 2026-07-09 (`docs/re/coverage-audit.md` flagged this row). |
| `LEVELS.DAT` | Binary | 🟡 Still unRE'd — the one remaining loose end from the original survey. Never confirmed as load-bearing for anything currently shipped; see `docs/re/coverage-audit.md` §3's open item. |
| `BM95.EXE` logic | Binary | 🔴 Only true black box: exact AI behavior, movement/collision feel, disease effects, timing. Mitigated by VALUELST comments + `docs/re/facts.md`'s from-the-binary findings (the "existing open reimplementations" angle was an early-survey guess; in practice the facts came from direct decompilation, not those projects). |

## Key external references

- [fpc_atomic](https://github.com/PascalCorpsman/fpc_atomic) — complete playable clone (FreePascal) incl. AI + network; readable ANI parser to port
- [HerbFargus/Atomic-Bomberman wiki](https://github.com/HerbFargus/Atomic-Bomberman/wiki/Sprites) — sprite/ANI/Fredit docs
- [ivansandrk/AtomBomberman](https://github.com/ivansandrk/AtomBomberman) — another clone
- Wikipedia: built from licensed Super Bomberman 3 code; 10-player support; hidden level editor

## Binary reverse-engineering

Static analysis of `BM95.EXE` itself (Watcom/i386/DirectDraw; the `getvalue`
mechanism; open questions and still-guessed constants) is tracked in
[re/facts.md](re/facts.md), with the method and the autonomous build/test loop
in [re/method.md](re/method.md). The executable and any disassembly are kept
out of the repo — only distilled facts are recorded.

## Legal note

Rewrite the engine clean-room (no code copied from disassembly into the repo). Original assets (ANI/PCX/RSS) remain Interplay/Konami property — the repo should require users to point at their own game install, like OpenRA/devilutionX do. Don't commit game assets.

## Proposed roadmap

1. **Asset pipeline first**: ANI/PCX/RSS/SCH loaders + a small asset viewer (immediate visual feedback, validates all parsing)
2. Core sim: 15×11 grid, fixed timestep, movement/collision tuned from VALUELST
3. Bombs/flames/powerups/diseases per VALUELST + scheme powerup tables
4. Rendering + audio (SDL2/raylib), menus, local multiplayer
5. AI, then optional netplay
