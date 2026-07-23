#pragma once

#include <string>

#include "bomber/game/screen_context.hpp"

// The scheme editor's save-as filename line-edit (sub_42E938, getstring 736),
// seeded with `seed` (the source filename when editing an existing scheme).
// Extracted verbatim from GameApp::present_scheme_filename_prompt (ADR-0009):
// max 30 chars; Enter-on-empty or Escape returns `seed`. Services-only.

namespace bomber::game {

class SchemeFilenamePrompt {
public:
    explicit SchemeFilenamePrompt(ScreenContext ctx) : ctx_(ctx) {}
    // Returns the chosen stem (no extension; the caller sanitises + appends .SCH).
    std::string run(const std::string& seed);

private:
    ScreenContext ctx_;
};

}  // namespace bomber::game
