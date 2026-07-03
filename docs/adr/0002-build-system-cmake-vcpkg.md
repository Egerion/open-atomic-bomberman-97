# ADR-0002: CMake + vcpkg (manifest mode), C++20

**Status:** Accepted
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

1. [ ] `vcpkg.json` (deps: sdl3), `CMakeLists.txt`, `CMakePresets.json`
2. [ ] README build instructions (Windows + Linux)
3. [ ] GitHub Actions CI once the skeleton compiles
