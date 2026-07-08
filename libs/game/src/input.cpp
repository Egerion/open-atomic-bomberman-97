#include "bomber/game/input.hpp"

#include <SDL3/SDL.h>

namespace bomber::game {

KeySet default_key_set(int set) {
    KeySet ks{};
    if ((set & 1) == 0) {
        // Set 0: arrows + Right Ctrl/Space (bomb), Right Shift (action2). A
        // single PlayerInput has one action1 bool, so the mapper ORs both
        // scancodes at read() time (kept as a special case there) — the
        // KeySet itself can only name one scancode per action, so this slot
        // holds the primary (RCtrl) and read() keeps the Space fallback.
        ks.scancode[static_cast<int>(KeyAction::Up)] = SDL_SCANCODE_UP;
        ks.scancode[static_cast<int>(KeyAction::Right)] = SDL_SCANCODE_RIGHT;
        ks.scancode[static_cast<int>(KeyAction::Down)] = SDL_SCANCODE_DOWN;
        ks.scancode[static_cast<int>(KeyAction::Left)] = SDL_SCANCODE_LEFT;
        ks.scancode[static_cast<int>(KeyAction::Action1)] = SDL_SCANCODE_RCTRL;
        ks.scancode[static_cast<int>(KeyAction::Action2)] = SDL_SCANCODE_RSHIFT;
    } else {
        // Set 1: WASD + Left Ctrl (bomb), Left Shift (action2).
        ks.scancode[static_cast<int>(KeyAction::Up)] = SDL_SCANCODE_W;
        ks.scancode[static_cast<int>(KeyAction::Right)] = SDL_SCANCODE_D;
        ks.scancode[static_cast<int>(KeyAction::Down)] = SDL_SCANCODE_S;
        ks.scancode[static_cast<int>(KeyAction::Left)] = SDL_SCANCODE_A;
        ks.scancode[static_cast<int>(KeyAction::Action1)] = SDL_SCANCODE_LCTRL;
        ks.scancode[static_cast<int>(KeyAction::Action2)] = SDL_SCANCODE_LSHIFT;
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
    // Preserve the port's pre-existing Space-as-bomb fallback for set 0 (the
    // original binding table before this remap UI existed accepted either
    // Right Ctrl or Space) — additive, so rebinding Action1 away from RCtrl
    // does not remove the Space fallback; only rebinding to a DIFFERENT key
    // changes what Space stacks with, exactly like the pre-existing behaviour.
    in.players[0].action1 = in.players[0].action1 || keys[SDL_SCANCODE_SPACE];
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
