#pragma once

#include <cctype>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <string>
#include <system_error>

#include "bomber/assets/reslist.hpp"
#include "bomber/assets/sch.hpp"

// Cross-screen front-end helpers shared by the pre-match screens (player setup,
// options, level select, scheme editor, campaign picker). Free functions in one
// header so a SINGLE copy of the presentation-LCG advance is used everywhere:
// the ADR-0008 decomposition put these screens in separate files, and a
// per-screen copy of pick_glue would desync the GLUE sequence across them.

namespace bomber::game {

// A random GLUE<n> backdrop name (sub_4148E5: getvalue(16) count, rand()%n),
// advancing the shared presentation LCG IN PLACE — never sim::State::rng. Seven
// front-end call sites depend on this being the SAME helper; a per-screen copy
// still returns plausible names, so only a test notices.
inline std::string pick_glue(std::uint32_t& setup_lcg, const assets::res::ValueList& values) {
    setup_lcg = setup_lcg * 1664525u + 1013904223u;
    int glue_n = static_cast<int>(values.column_or(16, 0, 7));  // getvalue(16)
    if (glue_n < 1) glue_n = 1;
    return "GLUE" + std::to_string(static_cast<int>((setup_lcg >> 16) % static_cast<unsigned>(glue_n)));
}

// Case-insensitive DATA/SCHEMES/<name>.SCH resolve + load into `scheme`. Returns
// false (scheme untouched) when the name doesn't resolve or the file is corrupt.
// Shared by the Options scheme-picker, the campaign stage loader, and init()'s
// options.ini schemefilename= resolution (the original re-parses byte_4648C4 at
// Play-flow entry, sub_410F81 -> sub_4046CC -> sub_403EEE).
inline bool reload_scheme(assets::sch::Scheme& scheme, const std::filesystem::path& game_dir,
                          const std::string& name) {
    // With or without an extension: "BASIC.SCH" from the picker (sub_407582 cuts
    // the row at its ':' and keeps the extension) and a hand-edited "BASIC" from
    // options.ini alike. Mirrors sub_403EEE @0x403FE8 — strrchr(name, '.'),
    // truncate, strcat ".sch" — the LAST dot, so "MY.MAP.SCH" keeps "MY.MAP".
    std::string want = name;
    if (auto dot = want.rfind('.'); dot != std::string::npos) want.erase(dot);
    for (auto& c : want) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    if (want.empty()) return false;
    std::filesystem::path schemes_dir = game_dir / "DATA" / "SCHEMES";
    std::error_code ec;
    std::filesystem::path found;
    for (const auto& entry : std::filesystem::directory_iterator(schemes_dir, ec)) {
        if (!entry.is_regular_file()) continue;
        std::string stem = entry.path().stem().string();
        std::string ext = entry.path().extension().string();
        for (auto& c : stem) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        for (auto& c : ext) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        if (ext == ".SCH" && stem == want) {
            found = entry.path();
            break;
        }
    }
    if (found.empty()) return false;
    try {
        scheme = assets::sch::load(found);
    } catch (const std::exception&) {
        return false;
    }
    return true;
}

}  // namespace bomber::game
