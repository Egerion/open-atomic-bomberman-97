#pragma once

#include <cstdint>

// Field geometry and the fixed-point pixel unit — the SDL-free vocabulary shared
// by the sim and the presentation (ADR-0008 libs/core). Confirmed against
// BM95.EXE (docs/re/facts.md): 15x11 grid of 40x36-pixel tiles.
namespace bomber::core {

// Fixed-point unit: 1/100 pixel, matching VALUELST speed units. A bare typedef
// for now; a real `struct Fixed` is a deferred, golden-sensitive follow-up
// (ADR-0008 D7).
using Fixed = std::int32_t;

inline constexpr int kGridWidth = 15;
inline constexpr int kGridHeight = 11;
inline constexpr int kTileW = 40;  // pixels
inline constexpr int kTileH = 36;
inline constexpr Fixed kScale = 100;
inline constexpr Fixed kTileWF = kTileW * kScale;
inline constexpr Fixed kTileHF = kTileH * kScale;

}  // namespace bomber::core
