#include "bomber/game/keyremap_screen.hpp"

#include <cstdio>

namespace bomber::game {

namespace {

// §2 CONFIRMED grid geometry: x = 320*set + 100, y = 60*action + 60.
constexpr int kGridX0 = 100;
constexpr int kGridXStep = 320;
constexpr int kGridY0 = 60;
constexpr int kGridYStep = 60;
constexpr int kHeaderX = 400;  // §2: fixed (400, 20)
constexpr int kHeaderY = 20;
constexpr int kHintY = 460;

constexpr Uint8 kInkR = 230, kInkG = 230, kInkB = 210;
constexpr Uint8 kSelR = 255, kSelG = 220, kSelB = 80;
constexpr Uint8 kCapR = 255, kCapG = 90, kCapB = 90;
constexpr Uint8 kHintR = 160, kHintG = 160, kHintB = 160;

}  // namespace

const char* KeyRemapScreen::action_name(int action) {
    // getstring(1120+action): 1120 Move Up, 1121 Move Right, 1122 Move Down,
    // 1123 Move Left, 1124 Action 1, 1125 Action 2 (§2, CONFIRMED order).
    switch (static_cast<KeyAction>(action)) {
        case KeyAction::Up: return "MOVE UP";
        case KeyAction::Right: return "MOVE RIGHT";
        case KeyAction::Down: return "MOVE DOWN";
        case KeyAction::Left: return "MOVE LEFT";
        case KeyAction::Action1: return "ACTION 1";
        case KeyAction::Action2: return "ACTION 2";
        default: return "?";
    }
}

std::string KeyRemapScreen::scancode_name(SDL_Scancode sc) {
    const char* n = SDL_GetScancodeName(sc);
    return (n && *n) ? n : "?";
}

void KeyRemapScreen::enter(const std::array<KeySet, kKeyboardSets>& current) {
    edited_ = current;
    cursor_set_ = 0;
    cursor_action_ = 0;
    capturing_ = false;
    done_ = false;
}

void KeyRemapScreen::on_key(SDL_Keycode key, SDL_Scancode scancode, AudioEngine& audio) {
    if (capturing_) {
        // sub_407AD9's modal capture: Esc cancels (no-op), any other real key
        // is stored straight into the slot — no validation, duplicates across
        // actions allowed (§2, CONFIRMED).
        if (key == SDLK_ESCAPE) {
            capturing_ = false;
            audio.play(10);
            return;
        }
        edited_[cursor_set_].scancode[cursor_action_] = static_cast<int>(scancode);
        capturing_ = false;
        audio.play(10);
        return;
    }

    switch (key) {
        case SDLK_UP:
            cursor_action_ = (cursor_action_ + kKeyActionCount - 1) % kKeyActionCount;
            audio.play(20);
            break;
        case SDLK_DOWN:
            cursor_action_ = (cursor_action_ + 1) % kKeyActionCount;
            audio.play(20);
            break;
        case SDLK_LEFT:
        case SDLK_RIGHT:
            cursor_set_ ^= 1;  // only 2 sets — toggle
            audio.play(20);
            break;
        case SDLK_RETURN:
        case SDLK_KP_ENTER:
        case SDLK_SPACE:
            // Clicking/selecting a button (hit id 1000*set+action) enters the
            // modal capture (§2, CONFIRMED). The screen itself is not
            // dismissed by this key while a slot is highlighted — only
            // Escape (below) or the caller's own Enter-outside-a-row leaves
            // the whole grid, mirroring §2's "Enter/Esc/Space leave the
            // screen" exit clause being reserved for when nothing is being
            // captured.
            capturing_ = true;
            audio.play(10);
            break;
        case SDLK_ESCAPE:
            // §2 "Exit": Enter/Esc/Space (< 0x20, == 0x20) leave the screen
            // when not mid-capture (the mid-capture case is handled above).
            audio.play(10);
            done_ = true;
            break;
        default:
            break;
    }
}

void KeyRemapScreen::restore_defaults(AudioEngine& audio) {
    // Row 999 "Return to default keys" (§2, sub_40614A): resets both sets.
    for (int s = 0; s < kKeyboardSets; ++s) edited_[s] = default_key_set(s);
    audio.play(10);
}

void KeyRemapScreen::draw(SDL_Renderer* ren) const {
    if (!ren) return;
    // §2: "no new backdrop call — sub_407B9D draws directly over the Options
    // screen's own frame". We therefore only clear if the caller hasn't
    // already painted something (present_keyremap_screen paints the Options
    // screen's own backdrop first, then this draw() layers on top).
    if (!font_ || !font_->loaded()) return;

    font_->draw(ren, "KEYBOARD DEFINITIONS", static_cast<float>(kHeaderX),
                static_cast<float>(kHeaderY), kInkR, kInkG, kInkB);

    for (int set = 0; set < kKeyboardSets; ++set) {
        for (int action = 0; action < kKeyActionCount; ++action) {
            float x = static_cast<float>(kGridX0 + kGridXStep * set);
            float y = static_cast<float>(kGridY0 + kGridYStep * action);
            bool sel = (set == cursor_set_ && action == cursor_action_);
            bool cap = sel && capturing_;
            Uint8 r = cap ? kCapR : (sel ? kSelR : kInkR);
            Uint8 g = cap ? kCapG : (sel ? kSelG : kInkG);
            Uint8 b = cap ? kCapB : (sel ? kSelB : kInkB);

            char label[64];
            std::snprintf(label, sizeof label, "%sKey %d, %s", sel ? "> " : "  ", set,
                          action_name(action));
            font_->draw(ren, label, x, y, r, g, b);

            std::string keyname =
                cap ? "PRESS A KEY..."
                    : ("Key: '" +
                       scancode_name(static_cast<SDL_Scancode>(edited_[set].scancode[action])) +
                       "'");
            font_->draw(ren, keyname, x, y + 18.0f, kHintR, kHintG, kHintB);
        }
    }

    font_->draw(ren, "UP/DOWN ROW   LEFT/RIGHT SET   ENTER REBIND   ESC DONE", 40.0f,
                static_cast<float>(kHintY), kHintR, kHintG, kHintB);
}

}  // namespace bomber::game
