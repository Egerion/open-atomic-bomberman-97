#pragma once

#include <string>

#include "bomber/ui/screen_context.hpp"

// The scheme editor's save-as filename line-edit (sub_42E938, getstring 736),
// seeded with the source filename when editing an existing scheme. Max 30
// characters; Enter-on-empty or Escape returns the seed.

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
