#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>

#include "bomber/assets/reslist.hpp"
#include "bomber/assets/sch.hpp"
#include "bomber/frontend/options_screen.hpp"  // OptionsSnapshot
#include "bomber/game/app_options.hpp"
#include "bomber/input/input.hpp"
#include "bomber/render/asset_store.hpp"

// The config ROUND TRIP: scheme + VALUELST + options.ini + nodename.ini in, the same
// two files out at exit. Load and save live in one file because they must agree key
// for key — a row added to one and forgotten in the other silently stops persisting.

namespace bomber::game {

// The shell members the round trip reads and writes, bundled by reference the way
// ScreenContext bundles the presentation services.
struct SettingsSlots {
    const AppOptions& opts;
    AssetStore& assets;  // getstring, for the absent-nodename fallback only
    assets::sch::Scheme& scheme;
    assets::res::ValueList& values;
    KeyboardMapper& keyboard;
    OptionsSnapshot& options;
    bool& options_dirty;
    std::optional<int>& conveyor_speed_index;
    std::optional<int>& num_to_win_match;
    bool& team_play;
    bool& fullscreen;
    bool& uncap_fps;
    bool& native_cadence;
    bool& show_fps;
    bool& soft_scaling;
    std::uint32_t& setup_lcg;
    std::filesystem::path& options_path;
    std::filesystem::path& node_name_path;
    std::string& node_name_loaded;
};

// Scheme + VALUELST + options.ini into the slots. False on any parse error (the
// whole load is one try/catch). `opts.game_dir` must already be resolved.
bool load_settings(const SettingsSlots& s, const std::filesystem::path& scheme_path);

// The absent-NODENAME.INI fallback (sub_40C74C): a random one of the 49 names at
// MESSAGES ids 500..548, `getstring(500 + rand() % getvalue(47))`. Needs the message
// table, so it runs after the assets load rather than beside the file read above.
void seed_default_node_name(const SettingsSlots& s);

// Write-on-exit (docs/re/results-and-options.md §2 "Persistence — CONFIRMED via an
// exit-time write-back": sub_405DE3 only runs through sub_410EBF's atexit-style hook
// on a normal exit, never per-edit). Idempotent, and a no-op unless something
// changed. Also flushes nodename.ini, whose original has its OWN shutdown callback
// (sub_40C4DB -> sub_40C140), folded in so the shell keeps one exit-time call.
void flush_settings(const SettingsSlots& s);

}  // namespace bomber::game
