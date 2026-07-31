#pragma once

#include <array>
#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>

namespace bomber::assets {

// Locates the player's original Atomic Bomberman installation, or an empty path.
// The probe order is the contract and lives with the implementation.
//
// `exe_dir` is passed in rather than discovered here because this library is
// deliberately SDL-free (the caller has SDL_GetBasePath). Leaving it empty runs
// only the CWD-relative probes — which is what a shortcut with a different
// "Start in" used to silently reduce this to.
std::filesystem::path default_game_dir(const std::filesystem::path& exe_dir = {});

// The key-remap bindings (docs/re/results-and-options.md §2,
// dword_4645BC[10*set+action]). Two sets of 10 slots, though the remap UI
// (sub_407B9D) exposes only the first 6 — slots 6-9 have no UI and just
// round-trip.
//
// `scancode` is the original's RAW value (its own low-level space, byte_4A2BA0
// indexed) and is stored VERBATIM rather than remapped through SDL_Scancode, so
// a value this port never wrote survives a save/load cycle. -1 = absent.
struct KeyDef {
    static constexpr int kSets = 2;
    static constexpr int kActionsPerSet = 10;  // reader's array width (§2)
    std::array<std::array<int, kActionsPerSet>, kSets> scancode;

    KeyDef() {
        for (auto& set : scancode) set.fill(-1);
    }
};

// The install-root `options.ini` (read by sub_406238, written by sub_405DE3): a
// plain "key=value" text file. Every field is optional — an absent key leaves
// its optional empty so the caller keeps the binary's hardcoded default rather
// than guessing one.
//
// All 22 original keys are typed here, INCLUDING the ones this port never
// consumes (modem, netprotocol), so load/save round-trips them losslessly;
// save_options' unknown-line preservation is a backstop for genuinely foreign
// keys, not a crutch for ones we simply have not typed. Addresses, row numbers
// and the full clamp table: docs/re/results-and-options.md §3.
struct Options {
    std::optional<int> levelno;            // dword_464998; clamp < -1 -> -1 (RANDOM)
    std::optional<int> num_to_win_match;   // dword_464A7C; clamp < 1 -> 1
    std::optional<int> enclosement_depth;  // dword_464974; clamp < 0 -> 0
    // dword_464930: 0 low / 1 medium / 2 high, addressing VALUELST 190+idx. The
    // binary defaults to medium when no options.ini exists.
    std::optional<int> conveyor_speed;
    // dword_464964. Forces win_by_kills off while set (consumer-side).
    std::optional<bool> team_play;
    std::optional<bool> random_start;            // dword_464AE8
    std::optional<bool> stomped_bombs_detonate;  // dword_464940
    std::optional<bool> win_by_kills;            // dword_46497C
    std::optional<bool> goldman;                 // dword_4648BC
    std::optional<std::string> schemefilename;   // byte_4648C4[100], no clamp
    // dword_464948; clamp < 60 -> 60, and > 600 -> 600 EXCEPT the 1001
    // "unlimited" sentinel, which is never clamped.
    std::optional<int> playtime;
    std::optional<bool> assign_keyboards;      // dword_464968
    std::optional<bool> diseases_destroyable;  // dword_464990
    // dword_464928. CONSUMED by netplay: the caller copies it into
    // net::DropPolicy::revert_to_ai (peer drop -> AI handoff, ADR-0011).
    std::optional<bool> lost_net_revert_ai;
    std::optional<bool> disable_game_music;  // dword_4648C0
    std::optional<int> modemport;            // dword_464970, round-tripped only
    std::optional<int> modembaud;            // dword_46482C, round-tripped only
    std::optional<int> modemirq;             // dword_4648B8, round-tripped only
    std::optional<std::string> modemdial;    // no clamp, round-tripped only
    std::optional<int> netprotocol;          // dword_464828; clamp to [0, 3]
    // dword_464824 ("Use Enhanced Memory Model", displayed inverted). Inert in
    // THIS port, but NOT in the original: sub_42814B's cull keeps 1 clip per
    // voice group instead of the authored 2-8 (docs/re/sound-engine.md §3).
    std::optional<bool> smallmemory;
    // 20 `keydef=<set>,<action>,<scancode>` lines. The reader drops a whole line
    // whose set/action is out of range; the writer emits all 20.
    std::optional<KeyDef> keydef;
    // PORT-ONLY from here down — none of these exists in the 1997 binary, which
    // is a fixed 640x480 window. They persist through the SAME read-modify-write
    // file so they round-trip like every RE'd toggle above, and every absent key
    // falls back to the faithful behaviour: windowed, vsync on, cadence off, fps
    // hidden, and nearest-neighbour scaling (the original scales nothing at all,
    // so smoothing reproduces a modern display scaler, not the game).
    std::optional<bool> fullscreen;
    std::optional<bool> vsync;
    std::optional<bool> native_cadence;
    std::optional<bool> show_fps;
    std::optional<bool> soft_scaling;
};

// Reads the install-root options.ini; a missing or unreadable file yields an
// all-empty Options. Mirrors the original's parse — split on the first '=',
// match the key case-insensitively, atoi the value — and clamps per
// docs/re/results-and-options.md §3. An unrecognised key is ignored here and
// still preserved verbatim by save_options.
Options load_options(const std::filesystem::path& path);

// Read-modify-write: only the keys SET in `opts` change. Every other line
// survives verbatim — comments, unknown keys, original ordering and casing — so
// a hand-edited options.ini keeps its shape; named keys are rewritten in place
// or appended, and a missing file is created fresh. A set `opts.keydef` rewrites
// all 20 triples, skipping any whose scancode is still -1.
//
// Throws std::runtime_error if the file cannot be written.
void save_options(const std::filesystem::path& path, const Options& opts);

// The longest node name the original can hold: `sub_40C08C` reads into a 40-byte
// buffer with `fgets`, so 39 characters plus the NUL. (The Options edit field
// caps at 30 — that limit belongs to the UI, this one to the file.)
inline constexpr std::size_t kNodeNameMax = 39;

// The install-root `nodename.ini` — this machine's NET IDENTITY, shown in every
// lobby row. Deliberately NOT part of `Options`: a separate one-line file with
// its own reader (`sub_40C08C`) and writer (`sub_40C140`) in the binary.
//
// Both ends sanitise identically: strip '\r' and control bytes, truncate at
// kNodeNameMax. That is what stops a hand-edited file from injecting a newline,
// or a glyph the FON cannot draw, into the lobby roster.
//
// A missing or empty file yields an empty string. Picking the original's random
// default needs the message table, so it is the CALLER's job, not this loader's.
std::string load_node_name(const std::filesystem::path& path);

// Throws std::runtime_error if the file cannot be written.
void save_node_name(const std::filesystem::path& path, const std::string& name);

}  // namespace bomber::assets
