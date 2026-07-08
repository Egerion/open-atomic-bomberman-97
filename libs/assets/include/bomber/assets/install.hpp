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
    // "team_play=" — the Team Play game-option toggle (0/1). NOT independently
    // RE'd from the binary (no sub_XXXX citation for this exact key/site); the
    // key name mirrors this install's shipped options.ini, which already
    // carries a "team_play=0" line (see tests/test_options.cpp). The setup
    // screen's team-mode display (docs/re/setup-screens.md, dword_464964) is
    // the confirmed CONSUMER of a team-mode flag; where the flag is itself
    // persisted in options.ini is our own bridging choice — treat this key as
    // "our tunable" until a decompile citation pins the write site.
    std::optional<bool> team_play;
};

// Reads and parses `<path>` (the install-root options.ini). A missing or
// unreadable file yields a default-constructed Options (all fields empty).
// Mirrors the original's line parse: split each line on '=', match the key
// case-insensitively, atoi the value.
Options load_options(const std::filesystem::path& path);

// Read-modify-write: updates only the keys present in `opts` (empty fields are
// left untouched), preserving every other line in the file VERBATIM (comments,
// unknown keys, original ordering/casing) so a hand-edited options.ini keeps
// its shape. Keys named in `opts` that already exist in the file are rewritten
// in place; keys named in `opts` that are absent are appended. A missing file
// is created fresh with just the given keys. Throws std::runtime_error if the
// file cannot be written (caller decides how to surface that).
void save_options(const std::filesystem::path& path, const Options& opts);

}  // namespace bomber::assets
