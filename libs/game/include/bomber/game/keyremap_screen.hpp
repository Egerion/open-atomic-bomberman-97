#pragma once

#include <SDL3/SDL.h>

#include <array>
#include <cstdint>
#include <string>

#include "bomber/game/asset_store.hpp"
#include "bomber/audio/audio_engine.hpp"
#include "bomber/game/bmscreen.hpp"
#include "bomber/game/input.hpp"

// The key-remap UI — sub_407B9D @0x407B9D (docs/re/results-and-options.md
// §2), rebuilt 1:1 2026-07-13 from a full body re-read: the screen is
// MOUSE-DRIVEN. It brackets itself in the widget library's mouse-cursor
// show/hide pair (sub_431178/sub_431360 — the same pair the sub_41485A list
// dialogs use), creates 13 REAL bevel-button widgets (sub_432298: 2x6 grid
// id 1000*set+action at x = 320*set+100, y = 60*action+60, plus id 999
// "Return to default keys" at (40, 430)), and reads clicks back as widget
// ids from the same getkey queue (sub_4102B7 returns 1000..2998 / 999).
// There is NO keyboard navigation over the grid in the original — the
// keyboard's only roles are Enter(13)/Space(32)/Esc(27) = leave the screen
// and F1(0x13B) = help browser; the port's former arrow-key focus cursor +
// yellow focus rect were invented and are gone.
//
// Widget behaviour (sub_432998, the widget-lib pump): a button shows its
// "down" bitmap while the mouse button is held INSIDE it (bevel pair
// swapped, unwashed face — dialog_chrome.hpp's pressed state) and fires its
// id on RELEASE inside. The default mouse cursor is the widget library's own
// 8x8 bitmap (byte_45C310, read from BM95.EXE data + sub_430E4C's colour
// remap: value 15 -> white byte_49D38F, 1 -> byte_497498 = LUT 0x2108
// (60,68,56), 0 -> transparent), hotspot (1,1) (sub_430EDC's default-arm).
//
// Chrome (all inks LUT-true, docs/re/frontend-flow.md "COLOR.PAL"):
// getstring(1100) header at (20, 20) clip 400 in byte_49D37A (252,248,88);
// under each grid button, getstring(1140) "Key: '%s'" at (x, y+22) clip 200
// in byte_49D38F (240,248,252) — drawn ONLY while `(code & 0x7F) < 0x59`,
// with the name from the original's own 89-entry scancode-name table
// (dos_scancode.hpp; the E0-extended arrows alias their numpad names). The
// screen paints NO backdrop of its own — each frame restores the SAVED
// backdrop store (sub_415CA4 = memcpy from dword_460BCC), which holds the
// Options screen's random GLUE picture (not its rows), so this draw()
// re-blits that same GLUE image.
//
// Rebind (sub_407AD9): activating a grid button opens the completion-window
// modal (sub_412E33's chrome: 360 x 8*fontheight window at y=200, WINZ
// 9-patch, the getstring(1105) message centered at 1.5 lines in white, a
// "%d%%" readout at 3.5 lines in yellow, the white-framed 300-px track at
// 5.5 lines), waits 500 ms with the key queue flushed (sub_413CB0(500) +
// sub_41043C), then polls the RAW 256-byte scancode state (byte_4A2BA0)
// until any key is down — the 0..255 ascending scan keeps overwriting its
// result, so the HIGHEST held index wins — and stores it UNCONDITIONALLY
// (the caller writes it into the binding table dword_4645BC at index
// 10*set + action, with no cancel
// branch: pressing Esc binds Esc; only the tap-released-between-polls race
// can store 0 = unbound, a timing artifact this port does not reproduce).
// No validation, duplicates across actions allowed.
//
// Sounds: sub_407B9D itself plays NOTHING. The only sound on this screen is
// the nav blip inside sub_414340's own key loop — the "NOTE!"/"Default key
// controls restored" acknowledge modal the defaults button pops (blip 20 on
// any key; closes on Enter/Space/Esc or its " Ok " button, getstring(27)).
//
// Persistence: edits an in-memory copy (edited_); the caller applies it to
// the live KeyboardMapper on dismissal, and options.ini's keydef= triples
// (DOS scancodes — dos_scancode.hpp translates) flush on app exit like every
// other option (§2's sub_405DE3-via-exit-hook write-back).
namespace bomber::game {

class KeyRemapScreen {
public:
    KeyRemapScreen(const AssetStore& assets, const FontTextures& font)
        : assets_(&assets), font_(&font) {}

    // (Re)enter with the CURRENT live bindings and the Options screen's own
    // GLUE backdrop name (the saved-backdrop restore described above).
    void enter(const std::array<KeySet, kKeyboardSets>& current, std::string backdrop);

    // Frame clock (SDL_GetTicks) — drives the capture modal's 500 ms arm
    // delay (sub_413CB0(500)).
    void tick(std::uint64_t now_ms) { now_ms_ = now_ms; }

    // Keyboard. While capturing: any keydown past the arm delay binds its
    // scancode (Esc included — see the file doc). While the NOTE modal is
    // up: sub_414340's own loop (blip on any key, close on Enter/Space/Esc).
    // Otherwise: Enter/Space/Esc end the screen; everything else is inert
    // and SILENT (sub_407B9D plays no sound).
    void on_key(SDL_Keycode key, SDL_Scancode scancode, AudioEngine& audio);

    // Mouse, in 640x480 logical coordinates. Buttons show the pressed bevel
    // while held inside and fire on release inside (sub_432998).
    void on_mouse_move(float x, float y);
    void on_mouse_down(float x, float y);
    void on_mouse_up(float x, float y, AudioEngine& audio);

    // sub_407AD9's raw keyboard-state poll — call once per frame while
    // capturing() (binds a key HELD across the arm delay, which the event
    // path alone would miss).
    void poll_capture();

    void draw(SDL_Renderer* ren) const;

    bool done() const { return done_; }
    bool capturing() const { return capturing_; }
    bool showing_note() const { return showing_note_; }
    const std::array<KeySet, kKeyboardSets>& edited() const { return edited_; }

private:
    struct Rect {
        float x, y, w, h;
        bool contains(float px, float py) const {
            return px >= x && px < x + w && py >= y && py < y + h;
        }
    };

    std::string msg(int id, const char* fb) const;
    // getstring(1110) "Key %u, %s" with the 0-BASED set number and the
    // action name getstring(1120+action) — sub_407B9D formats i, not i+1.
    std::string grid_label(int set, int action) const;
    Rect grid_rect(int set, int action) const;
    Rect defaults_rect() const;
    Rect note_ok_rect() const;
    // sub_432298's id scheme: 1000*set+action+1000 for the grid, 999 for the
    // defaults button; -1 = no widget at (x, y).
    int widget_at(float x, float y) const;
    void activate_widget(int id);

    const AssetStore* assets_ = nullptr;
    const FontTextures* font_ = nullptr;

    std::string backdrop_;  // the Options screen's GLUE<n> pick
    std::array<KeySet, kKeyboardSets> edited_{};

    float mouse_x_ = 320.0f, mouse_y_ = 240.0f;
    int pressed_id_ = -1;  // widget under a held left button (27 = the NOTE Ok)

    bool capturing_ = false;
    int capture_set_ = 0;
    int capture_action_ = 0;
    std::uint64_t now_ms_ = 0;
    std::uint64_t capture_start_ms_ = 0;

    bool showing_note_ = false;
    bool done_ = false;
};

}  // namespace bomber::game
