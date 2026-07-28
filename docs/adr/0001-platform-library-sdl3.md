# ADR-0001: SDL3 as platform/graphics/audio library

**Status:** Accepted
**Date:** 2026-07-02
**Deciders:** Ege

## Context

The rewrite needs a C++-friendly layer for window/renderer, 2D sprite blitting (8-bit palettized source art), raw PCM audio playback (2027 headerless 22 kHz s16le stereo `.RSS` files, many concurrent voice clips), and input for up to 10 local players (multiple keyboards + gamepads). Exact-feel replication matters, so low-level control beats convenience.

## Decision

Use **SDL3** (vcpkg port `sdl3`, currently 3.4.10) for platform, rendering, audio, and input. No sdl3-image/mixer needed — PCX/ANI/RSS get custom loaders anyway.

> **How SDL3 is acquired has since changed** (ADR-0002's 2026-07-28 amendment):
> every preset that builds SDL3 — `windows-fetch`, `linux`, `macos` — fetches
> and statically links it from source (`cmake/BomberSDL3.cmake`, tag
> `release-3.4.10`, so the version above is still current). Only `windows-msvc`
> uses the vcpkg port, and `headless` builds no SDL at all. The choice OF SDL3,
> which is what this ADR decides, is unaffected.

## Options Considered

| Dimension | SDL3 | SDL2 | raylib | SFML 3 |
|---|---|---|---|---|
| Maturity | Stable since 2025, active mainline | Most proven, but maintenance mode | Stable | Stable, smaller ecosystem |
| Control level | Low-level ✅ | Low-level ✅ | High-level abstractions | Mid-level |
| Raw PCM audio | Audio streams, ideal ✅ | Callback/queue, fine | Buffer-oriented, less direct | Custom SoundStream subclass |
| Multi-keyboard input | Per-keyboard IDs where OS allows ✅ | No | No | No |
| Remake precedent | Growing | Huge (devilutionX etc.) | Some | Some |
| vcpkg | ✅ | ✅ | ✅ | ✅ |

**SDL3 pros:** current mainline (SDL2 gets fixes only); cleaner API (GPU/renderer, audio streams, properties); per-keyboard event IDs map directly onto Atomic Bomberman's multi-keyboard local multiplayer; trivial later port targets (Linux/macOS/consoles).
**SDL3 cons:** fewer tutorials than SDL2; API still occasionally adjusted between minor versions.

**raylib** rejected: fastest to prototype, but global-state API and higher-level abstractions get in the way of palette handling, audio mixing control, and multi-keyboard input.
**SFML 3** rejected: pleasant C++ API but smallest ecosystem, no advantage for this workload.
**SDL2** rejected: no new features; starting a greenfield project on the maintenance branch means a guaranteed future migration.

## Trade-off Analysis

The real contest was SDL2 vs SDL3. Their APIs are similar enough that skills transfer; choosing SDL3 costs some tutorial availability but avoids a certain SDL2→SDL3 migration later and buys the multi-keyboard feature the original game is famous for.

## Consequences

- Easier: audio (one stream per channel, push raw PCM), input routing per player, future ports, netplay-era timing control
- Harder: occasional need to read SDL3 docs/source instead of Stack Overflow answers
- Revisit: rendering path (SDL_Renderer vs SDL_GPU) once the asset viewer works — SDL_Renderer is the default assumption

## Action Items

1. [ ] Scaffold project with vcpkg manifest depending on `sdl3`
2. [ ] Asset viewer window + texture upload of decoded PCX as first SDL3 usage
