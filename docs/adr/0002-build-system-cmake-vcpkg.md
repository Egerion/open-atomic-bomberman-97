# ADR-0002: CMake + vcpkg (manifest mode), C++20

**Status:** Accepted — amended 2026-07-28 (vcpkg → FetchContent as the default
dependency path; see "Amendment" below). The C++20 half of this ADR still holds
in full.
**Date:** 2026-07-02
**Deciders:** Ege

## Context

Open-source repo, primary dev on Windows/MSVC, should stay cross-platform-friendly. Dependencies are few (SDL3, later maybe a test framework).

## Decision

CMake ≥3.28 with **vcpkg in manifest mode** (`vcpkg.json` in repo, toolchain via `CMAKE_TOOLCHAIN_FILE` or `VCPKG_ROOT`). Language standard **C++20**. CMake presets (`CMakePresets.json`) for one-command configure on MSVC and Linux/gcc.

## Options Considered

**CMake + vcpkg (chosen)** — binary caching (fast CI/dev), manifest pins deps per-repo, first-class MSVC + SDL3 support. Con: contributors need vcpkg installed (mitigated by presets + README).
**CMake + FetchContent** — zero external tooling, but SDL3 builds from source every fresh clone (~minutes) and transitive config is manual. Kept as documented fallback; nothing prevents adding it later behind an option.
**Conan** — powerful but heavier setup for a two-dependency project.

## Consequences

- Easier: adding deps (fmt, doctest), CI on GitHub Actions (vcpkg binary cache action exists)
- Harder: first-time contributor setup has one extra step
- C++20 gives designated initializers, `std::span`, ranges, `constexpr` improvements — MSVC/gcc/clang all solid. C++23 revisit later if a feature pulls its weight.

## Action Items

1. [x] `vcpkg.json` (deps: sdl3), `CMakeLists.txt`, `CMakePresets.json`
2. [x] README build instructions (Windows + Linux)
3. [x] GitHub Actions CI once the skeleton compiles

## Amendment (2026-07-28): FetchContent is the default; vcpkg is the fallback

**The decision above is inverted relative to the build that exists.** Everything
in "Options Considered" is still an accurate account of what was weighed in July
2026; what changed is which option won in practice. Recording it as an amendment
rather than a rewrite, because the reversal was gradual and was never decided in
one sitting — it is worth being able to see that it drifted.

**What the tree does now.**

- Of five presets in `CMakePresets.json`, **one** (`windows-msvc`) references the
  vcpkg toolchain. `windows-fetch`, `linux` and `macos` set
  `BOMBER_FETCH_SDL3=ON`; `headless` builds no SDL at all.
- Of five external dependencies, **one** (SDL3) can come from vcpkg. IXWebSocket,
  mbedTLS, nlohmann/json and doctest are FetchContent-only — so even on
  `windows-msvc`, four of five come from FetchContent, and `vcpkg.json`
  (unchanged since it was written, listing `sdl3` alone) can no longer describe
  the dependency set.
- CI runs `linux`, `macos` and `windows-fetch`. There is no vcpkg job and no
  binary cache, which is the specific advantage this ADR chose vcpkg for.
- `Makefile` defaults to `PRESET ?= windows-fetch` on Windows and hardcodes
  `-DBOMBER_FETCH_SDL3=ON` off it; it never mentions vcpkg.

**How it got here.** The drift did not start after the decision — it started
inside it. The same commit that added this ADR (`a00387b`, 2026-07-03) also added
`BOMBER_FETCH_SDL3`, the `windows-fetch` preset, and a Makefile already
defaulting to it, so the "documented fallback" was the default from day one. It
then hardened three times: `d36bb45` (2026-07-10) added the `linux`/`macos`
presets and a CI matrix that has never exercised vcpkg; `f0f7b97` (2026-07-24)
made a statically-linked, single-file `OPEN-BM95.exe` the packaging contract,
which vcpkg's default dynamic port does not give; and `4dacc25` (2026-07-24)
added three dependencies via FetchContent by explicit constraint
(`cmake/BomberIXWebSocket.cmake`: "FetchContent-only — no vcpkg, no system
packages"). That last one is the honest "superseded as of" date, because it is
where the manifest stopped being able to express the build.

**Why it went that way, stated as the decision it should have been.** The con
this ADR assigned to FetchContent — SDL3 building from source on a fresh clone —
turned out to be cheap and to buy the thing the project cares about more: a clone
that needs nothing but CMake and a compiler, and a static link that ships as one
self-contained executable. The pro assigned to vcpkg — binary caching in CI — was
never claimed. "Contributors need vcpkg installed" is a real cost for a repo
whose point is that a stranger can clone and build it.

**Consequently amended:**

- `windows-msvc` is retained as a convenience for developers who already have a
  vcpkg SDL3, not as the supported path. `vcpkg.json` is kept in step with that
  single dependency and is not the manifest of record.
- CMake ≥ **3.25**, not 3.28 (`CMakeLists.txt`, `README.md`). The ADR's figure
  was never what was enforced.
- "a two-dependency project" is out of date: five external dependencies today.
- The C++20 decision (Consequences, third bullet) is untouched and still holds —
  `CMakeLists.txt` sets C++20, required, extensions off.
- **Not amended here:** ADR-0001 carries the same vcpkg framing for SDL3
  ("vcpkg port `sdl3`"); its SDL3-vs-alternatives decision is unaffected, but its
  acquisition wording inherits this amendment.
