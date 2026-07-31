#pragma once

#include <cstdint>

#include "bomber/core/geometry.hpp"
#include "bomber/core/limits.hpp"

// Timing/cadence constants confirmed against BM95.EXE (docs/re/facts.md). The
// field geometry, the fixed-point pixel unit and the slot/rate limits live in
// bomber::core (ADR-0008) and are re-exported here by using-decls, which name
// the SAME constexpr objects.

namespace bomber::sim {

using core::Fixed;
using core::kGridHeight;
using core::kGridWidth;
using core::kMaxPlayers;
using core::kScale;
using core::kTicksPerSecond;
using core::kTileH;
using core::kTileHF;
using core::kTileW;
using core::kTileWF;

// [0x46494C] = 1000/getvalue(30): the original's 50 ms frame quantum, the
// divisor of every per-frame budget accrual and the step of every anim/state
// ms-accumulator.
inline constexpr std::int32_t kMsPerTick = 1000 / kTicksPerSecond;  // 50

// Canonical frame cadence inside one 20 Hz tick (facts.md "Canonical frame
// cadence", ADR-0006). The original's gameplay driver sub_42A191 runs once per
// DISPLAYED frame with the measured integer-ms delta [0x464958]: timers and anims
// quantize back to 50 ms through per-entity accumulators, but input acquisition,
// AI decisions and the movement-budget accruals genuinely run at display rate. A
// deterministic sim cannot consume measured deltas, so we pin a CANONICAL display
// rate and run those mechanics as a fixed number of sub-frames per tick.
//
// Nine is MEASURED, not extracted — there is no rate in the binary to extract,
// because it free-runs unlocked: DirectDraw's windowed present does not block on
// vblank under DWM. On the reference Win11 box (NVIDIA TITAN X, 60 Hz panel) a
// full 180 s draw round rendered 33146 frames (bmstats "Last Run") — ~184
// gameplay-driver callbacks a second, so those mechanics really do run ~9x per
// tick. The earlier 60 fps / 3-sub-frame choice left the AI ~3x too calm and the
// head-stun ~3x too long: at ~180 fps a 16-frame head stun is ~89 ms, not 267 ms.
// The {6,5,...} pattern sums to the 50 ms tick. Treat this as a calibration
// rather than a citation — it is the single lever for AI/movement "temperature".
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
