#pragma once

#include <array>
#include <cstdint>

#include "bomber/sim/types.hpp"

// The Goldman Roulette wheel's PURE math — geometry, spin setup, per-frame
// stepping, prize resolution — mirrored from `sub_4034BC` @0x4034BC
// (docs/re/goldman-roulette.md §3). Everything here is PRESENTATION randomness
// (a private LCG, never bomber::sim::State::rng), so the wheel has no bearing on
// the determinism contract (ADR-0003).

namespace bomber::game {

// Angular circle resolution: T = 6 * getvalue(1004), split into 6 segments of
// T/6 steps each (doc §3 "Geometry"). VALUELST 1004 = 70 -> T = 420.
inline constexpr int kWheelSegments = 6;
inline constexpr int kWheelSegmentSteps = 70;  // getvalue(1004) fallback
inline constexpr int wheel_circle_steps(int segment_steps = kWheelSegmentSteps) {
    return kWheelSegments * segment_steps;
}

// The six wheel slots as inventory powerup ids (dword_45B7BC, pseudo.c 1934):
// extra bomb, flame, kick, goldflame, skate, clogs (13, the booby prize —
// outside our 13-kind bomber::sim::PowerupType space, doc §8).
inline constexpr int kClogsPrizeId = 13;
inline constexpr std::array<int, kWheelSegments> kWheelPrizeIds = {0, 1, 3, 8, 4, 13};

// Maps a wheel prize id (0/1/3/4/8) onto our sim::PowerupType. None for 13
// (clogs) PERMANENTLY: clogs is not a scheme/-P/spawn/forbid kind (doc §8/§9.2)
// and is ported as MatchConfig::born_with_clogs feeding Player::clogs directly
// (doc §9.4). Callers must route kClogsPrizeId to that path BEFORE calling this
// — None here is "not a PowerupType by design", not "not yet ported".
constexpr bomber::sim::PowerupType wheel_prize_to_powerup(int prize_id) {
    switch (prize_id) {
        case 0: return bomber::sim::PowerupType::ExtraBomb;
        case 1: return bomber::sim::PowerupType::Flame;
        case 3: return bomber::sim::PowerupType::Kick;
        case 4: return bomber::sim::PowerupType::Skate;
        case 8: return bomber::sim::PowerupType::Goldflame;
        default: return bomber::sim::PowerupType::None;  // 13 = clogs, no kind yet
    }
}

// The wheel's phase, mirroring dword_45E034 (0 free spin / 1 winding down /
// 2 settled).
enum class WheelPhase : std::uint8_t { FreeSpin = 0, WindingDown = 1, Settled = 2 };

// One mover's rotation position + remaining step budget (wheel or ring).
struct WheelMover {
    int pos = 0;     // dword_45E024 (wheel) / dword_45E038 (ring)
    int budget = 0;  // dword_45E030 (wheel) / dword_45E03C (ring)
};

// The whole spin state (dword_45E024/28/2C/30/34/38/3C). A plain aggregate so
// tests can construct/compare it directly.
struct WheelState {
    int direction = 1;      // dword_45E028, +-1
    WheelMover wheel;       // dword_45E024/30
    WheelMover ring;        // dword_45E038/3C
    WheelPhase phase = WheelPhase::FreeSpin;  // dword_45E034
    int result = -1;        // dword_45E02C, prize id, -1 while spinning/aborted
};

// A tiny presentation LCG, the same shape as pick_glue()/panic_roll() in
// game_app.cpp/renderer.cpp — never bomber::sim::State::rng.
struct WheelRng {
    std::uint32_t state = 0x5E7C0DE5u;
    std::uint32_t next() {
        state = state * 1664525u + 1013904223u;
        return state >> 16;
    }
};

// Setup — EXACTLY 5 rand() draws, in this fixed order (doc §3 "Setup"):
//   1. direction = 2*(rand%2) - 1
//   2. wheel.pos = rand % T
//   3. ring.pos  = rand % T
//   4. wheel.budget = rand%20 + 20            (20..39)
//   5. ring.budget  = wheel.budget + rand%20  (>= wheel's)
inline WheelState spin_setup(WheelRng& rng, int circle_steps = wheel_circle_steps()) {
    WheelState w;
    w.direction = 2 * static_cast<int>(rng.next() % 2) - 1;
    w.wheel.pos = static_cast<int>(rng.next() % static_cast<std::uint32_t>(circle_steps));
    w.ring.pos = static_cast<int>(rng.next() % static_cast<std::uint32_t>(circle_steps));
    w.wheel.budget = static_cast<int>(rng.next() % 20) + 20;
    w.ring.budget = w.wheel.budget + static_cast<int>(rng.next() % 20);
    w.phase = WheelPhase::FreeSpin;
    w.result = -1;
    return w;
}

// Steps one mover by one frame (doc §3 "Per frame"): moves
// max(budget, 6) steps this frame, each step +-1 (wrapping mod circle_steps);
// every time it lands on a segment boundary (pos % segment_steps == 0) the
// caller is told (tick_boundary out-param, used for SFX 1300 on the ring) —
// and, ONLY in WindingDown phase, the budget is decremented on that
// boundary, until it hits 0 (the mover then halts exactly on a boundary).
// `dir_sign` is the mover's own direction: the ring steps by +direction, the
// wheel by -direction (doc §3), so callers pass the already-signed value.
inline bool step_mover(WheelMover& m, WheelPhase phase, int dir_sign, int circle_steps,
                       int segment_steps = kWheelSegmentSteps) {
    if (m.budget <= 0) return false;
    bool crossed_boundary = false;
    int steps_this_frame = m.budget > 6 ? m.budget : 6;
    for (int i = 0; i < steps_this_frame; ++i) {
        m.pos += dir_sign;
        m.pos %= circle_steps;
        if (m.pos < 0) m.pos += circle_steps;
        if (m.pos % segment_steps == 0) {
            crossed_boundary = true;
            if (phase == WheelPhase::WindingDown) {
                if (--m.budget == 0) break;  // halts exactly on the boundary
            }
        }
    }
    return crossed_boundary;
}

// Result resolution (doc §3 "Result resolution"), once both budgets are 0:
//   offset = ring - wheel; if (ring < wheel) offset += T;
//   prize  = kWheelPrizeIds[offset / segment_steps];
inline int resolve_prize(const WheelState& w, int circle_steps = wheel_circle_steps(),
                         int segment_steps = kWheelSegmentSteps) {
    int offset = w.ring.pos - w.wheel.pos;
    if (w.ring.pos < w.wheel.pos) offset += circle_steps;
    int slot = offset / segment_steps;
    if (slot < 0) slot = 0;
    if (slot >= kWheelSegments) slot = kWheelSegments - 1;
    return kWheelPrizeIds[static_cast<std::size_t>(slot)];
}

}  // namespace bomber::game
