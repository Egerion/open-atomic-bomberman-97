#include "app_settings.hpp"

#include <cstdlib>
#include <exception>

#include "bomber/assets/install.hpp"
#include "bomber/game_util/frontend_util.hpp"  // reload_scheme
#include "bomber/game_util/log.hpp"
#include "bomber/game_util/scale_filter.hpp"
#include "bomber/input/dos_scancode.hpp"
#include "bomber/render/sprites.hpp"  // set_scale_filter

namespace bomber::game {

namespace fs = std::filesystem;

namespace {

// The two GAMEPLAY options.ini values a capture pins. Both are deliberate scenario
// parameters of the scripted demo match, in the same class as its fixed seed and
// LCG literals — chosen for what they make the match cover, not read from whatever
// the player last saved. They are the values tests/visual/shots.txt was captured
// under, so the pin makes those frames reproducible rather than replacing them.
//
// Random Start ON is also the ORIGINAL's default (VALUELST id 40 = 1), so a capture
// plays the spawn assignment a fresh install plays. Conveyor Speed 2 (High, 450 —
// ids 190-192) is the setting under which the scripted match actually exercises
// level 10's conveyor loop: the first bomb rides the belt across the board, so the
// belt draw and the belt-carried bomb are covered rather than dead code.
constexpr bool kCaptureRandomStart = true;
constexpr int kCaptureConveyorSpeed = 2;

// Absent-key defaults for the sim-consumed toggles come from VALUELST, mirroring
// sub_41095A's init order: dword_464AE8 = getvalue(40) (randomize starting
// positions), dword_464940 = getvalue(46) (a wall segment closing on a bomb detonates
// it), dword_464990 = getvalue(120) (diseases blow up like other powerups).
// options.ini then overrides; the shipped file sets all three to 1 too.
// docs/re/facts.md "Options toggles".
//
// Conveyor Speed, Random Start, Team Play and playtime are all PINNED on a capture,
// each for the same reason: they feed the sim or the HUD, so reading them off a
// mutable options.ini made the visual golden depend on a setting the player can
// change between two runs of the same build. Not theoretical — random_start is THE
// reason the harness went red once (the pins were taken under random_start=1, a later
// session saved 0, and the same source then rendered five different frames on the
// same machine), conveyor_speed was the other half of it, and a playtime of 180 s
// silently re-hashed every pinned shot on 2026-07-12.
void apply_gameplay_options(const SettingsSlots& s, const assets::Options& in, bool capture) {
    s.conveyor_speed_index =
        capture ? std::optional<int>{kCaptureConveyorSpeed} : in.conveyor_speed;
    // Absent team_play key means OFF, the confirmed default (docs/re/
    // setup-screens.md: "Team mode ... OFF by default").
    //
    // team_play and playtime are pinned on `capture` — the SAME predicate as
    // every other pinned key. They used to sit on opts.demo, which held only
    // because all four capture entry points happen to set demo too; capture is
    // a superset of demo (is_capture_run), so this is strictly stronger, and it
    // closes failure mode #5 of the class tests/visual/README.md documents.
    s.team_play = !capture && in.team_play.value_or(false);
    s.options.team_play = s.team_play;
    s.options.random_start =
        capture ? kCaptureRandomStart : in.random_start.value_or(s.values.at_or(40, 1) != 0);
    s.options.conveyor_speed_index = s.conveyor_speed_index.value_or(1);
    s.options.playtime_seconds = capture ? 150 : in.playtime.value_or(150);
    s.options.stomped_bombs_detonate =
        in.stomped_bombs_detonate.value_or(s.values.at_or(46, 1) != 0);
    s.options.diseases_destroyable = in.diseases_destroyable.value_or(s.values.at_or(120, 1) != 0);
    s.options.win_by_kills = in.win_by_kills.value_or(false);
    s.options.goldman = in.goldman.value_or(false);
    s.options.enclosement_depth = in.enclosement_depth.value_or(1);
    s.options.disable_game_music = in.disable_game_music.value_or(false);
    // Rows 10/12/17 — real options.ini keys with no gameplay consumer
    // (docs/re/results-and-options.md §3); round-tripped like every other toggle.
    s.options.assign_keyboards = in.assign_keyboards.value_or(false);
    s.options.lost_net_revert_ai = in.lost_net_revert_ai.value_or(false);
    s.options.small_memory = in.smallmemory.value_or(false);
    // Row 14's four modem fields are display-only (getstring(264)); the defaults are
    // the shipped install's values.
    s.options.modemport = in.modemport.value_or(2);
    s.options.modemirq = in.modemirq.value_or(3);
    s.options.modembaud = in.modembaud.value_or(19200);
    s.options.modemdial = in.modemdial.value_or("555-1212");
    // Seeds win_target_'s default: reset_match_scores() falls back to it when
    // getvalue(310) is absent, and the WINS row still overrides per-match on top.
    s.num_to_win_match = in.num_to_win_match;
}

// The PORT-ONLY keys (install.hpp) — none of them one of the original's confirmed
// 22, because the 1997 binary is a fixed 640x480 window with no fullscreen, no
// vsync toggle and no scaler at all. Faithful defaults: vsync ON (so uncap, its
// inverse, is off), native cadence off, fps readout hidden.
//
// A capture pins all four rather than honouring the file. show_fps draws an overlay
// over every captured frame; native_cadence drives the sim off the wall clock,
// non-deterministic by construction; vsync changes pacing a capture has no reason
// to inherit; and soft_scaling reaches furthest of the four — it swaps nearest for
// linear sampling on EVERY classic texture, so a saved soft_scaling=1 would re-hash
// all five tests/visual pins at once. scale_filter_for() is the pin, and it is
// unit-tested (tests/game/test_scale_filter.cpp) rather than trusted as a ternary.
void apply_port_options(const SettingsSlots& s, const assets::Options& in, bool capture) {
    s.fullscreen = in.fullscreen.value_or(false);  // absent -> windowed, the original's only mode
    s.uncap_fps = capture ? false : !in.vsync.value_or(true);
    s.native_cadence = capture ? false : in.native_cadence.value_or(false);
    s.show_fps = capture ? false : in.show_fps.value_or(false);
    s.soft_scaling = capture ? false : in.soft_scaling.value_or(false);
    // Set BEFORE the asset load uploads anything, so the boot textures are
    // created with the right sampling mode instead of being re-stamped.
    set_scale_filter(scale_filter_for(s.soft_scaling, capture));
}

// "keydef=" holds the ORIGINAL's DOS/AT set-1 scancodes, so a shared install stays
// interchangeable with BM95.EXE; translate into SDL_Scancode space for the live
// mapper. A triple with scancode == -1 (never written) keeps that action's
// compiled-in default; DOS 0 or an unmappable code binds SDL_SCANCODE_UNKNOWN, i.e.
// unbound, like the original.
void apply_key_bindings(const SettingsSlots& s, const assets::Options& in) {
    if (!in.keydef) return;
    for (int set = 0; set < assets::KeyDef::kSets; ++set) {
        KeySet ks = s.keyboard.key_set(set);
        for (int action = 0; action < kKeyActionCount; ++action) {
            const int sc = in.keydef->scancode[set][action];
            if (sc >= 0) ks.scancode[action] = sdl_scancode_from_dos(sc);
        }
        s.keyboard.set_key_set(set, ks);
    }
}

// Row 8: schemefilename= DRIVES the loaded scheme — the original re-parses
// byte_4648C4 at every Play-flow entry (sub_410F81 -> sub_4046CC -> sub_403EEE), so
// the key was never display-only. An explicit --scheme wins, and a capture stays
// pinned to BASIC.SCH for the same reproducibility reason the playtime pin cites
// — on `capture`, the predicate that exists for exactly this, not on opts.demo,
// which merely coincided with it (see apply_gameplay_options).
void apply_scheme_choice(const SettingsSlots& s, const assets::Options& in,
                         const fs::path& scheme_path, bool capture) {
    s.options.scheme_filename = in.schemefilename.value_or(scheme_path.filename().string());
    if (capture || !s.opts.scheme.empty()) return;
    if (!in.schemefilename || in.schemefilename->empty()) return;
    if (reload_scheme(s.scheme, s.opts.game_dir, *in.schemefilename)) return;
    log_warn("schemefilename '%s' not found; keeping %s", in.schemefilename->c_str(),
             scheme_path.filename().string().c_str());
}

// The node name is NOT one of options.ini's 22 keys — it has its own install-root
// file, read the way sub_40C08C reads it from the boot init sub_40C74C
// (docs/re/results-and-options.md §3 row 2). An absent or empty file leaves it blank
// until seed_default_node_name() draws the random fallback.
void load_node_identity(const SettingsSlots& s) {
    s.node_name_path = s.opts.game_dir / "nodename.ini";
    s.node_name_loaded = assets::load_node_name(s.node_name_path);
    s.options.node_name = s.node_name_loaded;
}

void fill_gameplay_keys(assets::Options& out, const SettingsSlots& s) {
    out.team_play = s.options.team_play;
    out.random_start = s.options.random_start;
    out.conveyor_speed = s.options.conveyor_speed_index;
    out.stomped_bombs_detonate = s.options.stomped_bombs_detonate;
    out.win_by_kills = s.options.win_by_kills;
    out.goldman = s.options.goldman;
    out.enclosement_depth = s.options.enclosement_depth;
    out.playtime = s.options.playtime_seconds;
    out.assign_keyboards = s.options.assign_keyboards;
    out.lost_net_revert_ai = s.options.lost_net_revert_ai;
    out.smallmemory = s.options.small_memory;
    out.diseases_destroyable = s.options.diseases_destroyable;
    out.disable_game_music = s.options.disable_game_music;
    // Row 8 round-trips whatever is shown (the picker stores it extension-stripped
    // and uppercased; a hand-edited value survives as-is).
    if (!s.options.scheme_filename.empty()) out.schemefilename = s.options.scheme_filename;
}

void fill_port_keys(assets::Options& out, const SettingsSlots& s) {
    out.fullscreen = s.fullscreen;
    out.vsync = !s.uncap_fps;  // vsync is the inverse of the internal uncap flag
    out.native_cadence = s.native_cadence;
    out.show_fps = s.show_fps;
    out.soft_scaling = s.soft_scaling;
    // Always write the live bindings so a rebind survives a restart, translated back
    // into the ORIGINAL's DOS/AT scancode space — before 2026-07-13 the port wrote
    // raw SDL_Scancode values, which BM95.EXE misreads against the same install. An
    // SDL key with no DOS equivalent writes 0 = unbound. Slots 6-9 per set have no
    // in-game UI and stay at -1, which save_options skips, so a pre-existing keydef=
    // line for them survives the read-modify-write.
    assets::KeyDef kd;
    for (int set = 0; set < assets::KeyDef::kSets; ++set) {
        const KeySet& ks = s.keyboard.key_set(set);
        for (int action = 0; action < kKeyActionCount; ++action)
            kd.scancode[set][action] = dos_scancode_from_sdl(ks.scancode[action]);
    }
    out.keydef = kd;
}

// The original rewrites nodename.ini unconditionally; skipping a write whose bytes
// would be identical is the same outcome without touching the install every launch.
// A CAPTURE never writes at all — same class of pin as the reads above: hermetic
// runs must not touch the install (tests/visual/README.md).
void flush_node_name(const SettingsSlots& s) {
    if (is_capture_run(s.opts) || s.node_name_path.empty()) return;
    if (s.options.node_name.empty() || s.options.node_name == s.node_name_loaded) return;
    try {
        assets::save_node_name(s.node_name_path, s.options.node_name);
        s.node_name_loaded = s.options.node_name;
    } catch (const std::exception& e) {
        log_warn("nodename.ini save failed: %s", e.what());
    }
}

}  // namespace

bool load_settings(const SettingsSlots& s, const fs::path& scheme_path) {
    const fs::path& game = s.opts.game_dir;
    try {
        s.scheme = assets::sch::load(scheme_path);
        s.values = assets::res::load_values(game / "DATA" / "RES" / "VALUELST.RES");
        if (const char* env = std::getenv("BOMBER_GAME_SECONDS"); env && *env)
            s.values.values[100] = std::atoi(env);  // testing hook
        // The install-root options.ini (sub_406238) — all 22 keys, docs/re/
        // results-and-options.md §3. Read ONCE here; every Options-screen edit
        // thereafter mutates the in-memory snapshot.
        s.options_path = game / "options.ini";
        const assets::Options loaded = assets::load_options(s.options_path);
        const bool capture = is_capture_run(s.opts);
        apply_gameplay_options(s, loaded, capture);
        load_node_identity(s);
        apply_scheme_choice(s, loaded, scheme_path, capture);
        apply_key_bindings(s, loaded);
        apply_port_options(s, loaded, capture);
    } catch (const std::exception& e) {
        log_warn("%s", e.what());
        return false;
    }
    return true;
}

void seed_default_node_name(const SettingsSlots& s) {
    if (!s.options.node_name.empty()) return;
    // A capture must not draw from a pinned LCG nor rewrite the install; it never
    // reaches the Options screen or the lobby either. Gated on the capture
    // predicate its own comment always described, not the demo coincidence.
    if (is_capture_run(s.opts)) return;
    // getvalue(47) = 49 (MESSAGES ids 500..548). Presentation-only, so it runs on the
    // shared front-end LCG and never State::rng (ADR-0004) — one draw, once per
    // install, since flush_settings then makes the name permanent as sub_40C140 does.
    const int count = static_cast<int>(s.values.at_or(47, 49));
    s.setup_lcg = s.setup_lcg * 1664525u + 1013904223u;
    const int pick =
        count > 0 ? static_cast<int>((s.setup_lcg >> 16) % static_cast<unsigned>(count)) : 0;
    s.options.node_name = s.assets.getstring(500 + pick, "Bomberman");
}

void flush_settings(const SettingsSlots& s) {
    // Ahead of the dirty gate below, which governs options.ini alone.
    flush_node_name(s);
    // Guarded so a run that never touched an Options row, or one that never
    // resolved a game_dir (boot already bailed), does nothing.
    if (!s.options_dirty || s.options_path.empty()) return;
    assets::Options to_write;
    fill_gameplay_keys(to_write, s);
    fill_port_keys(to_write, s);
    try {
        assets::save_options(s.options_path, to_write);
        s.options_dirty = false;
    } catch (const std::exception& e) {
        log_warn("options.ini save failed: %s", e.what());
    }
}

}  // namespace bomber::game
