#include "bomber/input/input.hpp"

#include <SDL3/SDL.h>

namespace bomber::game {

KeySet default_key_set(int set) {
    KeySet ks{};
    if ((set & 1) == 0) {
        // Set 0 (sub_40614A @0x40614A, dword_4645BC[0..5] = 200/205/208/203/
        // 57/28): arrows + Space (action1) + Enter (action2). The DOS codes
        // are the E0-extended arrow set (0x80|code) + the base set; here as
        // the same physical keys in SDL_Scancode space (dos_scancode.cpp is
        // the ini-boundary translator).
        ks.scancode[static_cast<int>(KeyAction::Up)] = SDL_SCANCODE_UP;
        ks.scancode[static_cast<int>(KeyAction::Right)] = SDL_SCANCODE_RIGHT;
        ks.scancode[static_cast<int>(KeyAction::Down)] = SDL_SCANCODE_DOWN;
        ks.scancode[static_cast<int>(KeyAction::Left)] = SDL_SCANCODE_LEFT;
        ks.scancode[static_cast<int>(KeyAction::Action1)] = SDL_SCANCODE_SPACE;
        ks.scancode[static_cast<int>(KeyAction::Action2)] = SDL_SCANCODE_RETURN;
    } else {
        // Set 1 (sub_40614A, dword_4645BC[10..15] = 19/34/33/32/31/30): the
        // R/G/F/D diamond (up/right/down/left) + S (action1) + A (action2).
        // The original swaps A for Q (16) when the BIOS keyboard-nationality
        // global dword_4A2CA4 == 1 (an AZERTY accommodation with no SDL
        // analogue — SDL scancodes are positional already, so plain A is the
        // faithful pick here).
        ks.scancode[static_cast<int>(KeyAction::Up)] = SDL_SCANCODE_R;
        ks.scancode[static_cast<int>(KeyAction::Right)] = SDL_SCANCODE_G;
        ks.scancode[static_cast<int>(KeyAction::Down)] = SDL_SCANCODE_F;
        ks.scancode[static_cast<int>(KeyAction::Left)] = SDL_SCANCODE_D;
        ks.scancode[static_cast<int>(KeyAction::Action1)] = SDL_SCANCODE_S;
        ks.scancode[static_cast<int>(KeyAction::Action2)] = SDL_SCANCODE_A;
    }
    return ks;
}

KeyboardMapper::KeyboardMapper() {
    for (int s = 0; s < kKeyboardSets; ++s) sets_[s] = default_key_set(s);
}

sim::TickInputs KeyboardMapper::read() const {
    const bool* keys = SDL_GetKeyboardState(nullptr);
    sim::TickInputs in;
    for (int s = 0; s < kKeyboardSets; ++s) {
        const KeySet& ks = sets_[s];
        auto& p = in.players[s];
        p.up = keys[ks.scancode[static_cast<int>(KeyAction::Up)]];
        p.down = keys[ks.scancode[static_cast<int>(KeyAction::Down)]];
        p.left = keys[ks.scancode[static_cast<int>(KeyAction::Left)]];
        p.right = keys[ks.scancode[static_cast<int>(KeyAction::Right)]];
        p.action1 = keys[ks.scancode[static_cast<int>(KeyAction::Action1)]];
        p.action2 = keys[ks.scancode[static_cast<int>(KeyAction::Action2)]];
    }
    // (The port's old Space-as-bomb OR fallback for set 0 is gone: the
    // original's own default already binds Space as set 0's action1
    // (sub_40614A above), and its input decode reads ONLY the bound
    // scancode per action — the extra OR was an invented shim from before
    // the remap UI existed.)
    return in;
}

sim::TickInputs demo_inputs(int t) {
    sim::TickInputs in;
    int phase = (t / 25) % 4;
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

}  // namespace bomber::game
