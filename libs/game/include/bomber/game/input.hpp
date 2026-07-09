#pragma once

#include <array>
#include <cstdint>

#include "bomber/sim/types.hpp"

// Input mapping: keyboard state -> TickInputs.
//
// The two key-sets are DATA-DRIVEN (docs/re/results-and-options.md §2, the
// key-remap UI sub_407B9D): each set binds 6 actions (Up/Right/Down/Left/
// Action1/Action2, the §2 action-name order 1120..1125) to one scancode.
// Deliberately SDL-free (stored as plain `int`, the caller's SDL_Scancode
// cast at the read()/game_app.cpp boundary) so this header stays includable
// by the headless build/tests (bomber_frontend_tests links no SDL3 — see
// tests/CMakeLists.txt's own note; `tests/` is add_subdirectory'd BEFORE
// libs/game's SDL3 setup in the top-level CMakeLists, and the `headless`
// preset never adds it at all). input.cpp (which DOES link SDL3 via
// bomber::game) does the SDL_GetKeyboardState/SDL_Scancode work.
//
// Defaults mirror the port's pre-existing hardcoded pairing (arrows+RCtrl/
// Space/RShift for set 0, WASD+LCtrl/LShift for set 1) — NOT the original
// binary's own default scancodes (200/205/208/203/57/46 and 17/32/31/30/2/3,
// §2's "Restore defaults" row), since those are raw DOS scancodes in a
// different numbering space than SDL_Scancode and this port has shipped with
// the arrows/WASD pairing since before this remap UI existed. A player who
// wants the ORIGINAL's defaults can already reach them by rebinding through
// the in-game UI once; default_key_set() below is this port's OWN default,
// not a decompile citation.

namespace bomber::game {

// The 6 bindable actions per keyboard set, in §2's action-name id order
// (1120 Move Up .. 1125 Action 2).
enum class KeyAction : std::uint8_t { Up, Right, Down, Left, Action1, Action2, kCount };
inline constexpr int kKeyActionCount = static_cast<int>(KeyAction::kCount);
inline constexpr int kKeyboardSets = 2;

// One keyboard set's 6 scancode bindings. `scancode[i]` is an SDL_Scancode
// value stored as `int` (see the file doc's SDL-free rationale) — cast back
// via `static_cast<SDL_Scancode>(...)` at the point of use.
struct KeySet {
    std::array<int, kKeyActionCount> scancode;
};

class KeyboardMapper {
public:
    KeyboardMapper();

    sim::TickInputs read() const;

    // The live bindings for keyboard set `set` (0 or 1) — used by both
    // read() and the key-remap screen (which edits a copy, then calls
    // set_key_set() to apply it live once the player leaves the screen).
    const KeySet& key_set(int set) const { return sets_[set & 1]; }
    void set_key_set(int set, const KeySet& ks) { sets_[set & 1] = ks; }

private:
    std::array<KeySet, kKeyboardSets> sets_;
};

// This port's own default bindings (see the class-doc note above) — arrows/
// RCtrl+Space/RShift for set 0, WASD/LCtrl+E/LShift for set 1, matching what
// shipped before the remap UI existed. Used to seed KeyboardMapper and by the
// key-remap screen's "restore defaults" action.
KeySet default_key_set(int set);

// Scripted inputs for the headless --demo mode (both players walk squares
// and drop bombs periodically).
sim::TickInputs demo_inputs(int t);

// The PLAYER INPUT TYPE SELECTION slot categories (docs/re/setup-screens.md,
// sub_421DD2's player byte +16). Mirrors the original's type numbering so the
// setup-screen switch statements (present_setup) read directly against it.
enum class SlotInputType : std::uint8_t { Off = 0, Computer = 1, Keyboard = 2, Joystick = 3, Other = 4 };

// ATTRACT-MODE roster/stage rolls (docs/re/frontend-flow.md "Attract mode",
// sub_410F81's attract branch, pseudo.c 15125-15143). The menu idle timeout
// forces v10=0 (Play) with the attract flag set; sub_410F81 then short-
// circuits: every slot OFF, then `rand()%10 + 1` (clamped to a MINIMUM of 3)
// slots flipped to COMPUTER, and the level set to `rand() % getvalue(35)`
// DIRECTLY — bypassing the VALUELST 1150-1160 random-level enable flags the
// normal RANDOM pick honours, so attract can land on a disabled stage. Both
// rolls are presentation-side (never bomber::sim::State::rng) — the caller
// supplies raw LCG draws, pre-masked the same way pick_glue()/the goldman
// wheel already fold their own draws into a bounded range. Pure/SDL-free so
// the bounds/composition are unit-testable without a window.
constexpr int attract_computer_count(unsigned roll) {
    int n = static_cast<int>(roll % 10) + 1;  // rand()%10 + 1 -> 1..10
    return n < 3 ? 3 : n;                      // "clamped to a minimum of 3"
}

// Fills 10 roster slots for an attract demo: the first `computer_count`
// become COMPUTER (sub 0), the rest OFF — sub_410F81's attract branch sets
// every slot OFF first, then flips exactly `computer_count` of them in slot
// order. Team is zeroed for every slot too (doc: "forces team play off").
// `type`/`sub`/`team` must each have exactly `count` elements (kMaxPlayers,
// 10) — the caller (GameApp) supplies its own std::array<int, kMaxPlayers>.
template <std::size_t N>
constexpr void fill_attract_roster(int computer_count, std::array<int, N>& type,
                                   std::array<int, N>& sub, std::array<int, N>& team) {
    for (std::size_t i = 0; i < N; ++i) {
        bool on = static_cast<int>(i) < computer_count;
        type[i] =
            on ? static_cast<int>(SlotInputType::Computer) : static_cast<int>(SlotInputType::Off);
        sub[i] = 0;
        team[i] = 0;
    }
}

// Attract-mode stage pick: `rand() % level_count` DIRECTLY, ignoring the
// enable-flag rotation `pick_stage` (match/setup) honours for a normal
// RANDOM-level match — the doc is explicit that attract "bypasses" those
// flags. `level_count` is getvalue(35) (11 in the shipped VALUELST); `roll`
// is the caller's presentation LCG draw.
constexpr int attract_stage_pick(unsigned roll, int level_count) {
    if (level_count < 1) level_count = 1;
    return static_cast<int>(roll % static_cast<unsigned>(level_count));
}

// Cycle a slot's (type, sub) one step FORWARD (sub_421E80 @0x421E80, CONFIRMED
// shape): off -> computer -> keyboard set 0 -> keyboard set 1 -> joystick 0 ..
// joystick (joystick_count-1) -> off. `joystick_count` is the number of
// CONNECTED sticks (GamepadMapper::count()) — when it is 0 the cycle skips
// straight from keyboard 1 back to off, exactly like the original wraps past
// an empty joystick list (sub_429628 finds none present). Pure/SDL-free so it
// is unit-testable without a window or a physical pad.
constexpr void cycle_slot_input_type(int& type, int& sub, int joystick_count) {
    auto t = static_cast<SlotInputType>(type);
    if (t == SlotInputType::Off) {
        type = static_cast<int>(SlotInputType::Computer);
        sub = 0;
    } else if (t == SlotInputType::Computer) {
        type = static_cast<int>(SlotInputType::Keyboard);
        sub = 0;
    } else if (t == SlotInputType::Keyboard && sub == 0) {
        sub = 1;
    } else if (t == SlotInputType::Keyboard && sub == 1) {
        if (joystick_count > 0) {
            type = static_cast<int>(SlotInputType::Joystick);
            sub = 0;
        } else {
            type = static_cast<int>(SlotInputType::Off);
            sub = 0;
        }
    } else if (t == SlotInputType::Joystick && sub + 1 < joystick_count) {
        ++sub;
    } else {
        // Joystick's last present stick (or any type==4 OTHER, which the
        // original never routes into this cycle either) wraps to off.
        type = static_cast<int>(SlotInputType::Off);
        sub = 0;
    }
}

}  // namespace bomber::game
