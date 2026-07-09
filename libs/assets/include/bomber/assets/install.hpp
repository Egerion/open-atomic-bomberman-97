#pragma once

#include <array>
#include <filesystem>
#include <optional>
#include <string>

namespace bomber::assets {

// Locates the player's original Atomic Bomberman installation. Probes, in
// order: the BOMBER_GAME_DIR environment variable, a gamedir.txt in the
// working directory, then the standard install locations. Returns an empty
// path when nothing is found.
std::filesystem::path default_game_dir();

// The key-remap bindings (docs/re/results-and-options.md §2, `keydef=<set>,
// <action>,<scancode>` triples, dword_4645BC[10*set+action]). Two keyboard
// sets, 10 action slots each even though the in-game remap UI (sub_407B9D)
// only exposes the first 6 (Up/Right/Down/Left/Action1/Action2, §2's action
// name order 1120..1125); slots 6-9 are write-only from options.ini (no UI)
// and simply round-trip. `scancode` is the RAW value written by the original
// (its own low-level scancode space, byte_4A2BA0 indexed) — we store it
// verbatim rather than remapping through SDL_Scancode, so a value this port
// did not write (e.g. hand-edited, or one of the UI-less slots 6-9) survives
// a save/load cycle unchanged. -1 = "absent" (key never appeared in the file).
struct KeyDef {
    static constexpr int kSets = 2;
    static constexpr int kActionsPerSet = 10;  // reader's array width (§2)
    std::array<std::array<int, kActionsPerSet>, kSets> scancode;

    KeyDef() {
        for (auto& set : scancode) set.fill(-1);
    }
};

// The install-root `options.ini` (parsed by sub_406238): a plain "key=value"
// text file the original reads at startup. Each field is optional — a key
// absent from the file leaves the corresponding optional empty so the caller
// keeps the binary's hardcoded default rather than guessing.
//
// docs/re/results-and-options.md §3 "The full options.ini key list" pins ALL
// 22 keys via the writer's (sub_405DE3) fixed fprintf order, positionally
// matching the reader's (sub_406238) stricmp chain. Every key gets a typed
// field here so load/save round-trip losslessly even for keys this port does
// not yet consume (e.g. modem/netprotocol) — save_options' unknown-line
// preservation is now a backstop for genuinely foreign keys, not a crutch for
// ones we simply haven't typed.
struct Options {
    // "levelno=" — dword_464998, the LEVEL & ROUNDS screen's committed stage
    // index. Clamp (post-read): < -1 -> -1 (RANDOM); >= getvalue(35) ->
    // getvalue(35)-1.
    std::optional<int> levelno;
    // "num_to_win_match=" — dword_464A7c, "how many wins to clinch the match"
    // (docs/re/results-and-options.md §1's match-clinch check). Clamp: < 1 ->
    // 1. Consumed here to seed GameApp::win_target_'s default (task item 5);
    // the LEVEL & ROUNDS screen's WINS row still overrides per-match.
    std::optional<int> num_to_win_match;
    // "enclosement_depth=" — dword_464974, Options row 7. Clamp: < 0 -> 0;
    // >= getvalue(28) -> getvalue(28)-1 (0..3: None/A Little/A Lot/All the way).
    std::optional<int> enclosement_depth;
    // "conveyor_speed=" — the Conveyor Speed game-option index (dword_464930):
    // 0 low, 1 medium, 2 high; addresses VALUELST 190/191/192 via
    // getvalue(190+idx). The binary defaults it to 1 (medium, pseudo.c 14652)
    // when no options.ini is present; this file overrides it. Clamped by the
    // consumer to [0, getvalue(189)-1].
    std::optional<int> conveyor_speed;
    // "team_play=" — the Team Play game-option toggle (0/1), dword_464964
    // (docs/re/results-and-options.md §3 row 0). Forces win_by_kills off when
    // set (consumer-side, present_options_screen).
    std::optional<bool> team_play;
    // "random_start=" — dword_464AE8, Options row 1 (§3). Normalized 0/1.
    std::optional<bool> random_start;
    // "stomped_bombs_detonate=" — dword_464940, Options row 4. Normalized 0/1.
    std::optional<bool> stomped_bombs_detonate;
    // "win_by_kills=" — dword_46497C, Options row 5. Normalized 0/1; forced
    // off whenever Team Play is on (consumer-side).
    std::optional<bool> win_by_kills;
    // "goldman=" — dword_4648BC, Options row 6 ("Gold Bomberman"). Normalized
    // 0/1; toggling it also resets the pending roulette winner (consumer-side,
    // presentation-only — no roulette wheel in this port yet).
    std::optional<bool> goldman;
    // "schemefilename=" — byte_4648C4[100], Options row 8. Plain strcpy_, no
    // numeric clamp.
    std::optional<std::string> schemefilename;
    // "playtime=" — dword_464948, Options row 9. Clamp: < 60 -> 60; != 1001
    // (the "unlimited" sentinel) && > 600 -> 600.
    std::optional<int> playtime;
    // "assign_keyboards=" — dword_464968, Options row 10. Normalized 0/1.
    std::optional<bool> assign_keyboards;
    // "diseases_destroyable=" — dword_464990, Options row 11. No clamp
    // observed.
    std::optional<bool> diseases_destroyable;
    // "lost_net_revert_ai=" — dword_464928, Options row 12. No clamp observed.
    std::optional<bool> lost_net_revert_ai;
    // "disable_game_music=" — dword_4648C0, Options row 13. Normalized 0/1.
    std::optional<bool> disable_game_music;
    // "modemport=" — dword_464970, part of Options row 14's nested modem
    // sub-screen. No clamp observed. Not consumed by this port (no modem/net
    // play); round-tripped typed for completeness per §3.
    std::optional<int> modemport;
    // "modembaud=" — dword_46482C. See modemport.
    std::optional<int> modembaud;
    // "modemirq=" — dword_4648B8. See modemport.
    std::optional<int> modemirq;
    // "modemdial=" — string buffer. Plain strcpy_, no numeric clamp.
    std::optional<std::string> modemdial;
    // "netprotocol=" — dword_464828, Options row 16. Clamp: < 0 -> 0; > 3 -> 3.
    // Not consumed by this port (no network play); round-tripped typed.
    std::optional<int> netprotocol;
    // "smallmemory=" — dword_464824, Options row 17 ("Use Enhanced Memory
    // Model", inverted display). Normalized 0/1. Not consumed (no memory-model
    // concept in a modern build); round-tripped typed.
    std::optional<bool> smallmemory;
    // "keydef=<set>,<action>,<scancode>" x20 — dword_4645BC[10*set+action],
    // Options row 15 ("Define keyboard layouts", sub_407B9D, §2). The reader
    // clamps set in [0,1] / action in [0,9], dropping the whole line
    // otherwise; the writer always emits all 20 triples. See KeyDef's doc.
    std::optional<KeyDef> keydef;
    // "fullscreen=" — PORT-ONLY key, NOT one of the original's confirmed 22
    // options.ini keys above (the 1997 binary is a fixed 640x480 window with
    // no fullscreen/resize concept). A deliberate port enhancement
    // (GameApp's Alt+Enter/F11 toggle, game_app.cpp/game_app.hpp), persisted
    // through this SAME read-modify-write file so it round-trips like every
    // RE'd toggle above. Normalized 0/1; absent key -> windowed (matching the
    // original's only mode).
    std::optional<bool> fullscreen;
};

// Reads and parses `<path>` (the install-root options.ini). A missing or
// unreadable file yields a default-constructed Options (all fields empty).
// Mirrors the original's line parse: split each line on '=', match the key
// case-insensitively, atoi the value. Every key from docs/re/results-and-
// options.md §3's full table is recognized and clamped per that doc; a
// `keydef=` line updates ONE (set,action) triple in the (lazily default-
// constructed) KeyDef, out-of-range set/action silently dropped (matching the
// original's "drop the whole line" clamp). Any OTHER key is intentionally
// ignored here (still preserved verbatim by save_options' read-modify-write).
Options load_options(const std::filesystem::path& path);

// Read-modify-write: updates only the keys present in `opts` (empty fields are
// left untouched), preserving every other line in the file VERBATIM (comments,
// unknown keys, original ordering/casing) so a hand-edited options.ini keeps
// its shape. Keys named in `opts` that already exist in the file are rewritten
// in place; keys named in `opts` that are absent are appended. `keydef=` is
// special: a set `opts.keydef` rewrites/appends all 20 `keydef=<set>,<action>,
// <scancode>` lines (skipping any (set,action) whose scancode is -1/absent).
// A missing file is created fresh with just the given keys. Throws
// std::runtime_error if the file cannot be written (caller decides how to
// surface that).
void save_options(const std::filesystem::path& path, const Options& opts);

}  // namespace bomber::assets
