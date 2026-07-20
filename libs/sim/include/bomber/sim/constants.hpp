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

// [0x46494C] = 1000/getvalue(30): the original's 50 ms frame quantum, the
// divisor of every per-frame budget accrual and the step of every anim/state
// ms-accumulator.
inline constexpr std::int32_t kMsPerTick = 1000 / kTicksPerSecond;  // 50

// Canonical frame cadence inside one 20 Hz tick (docs/re/facts.md "Canonical
// frame cadence", ADR-0006). The original's gameplay driver (sub_42A191) runs
// once per DISPLAYED frame with the measured integer-ms delta [0x464958]:
// timers/anims quantize back to 50 ms through per-entity accumulators, but
// input acquisition, AI decisions and the movement-budget accruals genuinely
// run at display rate. A deterministic sim cannot consume measured deltas, so
// we pin a CANONICAL display rate and run those per-frame mechanics as a fixed
// number of sub-frames per tick.
//
// Rate = ~180 fps (nine sub-frames per 50 ms tick). The original is NOT vsync-
// limited on modern hardware: DirectDraw's windowed present does not block on
// vblank under DWM, so BM95.EXE free-runs at whatever the GPU delivers. On the
// reference Win11 box (NVIDIA TITAN X, 60 Hz panel) a full 180 s draw round
// rendered 33146 frames (bmstats "Last Run"), i.e. ~180 fps of gameplay-driver
// callbacks — so the AI brain, input sampling and movement budget genuinely ran
// ~9x per tick, not 3x. The earlier 60 fps / 3-sub-frame choice (ADR-0006,
// pre-measurement) left the AI ~3x too calm and the head-stun ~3x too long
// versus what the user actually sees. Nine sub-frames restores that: at ~180 fps
// a 16-frame head stun is ~89 ms (was 267 ms at 3 sub-frames), the 30-slot ice
// buffer spans ~167 ms, and the AI re-decides nine times per tick. The {6,5,...}
// pattern sums to the 50 ms tick. (kSubFrames is the single tuning lever for the
// AI/movement "temperature"; drop it toward 3 for a calmer, period-hardware feel.)
inline constexpr int kSubFrames = 9;
inline constexpr std::int32_t kSubFrameMs[kSubFrames] = {6, 5, 6, 5, 6, 5, 6, 5, 6};

// One frame's movement-budget accrual: sub_41F29B / sub_401B5C / sub_42331C's
// `speed * frameDelta / [0x46494C]`, with the original's integer truncation
// kept — at nine sub-frames a stock 923 walker accrues 5x110 + 4x92 = 918 per
// 50 ms, not 923. That sub-1% shortfall is the original's own per-frame
// truncation (finer here than the old 921 at 3 sub-frames), not a port artefact.
constexpr Fixed frame_budget(Fixed speed, std::int32_t delta_ms) {
    return speed * delta_ms / kMsPerTick;
}

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
