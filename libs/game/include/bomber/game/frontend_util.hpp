#pragma once

#include <cstdint>
#include <string>

#include "bomber/assets/reslist.hpp"

// Cross-screen front-end helpers shared by the pre-match screens (player setup,
// options, level select, scheme editor, campaign picker). Kept as free
// functions in a shared header so a SINGLE copy of the shared presentation-LCG
// advance is used everywhere — the god-object decomposition (ADR-0008) moves
// these screens into their own files, and a per-screen copy of pick_glue would
// desync the GLUE-pick RNG sequence across them.

namespace bomber::game {

// A random GLUE<n> backdrop name (sub_4148E5: getvalue(16) count, rand()%n).
// Advances the presentation LCG `setup_lcg` IN PLACE (never sim::State::rng) —
// the one LCG every pre-match screen shares. Seven call sites across the
// front-end depend on this being the same helper; a per-screen copy would
// diverge the GLUE picks (a visible backdrop difference).
inline std::string pick_glue(std::uint32_t& setup_lcg, const assets::res::ValueList& values) {
    setup_lcg = setup_lcg * 1664525u + 1013904223u;
    int glue_n = static_cast<int>(values.column_or(16, 0, 7));  // getvalue(16)
    if (glue_n < 1) glue_n = 1;
    return "GLUE" +
           std::to_string(static_cast<int>((setup_lcg >> 16) % static_cast<unsigned>(glue_n)));
}

}  // namespace bomber::game
