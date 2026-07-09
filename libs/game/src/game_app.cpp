#include "bomber/game/game_app.hpp"

#include <algorithm>  // std::max_element
#include <cctype>     // std::isalnum (present_editor's filename sanitizer)
#include <chrono>     // random_boot_seed
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <iterator>  // std::size
#include <random>    // random_boot_seed
#include <string>

#include "bomber/assets/install.hpp"
#include "bomber/game/anim_pace.hpp"
#include "bomber/game/bmscreen.hpp"
#include "bomber/game/dialog_chrome.hpp"
#include "bomber/game/sprites.hpp"
#include "bomber/match/match_factory.hpp"

namespace bomber::game {

namespace fs = std::filesystem;

// NOTE on the `renderer_->`/`screen_->` NOLINTs below (bugprone-unchecked-
// optional-access): both are std::optional<T> members populated exactly once,
// unconditionally, at the end of a successful init() (this file, below) —
// every method that dereferences them runs only from GameApp::run(), which
// the app mains call strictly after a successful init(). clang-tidy's flow
// analysis is per-function and can't see that cross-method invariant; an
// `if (renderer_)` guard at each of the ~12 call sites (scattered across the
// per-frame render/event loops) would be pure churn around a precondition
// that's already true by construction, so these are deliberate one-offs
// rather than fixed.

namespace {

// The DialogRect/dialog_rect*/draw_dialog_chrome/draw_dialog_button
// primitives moved to bomber/game/dialog_chrome.hpp (2026-07-09) so the
// scheme editor's own sub_41456C/sub_42E938 dialogs (docs/re/
// results-and-options.md #5, "exact dialog chrome") can reuse the SAME
// pinned facts instead of re-deriving them — see that header for the full
// sub_43C734/sub_432298 provenance comments.
// A fresh per-process seed for the front end's presentation-only LCGs
// (setup_lcg_, attract_lcg_, goldman_lcg_, next_seed_ — GameApp::init() below
// assigns one call's result to each). The original seeds its single C
// rand() from the wall clock exactly once at boot — `time_(); srand_();`
// twice in a row inside the init routine sub_41095A (pseudo.c 14610-14611
// and, after loading the boot dialogs/config, again at 14639-14640) — so
// EVERY "random" pick downstream of it (the glue-screen backdrop, the
// LEVEL & ROUNDS RANDOM level, the per-match brick fill, the attract-mode
// roster/stage, the Goldman wheel draws) varies from one launch to the next
// (docs/re/facts.md "Per-match brick fill" already cites this same
// `time_()`/`srand_()` pair). Our port previously left every one of these
// LCGs at a fixed literal seed, so each replayed the exact same sequence on
// every process start — most visibly, picking RANDOM on the LEVEL & ROUNDS
// screen always resolved to the same map on a fresh launch (`next_seed_`
// feeds `match::pick_stage`). This mixes `std::random_device` (real OS
// entropy on every platform we ship) with the high-resolution clock so the
// result still varies even on a `random_device` implementation that is
// itself deterministic (a known quirk of some older toolchains) — matching
// the original's per-boot reseed while staying entirely presentation-side:
// this seeds LCGs that only ever touch cosmetic/setup-time picks, never
// `bomber::sim::State::rng` (ADR-0003 untouched).
std::uint32_t random_boot_seed() {
    static std::random_device rd;
    auto clock_bits = static_cast<std::uint32_t>(
        std::chrono::high_resolution_clock::now().time_since_epoch().count());
    std::uint32_t seed = static_cast<std::uint32_t>(rd()) ^ clock_bits;
    return seed != 0 ? seed : 1u;  // an LCG's own step never produces 0 from a 0 seed here,
                                    // but avoid a literal 0 seed on principle
}

// The boot LOADING dialog — PINNED 2026-07-09 (docs/re/frontend-flow.md "The
// boot LOADING dialog" + "The percent-bar dialog, sub_412E33"). This is NOT
// the IPLOGO/HSLOGO/TITLE screen chain (sub_42B060, a separate later step):
// it is a small modal progress window (sub_43C734, above) that the entry
// point sub_42BE22 shows TWICE before sub_42B060 ever runs — once for
// "Loading data..." (getstring(201), sub_41D695's MASTER.ALI read) and once
// for "Loading sound..." (getstring(200), sub_4287B9's SOUNDLST group
// preload) — both driven by the shared percent-bar primitive sub_412E33 (a
// two-tone bar plus a "%d" readout, "Completion" caption). It is
// programmatically drawn (box + bar + text), NOT a PCX asset — no
// LOADING*.PCX exists anywhere in the install or the decompile.
//
// Pinned geometry (sub_412E33, pseudo.c 16157-16211): window
// y=200 (CONFIRMED literal, not centered — contrast the confirm dialog
// below), height=8*fontheight, width=360 (x auto-centered, see the chrome
// comment above); caption "Completion" centered at y=1.5*fontheight in
// white (byte_49D38F); "%d" readout centered at y=3.5*fontheight in YELLOW
// (byte_49D37A, RGB (255,255,90) — corrects an earlier pass's assumption
// that it shared the caption's white); two-tone bar at x=31, y=5.5*
// fontheight+1, height=fontheight-1, width 300 split at 3*pct, filled
// portion byte_49A624 mid-grey (168,168,164), unfilled black.
//
// Our AssetStore::load() has no per-file progress callback (a monolithic
// try-block of ANI/PCX loads), and on modern hardware the whole thing is
// sub-second — so a live animated percent would be fake motion. Faithful
// simplification (documented per CLAUDE.md's RE workflow, UNCHANGED by this
// pass): flash the SAME two captions in the SAME order, each fully
// "complete" (bar full, 100%) for one presented frame, matching the
// original's caption-then-bar shape without inventing progress data we
// don't have.
//
// Font: FONT6 is CONFIRMED ready before BOTH flashes (docs/re/
// frontend-flow.md "FONT6 timing" — sub_414DF4 pins it via sub_431E9C(6)
// before sub_41095A ever calls the loading dialogs), correcting the earlier
// port comment that assumed a readiness gap at the first flash. GameApp::init
// now loads FONT6 standalone ahead of this call, so both flashes render with
// the real glyph textures; SDL_RenderDebugText is no longer used here.
void draw_boot_loading_dialog(SDL_Renderer* ren, const FontTextures& font, const char* caption) {
    SDL_SetRenderDrawColor(ren, 0, 0, 0, 255);
    SDL_RenderClear(ren);

    const float h = static_cast<float>(font.loaded() ? font.line_height() : 12);
    const DialogRect win = dialog_rect(200.0f, 8.0f * h, 360.0f);
    draw_dialog_chrome(ren, win);

    // Caption "Completion" — white (byte_49D38F), horizontally centered,
    // y = 1.5*fontheight (window-relative).
    std::string cap_str = caption;
    float cap_w = font.loaded() ? static_cast<float>(font.measure(cap_str)) : 0.0f;
    font.draw(ren, cap_str, win.x + (win.w - cap_w) / 2, win.y + 1.5f * h, 255, 255, 255);

    // The bar sits at a fixed "complete" 100% (the documented simplification
    // above) — the filled segment spans the full 300 px track, so the
    // never-drawn unfilled segment is omitted rather than drawn zero-width.
    SDL_FRect bar{win.x + 31.0f, win.y + 5.5f * h + 1.0f, 300.0f, h - 1.0f};
    SDL_SetRenderDrawColor(ren, 168, 168, 164, 255);  // byte_49A624
    SDL_RenderFillRect(ren, &bar);

    // "%d" readout (100, matching the always-complete bar) — yellow
    // (byte_49D37A), y = 3.5*fontheight, horizontally centered.
    std::string pct_str = "100";
    float pct_w = font.loaded() ? static_cast<float>(font.measure(pct_str)) : 0.0f;
    font.draw(ren, pct_str, win.x + (win.w - pct_w) / 2, win.y + 3.5f * h, 255, 255, 90);

    SDL_RenderPresent(ren);
}

}  // namespace

bool GameApp::init() {
    // Reseed the front end's presentation-only LCGs from real per-process
    // entropy, mirroring sub_41095A's boot-time `time_(); srand_();` (see
    // random_boot_seed()'s comment above) — done first, before anything that
    // could read one of them, exactly like the original reseeds before its
    // own config/subsystem init runs. Fixes RANDOM level selection (and the
    // per-match brick fill/attract roster/Goldman wheel) always replaying the
    // same pick on a fresh launch.
    setup_lcg_ = random_boot_seed();
    attract_lcg_ = random_boot_seed();
    goldman_lcg_ = random_boot_seed();
    next_seed_ = random_boot_seed();

    fs::path game = !opts_.game_dir.empty() ? opts_.game_dir : assets::default_game_dir();
    if (game.empty() || !fs::is_directory(game / "DATA")) {
        std::fprintf(stderr,
                     "usage: bomber_game [game_dir] [scheme.sch]\n"
                     "(or set BOMBER_GAME_DIR / gamedir.txt)\n");
        return false;
    }
    opts_.game_dir = game;
    fs::path scheme_path =
        !opts_.scheme.empty() ? opts_.scheme : game / "DATA" / "SCHEMES" / "BASIC.SCH";

    try {
        scheme_ = assets::sch::load(scheme_path);
        values_ = assets::res::load_values(game / "DATA" / "RES" / "VALUELST.RES");
        if (const char* env = std::getenv("BOMBER_GAME_SECONDS"); env && *env)
            values_.values[100] = std::atoi(env);  // testing hook
        // The install-root options.ini (sub_406238) — ALL 22 keys, docs/re/
        // results-and-options.md §3. Loaded ONCE here into options_; every
        // Options-screen edit thereafter mutates options_ in memory only
        // (write-on-exit, §2 — flush_options() is the sole writer).
        options_path_ = game / "options.ini";
        assets::Options loaded_opts = assets::load_options(options_path_);
        conveyor_speed_index_ = loaded_opts.conveyor_speed;
        // Team Play ("team_play="): absent key ⇒ OFF, matching the confirmed
        // team-mode default (docs/re/setup-screens.md: "Team mode is toggled on
        // the OPTIONS game-type screen, OFF by default").
        team_play_ = loaded_opts.team_play.value_or(false);
        options_.team_play = team_play_;
        // Absent-key defaults for the sim-consumed toggles come from VALUELST,
        // mirroring sub_41095A's init order exactly: dword_464AE8=getvalue(40)
        // ("do we randomize player starting positions?" = 1), dword_464940 =
        // getvalue(46) ("wall segment closes in on a bomb ... 1 - detonate" =
        // 1), dword_464990 = getvalue(120) ("can diseases be blown up like
        // all other powerups?" = 1) — and options.ini then overrides (the
        // shipped file sets all three to 1 as well). docs/re/facts.md
        // "Options toggles: stomped_bombs_detonate / diseases_destroyable".
        options_.random_start =
            loaded_opts.random_start.value_or(values_.at_or(40, 1) != 0);
        options_.conveyor_speed_index = conveyor_speed_index_.value_or(1);
        options_.stomped_bombs_detonate =
            loaded_opts.stomped_bombs_detonate.value_or(values_.at_or(46, 1) != 0);
        options_.win_by_kills = loaded_opts.win_by_kills.value_or(false);
        options_.goldman = loaded_opts.goldman.value_or(false);
        options_.enclosement_depth = loaded_opts.enclosement_depth.value_or(1);
        options_.playtime_seconds = loaded_opts.playtime.value_or(150);
        options_.diseases_destroyable =
            loaded_opts.diseases_destroyable.value_or(values_.at_or(120, 1) != 0);
        options_.disable_game_music = loaded_opts.disable_game_music.value_or(false);
        // "keydef=" -> KeyboardMapper's two live key-sets (docs/re/results-and-
        // options.md §2). A KeyDef triple with scancode == -1 (never written)
        // keeps that action's compiled-in default (input.hpp's
        // default_key_set) rather than binding to scancode 0.
        if (loaded_opts.keydef) {
            for (int set = 0; set < assets::KeyDef::kSets; ++set) {
                KeySet ks = keyboard_.key_set(set);
                for (int action = 0; action < kKeyActionCount; ++action) {
                    int sc = loaded_opts.keydef->scancode[set][action];
                    if (sc >= 0) ks.scancode[action] = sc;
                }
                keyboard_.set_key_set(set, ks);
            }
        }
        // "num_to_win_match=" seeds win_target_'s default (task item 5, §5):
        // reset_match_scores() falls back to this when getvalue(310) is
        // absent/invalid, and the LEVEL & ROUNDS screen's WINS row still
        // overrides per-match on top of whichever default won.
        num_to_win_match_ = loaded_opts.num_to_win_match;
        // "fullscreen=" — PORT-ONLY key, NOT one of the original's confirmed
        // 22 options.ini keys (results-and-options.md §3): the 1997 binary is
        // a fixed 640x480 window with no resize/fullscreen concept at all. A
        // deliberate port enhancement (task: "widescreen/fullscreen support"),
        // persisted through the SAME read-modify-write options.ini machinery
        // so it round-trips like every other toggle; absent key -> windowed,
        // matching the original's only mode.
        fullscreen_ = loaded_opts.fullscreen.value_or(false);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "%s\n", e.what());
        return false;
    }

    video_.emplace();
    if (!video_->ok()) {
        std::fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return false;
    }
    // PORT ENHANCEMENT (task: "widescreen/fullscreen support", not an RE
    // fidelity item — the original is a hardcoded 640x480 window, no resize
    // or fullscreen path exists in the binary at all). SDL_WINDOW_RESIZABLE
    // makes the OS maximize button/drag-resize work; the sim's logical
    // resolution stays exactly 640x480 (kScreenW/kScreenH, untouched) via
    // SDL_LOGICAL_PRESENTATION_LETTERBOX below, which scales+letterboxes to
    // whatever window/monitor size the player picks without stretching. Any
    // fullscreen toggle (Alt+Enter/F11, sdl_event_filter below) just resizes
    // the OS window/output — it never touches kScreenW/kScreenH or the sim.
    SDL_Window* win = nullptr;
    SDL_Renderer* ren = nullptr;
    if (!SDL_CreateWindowAndRenderer("Open Bomberman", kScreenW * 2, kScreenH * 2,
                                     SDL_WINDOW_RESIZABLE, &win, &ren)) {
        std::fprintf(stderr, "SDL_CreateWindowAndRenderer: %s\n", SDL_GetError());
        return false;
    }
    window_.reset(win);
    sdl_renderer_.reset(ren);
    SDL_SetRenderLogicalPresentation(ren, kScreenW, kScreenH,
                                     SDL_LOGICAL_PRESENTATION_LETTERBOX);
    if (fullscreen_) SDL_SetWindowFullscreen(win, true);  // restore last session's choice
    // Global Alt+Enter/F11 fullscreen toggle (task item 1): an SDL_EventFilter
    // runs synchronously inside SDL_PumpEvents (before the event ever reaches
    // any of this file's many per-screen SDL_PollEvent loops), so it works
    // everywhere at once and swallows the keypress (returns false) rather
    // than leaking it into a screen's "any key" handling (present_screen's
    // advance-on-any-key, the editor's text input, etc). Installed once here,
    // for the window's whole lifetime.
    SDL_SetEventFilter(&GameApp::sdl_event_filter, this);
    // Vsync the present loop (best-effort; a no-op on a driver that can't,
    // e.g. the dummy/offscreen video driver some CI runs use). The original
    // is a DirectDraw flip loop with no getvalue()-backed frame-rate id
    // anywhere in VALUELST, so its own pacing is whatever the display's
    // vertical blank gave it (front-end loops like sub_42B9CE's menu poll
    // once per iteration with no separate throttle — the flip IS the
    // throttle). Every front-end screen that free-runs its own cosmetic
    // frame counter once per render iteration (docs/re/frontend-flow.md
    // "Cursor anchor" — present_menu's `++frame` driving the animated
    // bomb-trigger cursor is the specific case that surfaced this, but the
    // same pattern recurs in the Goldman wheel spin, boot logo timing, and
    // attract idle) was, pre-fix, advancing at this loop's uncapped
    // `SDL_Delay(2)` rate (~500 Hz) instead of the original's
    // vsync-limited rate (~60-75 Hz) — a visibly-too-fast flicker with no
    // faithful fixed millisecond constant to substitute, since the
    // original's own pacing IS "one step per displayed frame". Syncing our
    // present to the display's refresh is the faithful fix: it makes "one
    // step per displayed frame" true here too, the same relationship the
    // original had, without guessing a magic delay.
    SDL_SetRenderVSync(ren, 1);

    // FONT6, loaded standalone BEFORE the boot LOADING dialogs — matching the
    // real init order (docs/re/frontend-flow.md "FONT6 timing", CONFIRMED):
    // sub_41095A calls sub_414DF4 (which pins FONT6 via sub_431E9C(6) as its
    // very last step) BEFORE it calls sub_41D695/sub_42896E, the two dialogs.
    // FONT6.FON is install-root and independent of assets_.load()'s ANI/PCX
    // work, so hoisting just this one file out is cheap and safe. A missing
    // font leaves front_font_ empty; draw_dialog_chrome's text calls then
    // silently no-op (FontTextures::draw on an unbuilt font), same fallback
    // behaviour the .BM viewer already relies on.
    assets_.load_frontend_font(game);
    front_font_.build(ren, assets_.frontend_font());

    // The boot LOADING dialog (sub_42BE22 -> sub_41095A -> sub_41D695/
    // sub_42896E, PINNED — see draw_boot_loading_dialog's comment): the ORIGINAL
    // shows "Loading data..." (getstring 201) before its asset preload and
    // "Loading sound..." (getstring 200) before its sound preload, BOTH before
    // the IPLOGO/HSLOGO/TITLE chain (sub_42B060) ever runs. MESSAGES.TXT is not
    // loaded yet at this first flash (it lives inside assets_.load()), so it
    // uses the literal fallback text; the second flash below reads the real
    // string once it's available. Both flashes now render with FONT6 (above),
    // matching the trace — there is no font-readiness gap at the FIRST flash.
    draw_boot_loading_dialog(ren, front_font_, "Loading data...");

    if (!assets_.load(ren, game)) return false;
    seqs_.resolve(assets_);

    // Initial joystick enumeration (docs/re/setup-screens.md joystick pane,
    // sub_429628). Hotplug events refresh this again in present_setup/run_match
    // so a stick plugged in after boot still shows up without a restart.
    gamepads_.refresh();

    // Second phase of the boot LOADING dialog: "Loading sound..." (getstring
    // 200), shown before the sound-preload step (sub_42896E/sub_4287B9) — here,
    // audio_.init(). Skipped in --demo mode, matching that the demo path never
    // calls audio_.init either.
    if (!opts_.demo) {
        draw_boot_loading_dialog(ren, front_font_, assets_.getstring(200, "Loading sound...").c_str());
        if (!audio_.init(game)) std::fprintf(stderr, "audio unavailable, continuing silent\n");
    }

    base_tuning_ = match::build_match_config(scheme_, 2, 0, &values_).tuning;
    // Seed setup-screen slot colours from VALUELST for any colour without a .RMP
    // tail (a loaded .RMP keeps its own authoritative tail), then build the
    // per-player recolored sprite sets (authentic .RMP remap where available).
    assets_.set_color_fallbacks(base_tuning_.color_rgb, 10);
    assets_.build_player_sets(base_tuning_.color_rgb);
    seqs_.resolve(assets_);  // re-resolve: player sprite sets exist now

    renderer_.emplace(ren, assets_, seqs_, values_);
    screen_.emplace(assets_, audio_);
    transition_.emplace(assets_);
    // front_font_ (FONT6.FON glyph textures for the dialog chrome and the .BM
    // help/credits screens) was already built above, before the boot LOADING
    // dialogs — matching sub_41095A's real init order. assets_.load() reloads
    // the same FONT6.FON into assets_.frontend_font() (harmless — identical
    // file), so no second build() is needed here.
    return true;
}

namespace {

// The front-end screen library (docs/re/frontend-flow.md). Data-only — each
// full-screen image maps to one ScreenDef; a screen carries NO music id (music
// is a separate concern, sub_42741E, started once by the boot/menu callers and
// played continuously — see run_boot_attract / present_menu). The waited-screen
// dwell is CONFIRMED getvalue(12) = 7 s (VALUELST line `12,7`; sub_42A088's wait
// loop times out at start + getvalue(12) using C time_() = seconds, synthesizing
// Enter). Logos and title all use that 7 s dwell and stay keypress-skippable;
// results wait a bounded beat then return. Missing art just clears to black and
// the screen still advances.

// The CONFIRMED front-end SOUNDLST ids (BM95.EXE, docs/re/frontend-flow.md):
//   boot/title music 1000 (0x3E8, sub_42741E in sub_42B060 @0x42B060)
//   menu music       1010 (0x3F2, sub_42741E in sub_42B9CE @0x42B9CE)
//   title intro sting 2800 (sub_427BFB(2800) in sub_42B060, one-shot)
//   accept sting       10  (menuexit, sub_427961(10) in sub_42A088)
//   nav blip           20  (letter1,  sub_427961(20) in sub_42A088)
// Music tracks loop (sub_4273A4 sets loop count 0xFFFF); stings/blips are
// one-shot (sub_427B36 sets loop count 0). AudioEngine mirrors this split:
// start_music() = the looping music channel, play() = a one-shot SFX voice.
constexpr int kBootMusicId = 1000;   // 0x3E8 — TITLE.RSS, the continuous boot track
constexpr int kMenuMusicId = 1010;   // 0x3F2 — MENU.RSS, started on menu entry
// The Play-handler music (sub_42A3F6), CORRECTED by docs/re/in-match-shell.md
// §2 (supersedes this file's earlier "1020 under VICTORY" reading):
//   - Play entry (pseudo.c 29696): sub_42741E(0x3FC) = 1020 ("win") — this is
//     actually the SETUP-SCREENS track (player select / LEVEL & ROUNDS), not
//     victory music. Kept as kWinMusicId for the name's sake (matches
//     SOUNDLST's own "win" label) but used only where the setup screens run.
//   - Round end (pseudo.c 29820): sub_42741E(0x46A) = 1130 ("draw") replaces
//     the stage track UNCONDITIONALLY, before the survivor test — so DRAW,
//     the RESULTS tally, AND VICTORY/TEAM all play under 1130; nothing
//     restarts 1020 anywhere in the outcome tier.
// Both are looping tracks (start_music), replacing the menu/stage music.
constexpr int kWinMusicId = 1020;    // 0x3FC — WIN.RSS, setup-screens backdrop (NOT victory)
constexpr int kDrawMusicId = 1130;   // 0x46A — DRAW.RSS, DRAW *and* RESULTS *and* VICTORY backdrop
// Per-level in-round stage track fallback (sub_4293E5, docs/re/
// in-match-shell.md §2): SOUNDLST 1100+level, or this id when the level has
// no entry (a stripped/minimal-install SOUNDLST — every built-in stage here
// has a real 1100..1110 entry).
constexpr int kStageMusicFallback = 1120;  // 0x460 — GENERIC.RSS
// The title intro sting is a contiguous SOUNDLST GROUP (sub_427BFB(2800) picks a
// random member): 2800..2810 = "ATOMIC BOMBERMAN!" takes (GEN8A/…); the file's
// "2899 is the last intro" comment is the group's nominal end. We span 2800..2899
// and let play_random_in_range hit only the loaded ids.
constexpr int kTitleStingLo = 2800;
constexpr int kTitleStingHi = 2899;
// The menu-quit / exit sting group (sub_427BFB(2600) in the quit handler
// sub_412987): 2600..2699 = "go outside and play now!" takes (quitgame/EOFM7*/…).
constexpr int kQuitStingLo = 2600;
constexpr int kQuitStingHi = 2699;

// The waited-screen dwell — CONFIRMED getvalue(12) = 7 (VALUELST line `12,7`).
// sub_42A088's wait loop times out at start + getvalue(12) using C time_()
// (whole seconds), then synthesizes Enter (13) and advances. The logos and the
// title all share this one timeout; on the title's timeout the original falls
// straight through to the menu (it does NOT re-run the intro). We express it in
// ms (getvalue(12) * 1000) so it is resolution-independent.
constexpr std::uint32_t kBootDwellMs = 7000;  // getvalue(12) == 7 s

// The main-menu ATTRACT idle timeout — CONFIRMED getvalue(92) = 30 (VALUELST
// `92,30`), gated `> 5` (the file's own legend: values < 5 disable attract
// entirely), distinct from the waited-screen getvalue(12) above
// (docs/re/frontend-flow.md "Attract mode" / "Tunables"). Expressed as a
// fallback in ms; present_menu reads the live VALUELST value via
// values_.at_or(92, ...) so a modified install's timeout is honoured.
constexpr std::int64_t kAttractIdleFallbackS = 30;
constexpr std::int64_t kAttractIdleMinS = 5;  // getvalue(92) <= 5 disables attract

ScreenDef logo_screen(const char* bg) {
    return ScreenDef{bg, {}, /*dwell_ms*/ kBootDwellMs, /*skippable*/ true};
}
ScreenDef title_screen() {
    return ScreenDef{"TITLE", {}, /*dwell_ms*/ kBootDwellMs, /*skippable*/ true};
}
// Results: DRAW (no survivor / time up) or VICTORY<player> (one survivor). The
// original draws these with sub_42A088(name, 0) then a bespoke "any key, or 6 s
// in attract" loop (sub_42A3F6); we model it as a normal Screen with a bounded
// dwell so an unattended machine returns to the menu on its own.
constexpr std::uint32_t kResultsDwellMs = 6000;  // sub_42A3F6 attract auto-advance
// Format a MESSAGES.TXT label that carries a single %u/%d/%i with `v`, safely:
// the format string is the user's own file, so ignore any %s/%% (leave literal)
// rather than risk a wrong-type sprintf. A minimal, crash-proof getstring format.
std::string fmt_u(const std::string& f, int v) {
    auto p = f.find('%');
    if (p == std::string::npos) return f;
    std::size_t q = p + 1;
    while (q < f.size() && f[q] != 'u' && f[q] != 'd' && f[q] != 'i' && f[q] != 's' &&
           f[q] != '%')
        ++q;
    if (q < f.size() && (f[q] == 'u' || f[q] == 'd' || f[q] == 'i'))
        return f.substr(0, p) + std::to_string(v) + f.substr(q + 1);
    return f;
}

// Same crash-proof single-specifier substitution for a %s label (the level-line
// getstring(210)): splice `v` in for the first %s, leave any other specifier
// literal. The format string is the user's own MESSAGES.TXT entry.
std::string fmt_s(const std::string& f, const std::string& v) {
    auto p = f.find('%');
    if (p == std::string::npos) return f;
    std::size_t q = p + 1;
    while (q < f.size() && f[q] != 'u' && f[q] != 'd' && f[q] != 'i' && f[q] != 's' &&
           f[q] != '%')
        ++q;
    if (q < f.size() && f[q] == 's') return f.substr(0, p) + v + f.substr(q + 1);
    return f;
}

ScreenDef draw_screen() {
    // DRAW.PCX. The draw sting is a ONE-SHOT group play (sub_427BFB(1700) picks a
    // random member of the contiguous "tie game/draw game" SOUNDLST run at 1700),
    // fired once by run_app on entering Results via audio_.play_random_in_range —
    // NOT looped: a screen carries no music id, so nothing restarts the sting.
    return ScreenDef{"DRAW", {}, kResultsDwellMs, /*skippable*/ true};
}
// The SOUNDLST "tie game/draw game" voice group begins at 1700 (the file's own
// "; tie game/draw game" comment) and runs contiguously to its "1999 is the last
// tie game/draw game sound" bound; sub_427BFB(1700) plays a random member once.
// We span the full 1700..1999 group so play_random_in_range can pick any loaded
// take (GUMP1/GEN11*/ZAA*/…), matching the original's variety.
constexpr int kDrawStingLo = 1700;
constexpr int kDrawStingHi = 1999;
// VICTORY<player>.PCX / TEAM<0/1>.PCX — the original resolves "victory%u"/
// "team%u" against the winner index / clinching team (sub_42A3F6 aVictoryU/
// aTeamU); see results.hpp's victory_background_name for the full RE
// citation. The "we have a winner" voice group (2000) is played by run_app's
// Results handler, under this screen, per §1 (fires as soon as v73 is
// computed). (ScreenDef.background owns its own std::string copy, so this is
// safe.)
ScreenDef victory_screen(bool team_mode, int player, int team) {
    return ScreenDef{victory_background_name(team_mode, player, team), {}, kResultsDwellMs,
                     /*skippable*/ true};
}
// --- Main-menu model (sub_42B9CE) -----------------------------------------
// The original menu highlights one of seven rows (its selection variable v10
// runs 0..6) over MAINMENU.PCX and dispatches on Enter: 0=Play (sub_42A3F6 runs
// a match + results), 1/2=setup screens (sub_42B0CE/sub_42B47D), 3=editor,
// 4=credits (.BM), 5=the generic *.BM help browser (sub_41431C — CORRECTED,
// docs/re/results-and-options.md §4: the row's old "Roulette" label was wrong;
// it lists every *.BM in the install root, ROULETTE.BM among them), 6=quit
// (sub_412987, exit sting 2600). We keep the row set and order but map the
// not-yet-built leaves to their stub AppInputs; Start/Quit/Credits/Help are
// wired live. (docs/re/frontend-flow.md.)
struct MenuItem {
    AppInput action;   // resolved when Enter selects this row
    bool live;         // false = a documented stub row (no handler yet, inert)
};

// Seven rows in the ORIGINAL's v10 order (sub_42B9CE), so the cursor anchor
// (getvalue 700-702) lands on the labels baked into MAINMENU.PCX. Row targets
// per the CORRECTED dispatch (docs/re/results-and-options.md: v10==3 goes to
// sub_4080DC = the OPTIONS screen, NOT an editor; v10==1/2 are the START/JOIN
// NET GAME screens sub_42B0CE/sub_42B47D, netplay-deferred per ADR-0003):
//   0 Play           -> StartMatch   (live)
//   1 net game A     -> OpenNetwork  (NETWORK.BM help overlay; the real
//                                     START NET GAME screen = netplay, deferred)
//   2 net game B     -> OpenNetwork  (ditto for JOIN NET GAME)
//   3 Options        -> OpenOptions  (the interactive sub_4080DC screen;
//                                     F1 on it reaches the help browser)
//   4 Credits        -> OpenCredits  (live: CREDITS.BM viewer)
//   5 Help browser   -> live, handled INLINE (see the SDLK_RETURN case below):
//                       sub_41431C dispatches with no wipe in the original, so
//                       row 5 is special-cased ahead of this table rather than
//                       routed through AppInput/next() like rows 0-4/6 are —
//                       its kMenuItems entry below is unused/dead for row 5.
//   6 Quit           -> Quit         (live)
// INPUT.BM has no dedicated main-menu row or controller-setup screen in the
// original — CONFIRMED negative (docs/re/frontend-flow.md, 2026-07-09): no
// "controller" string and no "INPUT.BM" literal exist anywhere in pseudo.c.
// It is one of ~10 topics the generic *.BM help browser (row 5/F1,
// HelpBrowser) already globs and lists, which this port already reproduces.
// The OpenControllers edge below is exercised by the doctest/flow but
// deliberately left unbound to any row — there is nothing in the original's
// seven menu rows to bind it to.
constexpr MenuItem kMenuItems[] = {
    {AppInput::StartMatch, true},       // 0 Play
    {AppInput::OpenNetwork, true},      // 1 START NET GAME -> network help
    {AppInput::OpenNetwork, true},      // 2 JOIN NET GAME -> network help
    {AppInput::OpenOptions, true},      // 3 Options (sub_4080DC) — was misbound to row 1
    {AppInput::OpenCredits, true},      // 4 Credits
    {AppInput::Advance, false},         // 5 Help browser (handled inline, entry unused)
    {AppInput::Quit, true},             // 6 Quit
};
constexpr int kMenuCount = static_cast<int>(std::size(kMenuItems));

// Cursor anchor over MAINMENU.PCX — CONFIRMED getvalue(700/701/702) (sub_42B9CE:
// v11=getvalue(700)=X, v1=getvalue(701)=Y, getvalue(702)=Y-step; the bomb-
// trigger sprite is blitted at x=X, y=Y + Ystep*row). Read live from VALUELST
// (columns of the multi-value row 700, whose own legend reads "X, Y - first item
// / YS - y-spacing"); these fallbacks are that install's values (332,140,38) so
// a stripped VALUELST still positions sanely. The idle/attract timeout uses
// getvalue(92) (sub_42B9CE), distinct from the waited-screen getvalue(12).
constexpr int kMenuCursorXFallback = 332;      // getvalue(700)
constexpr int kMenuCursorYFallback = 140;      // getvalue(701)
constexpr int kMenuCursorStepFallback = 38;    // getvalue(702)

}  // namespace

void GameApp::start_match(std::uint32_t seed) {
    // Random Start (options.ini "random_start=" / Options row 1, §3):
    // shuffles which of the scheme's own spawn slots each player index gets
    // — CONFIRMED as the original's 200-pair-swap over the 10 start slots
    // (sub_421793; match_factory.hpp mirrors the loop, docs/re/facts.md
    // "Options toggles").
    sim::MatchConfig cfg = match::build_match_config(scheme_, sim::kMaxPlayers, seed, &values_,
                                                      options_.random_start);
    // Roster from the PLAYER INPUT screen (present_setup): OFF slots are inactive,
    // COMPUTER slots are AI-driven, KEYBOARD slots are local human(s). The per-slot
    // team feeds MatchConfig::team[] -> the hashed Player::team.
    //
    // Mapping: the original's +84 byte is 0 or 1 and IN TEAM MODE BOTH values
    // are real teams (sub_4141F8 inks team 1 vs "the rest" — two sides), while
    // the sim's convention reserves team 0 for "no team / solo side"
    // (simulation.cpp on_same_side). So shift the setup byte up by one when
    // team play is on: +84==0 -> sim team 1, +84==1 -> sim team 2. Without the
    // shift every un-toggled slot would wrongly fight solo.
    for (int i = 0; i < sim::kMaxPlayers; ++i) {
        cfg.active[i] = setup_type_[i] != 0;
        cfg.ai[i] = setup_type_[i] == 1;
        cfg.team[i] = static_cast<std::uint8_t>(setup_team_[i] + 1);
    }
    // Override the Conveyor Speed index from options.ini if present (this
    // install = 2 high); otherwise Tuning keeps the confirmed default (1
    // medium). conveyor_speed() clamps to [0, count-1], so a raw index is safe.
    if (conveyor_speed_index_) cfg.tuning.conveyor_speed_index = *conveyor_speed_index_;
    // Stomped Bombs Detonate (options.ini "stomped_bombs_detonate=" / Options
    // row 4, §3 — dword_464940): whether a closing enclosement wall landing
    // on a grounded bomb DETONATES it (queued full explosion) or silently
    // eats it. The enclosure site reads the merged global directly
    // (sub_426818 ~27260), so the Options value overrides the VALUELST id 46
    // seed unconditionally — options_ was itself seeded from getvalue(46)
    // at init, mirroring sub_41095A. Consumer: EnclosureSystem::drop_wall.
    cfg.tuning.wall_detonates = options_.stomped_bombs_detonate ? 1 : 0;
    // Diseases Can Be Destroyed (options.ini "diseases_destroyable=" /
    // Options row 11, §3 — dword_464990): OFF makes a destroyed floor skull
    // relocate to a random free tile instead of being lost (sub_4230A5 /
    // sub_42331C flame walk -> sub_4255B2(2)). Same merged-global override
    // as above (seed = getvalue(120), sub_41095A). Consumers:
    // FlameSystem::spread_to and BombSystem::slide.
    cfg.tuning.diseases_destroyable = options_.diseases_destroyable;
    // Enclosement Depth (options.ini "enclosement_depth=" / Options row 7,
    // §3): a REAL Tuning consumer (enclosure.cpp/ai.cpp). base_tuning_ already
    // carries the VALUELST default; the Options screen's live edit overrides
    // it per match, same pattern as Conveyor Speed above.
    cfg.tuning.enclosement_depth = options_.enclosement_depth;
    // Play Time (options.ini "playtime=" / Options row 9, §3): a REAL Tuning
    // consumer (setup.cpp's ticks_left = game_seconds * kTicksPerSecond). The
    // "unlimited" sentinel (1001) has no sim meaning yet — a very long but
    // finite clock is the closest faithful stand-in without inventing a
    // separate "no clock" sim mode (out of scope: PRESENTATION/CONFIG ONLY).
    cfg.tuning.game_seconds =
        options_.playtime_seconds == 1001 ? 99999 : options_.playtime_seconds;
    // Team Play (options.ini "team_play=" / the interactive Options screen):
    // the game-type-level team-mode GATE (docs/re/setup-screens.md
    // `dword_464964`), separate from each slot's own +84 team byte. OFF means
    // team mode is off regardless of what a slot's 'T' toggle left behind, so
    // zero every slot's team here (sim team 0 = solo side) — MatchConfig::
    // team[] stays the single source of truth for the hashed Player::team.
    if (!team_play_) cfg.team.fill(0);
    // Goldman wheel award (docs/re/goldman-roulette.md §4/§9): sub_4214BC
    // grants the last spin's prize to the gold player EVERY round of the
    // following match, not just the round right after the spin —
    // build_match_config runs at every start_match() call (including
    // RoundContinue's re-init), so re-applying gold_prize_/gold_player_ here
    // reproduces that "persists until the next spin" behaviour for free. A
    // no-op (all-false/all-zero overlay) whenever gold_prize_ < 0 (no
    // successful spin yet).
    if (gold_player_ >= 0 && gold_prize_ >= 0) {
        // Clogs (prize 13) is NOT a sim::PowerupType (doc §8/§9.2 — never a
        // scheme/spawn kind) — it routes to MatchConfig::born_with_clogs
        // instead of wheel_prize_to_powerup/born_with_extra, alongside (not
        // instead of) the normal-kind branch below.
        bool is_clogs = gold_prize_ == kClogsPrizeId;
        sim::PowerupType pt =
            is_clogs ? sim::PowerupType::None : wheel_prize_to_powerup(gold_prize_);
        if (pt != sim::PowerupType::None || is_clogs) {
            auto kind = static_cast<int>(pt);
            if (team_play_) {
                // Team mode: the doc's "team id encoded as 0 or 2" compares
                // against the RAW +84 byte, i.e. our setup_team_[] before the
                // +1 shift above — every member of the gold TEAM gets the
                // bump (doc §4 "every member of the gold team").
                for (int i = 0; i < sim::kMaxPlayers; ++i) {
                    if (!cfg.active[i] || setup_team_[i] != gold_player_) continue;
                    if (is_clogs)
                        cfg.born_with_clogs[i] = 1;  // reset-then-+1 every round, §9.3 — not accumulated
                    else
                        cfg.born_with_extra[i][kind] = true;
                }
            } else if (gold_player_ < sim::kMaxPlayers && cfg.active[gold_player_]) {
                if (is_clogs)
                    cfg.born_with_clogs[gold_player_] = 1;  // reset-then-+1 every round, §9.3
                else
                    cfg.born_with_extra[gold_player_][kind] = true;
            }
        }
    }
    // Level from the LEVEL screen (present_map_select -> dword_464998): the match
    // init (sub_410B6E) resolves it to a stage index dword_46499C. RANDOM (-1) ->
    // keep pick_stage over the enabled rotation (VALUELST 1150-1160, the same
    // 200-try random loop the original runs); a specific level (0..10) -> use that
    // index directly. Clamp to the valid stage range defensively.
    int stage;
    if (selected_level_ < 0) {
        stage = match::pick_stage(base_tuning_, seed);
    } else {
        stage = selected_level_;
        if (stage > 10) stage = 10;
    }
    // Overlay this board's stage actors (conveyors/trampolines/etc) from
    // EXTRA<stage>.RES before constructing the sim — the actor layout is a
    // hashed setup input like the cell grid (docs/re/stage-actors.md). A board
    // with no EXTRA file simply has none. Random '-T,H' trampolines resolve off
    // a setup-only RNG inside apply_actors, never the sim's per-tick stream.
    auto actors = assets::extra::load_for_board(opts_.game_dir, stage, sim::kGridWidth,
                                                sim::kGridHeight);
    match::apply_actors(cfg, actors, seed);
    // Campaign rover/ghost hazards (docs/re/campaign.md "Rover/ghost/AI
    // roster", "sub_40151B — the REAL per-stage starter"): fields 3-6 of the
    // current stage's .CAM record. build_state (setup.cpp) spawns them (ghost
    // first, then rover, matching sub_40151B's own call order) as part of
    // Simulation's constructor. A non-campaign match leaves these at 0
    // (MatchConfig's default), so RoverSystem::spawn/tick are true no-ops.
    if (campaign_active_ &&
        campaign_stage_index_ >= 0 &&
        campaign_stage_index_ < static_cast<int>(campaign_stages_.size())) {
        const assets::res::CampaignStage& stage_rec =
            campaign_stages_[static_cast<std::size_t>(campaign_stage_index_)];
        cfg.campaign_rovers = stage_rec.rovers;
        cfg.campaign_rover_speed = stage_rec.rover_speed;
        cfg.campaign_ghosts = stage_rec.ghosts;
        cfg.campaign_ghost_speed = stage_rec.ghost_speed;
    }
    sim_ = sim::Simulation(cfg);
    if (assets_.load_stage(stage)) {
        seqs_.resolve_stage(assets_, stage);
        // Disable music during gameplay (options.ini "disable_game_music=" /
        // Options row 13, §3): a REAL consumer — simply don't start the
        // in-match track. Menu/results music is untouched (the option is
        // specifically "during gameplay").
        //
        // Per-level stage track (docs/re/in-match-shell.md §2, sub_4293E5):
        // SOUNDLST 1100+level, falling back to 1120 ("generic") when the level
        // has no entry — our 11 built-in stages all have one (SOUNDLST.RES
        // 1100..1110), so this only matters for a stripped/modified install.
        if (!options_.disable_game_music) {
            int stage_music = 1100 + stage;
            if (!audio_.has_track(stage_music)) stage_music = kStageMusicFallback;  // 1120
            audio_.start_music(stage_music);
        }
    }
    // Untimed round HUD (docs/re/in-match-shell.md §3): the 1001 sentinel is a
    // presentation-only concept (see cfg.tuning.game_seconds's own comment
    // just above — the sim gets a very long but finite clock instead), so
    // tell the renderer directly rather than trying to infer "untimed" back
    // out of ticks_left.
    renderer_->reset_match(options_.playtime_seconds == 1001);  // NOLINT(bugprone-unchecked-optional-access)
    sounds_.reset();
}

int GameApp::run_demo() {
    for (int t = 0; t < opts_.demo_ticks; ++t) {
        sim_.tick(demo_inputs(t));
        sounds_.on_tick(sim_.state());
        renderer_->on_events(sim_.state());  // NOLINT(bugprone-unchecked-optional-access)
        renderer_->draw_frame(sim_.state());  // NOLINT(bugprone-unchecked-optional-access) — keeps walk-anim sampling in sync
    }
    SDL_Surface* shot = SDL_RenderReadPixels(sdl_renderer_.get(), nullptr);
    int rc = 1;
    if (shot) {
        rc = SDL_SaveBMP(shot, opts_.demo_out.string().c_str()) ? 0 : 1;
        SDL_DestroySurface(shot);
        std::printf("demo: %d ticks, alive %d, screenshot %s\n", opts_.demo_ticks,
                    sim::alive_count(sim_.state()), opts_.demo_out.string().c_str());
    }
    return rc;
}

AppInput GameApp::present_screen(const ScreenDef& def) {
    // Enter the screen (resets its clock/counter; music is NOT touched here —
    // the caller owns the continuous track, sub_42A088 only presents an image).
    screen_->enter(def, SDL_GetTicks());  // NOLINT(bugprone-unchecked-optional-access)
    AppInput result = AppInput::Advance;
    bool waiting = true;
    while (waiting) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_EVENT_QUIT) return AppInput::Quit;
            if (ev.type == SDL_EVENT_KEY_DOWN) {
                // Feed every key to the screen: sub_42A088 blips (SFX 20) on any
                // key and, for the accept keys (Enter/Space/Escape), plays the
                // accept sting (SFX 10) and finishes. Escape additionally routes
                // us "back"; Enter/Space "advance". The blip/sting come from the
                // Screen, so the music track is untouched — only the screen ends.
                screen_->on_key(ev.key.key);  // NOLINT(bugprone-unchecked-optional-access)
                if (ev.key.key == SDLK_ESCAPE) {
                    result = AppInput::Back;
                    waiting = false;
                }
            }
        }
        std::uint64_t now = SDL_GetTicks();
        screen_->update(now);  // NOLINT(bugprone-unchecked-optional-access)
        if (screen_->done()) waiting = false;  // NOLINT(bugprone-unchecked-optional-access)

        audio_.update_music();
        SDL_SetRenderDrawColor(sdl_renderer_.get(), 0, 0, 0, 255);
        SDL_RenderClear(sdl_renderer_.get());
        screen_->draw(sdl_renderer_.get());  // NOLINT(bugprone-unchecked-optional-access)
        SDL_RenderPresent(sdl_renderer_.get());
        SDL_Delay(2);
    }

    // No transition out: sub_42A088 CUTS between screens — it sets the palette
    // (sub_41522D, instant; the >>2 is the 8->6-bit VGA palette conversion, NOT
    // a fade loop), blits (sub_429FF1), and flips (sub_41043C). There is no wipe
    // on a waited screen (logos, title, results, .BM), so the next screen simply
    // replaces this one. (The menu->match select wipe in present_menu is a
    // separate, intentional use and is left alone.)
    return result;
}

AppInput GameApp::present_bm_screen(const std::string& bm_name) {
    // The `.BM` text-screen viewer (sub_41302D): MAINMENU.PCX as the persistent
    // backdrop (the original composites the scroll window over the menu page),
    // the parsed .BM text + inline images over it, keyboard line/page scroll,
    // and Enter/Escape to dismiss. No auto-scroll or dwell — it waits for input
    // exactly like the original.
    BmScreen bm(assets_, front_font_);
    bm.enter(bm_name);
    AppInput result = AppInput::Advance;
    while (!bm.done()) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_EVENT_QUIT) return AppInput::Quit;
            if (ev.type == SDL_EVENT_KEY_DOWN) {
                if (ev.key.key == SDLK_ESCAPE) result = AppInput::Back;
                bm.on_key(ev.key.key);
            }
        }
        audio_.update_music();
        SDL_SetRenderDrawColor(sdl_renderer_.get(), 0, 0, 0, 255);
        SDL_RenderClear(sdl_renderer_.get());
        // Backdrop: keep the menu art behind the text panel.
        const Sprite& bg = assets_.frontend_pcx("MAINMENU");
        if (bg.tex) {
            SDL_FRect d{0, 0, static_cast<float>(bg.w), static_cast<float>(bg.h)};
            SDL_RenderTexture(sdl_renderer_.get(), bg.tex, nullptr, &d);
        }
        bm.draw(sdl_renderer_.get());
        SDL_RenderPresent(sdl_renderer_.get());
        SDL_Delay(2);
    }

    // No wipe out: the .BM viewer (sub_41302D) dismisses back to the menu by a
    // cut, like every sub_42A088-style screen — the menu is redrawn from scratch
    // on the next frame. No screen-to-screen transition here.
    return result;
}

AppInput GameApp::present_help_browser() {
    // sub_41431C -> sub_414235 (docs/re/results-and-options.md §4): glob every
    // *.BM in the install root, show the list, open the pick through the same
    // .BM viewer, and re-show the list on return (HelpBrowser owns that
    // loop-back internally) until Esc cancels the list itself. MAINMENU stays
    // the persistent backdrop behind both the list and the viewer, matching
    // present_bm_screen's own convention (the original composites over
    // whatever screen was already up — the menu here, the live match field at
    // the in-round F1 call site, where the caller paints its own frame first).
    HelpBrowser browser(assets_, front_font_);
    // getvalue(15) ("is the online manual enabled?", default 1, §4): gate
    // BEFORE the glob, matching sub_414235's own order.
    browser.enter(values_.at_or(15, 1) != 0);
    while (!browser.done()) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_EVENT_QUIT) return AppInput::Quit;
            if (ev.type != SDL_EVENT_KEY_DOWN) continue;
            browser.on_key(ev.key.key, audio_);
        }
        if (browser.viewing() && browser.viewer().done()) browser.close_viewer();
        audio_.update_music();
        SDL_SetRenderDrawColor(sdl_renderer_.get(), 0, 0, 0, 255);
        SDL_RenderClear(sdl_renderer_.get());
        const Sprite& bg = assets_.frontend_pcx("MAINMENU");
        if (bg.tex) {
            SDL_FRect d{0, 0, static_cast<float>(bg.w), static_cast<float>(bg.h)};
            SDL_RenderTexture(sdl_renderer_.get(), bg.tex, nullptr, &d);
        }
        browser.draw(sdl_renderer_.get());
        SDL_RenderPresent(sdl_renderer_.get());
        SDL_Delay(2);
    }
    // No wipe out — same cut-back-to-caller convention as present_bm_screen.
    return AppInput::Advance;
}

AppInput GameApp::present_help_browser_modal() {
    // The in-round F1 opening of the SAME browser (docs/re/in-match-shell.md
    // §1's sub_42A16F(1)/(0) bracket): this loop never calls sim_.tick — the
    // match is genuinely frozen for its duration, exactly like the menu-row
    // browser never advances anything either. The backdrop is the live
    // (frozen) match render rather than MAINMENU, since the original
    // overlays the list dialog on whatever screen was already current.
    HelpBrowser browser(assets_, front_font_);
    // getvalue(15) ("is the online manual enabled?", default 1, §4): gate
    // BEFORE the glob, matching sub_414235's own order — the SAME gate the
    // menu-row browser above applies, since sub_41431C is one routine.
    browser.enter(values_.at_or(15, 1) != 0);
    while (!browser.done()) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_EVENT_QUIT) return AppInput::Quit;
            if (ev.type != SDL_EVENT_KEY_DOWN) continue;
            browser.on_key(ev.key.key, audio_);
        }
        if (browser.viewing() && browser.viewer().done()) browser.close_viewer();
        audio_.update_music();
        SDL_SetRenderDrawColor(sdl_renderer_.get(), 0, 0, 0, 255);
        SDL_RenderClear(sdl_renderer_.get());
        renderer_->draw_frame(sim_.state());  // NOLINT(bugprone-unchecked-optional-access) — last sim frame, frozen, no tick here
        browser.draw(sdl_renderer_.get());
        SDL_RenderPresent(sdl_renderer_.get());
        SDL_Delay(2);
    }
    return AppInput::Advance;
}

AppInput GameApp::present_options_screen() {
    // The interactive Options screen (options_screen.hpp/.cpp): the full
    // §3 19-item list's LIVE subset, over a random GLUE<n> backdrop like
    // present_setup's documented convention (docs/re/setup-screens.md). F1
    // opens the generic *.BM help browser — CORRECTED 2026-07-08: reading
    // sub_4080DC's own F1 dispatch (pseudo.c, `if (v165 <= 0x13B) sub_41431C();`)
    // shows it calls the SAME sub_41431C generic browser row 5 and the
    // in-round F1 key open (§4), not a fixed OPTIONS.BM cut. OPTIONS.BM is
    // just one entry in that browser's *.BM glob, same as EDITOR.BM (§3's
    // own correction: "reachable only as a directory-listing entry of the
    // help browser's *.BM glob"). Music left untouched here — unlike
    // present_setup this screen is reached straight from the main menu (not
    // the Play handler sub_42A3F6), so there is no confirmed "inherits 1020"
    // citation; it plays on under whatever the menu already started (1010,
    // kMenuMusicId).
    OptionsScreen opt(assets_, front_font_);
    opt.enter(options_, pick_glue());
    AppInput result = AppInput::Advance;
    while (!opt.done()) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_EVENT_QUIT) return AppInput::Quit;
            if (ev.type != SDL_EVENT_KEY_DOWN) continue;
            if (ev.key.key == SDLK_F1) {
                // Keep the .BM help reachable without leaving the interactive
                // screen: present the generic browser modally (same routine
                // row 5 and in-round F1 open, §4), then resume with the same
                // in-progress edits (present_help_browser owns its own loop).
                AppInput help = present_help_browser();
                if (help == AppInput::Quit) return AppInput::Quit;
                continue;
            }
            if (ev.key.key == SDLK_ESCAPE) result = AppInput::Back;
            opt.on_key(ev.key.key, audio_);
            // "Define keyboard layouts" (row 15, §3): push the key-remap
            // sub-screen (§2) modally, exactly like the F1 help overlay
            // above, then resume the Options screen with its in-progress
            // edits untouched (present_keyremap_screen owns its own loop and
            // applies its own result to keyboard_/options_dirty_ directly).
            if (opt.open_keyremap()) present_keyremap_screen();
        }
        audio_.update_music();
        SDL_SetRenderDrawColor(sdl_renderer_.get(), 0, 0, 0, 255);
        SDL_RenderClear(sdl_renderer_.get());
        opt.draw(sdl_renderer_.get());
        SDL_RenderPresent(sdl_renderer_.get());
        SDL_Delay(2);
    }

    // Edit the in-memory snapshot ONLY on an actual change (task requirement
    // 3's write-on-exit semantics, §2's CONFIRMED "held in memory ... only
    // flushed ... when the application exits normally") — options.ini itself
    // is untouched here; flush_options() (run()'s tail) is the sole writer.
    if (opt.changed()) {
        // doc §2: "Cleared to -1 by: ... the Options-screen Gold Bomberman
        // toggle" — ANY edit of that row (on or off) forfeits a pending gold
        // player, checked before options_ is overwritten with the new
        // snapshot so this compares old vs new.
        if (opt.snapshot().goldman != options_.goldman) gold_player_ = -1;
        options_ = opt.snapshot();
        team_play_ = options_.team_play;
        conveyor_speed_index_ = options_.conveyor_speed_index;
        options_dirty_ = true;
    }
    return result;
}

void GameApp::present_keyremap_screen() {
    // The key-remap UI (docs/re/results-and-options.md §2, sub_407B9D): a
    // 2x6 scancode-capture grid drawn OVER whatever the caller already
    // painted this frame (present_options_screen's Options backdrop — §2
    // "no new backdrop call"). Draws its own frame here rather than sharing
    // the caller's SDL_RenderPresent, since it needs its own event pump to
    // capture raw scancodes without those keys also driving the Options
    // cursor underneath.
    KeyRemapScreen remap(assets_, front_font_);
    std::array<KeySet, kKeyboardSets> current{keyboard_.key_set(0), keyboard_.key_set(1)};
    remap.enter(current);
    while (!remap.done()) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_EVENT_QUIT) { remap.on_key(SDLK_ESCAPE, SDL_SCANCODE_ESCAPE, audio_); return; }
            if (ev.type != SDL_EVENT_KEY_DOWN) continue;
            remap.on_key(ev.key.key, ev.key.scancode, audio_);
        }
        audio_.update_music();
        SDL_SetRenderDrawColor(sdl_renderer_.get(), 0, 0, 0, 255);
        SDL_RenderClear(sdl_renderer_.get());
        // §2: "no new backdrop call — sub_407B9D draws directly over the
        // Options screen's own frame". This screen has its own event pump
        // (to capture raw scancodes without leaking into the Options cursor
        // underneath), so there is no single shared frame to draw "over" —
        // a plain dark panel is the simplest faithful stand-in, since
        // sub_407B9D's own drawing (the grid + header) is self-contained and
        // legible against any backdrop.
        SDL_SetRenderDrawColor(sdl_renderer_.get(), 20, 20, 30, 255);
        SDL_RenderClear(sdl_renderer_.get());
        remap.draw(sdl_renderer_.get());
        SDL_RenderPresent(sdl_renderer_.get());
        SDL_Delay(2);
    }
    // Apply live (KeyboardMapper reads collect_inputs() every match tick) and
    // mark dirty for the write-on-exit flush — never write options.ini here.
    const auto& edited = remap.edited();
    keyboard_.set_key_set(0, edited[0]);
    keyboard_.set_key_set(1, edited[1]);
    options_dirty_ = true;
}

void GameApp::present_editor() {
    // The hidden scheme editor (docs/re/results-and-options.md §5): the
    // chooser (sub_403184) -> optionally the *.SCH file picker (sub_407582)
    // -> the editor proper (sub_4028D2) -> optionally the powerup rules
    // sub-editor (sub_402595). Runs its own nested loop exactly like
    // present_keyremap_screen() — this screen has no AppState/AppInput slot
    // (there is no menu row for it), so it simply returns to present_menu's
    // own loop when the chooser is dismissed.
    EditorChooserScreen chooser(assets_, front_font_);
    chooser.enter(pick_glue());

    while (true) {
        EditorChooserResult action = EditorChooserResult::None;
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_EVENT_QUIT) return;
            if (ev.type != SDL_EVENT_KEY_DOWN) continue;
            EditorChooserResult r = chooser.on_key(ev.key.key, audio_);
            if (r != EditorChooserResult::None) action = r;
        }
        audio_.update_music();
        SDL_SetRenderDrawColor(sdl_renderer_.get(), 0, 0, 0, 255);
        SDL_RenderClear(sdl_renderer_.get());
        chooser.draw(sdl_renderer_.get());
        SDL_RenderPresent(sdl_renderer_.get());
        SDL_Delay(2);

        if (action == EditorChooserResult::Exit) return;
        if (action == EditorChooserResult::Help) {
            // §5: F1 opens the same generic help browser (sub_41431C, §4)
            // every other screen reaches on F1 — CORRECTED 2026-07-08: this
            // was calling present_bm_screen("EDITOR") directly (a fixed-topic
            // cut), contradicting this very comment. Confirmed against
            // sub_403184's own F1 branch (pseudo.c, `if (v18 == 315)
            // sub_41431C();`): it is the generic browser, listing EDITOR.BM
            // as one glob entry among the rest (§3's correction: "reachable
            // only as a directory-listing entry of the help browser's *.BM
            // glob") — not a direct open of it.
            if (present_help_browser() == AppInput::Quit) return;
            continue;
        }

        std::optional<assets::sch::Scheme> initial;
        bool opened = false;
        if (action == EditorChooserResult::New) {
            opened = true;  // sub_4028D2(1): blank board, EditorScreen::enter(nullopt, ...)
        } else if (action == EditorChooserResult::EditExisting) {
            // sub_407582: the *.SCH file picker over DATA/SCHEMES.
            SchemeFilePicker picker(assets_, front_font_);
            picker.enter(opts_.game_dir / "DATA" / "SCHEMES", pick_glue());
            while (!picker.done()) {
                SDL_Event pev;
                while (SDL_PollEvent(&pev)) {
                    if (pev.type == SDL_EVENT_QUIT) return;
                    if (pev.type != SDL_EVENT_KEY_DOWN) continue;
                    picker.on_key(pev.key.key, audio_);
                }
                audio_.update_music();
                SDL_SetRenderDrawColor(sdl_renderer_.get(), 0, 0, 0, 255);
                SDL_RenderClear(sdl_renderer_.get());
                picker.draw(sdl_renderer_.get());
                SDL_RenderPresent(sdl_renderer_.get());
                SDL_Delay(2);
            }
            if (!picker.cancelled() && !picker.empty()) {
                try {
                    initial = assets::sch::load(picker.selected());
                    opened = true;
                } catch (const std::exception&) {
                    opened = false;  // corrupt/unreadable file: fall back to the chooser
                }
            }
        }

        if (!opened) continue;  // back to the chooser menu

        // Canvas art: the editor draws the "tile 0 blank/solid/brick"
        // sequences (sub_402206's dword_45B7B8 is only ever 0 — §5), so make
        // sure tileset 0 is the one loaded in AssetStore; a later
        // start_match reloads whatever stage the match picks.
        assets_.load_stage(0);

        // sub_4049C0's default start positions for a NEW scheme: VALUELST
        // x = getvalue(600+2j), y = getvalue(601+2j) (wrapped into the board
        // by EditorGrid::reset). Fallback 0s if VALUELST is absent.
        std::array<std::array<int, 2>, kEditorMaxStarts> default_starts{};
        for (int j = 0; j < kEditorMaxStarts; ++j) {
            default_starts[static_cast<std::size_t>(j)][0] =
                static_cast<int>(values_.column_or(600 + 2 * j, 0, 0));
            default_starts[static_cast<std::size_t>(j)][1] =
                static_cast<int>(values_.column_or(601 + 2 * j, 0, 0));
        }

        EditorScreen editor(assets_, front_font_);
        editor.enter(initial, pick_glue(), &default_starts);
        // The 'N'/'n' scheme-name prompt (§5) needs real text input (letters
        // beyond the raw keycode switch below); start it for the whole
        // editor session — harmless while the prompt is closed since
        // on_text_input() only accepts characters when prompt_kind_==Name.
        SDL_StartTextInput(window_.get());
        while (!editor.done()) {
            SDL_Event eev;
            while (SDL_PollEvent(&eev)) {
                if (eev.type == SDL_EVENT_QUIT) return;
                if (editor.editing_powerups()) {
                    if (eev.type != SDL_EVENT_KEY_DOWN) continue;
                    editor.powerups_screen().on_key(eev.key.key, audio_);
                    // sub_402595 returns into sub_4028D2's loop on its exit
                    // keys — mirror that by closing the sub-editor here (the
                    // screen sets done() but cannot clear the parent's
                    // routing flag itself).
                    if (editor.powerups_screen().done()) editor.close_powerups();
                    continue;
                }
                if (eev.type == SDL_EVENT_TEXT_INPUT) {
                    editor.on_text_input(eev.text.text);
                    continue;
                }
                if (eev.type == SDL_EVENT_MOUSE_BUTTON_DOWN) {
                    // Window/backbuffer pixel -> logical (640x480) coordinate,
                    // per the SDL_LOGICAL_PRESENTATION_LETTERBOX mode set in
                    // init() — mirrors sub_42665C/sub_4266A3's pixel->cell
                    // mappers (§5), then the logical pixel -> grid cell via
                    // EditorScreen's own fixed cell geometry.
                    float lx = 0, ly = 0;
                    SDL_RenderCoordinatesFromWindow(sdl_renderer_.get(), eev.button.x, eev.button.y,
                                                    &lx, &ly);
                    int gx = (static_cast<int>(lx) - EditorScreen::kOriginX) / EditorScreen::kCellW;
                    int gy = (static_cast<int>(ly) - EditorScreen::kOriginY) / EditorScreen::kCellH;
                    editor.on_mouse_down(eev.button.button, gx, gy);
                    continue;
                }
                if (eev.type == SDL_EVENT_MOUSE_MOTION) {
                    // §5d, PINNED: sub_431804 reads the live cursor position
                    // every loop iteration to draw the brush preview AT it
                    // (pseudo.c 5520-5524) — same logical-coordinate mapping
                    // as the button-down case above, but RAW pixels (no
                    // grid-cell snapping; EditorScreen::on_mouse_move does
                    // its own hotspot-anchored draw).
                    float lx = 0, ly = 0;
                    SDL_RenderCoordinatesFromWindow(sdl_renderer_.get(), eev.motion.x, eev.motion.y,
                                                    &lx, &ly);
                    editor.on_mouse_move(lx, ly);
                    continue;
                }
                if (eev.type != SDL_EVENT_KEY_DOWN) continue;
                // Ctrl+F (flood fill) and Ctrl+B (reset, §5) both need the
                // modifier; every other editor key (including '0's tileset
                // toggle) is unmodified — editor_key_needs_ctrl
                // (editor_screen.hpp) lists exactly those two, so plain
                // 'F'/'B' (e.g. the powerup sub-editor's own 'F' forbidden
                // toggle — a SEPARATE screen/handler) never reach here
                // ungated.
                bool needs_ctrl = editor_key_needs_ctrl(eev.key.key);
                bool ctrl_down = (eev.key.mod & SDL_KMOD_CTRL) != 0;
                if (!needs_ctrl || ctrl_down) editor.on_key(eev.key.key, audio_);
            }
            audio_.update_music();
            SDL_SetRenderDrawColor(sdl_renderer_.get(), 0, 0, 0, 255);
            SDL_RenderClear(sdl_renderer_.get());
            editor.draw(sdl_renderer_.get());
            SDL_RenderPresent(sdl_renderer_.get());
            SDL_Delay(2);
        }
        SDL_StopTextInput(window_.get());

        // §5: exit writes through sub_403C16 — our assets::sch::write() — on
        // a confirmed save. Written schemes go to the install's DATA/
        // SCHEMES dir (the SAME place the game loads them, §3's "Scheme
        // File" row / init()'s scheme_path), NEVER the repo. The file name
        // is derived from the in-editor -N name (§5 'N'/'n'); an empty name
        // falls back to a generic "EDITED.SCH" rather than inventing a
        // prompt-less silent overwrite of BASIC.SCH.
        if (editor.save_requested()) {
            assets::sch::Scheme out = editor.grid().to_scheme();
            std::string file_stem = out.name.empty() ? std::string("EDITED") : out.name;
            for (auto& c : file_stem)
                if (!std::isalnum(static_cast<unsigned char>(c))) c = '_';
            std::filesystem::path schemes_dir = opts_.game_dir / "DATA" / "SCHEMES";
            std::error_code ec;
            std::filesystem::create_directories(schemes_dir, ec);
            std::filesystem::path out_path = schemes_dir / (file_stem + ".SCH");
            try {
                assets::sch::write(out, out_path);
                // Make the freshly-saved scheme immediately selectable
                // through the existing scheme rotation/pick path (task
                // requirement 3): point this session's live scheme at it,
                // exactly like passing --scheme would.
                scheme_ = out;
                opts_.scheme = out_path;
            } catch (const std::exception& e) {
                std::fprintf(stderr, "scheme editor: save failed: %s\n", e.what());
            }
        }
        // Either way (saved or discarded), fall back to the chooser so
        // Ctrl+E's single trigger can serve multiple edits without
        // re-pressing the 6-key sequence — §5 does not document the
        // chooser as single-shot, and sub_403184's own loop (its Esc/'Q'
        // exit case) implies it re-shows after each sub_4028D2 return.
    }
}

void GameApp::present_campaign_picker() {
    // sub_4015C6 (docs/re/campaign.md "Trace: string refs -> loader ->
    // trigger -> entry point"): glob "*.cam" in the install root,
    // list, pick, parse, arm campaign mode. Runs its own nested loop exactly
    // like present_editor's chooser/picker loops — no AppState/AppInput slot,
    // since there is no menu row for this screen either.
    CampaignFilePicker picker(assets_, front_font_);
    picker.enter(opts_.game_dir, pick_glue());
    while (!picker.done()) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_EVENT_QUIT) return;
            if (ev.type != SDL_EVENT_KEY_DOWN) continue;
            picker.on_key(ev.key.key, audio_);
        }
        audio_.update_music();
        SDL_SetRenderDrawColor(sdl_renderer_.get(), 0, 0, 0, 255);
        SDL_RenderClear(sdl_renderer_.get());
        picker.draw(sdl_renderer_.get());
        SDL_RenderPresent(sdl_renderer_.get());
        SDL_Delay(2);
    }
    if (picker.cancelled() || picker.empty()) return;  // sub_4015C6's error-dialog path (port: silent)

    try {
        assets::res::Campaign parsed = assets::res::load_campaign(picker.selected());
        if (parsed.stages.empty()) return;  // "Couldn't open..." / zero-stage file: leave state untouched
        campaign_stages_ = std::move(parsed.stages);
        campaign_stage_index_ = 0;  // dword_4648B0 = 0
        if (!load_campaign_stage(0)) {
            // The stage's scheme couldn't be resolved (e.g. a hand-authored
            // .CAM naming a scheme the install doesn't ship) — bail out of
            // arming campaign mode rather than starting a match against a
            // stale/mismatched board (port convenience; unpinned by the RE).
            campaign_stages_.clear();
            return;
        }
        campaign_active_ = true;  // dword_46489C = 1
        // sub_4015C6's own confirmation overlay (getstring 1210 + 95) —
        // PORTED 2026-07-09 (present_campaign_confirm, above), replacing the
        // former accept-sting stand-in. The SEPARATE stage-start banner
        // (sub_40133F, getstring 1235/1230 — docs/re/campaign.md "Stage
        // banner") follows right after, same as the original's sub_410B6E
        // showing it for the freshly-armed stage 0.
        if (present_campaign_confirm() == AppInput::Quit) {
            campaign_active_ = false;
            campaign_stages_.clear();
            return;
        }
        if (present_campaign_banner() == AppInput::Quit) {
            campaign_active_ = false;
            campaign_stages_.clear();
            return;
        }
    } catch (const std::exception&) {  // NOLINT(bugprone-empty-catch)
        // aCouldnTOpenCam path (§1/§3): unreadable/corrupt file. Leave
        // campaign mode untouched, same as a cancelled picker.
    }
}

bool GameApp::load_campaign_stage(int index) {
    if (index < 0 || index >= static_cast<int>(campaign_stages_.size())) return false;
    const assets::res::CampaignStage& stage = campaign_stages_[static_cast<std::size_t>(index)];

    // Resolve the stage's "scheme to use" name to a DATA/SCHEMES/<name>.SCH
    // path, case-insensitively (DOS filenames are case-insensitive; every
    // other picker in this codebase does a case-insensitive extension/name
    // match for the same reason — editor_screen.cpp's SchemeFilePicker).
    std::filesystem::path schemes_dir = opts_.game_dir / "DATA" / "SCHEMES";
    std::error_code ec;
    std::filesystem::path found;
    for (const auto& entry : std::filesystem::directory_iterator(schemes_dir, ec)) {
        if (!entry.is_regular_file()) continue;
        std::string stem = entry.path().stem().string();
        std::string ext = entry.path().extension().string();
        for (auto& c : stem) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        for (auto& c : ext) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        std::string want = stage.scheme;
        for (auto& c : want) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        if (ext == ".SCH" && stem == want) { found = entry.path(); break; }
    }
    if (found.empty()) return false;

    try {
        scheme_ = assets::sch::load(found);
    } catch (const std::exception&) {
        return false;
    }

    // AI roster auto-fill — CORRECTED 2026-07-09 (docs/re/campaign.md
    // "Rover/ghost/AI roster — CORRECTED"). sub_40151B (the real per-stage
    // starter gated dword_46489C, not sub_42288C as previously mislabelled)
    // is the actual roster/actor seeder: `for (j=0;j<ai_count;++j)
    // sub_422928()`, where sub_422928 picks a RANDOM currently-OFF slot
    // (`rand()%10`, retried up to 100 times) and flips it to COMPUTER — not
    // a sequential fill from slot 0. Only the AI COUNT (field 7) seeds
    // player slots at all; rovers/ghosts are NOT player slots (see below),
    // so folding them into COMPUTER slots (the prior port behaviour) was a
    // mislabelling, now removed. Every slot starts OFF, then exactly
    // `ai_count` distinct slots (clamped to kMaxPlayers) are flipped to
    // COMPUTER at random, matching sub_422928's `rand()%10` + retry-on-
    // occupied shape but using the presentation LCG (setup_lcg_), never
    // State::rng — this only steers which slot ids get the pre-supplied
    // roster, no sim RNG draw.
    for (int slot = 0; slot < sim::kMaxPlayers; ++slot) {
        setup_type_[slot] = 0;  // OFF (sub_421E33(i,0,0) semantics)
        setup_sub_[slot] = 0;
        setup_team_[slot] = 0;
    }
    for (int slot : seed_campaign_ai_slots(setup_lcg_, stage.ai_count)) setup_type_[slot] = 1;
    // Rovers/ghosts (fields 3-6, docs/re/campaign.md "Rover/ghost/AI
    // roster") are NOT player slots — they are autonomous roaming map-hazard
    // actors, now a real libs/sim actor kind (RoverSystem: spawn, wander AI,
    // flame death + kill-score, landing-tile player kill; hashed
    // State::rovers). start_match reads stage.rovers/rover_speed/ghosts/
    // ghost_speed straight off campaign_stages_[campaign_stage_index_] into
    // MatchConfig::campaign_rovers/etc (this function only prepares the
    // scheme/roster/banner, not the sim config, so the counts are read
    // there, not stashed here). ai_difficulty (field 8) is CONFIRMED dead
    // code (grep of the whole binary: dword_45E010's field-8 slot,
    // v20[27]/v6[27], is written once by the loader and read NOWHERE else),
    // matching the .CAM format's own "(unused at present)" comment exactly
    // — not a guess, a confirmed negative. Round pacing clauses 1/3/4/5
    // (docs/re/campaign.md "Round pacing") are now wired too: clause 1 by
    // RoverSystem::tick itself (simulation.cpp), clause 3 by run_match's
    // hazard_clear_timer edge-check, clauses 4-5 by the Results DRAW branch
    // (run_app) — see each site's own comment for the exact mechanism.

    // Stage display banner (docs/re/campaign.md "Stage banner — CONFIRMED"):
    // sub_40133F formats getstring(1235)="(%s)" with the stage's OWN name
    // (campaign record field 0, the first bytes of the 112-byte record) and
    // shows it alongside getstring(1230)="Prepare to begin Campaign!" as a
    // blocking two-line dialog at every stage transition (same sub_414340
    // dialog family present_campaign_picker's OWN confirm dialog,
    // present_campaign_confirm, now ports with the real chrome). Stored here
    // for present_setup/run_app to draw;
    // levelno (field 1) has no further consumer beyond this banner and the
    // scheme/roster application above — selected_level_ stays -1
    // (RANDOM/pick_stage) since a campaign stage supplies its own SCHEME,
    // not one of the 11 built-in level tilesets.
    campaign_banner_ = "(" + stage.name + ")";
    return true;
}

// The campaign-activation confirmation dialog (sub_4015C6, docs/re/
// campaign.md "Campaign-activation confirmation dialog") — PORTED
// 2026-07-09, replacing the accept-sting stand-in
// (formerly a coverage-audit.md crumb, now closed). Uses the SAME sub_43C734 chrome
// primitive (DialogRect/draw_dialog_chrome, above) as the quit-confirm
// dialog, sized from BOTH lines' text extents (sub_414340's own v24 =
// max(measure(top), measure(bottom)), traced from the raw disassembly at
// 0x41436c-0x4143a1: it measures LODWORD's text, then HIDWORD's, and keeps
// the wider) — width = max(that, 80)+64, height = 4*fontheight+64+2*
// fontheight (two lines).
//
// Line order/content — CONFIRMED via raw disassembly (BM95.EXE, imagebase
// 0x400000, capstone; see docs/re/campaign.md "Round pacing" provenance note
// for the same disassembly method), NOT guessed:
//   sub_4015C6 @ 0x401653-0x401669: `mov eax,0x4ba(1210); call getstring;
//   mov edx,eax; mov eax,0x5f(95); call getstring; call sub_414340` — so at
//   the call, EDX=getstring(1210)="Campaign Mode Activated!", EAX=
//   getstring(95)="NOTE!".
//   sub_414340 @ 0x414471-0x4144bb: draws the caller's EAX-sourced text
//   FIRST at the top y (fontheight+32), then the EDX-sourced text SECOND,
//   fontheight+2 further down — i.e. LODWORD/EAX is the TOP line, HIDWORD/
//   EDX is the BOTTOM line. So "NOTE!" (95) is on top, "Campaign Mode
//   Activated!" (1210) is below it — matching the SAME header-word-on-top
//   pattern the sibling error dialog uses (getstring(97)="Warning!" over
//   getstring(1215)="Campaigns not available!...", identical EAX/EDX
//   assignment at 0x4016b6-0x4016cc).
//   Both lines draw in the general white ink (byte_49D38F): the pushed
//   stack args at the sub_4172BA call sites are [byte_495390[0]=black,
//   byte_49D38F=white], and the register/ink wiring matches every other
//   sub_414340 call site already pinned in this file.
//   Position: y=(480-height)/2, x=(640-width)/2 — BOTH axes explicitly
//   computed by sub_414340 itself (0x4143fe-0x414425, against
//   dword_464A70=640/dword_464A6C=480), matching (and confirming, not just
//   approximating) the port's existing horizontal-centering convention for
//   this whole dialog family (dialog_rect/dialog_rect_vcentered, above).
//
// Dismiss behaviour — traced from sub_414340's own key loop
// (0x414510-0x414548, pseudo.c 17085-17106): every real key event plays the
// nav-blip (sub_427961(20)); only Enter(13)/Space(32)/Escape(27) close the
// dialog (v33=1 branch) — any OTHER key (arrows, letters, extended codes)
// just loops, waiting for another key. No Yes/No choice — it is a plain
// acknowledgement modal.
AppInput GameApp::present_campaign_confirm() {
    const float h = static_cast<float>(front_font_.loaded() ? front_font_.line_height() : 12);
    std::string top_line = assets_.getstring(95, "NOTE!");
    std::string bottom_line = assets_.getstring(1210, "Campaign Mode Activated!");
    float top_w = front_font_.loaded() ? static_cast<float>(front_font_.measure(top_line)) : 0.0f;
    float bottom_w =
        front_font_.loaded() ? static_cast<float>(front_font_.measure(bottom_line)) : 0.0f;
    float win_w = std::max(std::max(top_w, bottom_w), 80.0f) + 64.0f;
    float win_h = 4.0f * h + 64.0f + 2.0f * h;
    DialogRect win = dialog_rect_vcentered(win_h, win_w);
    while (true) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_EVENT_QUIT) return AppInput::Quit;
            if (ev.type != SDL_EVENT_KEY_DOWN) continue;
            const SDL_Keycode k = ev.key.key;
            audio_.play(20);  // nav blip, EVERY key (sub_427961(20))
            if (k == SDLK_RETURN || k == SDLK_KP_ENTER || k == SDLK_SPACE || k == SDLK_ESCAPE) {
                return AppInput::Advance;
            }
        }
        audio_.update_music();
        SDL_SetRenderDrawColor(sdl_renderer_.get(), 0, 0, 0, 255);
        SDL_RenderClear(sdl_renderer_.get());
        renderer_->draw_frame(sim_.state());  // NOLINT(bugprone-unchecked-optional-access) — last frame as backdrop, like the stage banner
        draw_dialog_chrome(sdl_renderer_.get(), win);
        front_font_.draw(sdl_renderer_.get(), top_line, win.x + (win.w - top_w) / 2.0f,
                         win.y + h + 32.0f, 255, 255, 255);  // byte_49D38F
        front_font_.draw(sdl_renderer_.get(), bottom_line, win.x + (win.w - bottom_w) / 2.0f,
                         win.y + h + 32.0f + h + 2.0f, 255, 255, 255);
        SDL_RenderPresent(sdl_renderer_.get());
        SDL_Delay(2);
    }
}

// The stage-start banner (sub_40133F, docs/re/campaign.md "Stage banner"):
// getstring(1235)="(%s)" formatted with the stage name, over getstring(1230)
// ="Prepare to begin Campaign!". The original's dialog (sub_414340) blocks
// for a keypress; this port additionally dwells a couple seconds so an
// unattended auto-advance (stage-clear -> next stage) doesn't stall forever.
AppInput GameApp::present_campaign_banner() {
    if (campaign_banner_.empty()) return AppInput::Advance;
    constexpr std::uint64_t kDwellMs = 2000;
    const std::uint64_t start = SDL_GetTicks();
    const std::string prepare = assets_.getstring(1230, "Prepare to begin Campaign!");
    while (true) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_EVENT_QUIT) return AppInput::Quit;
            if (ev.type == SDL_EVENT_KEY_DOWN &&
                (ev.key.key == SDLK_RETURN || ev.key.key == SDLK_KP_ENTER ||
                 ev.key.key == SDLK_SPACE || ev.key.key == SDLK_ESCAPE)) {
                audio_.play(10);  // accept sting, sub_427961(10)
                return AppInput::Advance;
            }
        }
        if (SDL_GetTicks() - start >= kDwellMs) return AppInput::Advance;
        audio_.update_music();
        SDL_SetRenderDrawColor(sdl_renderer_.get(), 0, 0, 0, 255);
        SDL_RenderClear(sdl_renderer_.get());
        renderer_->draw_frame(sim_.state());  // NOLINT(bugprone-unchecked-optional-access) — last frame as backdrop, like the help modal
        front_font_.draw(sdl_renderer_.get(), campaign_banner_, 220.0f, 200.0f, 255, 255, 255);
        front_font_.draw(sdl_renderer_.get(), prepare, 220.0f, 224.0f, 255, 220, 80);
        SDL_RenderPresent(sdl_renderer_.get());
        SDL_Delay(2);
    }
}

int GameApp::round_winner() const {
    // A round win is exactly one SIDE of survivors with the clock still
    // running; a mutual wipe-out or a time-out is a draw. Mirrors sub_42A3F6,
    // which shows DRAW when the survivor query (sub_4219B0) returns none and
    // VICTORY<idx> for the lone survivor. Team-aware via sim::winning_side
    // (docs/re/ai.md TEAM follow-up, "our semantics"): teammates count as one
    // side, so a solo match (every team byte 0) is unchanged — the returned
    // slot is still the sole survivor, just resolved through the same-side
    // rule instead of a raw single-player check.
    const sim::State& s = sim_.state();
    if (s.ticks_left == 0) return -1;  // time up -> draw
    return sim::winning_side(s);
}

bool GameApp::campaign_no_human_survivor() const {
    // sub_4016DA clauses 4-5 (docs/re/campaign.md "Round pacing"), confirmed
    // against pseudo.c 4634-4648: `for (i=0;i<10;++i) { sub_421DD2(i,&type,0);
    // if (type!=1 && type && sub_4228C4(i)) return; }` — bail (no override)
    // the instant ANY present, non-COMPUTER, ALIVE slot is found; falling
    // through the loop means every human/joystick slot is dead. type==1 is
    // COMPUTER (setup_type_'s own convention, matching sub_421DD2's "type"
    // out-param) — a live COMPUTER slot does NOT stop the fall-through. The
    // actual predicate is the SDL-free campaign_round_needs_replay
    // (results.hpp, unit-tested in test_frontend.cpp) — this wrapper just
    // gathers the three per-slot arrays it needs from sim::State/setup_type_.
    if (!campaign_active_) return false;
    const sim::State& s = sim_.state();
    std::array<bool, sim::kMaxPlayers> present{};
    std::array<bool, sim::kMaxPlayers> alive{};
    for (int i = 0; i < sim::kMaxPlayers; ++i) {
        present[i] = s.players[i].present;
        alive[i] = s.players[i].alive;
    }
    return campaign_round_needs_replay(present, alive, setup_type_);
}

bool GameApp::is_team_mode() const {
    // Team mode (docs/re/setup-screens.md dword_464964): any two ACTIVE
    // players sharing a MatchConfig team means team rows/strings apply.
    // setup_team_[] is the frontend's per-slot +84 byte; team_play_ is the
    // game-type gate (start_match zeroes every slot's team when it is off,
    // so gating on team_play_ here keeps this in lockstep with the roster
    // actually built for the match in progress).
    if (!team_play_) return false;
    const sim::State& s = sim_.state();
    std::array<bool, sim::kMaxPlayers> team_seen{};
    for (int i = 0; i < sim::kMaxPlayers; ++i) {
        if (!s.players[i].present) continue;
        int t = setup_team_[i];
        if (t < 0 || t >= sim::kMaxPlayers) continue;
        if (team_seen[t]) return true;
        team_seen[t] = true;
    }
    return false;
}

int GameApp::match_clinch() const {
    // §1 v73: the default win-count clinch, or — in team mode with
    // win_by_kills set (§1's "in team mode with win_by_kills set, the clinch
    // instead compares the highest round-kill total against ... the target,
    // breaking ties by requiring a single unique leader") — the kill-count
    // clinch via results.hpp's win_by_kills_clinch(), so both call sites
    // (run_app's Results handler and present_scoreboard) agree on whether
    // the match is over.
    const sim::State& s = sim_.state();
    if (is_team_mode() && options_.win_by_kills) {
        std::array<bool, sim::kMaxPlayers> present{};
        for (int i = 0; i < sim::kMaxPlayers; ++i) present[i] = s.players[i].present;
        return win_by_kills_clinch(kill_count_, present, win_target_);
    }
    for (int i = 0; i < sim::kMaxPlayers; ++i) {
        if (!s.players[i].present) continue;
        if (win_count_[i] >= win_target_) return i;
    }
    return -1;
}

AppInput GameApp::run_boot_attract() {
    // The boot presentation (sub_42B060 @0x42B060) — STRAIGHT-LINE, no loop.
    // The original's exact order:
    //   sub_42741E(0x3E8)         ; start the boot music (1000) FIRST of all
    //   if (!sub_413D01()) {      ; skip-logos gate
    //       show IPLOGO           ; sub_42A088(aIplogo, 1), a waited screen
    //       show HSLOGO           ; sub_42A088(aHslogo, 1), a waited screen
    //   }
    //   sub_427BFB(2800)          ; the one-shot title intro sting, before TITLE
    //   show TITLE                ; sub_42A088(aTitle, 1), a waited screen
    //   return                    ; caller enters the menu (sub_42B9CE)
    // sub_42B060 does NOT loop: each screen advances on a key OR the getvalue(12)
    // = 7 s timeout (which synthesizes Enter, 13), and after the title it simply
    // returns so the caller drops into the menu. There is NO attract re-run of
    // the logos/title. So the port is linear: present each screen; a plain
    // Advance (key accept OR the 7 s dwell) walks to the next; the title's
    // Advance returns to run_app, which enters present_menu and switches to the
    // 1010 menu music. Only Back/Quit short-circuit out.
    //
    // The boot music is started ONCE here and plays CONTINUOUSLY across the
    // logos and the title — the logos are NOT silent. We must not (re)start the
    // track per screen: start_music replaces the current track (sub_427342 frees
    // it first), so a per-screen call would restart the boot music every time.
    audio_.start_music(kBootMusicId);

    AppInput ev = present_screen(logo_screen("IPLOGO"));
    if (ev == AppInput::Quit || ev == AppInput::Back) return ev;
    ev = present_screen(logo_screen("HSLOGO"));
    if (ev == AppInput::Quit || ev == AppInput::Back) return ev;

    // The one-shot title intro sting, fired right before the title image. In the
    // binary this is sub_427BFB(2800), which is NOT a fixed clip: it picks a
    // RANDOM member of the contiguous SOUNDLST run starting at 2800 (the "ATOMIC
    // BOMBERMAN!" intro group 2800..2810 — GEN8A/GEN8B/GEN8C/… ; the file's own
    // "2899 is the last intro" comment bounds it). So each boot can voice a
    // different take. play_random_in_range picks across exactly the loaded ids in
    // that span, matching the group pick; it is a one-shot SFX voice, so it plays
    // over the still-running boot track without disturbing it.
    audio_.play_random_in_range(kTitleStingLo, kTitleStingHi);

    // The title: a normal waited screen. present_screen returns Advance on a
    // real accept OR the 7 s timeout — both fall through to the menu here,
    // faithful to sub_42B060 synthesizing Enter on timeout and returning. Back
    // (Escape) exits the app; Quit closes the window.
    ev = present_screen(title_screen());
    if (ev == AppInput::Quit || ev == AppInput::Back) return ev;
    return AppInput::Advance;  // key OR 7 s timeout -> caller enters the menu
}

AppInput GameApp::present_menu() {
    // The navigable main menu (sub_42B9CE @0x42B9CE): MAINMENU.PCX as the
    // backdrop, an up/down highlight over the item rows (wrapping), Enter
    // selects, Escape quits. On entry the original plays sub_42741E(0x3F2) once
    // (v14-gated) — the CONFIRMED menu track 1010 (0x3F2 == MENU.RSS; the RE
    // brief's 0x3FC/1020 was the round/results path sub_42A3F6, not this). This
    // switches the looping music from the boot track to the menu track and keeps
    // it playing while in the menu. Returns the AppInput the highlighted row
    // resolves to, or Quit on window close.
    audio_.start_music(kMenuMusicId);
    std::uint64_t frame = 0;
    // ATTRACT idle timer (docs/re/frontend-flow.md "Attract mode"): seeded to
    // "now" on every fresh visit to the menu (including a re-entry after an
    // attract match itself, so an unattended machine cycles demo matches
    // forever, one getvalue(92)-second gap apart — matching sub_42B9CE's
    // idle counter, which is never suppressed after firing once).
    menu_idle_since_ms_ = SDL_GetTicks();
    const std::int64_t idle_s = values_.at_or(92, kAttractIdleFallbackS);
    const bool attract_enabled = idle_s > kAttractIdleMinS;  // legend: <=5 disables attract
    // The Quit confirm overlay (sub_412987 @0x412987, PINNED 2026-07-09): Escape
    // does NOT quit directly. It selects row 6 (Quit, with the usual blip-20 +
    // accept-10 sound pair), and the row-6 dispatch calls sub_412987, which pops
    // a modal yes/no dialog — sub_41456C(getstring(10), ...) — BEFORE anything
    // exits. getstring(10) = "Are you sure you want to exit?", buttons
    // getstring(26)=" Yes "/getstring(25)=" No ". sub_41456C's own key loop
    // (pseudo.c ~17220-17270) accepts Y/y/Enter/Space as Yes (returns 1) and
    // N/n/Escape as No (returns 0) — confirmed by the raw key-code ranges
    // (0x1B/78/110 -> No; 13/32/89/121 -> Yes). Only on Yes does sub_412987 play
    // the exit sting (sub_427BFB(2600), skip-logos-gated) and Sleep(0xFA0 = 4 s)
    // before the real process exit (sub_4128C9(0)); No just closes the dialog
    // and returns to the menu with nothing else touched. The port mirrors this
    // exactly: quit_confirm gates a small modal drawn over the menu backdrop
    // (same box-plus-FontTextures convention as EditorScreen's SaveConfirm).
    bool quit_confirm = false;
    while (true) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_EVENT_QUIT) return AppInput::Quit;
            // ANY real input resets the idle clock (doc: "any input resets
            // the timer") — key, mouse button, or gamepad button, mirroring
            // the original's "any real key" blip path plus this port's own
            // mouse/pad input surfaces (sub_42B9CE only had a keyboard).
            if (ev.type == SDL_EVENT_KEY_DOWN || ev.type == SDL_EVENT_MOUSE_BUTTON_DOWN ||
                ev.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN)
                menu_idle_since_ms_ = SDL_GetTicks();
            if (ev.type != SDL_EVENT_KEY_DOWN) continue;

            if (quit_confirm) {
                // sub_41456C's key loop: any real key blips (20); Yes accepts
                // (Y/Enter/Space), No cancels (N/Escape) — every other key is
                // ignored and the dialog stays up.
                audio_.play(20);
                switch (ev.key.key) {
                    case SDLK_Y:
                    case SDLK_RETURN:
                    case SDLK_KP_ENTER:
                    case SDLK_SPACE:
                        audio_.play(10);  // accept sting, sub_41456C returning 1
                        audio_.play_random_in_range(kQuitStingLo, kQuitStingHi);  // 2600 group
                        // sub_412987 Sleep(0xFA0)s before sub_4128C9(0) exits, so the
                        // exit sting is audible rather than cut off by window teardown.
                        SDL_Delay(4000);
                        return AppInput::Quit;
                    case SDLK_N:
                    case SDLK_ESCAPE:
                        audio_.play(10);  // sub_41456C returning 0 is also a real dismiss
                        quit_confirm = false;
                        break;
                    default:
                        break;
                }
                continue;  // modal: no other input reaches the row switch below
            }

            // Hidden scheme-editor trigger (docs/re/results-and-options.md
            // §5, CONFIRMED): sub_42B9CE's input loop tracks a same-key
            // repeat counter on raw key code 5 (ASCII Ctrl+E); any OTHER key
            // resets it; `++counter > 5` fires on the 6th CONSECUTIVE press
            // (plays accept SFX 10, then calls sub_40330E -> sub_403184).
            // SDL reports Ctrl+E as SDLK_E with KMOD_CTRL set — there is no
            // menu row for this, so it is checked directly in the raw event
            // loop, ahead of (and independent of) the row-navigation switch
            // below, and does not fall through to it on a match.
            bool is_ctrl_e = (ev.key.key == SDLK_E) && (ev.key.mod & SDL_KMOD_CTRL) != 0;
            if (is_ctrl_e) {
                if (++editor_trigger_count_ > 5) {
                    editor_trigger_count_ = 0;
                    audio_.play(10);  // accept sting (SFX 10), §5
                    present_editor();
                }
                continue;  // Ctrl+E itself never falls into the row switch
            }
            editor_trigger_count_ = 0;  // any other key resets the counter

            switch (ev.key.key) {
                case SDLK_UP:
                case SDLK_W:
                    menu_index_ = (menu_index_ + kMenuCount - 1) % kMenuCount;
                    audio_.play(20);  // nav blip (SOUNDLST 20, sub_427961(20))
                    break;
                case SDLK_DOWN:
                case SDLK_S:
                    menu_index_ = (menu_index_ + 1) % kMenuCount;
                    audio_.play(20);
                    break;
                case SDLK_ESCAPE:
                    // Escape's full sound path in sub_42B9CE: the "any real key"
                    // line fires the nav blip (SFX 20) for EVERY key including 27,
                    // then Escape (27) reaches the accept branch `if (v8>=17 &&
                    // (v8<=17 || v8==27)) { sub_427961(10); v10=6; }` — the accept
                    // sting (SFX 10) — and selects row 6 = Quit. The Quit row then
                    // dispatches to sub_412987, which pops the "Are you sure you
                    // want to exit?" confirm dialog (PINNED, see quit_confirm's
                    // comment above) — it does NOT exit directly. Only a Yes in
                    // that dialog plays the 2600 exit sting and quits.
                    audio_.play(20);   // nav blip on the key (SFX 20)
                    audio_.play(10);   // accept sting selecting Quit (SFX 10)
                    menu_index_ = 6;   // v10 = 6, matches the cursor landing on Quit
                    quit_confirm = true;
                    break;
                case SDLK_RETURN:
                case SDLK_KP_ENTER:
                case SDLK_SPACE: {
                    // sub_42B9CE plays the accept sting (SFX 10, sub_427961(10))
                    // for BOTH Enter (13) and Space (32) on EVERY row — there is
                    // no "inert row" concept in the original; each row 0..6 is a
                    // live dispatch. So the accept sound fires first, always.
                    audio_.play(10);  // accept sting (SOUNDLST 10, menuexit)
                    // Row 5 = the generic help-file browser (sub_41431C, §4 —
                    // CORRECTED from the old "Roulette" label, see kMenuItems'
                    // comment above). sub_42B9CE's row switch calls it DIRECTLY
                    // (case 5: sub_41431C(); break;) with NO sub_4121FF() wipe
                    // first, unlike rows 0-2 — so unlike Credits/Options (which
                    // this port already routes through the AppState wipe), this
                    // row is handled here inline, staying on the menu loop, and
                    // never touches AppInput/next() (task brief: prefer not to
                    // add new AppInputs for this leaf).
                    if (menu_index_ == 5) {
                        if (present_help_browser() == AppInput::Quit) return AppInput::Quit;
                        break;
                    }
                    // A row we have not built yet (Editor) still plays the
                    // accept sting to stay faithful, but has no leaf to jump to, so
                    // it simply stays put instead of dead-ending on an unbuilt
                    // screen. (Documented inert stub — the accept is real, the
                    // destination is a deferred effort.)
                    if (!kMenuItems[menu_index_].live) break;
                    AppInput sel = kMenuItems[menu_index_].action;
                    // Quit selected from the menu (Enter/Space on row 6): the SAME
                    // sub_412987 dispatch Escape reaches, so it pops the SAME confirm
                    // dialog rather than quitting outright.
                    if (sel == AppInput::Quit) {
                        quit_confirm = true;
                        break;
                    }
                    // Otherwise wipe out, then hand the selection to the flow.
                    transition_->start(SDL_GetTicks());
                    while (transition_->active()) {
                        SDL_Event tev;
                        while (SDL_PollEvent(&tev))
                            if (tev.type == SDL_EVENT_QUIT) return AppInput::Quit;
                        std::uint64_t now = SDL_GetTicks();
                        transition_->update(now);
                        SDL_SetRenderDrawColor(sdl_renderer_.get(), 0, 0, 0, 255);
                        SDL_RenderClear(sdl_renderer_.get());
                        // keep the menu underneath the wipe
                        {
                            const Sprite& bg = assets_.frontend_pcx("MAINMENU");
                            if (bg.tex) {
                                SDL_FRect d{0, 0, static_cast<float>(bg.w),
                                            static_cast<float>(bg.h)};
                                SDL_RenderTexture(sdl_renderer_.get(), bg.tex, nullptr, &d);
                            }
                        }
                        transition_->draw(sdl_renderer_.get(), now);
                        SDL_RenderPresent(sdl_renderer_.get());
                        SDL_Delay(2);
                    }
                    return sel;
                }
                default:
                    break;
            }
        }

        // ATTRACT trigger (docs/re/frontend-flow.md "Attract mode"): once the
        // idle clock exceeds getvalue(92) seconds (gated > 5), fire the SAME
        // Play dispatch a real Enter-on-row-0 would — sub_42B9CE forces
        // v10=0 regardless of the highlighted row, so this returns StartMatch
        // directly rather than nudging menu_index_. roll_attract_match() does
        // the sub_4224E2 save + the roster/stage rolls; run_app's StartMatch
        // handler (game_app.cpp) checks attract_ and skips the goldman wheel/
        // setup/level screens, matching sub_410F81's short-circuit.
        // The quit-confirm dialog is modal (sub_41456C blocks sub_42B9CE's own
        // loop until answered) — hold off the attract idle trigger while it is
        // up so a demo match cannot yank the confirm away mid-decision.
        if (attract_enabled && !quit_confirm &&
            SDL_GetTicks() - menu_idle_since_ms_ >= static_cast<std::uint64_t>(idle_s) * 1000) {
            roll_attract_match();
            return AppInput::StartMatch;
        }

        audio_.update_music();
        SDL_SetRenderDrawColor(sdl_renderer_.get(), 0, 0, 0, 255);
        SDL_RenderClear(sdl_renderer_.get());
        // Backdrop.
        const Sprite& bg = assets_.frontend_pcx("MAINMENU");
        if (bg.tex) {
            SDL_FRect d{0, 0, static_cast<float>(bg.w), static_cast<float>(bg.h)};
            SDL_RenderTexture(sdl_renderer_.get(), bg.tex, nullptr, &d);
        }
        // Animated "bomb trigger green" cursor at the CONFIRMED anchor
        // (sub_42B9CE: x=getvalue(700), y=getvalue(701)+getvalue(702)*row; frame
        // = counter % statecnt). Anchor read live from VALUELST row 700's
        // columns, with this install's values as fallback. TRIGBOMB.ANI holds
        // the "bomb trigger green" sequence; if it is absent we draw a pulsing
        // highlight bar instead so the selection stays visible.
        {
            ++frame;
            int cx = static_cast<int>(values_.column_or(700, 0, kMenuCursorXFallback));
            int cy0 = static_cast<int>(values_.column_or(700, 1, kMenuCursorYFallback));
            int cstep = static_cast<int>(values_.column_or(700, 2, kMenuCursorStepFallback));
            int cy = cy0 + menu_index_ * cstep;
            Anim cur = resolve_sequence(assets_.trigbomb(-1), "bomb trigger green");
            if (!cur.steps.empty()) {
                const Sprite& sp = cur.steps[anim_step_index(frame, cur.steps.size())];
                if (sp.tex) {
                    SDL_FRect d{static_cast<float>(cx - sp.hx), static_cast<float>(cy - sp.hy),
                                static_cast<float>(sp.w), static_cast<float>(sp.h)};
                    SDL_RenderTexture(sdl_renderer_.get(), sp.tex, nullptr, &d);
                }
            } else {
                Uint8 pulse = static_cast<Uint8>(90 + 60 * ((frame / 8) % 2));
                SDL_FRect bar{static_cast<float>(cx), static_cast<float>(cy), 240.0f, 22.0f};
                SDL_SetRenderDrawBlendMode(sdl_renderer_.get(), SDL_BLENDMODE_BLEND);
                SDL_SetRenderDrawColor(sdl_renderer_.get(), 255, 220, 60, pulse);
                SDL_RenderFillRect(sdl_renderer_.get(), &bar);
            }
        }
        // The Quit confirm modal (sub_412987 -> sub_41456C, PINNED — docs/re/
        // frontend-flow.md "Escape/Quit-row confirm dialog"): the SAME
        // sub_43C734 chrome as the loading dialog, sized from the actual
        // button label extents (v29=max(textwidth,80), v30=v29+64=width,
        // v32=4*fontheight+64+fontheight=height for this one-line prompt),
        // centered on screen (both axes — see the chrome comment's X-
        // placement TODO(RE)). Prompt at y=fontheight+32 (window-relative,
        // centered), general white ink (byte_49D38F); two sub_432298 buttons
        // at the pinned y=height-32-fontheight-6, x=width/2-80 (Yes) /
        // width/2+22 (No). Behaviour (Y/Enter/Space confirm, N/Escape
        // cancel, sound path, 4s exit delay) is UNCHANGED — chrome-only pass.
        if (quit_confirm) {
            std::string prompt = assets_.getstring(10, "Are you sure you want to exit?");
            std::string yes_label = assets_.getstring(26, " Yes ");
            std::string no_label = assets_.getstring(25, " No ");
            draw_confirm_dialog(sdl_renderer_.get(), front_font_, prompt, "", yes_label, no_label);
        }
        SDL_RenderPresent(sdl_renderer_.get());
        SDL_Delay(2);
    }
}

// Attract-mode entry — sub_4224E2's save + sub_410F81's attract branch
// (docs/re/frontend-flow.md "Attract mode" point 1, pseudo.c 15125-15143).
void GameApp::roll_attract_match() {
    // sub_4224E2: snapshot the CURRENT selections before overwriting them, so
    // restore_from_attract() (sub_422552) can put them back untouched.
    attract_saved_.type = setup_type_;
    attract_saved_.sub = setup_sub_;
    attract_saved_.team = setup_team_;
    attract_saved_.level = selected_level_;
    attract_saved_.team_play = team_play_;

    attract_ = true;  // dword_464938 = 1

    // "forces team play off" (doc point 2) — a demo match is never team mode,
    // regardless of what the player had configured.
    team_play_ = false;

    // Two presentation-LCG rolls (never State::rng), advanced in order —
    // roster count then stage, matching the doc's read order (roster is
    // rolled first in sub_410F81, the level right after).
    attract_lcg_ = attract_lcg_ * 1664525u + 1013904223u;
    int computer_count = attract_computer_count(attract_lcg_ >> 16);
    fill_attract_roster(computer_count, setup_type_, setup_sub_, setup_team_);

    attract_lcg_ = attract_lcg_ * 1664525u + 1013904223u;
    int level_count = static_cast<int>(values_.column_or(35, 0, 11));  // getvalue(35)
    selected_level_ = attract_stage_pick(attract_lcg_ >> 16, level_count);

    // Campaign trigger inert during attract (task point 2): an attract match
    // never has campaign state armed (it can only be armed by present_setup's
    // 'C'x5 trigger, which the attract short-circuit never visits), so there
    // is nothing to suppress here beyond simply not touching
    // campaign_active_ — documented for the reader, not a defensive reset.
}

// Attract-mode exit — sub_422552 (doc point 3 "Menu re-entry restores
// everything"). Idempotent: a no-op if attract_ is already false, so callers
// on both the natural-end and abort paths can call it unconditionally.
void GameApp::restore_from_attract() {
    if (!attract_) return;
    setup_type_ = attract_saved_.type;
    setup_sub_ = attract_saved_.sub;
    setup_team_ = attract_saved_.team;
    selected_level_ = attract_saved_.level;
    team_play_ = attract_saved_.team_play;
    attract_ = false;  // dword_464938 = 0
}

void GameApp::reset_match_scores() {
    win_count_.fill(0);
    kill_count_.fill(0);
    // getvalue(310) "how many wins to win a match?" (first-column value, else
    // options.ini's num_to_win_match= if the VALUELST key is absent, else our
    // own fallback of 2 (task item 5 / §5: "num_to_win_match ... should seed
    // the frontend's win_target_ default"). The LEVEL & ROUNDS screen's WINS
    // row (present_map_select) still overrides on top of whichever default
    // wins here — this only affects the value shown before the player edits it.
    auto it = values_.values.find(310);
    if (it != values_.values.end())
        win_target_ = static_cast<int>(it->second);
    else
        win_target_ = num_to_win_match_.value_or(2);
    if (win_target_ < 1) win_target_ = 1;
}

// The between-round RESULTS cumulative-tally screen (sub_42A3F6 tail,
// docs/re/results-and-options.md §1): RESULTS.PCX backdrop, a header drawn
// once per round, one row per active player/team with a win-count + kill-
// count tally in per-player ink, and an outcome line reporting either "still
// need N" (match not yet clinched) or "wins the match" (clinched). Any key
// (or the 6 s idle dwell) dismisses it; the caller then starts the next round
// or, if the outcome line reports a clinch, the flow instead shows the
// VICTORY screen and never reaches this scoreboard (run_app's Results case).
AppInput GameApp::present_scoreboard() {
    const sim::State& s = sim_.state();

    // Header — getstring(30) "Game Winner was %s !", getvalue(780/781/783).
    const float hx = static_cast<float>(values_.column_or(780, 0, 150));
    const float hy = static_cast<float>(values_.column_or(780, 1, 140));
    // getvalue(783) is a colour index in the original — resolved:
    // docs/re/results-and-options.md's "screen-ink byte globals" pin.
    // byte_49D38F (general draw ink) is an offset into the shared RGB555 ->
    // palette-index LUT (byte_495390), decoding to RGB555 (31,31,31) = white;
    // verified against the install's FIELD0/5/10/MAINMENU.PCX palettes
    // (nearest entry (255,255,255), dist2=0 on all four).
    constexpr Uint8 kHeaderR = 255, kHeaderG = 255, kHeaderB = 255;

    // Per-player row — getstring(31) non-team "Player %u score: %u (kills: %d)"
    // / getstring(38) team "Team %u score: %u", getvalue(785/786/787/788).
    const float rx = static_cast<float>(values_.column_or(785, 0, 150));
    const float ry0 = static_cast<float>(values_.column_or(785, 1, 210));
    const float rystep = static_cast<float>(values_.column_or(785, 2, 20));

    // Outcome line — getvalue(800/801/803); string 120/121 "still need N" vs
    // 35/36 "wins the match" depending on team mode (§1's dword_46497C /
    // win_by_kills branch, wired below via options_.win_by_kills).
    const float ox = static_cast<float>(values_.column_or(800, 0, 150));
    const float oy = static_cast<float>(values_.column_or(800, 1, 94));

    // Team mode + the §1 v73 match-clinch check — factored into is_team_mode()
    // / match_clinch() (game_app.hpp) so run_app's Results handler (the
    // VICTORY-vs-scoreboard decision) and this render agree on the exact same
    // predicate, including the win_by_kills branch (docs/re/
    // results-and-options.md §3 row 5, now live).
    bool team_mode = is_team_mode();
    int clinched_player = match_clinch();

    // Header text (getstring(30), "Game Winner was %s !"), drawn once per
    // round on entry — the winner named is this ROUND's winner (round_winner()),
    // not necessarily the player who clinched the whole match.
    const int round_w = round_winner();
    const std::string header =
        fmt_s(assets_.getstring(30, "Game Winner was %s !"),
              round_w >= 0 ? "P" + std::to_string(round_w + 1) : std::string("-"));

    const std::uint64_t start = SDL_GetTicks();
    AppInput result = AppInput::Advance;
    bool waiting = true;
    while (waiting) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_EVENT_QUIT) return AppInput::Quit;
            if (ev.type == SDL_EVENT_KEY_DOWN) {
                audio_.play(20);  // any-key blip then accept sting (sub_42A088)
                audio_.play(10);
                result = ev.key.key == SDLK_ESCAPE ? AppInput::Back : AppInput::Advance;
                waiting = false;
            }
        }
        if (SDL_GetTicks() - start >= kResultsDwellMs) waiting = false;  // attract auto-advance
        audio_.update_music();
        SDL_SetRenderDrawColor(sdl_renderer_.get(), 0, 0, 0, 255);
        SDL_RenderClear(sdl_renderer_.get());
        const Sprite& bg = assets_.frontend_pcx("RESULTS");
        if (bg.tex) {
            SDL_FRect dst{0.0f, 0.0f, static_cast<float>(bg.w), static_cast<float>(bg.h)};
            SDL_RenderTexture(sdl_renderer_.get(), bg.tex, nullptr, &dst);
        }

        // Header, drawn once per round (this screen IS one round's worth of
        // display, so we always draw it — sub_42A3F6's "!dword_464AEC" gate is
        // about not re-drawing across frames of the SAME round, which our
        // per-round call already satisfies).
        front_font_.draw(sdl_renderer_.get(), header, hx, hy, kHeaderR, kHeaderG, kHeaderB);

        // Per-player / per-team tally rows. Non-team rows keep the slot's own
        // ink (sub_41672F -> AssetStore::slot_color, docs/re/player-colour.md).
        // Team rows use the original's fixed two-ink helper sub_4141F8
        // (@0x4141F8, `team ? byte_49D0DA : byte_49D38F`) — both inks are
        // RGB555 offsets into the byte_495390 LUT (results-and-options.md §1
        // "screen-ink byte globals"): team 0 = the general white ink
        // (31,31,31), team != 0 = red (31,10,10) -> (252,80,80) against the
        // install's shared UI palette entries.
        if (team_mode) {
            std::array<bool, sim::kMaxPlayers> team_drawn{};
            int row = 0;
            for (int i = 0; i < sim::kMaxPlayers; ++i) {
                if (!s.players[i].present) continue;
                int t = setup_team_[i];
                if (t < 0 || t >= sim::kMaxPlayers || team_drawn[t]) continue;
                team_drawn[t] = true;
                std::string line = fmt_u(assets_.getstring(38, "Team %u score: %u"),
                                          static_cast<unsigned>(t + 1));
                // getstring(38) carries one %u (team number); splice the score
                // in after it manually since fmt_u only substitutes the first.
                line += " " + std::to_string(win_count_[i]);
                const bool team1 = t != 0;  // sub_4141F8's `a1 ?` branch
                const std::uint8_t c[3] = {static_cast<std::uint8_t>(team1 ? 252 : 255),
                                           static_cast<std::uint8_t>(team1 ? 80 : 255),
                                           static_cast<std::uint8_t>(team1 ? 80 : 255)};
                front_font_.draw(sdl_renderer_.get(), line, rx,
                                 ry0 + rystep * static_cast<float>(row), c[0], c[1], c[2]);
                ++row;
            }
        } else {
            int row = 0;
            for (int i = 0; i < sim::kMaxPlayers; ++i) {
                if (!s.players[i].present) continue;
                // getstring(31) "Player %u score: %u (kills: %d)" — two
                // independent counters (§1): win_count_ (match score) and
                // kill_count_ (cumulative match kills, NOT round kills
                // despite the string's "kills" label — see kill_count_'s
                // declaration comment in game_app.hpp for the §1 citation;
                // tallied every tick from PlayerDied events, self-kills
                // excluded per our documented semantics).
                std::string line = assets_.getstring(31, "Player %u score: %u (kills: %d)");
                line = fmt_u(line, i + 1);
                // fmt_u only substitutes the FIRST specifier; splice the
                // remaining two (score, kills) in by hand so the RE'd format
                // string still reads naturally with real fallback text.
                auto splice_next = [](std::string& f, int v) {
                    auto p = f.find('%');
                    if (p == std::string::npos) return;
                    std::size_t q = p + 1;
                    while (q < f.size() && f[q] != 'u' && f[q] != 'd' && f[q] != 'i') ++q;
                    if (q < f.size()) f = f.substr(0, p) + std::to_string(v) + f.substr(q + 1);
                };
                splice_next(line, win_count_[i]);
                splice_next(line, kill_count_[i]);
                std::uint8_t c[3];
                assets_.slot_color(i, c);
                front_font_.draw(sdl_renderer_.get(), line, rx,
                                 ry0 + rystep * static_cast<float>(row), c[0], c[1], c[2]);
                ++row;
            }
        }

        // Outcome line inks — pinned (results-and-options.md §1 "screen-ink
        // byte globals"): byte_49A624 ("still playing") and byte_497F8F
        // ("match over") are RGB555 offsets into the byte_495390 LUT,
        // decoding to (20,20,20) mid-grey and (10,31,31) cyan; resolved to
        // (168,168,164) and (96,252,252) against the install's shared UI
        // palette entries (identical across FIELD0/5/10 + MAINMENU.PCX).
        {
            std::string outcome;
            std::uint8_t oc[3];
            if (clinched_player < 0) {
                // "Still needs N" reports against whichever tally the active
                // clinch mode actually compares (§1): kill_count_ under
                // win_by_kills, win_count_ otherwise — keeps this line
                // consistent with what clinched_player was decided from.
                const auto& lead_tally =
                    (team_mode && options_.win_by_kills) ? kill_count_ : win_count_;
                int needed = win_target_ - *std::max_element(lead_tally.begin(), lead_tally.end());
                if (needed < 0) needed = 0;
                std::string fmt = team_mode ? assets_.getstring(121, "Team still needs %u to win")
                                            : assets_.getstring(120, "Still need %u to win");
                outcome = fmt_u(fmt, needed);
                oc[0] = 168; oc[1] = 168; oc[2] = 164;  // byte_49A624: RGB555 (20,20,20) grey
            } else {
                if (team_mode) {
                    std::string fmt = assets_.getstring(36, "TEAM %u WINS THE MATCH!");
                    outcome = fmt_u(fmt, static_cast<unsigned>(setup_team_[clinched_player] + 1));
                } else {
                    std::string fmt = assets_.getstring(35, "%s WINS THE MATCH!");
                    outcome = fmt_s(fmt, "P" + std::to_string(clinched_player + 1));
                }
                oc[0] = 96; oc[1] = 252; oc[2] = 252;  // byte_497F8F: RGB555 (10,31,31) cyan
            }
            front_font_.draw(sdl_renderer_.get(), outcome, ox, oy, oc[0], oc[1], oc[2]);
        }

        SDL_RenderPresent(sdl_renderer_.get());
        SDL_Delay(2);
    }
    return result;
}

// A random GLUE<n> backdrop (sub_4148E5 @0x4148E5): getvalue(16) = glue count,
// rand() % count, load GLUE<n>.PCX. Both pre-match screens share it. The pick is
// a presentation LCG (setup_lcg_), never State::rng.
std::string GameApp::pick_glue() {
    setup_lcg_ = setup_lcg_ * 1664525u + 1013904223u;
    int glue_n = static_cast<int>(values_.column_or(16, 0, 7));  // getvalue(16)
    if (glue_n < 1) glue_n = 1;
    return "GLUE" + std::to_string(static_cast<int>((setup_lcg_ >> 16) %
                                                    static_cast<unsigned>(glue_n)));
}

// The Goldman Roulette wheel (docs/re/goldman-roulette.md), sub_4034BC. Run
// from run_app's Menu/StartMatch handler, BEFORE present_setup — the exact
// gate order at the head of sub_410F81 (doc §2): !attract (this port has no
// attract-mode match yet, so that leg is always true) && goldman option on
// && local game (always true, no network play) && a gold player pending
// (gold_player_ >= 0 — doc's re-entry check re-derived from sub_4034BC's own
// internal guard, "with no pending gold player the function is a silent
// no-op"). The caller (run_app) is expected to have already checked
// options_.goldman && gold_player_ >= 0 before calling this, matching the
// doc's gate order; this function itself only runs the spin/award, plus the
// Esc-abort's gold_player_ clear (doc §2 "Cleared to -1 by: Esc on the
// wheel").
AppInput GameApp::present_goldman_wheel() {
    audio_.start_music(kWinMusicId);  // 1020 inherits from the Play handler (doc §7); no new music
    const int segment_steps = static_cast<int>(values_.column_or(1004, 0, kWheelSegmentSteps));
    const int cx = static_cast<int>(values_.column_or(1000, 0, 320));
    const int cy = static_cast<int>(values_.column_or(1000, 1, 240));
    const int rx = static_cast<int>(values_.column_or(1002, 0, 200));
    const int ry = static_cast<int>(values_.column_or(1002, 1, 150));
    const int freq_x = static_cast<int>(values_.column_or(1006, 0, 1));
    const int freq_y = static_cast<int>(values_.column_or(1006, 1, 1));

    GoldmanScreen wheel(assets_, seqs_, front_font_);
    // Advance a dedicated presentation LCG seed per spin (never State::rng) —
    // same shape as setup_lcg_/panic_lcg_ elsewhere in this file.
    goldman_lcg_ = goldman_lcg_ * 1664525u + 1013904223u;
    wheel.enter(goldman_lcg_, segment_steps);

    AppInput result = AppInput::Advance;
    while (!wheel.done()) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_EVENT_QUIT) return AppInput::Quit;
            if (ev.type != SDL_EVENT_KEY_DOWN) continue;
            const SDL_Keycode k = ev.key.key;
            if (k == SDLK_F1) {
                // doc §5: F1 opens the help browser (local host); our port has
                // no separate ROULETTE.BM help text, so this reaches the same
                // OPTIONS.BM viewer the rest of the front end falls back to
                // rather than doing nothing on the key.
                AppInput help = present_bm_screen("OPTIONS");
                if (help == AppInput::Quit) return AppInput::Quit;
                continue;
            }
            wheel.on_key(k, audio_);
        }
        wheel.tick(audio_);
        audio_.update_music();
        SDL_SetRenderDrawColor(sdl_renderer_.get(), 0, 0, 0, 255);
        SDL_RenderClear(sdl_renderer_.get());
        wheel.draw(sdl_renderer_.get(), cx, cy, rx, ry, freq_x, freq_y);
        SDL_RenderPresent(sdl_renderer_.get());
        SDL_Delay(2);
    }

    if (wheel.aborted()) {
        // doc §2/§5: Esc aborts the WHOLE Play flow and forfeits the gold
        // player — the caller must skip present_setup/present_map_select and
        // return to the menu on AppInput::Back.
        gold_player_ = -1;
        return AppInput::Back;
    }
    // doc §4: the prize persists (gold_prize_) until the NEXT spin; start_match
    // re-applies it every round of the following match via born_with_extra.
    gold_prize_ = wheel.prize();
    return result;
}

// Cycle a slot's input type FORWARD one step (sub_421E80 @0x421E80): 0 off ->
// 1 computer -> 2 keyboard sub 0 -> 2 keyboard sub 1 -> 3 joystick per present
// stick -> back to 0. The pure wrap-order logic lives in cycle_slot_input_type
// (input.hpp, unit-tested); this just supplies the live connected-gamepad
// count so the cycle offers exactly the sticks in gamepads_ right now.
void GameApp::cycle_input_type(int slot) {
    cycle_slot_input_type(setup_type_[slot], setup_sub_[slot], gamepads_.count());
}

// The PLAYER INPUT TYPE SELECTION screen (sub_410F81 @0x410F81, VALUELST
// "PLAYER INPUT TYPE SELECTION" getvalue 705-713). Screen 1 of the pre-match
// flow reached from Play. A random GLUE<n> backdrop under the 1020 track
// (inherited from the Play handler sub_42A3F6 — this screen starts no music),
// header getstring(50), and the 10-slot list: each slot's input type via
// getstring(220..224), PREFIXED by getstring(51) (Player %u) and TINTED with the
// slot's intrinsic colour (VALUELST 200-247 = Tuning::color_rgb — there is no
// colour picker; colour is fixed per slot index, applied in-game via i.rmp), plus
// a team marker when the slot's team flag is set. Keys mirror the confirmed table
// (docs/re/setup-screens.md): Up/Down pick a slot, Right cycles its type
// (OFF->CPU->KBD0->KBD1->OFF), Left/'0' set it OFF, 'T' toggles its team, Enter
// goes on to the LEVEL screen, Escape cancels to the menu. Presentation only.
AppInput GameApp::present_setup() {
    audio_.start_music(kWinMusicId);  // 1020, the Play-handler track (sub_42A3F6)
    const std::string glue = pick_glue();
    // Layout (VALUELST X,Y,YS,colour -> consecutive getvalue ids): header 705,
    // list 710, joystick pane heading 715, joystick pane list 720.
    const float hx = static_cast<float>(values_.column_or(705, 0, 40));
    const float hy = static_cast<float>(values_.column_or(705, 1, 140));
    const float lx = static_cast<float>(values_.column_or(710, 0, 70));
    const float ly = static_cast<float>(values_.column_or(710, 1, 170));
    const float lys = static_cast<float>(values_.column_or(710, 2, 24));
    const float jhx = static_cast<float>(values_.column_or(715, 0, 300));
    const float jhy = static_cast<float>(values_.column_or(715, 1, 140));
    const float jlx = static_cast<float>(values_.column_or(720, 0, 320));
    const float jly = static_cast<float>(values_.column_or(720, 1, 170));
    const float jlys = static_cast<float>(values_.column_or(720, 2, 24));

    int cursor = 0;
    bool waiting = true;
    while (waiting) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_EVENT_QUIT) return AppInput::Quit;
            // Hotplug (sub_429628 "joystick present" is polled live in the
            // original; SDL3 gives us an event instead): rescan so the pane
            // and the Right-cycle's joystick count reflect what's plugged in
            // right now, without needing a restart.
            if (ev.type == SDL_EVENT_GAMEPAD_ADDED || ev.type == SDL_EVENT_GAMEPAD_REMOVED) {
                gamepads_.refresh();
                continue;
            }
            if (ev.type != SDL_EVENT_KEY_DOWN) continue;
            const SDL_Keycode k = ev.key.key;

            // Hidden campaign-mode trigger (docs/re/campaign.md §4,
            // sub_410F81 pseudo.c 15357-15365): 5 CONSECUTIVE 'C' presses
            // (any other key resets the counter — same same-key-repeat
            // pattern as present_menu's Ctrl+E x6) opens the *.cam picker.
            // Local-only in the original (sub_40C06A() guard); this port has
            // no netplay (ADR-0003), so that guard is always-true and
            // omitted. Checked BEFORE the general dispatch below so 'C'
            // itself never falls into the row-navigation switch.
            if (k == SDLK_C) {
                if (++campaign_trigger_count_ == 5) {
                    campaign_trigger_count_ = 0;
                    audio_.play(10);  // accept sting (SFX 10), mirrors the editor trigger
                    present_campaign_picker();
                }
                continue;
            }
            campaign_trigger_count_ = 0;  // any other key resets the counter

            if (k == SDLK_ESCAPE) {
                audio_.play(20);
                audio_.play(10);
                // doc §2: "Cleared to -1 by: ... Esc on the player-setup
                // screen" — cancelling the whole Play flow here also forfeits
                // any gold player pending from an earlier match.
                gold_player_ = -1;
                // Campaign quit semantics — CONFIRMED negative, docs/re/
                // campaign.md "Campaign-exit key": grepped every read/write
                // of dword_46489C in the binary; it is written in exactly
                // TWO places total (sub_4015C6's `=1` and sub_42A3F6's own
                // entry `=0`, pseudo.c 29692) — there is NO key anywhere,
                // Escape or otherwise, that explicitly clears it. The
                // original's own Escape-on-setup just aborts the current
                // sub_42A3F6 call to the menu (dword_464A68=2); dword_46489C
                // is left stale until the NEXT "Play" click resets it at
                // entry, which is behaviourally invisible (that stale value
                // is never read before being overwritten). Our explicit
                // clear here produces the identical observable outcome
                // (back at the menu, campaign not running) via an immediate
                // reset instead of an implicit one — a faithful convenience,
                // not a guess. This only fires if a *.cam pick from THIS
                // visit to present_setup hasn't been confirmed into a
                // running match yet; an in-progress campaign is abandoned
                // via run_match's own Esc/Ctrl+Q (below), which — matching
                // the original — doesn't touch campaign_active_ either;
                // it only clears on the NEXT Menu->StartMatch transition
                // (see that path's own comment).
                campaign_active_ = false;
                campaign_stages_.clear();
                campaign_stage_index_ = 0;
                return AppInput::Back;
            }
            // Enter (< 0x20 branch in sub_410F81) leaves this screen and proceeds
            // to match init / the LEVEL screen.
            if (k == SDLK_RETURN || k == SDLK_KP_ENTER) { audio_.play(10); waiting = false; break; }
            audio_.play(20);  // any real key blips first (sub_427961(20))
            if (k == SDLK_UP) cursor = (cursor + 9) % 10;               // 328
            else if (k == SDLK_DOWN) cursor = (cursor + 1) % 10;        // 336
            else if (k == SDLK_RIGHT) cycle_input_type(cursor);         // 333 sub_421E80
            else if (k == SDLK_LEFT || k == SDLK_0) {                   // 331 / '0'
                setup_type_[cursor] = 0;                                // sub_421E33(i,0,0)
                setup_sub_[cursor] = 0;
            } else if (k == SDLK_T) {                                   // 'T' team toggle (+84)
                setup_team_[cursor] = setup_team_[cursor] ? 0 : 1;
            }
        }
        audio_.update_music();
        SDL_SetRenderDrawColor(sdl_renderer_.get(), 0, 0, 0, 255);
        SDL_RenderClear(sdl_renderer_.get());
        const Sprite& bg = assets_.frontend_pcx(glue);
        if (bg.tex) {
            SDL_FRect dst{0.0f, 0.0f, static_cast<float>(bg.w), static_cast<float>(bg.h)};
            SDL_RenderTexture(sdl_renderer_.get(), bg.tex, nullptr, &dst);
        }
        front_font_.draw(sdl_renderer_.get(), assets_.getstring(50, "PLAYER SETUP"), hx, hy, 255,
                         255, 255);
        for (int i = 0; i < 10; ++i) {
            const std::string label = fmt_u(assets_.getstring(51, "Player %u - "), i + 1);
            const int t = setup_type_[i];
            std::string type;
            switch (t) {
                case 1: type = assets_.getstring(221, "COMPUTER"); break;
                case 2: type = fmt_u(assets_.getstring(222, "KEYBOARD %u"), setup_sub_[i]); break;
                case 3: type = fmt_u(assets_.getstring(223, "JOYSTICK %u"), setup_sub_[i]); break;
                case 4: type = assets_.getstring(224, "OTHER"); break;
                default: type = assets_.getstring(220, "OFF"); break;
            }
            std::string line = label + type;
            // Tint the label with the slot's authentic on-screen colour: the
            // original inks each slot line via sub_41672F(i), which quantises the
            // slot's stored RGB (the .RMP tail) to 5 bits/channel and looks it up
            // in the palette. AssetStore::slot_color reproduces that (truecolour
            // expand5 of the quantised channels) from the loaded .RMP tail, so a
            // slot reads as its real in-game colour. The selected row is nudged
            // brighter so the cursor is legible over any colour (ours; the
            // original moves a separate cursor glyph, sub_413BD6).
            //
            // CONFIRMED this label ink is NEVER the team red/white override: the
            // caller (sub_410F81, pseudo.c ~15191-15204) saves dword_464964,
            // ZEROES it, calls sub_41672F(i)/sub_416867(i) for THIS text, then
            // restores it — deliberately forcing sub_41672F's non-team branch
            // (the slot's own .RMP tail) even in Team Play. Only the SEPARATE
            // team-marker glyph appended after it (getstring(230), pseudo.c
            // ~15212-15224) is inked via sub_4141F8(team) (red/white) — see
            // below. (Team Play's red/white override IS real for the in-match
            // sprites — Renderer::render_colour, docs/re/player-colour.md "Team
            // Play colour override" — just not for this particular label ink.)
            std::uint8_t sc[3];
            assets_.slot_color(i, sc);
            const bool sel = i == cursor;
            auto boost = [sel](std::uint8_t v) {
                int x = v + (sel ? 70 : 0);
                return static_cast<Uint8>(x > 255 ? 255 : x);
            };
            float lx_end = front_font_.draw(sdl_renderer_.get(), line, lx,
                                            ly + lys * static_cast<float>(i), boost(sc[0]),
                                            boost(sc[1]), boost(sc[2]));
            if (team_play_) {
                // Team marker: getstring(230), drawn for EVERY slot whenever Team
                // Play is on (gated on the GLOBAL dword_464964, pseudo.c ~15212 —
                // NOT on this slot's own team byte, unlike our old placeholder).
                // CONFIRMED unformatted: the original never sprintf's it (no
                // sub_4518D0 call before the two back-to-back sub_4124A4(230)
                // reads at pseudo.c ~15221/15223 — the second is the raw string
                // pointer passed straight to the draw), so it carries no "%u" —
                // the COLOUR alone tells the two teams apart, via sub_4141F8(v96)
                // (v96 = sub_4223E7(i), this slot's own team byte): team byte != 0
                // -> byte_49D0DA red (252,80,80), else byte_49D38F white
                // (255,255,255) — the same red/white split as the in-match sprite
                // override (Renderer::render_colour, docs/re/player-colour.md
                // "Team Play colour override"). Drawn as its own run continuing
                // the same line (the original positions it via its own
                // getvalue(710/711/712) x/y, not literally appended text, but the
                // visual result — a coloured marker trailing the slot line — is
                // the same).
                std::string marker = "  " + assets_.getstring(230, "TEAM");
                const bool team1 = setup_team_[i] != 0;  // sub_4141F8's `a1 ?` branch
                const std::uint8_t mc[3] = {static_cast<std::uint8_t>(team1 ? 252 : 255),
                                            static_cast<std::uint8_t>(team1 ? 80 : 255),
                                            static_cast<std::uint8_t>(team1 ? 80 : 255)};
                front_font_.draw(sdl_renderer_.get(), marker, lx_end,
                                 ly + lys * static_cast<float>(i), boost(mc[0]), boost(mc[1]),
                                 boost(mc[2]));
            }
        }
        // Joystick pane (getvalue 715/720): heading msg 40, then one line per
        // detected stick (msg 41 + index, from GamepadMapper::name) or, if none
        // are connected, the single "none" line (msg 42) — sub_429628(i)'s
        // present/absent branch collapsed to "any present at all" since we
        // enumerate rather than poll per-index.
        front_font_.draw(sdl_renderer_.get(), assets_.getstring(40, "JOYSTICKS"), jhx, jhy, 255,
                         255, 255);
        const int joy_count = gamepads_.count();
        if (joy_count == 0) {
            front_font_.draw(sdl_renderer_.get(), assets_.getstring(42, "none"), jlx, jly, 150,
                             150, 150);
        } else {
            for (int j = 0; j < joy_count; ++j) {
                std::string jline =
                    fmt_u(assets_.getstring(41, "JOYSTICK %u"), j) + " " + gamepads_.name(j);
                front_font_.draw(sdl_renderer_.get(), jline, jlx,
                                 jly + jlys * static_cast<float>(j), 200, 200, 200);
            }
        }
        front_font_.draw(sdl_renderer_.get(),
                         "UP/DN PICK  RIGHT CYCLE  0 OFF  T TEAM  ENTER NEXT", lx,
                         ly + lys * 11.0f, 150, 150, 150);
        SDL_RenderPresent(sdl_renderer_.get());
        SDL_Delay(2);
    }
    return AppInput::Advance;
}

// The 11 built-in level names (VALUELST 450-460 / getvalue(150+n)); RANDOM is
// getstring(149). These GENERIC fallbacks are ours — the real names live in the
// user's MESSAGES.TXT and load at runtime via getstring, never committed.
const char* GameApp::level_fallback(int idx) {
    static const char* kNames[] = {
        "NEW TRADITIONALIST",  "CLASSIC GREEN ACRES", "HOCKEY RINK",   "ANCIENT EGYPT",
        "COAL MINE",           "BEACH",               "ALIENS",        "HAUNTED HOUSE",
        "UNDER THE OCEAN",     "DEEP FOREST GREEN",   "INNER CITY TRASH"};
    return (idx >= 0 && idx < static_cast<int>(std::size(kNames))) ? kNames[idx] : "LEVEL";
}

// The LEVEL & ROUNDS screen (sub_406DDE @0x406DDE, the VALUELST "OPTIONS SCREEN"
// getvalue 730/735). Screen 2 of the pre-match flow. A 2-row list on a random
// GLUE<n> backdrop (1020 track inherited): row 0 = LEVEL (-1 RANDOM else 0..10 of
// getvalue(35)=11 built-ins, named getstring(150+n) / getstring(149)); row 1 =
// NUMBER OF WINS (1..100). Left/Right cycle the highlighted row's value (level
// wraps [-1 .. 10]; wins +-1 or +-5 on PgUp/PgDn), Up/Down switch rows. Enter
// commits the level (selected_level_ -> dword_464998) and win target (win_target_
// -> dword_464A7C) and starts; Escape backs to the player screen. Presentation
// only — the committed level drives start_match's stage choice.
AppInput GameApp::present_map_select() {
    const std::string glue = pick_glue();
    const int level_count = static_cast<int>(values_.column_or(35, 0, 11));  // getvalue(35)
    const float lx = static_cast<float>(values_.column_or(735, 0, 55));
    const float ly = static_cast<float>(values_.column_or(735, 1, 170));
    const float lys = static_cast<float>(values_.column_or(735, 2, 24));

    // Sample-block preview geometry (docs/re/setup-screens.md "The sample
    // block preview", sub_406AA3, VALUELST 730-733): X,Y = grid origin,
    // XSize/YSize = grid size IN CELLS (5x5). Cell pitch is the same 40x36
    // the in-match renderer uses (sim::kTileW/kTileH), 1:1, no stretching.
    const int px = static_cast<int>(values_.column_or(730, 0, 400));
    const int py = static_cast<int>(values_.column_or(730, 1, 100));
    const int pxsize = static_cast<int>(values_.column_or(730, 2, 5));
    const int pysize = static_cast<int>(values_.column_or(730, 3, 5));

    int row = 0;  // 0 = level, 1 = wins (v34 = 2 rows in sub_406DDE)
    // The sample-block pattern (which cells are blank/solid/brick, and which
    // level's tile art each drawn cell uses) is re-rolled only on screen
    // entry and on a LEVEL row change (sub_406AA3's v35 re-arm), NEVER every
    // frame — pinned in the doc above. -2 is a sentinel forcing the first
    // roll below.
    int pattern_level = -2;
    // tile_of[row][col]: the stage index whose "tile <n> solid/brick" art
    // that cell draws, or -1 for a blank cell. Solid/brick-ness itself is
    // re-derived below from the (j&1,i&1) parity rule, which is pure
    // geometry and does not need re-rolling.
    std::vector<std::vector<int>> tile_of(static_cast<std::size_t>(pysize),
                                          std::vector<int>(static_cast<std::size_t>(pxsize), -1));
    int field_stage = -1;  // the field-swatch stage picked alongside tile_of

    bool waiting = true;
    while (waiting) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_EVENT_QUIT) return AppInput::Quit;
            if (ev.type != SDL_EVENT_KEY_DOWN) continue;
            const SDL_Keycode k = ev.key.key;
            if (k == SDLK_ESCAPE) { audio_.play(20); return AppInput::Back; }  // back to setup
            if (k == SDLK_RETURN || k == SDLK_KP_ENTER) {  // commit + start (LABEL_101)
                audio_.play(10);
                waiting = false;
                break;
            }
            audio_.play(20);
            if (k == SDLK_UP || k == SDLK_DOWN) row = (row + 1) % 2;  // 2 rows: either arrow toggles
            else if (row == 0 && k == SDLK_LEFT) {         // --level, wrap below -1
                if (--selected_level_ < -1) selected_level_ = level_count - 1;
            } else if (row == 0 && k == SDLK_RIGHT) {      // ++level, wrap above count-1 to -1
                if (++selected_level_ >= level_count) selected_level_ = -1;
            } else if (row == 1 && (k == SDLK_LEFT)) {     // wins -1
                if (--win_target_ < 1) win_target_ = 1;
            } else if (row == 1 && (k == SDLK_RIGHT)) {    // wins +1
                if (++win_target_ > 100) win_target_ = 100;
            } else if (row == 1 && k == SDLK_PAGEUP) {     // wins +5 (sub_406DDE 0x174)
                win_target_ += 5;
                if (win_target_ > 100) win_target_ = 100;
            } else if (row == 1 && k == SDLK_PAGEDOWN) {   // wins -5 (371)
                win_target_ -= 5;
                if (win_target_ < 1) win_target_ = 1;
            }
        }
        // Re-roll the sample-block pattern on entry and whenever the LEVEL row
        // changes (sub_406AA3's v35 re-arm) — never every frame.
        if (selected_level_ != pattern_level) {
            pattern_level = selected_level_;
            int max_n = level_count > 1 ? level_count : 1;
            for (int i = 0; i < pysize; ++i) {
                for (int j = 0; j < pxsize; ++j) {
                    bool solid_cell = (j & 1) != 0 && (i & 1) != 0;
                    bool brick_cell = false;
                    if (!solid_cell && (j > 1 || i > 1)) {
                        setup_lcg_ = setup_lcg_ * 1664525u + 1013904223u;
                        brick_cell = (setup_lcg_ >> 16) % 5 != 0;  // rand()%5 != 0
                    }
                    if (!solid_cell && !brick_cell) {
                        tile_of[static_cast<std::size_t>(i)][static_cast<std::size_t>(j)] = -1;
                        continue;
                    }
                    int n = selected_level_;
                    if (n < 0) {  // RANDOM: re-pick per cell (pinned quirk)
                        setup_lcg_ = setup_lcg_ * 1664525u + 1013904223u;
                        n = static_cast<int>((setup_lcg_ >> 16) % static_cast<unsigned>(max_n));
                    }
                    tile_of[static_cast<std::size_t>(i)][static_cast<std::size_t>(j)] = n;
                }
            }
            field_stage = selected_level_;
            if (field_stage < 0) {
                setup_lcg_ = setup_lcg_ * 1664525u + 1013904223u;
                field_stage = static_cast<int>((setup_lcg_ >> 16) % static_cast<unsigned>(max_n));
            }
        }

        audio_.update_music();
        SDL_SetRenderDrawColor(sdl_renderer_.get(), 0, 0, 0, 255);
        SDL_RenderClear(sdl_renderer_.get());
        const Sprite& bg = assets_.frontend_pcx(glue);
        if (bg.tex) {
            SDL_FRect dst{0.0f, 0.0f, static_cast<float>(bg.w), static_cast<float>(bg.h)};
            SDL_RenderTexture(sdl_renderer_.get(), bg.tex, nullptr, &dst);
        }
        // Sample-block preview panel (sub_406AA3, docs/re/setup-screens.md):
        // a bordered box around a stretched field-swatch backdrop, then the
        // 5x5 solid/brick grid drawn at native 40x36 cell size on top.
        {
            const float bx = static_cast<float>(px - 22);
            const float by = static_cast<float>(py - 20);
            const float bw = static_cast<float>(pxsize * sim::kTileW + 24);
            const float bh = static_cast<float>(pysize * sim::kTileH + 22);
            SDL_SetRenderDrawColor(sdl_renderer_.get(), 40, 40, 60, 255);
            SDL_FRect border{bx, by, bw, bh};
            SDL_RenderFillRect(sdl_renderer_.get(), &border);
            if (field_stage >= 0) {
                const AssetStore::StagePreview& fprev = assets_.stage_preview(field_stage);
                if (fprev.field) {
                    SDL_FRect panel{static_cast<float>(px - 20), static_cast<float>(py - 18),
                                    static_cast<float>(pxsize * sim::kTileW + 20),
                                    static_cast<float>(pysize * sim::kTileH + 18)};
                    SDL_RenderTexture(sdl_renderer_.get(), fprev.field, nullptr, &panel);
                }
            }
            for (int i = 0; i < pysize; ++i) {
                for (int j = 0; j < pxsize; ++j) {
                    int n = tile_of[static_cast<std::size_t>(i)][static_cast<std::size_t>(j)];
                    if (n < 0) continue;
                    const AssetStore::StagePreview& prev = assets_.stage_preview(n);
                    bool solid_cell = (j & 1) != 0 && (i & 1) != 0;
                    const Anim& a = solid_cell ? prev.solid : prev.brick;
                    if (a.steps.empty()) continue;
                    const Sprite& sp = a.steps[0];
                    if (!sp.tex) continue;
                    SDL_FRect cell{static_cast<float>(px + j * sim::kTileW),
                                  static_cast<float>(py + i * sim::kTileH),
                                  static_cast<float>(sim::kTileW), static_cast<float>(sim::kTileH)};
                    SDL_RenderTexture(sdl_renderer_.get(), sp.tex, nullptr, &cell);
                }
            }
        }
        // Row 0: LEVEL. getstring(210) is the level-line format (%s = name);
        // name = getstring(150+n) for a specific level, getstring(149) for RANDOM.
        const std::string level_name = selected_level_ < 0
            ? assets_.getstring(149, "RANDOM")
            : assets_.getstring(150 + selected_level_, level_fallback(selected_level_));
        const std::string level_line = fmt_s(assets_.getstring(210, "LEVEL: %s"), level_name);
        front_font_.draw(sdl_renderer_.get(), level_line, lx, ly, row == 0 ? 255 : 180,
                         row == 0 ? 220 : 180, row == 0 ? 60 : 180);
        // Row 1: NUMBER OF WINS. getstring(211) is the rounds-line format (%u).
        const std::string wins_line = fmt_u(assets_.getstring(211, "WINS TO WIN: %u"), win_target_);
        front_font_.draw(sdl_renderer_.get(), wins_line, lx, ly + lys, row == 1 ? 255 : 180,
                         row == 1 ? 220 : 180, row == 1 ? 60 : 180);
        front_font_.draw(sdl_renderer_.get(), "UP/DN ROW  LEFT/RIGHT CHANGE  ENTER START", lx,
                         ly + lys * 3.0f, 150, 150, 150);
        SDL_RenderPresent(sdl_renderer_.get());
        SDL_Delay(2);
    }
    return AppInput::Advance;
}

// Assembles one tick's TickInputs across every roster slot (docs/re/setup-
// screens.md: OFF/COMPUTER slots contribute neutral input — AISystem drives
// COMPUTER from Player::ai, OFF is simply absent — KEYBOARD slots read the
// shared KeyboardMapper's player 0/1 half by sub-index, JOYSTICK slots read
// GamepadMapper::read(sub). A disconnected pad (index now out of range, or
// still indexed but closed) falls through GamepadMapper::read's own
// out-of-range/null guard to neutral input, so a mid-match unplug degrades
// gracefully instead of crashing or freezing that slot's last input.
sim::TickInputs GameApp::collect_inputs() const {
    sim::TickInputs in;
    const sim::TickInputs kb = keyboard_.read();
    for (int i = 0; i < sim::kMaxPlayers; ++i) {
        switch (static_cast<SlotInputType>(setup_type_[i])) {
            case SlotInputType::Keyboard:
                in.players[i] = kb.players[setup_sub_[i] == 0 ? 0 : 1];
                break;
            case SlotInputType::Joystick:
                in.players[i] = gamepads_.read(setup_sub_[i]);
                break;
            default:
                break;  // Off/Computer/Other: neutral — AI or absence owns the slot
        }
    }
    return in;
}

// docs/re/in-match-shell.md "The player row" — CONFIRMED, pixel-exact
// against the VALUELST file's own comments (ids 113/114 = "two vertical (Y)
// coordinates of each player row across the top", 115-119 = "left (X)
// coordinates of each player column across the top"). Message 37 = "S:%d
// K:%d" (MESSAGES.TXT); the two values are sub_421AC8(i) (win_count_, the
// SAME field the RESULTS screen's "score" already uses) and sub_421B0F(i)
// (kill_count_, ditto "kills") — sub_420F07's own two accessors, already
// wired to these exact members for the RESULTS screen (present_scoreboard,
// docs/re/results-and-options.md §1). No panel/background art backs this
// row (no draw call site found behind it in sub_420F07) — a bare overlay
// directly on the live field, ported the same way.
void GameApp::draw_player_row(const sim::State& s) {
    auto splice_next = [](std::string& f, int v) {
        auto p = f.find('%');
        if (p == std::string::npos) return;
        std::size_t q = p + 1;
        while (q < f.size() && f[q] != 'u' && f[q] != 'd' && f[q] != 'i') ++q;
        if (q < f.size()) f = f.substr(0, p) + std::to_string(v) + f.substr(q + 1);
    };
    for (int i = 0; i < sim::kMaxPlayers; ++i) {
        // byte_461BD4 (+0x10, "alive/on-screen" per facts.md's "Player struct"
        // entry) gates the whole row entry -> sim::Player::present, which is
        // set once at match setup and stays true for the rest of the match
        // regardless of round elimination (unlike `alive`, checked below).
        if (!s.players[i].present) continue;
        int col = i / 2;   // getvalue(115 + i/2): 5 columns, VALUELST 10/110/210/310/410
        int row = i & 1;   // getvalue(113 + i&1): 2 rows, VALUELST 6/26
        float x = static_cast<float>(values_.column_or(115 + col, 0, 10 + 100 * col));
        float y = static_cast<float>(values_.column_or(113 + row, 0, 6 + 20 * row));

        std::string line = assets_.getstring(37, "S:%d K:%d");
        splice_next(line, win_count_[i]);
        splice_next(line, kill_count_[i]);
        std::uint8_t c[3];
        assets_.slot_color(i, c);
        front_font_.draw(sdl_renderer_.get(), line, x, y, c[0], c[1], c[2]);

        // dword_461BC4 (+0x00, "active/moving state") gates the "xxx" overlay
        // -> sim::Player::alive, the per-ROUND flag (reset every round,
        // unlike `present` above) — a player dead THIS round still keeps
        // their score visible underneath the marker.
        if (!s.players[i].alive && !seqs_.eliminated_marker.steps.empty()) {
            const Sprite& sp = seqs_.eliminated_marker.steps[0];
            if (sp.tex) {
                SDL_FRect dst{x - static_cast<float>(sp.hx), y - static_cast<float>(sp.hy),
                              static_cast<float>(sp.w), static_cast<float>(sp.h)};
                SDL_RenderTexture(sdl_renderer_.get(), sp.tex, nullptr, &dst);
            }
        }
    }
}

AppInput GameApp::run_match() {
    start_match(next_seed_++);
    const std::uint64_t tick_ms = 1000 / sim::kTicksPerSecond;
    std::uint64_t last = SDL_GetTicks();
    std::uint64_t acc = 0;
    int over_ticks = -1;
    while (true) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_EVENT_QUIT) return AppInput::Quit;
            // ATTRACT abort (docs/re/frontend-flow.md "Attract mode" point 3,
            // mirroring sub_42A3F6's round-loop tail `if (dword_464938) goto
            // LABEL_34` on a keypress): ANY key, mouse button, or gamepad
            // button input during an attract demo returns to the menu
            // IMMEDIATELY — checked first, ahead of the specific-key
            // handling below, and only while attract_ is armed (a real match
            // never takes this branch, so a human round's own key bindings
            // are unaffected). run_app's StartMatch handler calls
            // restore_from_attract() unconditionally once this returns,
            // whether the round ended naturally or was aborted here — an
            // attract match never shows Results either way (point 2).
            if (attract_ && (ev.type == SDL_EVENT_KEY_DOWN ||
                             ev.type == SDL_EVENT_MOUSE_BUTTON_DOWN ||
                             ev.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN))
                return AppInput::MatchOver;
            // Ctrl+Q = CONFIRMED instant, unconfirmed-dialog forfeit
            // (docs/re/in-match-shell.md "Esc negative finding": raw key 0x11
            // = 17 = Ctrl+Q is the ONLY key that aborts a round mid-match in
            // the original — dword_46492C=-1/dword_464A68=2, no confirm
            // prompt, straight to the standard teardown). Wired here as the
            // faithful key.
            if (ev.type == SDL_EVENT_KEY_DOWN && ev.key.key == SDLK_Q &&
                (ev.key.mod & SDL_KMOD_CTRL) != 0)
                return AppInput::MatchOver;
            // Esc also bails to the menu — a PORT CONVENIENCE, not a binary
            // fact: the same doc's finding is that literal Esc (27) is INERT
            // mid-round in the original (falls through the round loop's key
            // chain untouched; only Ctrl+Q aborts). We keep this binding
            // anyway because it gives players a familiar "quit to menu" key,
            // functionally standing in for the original's Ctrl+Q rather than
            // matching its own (inert) Esc — see the doc's "Port status"
            // paragraph for the full rationale.
            if (ev.type == SDL_EVENT_KEY_DOWN && ev.key.key == SDLK_ESCAPE)
                return AppInput::MatchOver;
            // docs/re/in-match-shell.md §1, auxiliary key table row
            // 0x13B=315=F1 (local only, matching `!sub_40C06A()` — no
            // network gate needed here since this port has no network play):
            // opens the SAME generic *.BM help browser row 5 opens
            // (sub_41431C -> sub_414235, §4) without leaving the round,
            // bracketed by the tick-suspend guard sub_42A16F(1)/(0)
            // (pseudo.c 29769-29771) — "the whole game freezes under the
            // help overlay: sim, rendering, HUD, everything" while it is up.
            if (ev.type == SDL_EVENT_KEY_DOWN && ev.key.key == SDLK_F1) {
                // sub_42A16F(1): suspend the tick callback. present_help_
                // browser_modal draws the LAST rendered match frame as its
                // backdrop (never advancing the sim) and returns once the
                // browser is dismissed.
                AppInput help = present_help_browser_modal();
                if (help == AppInput::Quit) return AppInput::Quit;
                // sub_42A16F(0): resume. Reset the accumulator/clock instead
                // of reproducing the original's documented bug (the round
                // clock silently absorbs the whole modal duration in one
                // lump, §1's "Pause negative finding" point 2) — a
                // DELIBERATE deviation, per this task's brief, so closing
                // the browser does not fire a tick burst or eat round time.
                last = SDL_GetTicks();
                acc = 0;
                continue;
            }
            // A pad unplugged/plugged mid-match: rescan so a disconnect drops
            // that slot to neutral input (via collect_inputs' range check)
            // rather than leaving it wedged, and a reconnect resumes control at
            // its old index without needing a trip back to the setup screen.
            if (ev.type == SDL_EVENT_GAMEPAD_ADDED || ev.type == SDL_EVENT_GAMEPAD_REMOVED)
                gamepads_.refresh();
        }

        std::uint64_t now = SDL_GetTicks();
        acc += now - last;
        last = now;
        while (acc >= tick_ms) {
            acc -= tick_ms;
            sim_.tick(collect_inputs());
            sounds_.on_tick(sim_.state());
            renderer_->on_events(sim_.state());  // NOLINT(bugprone-unchecked-optional-access)
            // §1's kill tally (sub_421B0F): a GameApp-side pass over this
            // tick's events, separate from the renderer's own on_events walk
            // (renderer_ never mutates GameApp state — CLAUDE.md's libs/game
            // boundary). Cumulative for the whole match (see kill_count_'s
            // doc comment); reset only in reset_match_scores().
            tally_kills(sim_.state().events, kill_count_);

            const sim::State& s = sim_.state();
            // Campaign hazard-clear grace timer (docs/re/campaign.md "Round
            // pacing" clause 3, sub_4016DA's dword_4646C0): once every
            // rover/ghost has been dead for kHazardClearTicks ticks, the
            // ORIGINAL flags "stage clear, pending" — reached even if a
            // human survivor is ALSO already about to end the round the
            // normal way (clause 2 below), so this is an independent, not
            // additional, early-out. Edge-detected on the STATE field itself
            // (RoverSystem is a private stack object of simulation.cpp) —
            // fires exactly once, the tick the timer reaches the threshold.
            if (over_ticks < 0 && campaign_active_ &&
                s.hazard_clear_timer == sim::kHazardClearTicks) {
                over_ticks = 3 * sim::kTicksPerSecond;
            }
            // Team-aware round-over: "one SIDE left", not "one player left"
            // (docs/re/ai.md TEAM follow-up). sides_remaining() degenerates to
            // alive_count() when every team byte is 0 (the default), so a solo
            // match's timing is unchanged.
            if (over_ticks < 0 && (sim::sides_remaining(s) <= 1 || s.ticks_left == 0)) {
                // Linger a few seconds on the final frame, then hand back to the
                // flow so the Results screen can come up.
                over_ticks = 3 * sim::kTicksPerSecond;
                if (s.ticks_left == 0) {
                    std::printf("time up — draw!\n");
                } else {
                    // The "we have a winner" voice group (2000) fires under the
                    // RESULTS scoreboard itself once v73 is computed (§1), NOT
                    // here during the match's own end-of-round linger — moved to
                    // run_app's Results handler (present_scoreboard/victory_screen
                    // call site) so it plays under the right screen.
                    for (int i = 0; i < sim::kMaxPlayers; ++i)
                        if (s.players[i].present && s.players[i].alive)
                            std::printf("player %d wins!\n", i);
                }
            }
            if (over_ticks > 0 && --over_ticks == 0) return AppInput::MatchOver;
        }

        audio_.update_music();
        // Gold Bomberman twinkle (docs/re/goldman-roulette.md §6): tell the
        // renderer which player/team is the pending gold winner every frame —
        // gold_player_ only changes between rounds, but this is a cheap int
        // pair and keeps the renderer decoupled from GameApp's own state.
        renderer_->set_gold_player(gold_player_, is_team_mode());  // NOLINT(bugprone-unchecked-optional-access)
        renderer_->draw_frame(sim_.state());  // NOLINT(bugprone-unchecked-optional-access)
        // The player-row HUD strip (docs/re/in-match-shell.md "The player
        // row") needs GameApp's own win_count_/kill_count_/front_font_, none
        // of which Renderer owns — drawn as a GameApp-side overlay on top of
        // Renderer's frame, same layering the original has (sub_420F07 draws
        // it every tick, after the field/world but the clock/hurry HUD is
        // logically part of the same pass).
        draw_player_row(sim_.state());
        SDL_RenderPresent(sdl_renderer_.get());
        SDL_Delay(2);
    }
}

int GameApp::run_app() {
    // The front-end shell: the pure flow graph (app_flow.hpp) decides where each
    // event leads; these per-state handlers are the SDL side. A dev fast-path
    // can start in Match and return to the menu when it ends.
    //
    // Two states own their own multi-screen loop rather than a single
    // present_screen: Boot runs the linear IPLOGO->HSLOGO->TITLE chain
    // (run_boot_attract; no attract re-run — the title's timeout falls through
    // to the menu), and Menu runs the navigable menu (present_menu), which
    // resolves the highlighted row into a concrete AppInput. Everything else is
    // one asset-driven Screen.
    AppState state = opts_.boot_match ? AppState::Match : AppState::Boot;
    while (!is_terminal(state)) {
        AppInput ev = AppInput::Advance;
        switch (state) {
            case AppState::Boot:
                // The whole LINEAR boot presentation (IPLOGO -> HSLOGO -> TITLE,
                // sub_42B060). Returns Advance on the title's key/7 s timeout (no
                // attract re-run — the timeout falls through to the menu) or
                // Back/Quit; next() forwards Advance into Logo, and the Logo/Title
                // cases below are pass-throughs on to the menu (this one handler
                // already drew the whole chain).
                ev = run_boot_attract();
                break;
            case AppState::Logo:
            case AppState::Title:
                // Pass-throughs: run_boot_attract already presented the logos
                // and title, so nothing is drawn here — just walk the flow graph
                // on to the menu (keeps next() authoritative over the boot hops).
                ev = AppInput::Advance;
                break;
            case AppState::Menu: {
                ev = present_menu();  // navigable; resolves the selected row
                if (ev == AppInput::StartMatch) {
                    // Campaign-flag reset — PORTED 2026-07-09 (docs/re/
                    // campaign.md "Campaign-exit key"): `dword_46489C = 0` is
                    // the LITERAL FIRST statement of sub_42A3F6 (pseudo.c
                    // 29692), unconditionally, every time "Play" is entered
                    // fresh from the menu — there is no dedicated exit KEY
                    // anywhere in the binary (grepped every read/write of
                    // dword_46489C: exactly two writes total, this entry
                    // reset and sub_4015C6's own `=1`), but this unconditional
                    // entry-point reset IS the mechanism that keeps a
                    // previous campaign run from leaking into a fresh one.
                    // Without it, aborting mid-campaign (Ctrl+Q/Esc during
                    // Match -> Results -> Back to Menu, none of which clear
                    // campaign_active_) would leave a stale campaign_active_/
                    // campaign_stages_/campaign_stage_index_ armed for the
                    // NEXT Play, silently skipping present_map_select() and
                    // resuming the abandoned stage instead of a normal game —
                    // matching this reset closes that gap. Placed first, same
                    // as the original's ordering, before the attract/goldman/
                    // present_setup steps below (present_setup's OWN 'C'x5
                    // trigger can re-arm it later in this same StartMatch
                    // pass, exactly like sub_410F81 re-arming dword_46489C
                    // after sub_42A3F6's entry reset).
                    campaign_active_ = false;
                    campaign_stages_.clear();
                    campaign_stage_index_ = 0;
                    // The pre-match flow reached from Play (sub_42A3F6): the PLAYER
                    // INPUT screen (sub_410F81) then the LEVEL & ROUNDS screen
                    // (sub_406DDE), then the match. Escape backs up ONE step at
                    // each screen: cancel on the level screen -> back to the player
                    // screen; cancel on the player screen -> back to the menu.
                    // reset_match_scores() clears the tally; the level screen owns
                    // the win target so we reset FIRST, then let the level screen
                    // adjust win_target_.
                    reset_match_scores();
                    // ATTRACT short-circuit (docs/re/frontend-flow.md
                    // "Attract mode" point 1, sub_410F81 pseudo.c 15125-15143):
                    // present_menu() already rolled the demo roster/stage into
                    // setup_type_/setup_sub_/setup_team_/selected_level_ (via
                    // roll_attract_match()) and set attract_ before returning
                    // StartMatch here, so this path goes STRAIGHT to the
                    // match — no goldman wheel (doc §2's `!dword_464938`
                    // gate — a real pending prize is simply left untouched
                    // for the NEXT non-attract Play entry, not forfeited),
                    // no present_setup, no present_map_select ("neither the
                    // player screen nor the LEVEL & ROUNDS screen is shown").
                    // run_match's own attract_ check aborts back to the menu
                    // on any input; either way (natural end or abort) run_match
                    // returns MatchOver and this block calls run_match()
                    // DIRECTLY (bypassing the normal Match/Results AppState
                    // walk) so Results never renders — point 2's "Round end
                    // skips ALL outcome screens" (DRAW/RESULTS/VICTORY).
                    if (attract_) {
                        AppInput attractResult = run_match();
                        if (attractResult == AppInput::Quit) return 0;
                        // Doc point 2: an attract round NEVER shows DRAW/
                        // RESULTS/VICTORY — restore immediately and drop
                        // straight back to the menu, bypassing next()'s
                        // normal Match->Results->Menu walk entirely (there is
                        // no scoreboard/outcome state to fold into the flow
                        // graph for this round).
                        restore_from_attract();
                        ev = AppInput::Advance;  // Menu -> (stays) Menu
                        break;
                    }
                    // The Goldman wheel (docs/re/goldman-roulette.md §2): at
                    // the head of every Play entry, before present_setup —
                    // gated on not-attract (checked above — this is the
                    // normal, non-attract path), the goldman option, local-
                    // only (always true), and a gold player actually pending
                    // from a previous match's rounds. An Esc abort forfeits
                    // the whole Play flow (skip straight back to the menu,
                    // mirroring sub_410F81's post-call `if (dword_464A68)
                    // return`).
                    if (options_.goldman && gold_player_ >= 0) {
                        AppInput wheelResult = present_goldman_wheel();
                        if (wheelResult == AppInput::Quit) return 0;
                        if (wheelResult == AppInput::Back) { ev = AppInput::Advance; break; }
                    }
                    bool started = false;
                    while (!started) {
                        AppInput setup = present_setup();
                        if (setup == AppInput::Quit) return 0;
                        if (setup == AppInput::Back) { ev = AppInput::Advance; break; }
                        // Campaign mode SKIPS the LEVEL & ROUNDS screen
                        // entirely (docs/re/campaign.md "Skips the normal
                        // LEVEL & ROUNDS screen", sub_406DDE's `if
                        // (!dword_46489C)` gate): present_setup's own 'C'x5
                        // trigger already picked a stage and seeded the
                        // roster (present_campaign_picker), so a confirmed
                        // setup screen goes STRAIGHT to the match.
                        if (campaign_active_) { started = true; break; }
                        // Player screen accepted -> the LEVEL screen.
                        AppInput lvl = present_map_select();
                        if (lvl == AppInput::Quit) return 0;
                        if (lvl == AppInput::Back) continue;  // back to the player screen
                        started = true;  // both screens confirmed -> start the match
                    }
                    if (!started) ev = AppInput::Advance;  // cancelled all the way out
                }
                break;
            }
            case AppState::Match:
                ev = run_match();
                break;
            case AppState::Results: {
                // The three-tier sub_42A3F6 results tail (docs/re/frontend-flow.md
                // "results flow"): a round win bumps that player's tally; the
                // match is decided (VICTORY<n>) once the tally reaches
                // win_target_ (the LEVEL & ROUNDS screen's WINS row); otherwise
                // a survivor shows the RESULTS cumulative scoreboard, a draw
                // (round_winner() folds no-survivor and time-up) shows DRAW —
                // and both replay the next round. The winner voice group (2000)
                // fires here, as soon as v73 (the round winner / match-over
                // check) is computed (§1) — i.e. under BOTH the scoreboard and
                // the VICTORY screen, not only the latter. On a DRAW we fire the
                // tie-game sting instead (sub_427BFB(1700)) — a one-shot group
                // pick, not looped music.
                //
                // Results MUSIC (sub_42A3F6) — CORRECTED per docs/re/
                // in-match-shell.md §2: sub_42741E(0x46A)=1130 ("draw") starts
                // UNCONDITIONALLY on round-loop exit, BEFORE the survivor
                // test — so DRAW, the RESULTS tally, AND VICTORY/TEAM all
                // play under 1130; 1020 ("win") is the SETUP-SCREENS track
                // (present_setup/present_goldman_wheel), never restarted
                // anywhere in this outcome tier. This file previously read
                // 1020 as VICTORY's own track (frontend-flow.md's original,
                // now-corrected "Results MUSIC" paragraph) — fixed here to
                // kDrawMusicId in every branch below. start_music replaces
                // the leftover stage/menu track, so the results screen
                // carries its own backdrop music — previously our DRAW/
                // RESULTS/VICTORY screens played under whatever music was
                // left running, a silent-vs-original gap now closed.
                int w = round_winner();
                // Round-pacing clauses 4-5 override (docs/re/campaign.md
                // "Round pacing", sub_4016DA, PORTED 2026-07-09): in campaign
                // mode, a round where every human/joystick slot is dead is
                // force-ended and REPLAYED regardless of what round_winner()
                // says — even an AI side "winning" (w>=0, no human alive)
                // does not count. Route it exactly like a plain draw (below)
                // so it neither tallies a win nor advances the stage.
                if (w >= 0 && campaign_no_human_survivor()) w = -1;
                if (w >= 0) ++win_count_[w];  // tally the round win
                // The match-over check (§1 v73): the default win-count target,
                // or (team mode + win_by_kills) the kill-count clinch —
                // match_clinch() (game_app.hpp) so this agrees with
                // present_scoreboard's own clinch/outcome-line render. The
                // clinching slot can differ from the round winner `w` under
                // win_by_kills (a team's kill leader need not be this round's
                // sole survivor), so VICTORY names whoever match_clinch()
                // returns, not `w`.
                int clinched = w >= 0 ? match_clinch() : -1;
                bool match_over = clinched >= 0;
                // Gold player assignment (docs/re/goldman-roulette.md §2,
                // pseudo.c 30004-30022, LABEL_102): sub_42A3F6 only reaches
                // the RESULTS tier (and its unconditional dword_46492C
                // write) when sub_4219B0(...) != -1, i.e. a ROUND SURVIVOR
                // exists (`w >= 0` below) — a DRAW falls through to the
                // separate DRAW.PCX branch instead and never touches
                // dword_46492C at all, so a pending gold player survives a
                // draw round unchanged. When RESULTS does run, v73 (== our
                // `clinched` above) is the MATCH-CLINCH winner, never the
                // per-round winner `w` — so the gold player only changes
                // when a match is actually decided, and reverts to "none
                // pending" on every other clinch-less RESULTS pass (v73's
                // own -1 reset at the top of every RESULTS pass). Team mode
                // stores the raw team id (setup_team_[]), matching the wheel
                // award consumer in build_match_config and
                // present_scoreboard's own clinched_player -> setup_team_[]
                // lookup.
                if (w >= 0) {
                    gold_player_ =
                        assign_gold_player(options_.goldman, is_team_mode(), clinched, setup_team_);
                }
                if (match_over) {
                    // MATCH win: the target was reached -> VICTORY, then
                    // back to the menu (next(Results, Advance) = Menu).
                    audio_.start_music(kDrawMusicId);  // 1130 under VICTORY (doc §2 correction)
                    audio_.play_random_in_range(2000, 2299);  // "we have a winner", under VICTORY
                    // Team game -> TEAM<0/1>.PCX, else -> VICTORY<player>.PCX
                    // (frontend-flow.md "VICTORY" §3, aTeamU vs aVictoryU).
                    ev = present_screen(
                        victory_screen(is_team_mode(), clinched, setup_team_[clinched]));
                    // Campaign stage advance (docs/re/campaign.md
                    // "Advances through campaign stages automatically",
                    // sub_401312/sub_40133F gated `if (dword_46489C)`): a
                    // decided match steps dword_4648B0 to the next stage and
                    // loads its scheme/roster instead of returning to the
                    // menu. sub_4016DA's per-tick round pacing (RE'd
                    // 2026-07-09, docs/re/campaign.md "Round pacing —
                    // PINNED") ADDS an early-out once every rover/ghost is
                    // dead (a 2-second wall-clock grace before ending the
                    // stage) — clause 3, now wired via RoverSystem's hashed
                    // State::hazard_clear_timer and the run_match edge-check
                    // that sets over_ticks when it reaches kHazardClearTicks
                    // (see that check's own comment, same file). The clause
                    // this VICTORY-tail block still re-uses unmodified is
                    // clause 2, the SAME survivor-count check the normal
                    // best-of-N win_target_/win_by_kills clinch already is
                    // (sub_410578()<=1), which is exactly what
                    // match_over/clinched above already checks.
                    // Exhausting the stage list falls through to the menu
                    // and clears campaign state (port convenience; the
                    // original's own post-last-stage behaviour is unpinned
                    // — see ROADMAP.md).
                    if (campaign_active_ && ev != AppInput::Quit) {
                        ++campaign_stage_index_;  // ++dword_4648B0
                        if (campaign_stage_index_ <
                                static_cast<int>(campaign_stages_.size()) &&
                            load_campaign_stage(campaign_stage_index_)) {
                            reset_match_scores();
                            if (present_campaign_banner() == AppInput::Quit) return 0;
                            // Results -> Match with the NEXT stage's
                            // scheme/roster already loaded (app_flow.hpp's
                            // CampaignContinue), not a plain Advance (which
                            // would route to the menu, per next()'s Results
                            // case) or RoundContinue (documented as "same
                            // roster/settings", which this breaks).
                            ev = AppInput::CampaignContinue;
                        } else {
                            campaign_active_ = false;  // dword_46489C = 0 (stage list exhausted)
                            campaign_stages_.clear();
                            campaign_stage_index_ = 0;
                        }
                    }
                } else if (w >= 0) {
                    // Round win, match not over: show the running scores. The
                    // winner sting plays under THIS screen too (§1) — the
                    // original fires it as soon as the round decision is known,
                    // regardless of whether that decision also clinches the match.
                    audio_.start_music(kDrawMusicId);  // 1130 under RESULTS too (doc §2 correction)
                    audio_.play_random_in_range(2000, 2299);  // "we have a winner", under RESULTS
                    ev = present_scoreboard();
                } else {
                    // DRAW (no survivor / time-up), OR the campaign_no_human_
                    // survivor() override above forcing an AI-only "win" into
                    // this branch: nobody scores; replay a round. This IS
                    // clauses 4-5 of docs/re/campaign.md "Round pacing"
                    // (sub_4016DA): "all humans/network players dead -> undo
                    // the pending stage advance, replay the SAME stage".
                    // campaign_stage_index_ is only ever incremented in the
                    // match_over branch above, which requires w>=0 AFTER the
                    // override — so a plain draw (w==-1 from round_winner())
                    // never reached that increment to begin with, and the
                    // override now catches the one case that WOULD have
                    // (an AI side surviving with no human left): both land
                    // here, "replay a round" is exactly "replay the same
                    // campaign stage", with no separate decrement needed.
                    audio_.start_music(kDrawMusicId);  // 1130 draw track under DRAW
                    audio_.play_random_in_range(kDrawStingLo, kDrawStingHi);
                    ev = present_screen(draw_screen());
                }
                // Fold the screen's dismissal into the flow-graph event: an
                // undecided round's Advance becomes RoundContinue, so
                // next(Results, RoundContinue) loops straight back into Match
                // (sub_42A3F6's round loop) — the next round reuses the SAME
                // roster/level/win-target members the pre-match screens set;
                // only run_match's start_match reruns (fresh sim, next seed).
                // Back (Escape) abandons the match to the menu; a decided
                // match's Advance ends it there too. next() stays the single
                // authority over the state walk — no side-channel override.
                if (!match_over && ev == AppInput::Advance) ev = AppInput::RoundContinue;
                break;
            }
            // The .BM-backed leaves render their real help/credits text
            // (sub_41302D via BmScreen). Network/Controllers show the raw
            // NETWORK.BM/INPUT.BM text (Controllers is unreachable from any
            // menu row — docs/re/frontend-flow.md's INPUT.BM confirmed
            // negative, above); Credits shows CREDITS.BM with its inline
            // CREDBAR/JERM/KURT images. Options is now the fully-interactive
            // Team Play / Conveyor Speed screen (present_options_screen),
            // whose "Define keyboard layouts" row reaches the REAL interactive
            // key-remap UI (`sub_407B9D`, `KeyRemapScreen` — already ported,
            // NOT the same thing as the INPUT.BM text screen above); Options's
            // own F1 key still reaches the original OPTIONS.BM help text.
            case AppState::Options:
                ev = present_options_screen();
                break;
            case AppState::Controllers:
                ev = present_bm_screen("INPUT");
                break;
            case AppState::Network:
                ev = present_bm_screen("NETWORK");
                break;
            case AppState::Credits:
                ev = present_bm_screen("CREDITS");
                break;
            case AppState::Quit:
                break;
        }
        state = next(state, ev);
    }
    return 0;
}

// PORT ENHANCEMENT (task item 1, see init()'s window-creation comment): the
// Alt+Enter/F11 fullscreen toggle. Not an RE'd behaviour — the original has
// no fullscreen concept — so this lives outside any sub_XXXX-cited code path.
bool GameApp::handle_global_event(const SDL_Event& ev) {
    if (ev.type != SDL_EVENT_KEY_DOWN || ev.key.repeat) return true;  // keep; ignore key-repeat spam
    bool alt_enter = ev.key.key == SDLK_RETURN && (ev.key.mod & SDL_KMOD_ALT) != 0;
    bool f11 = ev.key.key == SDLK_F11;
    if (!alt_enter && !f11) return true;  // not ours: keep the event for the caller's own loop
    toggle_fullscreen();
    return false;  // swallow: don't let Enter/F11 also drive whatever screen is up
}

bool GameApp::sdl_event_filter(void* userdata, SDL_Event* event) {
    return static_cast<GameApp*>(userdata)->handle_global_event(*event);
}

void GameApp::toggle_fullscreen() {
    fullscreen_ = !fullscreen_;
    // SDL3's borderless "desktop" fullscreen (no explicit SDL_DisplayMode) —
    // resizes the OS window/output only; kScreenW/kScreenH and the sim are
    // untouched (SDL_LOGICAL_PRESENTATION_LETTERBOX keeps scaling correctly
    // at any output size, task item 2).
    SDL_SetWindowFullscreen(window_.get(), fullscreen_);
    options_dirty_ = true;  // persist the choice (task item 4), flush_options() below is the writer
}

void GameApp::flush_options() {
    // Write-on-exit (docs/re/results-and-options.md §2 "Persistence —
    // CONFIRMED via an exit-time write-back": sub_405DE3, the writer, is only
    // ever reached through sub_410EBF's atexit-style hook on a NORMAL app
    // exit — never per-edit). Guarded so a run that never touched an Options
    // row, or a run that never resolved a game_dir (init() already bailed),
    // does nothing.
    if (!options_dirty_ || options_path_.empty()) return;
    assets::Options to_write;
    to_write.team_play = options_.team_play;
    to_write.random_start = options_.random_start;
    to_write.conveyor_speed = options_.conveyor_speed_index;
    to_write.stomped_bombs_detonate = options_.stomped_bombs_detonate;
    to_write.win_by_kills = options_.win_by_kills;
    to_write.goldman = options_.goldman;
    to_write.enclosement_depth = options_.enclosement_depth;
    to_write.playtime = options_.playtime_seconds;
    to_write.assign_keyboards = std::nullopt;  // row omitted — never edited by this port
    to_write.diseases_destroyable = options_.diseases_destroyable;
    to_write.disable_game_music = options_.disable_game_music;
    // "fullscreen=" — PORT-ONLY key (see init()'s and toggle_fullscreen()'s
    // comments), always mirrored alongside the RE'd keys above.
    to_write.fullscreen = fullscreen_;
    // keydef=: always write the live KeyboardMapper bindings (both sets, all
    // 6 UI-exposed actions) so a rebind through the remap screen survives a
    // restart. Slots 6-9 per set (no in-game UI, §2) are left at -1/absent
    // here — save_options skips a -1 scancode, so any pre-existing keydef=
    // line for those slots (from a hand-edit or a future feature) is left
    // untouched by the read-modify-write rather than being clobbered blank.
    assets::KeyDef kd;
    for (int set = 0; set < assets::KeyDef::kSets; ++set) {
        const KeySet& ks = keyboard_.key_set(set);
        for (int action = 0; action < kKeyActionCount; ++action)
            kd.scancode[set][action] = ks.scancode[action];
    }
    to_write.keydef = kd;
    try {
        assets::save_options(options_path_, to_write);
        options_dirty_ = false;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "options.ini save failed: %s\n", e.what());
    }
}

int GameApp::run() {
    if (const char* env = std::getenv("BOMBER_BOOT_MATCH"); env && *env) opts_.boot_match = true;
    if (!init()) return opts_.game_dir.empty() ? 2 : 1;
    if (opts_.demo) {
        start_match(0xB0BB1E5);
        int rc = run_demo();
        flush_options();
        return rc;
    }
    int rc = run_app();
    flush_options();
    return rc;
}

}  // namespace bomber::game
