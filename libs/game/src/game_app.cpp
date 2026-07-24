#include "bomber/game/game_app.hpp"

#include <algorithm>  // std::max_element
#include <array>      // run_match's per-player tap latch
#include <chrono>     // random_boot_seed
#include <cstdint>    // load_window_icon's .ICO byte parsing
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
#include "bomber/game/dos_scancode.hpp"
#include "bomber/game/frontend_util.hpp"
#include "bomber/game/hud_format.hpp"
#include "bomber/game/match_outcome.hpp"
#include "bomber/game/screens/asset_screen.hpp"
#include "bomber/game/screens/boot_screen.hpp"
#include "bomber/game/screens/campaign_screens.hpp"
#include "bomber/game/screens/debug_info_screen.hpp"
#include "bomber/game/screens/editor_screen_runner.hpp"
#include "bomber/game/screens/help_screens.hpp"
#include "bomber/game/screens/map_select_screen.hpp"
#include "bomber/game/screens/match_runner.hpp"
#include "bomber/game/screens/menu_screen.hpp"
#include "bomber/game/screens/options_screens.hpp"
#include "bomber/game/screens/results_screens.hpp"
#include "bomber/game/screens/scheme_filename_prompt.hpp"
#include "bomber/game/screens/setup_screen.hpp"
#include "bomber/game/screens/video_settings_screen.hpp"
#include "bomber/game/sprites.hpp"
#include "bomber/match/match_factory.hpp"
#include "bomber/platform/frame_clock.hpp"

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

// The boot LOADING dialog — RE-PINNED 2026-07-10 (docs/re/frontend-flow.md
// "The percent-bar dialog, sub_412E33 (RE-PINNED)"). This is NOT the
// IPLOGO/HSLOGO/TITLE screen chain (sub_42B060, a separate later step): it
// is a small modal progress window (sub_43C734, above) that the entry point
// sub_42BE22 shows TWICE before sub_42B060 ever runs — once for "Loading
// data..." (getstring(201), sub_41D695's MASTER.ALI read) and once for
// "Loading sound..." (getstring(200), sub_4287B9's SOUNDLST group preload) —
// both driven by the shared percent-bar primitive sub_412E33. It is
// programmatically drawn (WINZ chrome + bar + text), NOT a full-screen PCX —
// no LOADING*.PCX exists anywhere in the install or the decompile.
//
// Pinned geometry/colours (sub_412E33, pseudo.c 16157-16211 + the 2026-07-10
// raw-byte passes): window y=200 (CONFIRMED literal, not centered — contrast
// the confirm dialog below), height=8*fontheight, width=360 (x
// auto-centered), painted with the WINZ.PCX 9-patch via sub_41726B @ 16181 —
// the BLUE textured window the user remembers, correcting the earlier flat
// (82,82,82) grey. Caption centered at y=1.5*fontheight in the general white
// ink (byte_49D38F -> (240,248,252)): the caption STRING is the buffer at
// 0x45BC5C, whose "Completion" initial value IS IDA's `aCompletion` symbol —
// but sub_412E0C strcpy's the caller's getstring(201)/(200) text over it
// before the dialog ever shows, so the visible caption is "Loading
// data..."/"Loading sound..." (the raw-byte pass that resolved the buffer
// address corrected the earlier "captioned Completion" reading). "%d"
// readout centered at y=3.5*fontheight in YELLOW (byte_49D37A -> LUT idx 182
// -> (252,248,88)); a 1-px WHITE FRAME around the bar band (sub_43D080 @
// 16191, y from 5.5*fontheight to 6.5*fontheight — its x extent is a
// decompiler-lost window field, reconstructed as one px around the track);
// two-tone bar at x=31, y=5.5*fontheight+1, height=fontheight-1, width 300
// split at 3*pct, filled portion byte_49A624 -> idx 178 -> (168,168,164),
// unfilled black. All text via sub_41696C = ink over a 4-pass 1-px black
// outline (draw_dialog_text).
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
// before sub_41095A ever calls the loading dialogs). WINZ.PCX is likewise
// loaded by sub_414DF4 itself ("winz.plt"), so GameApp::init pre-warms both
// before the first flash.
void draw_boot_loading_dialog(SDL_Renderer* ren, const FontTextures& font, const Sprite* winz,
                              const char* caption) {
    SDL_SetRenderDrawColor(ren, 0, 0, 0, 255);
    SDL_RenderClear(ren);

    const float h = static_cast<float>(font.loaded() ? font.line_height() : 12);
    const DialogRect win = dialog_rect(200.0f, 8.0f * h, 360.0f);
    draw_dialog_chrome(ren, win, winz);

    // Caption — the getstring(201)/(200) text (see the buffer note above),
    // general white ink, centered, y = 1.5*fontheight (window-relative).
    std::string cap_str = caption;
    float cap_w = font.loaded() ? static_cast<float>(font.measure(cap_str)) : 0.0f;
    draw_dialog_text(ren, font, cap_str, win.x + (win.w - cap_w) / 2, win.y + 1.5f * h, kDialogInkR,
                     kDialogInkG, kDialogInkB);

    // The white bar frame (sub_43D080): 1-px outline in the general white
    // ink spanning the 5.5h..6.5h band, one px around the 300-px track.
    SDL_FRect frame{win.x + 30.0f, win.y + 5.5f * h, 302.0f, h + 1.0f};
    SDL_SetRenderDrawColor(ren, kDialogInkR, kDialogInkG, kDialogInkB, 255);
    SDL_RenderRect(ren, &frame);

    // The bar sits at a fixed "complete" 100% (the documented simplification
    // above) — the filled segment spans the full 300 px track, so the
    // never-drawn unfilled segment is omitted rather than drawn zero-width.
    SDL_FRect bar{win.x + 31.0f, win.y + 5.5f * h + 1.0f, 300.0f, h - 1.0f};
    SDL_SetRenderDrawColor(ren, 168, 168, 164, 255);  // byte_49A624 -> idx 178
    SDL_RenderFillRect(ren, &bar);

    // "%d" readout (100, matching the always-complete bar) — yellow
    // (byte_49D37A -> idx 182), y = 3.5*fontheight, horizontally centered.
    std::string pct_str = "100";
    float pct_w = font.loaded() ? static_cast<float>(font.measure(pct_str)) : 0.0f;
    draw_dialog_text(ren, font, pct_str, win.x + (win.w - pct_w) / 2, win.y + 3.5f * h, 252, 248,
                     88);

    SDL_RenderPresent(ren);
}

}  // namespace

// Loads the original's BM95.ICO (a shipped install file, runtime-loaded like
// every other asset — never committed) into an RGBA surface for the window
// icon, matching the native's window/taskbar icon (sub_41095A sets it via the
// EXE's own icon resource, which BM95.ICO mirrors). Picks the 32x32 8-bit entry
// (or the largest 8-bit one), applies its BGRA palette + 1-bpp AND transparency
// mask. Classic .ICO = a stack of bottom-up BMP DIBs. Returns nullptr on any
// malformation (the window just stays icon-less). Caller owns the surface.
static SDL_Surface* load_window_icon(const std::filesystem::path& ico_path) {
    std::size_t sz = 0;
    void* raw = SDL_LoadFile(ico_path.string().c_str(), &sz);
    if (!raw) return nullptr;
    const auto* d = static_cast<const std::uint8_t*>(raw);
    auto u16 = [&](std::size_t o) { return static_cast<std::uint16_t>(d[o] | (d[o + 1] << 8)); };
    auto u32 = [&](std::size_t o) {
        return static_cast<std::uint32_t>(d[o] | (d[o + 1] << 8) | (d[o + 2] << 16) |
                                          (static_cast<std::uint32_t>(d[o + 3]) << 24));
    };
    SDL_Surface* surf = nullptr;
    do {
        if (sz < 6 || u16(2) != 1) break;  // ICONDIR: reserved0, type==1
        const int count = u16(4);
        int best = -1, best_score = -1;
        for (int i = 0; i < count; ++i) {
            const std::size_t e = 6 + static_cast<std::size_t>(i) * 16;
            if (e + 16 > sz) break;
            const std::uint32_t off = u32(e + 12);
            if (static_cast<std::size_t>(off) + 40 > sz) continue;
            if (u16(off + 14) != 8) continue;  // 8-bpp DIB entries only (simple palette path)
            const int w = d[e] ? d[e] : 256;
            const int score = (w == 32) ? 10000 : w;  // prefer 32x32, else the largest
            if (score > best_score) {
                best_score = score;
                best = i;
            }
        }
        if (best < 0) break;
        const std::size_t e = 6 + static_cast<std::size_t>(best) * 16;
        const std::uint32_t off = u32(e + 12);
        const int w = static_cast<int>(u32(off + 4));
        const int hh = static_cast<int>(u32(off + 8)) / 2;  // DIB height is 2x (XOR bitmap + AND mask)
        if (w <= 0 || hh <= 0 || w > 256 || hh > 256) break;
        const std::size_t pal = off + 40;                                   // 256 BGRA entries
        const std::size_t xoff = pal + 256 * 4;                             // XOR (colour) bitmap
        const int rowb = ((w + 3) / 4) * 4;                                 // 8-bpp row, 4-aligned
        const int maskrow = ((w + 31) / 32) * 4;                            // 1-bpp AND row, 4-aligned
        const std::size_t aoff = xoff + static_cast<std::size_t>(rowb) * hh;  // AND (mask) bitmap
        if (aoff + static_cast<std::size_t>(maskrow) * hh > sz) break;
        surf = SDL_CreateSurface(w, hh, SDL_PIXELFORMAT_RGBA32);
        if (!surf) break;
        auto* px = static_cast<std::uint8_t*>(surf->pixels);
        for (int y = 0; y < hh; ++y) {
            const int sy = hh - 1 - y;  // DIB rows are bottom-up
            for (int x = 0; x < w; ++x) {
                const std::uint8_t idx = d[xoff + static_cast<std::size_t>(sy) * rowb + x];
                const std::size_t p = pal + static_cast<std::size_t>(idx) * 4;
                const std::uint8_t m = d[aoff + static_cast<std::size_t>(sy) * maskrow + (x / 8)];
                const bool clear = (m >> (7 - (x & 7))) & 1;  // AND-mask bit set == transparent
                std::uint8_t* o = px + static_cast<std::size_t>(y) * surf->pitch +
                                  static_cast<std::size_t>(x) * 4;
                o[0] = d[p + 2];  // R (palette is BGRA)
                o[1] = d[p + 1];  // G
                o[2] = d[p + 0];  // B
                o[3] = clear ? 0 : 255;
            }
        }
    } while (false);
    SDL_free(raw);
    return surf;
}

bool GameApp::init() {
    seed_front_end_rngs();

    fs::path game;
    fs::path scheme_path;
    if (!resolve_install_paths(game, scheme_path)) return false;
    if (!load_config(game, scheme_path)) return false;

    SDL_Renderer* ren = nullptr;
    if (!init_video(ren)) return false;
    if (!load_assets(ren, game)) return false;

    build_presentation(ren);
    return true;
}

void GameApp::seed_front_end_rngs() {
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
    // --demo / --demo-shots (tests/visual/, the screenshot regression
    // harness): pin every presentation-side LCG to a fixed literal so two
    // runs of the SAME scripted match are byte-identical. run()'s demo path
    // (start_match(0xB0BB1E5)) doesn't currently read any of these four —
    // it never visits the menu/attract/Goldman-wheel/campaign-roster code
    // that touches them — but overwriting the entropy-seeded values here
    // removes that as an assumption the harness has to keep re-verifying if
    // the demo script grows to cover more of the front end later. Renderer's
    // own cosmetic LCGs (panic_lcg_/flash_lcg_/gold_lcg_) are already fixed
    // literals by default (renderer.hpp) and are never reseeded, so they
    // need no override here.
    if (opts_.demo) {
        setup_lcg_ = attract_lcg_ = goldman_lcg_ = next_seed_ = 0xD3701234u;
    }
}

bool GameApp::resolve_install_paths(fs::path& game, fs::path& scheme_path) {
    game = !opts_.game_dir.empty() ? opts_.game_dir : assets::default_game_dir();
    if (game.empty() || !fs::is_directory(game / "DATA")) {
        std::fprintf(stderr,
                     "usage: bomber_game [game_dir] [scheme.sch]\n"
                     "(or set BOMBER_GAME_DIR / gamedir.txt)\n");
        return false;
    }
    opts_.game_dir = game;
    scheme_path = !opts_.scheme.empty() ? opts_.scheme : game / "DATA" / "SCHEMES" / "BASIC.SCH";
    return true;
}

bool GameApp::load_config(const fs::path& game, const fs::path& scheme_path) {
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
        // the OPTIONS game-type screen, OFF by default"). PINNED OFF on the
        // demo/screenshot path (like the seed + LCGs, tests/visual/README.md):
        // team_play changes the in-match player render (team vs own colour), so
        // reading it from the mutable options.ini would make the visual golden
        // depend on whatever the user last saved — a real fragility that surfaced
        // once options.ini held team_play=1 (every golden shot went white).
        team_play_ = !opts_.demo && loaded_opts.team_play.value_or(false);
        options_.team_play = team_play_;
        // Absent-key defaults for the sim-consumed toggles come from VALUELST,
        // mirroring sub_41095A's init order exactly: dword_464AE8=getvalue(40)
        // ("do we randomize player starting positions?" = 1), dword_464940 =
        // getvalue(46) ("wall segment closes in on a bomb ... 1 - detonate" =
        // 1), dword_464990 = getvalue(120) ("can diseases be blown up like
        // all other powerups?" = 1) — and options.ini then overrides (the
        // shipped file sets all three to 1 as well). docs/re/facts.md
        // "Options toggles: stomped_bombs_detonate / diseases_destroyable".
        options_.random_start = loaded_opts.random_start.value_or(values_.at_or(40, 1) != 0);
        options_.conveyor_speed_index = conveyor_speed_index_.value_or(1);
        options_.stomped_bombs_detonate =
            loaded_opts.stomped_bombs_detonate.value_or(values_.at_or(46, 1) != 0);
        options_.win_by_kills = loaded_opts.win_by_kills.value_or(false);
        options_.goldman = loaded_opts.goldman.value_or(false);
        options_.enclosement_depth = loaded_opts.enclosement_depth.value_or(1);
        // Demo/screenshot mode PINS playtime to the golden's 150 s (2:30),
        // independent of the install's options.ini: the visual-golden harness
        // hashes the presented frame, whose clock HUD would otherwise change
        // whenever a real session (original or port) rewrites playtime= — a
        // 180 s value silently re-hashed every pinned shot (caught
        // 2026-07-12). Same reproducibility contract as the seed/LCG pins
        // above. Interactive runs read the file as normal.
        options_.playtime_seconds = opts_.demo ? 150 : loaded_opts.playtime.value_or(150);
        options_.diseases_destroyable =
            loaded_opts.diseases_destroyable.value_or(values_.at_or(120, 1) != 0);
        options_.disable_game_music = loaded_opts.disable_game_music.value_or(false);
        // Options-screen rows 10/12/17 — trivial booleans with real
        // options.ini keys but no gameplay consumer (docs/re/results-and-
        // options.md §3); round-tripped like every other toggle now that the
        // full-audit pass (2026-07-09) added their rows back to the screen.
        options_.assign_keyboards = loaded_opts.assign_keyboards.value_or(false);
        options_.lost_net_revert_ai = loaded_opts.lost_net_revert_ai.value_or(false);
        options_.small_memory = loaded_opts.smallmemory.value_or(false);
        // Row 14's four modem fields (display-only; getstring(264) — chrome
        // audit 2026-07-12): straight from options.ini's modem keys, defaults
        // = the shipped install's values. node_name stays "" (sub_40FE34's
        // runtime buffer is bss-empty and NOT an options.ini key).
        options_.modemport = loaded_opts.modemport.value_or(2);
        options_.modemirq = loaded_opts.modemirq.value_or(3);
        options_.modembaud = loaded_opts.modembaud.value_or(19200);
        options_.modemdial = loaded_opts.modemdial.value_or("555-1212");
        // Row 8: schemefilename= now actually DRIVES the loaded scheme (the
        // original re-parses byte_4648C4 at every Play-flow entry —
        // sub_410F81 -> sub_4046CC -> sub_403EEE — so the key was never
        // display-only). An explicit --scheme argument still wins, and the
        // demo/visual-golden harness stays pinned to BASIC.SCH for the same
        // reproducibility reason the playtime pin above cites. BASIC.SCH is
        // only the fallback when the key is absent or doesn't resolve.
        options_.scheme_filename =
            loaded_opts.schemefilename.value_or(scheme_path.filename().string());
        if (!opts_.demo && opts_.scheme.empty() && loaded_opts.schemefilename &&
            !loaded_opts.schemefilename->empty()) {
            if (!reload_scheme_from_name(*loaded_opts.schemefilename))
                std::fprintf(stderr, "schemefilename '%s' not found; keeping %s\n",
                             loaded_opts.schemefilename->c_str(),
                             scheme_path.filename().string().c_str());
        }
        // "keydef=" -> KeyboardMapper's two live key-sets (docs/re/results-and-
        // options.md §2). The file holds the ORIGINAL's DOS/AT set-1
        // scancodes (interchangeable with BM95.EXE against a shared
        // install); translate into SDL_Scancode space for the live mapper
        // (dos_scancode.hpp). A KeyDef triple with scancode == -1 (never
        // written) keeps that action's compiled-in default (input.hpp's
        // default_key_set); DOS 0 / an unmappable code binds
        // SDL_SCANCODE_UNKNOWN (0) = effectively unbound, like the original.
        if (loaded_opts.keydef) {
            for (int set = 0; set < assets::KeyDef::kSets; ++set) {
                KeySet ks = keyboard_.key_set(set);
                for (int action = 0; action < kKeyActionCount; ++action) {
                    int sc = loaded_opts.keydef->scancode[set][action];
                    if (sc >= 0) ks.scancode[action] = sdl_scancode_from_dos(sc);
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
        // PORT-ONLY "Video Settings" keys (install.hpp), faithful defaults: vsync
        // ON (uncap off), native cadence OFF (deterministic), fps readout hidden.
        // vsync ON == uncapped OFF, so the internal uncap flag is its inverse.
        uncap_fps_ = !loaded_opts.vsync.value_or(true);
        native_cadence_ = loaded_opts.native_cadence.value_or(false);
        show_fps_ = loaded_opts.show_fps.value_or(false);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "%s\n", e.what());
        return false;
    }
    return true;
}

bool GameApp::init_video(SDL_Renderer*& ren) {
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
    // SDL_LOGICAL_PRESENTATION_STRETCH below, which scales the whole 640x480
    // frame to FILL whatever window/monitor size the player picks — matching
    // how the original presents on the reference Win11 machine (its
    // maximized/fullscreen surface fills the panel edge to edge, aspect not
    // preserved; user-verified side by side 2026-07-12). The earlier
    // LETTERBOX mode kept 4:3 with black bars — the reported mismatch. Any
    // fullscreen toggle (Alt+Enter/F11, sdl_event_filter below) just resizes
    // the OS window/output — it never touches kScreenW/kScreenH or the sim.
    SDL_Window* win = nullptr;
    ren = nullptr;
    // Window title matches the original (sub_41095A -> sub_43E5CC(aAtomicBomberma)).
    if (!SDL_CreateWindowAndRenderer("Atomic Bomberman", kScreenW * 2, kScreenH * 2,
                                     SDL_WINDOW_RESIZABLE, &win, &ren)) {
        std::fprintf(stderr, "SDL_CreateWindowAndRenderer: %s\n", SDL_GetError());
        return false;
    }
    window_.reset(win);
    sdl_renderer_.reset(ren);
    // Window/taskbar icon from the install's own BM95.ICO (matches the native).
    if (SDL_Surface* icon = load_window_icon(opts_.game_dir / "BM95.ICO")) {
        SDL_SetWindowIcon(window_.get(), icon);
        SDL_DestroySurface(icon);
    }
    // Demo/screenshot mode pins its OWN presentation: the visual-golden
    // harness hashes the presented backbuffer, so its pixels must not depend
    // on either the user's saved fullscreen state (a 3840x2160 fullscreen
    // demo run silently re-hashed every pinned shot — caught 2026-07-12) or
    // the interactive STRETCH mode's scaler (whose output differs from
    // LETTERBOX's even at an exact 2x 4:3 window). Interactive runs get
    // STRETCH — matching how the original fills the panel edge to edge on
    // the reference machine — and demo runs keep the LETTERBOX scaler every
    // existing pin was captured under.
    const bool demo_mode =
        opts_.demo_ticks > 0 || !opts_.demo_shots.empty() || !opts_.bm_shot_name.empty() ||
        !opts_.menu_shot_out.empty();
    SDL_SetRenderLogicalPresentation(ren, kScreenW, kScreenH,
                                     demo_mode ? SDL_LOGICAL_PRESENTATION_LETTERBOX
                                               : SDL_LOGICAL_PRESENTATION_STRETCH);
    if (fullscreen_ && !demo_mode) SDL_SetWindowFullscreen(win, true);
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
    // vsync ON by default; OFF when the Video Settings "vsync" toggle (uncap_fps_)
    // is set, so the render loop can free-run to the sub-frame rate (~180 fps).
    SDL_SetRenderVSync(ren, uncap_fps_ ? 0 : 1);
    return true;
}

bool GameApp::load_assets(SDL_Renderer* ren, const fs::path& game) {
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
    // WINZ.PCX (the blue 9-patch window skin, draw_boot_loading_dialog's
    // comment) is likewise pre-warmed here: sub_414DF4 loads "winz.plt"
    // during graphics init, before either dialog runs.
    const Sprite& winz = assets_.load_frontend_winz(ren, game);
    draw_boot_loading_dialog(ren, front_font_, &winz, "Loading data...");

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
        draw_boot_loading_dialog(ren, front_font_, &assets_.frontend_pcx("WINZ"),
                                 assets_.getstring(200, "Loading sound...").c_str());
        if (!audio_.init(game)) std::fprintf(stderr, "audio unavailable, continuing silent\n");
    }
    return true;
}

void GameApp::build_presentation(SDL_Renderer* ren) {
    base_tuning_ = match::build_match_config(scheme_, 2, 0, &values_).tuning;
    // Seed setup-screen slot colours from VALUELST for any colour without a .RMP
    // tail (a loaded .RMP keeps its own authoritative tail), then build the
    // per-player recolored sprite sets (authentic .RMP remap where available).
    assets_.set_color_fallbacks(base_tuning_.color_rgb, 10);
    assets_.build_player_sets(base_tuning_.color_rgb);
    seqs_.resolve(assets_);  // re-resolve: player sprite sets exist now

    renderer_.emplace(ren, assets_, seqs_, values_);
    screen_.emplace(assets_, audio_);
    // front_font_ (FONT6.FON glyph textures for the dialog chrome and the .BM
    // help/credits screens) was already built above, before the boot LOADING
    // dialogs — matching sub_41095A's real init order. assets_.load() reloads
    // the same FONT6.FON into assets_.frontend_font() (harmless — identical
    // file), so no second build() is needed here.
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
// kBootMusicId (1000) moved to screens/boot_screen.cpp with run_boot_attract.
// kMenuMusicId (1010, 0x3F2 MENU.RSS) moved to screens/menu_screen.cpp with present_menu.
// kWinMusicId (1020, 0x3FC WIN.RSS — the SETUP-SCREENS backdrop track, player
// select / LEVEL & ROUNDS, NOT victory music per docs/re/in-match-shell.md §2)
// moved to screens/results_screens.cpp with the goldman wheel, its last user in
// this file; setup_screen.cpp keeps its own copy too.
// The outcome-tier music this file still owns (sub_42A3F6), CORRECTED by
// docs/re/in-match-shell.md §2 (supersedes this file's earlier "1020 under
// VICTORY" reading): at Round end (pseudo.c 29820) sub_42741E(0x46A) = 1130
// ("draw") replaces the stage track UNCONDITIONALLY, before the survivor test —
// so DRAW, the RESULTS tally, AND VICTORY/TEAM all play under 1130; nothing
// restarts 1020 anywhere in the outcome tier. A looping track (start_music),
// replacing the menu/stage music.
constexpr int kDrawMusicId = 1130;  // 0x46A — DRAW.RSS, DRAW *and* RESULTS *and* VICTORY backdrop
// kStageMusicFallback (1120, 0x460 GENERIC.RSS — the per-level in-round stage
// track fallback) moved to screens/match_runner.cpp with start_match, its only
// user (sub_4293E5, docs/re/in-match-shell.md §2).
// kTitleStingLo/Hi (2800..2899, the title intro sting group) moved to
// screens/boot_screen.cpp with run_boot_attract.
// kQuitStingLo/Hi (2600..2699, the menu-quit / exit sting group played by
// sub_412987) moved to screens/menu_screen.cpp with present_menu.

// kBootDwellMs (the getvalue(12)=7s waited-screen dwell) + the logo_screen/
// title_screen ScreenDef factories moved to screens/boot_screen.cpp with
// run_boot_attract (the only user of all three).

// kAttractIdleFallbackS/MinS (the main-menu getvalue(92) ATTRACT idle timeout,
// CONFIRMED getvalue(92)=30 gated >5) moved to screens/menu_screen.cpp with
// present_menu.

// Results: DRAW (no survivor / time up) or VICTORY<player> (one survivor). The
// original draws these with sub_42A088(name, 0) then a bespoke "any key, or 6 s
// in attract" loop (sub_42A3F6); we model it as a normal Screen with a bounded
// dwell so an unattended machine returns to the menu on its own.
constexpr std::uint32_t kResultsDwellMs = 6000;  // sub_42A3F6 attract auto-advance
// fmt_u/fmt_s/fmt_us (the crash-proof single-specifier MESSAGES.TXT splices)
// moved to bomber/game/hud_format.hpp so the extracted screen classes share the
// same helpers instead of re-deriving them — see that header.

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
    // sub_42A3F6's VICTORY tail is sub_42A088(name, 0) (a CUT) then a hard
    // sub_413CB0(3000) — a fixed 3 s blocking sleep that pumps only OS messages
    // and reads NO game key. So the VICTORY/TEAM PCX shows for exactly 3 s and
    // cannot be skipped (unlike the 6 s keypress-skippable port model this
    // replaces). Non-skippable + 3000 ms reproduces both (Quit still exits).
    return ScreenDef{victory_background_name(team_mode, player, team),
                     {},
                     /*dwell_ms*/ 3000,
                     /*skippable*/ false};
}
// The main-menu model (sub_42B9CE) — the MenuItem struct, the seven-row
// kMenuItems table (v10 dispatch order), and kMenuCount moved to
// screens/menu_screen.cpp with present_menu.

// Cursor anchor over MAINMENU.PCX — CONFIRMED getvalue(700/701/702) (sub_42B9CE:
// v11=getvalue(700)=X, v1=getvalue(701)=Y, getvalue(702)=Y-step; the bomb-
// trigger sprite is blitted at x=X, y=Y + Ystep*row). Read live from VALUELST
// (columns of the multi-value row 700, whose own legend reads "X, Y - first item
// / YS - y-spacing"); these fallbacks are that install's values (332,140,38) so
// a stripped VALUELST still positions sanely. The idle/attract timeout uses
// getvalue(92) (sub_42B9CE), distinct from the waited-screen getvalue(12).
constexpr int kMenuCursorXFallback = 332;    // getvalue(700)
constexpr int kMenuCursorYFallback = 140;    // getvalue(701)
// kMenuCursorStepFallback (getvalue(702)=38) moved to screens/menu_screen.cpp
// with present_menu; kMenuCursorX/YFallback stay for run_menu_shot's row-0 draw.

}  // namespace

// Forwarder to the extracted MatchRunner (ADR-0009 §10). start_match builds the
// MatchConfig (scheme + VALUELST + base tuning + roster + options + goldman award
// + level/actors/campaign hazards), resets sim_ + renderer_, and seeds the match —
// RNG/seed-sensitive. Kept as a GameApp method (not just a MatchRunner private) so
// run()'s --demo path can build a match through it before run_demo() ticks sim_
// directly; run_match's own head calls MatchRunner::start_match instead.
void GameApp::start_match(std::uint32_t seed) {
    MatchRunner(sctx(), match_runner_state()).start_match(seed);
}

bool GameApp::save_screenshot(const fs::path& out) const {
    SDL_Surface* shot = SDL_RenderReadPixels(sdl_renderer_.get(), nullptr);
    if (!shot) return false;
    bool ok = SDL_SaveBMP(shot, out.string().c_str());
    SDL_DestroySurface(shot);
    return ok;
}

namespace {
// Names Event::Type for BOMBER_DEMO_TRACE below — kept local to this TU, only
// used to help a human pick --demo-shots tick numbers (tests/visual/
// README.md "Deriving new shot ticks"), never for gameplay logic.
const char* event_type_name(sim::Event::Type t) {
    switch (t) {
        case sim::Event::Type::BombPlaced: return "BombPlaced";
        case sim::Event::Type::BombKicked: return "BombKicked";
        case sim::Event::Type::Explosion: return "Explosion";
        case sim::Event::Type::BrickDestroyed: return "BrickDestroyed";
        case sim::Event::Type::PowerupRevealed: return "PowerupRevealed";
        case sim::Event::Type::PowerupPicked: return "PowerupPicked";
        case sim::Event::Type::PowerupBurned: return "PowerupBurned";
        case sim::Event::Type::PlayerDied: return "PlayerDied";
        case sim::Event::Type::TimeUp: return "TimeUp";
        case sim::Event::Type::Hurry: return "Hurry";
        case sim::Event::Type::WallClosed: return "WallClosed";
        case sim::Event::Type::BombPunched: return "BombPunched";
        case sim::Event::Type::BombBounced: return "BombBounced";
        case sim::Event::Type::BombGrabbed: return "BombGrabbed";
        case sim::Event::Type::BombThrown: return "BombThrown";
        case sim::Event::Type::HeadHit: return "HeadHit";
        case sim::Event::Type::Infected: return "Infected";
        case sim::Event::Type::BombStopped: return "BombStopped";
        case sim::Event::Type::JellyBounced: return "JellyBounced";
        case sim::Event::Type::TrampolineBounce: return "TrampolineBounce";
        case sim::Event::Type::WarpUsed: return "WarpUsed";
        case sim::Event::Type::RoverSpawned: return "RoverSpawned";
        case sim::Event::Type::RoverDied: return "RoverDied";
        case sim::Event::Type::RoverKilledPlayer: return "RoverKilledPlayer";
        case sim::Event::Type::TileRegrew: return "TileRegrew";
        case sim::Event::Type::DropRefused: return "DropRefused";
    }
    return "?";
}
}  // namespace

int GameApp::run_demo() {
    // Diagnostic only (tests/visual/README.md "Deriving new shot ticks"):
    // dump every sim event with its tick, so a human can pick --demo-shots
    // tick numbers (bomb placed -> pulsing; +fuse -> explosion; brick/
    // powerup events -> crumble/reveal) from a real run against the
    // installed assets instead of guessing tuning arithmetic by hand. Never
    // touches rendering or output; opt-in so normal --demo runs stay quiet.
    const bool trace = std::getenv("BOMBER_DEMO_TRACE") != nullptr;
    if (trace)
        std::fprintf(stderr, "tuning: fuse=%d flame=%d brick_burn=%d\n",
                    sim_.state().tuning.fuse_frames, sim_.state().tuning.flame_frames,
                    sim_.state().tuning.brick_burn_frames);

    // Visual golden harness (tests/visual/, --demo-shots): capture a NAMED
    // frame at each requested tick within one scripted run, instead of the
    // legacy single BMP at the end. Both modes tick the SAME deterministic
    // script (demo_inputs, input.cpp) — --demo-shots just adds save points
    // along the way, so the legacy final-frame screenshot at a given tick
    // count is byte-identical to what --demo alone would have produced.
    if (!opts_.demo_shots.empty()) {
        int max_tick = 0;
        for (const auto& [label, tick] : opts_.demo_shots) max_tick = std::max(max_tick, tick);
        int rc = 0;
        for (int t = 0; t < max_tick; ++t) {
            sim_.tick(demo_inputs(t));
            if (trace)
                for (const auto& e : sim_.state().events)
                    std::fprintf(stderr, "  t=%d %s player=%d (%d,%d) data=%d\n", t + 1,
                                event_type_name(e.type), e.player, e.x, e.y, e.data);
            sounds_.on_tick(sim_.state());
            renderer_->on_events(sim_.state());   // NOLINT(bugprone-unchecked-optional-access)
            renderer_->draw_frame(sim_.state());  // NOLINT(bugprone-unchecked-optional-access) —
                                                  // keeps walk-anim sampling in sync
            int reached = t + 1;
            for (const auto& [label, tick] : opts_.demo_shots) {
                if (tick != reached) continue;
                fs::path out = opts_.demo_shot_dir / (label + ".bmp");
                bool ok = save_screenshot(out);
                if (!ok) rc = 1;
                std::printf("demo-shots: tick %d (%s) alive %d -> %s%s\n", reached, label.c_str(),
                            sim::alive_count(sim_.state()), out.string().c_str(),
                            ok ? "" : " (FAILED)");
            }
        }
        return rc;
    }

    for (int t = 0; t < opts_.demo_ticks; ++t) {
        sim_.tick(demo_inputs(t));
        if (trace)
            for (const auto& e : sim_.state().events)
                std::fprintf(stderr, "  t=%d %s player=%d (%d,%d) data=%d\n", t + 1,
                            event_type_name(e.type), e.player, e.x, e.y, e.data);
        sounds_.on_tick(sim_.state());
        renderer_->on_events(sim_.state());   // NOLINT(bugprone-unchecked-optional-access)
        renderer_->draw_frame(sim_.state());  // NOLINT(bugprone-unchecked-optional-access) — keeps
                                              // walk-anim sampling in sync
    }
    bool ok = save_screenshot(opts_.demo_out);
    if (ok)
        std::printf("demo: %d ticks, alive %d, screenshot %s\n", opts_.demo_ticks,
                    sim::alive_count(sim_.state()), opts_.demo_out.string().c_str());
    return ok ? 0 : 1;
}

ScreenContext GameApp::sctx() {
    // *screen_ is emplaced in init() before run_app() drives any screen, so it is
    // always engaged here (same invariant as renderer_, see the file-top note).
    return ScreenContext{assets_,
                         audio_,
                         sounds_,
                         keyboard_,
                         gamepads_,
                         front_font_,
                         cursor_blink_,
                         *screen_,  // NOLINT(bugprone-unchecked-optional-access)
                         seqs_,
                         values_,
                         sdl_renderer_.get(),
                         window_.get()};
}

OptionsEditState GameApp::options_state() {
    // The Options cluster's shared-state seam (ADR-0009 §4): the non-service
    // members its three runner screens read/write, bundled by reference so the
    // runners need no GameApp&. Built fresh on demand, same as sctx().
    return OptionsEditState{options_,       options_dirty_, scheme_,
                            setup_lcg_,     opts_.game_dir,  gold_player_,
                            team_play_,     conveyor_speed_index_};
}

MenuState GameApp::menu_state() {
    // The main menu's shared-state seam (ADR-0009 §6): the non-service members
    // present_menu + roll_attract_match read/write (the cursor + idle clock +
    // editor trigger counter, the attract roster/level/team + LCG, the F10
    // video toggles, and the four editor members) — bundled by reference so
    // MenuScreen needs no GameApp&. Built fresh on demand, same as sctx()/
    // options_state()/editor_state(). Field order MUST track MenuState's.
    return MenuState{menu_index_,     menu_idle_since_ms_, editor_trigger_count_,
                     attract_,        attract_saved_,      attract_lcg_,
                     setup_type_,     setup_sub_,          setup_team_,
                     selected_level_, team_play_,          uncap_fps_,
                     native_cadence_, show_fps_,           options_dirty_,
                     setup_lcg_,      scheme_,             opts_.game_dir,
                     opts_.scheme};
}

MapSelectState GameApp::map_select_state() {
    // The LEVEL & ROUNDS screen's shared-state seam (ADR-0009 §7): the
    // non-service members present_map_select reads/writes (the committed level +
    // win-target, the shared preview LCG, the read-only win-by-kills option, and
    // the pending gold player Escape forfeits) — bundled by reference so
    // MapSelectScreen needs no GameApp&. Built fresh on demand, same as sctx()/
    // menu_state(). Field order MUST track MapSelectState's.
    return MapSelectState{selected_level_, win_target_, setup_lcg_, options_, gold_player_};
}

SetupState GameApp::setup_state() {
    // The PLAYER INPUT screen's shared-state seam (ADR-0009 §7): the non-service
    // members present_setup + cycle_input_type read/write (the 10-slot roster, the
    // shared pick_glue LCG, the team gate, the hidden 'C'x5 counter, the Esc-forfeit
    // gold player, and the campaign teardown Esc performs) — bundled by reference so
    // SetupScreen needs no GameApp&. Built fresh on demand, same as sctx()/
    // map_select_state(). Field order MUST track SetupState's. campaign_active_/
    // campaign_stages_/campaign_stage_index_ also feed campaign_state() (passed
    // alongside for the 'C'x5 picker), but the setup body reaches them through here.
    return SetupState{setup_type_,          setup_sub_,              setup_team_,
                      setup_lcg_,           campaign_trigger_count_,  team_play_,
                      gold_player_,         campaign_active_,         campaign_stages_,
                      campaign_stage_index_};
}

MatchBackdrop GameApp::match_backdrop() {
    // The match-coupled backdrop seam (ADR-0009 §8): the live Renderer + sim
    // State the campaign confirm/banner/complete dialogs and the in-round help
    // modal draw as their frozen backdrop, kept SEPARATE from the front-end-
    // service-only ScreenContext. renderer_ is emplaced in init() before run_app
    // drives any screen (same invariant as *screen_ in sctx(), see file-top note).
    return MatchBackdrop{*renderer_,  // NOLINT(bugprone-unchecked-optional-access)
                         sim_.state()};
}

CampaignState GameApp::campaign_state() {
    // The campaign flow's shared-state seam (ADR-0009 §8): the non-service
    // members present_campaign_picker + load_campaign_stage read/write — bundled
    // by reference so CampaignPickerScreen (and the free load_campaign_stage)
    // need no GameApp&. Built fresh on demand, same as sctx()/menu_state().
    // Field order MUST track CampaignState's.
    return CampaignState{campaign_active_, campaign_stages_, campaign_stage_index_,
                         campaign_banner_, setup_type_,      setup_sub_,
                         setup_team_,      setup_lcg_,       scheme_,
                         opts_.game_dir};
}

ScoreboardState GameApp::scoreboard_state() {
    // The RESULTS scoreboard's shared-state seam (ADR-0009 §8): the non-service
    // members present_scoreboard reads (the frozen round's sim::State, the
    // win/kill tally + roster it rows, the win-by-kills option, and the
    // demo/roster flags auto_advance_results() consults) — bundled by reference/
    // value so ScoreboardScreen needs no GameApp&. Built fresh on demand, same as
    // sctx()/campaign_state(). Field order MUST track ScoreboardState's.
    return ScoreboardState{sim_.state(), win_count_,  kill_count_,     win_target_,
                           setup_team_,  team_play_,  setup_type_,     options_,
                           opts_.demo,   opts_.demo_ticks, opts_.demo_shots};
}

GoldmanState GameApp::goldman_state() {
    // The Goldman wheel's shared-state seam (ADR-0009 §8): the three non-service
    // members present_goldman_wheel writes (the wheel's presentation LCG and the
    // pending gold-player/prize pair) — bundled by non-const reference so
    // GoldmanWheelScreen needs no GameApp&. Built fresh on demand, same as
    // sctx()/scoreboard_state(). Field order MUST track GoldmanState's.
    return GoldmanState{goldman_lcg_, gold_player_, gold_prize_};
}

MatchRunnerState GameApp::match_runner_state() {
    // The match runtime's shared-state seam (ADR-0009 §10): the non-service members
    // run_match + start_match + collect_inputs + draw_player_row + draw_fps_overlay
    // read/write — the ticked sim_/renderer_, the round seed, the kill tally, the
    // F7/F8/F9 live levers (non-const: the global event filter flips them mid-match),
    // and the read-only MatchConfig inputs (scheme/tuning/options/roster/level/
    // campaign/gold). Bundled by reference so MatchRunner needs no GameApp&; built
    // fresh on demand, same as sctx()/scoreboard_state(). Field order MUST track
    // MatchRunnerState's.
    return MatchRunnerState{sim_,
                            *renderer_,  // NOLINT(bugprone-unchecked-optional-access)
                            next_seed_,
                            kill_count_,
                            uncap_fps_,
                            native_cadence_,
                            show_fps_,
                            scheme_,
                            base_tuning_,
                            options_,
                            conveyor_speed_index_,
                            setup_type_,
                            setup_sub_,
                            setup_team_,
                            win_count_,
                            team_play_,
                            campaign_active_,
                            attract_,
                            gold_player_,
                            gold_prize_,
                            selected_level_,
                            campaign_stages_,
                            campaign_stage_index_,
                            opts_.game_dir,
                            opts_.demo};
}

AppInput GameApp::present_screen(const ScreenDef& def) {
    return present_asset_screen(sctx(), def);
}

AppInput GameApp::present_bm_screen(const std::string& bm_name) {
    return BmTextScreen(sctx()).run(bm_name);
}

AppInput GameApp::present_options_screen() {
    return OptionsScreenRunner(sctx(), options_state()).run();
}

bool GameApp::reload_scheme_from_name(const std::string& name) {
    return reload_scheme(scheme_, opts_.game_dir, name);
}

AppInput GameApp::present_campaign_banner() {
    return CampaignBannerScreen(sctx(), campaign_state(), match_backdrop()).run();
}

AppInput GameApp::present_campaign_complete() {
    return CampaignCompleteScreen(sctx(), match_backdrop()).run();
}

// The six match-outcome predicates were promoted VERBATIM to free functions in
// bomber/game/match_outcome.hpp (ADR-0009 §10) so the extracted ScoreboardScreen
// and MatchRunner — which hold no GameApp& — can call the SAME clinch/outcome
// logic run_app uses. GameApp keeps these thin 1-line forwarders for its own last
// remaining caller, run_app (run_match / draw_player_row moved into MatchRunner
// and call the free functions directly); the full RE citations live on the free
// functions. Each forwarder qualifies the call (::bomber::game::) so it names the
// free function, not itself.
int GameApp::round_winner() const { return ::bomber::game::round_winner(sim_.state()); }

bool GameApp::campaign_no_human_survivor() const {
    return ::bomber::game::campaign_no_human_survivor(campaign_active_, sim_.state(), setup_type_);
}

bool GameApp::is_team_mode() const {
    return ::bomber::game::is_team_mode(team_play_, sim_.state(), setup_team_);
}

int GameApp::match_clinch() const {
    return ::bomber::game::match_clinch(sim_.state(), team_play_, setup_team_, options_.win_by_kills,
                                        kill_count_, win_count_, win_target_);
}

bool GameApp::auto_advance_results() const {
    return ::bomber::game::auto_advance_results(opts_.demo, opts_.demo_ticks, opts_.demo_shots,
                                                setup_type_);
}

AppInput GameApp::run_boot_attract() {
    return BootScreen(sctx()).run();
}

AppInput GameApp::present_menu() {
    // Forwarder to the extracted MenuScreen (ADR-0009 §6). The whole navigable
    // menu — MAINMENU backdrop + row highlight, the attract idle timer, and the
    // F10 / F1 / Alt+D / Ctrl+E×6 hidden triggers — plus its private
    // roll_attract_match now live in screens/menu_screen.cpp; run_app still
    // drives it through the same Menu->AppInput edge. restore_from_attract
    // (below) stays a GameApp method because run_app calls it on every path back
    // to the menu after an attract match.
    return MenuScreen(sctx(), menu_state()).run();
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
    // Forwarder to the free reset_match_scores() (match_outcome.hpp); writes
    // win_count_/kill_count_/win_target_ in place, so they pass by non-const ref.
    ::bomber::game::reset_match_scores(win_count_, kill_count_, win_target_, values_,
                                       num_to_win_match_);
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
    // Forwarder to the extracted ScoreboardScreen (ADR-0009 §8). The whole
    // between-round RESULTS tally — RESULTS.PCX, the per-player/team win+kill
    // rows in slot ink, and the clinch/outcome line (via the promoted
    // match_outcome.hpp predicates run_app also uses) — now lives in
    // screens/results_screens.cpp; run_app still drives it through the same
    // Results edge.
    return ScoreboardScreen(sctx(), scoreboard_state()).run();
}

// A random GLUE<n> backdrop (sub_4148E5 @0x4148E5): getvalue(16) = glue count,
// rand() % count, load GLUE<n>.PCX. Both pre-match screens share it. The pick is
// a presentation LCG (setup_lcg_), never State::rng.
// pick_glue moved to a shared free function in bomber/game/frontend_util.hpp so
// every pre-match screen (and the screens being lifted out of this file) share
// the one presentation-LCG advance — see that header. Call sites below pass
// setup_lcg_ + values_ explicitly.

// Forwarder to the extracted GoldmanWheelScreen (ADR-0009 §8). The wheel's spin
// setup (5 goldman_lcg_ draws), the per-frame advance/draw, the F1 help browser,
// and the Esc-abort gold_player_ forfeit / prize award now live in
// screens/results_screens.cpp; run_app's Menu/StartMatch handler still gates the
// call (options_.goldman && gold_player_ >= 0, doc §2) and drives it the same way.
AppInput GameApp::present_goldman_wheel() {
    return GoldmanWheelScreen(sctx(), goldman_state()).run();
}

AppInput GameApp::present_setup() {
    // Forwarder to the extracted SetupScreen (ADR-0009 §7). The whole PLAYER
    // INPUT TYPE SELECTION screen — the 10-slot input-type/team roster with its
    // per-slot colour tint, the two start guards, the F1 help browser, and the
    // hidden 'C'×5 campaign trigger (which builds a CampaignPickerScreen from the
    // CampaignState + MatchBackdrop threaded in) — plus its private
    // cycle_input_type helper now live in screens/setup_screen.cpp; run_app still
    // drives it through the same Play-flow edge (Advance to go on to the LEVEL
    // screen, Back to cancel to the menu).
    return SetupScreen(sctx(), setup_state(), campaign_state(), match_backdrop()).run();
}

AppInput GameApp::present_map_select() {
    // Forwarder to the extracted MapSelectScreen (ADR-0009 §7). The whole LEVEL &
    // ROUNDS screen — RANDOM + the 11 named levels, the win-target row, the
    // sample-block preview (with its shared-LCG per-cell re-rolls), and the F1
    // help browser — plus its level_fallback helper now live in
    // screens/map_select_screen.cpp; run_app still drives it through the same
    // Play-flow edge (Advance to start the match, Back to abort to the menu).
    return MapSelectScreen(sctx(), map_select_state()).run();
}

// Forwarder to the extracted MatchRunner (ADR-0009 §10). The whole match runtime —
// start_match's MatchConfig build + seed, the fixed-20Hz / F9 native-cadence tick
// loop, collect_inputs' per-tick input assembly, the player-row HUD + F8 fps
// overlay, and the in-round keys (Ctrl+Q/Esc abort, F1 help modal, the attract
// any-input abort) — now lives in screens/match_runner.cpp; run_app still drives it
// through the same Match edge. start_match stays a GameApp forwarder (above) because
// run()'s --demo path also builds a match through it; collect_inputs / draw_player_row
// / draw_fps_overlay had no other caller and moved wholesale into MatchRunner.
AppInput GameApp::run_match() {
    return MatchRunner(sctx(), match_runner_state()).run();
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
                    // (sub_406DDE), then the match. CORRECTED: Escape does NOT
                    // back up one step at a time — sub_406DDE is called from
                    // sub_410F81's own TAIL (pseudo.c 15504-15517, `if
                    // (!dword_464A68) { ...; sub_406DDE(); }`) with nothing
                    // after that call but `sub_401312()` and return, so an Esc
                    // on EITHER screen aborts the WHOLE Play flow straight back
                    // to the menu (same shape as the Goldman wheel's own Esc,
                    // doc §5) — there is no "go back to the player screen"
                    // path anywhere in the original. reset_match_scores()
                    // clears the tally; the level screen owns the win target
                    // so we reset FIRST, then let the level screen adjust
                    // win_target_.
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
                        if (wheelResult == AppInput::Back) {
                            ev = AppInput::Advance;
                            break;
                        }
                    }
                    bool started = false;
                    while (!started) {
                        AppInput setup = present_setup();
                        if (setup == AppInput::Quit) return 0;
                        if (setup == AppInput::Back) {
                            ev = AppInput::Advance;
                            break;
                        }
                        // Campaign mode SKIPS the LEVEL & ROUNDS screen
                        // entirely (docs/re/campaign.md "Skips the normal
                        // LEVEL & ROUNDS screen", sub_406DDE's `if
                        // (!dword_46489C)` gate): present_setup's own 'C'x5
                        // trigger already picked a stage and seeded the
                        // roster (present_campaign_picker), so a confirmed
                        // setup screen goes STRAIGHT to the match.
                        if (campaign_active_) {
                            started = true;
                            break;
                        }
                        // Player screen accepted -> the LEVEL screen. Esc here
                        // aborts the WHOLE flow (see the comment above this
                        // loop) -- NOT a loop back to present_setup — so this
                        // mirrors the wheel-abort and player-screen-Esc
                        // branches above, not the campaign short-circuit.
                        AppInput lvl = present_map_select();
                        if (lvl == AppInput::Quit) return 0;
                        if (lvl == AppInput::Back) {
                            ev = AppInput::Advance;
                            break;
                        }
                        started = true;  // both screens confirmed -> start the match
                    }
                    if (!started) ev = AppInput::Advance;  // cancelled all the way out
                }
                break;
            }
            case AppState::Match: ev = run_match(); break;
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
                    // MATCH win. sub_42A3F6 still renders the RESULTS scoreboard
                    // on the clinching round (with the "WINS THE MATCH!" outcome
                    // line) and plays the 2000 "we have a winner" voice UNDER it
                    // — the ONLY site that voice fires (batch_0x4293E5.cpp:1298,
                    // inside the v73 != -1 clinch branch) — THEN cuts to VICTORY.
                    // The port formerly skipped the scoreboard and jumped straight
                    // to VICTORY (and mis-fired 2000 on every round win too).
                    audio_.start_music(kDrawMusicId);  // 1130 under RESULTS/VICTORY (doc §2)
                    audio_.play_random_in_range(2000, 2299);  // winner voice — clinch only
                    ev = present_scoreboard();  // the clinch scoreboard (WINS THE MATCH!)
                    // Then VICTORY<player>.PCX / TEAM<0/1>.PCX (frontend-flow.md
                    // "VICTORY" §3, aTeamU vs aVictoryU).
                    if (ev != AppInput::Quit)
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
                        if (campaign_stage_index_ < static_cast<int>(campaign_stages_.size()) &&
                            load_campaign_stage(campaign_stage_index_, campaign_state())) {
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
                            // Whole campaign cleared: the "Congratulations! You
                            // made it through the whole campaign!" banner
                            // (sub_40133F, getstring 1220/1225) before returning
                            // to the menu.
                            if (present_campaign_complete() == AppInput::Quit) return 0;
                            campaign_active_ = false;  // dword_46489C = 0 (stage list exhausted)
                            campaign_stages_.clear();
                            campaign_stage_index_ = 0;
                        }
                    }
                } else if (w >= 0) {
                    // Round win, match not over: show the running scores. NO
                    // winner voice here — sub_42A3F6 fires sub_427BFB(2000) only
                    // in the clinch branch (v73 != -1); a non-clinching RESULTS
                    // pass (v73 == -1, batch_0x4293E5.cpp:1260-1272) plays no
                    // "we have a winner" cue. (The port formerly fired it every
                    // round win.)
                    audio_.start_music(kDrawMusicId);  // 1130 under RESULTS (doc §2 correction)
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
                    // sub_42A3F6's DRAW loop only auto-advances (6 s) for an
                    // all-AI/attract roster; a human match waits for Enter. A
                    // 0 dwell means "no auto-advance" in the Screen model
                    // (screen.cpp:47), so zero it out when a human is playing.
                    ScreenDef ds = draw_screen();
                    if (!auto_advance_results()) ds.dwell_ms = 0;
                    ev = present_screen(ds);
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
            case AppState::Options: ev = present_options_screen(); break;
            case AppState::Controllers: ev = present_bm_screen("INPUT"); break;
            case AppState::Network: ev = present_bm_screen("NETWORK"); break;
            case AppState::Credits: ev = present_bm_screen("CREDITS"); break;
            case AppState::Quit: break;
        }
        state = next(state, ev);
    }
    return 0;
}

// PORT ENHANCEMENT (task item 1, see init()'s window-creation comment): the
// Alt+Enter/F11 fullscreen toggle. Not an RE'd behaviour — the original has
// no fullscreen concept — so this lives outside any sub_XXXX-cited code path.
bool GameApp::handle_global_event(const SDL_Event& ev) {
    if (ev.type != SDL_EVENT_KEY_DOWN || ev.key.repeat)
        return true;  // keep; ignore key-repeat spam
    if (ev.key.key == SDLK_TAB) {
        toggle_hd_artwork();
        return false;  // presentation shortcut; never leak Tab into a screen
    }
    if (ev.key.key == SDLK_F8) {
        // Uncapped-framerate toggle (see uncap_fps_'s doc): flip the flag and
        // the renderer's vsync in lockstep. OFF = vsync on (present blocks on
        // vblank, refresh-boundary pacer caps at 60); ON = vsync off (present
        // returns immediately, the sub-frame pacer free-runs to ~180). The
        // run_match pacer reads uncap_fps_ every iteration, so this takes
        // effect on the next frame with no restart.
        uncap_fps_ = !uncap_fps_;
        SDL_SetRenderVSync(sdl_renderer_.get(), uncap_fps_ ? 0 : 1);
        std::fprintf(stderr, "framerate: %s\n", uncap_fps_ ? "uncapped (~180 fps, native feel)"
                                                           : "vsync (60 fps, smooth)");
        return false;  // presentation shortcut; never leak F8 into a screen
    }
    if (ev.key.key == SDLK_F7) {
        show_fps_ = !show_fps_;
        std::fprintf(stderr, "fps indicator: %s\n", show_fps_ ? "on" : "off");
        return false;  // presentation shortcut; never leak F7 into a screen
    }
    if (ev.key.key == SDLK_F9) {
        // Native-cadence toggle (see native_cadence_): sim runs per displayed
        // frame on the real wall-clock delta, drawn without interpolation. Takes
        // effect on the next frame in run_match (which reads native_cadence_).
        native_cadence_ = !native_cadence_;
        std::fprintf(stderr, "cadence: %s\n",
                     native_cadence_ ? "native per-frame wall-clock (non-deterministic)"
                                     : "fixed 20 Hz + interpolation (deterministic)");
        return false;  // presentation shortcut; never leak F9 into a screen
    }
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
    // untouched (SDL_LOGICAL_PRESENTATION_STRETCH fills any output size edge
    // to edge, matching the original's own presentation — see init()).
    SDL_SetWindowFullscreen(window_.get(), fullscreen_);
    options_dirty_ = true;  // persist the choice (task item 4), flush_options() below is the writer
}

void GameApp::toggle_hd_artwork() {
    assets_.set_hd_enabled(!assets_.hd_enabled());
    SDL_SetWindowTitle(window_.get(), assets_.hd_enabled() ? "Atomic Bomberman [HD]"
                                                           : "Atomic Bomberman [Classic]");
    std::fprintf(stderr, "artwork mode: %s\n", assets_.hd_enabled() ? "HD" : "classic");
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
    // Rows 10/12/17 (docs/re/results-and-options.md §3) — LIVE toggles as of
    // the 2026-07-09 full-audit pass; round-tripped like every other row now.
    to_write.assign_keyboards = options_.assign_keyboards;
    to_write.lost_net_revert_ai = options_.lost_net_revert_ai;
    to_write.smallmemory = options_.small_memory;
    // Row 8 — round-trip whatever is currently shown (the picker stores it
    // extension-stripped + uppercased; a hand-edited value survives as-is).
    if (!options_.scheme_filename.empty()) to_write.schemefilename = options_.scheme_filename;
    to_write.diseases_destroyable = options_.diseases_destroyable;
    to_write.disable_game_music = options_.disable_game_music;
    // "fullscreen=" — PORT-ONLY key (see init()'s and toggle_fullscreen()'s
    // comments), always mirrored alongside the RE'd keys above.
    to_write.fullscreen = fullscreen_;
    // PORT-ONLY Video Settings keys (install.hpp). vsync is the inverse of the
    // internal uncapped-fps flag.
    to_write.vsync = !uncap_fps_;
    to_write.native_cadence = native_cadence_;
    to_write.show_fps = show_fps_;
    // keydef=: always write the live KeyboardMapper bindings (both sets, all
    // 6 UI-exposed actions) so a rebind through the remap screen survives a
    // restart — translated back into the ORIGINAL's DOS/AT scancode space
    // (dos_scancode.hpp; before 2026-07-13 the port wrote raw SDL_Scancode
    // values here, which BM95.EXE would misread against the same install).
    // An SDL key with no DOS equivalent writes 0 = unbound. Slots 6-9 per
    // set (no in-game UI, §2) are left at -1/absent here — save_options
    // skips a -1 scancode, so any pre-existing keydef= line for those slots
    // (from a hand-edit or a future feature) is left untouched by the
    // read-modify-write rather than being clobbered blank.
    assets::KeyDef kd;
    for (int set = 0; set < assets::KeyDef::kSets; ++set) {
        const KeySet& ks = keyboard_.key_set(set);
        for (int action = 0; action < kKeyActionCount; ++action)
            kd.scancode[set][action] = dos_scancode_from_sdl(ks.scancode[action]);
    }
    to_write.keydef = kd;
    try {
        assets::save_options(options_path_, to_write);
        options_dirty_ = false;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "options.ini save failed: %s\n", e.what());
    }
}

int GameApp::run_bm_shot() {
    // Mirror present_bm_screen's one-frame composite (MAINMENU backdrop +
    // BmScreen::draw), scrolled bm_shot_scroll lines down, then SaveBMP —
    // a headless snapshot of the sub_41302D viewer for layout verification.
    BmScreen bm(assets_, front_font_);
    bm.enter(opts_.bm_shot_name);
    for (int i = 0; i < opts_.bm_shot_scroll; ++i) bm.on_key(SDLK_DOWN);
    SDL_SetRenderDrawColor(sdl_renderer_.get(), 0, 0, 0, 255);
    SDL_RenderClear(sdl_renderer_.get());
    const Sprite& bg = assets_.frontend_pcx("MAINMENU");
    if (bg.tex) {
        SDL_FRect d{0, 0, static_cast<float>(bg.w), static_cast<float>(bg.h)};
        SDL_RenderTexture(sdl_renderer_.get(), bg.tex, nullptr, &d);
    }
    bm.draw(sdl_renderer_.get());
    // Read the backbuffer BEFORE presenting (SDL swaps on present, leaving the
    // read target undefined) — the same order run_demo's capture relies on.
    bool ok = save_screenshot(opts_.bm_shot_out);
    if (ok)
        std::printf("bm-shot: %s scroll %d -> %s\n", opts_.bm_shot_name.c_str(),
                    opts_.bm_shot_scroll, opts_.bm_shot_out.string().c_str());
    return ok ? 0 : 1;
}

int GameApp::run_menu_shot() {
    // One-frame headless snapshot of the main menu — the SAME composite
    // present_menu draws (MAINMENU backdrop + "V1.0" + the animated trigger
    // cursor), pinned to row 0 and animation frame 0 for determinism. The
    // ground-truth reference is the native oracle's `bm_native --boot-shot`,
    // which renders the ORIGINAL's own menu through the DirectDraw->SDL3 shim.
    menu_index_ = 0;
    SDL_SetRenderDrawColor(sdl_renderer_.get(), 0, 0, 0, 255);
    SDL_RenderClear(sdl_renderer_.get());
    const Sprite& bg = assets_.frontend_pcx("MAINMENU");
    if (bg.tex) {
        SDL_FRect d{0, 0, static_cast<float>(bg.w), static_cast<float>(bg.h)};
        SDL_RenderTexture(sdl_renderer_.get(), bg.tex, nullptr, &d);
    }
    front_font_.draw_outlined(sdl_renderer_.get(), "V1.0", 0, 0, 168, 168, 164, 0, 0, 0, 50.0f);
    const int cx = static_cast<int>(values_.column_or(700, 0, kMenuCursorXFallback));
    const int cy = static_cast<int>(values_.column_or(700, 1, kMenuCursorYFallback));
    Anim cur = resolve_sequence(assets_.trigbomb(-1), "bomb trigger green");
    if (!cur.steps.empty()) {
        const Sprite& sp = cur.steps[anim_step_index(0, cur.steps.size())];
        if (sp.tex) {
            SDL_FRect d{static_cast<float>(cx - sp.hx), static_cast<float>(cy - sp.hy),
                        static_cast<float>(sp.w), static_cast<float>(sp.h)};
            SDL_RenderTexture(sdl_renderer_.get(), sp.tex, nullptr, &d);
        }
    }
    bool ok = save_screenshot(opts_.menu_shot_out);
    if (ok) std::printf("menu-shot -> %s\n", opts_.menu_shot_out.string().c_str());
    return ok ? 0 : 1;
}

int GameApp::run() {
    if (const char* env = std::getenv("BOMBER_BOOT_MATCH"); env && *env) opts_.boot_match = true;
    if (!init()) return opts_.game_dir.empty() ? 2 : 1;
    if (!opts_.bm_shot_name.empty()) {
        int rc = run_bm_shot();
        flush_options();
        return rc;
    }
    if (!opts_.menu_shot_out.empty()) {
        int rc = run_menu_shot();
        flush_options();
        return rc;
    }
    if (opts_.demo) {
        if (!opts_.demo_shot_dir.empty()) {
            std::error_code ec;
            fs::create_directories(opts_.demo_shot_dir, ec);  // ignore: SDL_SaveBMP reports failure
        }
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
