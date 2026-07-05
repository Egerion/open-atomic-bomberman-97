#pragma once

#include <filesystem>
#include <optional>

namespace bomber::assets {

// Locates the player's original Atomic Bomberman installation. Probes, in
// order: the BOMBER_GAME_DIR environment variable, a gamedir.txt in the
// working directory, then the standard install locations. Returns an empty
// path when nothing is found.
std::filesystem::path default_game_dir();

// The install-root `options.ini` (parsed by sub_406238): a plain "key=value"
// text file the original reads at startup. Each field is optional — a key
// absent from the file leaves the corresponding optional empty so the caller
// keeps the binary's hardcoded default rather than guessing. Only the fields
// the sim/presentation actually consume are surfaced here; the rest (keydefs,
// modem, netprotocol, …) are irrelevant to a local match and skipped.
struct Options {
    // "conveyor_speed=" — the Conveyor Speed game-option index (dword_464930):
    // 0 low, 1 medium, 2 high; addresses VALUELST 190/191/192 via
    // getvalue(190+idx). The binary defaults it to 1 (medium, pseudo.c 14652)
    // when no options.ini is present; this file overrides it. Clamped by the
    // consumer to [0, getvalue(189)-1].
    std::optional<int> conveyor_speed;
};

// Reads and parses `<path>` (the install-root options.ini). A missing or
// unreadable file yields a default-constructed Options (all fields empty).
// Mirrors the original's line parse: split each line on '=', match the key
// case-insensitively, atoi the value.
Options load_options(const std::filesystem::path& path);

}  // namespace bomber::assets
