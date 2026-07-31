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
apps/viewer ───► (SDL3)   ├──► libs/audio  ─► libs/sim      (dependency-free)
                          ├──► libs/net   ──┘
                          └──► libs/platform (SDL3)
apps/abtool ─────────────────► libs/match, libs/sim, libs/assets
tests ───────────────────────► libs/sim, libs/net (+ doctest)

libs/core — header-only, depends on NOTHING; anything above may depend on it.

services/matchmaker (Go, separate build) ◄── libs/net's lobby layer, over the
                                             wire only — no code shared
```

- **libs/core** (`bomber::core`) — SDL-free, dependency-free shared vocabulary
  (ADR-0008): the fixed-point pixel unit + field geometry, slot/rate limits,
  scoped-enum cast helpers. Header-only, at the bottom of the graph.
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
- **libs/net** (`bomber::net`) — online multiplayer (ADR-0010 core, ADR-0011
  lobby). SDL-free and I/O-confined; consumes `libs/sim`'s value types and
  `state_hash` and NEVER leaks sockets into it (determinism rule 1) — the sim
  only ever sees a fully-assembled `TickInputs`. Three layers:
  - *in-match*: the per-tick input codec + typed `protocol` messages, the
    `LockstepSession` (input-delay) and `RollbackSession` (GGPO predict/re-sim,
    plus the host-scheduled peer-drop → AI handoff), the `SeedHandshake`.
  - *transports*, all behind one abstract `Transport` so the session above is
    identical whichever wins: `UdpTransport` (raw winsock/BSD), `LoopbackLink`
    (headless tests), `RelayedTransport` (the server-forwarded fallback when a
    punch fails), `StarHubTransport` (the >2-seat hub's fan-out + reflection).
  - *pre-match*: `LobbyClient` (WebSocket control plane), `StunClient`,
    `Rendezvous` (the NAT punch), `LinkProbe` (the MUTUAL path proof — a punch
    that only proves one direction used to start a match one side could not
    play, and the winner then stopped echoing, which made the loser's failure
    certain rather than unlucky), `SetupSession` (the host's authoritative
    `MatchConfig` over the wire), and `LobbyFlow`, the state machine that
    drives all of it. `build_hash` is the cross-build door both peers check.
    These are behind `BOMBER_ENABLE_LOBBY`, whose OFF *default* lets a consumer
    build the deterministic core with no WS/JSON/TLS deps — but every preset
    including `headless` pins it ON, so that is not a configuration this repo
    builds or gates on (see "Build & test").
- **libs/audio** — the audio module, extracted from `libs/game` (ADR-0008 stage
  2), and TWO targets split on the one header that includes SDL:
  `bomber::audio_core` is `SoundBank` (the SDL-FREE selection engine — group
  picks, least-played ordering, the load-time cull; a faithful port of
  `sub_427961` & friends) plus `SoundDirector` (sim `Event`s → SOUNDLST id
  ranges), and `bomber::audio` adds `AudioEngine` (SDL PCM stream pool, music +
  SFX) on top. The split is what puts the SDL-free half inside the pre-push gate
  (see "Build & test"); `tests/audio` links `audio_core` rather than recompiling
  its sources. Depends on assets and sim; nothing depends on it but `libs/game`.
  Its cosmetic RNG is its own and never touches `State::rng` (determinism rule
  6). Note the namespace is still `bomber::game` pending a mechanical rename.
  `AudioEngine` exposes the binary's three play primitives — `play`
  (`sub_427961`, group pick, counted), `play_exact` (`sub_4278F2`) and
  `play_sting` (`sub_427BFB`, the UNCOUNTED voice the concurrency cap cannot
  refuse). All four screen stings — title 2800, menu-quit 2600, draw 1700,
  winner 2000 — go through `play_sting`; two of them used to go through the
  counted pool, where a busy results transition could drop them.
- **libs/platform** (`bomber::platform`) — the engine-base layer (ADR-0008):
  frame clock and pacing, game-agnostic, so screens never re-implement the main
  loop. Header-only: `FrameClock` (SDL-backed) over `FramePacer`, which is
  SDL-FREE and therefore unit-testable (`tests/platform`) — the pacing DECISION
  is a pure function of the last frame's end time, so a change to it is pinned
  by a headless test rather than by eyeballing an FPS counter.
  The uncapped (F8) path targets the next **sub-frame instant**, not a
  wall-clock period. That distinction is load-bearing and was measured: a
  conventional catch-up pacer reaches 180 fps by re-presenting sub-frames it has
  already shown (265 of 1081 presents were duplicates), so the counter improves
  while the motion does not. Renderer interpolation quantises a tick into
  exactly `kSubFrames` steps, so the only honest target is one distinct step per
  present. The number on the overlay therefore means "distinct images
  delivered" and will read below 180 on a machine that cannot make them.
- **libs/game** (`bomber::game`) — SDL3 presentation and the front-end:
  `AssetStore` (textures, recoloring), `SequenceSet`, `Renderer`,
  `KeyboardMapper`, the per-screen classes under `src/screens/` (ADR-0009), and
  `GameApp`. Reads `State` + `events`; never mutates them.
- **services/matchmaker** — a small Go service (NOT part of the C++/CMake
  build) that introduces peers and relays for the ones whose NAT refuses a
  direct path. It never simulates and never sees `State`.
  `services/matchmaker/PROTOCOL.md` is the FROZEN wire contract both sides are
  written against — change it on one side only and you break a deployed game.
- **apps/** — thin mains: `bomber_game`, `bomber_viewer`, `abtool`.

When adding a gameplay mechanic: put the rules in an existing system (or a
new one under `libs/sim/src/systems/`), wire it in `simulation.cpp`'s tick
order, emit an `Event` for anything the presentation reacts to, map the sound
in `SoundDirector` (`libs/audio`), and add a doctest suite.

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
7. A sim behaviour change must MOVE `build_hash` (`libs/net/src/build_hash.cpp`)
   — that digest is the only thing stopping two disagreeing builds from meeting
   at the lobby door and desyncing mid-match. It is one fixed scenario per
   MECHANIC CLASS, and it is only as good as what those scenarios EXECUTE, so
   after a behaviour change build the digest before and after and confirm it
   moved. Coverage by coincidence is not coverage: three separate fixes have
   left it byte-identical, most recently the warphole/trampoline trigger, whose
   mechanic *was* present in two scenarios — they PLACED actors without ever
   driving a player onto one. Placement is not coverage either. `kSubFrames`
   pacing and other presentation work do NOT belong here; only `libs/sim`.

## Reverse-engineering workflow

- The binary (BM95.EXE, Watcom C, imagebase 0x400000) is the source of truth
  for gameplay. Facts are distilled into `docs/re/facts.md` with the sub_XXXX
  address and evidence; code comments cite those functions.
- NEVER commit exe-derived material: no pseudo.c, no .idb, no disassembly
  dumps, no original assets. Those stay in the BOMBRMAN folder / tmp.
- Every ported mechanic needs: a facts.md entry → a faithful port (mirror the
  original's arithmetic, don't paraphrase it) → tests.
- **facts.md's "Still guessed" table is EMPTY.** Every gameplay constant in
  `tuning.hpp` now traces to a `sub_XXXX` or an asset. The last two entries
  closed in different ways, and the difference matters:
  - *Fuse pause while a bomb is airborne* — confirmed 2026-07-03 against
    `sub_42331C`. A genuine extraction: the guess became a citation.
  - *Spawn-pocket clear* (`libs/sim/src/setup.cpp`) — closed 2026-07-21 in the
    OPPOSITE direction, and it left the table rather than being confirmed. A
    native `sub_4260F5` fill probe showed the original puts a brick on the
    spawn ~90% of the time and never clears it, so **the port's radius-2 clear
    is a proven live divergence, not a missing citation.** Do not repeat the
    older 2026-07-19 justification ("an exhaustive search found no function
    that clears bricks around a spawn, so the port uses the smallest shape
    matching live observation") — facts.md retracts both halves of it: the
    mechanism is proven absent rather than merely unfound, and the "BM95
    screenshots show ~2-tile pockets" observation is itself marked suspect. The
    clear stays only as a workaround for a clean-room AI-flee bug the
    original's AI does not have, so the open item is a PORT fix, not an RE
    extraction.
- Not every constant is pinned to an address, and the ones that are not say so
  where they live rather than in a table. `kSubFrames = 9`
  (`libs/sim/include/bomber/sim/constants.hpp`) is the notable one: the
  original's per-frame mechanics run at display rate and it free-runs unlocked,
  so there is no rate to extract. Nine is **measured** — 33146 frames over a
  180 s round on the reference Win11 box, ~184 gameplay-driver callbacks a
  second — and pinned as a canonical rate a deterministic sim can consume
  (ADR-0006's 2026-07-16 amendment). Treat it as a calibration, not a citation.
- VALUELST-driven values go through `Tuning::apply(id, value)`; document ids
  in `docs/valuelst-map.md`.

## Code standards

**`docs/coding-standards.md` is the long form** — adopted 2026-07-31 and
reconciled with this repo's two hard contracts. Read it before a refactor. Its
load-bearing amendments, because a general C++ guideline gets them wrong here:
OO and dependency inversion apply at the BOUNDARIES and `libs/sim` must stay a
plain virtual-free aggregate (§4); "replace a large switch with a strategy" does
NOT apply to a switch that mirrors `sub_XXXX`'s dispatch (§8); value semantics
are preferred over `unique_ptr`-by-default (§6); and the naming convention below
is deliberately kept rather than modernised (§2). What follows is the summary.

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
- Size and shape: class ≤200 lines, function 5–20 lines (40 hard ceiling),
  **≤3 parameters** (4 hard ceiling — past that, pass a parameter object the way
  `MatchConfig`/`TurnContext`/`ScreenContext` already do), guard clauses over
  nesting, no `else` after a `return`/`continue`/`break`, nesting ≤3,
  inheritance depth ≤3. These are targets a reviewer measures, not compiler
  errors.
- **Complexity is gated** (`scripts/complexity.sh`, in the pre-push hook), and
  the metric is clang-tidy's *cognitive* complexity rather than raw McCabe
  cyclomatic, threshold **25** (≈ cyclomatic 15). The difference is deliberate:
  cognitive complexity charges NESTING — the same branch costs more three levels
  down — and charges nothing for a flat `switch` over an enum. This codebase is
  full of faithful ports of the original's dispatch tables, which McCabe scores
  as catastrophic while a reader walks them without effort; what needs pushing
  down here is tangle, not arm count.
  It is a **RATCHET, not a wall**: `scripts/complexity-baseline.txt` records the
  66 functions that were already over when the gate was added (worst: 251), and
  they may stay — but none may get worse, and anything new must meet the
  threshold from its first line. The list only shrinks. Regenerate with
  `scripts/complexity.sh --update` AFTER a refactor lands, never to make a red
  gate go green; a number going up in that diff is the ratchet failing open.
  A shape that genuinely mirrors the binary's control flow is an exception —
  keep it, cite the `sub_XXXX`, and re-baseline deliberately.
- Tests: doctest, one suite per exe in `tests/`, registered in ctest. New
  gameplay code lands with tests; the full suite must stay green.

## Build & test

```
cmake --preset windows-fetch && cmake --build --preset windows-fetch   # Windows (no vcpkg)
ctest --test-dir build/windows-fetch -C Release --output-on-failure
make run / make viewer / make test / make survey / make deploy         # convenience wrapper
```

Presets: `windows-fetch` (SDL3 via FetchContent — the default everywhere:
the Makefile, CI, and the README's build instructions), `linux` and `macos`
(the same, per-platform), `windows-msvc` (the one preset that uses the vcpkg
toolchain, and only for SDL3), and `headless`.

`headless` means **no SDL** — not "no dependencies". It builds the sim,
`abtool`, the tests **and the full lobby stack**: all five presets pin
`BOMBER_ENABLE_LOBBY=ON` and `BOMBER_LOBBY_TLS=ON`, so `headless` fetches
IXWebSocket, nlohmann/json and mbedTLS and compiles mbedTLS from source. Both
pins are deliberate and both were bugs before they were pins. `ENABLE_LOBBY`
OFF put the whole lobby half of `tests/net` inside a skipped
`if(BOMBER_ENABLE_LOBBY)` block, so the pre-push gate never ran it and a broken
suite reached `main`. `LOBBY_TLS` OFF is worse: `test_lobby_tls.cpp` compiles
to its "this build has no TLS support" variant, the certificate- and
hostname-rejection cases are preprocessed away, and the suite PASSES — green,
having verified nothing. `libs/audio` was the third case of the same shape: it
was declared only inside the root `CMakeLists.txt`'s `BOMBER_BUILD_VIEWER`
block, so `headless` compiled none of it, and `tests/audio` papered over half of
that by recompiling `sound_bank.cpp`/`sound_director.cpp` into its own binaries.
Its SDL-free half is now the always-built `bomber::audio_core` and the suites
link it. What the gate still CANNOT cover is anything that includes SDL —
`audio_engine.cpp`, `libs/game`, `libs/platform`, `apps/game|viewer`; those are
compiled by CI's linux/macos/windows-fetch matrix and parsed (with clang, not
MSVC) by `scripts/lint.sh`. Runtime verification against
a real install: `abtool survey <game_dir>` and `bomber_viewer <game_dir>
--selftest`. The game auto-detects the install via `BOMBER_GAME_DIR`,
`gamedir.txt`, or the standard paths (`libs/assets/src/install.cpp`).
Renderer output is pinned the same way `test_golden.cpp` pins the sim —
`tests/visual/` (`ctest -R visual_golden`, SKIPs without an install).

A `lefthook` pre-push hook (`lefthook.yml`, `scripts/test.sh`,
`scripts/lint.sh`, `scripts/format.sh`) runs the `headless` build+ctest, a
repo-wide `clang-tidy` pass (`.clang-tidy`) and a `clang-format` check before
every push; see README "Git hooks"
to enable it per clone. `.clang-tidy`'s check list is curated to this
codebase's terse, faithful-port style (bugprone/performance/clang-analyzer,
not broad readability/cppcoreguidelines) — extend it there, not by adding
NOLINTs, unless a specific line is a deliberate one-off.

`scripts/format.sh` checks only the LINES a push changes, not whole files:
nothing ran `clang-format` until it was added and 170 of 333 `.cpp`/`.hpp` files
had drifted (~3.5k lines), so a whole-repo pass would bury every real change and
would reflow faithful-port layouts nobody asked it to touch. Pre-existing
violations in a file you edit therefore do not block you; the lines you write
do. For a block that deliberately mirrors the binary's structure, wrap it in
`// clang-format off` / `// clang-format on` rather than skipping the gate.

## Claude working notes

- Sandbox bash sees a STALE view of files rewritten via Write/Edit in earlier
  sessions (truncated or padded to the old size). Host Read is truth. Never
  write to repo files through bash (`sed -i`, `>>`, heredoc) — author with
  the Write/Edit tools, copy to /tmp to build, and re-emit any suspect file
  into the /tmp copy before trusting a build.
- Keep responses in Turkish with Ege; repo docs and comments in English.
- After changing CMake targets, remind Ege to re-run the CMake configure in
  Rider (and drop the CMake cache if targets moved).
