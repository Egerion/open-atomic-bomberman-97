#include "bomber/input/input.hpp"

#include <SDL3/SDL.h>

namespace bomber::game {
namespace {

void bind(KeySet& ks, KeyAction action, SDL_Scancode code) {
    ks.scancode[static_cast<int>(action)] = code;
}

}  // namespace

KeySet default_key_set(int set) {
    KeySet ks{};
    if ((set & 1) == 0) {
        // sub_40614A, dword_4645BC[0..5] = 200/205/208/203/57/28 — the
        // E0-extended arrows plus Space and Enter, as the same physical keys in
        // SDL_Scancode space.
        bind(ks, KeyAction::Up, SDL_SCANCODE_UP);
        bind(ks, KeyAction::Right, SDL_SCANCODE_RIGHT);
        bind(ks, KeyAction::Down, SDL_SCANCODE_DOWN);
        bind(ks, KeyAction::Left, SDL_SCANCODE_LEFT);
        bind(ks, KeyAction::Action1, SDL_SCANCODE_SPACE);
        bind(ks, KeyAction::Action2, SDL_SCANCODE_RETURN);
        return ks;
    }
    // sub_40614A, dword_4645BC[10..15] = 19/34/33/32/31/30. Plain A rather than
    // the original's AZERTY Q swap: SDL scancodes are positional already, so
    // dword_4A2CA4 has no analogue to honour.
    bind(ks, KeyAction::Up, SDL_SCANCODE_R);
    bind(ks, KeyAction::Right, SDL_SCANCODE_G);
    bind(ks, KeyAction::Down, SDL_SCANCODE_F);
    bind(ks, KeyAction::Left, SDL_SCANCODE_D);
    bind(ks, KeyAction::Action1, SDL_SCANCODE_S);
    bind(ks, KeyAction::Action2, SDL_SCANCODE_A);
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
    // No Space-as-bomb OR fallback for set 0: sub_40614A already binds Space as
    // set 0's action1, and the original's decode reads ONLY the bound scancode
    // per action. The extra OR was an invented shim from before the remap UI.
    return in;
}

}  // namespace bomber::game
