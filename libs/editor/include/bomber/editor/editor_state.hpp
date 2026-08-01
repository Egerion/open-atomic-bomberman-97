#pragma once

#include <cstdint>
#include <filesystem>

#include "bomber/assets/sch.hpp"

// Seam 2 (ADR-0009 §"shared front-end state"): the non-service state the hidden
// scheme editor mutates, bundled so EditorRunner can be its own class without
// threading a GameApp&. A cheap value type (references only), copied by value
// into the runner; every referent is a GameApp member that outlives it.
// ScreenContext stays front-end-service-only, so this lives separately.

namespace bomber::game {

struct EditorEditState {
    std::uint32_t& setup_lcg;               // the shared presentation LCG pick_glue advances
    assets::sch::Scheme& scheme;            // repointed at the freshly-saved scheme
    const std::filesystem::path& game_dir;  // the *.SCH picker path + the save dir
    std::filesystem::path& scheme_path;     // repointed at the saved file alongside `scheme`
};

}  // namespace bomber::game
