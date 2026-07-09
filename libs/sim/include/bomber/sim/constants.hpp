#pragma once

#include <cstdint>

// Field geometry and timing constants confirmed against BM95.EXE
// (docs/re/facts.md): 15x11 grid of 40x36-pixel tiles, 20 Hz nominal rate.

namespace bomber::sim {

// Fixed-point unit: 1/100 pixel, matching VALUELST speed units.
using Fixed = std::int32_t;

inline constexpr int kTicksPerSecond = 20;
inline constexpr int kMaxPlayers = 10;
inline constexpr int kGridWidth = 15;
inline constexpr int kGridHeight = 11;
inline constexpr int kTileW = 40;  // pixels
inline constexpr int kTileH = 36;
inline constexpr Fixed kScale = 100;
inline constexpr Fixed kTileWF = kTileW * kScale;
inline constexpr Fixed kTileHF = kTileH * kScale;

// Order matches the scheme -P table and the VALUELST id blocks (50/400/550).
inline constexpr int kPowerupKinds = 13;

// The nine skull diseases, in the original's rand()%9 index order.
inline constexpr int kDiseaseKinds = 9;

// Campaign-only grace period after the last rover/ghost dies, ticks
// (docs/re/campaign.md "Round pacing" clause 3). The original expresses this
// as wall-clock ms (`2 * dword_46494C(50ms) * getvalue(25)(20)` = 2000ms);
// at our locked 20 Hz that is exactly 40 ticks, so the port counts ticks
// directly instead of reproducing the ms/frame-delta indirection.
inline constexpr int kHazardClearTicks = 2 * kTicksPerSecond;  // 40

}  // namespace bomber::sim
