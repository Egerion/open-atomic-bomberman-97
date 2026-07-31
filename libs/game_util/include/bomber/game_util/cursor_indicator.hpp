#pragma once

#include <cstddef>
#include <cstdint>

// The "little bomber-dude" row cursor shared by the player-setup, options and
// level screens — sub_413BD6 @0x413BD6 (pseudo.c 16691-16721): MISC.ANI's 5-step
// "cursor1" sequence, hotspot-anchored at (list_x - 15, row_y) on the
// player-setup screen and (list_x - 20, row_y) everywhere else. It IDLES on step
// 0 and, when time() reaches a deadline, BLINKS: step jumps to 1 and advances
// one per rendered frame until it wraps back to 0, then the deadline re-arms to
// now + getvalue(690) + rand()%getvalue(691) seconds (VALUELST 690 = {2,2},
// "blink rate of the little bomber-dude cursor (base, random)").
//
// The original keeps ONE global step/deadline pair (dword_460559/dword_46055D)
// across every screen; per-screen instances only de-phase the blink, which is
// invisible. Cosmetic — the roll comes off a local LCG, never the sim's RNG.

namespace bomber::game {

class CursorIndicator {
public:
    // Advance one rendered frame; returns the sequence step to draw. `now_s` is
    // wall-clock seconds, as the original's time_() compare is.
    std::size_t step(std::uint64_t now_s, std::size_t steps, int base_s, int spread_s) {
        if (steps == 0) return 0;
        if (now_s < deadline_) {
            // Mid-blink: one step per rendered frame, wrap to the idle step.
            if (step_ != 0 && ++step_ >= steps) step_ = 0;
            return step_;
        }
        // Deadline reached: start a blink and re-arm. The original draws the
        // rand() only when getvalue(691) > 1 (sub_413BD6 16713).
        step_ = 1 % steps;
        int extra = 0;
        if (spread_s > 1) {
            lcg_ = lcg_ * 1664525u + 1013904223u;
            extra = static_cast<int>((lcg_ >> 16) % static_cast<std::uint32_t>(spread_s));
        }
        deadline_ = now_s + static_cast<std::uint64_t>(base_s > 0 ? base_s + extra : extra);
        return step_;
    }

private:
    std::uint64_t deadline_ = 0;  // seconds; starts elapsed, like dword_46055D's zero-init
    std::size_t step_ = 0;
    std::uint32_t lcg_ = 0x51D0C0DEu;
};

}  // namespace bomber::game
