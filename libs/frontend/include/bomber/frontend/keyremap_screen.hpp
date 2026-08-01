#pragma once

#include <SDL3/SDL.h>

#include <array>
#include <cstdint>
#include <string>

#include "bomber/audio/audio_engine.hpp"
#include "bomber/input/input.hpp"
#include "bomber/render/asset_store.hpp"
#include "bomber/ui/bmscreen.hpp"

// The key-remap UI — sub_407B9D @0x407B9D, rebuilt 1:1 2026-07-13 from a full
// body re-read. Widget ids, anchors, inks and the cursor bitmap are in
// docs/frontend-options-rows.md; three facts belong here because breaking them is
// silent:
//
//  - THE SCREEN IS MOUSE-DRIVEN. There is NO keyboard navigation over the grid;
//    the keyboard's only roles are Enter/Space/Esc and F1. The port's former
//    arrow-key focus cursor and yellow focus rect were invented.
//  - Rebind stores the captured scancode UNCONDITIONALLY — pressing Esc binds
//    Esc. No validation; duplicates across actions are allowed.
//  - sub_407B9D itself plays NOTHING; the only sound is the nav blip inside
//    sub_414340's own key loop.
//
// Edits go to an in-memory copy the caller applies on dismissal.

namespace bomber::game {

class KeyRemapScreen {
public:
    KeyRemapScreen(const AssetStore& assets, const FontTextures& font)
        : assets_(&assets), font_(&font) {}

    // (Re)enter with the CURRENT live bindings and the Options screen's own GLUE
    // backdrop name (the sub_415CA4 saved-backdrop restore).
    void enter(const std::array<KeySet, kKeyboardSets>& current, std::string backdrop);

    // Frame clock — drives the capture modal's 500 ms arm delay.
    void tick(std::uint64_t now_ms) { now_ms_ = now_ms; }

    // While capturing: any keydown past the arm delay binds its scancode. While
    // the NOTE modal is up: blip on any key, close on Enter/Space/Esc. Otherwise
    // those same three end the screen and everything else is inert and SILENT.
    void on_key(SDL_Keycode key, SDL_Scancode scancode, AudioEngine& audio);

    // Mouse, in 640x480 logical coordinates. Buttons show the pressed bevel while
    // held inside and fire on release inside (sub_432998).
    void on_mouse_move(float x, float y);
    void on_mouse_down(float x, float y);
    void on_mouse_up(float x, float y, AudioEngine& audio);

    // sub_407AD9's raw keyboard-state poll — call once per frame while
    // capturing(): it binds a key HELD across the arm delay, which the event path
    // alone would miss.
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
    // getstring(1110) "Key %u, %s" with the 0-BASED set number and the action
    // name getstring(1120+action) — sub_407B9D formats i, not i+1.
    std::string grid_label(int set, int action) const;
    Rect text_button_rect(const std::string& label, float x, float y) const;
    Rect grid_rect(int set, int action) const;
    Rect defaults_rect() const;
    Rect note_ok_rect() const;
    // -1 = no widget at (x, y).
    int widget_at(float x, float y) const;
    bool widget_pressed(int id) const;
    void activate_widget(int id);
    static bool is_dismiss_key(SDL_Keycode key);

    void draw_backdrop(SDL_Renderer* ren) const;
    void draw_grid(SDL_Renderer* ren) const;
    void draw_grid_button(SDL_Renderer* ren, int set, int action) const;
    void draw_defaults_button(SDL_Renderer* ren) const;
    void draw_capture_modal(SDL_Renderer* ren) const;
    void draw_note_modal(SDL_Renderer* ren) const;
    void draw_mouse_cursor(SDL_Renderer* ren) const;

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
