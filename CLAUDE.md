# Open Bomberman — project rules

Clean-room, modern C++20 rewrite of **Atomic Bomberman** (Interplay, 1997).
Original install lives at `D:\Program Files (x86)\INTRPLAY\BOMBRMAN` (assets
loaded at runtime, NEVER committed). RE findings live in `docs/` — facts in
`docs/re/facts.md`, formats in `docs/formats/`, tuning ids in
`docs/valuelst-map.md`, decisions in `docs/adr/`.

## Architecture

Component-based layout; each component is one CMake target with its public
headers under `include/bomber/<name>/`. Dependencies point one way only:

```
apps/game ─────► libs/game ──► libs/match ──► libs/assets   (SDL-free)
apps/viewer ───► (SDL3)   │                └► libs/sim      (dependency-free)
apps/abtool ──────────────┴──► libs/match, libs/sim, libs/assets
tests ────────► libs/sim (+ doctest)
```

- **libs/assets** (`bomber::assets`) — parsers for the original formats (ANI,
  PCX, SCH, RES lists, RSS) + the install locator. No SDL, no sim knowledge.
  Loaders throw `std::runtime_error`/`std::out_of_range`; treat 1997 files as
  untrusted input (bounds-check everything via `BinaryReader`).
- **libs/sim** (`bomber::sim`) — the deterministic 20 Hz gameplay core.
  `State` is a plain value type (hashable, copyable snapshot); behaviour lives
  in system classes under `src/systems/` (Movement, Bombs, Flames, Powerups,
  Diseases, Enclosure) orchestrated by `Simulation::tick`. System headers are
  private; the public API is `bomber/sim/simulation.hpp`.
- **libs/match** (`bomber::match`) — header-only glue: scheme + VALUELST →
  `MatchConfig`, stage rotation. Keeps assets and sim decoupled.
- **libs/game** (`bomber::game`) — SDL3 presentation: `AssetStore` (textures,
  recoloring), `SequenceSet`, `Renderer`, `AudioEngine`, `SoundDirector`,
  `KeyboardMapper`, `GameApp`. Reads `State` + `events`; never mutates them.
- **apps/** — thin mains: `bomber_game`, `bomber_viewer`, `abtool`.

When adding a gameplay mechanic: put the rules in an existing system (or a
new one under `libs/sim/src/systems/`), wire it in `simulation.cpp`'s tick
order, emit an `Event` for anything the presentation reacts to, map the sound
in `SoundDirector`, and add a doctest suite.

## Determinism contract (do not break)

The sim is deterministic lockstep (`docs/adr/0003`). Rules:

1. Integer math only in `libs/sim` — no floats, no wall clock, no I/O.
2. All randomness through `bomber/sim/rng.hpp` on `State::rng`. The ORDER and
   COUNT of RNG draws per tick is part of the contract.
3. The tick step order in `simulation.cpp` is part of the contract.
4. Every gameplay field of `State`/`Player`/`Bomb` must be mixed into
   `state_hash()` (`libs/sim/src/hash.cpp`). `State::events` are derived
   per-tick outputs: rebuilt every tick, never hashed, never read back by the
   sim.
5. `tests/test_golden.cpp` pins hashes of full scenarios. A refactor must
   keep them byte-identical. A deliberate behaviour change (new RE fact) must
   update the constants in the same commit and cite the `facts.md` entry.
6. Cosmetic randomness (sound picks, death-anim choice, disease flash) uses
   presentation-side RNGs, NEVER `State::rng`.

## Reverse-engineering workflow

- The binary (BM95.EXE, Watcom C, imagebase 0x400000) is the source of truth
  for gameplay. Facts are distilled into `docs/re/facts.md` with the sub_XXXX
  address and evidence; code comments cite those functions.
- NEVER commit exe-derived material: no pseudo.c, no .idb, no disassembly
  dumps, no original assets. Those stay in the BOMBRMAN folder / tmp.
- Every ported mechanic needs: a facts.md entry → a faithful port (mirror the
  original's arithmetic, don't paraphrase it) → tests.
- Constants not yet confirmed against the binary are marked "our tunable" in
  `tuning.hpp` and listed as remaining guesses in facts.md. The former only
  known guess, fuse pause while a bomb is airborne, was confirmed 2026-07-03
  against `sub_42331C`. facts.md's "Still guessed" table now holds one entry
  again: the spawn-pocket clear shape/radius (`libs/sim/src/setup.cpp`) —
  an exhaustive 2026-07-19 search of every writer to the board's tile array
  found no original function that clears bricks around a spawn, so the port
  uses the smallest shape matching live observation instead of a citation.
- VALUELST-driven values go through `Tuning::apply(id, value)`; document ids
  in `docs/valuelst-map.md`.

## Code standards

- C++20, warnings clean under MSVC `/W4` and GCC/Clang `-Wall -Wextra`
  (`bomber::warnings` target). Format with the repo `.clang-format`.
- Naming: `lower_snake` functions/variables, `PascalCase` types,
  `kCamelCase` constants, `member_` trailing underscore, one class/topic per
  file.
- Value semantics and RAII by default. OOP/interfaces live at the boundaries
  (rendering, audio, input, files); the hashed sim state stays a plain
  aggregate — no virtuals, no heap-owning members beyond `std::vector`.
- Systems take `State&` (and other systems) by reference in the constructor —
  cheap stack objects, explicit dependencies, no globals/singletons.
- Comments explain WHY (and cite RE facts); no redundant WHAT comments.
- Tests: doctest, one suite per exe in `tests/`, registered in ctest. New
  gameplay code lands with tests; the full suite must stay green.

## Build & test

```
cmake --preset windows-fetch && cmake --build --preset windows-fetch   # Windows (no vcpkg)
ctest --test-dir build/windows-fetch -C Release --output-on-failure
make run / make viewer / make test / make survey / make deploy         # convenience wrapper
```

Presets: `windows-msvc` (vcpkg), `windows-fetch` (SDL3 via FetchContent),
`headless` (no SDL: sim + abtool + tests only). Runtime verification against
a real install: `abtool survey <game_dir>` and `bomber_viewer <game_dir>
--selftest`. The game auto-detects the install via `BOMBER_GAME_DIR`,
`gamedir.txt`, or the standard paths (`libs/assets/src/install.cpp`).
Renderer output is pinned the same way `test_golden.cpp` pins the sim —
`tests/visual/` (`ctest -R visual_golden`, SKIPs without an install).

A `lefthook` pre-push hook (`lefthook.yml`, `scripts/test.sh`,
`scripts/lint.sh`) runs the `headless` build+ctest and a repo-wide
`clang-tidy` pass (`.clang-tidy`) before every push; see README "Git hooks"
to enable it per clone. `.clang-tidy`'s check list is curated to this
codebase's terse, faithful-port style (bugprone/performance/clang-analyzer,
not broad readability/cppcoreguidelines) — extend it there, not by adding
NOLINTs, unless a specific line is a deliberate one-off.

## Claude working notes

- Sandbox bash sees a STALE view of files rewritten via Write/Edit in earlier
  sessions (truncated or padded to the old size). Host Read is truth. Never
  write to repo files through bash (`sed -i`, `>>`, heredoc) — author with
  the Write/Edit tools, copy to /tmp to build, and re-emit any suspect file
  into the /tmp copy before trusting a build.
- Keep responses in Turkish with Ege; repo docs and comments in English.
- After changing CMake targets, remind Ege to re-run the CMake configure in
  Rider (and drop the CMake cache if targets moved).
