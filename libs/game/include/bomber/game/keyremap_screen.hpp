#pragma once

#include <SDL3/SDL.h>

#include <array>
#include <string>

#include "bomber/game/asset_store.hpp"
#include "bomber/game/audio_engine.hpp"
#include "bomber/game/bmscreen.hpp"
#include "bomber/game/input.hpp"

// The key-remap UI — sub_407B9D @0x407B9D (docs/re/results-and-options.md
// §2, CONFIRMED): a 2x6 grid, one clickable button per (keyboard_set,
// action). Reached as item 15 ("Define keyboard layouts") of the interactive
// Options screen (options_screen.hpp), NOT its own AppState/main-menu row —
// sub_407B9D draws directly over the Options screen's own backdrop, so this
// screen intentionally has no background of its own.
//
// Layout (§2, CONFIRMED coordinates): for keyboard_set in 0..1, action in
// 0..5: a button at x = 320*set + 100, y = 60*action + 60, labelled
// getstring(1110) "Key %u, %s" (set, action name getstring(1120+action)),
// with the bound key's name shown below via getstring(1140) "Key: '%s'".
//
// Rebind interaction (§2, CONFIRMED shape, adapted to SDL): clicking/
// selecting a slot enters a MODAL capture ("Press key for '%s'",
// getstring(1105)); the next real keydown is captured as that slot's new
// SDL_Scancode; Esc cancels the capture (per §2's documented input model,
// no-op, no change). The original polls a raw scancode table with no
// validation and allows duplicate bindings across actions — we do the same
// (SDL_Scancode is our port's equivalent of that raw table; no de-dup here).
//
// Persistence: this screen edits an in-memory copy only (edited_) — the
// caller (present_keyremap_screen) applies it to the live KeyboardMapper and
// options.ini only after the screen is dismissed and, per §2's confirmed
// exit-time write-back semantics (sub_405DE3 via sub_410EBF, the app's
// generic exit-hook — task requirement 3), options.ini itself is not
// rewritten until the app exits normally; this screen only mutates in-memory
// state, exactly like every other Options-screen row.
namespace bomber::game {

class KeyRemapScreen {
public:
    KeyRemapScreen(const AssetStore& assets, const FontTextures& font)
        : assets_(&assets), font_(&font) {}

    // (Re)enter with the CURRENT live bindings (one KeySet per keyboard set,
    // input.hpp) to edit as a working copy.
    void enter(const std::array<KeySet, kKeyboardSets>& current);

    // Feed one SDL keycode/scancode pair. When a capture is pending (see
    // capturing()), this IS the captured key (any key but Esc binds it; Esc
    // cancels the capture with no change, §2's exit clause `< 0x20`/`==0x20`
    // no-op path adapted 1:1 to a cancel). Otherwise it drives the (set,
    // action) cursor and starts/exits captures. Returns true once Enter/Esc
    // ends the whole screen (mirrors OptionsScreen::on_key's done() contract).
    void on_key(SDL_Keycode key, SDL_Scancode scancode, AudioEngine& audio);

    // "Restore defaults" (row 999, §2): resets BOTH sets to
    // default_key_set(0)/(1) (this port's own defaults — see input.hpp's
    // note on why we don't reproduce the original's raw DOS scancodes).
    void restore_defaults(AudioEngine& audio);

    void draw(SDL_Renderer* ren) const;

    bool done() const { return done_; }
    bool capturing() const { return capturing_; }
    const std::array<KeySet, kKeyboardSets>& edited() const { return edited_; }

private:
    const AssetStore* assets_ = nullptr;
    const FontTextures* font_ = nullptr;

    std::array<KeySet, kKeyboardSets> edited_{};
    int cursor_set_ = 0;     // 0 or 1 — which keyboard set is highlighted
    int cursor_action_ = 0;  // 0..5 (KeyAction) — which row is highlighted
    bool capturing_ = false;  // modal "press a key" state (sub_407AD9)
    bool done_ = false;

    static const char* action_name(int action);
    static std::string scancode_name(SDL_Scancode sc);
};

}  // namespace bomber::game
