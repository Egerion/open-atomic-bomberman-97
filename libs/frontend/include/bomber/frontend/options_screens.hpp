#pragma once

#include <optional>
#include <string>

#include "bomber/frontend/keyremap_screen.hpp"  // KeyRemapScreen (the pump's param)
#include "bomber/frontend/options_screen.hpp"   // OptionsScreen
#include "bomber/frontend/options_state.hpp"
#include "bomber/game_util/app_flow.hpp"
#include "bomber/ui/screen_context.hpp"

// The Options-cluster screens: the interactive Options screen and its two modal
// sub-screens. Named ...Runner so they do NOT collide with the OptionsScreen /
// KeyRemapScreen / SchemeFilePicker *components* each one drives — the runner is
// the outer event loop, the component is the widget it pumps.

namespace bomber::game {

// Random GLUE<n> backdrop; Esc leaves, F1 opens the generic *.BM help browser,
// and three rows push a sub-screen modally.
class OptionsScreenRunner {
public:
    OptionsScreenRunner(ScreenContext ctx, OptionsEditState state) : ctx_(ctx), state_(state) {}
    AppInput run();

private:
    std::optional<AppInput> pump_events(OptionsScreen& opt, const std::string& glue);
    void open_sub_screens(OptionsScreen& opt, const std::string& glue);
    void commit(const OptionsScreen& opt);

    ScreenContext ctx_;
    OptionsEditState state_;
    // How the screen was dismissed, latched during the pump: Esc routes the leaf
    // Back, anything else Advance. Both persist an already-made change.
    AppInput result_ = AppInput::Advance;
};

// The key-remap sub-screen (sub_407B9D), re-blitting the Options screen's own
// GLUE backdrop each frame. Applies its edits to the live KeyboardMapper on exit.
class KeyRemapScreenRunner {
public:
    KeyRemapScreenRunner(ScreenContext ctx, OptionsEditState state) : ctx_(ctx), state_(state) {}
    void run(const std::string& backdrop);

private:
    bool pump_events(KeyRemapScreen& remap);
    void handle_mouse(KeyRemapScreen& remap, const SDL_Event& ev);

    ScreenContext ctx_;
    OptionsEditState state_;
};

// The Options row-8 *.SCH file picker (sub_407582), run modally over the Options
// screen's GLUE backdrop; a selection writes the picked name into `opt` and
// reloads the live scheme.
class SchemePickerRunner {
public:
    SchemePickerRunner(ScreenContext ctx, OptionsEditState state) : ctx_(ctx), state_(state) {}
    void run(OptionsScreen& opt, const std::string& backdrop);

private:
    ScreenContext ctx_;
    OptionsEditState state_;
};

}  // namespace bomber::game
