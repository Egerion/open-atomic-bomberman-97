#pragma once

#include <string>

#include "bomber/game/app_flow.hpp"
#include "bomber/game/options_screen.hpp"  // OptionsScreen (SchemePickerRunner param)
#include "bomber/game/screen_context.hpp"
#include "bomber/game/screens/options_state.hpp"

// The Options-cluster screens, extracted VERBATIM from GameApp (ADR-0009 §4):
// the interactive Options screen and its two modal sub-screens (the key-remap
// grid and the *.SCH file picker). Each owns its own nested SDL event loop and
// returns exactly what the GameApp method it replaced returned. Two seams,
// both stored BY VALUE: ScreenContext (the shared front-end services) and
// OptionsEditState (the Options-specific mutable state GameApp still owns).
//
// Named ...Runner so they do NOT collide with the OptionsScreen / KeyRemapScreen
// / SchemeFilePicker *components* (options_screen.hpp / keyremap_screen.hpp /
// editor_screen.hpp) each runner drives — the runner is the outer event loop,
// the component is the widget it pumps.

namespace bomber::game {

// The interactive Options screen (was GameApp::present_options_screen): random
// GLUE<n> backdrop, Up/Down select a row, Left/Right change a value, Enter/Esc
// leave; F1 opens the generic *.BM help browser, and the "Define keyboard
// layouts" / "Scheme File" rows push the two sub-runners below modally. Returns
// Advance (Enter/Esc route the leaf back to the menu) or Quit on window close.
class OptionsScreenRunner {
public:
    OptionsScreenRunner(ScreenContext ctx, OptionsEditState state) : ctx_(ctx), state_(state) {}
    AppInput run();

private:
    ScreenContext ctx_;
    OptionsEditState state_;
};

// The key-remap sub-screen (was GameApp::present_keyremap_screen, sub_407B9D):
// the mouse-driven 2x6 button grid, re-blitting the Options screen's own GLUE
// backdrop each frame. Applies its edits to the live KeyboardMapper and marks
// options_dirty_ on exit.
class KeyRemapScreenRunner {
public:
    KeyRemapScreenRunner(ScreenContext ctx, OptionsEditState state) : ctx_(ctx), state_(state) {}
    void run(const std::string& backdrop);

private:
    ScreenContext ctx_;
    OptionsEditState state_;
};

// The Options row-8 *.SCH file picker (was GameApp::present_scheme_picker,
// sub_407582): run modally over the Options screen's GLUE backdrop; a selection
// writes the picked stem into `opt` and reloads the live scheme_. Takes the
// OptionsScreen& the same way the original did.
class SchemePickerRunner {
public:
    SchemePickerRunner(ScreenContext ctx, OptionsEditState state) : ctx_(ctx), state_(state) {}
    void run(OptionsScreen& opt, const std::string& backdrop);

private:
    ScreenContext ctx_;
    OptionsEditState state_;
};

}  // namespace bomber::game
