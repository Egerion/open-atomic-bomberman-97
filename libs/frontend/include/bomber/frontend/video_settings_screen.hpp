#pragma once

#include <SDL3/SDL.h>

#include "bomber/ui/dialog_chrome.hpp"  // DialogRect
#include "bomber/ui/screen_context.hpp"

// PORT-ONLY "Video Settings" panel (F10 from the menu), kept SEPARATE from the
// RE'd Options screen so its exact rows stay faithful. The four toggles live on
// the shell; the screen mutates them through this pointer bundle and applies
// vsync and soft scaling live.

namespace bomber::game {

struct VideoToggleRefs {
    bool* uncap_fps = nullptr;  // "VSync On" == uncap OFF
    bool* native_cadence = nullptr;
    bool* show_fps = nullptr;
    // "SOFT SCALING" — linear instead of nearest sampling on the upscale
    // (scale_filter.hpp). Deliberately labelled for what it DOES: calling it a
    // "retro" mode would imply crisp pixels are the inauthentic choice, when the
    // original scaled nothing at all and crisp is the closer of the two.
    bool* soft_scaling = nullptr;
    bool* options_dirty = nullptr;  // set when any toggle changes (flush-on-exit)
};

class VideoSettingsScreen {
public:
    VideoSettingsScreen(ScreenContext ctx, VideoToggleRefs toggles)
        : ctx_(ctx), toggles_(toggles) {}
    void run();

private:
    // False means "close" (Escape or window close).
    bool pump_events();
    static bool is_toggle_key(SDL_Keycode k);
    void toggle_row();
    void draw();
    void draw_rows(const DialogRect& win, float lh);

    ScreenContext ctx_;
    VideoToggleRefs toggles_;
    int row_ = 0;
};

}  // namespace bomber::game
