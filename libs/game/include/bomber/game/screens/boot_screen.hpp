#pragma once

#include "bomber/game/app_flow.hpp"
#include "bomber/game/screen_context.hpp"

// The IPLOGO -> HSLOGO -> TITLE boot presentation (sub_42B060). LINEAR — no
// attract re-run: each screen advances on a key OR the getvalue(12) = 7 s
// timeout, and the title's Advance returns so run_app drops into the menu.
// Extracted verbatim from GameApp::run_boot_attract (ADR-0009).

namespace bomber::game {

class BootScreen {
public:
    explicit BootScreen(ScreenContext ctx) : ctx_(ctx) {}
    // Returns Advance to enter the menu, or Back/Quit to short-circuit.
    AppInput run();

private:
    ScreenContext ctx_;
};

}  // namespace bomber::game
