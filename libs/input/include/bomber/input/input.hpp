#pragma once

#include <array>
#include <cstdint>

#include "bomber/sim/types.hpp"

// Input mapping: keyboard state -> TickInputs, plus the setup screen's pure
// slot/roster rules (docs/re/results-and-options.md §2, docs/re/setup-screens.md).
//
// SDL-FREE BY CONSTRUCTION, and that is the package's contract rather than a
// convenience: bindings are plain `int` holding an SDL_Scancode, cast back at
// the read()/game_app.cpp boundary, so every rule below is compiled and pinned
// by the headless preset (tests/game's `frontend` suite links no SDL3). Only
// input.cpp's SDL_GetKeyboardState half needs the library.

namespace bomber::game {

// The 6 bindable actions per keyboard set, in §2's action-name id order
// (1120 Move Up .. 1125 Action 2).
enum class KeyAction : std::uint8_t { Up, Right, Down, Left, Action1, Action2, kCount };
inline constexpr int kKeyActionCount = static_cast<int>(KeyAction::kCount);
inline constexpr int kKeyboardSets = 2;

// One keyboard set's 6 bindings, each an SDL_Scancode stored as `int`.
struct KeySet {
    std::array<int, kKeyActionCount> scancode;
};

class KeyboardMapper {
public:
    KeyboardMapper();

    sim::TickInputs read() const;

    // The live bindings for keyboard set `set` (0 or 1). The key-remap screen
    // edits a COPY and calls set_key_set() once the player leaves it.
    const KeySet& key_set(int set) const { return sets_[set & 1]; }
    void set_key_set(int set, const KeySet& ks) { sets_[set & 1] = ks; }

private:
    std::array<KeySet, kKeyboardSets> sets_;
};

// The original's own defaults — sub_40614A @0x40614A, CONFIRMED from the body
// 2026-07-13, which CORRECTS §2's earlier misread ("200/205/208/203/57/46" and
// "17/32/31/30/2/3"). Set 0 = arrows + Space + Enter (DOS 200/205/208/203/57/
// 28); set 1 = the R/G/F/D diamond + S + A (DOS 19/34/33/32/31/30). The
// original swaps A for Q under the BIOS AZERTY global dword_4A2CA4 == 1, which
// has no SDL analogue. dos_scancode.hpp translates at the options.ini `keydef=`
// boundary so a shared install stays interchangeable with BM95.EXE.
KeySet default_key_set(int set);

// Scripted inputs for the headless --demo mode: both players walk squares and
// drop bombs periodically. Inline (not in input.cpp) because it is the one
// piece of this package with no SDL in it, and a header is where the pre-push
// gate can reach it.
inline sim::TickInputs demo_inputs(int t) {
    sim::TickInputs in;
    const int phase = (t / 25) % 4;
    auto& p0 = in.players[0];
    p0.right = phase == 0;
    p0.down = phase == 1;
    p0.left = phase == 2;
    p0.up = phase == 3;
    p0.action1 = (t % 50) == 24;
    auto& p1 = in.players[1];
    p1.left = phase == 0;
    p1.up = phase == 1;
    p1.right = phase == 2;
    p1.down = phase == 3;
    p1.action1 = (t % 60) == 30;
    return in;
}

// The PLAYER INPUT TYPE SELECTION slot categories (docs/re/setup-screens.md,
// sub_421DD2's player byte +16). Mirrors the original's numbering so the setup
// screen's switches read directly against it.
enum class SlotInputType : std::uint8_t {
    Off = 0,
    Computer = 1,
    Keyboard = 2,
    Joystick = 3,
    Other = 4
};

// ATTRACT-MODE roster/stage rolls (sub_410F81's attract branch, pseudo.c
// 15125-15143). `rand()%10 + 1`, clamped to a MINIMUM of 3. Both rolls are
// presentation-side: the caller supplies raw LCG draws, never State::rng.
constexpr int attract_computer_count(unsigned roll) {
    const int n = static_cast<int>(roll % 10) + 1;
    return n < 3 ? 3 : n;
}

// sub_410F81's attract branch sets every slot OFF, then flips exactly
// `computer_count` of them in slot order, and zeroes every team byte
// ("forces team play off").
template <std::size_t N>
constexpr void fill_attract_roster(int computer_count, std::array<int, N>& type,
                                   std::array<int, N>& sub, std::array<int, N>& team) {
    for (std::size_t i = 0; i < N; ++i) {
        const bool on = static_cast<int>(i) < computer_count;
        type[i] =
            on ? static_cast<int>(SlotInputType::Computer) : static_cast<int>(SlotInputType::Off);
        sub[i] = 0;
        team[i] = 0;
    }
}

// Attract-mode stage pick: `rand() % level_count` DIRECTLY, BYPASSING the
// VALUELST 1150-1160 random-level enable flags a normal RANDOM pick honours —
// so attract can land on a stage the player disabled. `level_count` is
// getvalue(35) (11 in the shipped VALUELST).
constexpr int attract_stage_pick(unsigned roll, int level_count) {
    if (level_count < 1) level_count = 1;
    return static_cast<int>(roll % static_cast<unsigned>(level_count));
}

// Per-slot TEAM default on EVERY entry to the setup screen: alternating
// 0/1/0/1 by slot parity, NOT a flat 0 (docs/re/setup-screens.md "TEAM default
// — CORRECTED"; sub_4049C0 at pseudo.c 6716 writes the slot index's low bit
// into each slot's team field, and sub_410F81 re-applies it through
// sub_4046CC -> sub_403EEE -> sub_4049C0 before the screen draws a frame).
// This port's original flat 0 put every player on the SAME sim side whenever
// Team Play was on and nobody pressed 'T'.
constexpr int default_setup_team(int slot) {
    return slot & 1;
}

template <std::size_t N>
constexpr void reset_setup_teams(std::array<int, N>& team) {
    for (std::size_t i = 0; i < N; ++i) team[i] = default_setup_team(static_cast<int>(i));
}

// Cycle a slot's (type, sub) one step FORWARD — sub_421E80 @0x421E80, CONFIRMED
// shape: off -> computer -> keyboard 0 -> keyboard 1 -> joystick 0 .. joystick
// (joystick_count-1) -> off. `joystick_count` is the number of CONNECTED sticks
// (GamepadMapper::count()); at 0 the cycle skips straight from keyboard 1 back
// to off, exactly as the original wraps past an empty joystick list (sub_429628
// finds none present).
constexpr void cycle_slot_input_type(int& type, int& sub, int joystick_count) {
    const auto t = static_cast<SlotInputType>(type);
    if (t == SlotInputType::Off) {
        type = static_cast<int>(SlotInputType::Computer);
        sub = 0;
        return;
    }
    if (t == SlotInputType::Computer) {
        type = static_cast<int>(SlotInputType::Keyboard);
        sub = 0;
        return;
    }
    if (t == SlotInputType::Keyboard && sub == 0) {
        sub = 1;
        return;
    }
    if (t == SlotInputType::Keyboard && sub == 1 && joystick_count > 0) {
        type = static_cast<int>(SlotInputType::Joystick);
        sub = 0;
        return;
    }
    if (t == SlotInputType::Joystick && sub + 1 < joystick_count) {
        ++sub;
        return;
    }
    // Keyboard 1 with no stick present, the last present stick, and any
    // type==4 OTHER (which the original never routes into this cycle) all wrap.
    type = static_cast<int>(SlotInputType::Off);
    sub = 0;
}

}  // namespace bomber::game
