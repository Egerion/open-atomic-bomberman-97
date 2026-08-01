#include "bomber/frontend/keyremap_screen.hpp"

#include <cstdio>

#include "bomber/game_util/hud_format.hpp"  // fmt_u / fmt_s
#include "bomber/input/dos_scancode.hpp"
#include "bomber/ui/dialog_chrome.hpp"

namespace bomber::game {

namespace {

// sub_407AD9's 500 ms dwell (sub_413CB0(500)) before the raw-scancode poll arms;
// the key queue is flushed at the same moment (sub_41043C), so keys tapped during
// the dwell are discarded.
constexpr std::uint64_t kCaptureArmDelayMs = 500;

// LUT-true inks (docs/re/frontend-flow.md "COLOR.PAL — byte_495390 decoded for
// real"): byte_49D37A (header/percent yellow) and byte_49D38F (general white).
// These supersede the earlier nearest-search readings.
constexpr Rgb kYellow{252, 248, 88};  // byte_49D37A
constexpr Rgb kWhite{240, 248, 252};  // byte_49D38F
// byte_497498, LUT 0x2108 — the bevel-dark element of the widget cursor.
constexpr Rgb kBevel{60, 68, 56};

// The widget library's default 8x8 cursor — byte_45C310, read from BM95.EXE's
// data section (2026-07-13) through sub_430E4C's colour remap. Hotspot (1,1).
constexpr int kCursorSide = 8;
constexpr const char* kMouseCursorRows[kCursorSide] = {
    "1111111.",  //
    "1WWWWW1.",  //
    "1WWWW11.",  //
    "1WWWW11.",  //
    "1WWWWW11",  //
    "1W11WWW1",  //
    "11111WW1",  //
    "....1111",  //
};
constexpr int kCursorHotX = 1, kCursorHotY = 1;

// sub_432298's id scheme: 999 for the defaults button, 1000*set+action+1000 for
// the grid, 27 for the NOTE modal's " Ok ".
constexpr int kDefaultsWidgetId = 999;
constexpr int kNoteOkWidgetId = 27;
constexpr int kGridWidgetCount = kKeyboardSets * kKeyActionCount;

int grid_widget_id(int set, int action) {
    return 1000 * set + action + 1000;
}

}  // namespace

std::string KeyRemapScreen::msg(int id, const char* fb) const {
    return assets_ ? assets_->getstring(id, fb) : std::string(fb);
}

std::string KeyRemapScreen::grid_label(int set, int action) const {
    // Fallback action names mirror the shipped MESSAGES.TXT's own mixed-case
    // strings (1120..1125).
    static constexpr const char* kActionFb[kKeyActionCount] = {
        "Move Up", "Move Right", "Move Down", "Move Left", "Action 1", "Action 2"};
    return fmt_s(fmt_u(msg(1110, "Key %u, %s"), set), msg(1120 + action, kActionFb[action]));
}

// sub_432298 derives a button's size from its text.
KeyRemapScreen::Rect KeyRemapScreen::text_button_rect(const std::string& label, float x,
                                                      float y) const {
    const bool ready = font_ != nullptr && font_->loaded();
    const float w = static_cast<float>(ready ? font_->measure(label) : 0) + 16.0f;
    const float h = static_cast<float>(ready ? font_->line_height() : 12) + 6.0f;
    return Rect{x, y, w, h};
}

KeyRemapScreen::Rect KeyRemapScreen::grid_rect(int set, int action) const {
    // sub_407B9D's pinned grid anchors.
    return text_button_rect(grid_label(set, action), static_cast<float>(320 * set + 100),
                            static_cast<float>(60 * action + 60));
}

KeyRemapScreen::Rect KeyRemapScreen::defaults_rect() const {
    // pseudo.c 8762-8763.
    return text_button_rect(msg(1130, "Return to default keys"), 40.0f, 430.0f);
}

KeyRemapScreen::Rect KeyRemapScreen::note_ok_rect() const {
    if (!font_) return Rect{0, 0, 0, 0};
    const DialogRect win = acknowledge_dialog_rect(*font_, msg(95, "NOTE!"),
                                                   msg(1131, "Default key controls restored"));
    const DialogRect ok = acknowledge_ok_rect(*font_, win, msg(27, " Ok "));
    return Rect{ok.x, ok.y, ok.w, ok.h};
}

// The 2x6 grid walked flat, so the hit test is one loop rather than two.
int KeyRemapScreen::widget_at(float x, float y) const {
    for (int i = 0; i < kGridWidgetCount; ++i) {
        const int set = i / kKeyActionCount;
        const int action = i % kKeyActionCount;
        if (grid_rect(set, action).contains(x, y)) return grid_widget_id(set, action);
    }
    if (defaults_rect().contains(x, y)) return kDefaultsWidgetId;
    return -1;
}

void KeyRemapScreen::enter(const std::array<KeySet, kKeyboardSets>& current,
                           std::string backdrop) {
    edited_ = current;
    backdrop_ = std::move(backdrop);
    pressed_id_ = -1;
    capturing_ = false;
    showing_note_ = false;
    done_ = false;
}

void KeyRemapScreen::activate_widget(int id) {
    if (id == kDefaultsWidgetId) {
        // sub_40614A + the sub_414340 NOTE modal (pseudo.c 8799-8806).
        for (int s = 0; s < kKeyboardSets; ++s) edited_[s] = default_key_set(s);
        showing_note_ = true;
        return;
    }
    if (id < 1000 || id >= 2999) return;
    const int set = (id - 1000) / 1000;
    const int action = (id - 1000) % 1000;
    if (set < 0 || set > 1 || action < 0 || action >= kKeyActionCount) return;
    capture_set_ = set;
    capture_action_ = action;
    capture_start_ms_ = now_ms_;
    capturing_ = true;
}

void KeyRemapScreen::on_key(SDL_Keycode key, SDL_Scancode scancode, AudioEngine& audio) {
    if (capturing_) {
        // sub_407AD9: keys inside the 500 ms dwell are flushed; after it ANY key
        // binds, Esc included.
        (void)key;
        if (now_ms_ - capture_start_ms_ < kCaptureArmDelayMs) return;
        edited_[capture_set_].scancode[capture_action_] = static_cast<int>(scancode);
        capturing_ = false;
        return;
    }
    if (showing_note_) {
        // sub_414340's own key loop: nav blip for ANY real key; closes only on
        // Enter/Space/Esc (or the Ok button's click, on_mouse_up).
        audio.play(20);
        if (is_dismiss_key(key)) showing_note_ = false;
        return;
    }
    // sub_407B9D's exit clause (pseudo.c 8783-8792): Enter(13)/Esc(27)/Space(32)
    // leave the screen. Every other key is inert — and SILENT (no sub_427961 call
    // exists in the body).
    if (is_dismiss_key(key)) done_ = true;
}

bool KeyRemapScreen::is_dismiss_key(SDL_Keycode key) {
    return key == SDLK_RETURN || key == SDLK_KP_ENTER || key == SDLK_SPACE || key == SDLK_ESCAPE;
}

void KeyRemapScreen::on_mouse_move(float x, float y) {
    mouse_x_ = x;
    mouse_y_ = y;
}

void KeyRemapScreen::on_mouse_down(float x, float y) {
    mouse_x_ = x;
    mouse_y_ = y;
    if (capturing_) return;  // the modal owns input; widget clicks are inert
    if (showing_note_) {
        if (note_ok_rect().contains(x, y)) pressed_id_ = kNoteOkWidgetId;
        return;
    }
    pressed_id_ = widget_at(x, y);
}

void KeyRemapScreen::on_mouse_up(float x, float y, AudioEngine& audio) {
    mouse_x_ = x;
    mouse_y_ = y;
    const int was_pressed = pressed_id_;
    pressed_id_ = -1;
    if (capturing_ || was_pressed == -1) return;
    if (showing_note_) {
        // Clicking " Ok " posts widget id 27 into the key queue — the loop then
        // blips and closes exactly as if Esc were pressed.
        if (was_pressed == kNoteOkWidgetId && note_ok_rect().contains(x, y)) {
            audio.play(20);
            showing_note_ = false;
        }
        return;
    }
    // sub_432998 fires a widget on RELEASE inside it.
    if (widget_at(x, y) == was_pressed) activate_widget(was_pressed);
}

void KeyRemapScreen::poll_capture() {
    if (!capturing_) return;
    if (now_ms_ - capture_start_ms_ < kCaptureArmDelayMs) return;
    int numkeys = 0;
    const bool* keys = SDL_GetKeyboardState(&numkeys);
    if (!keys) return;
    // sub_407AD9's 0..255 ascending scan keeps overwriting its result, so the
    // HIGHEST held index wins when several keys are down.
    int found = -1;
    for (int i = 0; i < numkeys; ++i)
        if (keys[i]) found = i;
    if (found < 0) return;
    edited_[capture_set_].scancode[capture_action_] = found;
    capturing_ = false;
}

void KeyRemapScreen::draw(SDL_Renderer* ren) const {
    if (!ren) return;
    draw_backdrop(ren);
    if (!font_ || !font_->loaded()) return;
    // Title at (20, 20), clip 400 — recovered from the raw EXE bytes at 0x407BD0
    // after Hex-Rays lost the x; the old (400,20) read the CLIP WIDTH as the x.
    font_->draw_outlined(ren, msg(1100, "Keyboard definitions"), SDL_FPoint{20.0f, 20.0f},
                         OutlinedTextStyle{kYellow, {}, 400.0f});
    draw_grid(ren);
    draw_defaults_button(ren);
    if (capturing_) draw_capture_modal(ren);
    if (showing_note_) draw_note_modal(ren);
    draw_mouse_cursor(ren);  // last: the original's cursor composites over everything
}

// Each original frame starts with sub_415CA4's memcpy of the SAVED backdrop
// store, which holds the Options screen's GLUE picture but not its rows.
void KeyRemapScreen::draw_backdrop(SDL_Renderer* ren) const {
    if (!assets_) return;
    const Sprite& bg = assets_->frontend_pcx(backdrop_);
    if (bg.tex == nullptr) {
        SDL_SetRenderDrawColor(ren, 20, 20, 30, 255);
        SDL_RenderClear(ren);
        return;
    }
    SDL_FRect d{0, 0, static_cast<float>(bg.w), static_cast<float>(bg.h)};
    SDL_RenderTexture(ren, bg.tex, nullptr, &d);
}

// The 2x6 grid of REAL bevel-button widgets (sub_432298), walked flat.
void KeyRemapScreen::draw_grid(SDL_Renderer* ren) const {
    for (int i = 0; i < kGridWidgetCount; ++i)
        draw_grid_button(ren, i / kKeyActionCount, i % kKeyActionCount);
}

void KeyRemapScreen::draw_grid_button(SDL_Renderer* ren, int set, int action) const {
    const Rect r = grid_rect(set, action);
    draw_dialog_button(DialogPen{ren, *font_}, SDL_FPoint{r.x, r.y}, grid_label(set, action),
                       widget_pressed(grid_widget_id(set, action)));
    // The bound-key line sits 22 px below and is drawn ONLY while
    // `(code & 0x7F) < 0x59` (pseudo.c 8743-8744).
    const int dos = dos_scancode_from_sdl(edited_[set].scancode[action]);
    const char* name = dos_scancode_name(dos);
    if (name == nullptr) return;
    font_->draw_outlined(ren, fmt_s(msg(1140, "Key: '%s'"), name), SDL_FPoint{r.x, r.y + 22.0f},
                         OutlinedTextStyle{kWhite, {}, 200.0f});
}

void KeyRemapScreen::draw_defaults_button(SDL_Renderer* ren) const {
    const Rect r = defaults_rect();
    draw_dialog_button(DialogPen{ren, *font_}, SDL_FPoint{r.x, r.y},
                       msg(1130, "Return to default keys"), widget_pressed(kDefaultsWidgetId));
}

// A button shows its "down" bevel only while the mouse is held INSIDE it, and
// never under either modal.
bool KeyRemapScreen::widget_pressed(int id) const {
    return pressed_id_ == id && widget_at(mouse_x_, mouse_y_) == id && !capturing_ &&
           !showing_note_;
}

// sub_412E33's completion window: 360 x 8*fontheight at y=200, x centred, WINZ
// 9-patch, with the message at 1.5 lines, the "%d%%" readout (pinned 0 for the
// whole wait) at 3.5, and the white-framed 300-px track at 5.5.
void KeyRemapScreen::draw_capture_modal(SDL_Renderer* ren) const {
    const float lh = static_cast<float>(font_->line_height());
    const DialogRect win{(640.0f - 360.0f) / 2.0f, 200.0f, 360.0f, 8.0f * lh};
    draw_dialog_chrome(ren, win, assets_ ? &assets_->frontend_pcx("WINZ") : nullptr);
    const std::string m =
        fmt_s(msg(1105, "Press key for '%s'"), grid_label(capture_set_, capture_action_));
    const float mw = static_cast<float>(font_->measure(m));
    font_->draw_outlined(ren, m, SDL_FPoint{win.x + (360.0f - mw) / 2.0f, win.y + 1.5f * lh},
                         OutlinedTextStyle{kWhite});
    const std::string pct = "0%";
    const float pw = static_cast<float>(font_->measure(pct));
    font_->draw_outlined(ren, pct, SDL_FPoint{win.x + (360.0f - pw) / 2.0f, win.y + 3.5f * lh},
                         OutlinedTextStyle{kYellow});
    SDL_FRect track{win.x + 31.0f, win.y + 5.5f * lh + 1.0f, 300.0f, lh - 1.0f};
    SDL_SetRenderDrawColor(ren, 0, 0, 0, 255);
    SDL_RenderFillRect(ren, &track);
    SDL_FRect frame{win.x + 30.0f, win.y + 5.5f * lh, 302.0f, lh + 1.0f};
    SDL_SetRenderDrawColor(ren, kWhite.r, kWhite.g, kWhite.b, 255);
    SDL_RenderRect(ren, &frame);
}

// The post-restore acknowledge modal (pseudo.c 8799-8806: sub_414340 with
// getstring(95) "NOTE!" on top, getstring(1131) below, the general white ink, and
// its " Ok " button).
void KeyRemapScreen::draw_note_modal(SDL_Renderer* ren) const {
    const bool ok_pressed =
        pressed_id_ == kNoteOkWidgetId && note_ok_rect().contains(mouse_x_, mouse_y_);
    draw_acknowledge_dialog(DialogPen{ren, *font_},
                            assets_ ? &assets_->frontend_pcx("WINZ") : nullptr,
                            AcknowledgeLabels{msg(95, "NOTE!"),
                                              msg(1131, "Default key controls restored"),
                                              msg(27, " Ok ")},
                            AcknowledgeStyle{kWhite, ok_pressed});
}

// The widget library's own 8x8 arrow, hotspot (1,1), walked as one flat 64-cell
// pass rather than a nested row/column pair.
void KeyRemapScreen::draw_mouse_cursor(SDL_Renderer* ren) const {
    for (int i = 0; i < kCursorSide * kCursorSide; ++i) {
        const int row = i / kCursorSide;
        const int col = i % kCursorSide;
        const char c = kMouseCursorRows[row][col];
        if (c == '.') continue;
        const bool white = c == 'W';
        const Rgb ink = white ? kWhite : kBevel;
        SDL_SetRenderDrawColor(ren, ink.r, ink.g, ink.b, 255);
        SDL_FRect px{mouse_x_ - kCursorHotX + static_cast<float>(col),
                     mouse_y_ - kCursorHotY + static_cast<float>(row), 1.0f, 1.0f};
        SDL_RenderFillRect(ren, &px);
    }
}

}  // namespace bomber::game
