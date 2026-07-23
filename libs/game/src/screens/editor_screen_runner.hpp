#pragma once

#include "bomber/game/screen_context.hpp"
#include "bomber/game/screens/editor_state.hpp"

// The hidden scheme EDITOR screen, extracted VERBATIM from GameApp (ADR-0009 §9,
// docs/re/results-and-options.md §5): the chooser (sub_403184) -> optional *.SCH
// file picker (sub_407582) -> the editor proper (sub_4028D2) -> optional powerup
// rules sub-editor (sub_402595), a nested loop that owns its own SDL event pump
// and returns to present_menu's loop when the chooser is dismissed (there is no
// AppState/AppInput slot — the editor has no menu row, only present_menu's raw
// Ctrl+E x6 trigger reaches it). On a confirmed save it writes the edited scheme
// via assets::sch::write() into the install's DATA/SCHEMES dir (never the repo)
// and repoints the live scheme_ so the edit is immediately selectable.
//
// Named EditorRunner so it does NOT collide with the EditorChooserScreen /
// EditorScreen / SchemeFilePicker *components* (editor_screen.hpp) it drives —
// the runner is the outer event loop, the components are the widgets it pumps.
// Two seams, both stored BY VALUE: ScreenContext (the shared front-end services)
// and EditorEditState (the editor-specific mutable state GameApp still owns).

namespace bomber::game {

class EditorRunner {
public:
    EditorRunner(ScreenContext ctx, EditorEditState state) : ctx_(ctx), state_(state) {}
    void run();

private:
    ScreenContext ctx_;
    EditorEditState state_;
};

}  // namespace bomber::game
