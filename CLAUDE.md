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
                            THE PRESENTATION STACK (all SDL3 except game_util)

apps/game ────► libs/game ───► libs/netplay ──► libs/frontend ──► libs/editor ─┐
                (GameApp)      lobby/connect/   every screen +    the .SCH      │
                               online match     MatchRunner       editor        │
                                    │                │                │        │
                                    └────────────────┴──► libs/netui ─┴────────►│
                                                          chat, setup link,     │
                                                          F3 panel              │
                                                                                ▼
apps/viewer ──────────────────────────────────────────────────────► libs/ui ────┤
                                                    Screen/ScreenContext, .BM    │
                                                    viewer, dialog chrome        │
                                                                                 ▼
                                          libs/render ◄──────────────► libs/input
                                          textures, sprites,    keyboard/gamepad
                                          sequences, Renderer   (SDL-free headers)
                                                    │                │
                                                    └────────┬───────┘
                                                             ▼
                                                    libs/game_util   ◄── SDL-FREE
                                                    app_flow, results, match
                                                    outcome, editor grid, the
                                                    pixel/pacing rules, log seam

                            THE SDL-FREE CORE (unchanged)

libs/game_util ─► libs/match ──► libs/assets                (no SDL anywhere)
libs/render    ├─► libs/audio ──► libs/sim                  (dependency-free)
libs/frontend  ├─► libs/net   ──┘
libs/netplay   └─► libs/platform (SDL3)
apps/abtool ─────► libs/match, libs/sim, libs/assets
tests ───────────► libs/sim, libs/net, libs/game_util (+ doctest)

libs/core — header-only, depends on NOTHING; anything above may depend on it.

services/matchmaker (Go, separate build) ◄── libs/net's lobby layer, over the
                                             wire only — no code shared
```

The presentation arrows point ONE WAY, bottom to top:

    game_util → {input, render} → ui → netui → editor → frontend → netplay → game

Two of those arrows surprise people, and both are dependency facts rather than
taste. **The editor is BELOW the front end** because the front end opens it (the
main menu and the options screen both do), not the other way round; the generic
modals they share — the help browser, the filename prompt — therefore live in
`libs/ui`, and leaving either in `libs/frontend` is what would make frontend and
editor mutually dependent. **`netui` is BELOW the front end and `netplay` is
ABOVE it**, because the local setup and map-select screens EMBED the chat
overlay and the online setup link, while the lobby/connect/match runner DRIVE
those same screens. One package spanning both halves needs an arrow in each
direction across the same boundary; split where the arrow actually points and
neither half needs the other.

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
  its sources. Depends on assets and sim; only the presentation stack depends on
  it (`libs/render` and everything above).
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
- **The presentation stack** — nine packages, each one CMake target with public
  headers under `include/bomber/<name>/`, split out of what used to be a single
  13k-line `libs/game` holding the renderer, the asset store, the UI chrome,
  input, the editor, every screen, the whole netplay front end and the app shell
  behind one namespace and 73 headers in one directory. All of them read `State`
  + `events` and never mutate them.
  - **libs/game_util** (`bomber::game_util`) — the **SDL-FREE floor**, and the
    only one of the nine the `headless` preset builds. Pure models: the
    front-end state machine (`app_flow`), the match/results bookkeeping
    (`results`, `match_outcome`, `campaign_round_end`, `net_tally`, `net_esc`),
    the pixel and pacing rules the renderer applies (`alpha_bleed`, `key_color`,
    `scale_filter`, `anim_pace`, `carry_pose`), the list-dialog geometry, the
    editor's grid model, and the §11 log seam. Being SDL-free is what puts it
    inside the pre-push gate (see "Build & test"), so `tests/game`'s suites LINK
    or include it rather than recompiling sources. **Grow this package by
    preference**: a rule that can be stated without SDL belongs here, where a
    headless test can pin it. Adding a file that includes SDL breaks `headless`
    immediately, which is the intended feedback.
  - **libs/input** (`bomber::input`) — keyboard, gamepad, DOS-scancode bridge.
    Its public headers are deliberately SDL-free (bindings are plain `int`) so
    the headless suites can pin the binding model; only the three `.cpp` files
    need SDL, which is why the target is SDL-gated but the include dir stands
    alone.
  - **libs/render** (`bomber::render`) — how a pixel gets on screen: the SDL
    RAII shims, `AssetStore` (textures, recolouring), `SequenceSet`, sprite
    banks, and the world `Renderer`. Knows nothing about screens or dialogs.
  - **libs/ui** (`bomber::ui`) — the reusable chrome: the `Screen` primitive
    (`sub_42A088`), the `ScreenContext` bundle every screen is handed, the `.BM`
    text viewer + font textures (`sub_41302D`), the window/dialog/list chrome,
    and the two generic modals (help browser, filename prompt). `BmScreen` is
    here rather than in `render` because `bmscreen.cpp` draws through
    `dialog_chrome` while `dialog_chrome.hpp` includes `bmscreen.hpp` — they are
    one layer, and splitting them would need an arrow back up out of `render`.
  - **libs/netui** (`bomber::netui`) — the net widgets drawn INSIDE another
    screen: the chat overlay, the online setup link + its roster mapping, and
    the F3 diagnostic panel. Below the front end; owns no event loop that
    sequences screens.
  - **libs/editor** (`bomber::editor`) — the `.SCH` scheme editor's SDL surface
    over `game_util`'s grid model.
  - **libs/frontend** (`bomber::frontend`) — every screen the player sees, plus
    `MatchRunner`, the presentation-side driver of the deterministic sim. The
    largest package, and readable as a unit because its twenty-odd classes share
    ONE idiom: take a `ScreenContext`, own a nested SDL event loop, return an
    `AppInput`. `MatchRunner` is here rather than in the shell because it IS
    `AppState::Match` and because `netplay` drives it. GOLDEN-SENSITIVE —
    `match_runner.cpp` reproduces the sim's seeding and per-tick input assembly
    statement-for-statement.
  - **libs/netplay** (`bomber::netplay`) — the online session end to end: lobby,
    connect/host, the best-of-N online match, and the runner that sequences
    them. Top of the stack; nothing in the front end knows it exists.
  - **libs/game** (`bomber::game`) — the APPLICATION SHELL and only that.
    `GameApp`: SDL/window lifetime, the install resolve, the options.ini round
    trip, and the `AppState` loop that hands control to one screen at a time.
  - The C++ NAMESPACE is still `bomber::game` throughout, as it is in
    `libs/audio` — target identity and namespace are already decoupled here, and
    renaming 13k lines of namespace qualification is a separate mechanical
    commit, not part of a move.
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
5. `tests/sim/test_golden.cpp` pins hashes of full scenarios. A refactor must
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
  It is a **RATCHET, not a wall**: `scripts/complexity-baseline.txt` records
  what was already over when the gate was added — 70 functions, worst 251 — and
  they may stay, but none may get worse, and anything new must meet the
  threshold from its first line. The list only shrinks, and it has: as of
  2026-08-01 it is down to **6 entries, worst 58** (`player_turn`), all of them
  in `libs/sim`. Regenerate with
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
link it. The presentation stack is split on the same line and for the same
reason: `bomber::game_util` is declared unconditionally and everything above it
early-returns without the viewer. `libs/platform` is split on that line too, and
only the SDL header falls outside: `frame_pacer.hpp` is pinned headlessly by
`tests/platform` (ctest `frame_pacer`), which adds the include directory rather
than linking the SDL-pulling target; `frame_clock.hpp` is the uncovered half.
What the gate still CANNOT cover is anything that includes SDL —
`audio_engine.cpp`, `frame_clock.hpp`, and the seven SDL presentation packages
(`input`, `render`, `ui`, `netui`, `editor`, `frontend`, `netplay`) plus
`libs/game` and `apps/game|viewer`; those are compiled by CI's
linux/macos/windows-fetch matrix and parsed (with clang, not MSVC) by
`scripts/lint.sh`. Runtime verification against
a real install: `abtool survey <game_dir>` and `bomber_viewer <game_dir>
--selftest`. The game auto-detects the install via `BOMBER_GAME_DIR`,
`gamedir.txt`, or the standard paths (`libs/assets/src/install.cpp`).

Renderer output is pinned the same way `tests/sim/test_golden.cpp` pins the sim
— `tests/visual/` (`ctest -R visual_golden`, SKIPs without an install) — but
**that pin is NOT in the pre-push gate.** It is registered in
`apps/game/CMakeLists.txt`, because it needs the `bomber_game` target, and
`headless` sets `BOMBER_BUILD_VIEWER=OFF`, so the test does not exist in that
build at all (`tests/CMakeLists.txt` says so at the bottom). It runs only in
CI's three SDL presets, where it SKIPs for want of an install. So the
renderer's only regression pin fires exactly when a human runs it against a
real copy of the game — treat a renderer change as unpinned until you have.

A `lefthook` pre-push hook (`lefthook.yml`, `scripts/test.sh`,
`scripts/lint.sh`, `scripts/format.sh`, `scripts/complexity.sh`) runs four
commands in parallel before every push: the `headless` build+ctest, a repo-wide
`clang-tidy` pass (`.clang-tidy`), a `clang-format` check, and the complexity
ratchet. See README "Git hooks"
to enable it per clone. `.clang-tidy`'s check list is curated to this
codebase's terse, faithful-port style (bugprone/performance/clang-analyzer,
not broad readability/cppcoreguidelines) — extend it there, not by adding
NOLINTs, unless a specific line is a deliberate one-off.

Two things `lint.sh` and `complexity.sh` both carry are worth knowing, because
each was a **fourth and fifth instance of the green-having-measured-nothing
shape above**. They pass `--header-filter="(libs|apps)/"`: clang-tidy suppresses
diagnostics outside the main file by default, and `libs/core`, `libs/match` and
`libs/platform` have no `.cpp` between them, so "all N files clean" was true and
misleading in one breath — three whole components had never been read, and the
filter immediately surfaced two over-threshold functions in `match_factory.hpp`.
And they pass `-DBOMBER_HAS_LOBBY=1 -DBOMBER_HAS_LOBBY_TLS=1`, the defines
`libs/net/CMakeLists.txt` sets PUBLIC and every preset turns on; without them
both tools read the `#else` half of every lobby guard and the entire online
stack is invisible to them. Keep those defines in sync with
`libs/net/CMakeLists.txt` — a define the build sets and the gate does not is a
blind spot by construction.

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
- `origin` is SSH (`git@github.com:Egerion/open-atomic-bomberman-97.git`),
  authenticated with a passphrase-less deploy key generated for this purpose
  (2026-09-30). Ege has authorized pushing to `main` without asking first —
  commit and push once the working tree is in a good state, no per-push
  confirmation needed.
