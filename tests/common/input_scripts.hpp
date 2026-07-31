#pragma once

#include <cstddef>
#include <cstdint>

#include "bomber/sim/types.hpp"

// DETERMINISTIC PER-SEAT INPUT SCRIPTS for the bomber::net session suites.
//
// A net session test needs input that is a PURE FUNCTION of (seat, tick): every
// peer re-derives the same value during rollback re-simulation, and no peer has
// to be told what another pressed. It also needs the seats to DIFFER, or
// "predict the remote by repeating its last input" is right by accident and the
// rollback path is never exercised.
//
// THERE ARE THREE SCRIPTS AND THEY ARE NOT INTERCHANGEABLE. Nine suites had
// grown their own copy of a function called `scripted()`, and the copies had
// drifted into three genuinely different signals; collapsing them into one would
// silently change what several suites exercise. They are named here so that a
// call site states which signal it is asserting against.
//
//   scripted_cycle6  period 6, seat offset 3. Five of every six ticks carry a
//                    press and one is idle. Offset 3 is HALF the period, so with
//                    two seats the peers are exactly antiphase — the densest,
//                    most adversarial signal for a prediction test.
//   scripted_cycle8  period 8, seat offset 7. Five of every eight ticks carry a
//                    press and THREE are idle. Offset 7 ≡ −1 (mod 8), so seat N
//                    trails seat N−1 by one tick rather than sitting opposite it.
//                    The longer idle run is what makes it a different test: a
//                    peer that stalls over an idle stretch mispredicts nothing.
//   scripted_held    period 8, seat offset 3, and the only script that HOLDS a
//                    direction across consecutive ticks (left on phases 1-2,
//                    right on 5-6). A one-tick press cannot express a walk that
//                    survives a rollback boundary, so this is the script for
//                    tests about movement continuing across a re-simulation.
//
// Both counted scripts drop a bomb on exactly one phase (action1), which is what
// puts bombs, flames and deaths into an otherwise pure movement run.

namespace bomber::test {

// Lift one seat's input into a full-roster frame; every other seat stays idle.
inline sim::TickInputs seat_input(int seat, const sim::PlayerInput& in) {
    sim::TickInputs t;
    t.players[static_cast<std::size_t>(seat)] = in;
    return t;
}

// Period 6, seats antiphase, one idle phase in six.
inline sim::PlayerInput scripted_cycle6(int seat, std::uint32_t tick) {
    sim::PlayerInput in;
    switch ((tick + static_cast<std::uint32_t>(seat) * 3U) % 6U) {
        case 0: in.right = true; break;
        case 1: in.down = true; break;
        case 2: in.left = true; break;
        case 3: in.up = true; break;
        case 4: in.action1 = true; break;
        default: break;  // idle
    }
    return in;
}

// Period 8, seats one tick apart, three idle phases in eight.
inline sim::PlayerInput scripted_cycle8(int seat, std::uint32_t tick) {
    sim::PlayerInput in;
    switch ((tick + static_cast<std::uint32_t>(seat) * 7U) % 8U) {
        case 0: in.right = true; break;
        case 1: in.down = true; break;
        case 2: in.left = true; break;
        case 3: in.up = true; break;
        case 4: in.action1 = true; break;  // drop bomb
        default: break;                    // idle
    }
    return in;
}

// Period 8, and the directions are HELD for two consecutive ticks so a walk
// spans a rollback boundary instead of ending inside one.
inline sim::PlayerInput scripted_held(int seat, std::uint32_t tick) {
    sim::PlayerInput in;
    const std::uint32_t phase = (tick + static_cast<std::uint32_t>(seat) * 3U) % 8U;
    in.left = phase == 1 || phase == 2;
    in.right = phase == 5 || phase == 6;
    in.up = phase == 3;
    in.down = phase == 7;
    in.action1 = phase == 4;
    return in;
}

}  // namespace bomber::test
