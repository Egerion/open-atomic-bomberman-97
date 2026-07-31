#include "bomber/frontend/keyremap_screen.hpp"

#include <cstdio>

#include "bomber/input/dos_scancode.hpp"
#include "bomber/ui/dialog_chrome.hpp"

namespace bomber::game {

namespace {

// sub_407AD9's 500 ms dwell (sub_413CB0(500)) before the raw-scancode poll
// arms; the key queue is flushed at the same moment (sub_41043C), so keys
// tapped during the dwell are discarded.
constexpr std::uint64_t kCaptureArmDelayMs = 500;

// LUT-true inks (docs/re/frontend-flow.md "COLOR.PAL — byte_495390 decoded
// for real"): byte_49D37A (header/percent yellow) and byte_49D38F (general
// white) — these supersede the earlier nearest-search (255,255,82)/(255,255,
// 255) readings this file used.
constexpr Uint8 kYellowR = 252, kYellowG = 248, kYellowB = 88;  // byte_49D37A
constexpr Uint8 kWhiteR = 240, kWhiteG = 248, kWhiteB = 252;    // byte_49D38F

// The widget library's default 8x8 mouse cursor — byte_45C310, read straight
// from BM95.EXE's data section (2026-07-13), through sub_430E4C's in-place
// colour remap: '1' -> byte_497498 (LUT 0x2108 -> (60,68,56), the bevel-dark
// element), 'W' (value 15) -> byte_49D38F white, '.' (0) -> transparent.
// Hotspot (1,1) (sub_430EDC's default-arm a5/a6).
constexpr const char* kMouseCursorRows[8] = {
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

// Crash-proof MESSAGES.TXT splices (same rule as game_app.cpp's fmt_u/fmt_s
// — a modified file's unexpected specifier stays literal).
std::string fmt_u(const std::string& f, int v) {
    auto p = f.find('%');
    if (p == std::string::npos) return f;
    std::size_t q = p + 1;
    while (q < f.size() && f[q] != 'u' && f[q] != 'd' && f[q] != 'i' && f[q] != 's' && f[q] != '%')
        ++q;
    if (q < f.size() && (f[q] == 'u' || f[q] == 'd' || f[q] == 'i'))
        return f.substr(0, p) + std::to_string(v) + f.substr(q + 1);
    return f;
}
std::string fmt_s(const std::string& f, const std::string& v) {
    auto p = f.find('%');
    if (p == std::string::npos) return f;
    std::size_t q = p + 1;
    while (q < f.size() && f[q] != 'u' && f[q] != 'd' && f[q] != 'i' && f[q] != 's' && f[q] != '%')
        ++q;
    if (q < f.size() && f[q] == 's') return f.substr(0, p) + v + f.substr(q + 1);
    return f;
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

KeyRemapScreen::Rect KeyRemapScreen::grid_rect(int set, int action) const {
    // sub_432298's text-derived size at sub_407B9D's pinned grid anchors.
    const float w =
        static_cast<float>(font_ && font_->loaded() ? font_->measure(grid_label(set, action)) : 0) +
        16.0f;
    const float h = static_cast<float>(font_ && font_->loaded() ? font_->line_height() : 12) + 6.0f;
    return Rect{static_cast<float>(320 * set + 100), static_cast<float>(60 * action + 60), w, h};
}

KeyRemapScreen::Rect KeyRemapScreen::defaults_rect() const {
    const std::string label = msg(1130, "Return to default keys");
    const float w =
        static_cast<float>(font_ && font_->loaded() ? font_->measure(label) : 0) + 16.0f;
    const float h = static_cast<float>(font_ && font_->loaded() ? font_->line_height() : 12) + 6.0f;
    return Rect{40.0f, 430.0f, w, h};
}

KeyRemapScreen::Rect KeyRemapScreen::note_ok_rect() const {
    if (!font_) return Rect{0, 0, 0, 0};
    const DialogRect win = acknowledge_dialog_rect(*font_, msg(95, "NOTE!"),
                                                   msg(1131, "Default key controls restored"));
    const DialogRect ok = acknowledge_ok_rect(*font_, win, msg(27, " Ok "));
    return Rect{ok.x, ok.y, ok.w, ok.h};
}

int KeyRemapScreen::widget_at(float x, float y) const {
    for (int set = 0; set < kKeyboardSets; ++set)
        for (int action = 0; action < kKeyActionCount; ++action)
            if (grid_rect(set, action).contains(x, y)) return 1000 * set + action + 1000;
    if (defaults_rect().contains(x, y)) return 999;
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
    if (id == 999) {
        // sub_40614A + the sub_414340 NOTE modal (pseudo.c 8799-8806).
        for (int s = 0; s < kKeyboardSets; ++s) edited_[s] = default_key_set(s);
        showing_note_ = true;
        return;
    }
    if (id >= 1000 && id < 2999) {
        const int set = (id - 1000) / 1000;
        const int action = (id - 1000) % 1000;
        if (set < 0 || set > 1 || action < 0 || action >= kKeyActionCount) return;
        capture_set_ = set;
        capture_action_ = action;
        capture_start_ms_ = now_ms_;
        capturing_ = true;
    }
}

void KeyRemapScreen::on_key(SDL_Keycode key, SDL_Scancode scancode, AudioEngine& audio) {
    if (capturing_) {
        // sub_407AD9: keys inside the 500 ms dwell are flushed away; after
        // it, ANY key binds — Esc included (the caller's store is
        // unconditional; see the header doc for the one race artifact).
        (void)key;
        if (now_ms_ - capture_start_ms_ < kCaptureArmDelayMs) return;
        edited_[capture_set_].scancode[capture_action_] = static_cast<int>(scancode);
        capturing_ = false;
        return;
    }
    if (showing_note_) {
        // sub_414340's own key loop: nav blip for ANY real key; closes only
        // on Enter/Space/Esc (or the Ok button's click, on_mouse_up).
        audio.play(20);
        if (key == SDLK_RETURN || key == SDLK_KP_ENTER || key == SDLK_SPACE ||
            key == SDLK_ESCAPE)
            showing_note_ = false;
        return;
    }
    // sub_407B9D's exit clause (pseudo.c 8783-8792): Enter(13)/Esc(27)/
    // Space(32) leave the screen. Every other key is inert — and SILENT
    // (no sub_427961 call exists in the body).
    if (key == SDLK_RETURN || key == SDLK_KP_ENTER || key == SDLK_SPACE || key == SDLK_ESCAPE)
        done_ = true;
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
        if (note_ok_rect().contains(x, y)) pressed_id_ = 27;  // the Ok widget's id
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
        // Clicking " Ok " posts widget id 27 into the key queue — the loop
        // then blips and closes exactly as if Esc were pressed.
        if (was_pressed == 27 && note_ok_rect().contains(x, y)) {
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
    // sub_407AD9's 0..255 ascending scan keeps overwriting its result, so
    // the HIGHEST held index wins when several keys are down.
    int found = -1;
    for (int i = 0; i < numkeys; ++i)
        if (keys[i]) found = i;
    if (found >= 0) {
        edited_[capture_set_].scancode[capture_action_] = found;
        capturing_ = false;
    }
}

void KeyRemapScreen::draw(SDL_Renderer* ren) const {
    if (!ren) return;
    // Each original frame starts with sub_415CA4 — a memcpy of the SAVED
    // backdrop store, which holds the Options screen's GLUE picture (the
    // rows were never saved into it). Re-blit that same GLUE image.
    if (assets_) {
        const Sprite& bg = assets_->frontend_pcx(backdrop_);
        if (bg.tex) {
            SDL_FRect d{0, 0, static_cast<float>(bg.w), static_cast<float>(bg.h)};
            SDL_RenderTexture(ren, bg.tex, nullptr, &d);
        } else {
            SDL_SetRenderDrawColor(ren, 20, 20, 30, 255);
            SDL_RenderClear(ren);
        }
    }
    if (!font_ || !font_->loaded()) return;

    // Title (chrome audit 2026-07-12): getstring(1100) at (20, 20), clip
    // 400, ink byte_49D37A — LUT-true (252,248,88) — black outline.
    // Recovered from the raw EXE bytes at 0x407BD0 after Hex-Rays lost the
    // x; the old (400,20) header read the CLIP WIDTH as the x.
    font_->draw_outlined(ren, msg(1100, "Keyboard definitions"), 20.0f, 20.0f, kYellowR, kYellowG,
                         kYellowB, 0, 0, 0, 400.0f);

    // The 2x6 grid — REAL bevel-button widgets (sub_432298) at
    // (320*set + 100, 60*action + 60), labelled getstring(1110) "Key %u, %s"
    // (0-based set); pressed bevel while the mouse holds inside one
    // (sub_432998). The bound-key line getstring(1140) "Key: '%s'" sits
    // 22 px below, white/black outline, clip 200 — drawn ONLY while
    // `(code & 0x7F) < 0x59` (pseudo.c 8743-8744), with the name from the
    // original's own table (the E0 arrows alias their numpad names).
    for (int set = 0; set < kKeyboardSets; ++set) {
        for (int action = 0; action < kKeyActionCount; ++action) {
            const Rect r = grid_rect(set, action);
            const int id = 1000 * set + action + 1000;
            const bool pressed = pressed_id_ == id && widget_at(mouse_x_, mouse_y_) == id &&
                                 !capturing_ && !showing_note_;
            draw_dialog_button(ren, *font_, r.x, r.y, grid_label(set, action), pressed);
            const int dos =
                dos_scancode_from_sdl(edited_[set].scancode[action]);
            if (const char* name = dos_scancode_name(dos)) {
                const std::string keyname = fmt_s(msg(1140, "Key: '%s'"), name);
                font_->draw_outlined(ren, keyname, r.x, r.y + 22.0f, kWhiteR, kWhiteG, kWhiteB, 0,
                                     0, 0, 200.0f);
            }
        }
    }

    // "Return to default keys" (getstring(1130)) — widget id 999 at
    // (40, 430) (pseudo.c 8762-8763).
    {
        const Rect r = defaults_rect();
        const bool pressed = pressed_id_ == 999 && widget_at(mouse_x_, mouse_y_) == 999 &&
                             !capturing_ && !showing_note_;
        draw_dialog_button(ren, *font_, r.x, r.y, msg(1130, "Return to default keys"), pressed);
    }

    const float lh = static_cast<float>(font_->line_height());
    if (capturing_) {
        // sub_407AD9 -> sub_412E33's completion window: 360 x 8*fontheight
        // at y=200, x centred, WINZ 9-patch. Contents: the getstring(1105)
        // message centred at 1.5 lines in white, the "%d%%" readout (0%)
        // centred at 3.5 lines in yellow, and the white-framed 300-px track
        // at 5.5 lines with its interior black (0% done: grey fill width
        // 3*pct = 0, black remainder 300).
        const DialogRect win{(640.0f - 360.0f) / 2.0f, 200.0f, 360.0f, 8.0f * lh};
        draw_dialog_chrome(ren, win, assets_ ? &assets_->frontend_pcx("WINZ") : nullptr);
        const std::string m =
            fmt_s(msg(1105, "Press key for '%s'"), grid_label(capture_set_, capture_action_));
        const float mw = static_cast<float>(font_->measure(m));
        font_->draw_outlined(ren, m, win.x + (360.0f - mw) / 2.0f, win.y + 1.5f * lh, kWhiteR,
                             kWhiteG, kWhiteB, 0, 0, 0);
        const std::string pct = "0%";  // aD_0 = "%d%%", pinned 0 for the whole wait
        const float pw = static_cast<float>(font_->measure(pct));
        font_->draw_outlined(ren, pct, win.x + (360.0f - pw) / 2.0f, win.y + 3.5f * lh, kYellowR,
                             kYellowG, kYellowB, 0, 0, 0);
        SDL_FRect track{win.x + 31.0f, win.y + 5.5f * lh + 1.0f, 300.0f, lh - 1.0f};
        SDL_SetRenderDrawColor(ren, 0, 0, 0, 255);
        SDL_RenderFillRect(ren, &track);
        SDL_FRect frame{win.x + 30.0f, win.y + 5.5f * lh, 302.0f, lh + 1.0f};
        SDL_SetRenderDrawColor(ren, kWhiteR, kWhiteG, kWhiteB, 255);
        SDL_RenderRect(ren, &frame);
    }

    if (showing_note_) {
        // The post-restore acknowledge modal (pseudo.c 8799-8806: sub_414340
        // with getstring(95) "NOTE!" on top, getstring(1131) below, the
        // general white ink, and its " Ok " button).
        const bool ok_pressed =
            pressed_id_ == 27 && note_ok_rect().contains(mouse_x_, mouse_y_);
        draw_acknowledge_dialog(ren, *font_, assets_ ? &assets_->frontend_pcx("WINZ") : nullptr,
                                msg(95, "NOTE!"), msg(1131, "Default key controls restored"),
                                msg(27, " Ok "), kWhiteR, kWhiteG, kWhiteB, ok_pressed);
    }

    // The mouse cursor, last (the original's cursor composites over
    // everything): the widget library's own 8x8 arrow, hotspot (1,1).
    for (int row = 0; row < 8; ++row) {
        for (int col = 0; col < 8; ++col) {
            const char c = kMouseCursorRows[row][col];
            if (c == '.') continue;
            if (c == 'W')
                SDL_SetRenderDrawColor(ren, kWhiteR, kWhiteG, kWhiteB, 255);
            else
                SDL_SetRenderDrawColor(ren, 60, 68, 56, 255);  // byte_497498, LUT 0x2108
            SDL_FRect px{mouse_x_ - kCursorHotX + static_cast<float>(col),
                         mouse_y_ - kCursorHotY + static_cast<float>(row), 1.0f, 1.0f};
            SDL_RenderFillRect(ren, &px);
        }
    }
}

}  // namespace bomber::game
