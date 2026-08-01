#pragma once

#include "bomber/editor/editor_state.hpp"
#include "bomber/ui/screen_context.hpp"

// The hidden scheme EDITOR screen (docs/re/results-and-options.md §5): the
// chooser (sub_403184) -> optional *.SCH file picker (sub_407582) -> the editor
// proper (sub_4028D2) -> optional powerup-rules sub-editor (sub_402595). A
// nested loop that owns its own SDL event pump and returns to present_menu's
// loop when the chooser is dismissed — there is no AppState/AppInput slot, since
// only present_menu's raw Ctrl+E x6 trigger reaches the editor at all. On a
// confirmed save it writes through assets::sch::write() into the install's
// DATA/SCHEMES dir (never the repo) and repoints the live scheme.
//
// Named EditorRunner so it does not collide with the EditorChooserScreen /
// EditorScreen / SchemeFilePicker *components* it drives (editor_screen.hpp):
// the runner is the outer event loop, the components are the widgets it pumps.

namespace bomber::game {

class EditorRunner {
public:
    // Both seams are stored BY VALUE: the shared front-end services, and the
    // editor-specific mutable state GameApp still owns.
    EditorRunner(ScreenContext ctx, EditorEditState state) : ctx_(ctx), state_(state) {}
    void run();

private:
    ScreenContext ctx_;
    EditorEditState state_;
};

}  // namespace bomber::game
