#pragma once

#include "bomber/ui/screen_context.hpp"

// PORT-ONLY "Video Settings" panel (F10 from the menu), extracted from
// GameApp::present_video_settings (ADR-0008): vsync / native-cadence / show-fps /
// soft-scaling toggles, kept SEPARATE from the RE'd Options screen so its exact
// rows stay faithful. The four toggles + the options-dirty flag live on GameApp;
// the screen mutates them through this pointer bundle (keeps the ctor at two
// args, the <=4-param rule) and applies vsync live via ctx.sdl and soft scaling
// live via set_scale_filter().

namespace bomber::game {

struct VideoToggleRefs {
    bool* uncap_fps = nullptr;      // "VSync On" == uncap OFF
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
    ScreenContext ctx_;
    VideoToggleRefs toggles_;
};

}  // namespace bomber::game
