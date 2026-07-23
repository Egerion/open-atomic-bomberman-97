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
#include "bomber/game/screens/asset_screen.hpp"
#include "bomber/game/screens/boot_screen.hpp"
#include "bomber/game/screens/campaign_screens.hpp"
#include "bomber/game/screens/debug_info_screen.hpp"
#include "bomber/game/screens/editor_screen_runner.hpp"
#include "bomber/game/screens/help_screens.hpp"
#include "bomber/game/screens/map_select_screen.hpp"
#include "bomber/game/screens/menu_screen.hpp"
#include "bomber/game/screens/options_screens.hpp"
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
    SDL_Renderer* ren = nullptr;
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
// kBootMusicId (1000) moved to screens/boot_screen.cpp with run_boot_attract.
// kMenuMusicId (1010, 0x3F2 MENU.RSS) moved to screens/menu_screen.cpp with present_menu.
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
constexpr int kWinMusicId = 1020;   // 0x3FC — WIN.RSS, setup-screens backdrop (NOT victory)
constexpr int kDrawMusicId = 1130;  // 0x46A — DRAW.RSS, DRAW *and* RESULTS *and* VICTORY backdrop
// Per-level in-round stage track fallback (sub_4293E5, docs/re/
// in-match-shell.md §2): SOUNDLST 1100+level, or this id when the level has
// no entry (a stripped/minimal-install SOUNDLST — every built-in stage here
// has a real 1100..1110 entry).
constexpr int kStageMusicFallback = 1120;  // 0x460 — GENERIC.RSS
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

void GameApp::start_match(std::uint32_t seed) {
    // Random Start (options.ini "random_start=" / Options row 1, §3):
    // shuffles which of the scheme's own spawn slots each player index gets
    // — CONFIRMED as the original's 200-pair-swap over the 10 start slots
    // (sub_421793; match_factory.hpp mirrors the loop, docs/re/facts.md
    // "Options toggles").
    sim::MatchConfig cfg =
        match::build_match_config(scheme_, sim::kMaxPlayers, seed, &values_, options_.random_start);
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
    cfg.tuning.game_seconds = options_.playtime_seconds == 1001 ? 99999 : options_.playtime_seconds;
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
                        cfg.born_with_clogs[i] =
                            1;  // reset-then-+1 every round, §9.3 — not accumulated
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
    // The sim's per-level gates (tile regeneration ids 340-350/695, ice/
    // input-lag ids 450-460 — docs/re/facts.md "Per-level tile regeneration",
    // "Ice / input-lag") are indexed by the SAME stage number as dword_46499C
    // in the original, i.e. exactly this `stage` value.
    cfg.tuning.level_index = stage;
    // Overlay this board's stage actors (conveyors/trampolines/etc) from
    // EXTRA<stage>.RES before constructing the sim — the actor layout is a
    // hashed setup input like the cell grid (docs/re/stage-actors.md). A board
    // with no EXTRA file simply has none. Random '-T,H' trampolines resolve off
    // a setup-only RNG inside apply_actors, never the sim's per-tick stream.
    auto actors =
        assets::extra::load_for_board(opts_.game_dir, stage, sim::kGridWidth, sim::kGridHeight);
    match::apply_actors(cfg, actors, seed);
    // Campaign rover/ghost hazards (docs/re/campaign.md "Rover/ghost/AI
    // roster", "sub_40151B — the REAL per-stage starter"): fields 3-6 of the
    // current stage's .CAM record. build_state (setup.cpp) spawns them (ghost
    // first, then rover, matching sub_40151B's own call order) as part of
    // Simulation's constructor. A non-campaign match leaves these at 0
    // (MatchConfig's default), so RoverSystem::spawn/tick are true no-ops.
    if (campaign_active_ && campaign_stage_index_ >= 0 &&
        campaign_stage_index_ < static_cast<int>(campaign_stages_.size())) {
        const assets::res::CampaignStage& stage_rec =
            campaign_stages_[static_cast<std::size_t>(campaign_stage_index_)];
        cfg.campaign_rovers = stage_rec.rovers;
        cfg.campaign_rover_speed = stage_rec.rover_speed;
        cfg.campaign_ghosts = stage_rec.ghosts;
        cfg.campaign_ghost_speed = stage_rec.ghost_speed;
    }
    // --demo / --demo-shots: disarm the round-start input freeze (VALUELST
    // id 30 ≈ 1 s of dead input, facts.md "Round-start input freeze") — the
    // scripted demo match's tick-indexed input script and the visual-golden
    // shot ticks (tests/visual/shots.txt) were all captured acting from tick
    // 0, and shifting the whole choreography by 20 ticks would re-time every
    // pinned frame for no coverage gain. A demo-fixture pin like the
    // LETTERBOX scaler in init(); live play keeps the authentic freeze.
    if (opts_.demo) cfg.tuning.input_freeze_ticks = 0;
    sim_ = sim::Simulation(cfg);
    if (assets_.load_stage(stage)) {
        seqs_.resolve_stage(assets_, stage);
        // Disable music during gameplay (options.ini "disable_game_music=" /
        // Options row 13, §3): the original's round init (sub_410B6E
        // LABEL_48) FREES the music outright (sub_427342) when the option is
        // set — the round is SILENT, the setup-screens track (1020) does not
        // bleed into it. Menu/results music is untouched (the option is
        // specifically "during gameplay"; round end starts 1130 regardless).
        //
        // Per-level stage track (docs/re/in-match-shell.md §2, sub_4293E5):
        // SOUNDLST 1100+level, falling back to 1120 ("generic") when the level
        // has no entry — our 11 built-in stages all have one (SOUNDLST.RES
        // 1100..1110), so this only matters for a stripped/modified install.
        if (!options_.disable_game_music) {
            int stage_music = 1100 + stage;
            if (!audio_.has_track(stage_music)) stage_music = kStageMusicFallback;  // 1120
            audio_.start_music(stage_music);
        } else {
            audio_.stop_music();  // sub_427342: silent round, not "keep 1020 playing"
        }
    }
    // Untimed round HUD (docs/re/in-match-shell.md §3): the 1001 sentinel is a
    // presentation-only concept (see cfg.tuning.game_seconds's own comment
    // just above — the sim gets a very long but finite clock instead), so
    // tell the renderer directly rather than trying to infer "untimed" back
    // out of ticks_left.
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access) — emplaced in init()
    renderer_->reset_match(options_.playtime_seconds == 1001);
    sounds_.reset();
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

EditorEditState GameApp::editor_state() {
    // The scheme editor's shared-state seam (ADR-0009 §9): the non-service
    // members present_editor reads/writes, bundled by reference so the
    // EditorRunner needs no GameApp&. Built fresh on demand, same as sctx().
    return EditorEditState{setup_lcg_, scheme_, opts_.game_dir, opts_.scheme};
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

AppInput GameApp::present_screen(const ScreenDef& def) {
    return present_asset_screen(sctx(), def);
}

AppInput GameApp::present_bm_screen(const std::string& bm_name) {
    return BmTextScreen(sctx()).run(bm_name);
}

AppInput GameApp::present_help_browser() {
    return HelpBrowserScreen(sctx()).run();
}

AppInput GameApp::present_help_browser_modal() {
    return HelpBrowserModal(sctx(), match_backdrop()).run();
}

AppInput GameApp::present_debug_info_modal() {
    return DebugInfoScreen(sctx()).run();
}

void GameApp::present_video_settings() {
    // PORT-ONLY screen (NOT RE'd) — the video/cadence toggles that otherwise
    // only live on the F7/F8/F9 keys (show_fps_/uncap_fps_/native_cadence_),
    // surfaced as a small panel and persisted via the Video Settings keys
    // (install.hpp). Kept SEPARATE from the RE'd Options screen so its exact 18
    // rows stay faithful (no invented rows there — the deliberate design choice
    // for these modern-only settings). Same WINZ-panel modal shape as the
    // Alt+D debug window above; Up/Down select, Enter/Space/Left/Right toggle,
    // Esc closes. Toggles apply live and mark options_dirty_ so flush_options
    // round-trips them.
    VideoSettingsScreen(sctx(), {&uncap_fps_, &native_cadence_, &show_fps_, &options_dirty_}).run();
}

AppInput GameApp::present_options_screen() {
    return OptionsScreenRunner(sctx(), options_state()).run();
}

bool GameApp::reload_scheme_from_name(const std::string& name) {
    return reload_scheme(scheme_, opts_.game_dir, name);
}

std::string GameApp::present_scheme_filename_prompt(const std::string& seed) {
    return SchemeFilenamePrompt(sctx()).run(seed);
}

void GameApp::present_editor() {
    // Forwarder to the extracted EditorRunner (ADR-0009 §9). Still a GameApp
    // method because present_menu (not yet extracted) calls it from its own
    // event pump; inlined away once present_menu itself moves out.
    EditorRunner(sctx(), editor_state()).run();
}

void GameApp::present_campaign_picker() {
    CampaignPickerScreen(sctx(), campaign_state(), match_backdrop()).run();
}

AppInput GameApp::present_campaign_banner() {
    return CampaignBannerScreen(sctx(), campaign_state(), match_backdrop()).run();
}

AppInput GameApp::present_campaign_complete() {
    return CampaignCompleteScreen(sctx(), match_backdrop()).run();
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
    // §1 v73 (sub_42A3F6, batch_0x4293E5.cpp:1189-1255): the clinch splits on
    // TEAM mode (dword_464964). The team branch is ALWAYS wins-based; the
    // kill-count clinch (win_by_kills_clinch: highest round-kill total >=
    // target, unique leader v78==1) lives ONLY in the NON-team branch's
    // dword_46497C sub-case (line 1243). win_by_kills is inherently a non-team
    // feature — team play forces it OFF (batch_0x405B3A.cpp:685-686
    // `if (dword_464964) dword_46497C = 0;`, mirrored at
    // options_screen.cpp's activate_row). The old `is_team_mode() &&
    // win_by_kills` gate was therefore DEAD (never true), silently falling the
    // "Win Matches By Kill Total" mode through to the round-win loop. Both call
    // sites (run_app's Results handler and present_scoreboard) share this gate.
    const sim::State& s = sim_.state();
    if (!is_team_mode() && options_.win_by_kills) {
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

bool GameApp::auto_advance_results() const {
    // sub_42A3F6's DRAW and RESULTS wait loops honour the 6 s auto-advance ONLY
    // when `sub_42247A() || dword_4646B4` (batch_0x4293E5.cpp:1109/1333):
    // sub_42247A returns 1 iff NO slot is human (every +16 type is OFF=0 or
    // CPU=1 — batch_0x421E80.cpp), and dword_4646B4 is the attract/demo flag. A
    // human match instead waits indefinitely for Enter. Our attract path is
    // all-AI and the --demo/--demo-shots path is scripted, so both collapse to:
    // auto-advance unless a real human (KEYBOARD=2 / JOYSTICK=3) is playing.
    if (opts_.demo || opts_.demo_ticks > 0 || !opts_.demo_shots.empty()) return true;
    for (int i = 0; i < sim::kMaxPlayers; ++i)
        if (setup_type_[i] == 2 || setup_type_[i] == 3) return false;  // a human slot
    return true;  // all-AI roster
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
        // sub_42A3F6's RESULTS loop only auto-advances after 6 s for an all-AI/
        // attract roster; a human match waits for Enter (auto_advance_results()).
        if (auto_advance_results() && SDL_GetTicks() - start >= kResultsDwellMs)
            waiting = false;
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
        // sub_41696C (batch_0x4293E5.cpp:1171) — outlined, like every scoreboard
        // string; the color2 outline is byte_495390[0] = black.
        front_font_.draw_outlined(sdl_renderer_.get(), header, hx, hy, kHeaderR, kHeaderG,
                                  kHeaderB, 0, 0, 0);

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
                std::string line =
                    fmt_u(assets_.getstring(38, "Team %u score: %u"), static_cast<unsigned>(t + 1));
                // getstring(38) carries one %u (team number); splice the score
                // in after it manually since fmt_u only substitutes the first.
                line += " " + std::to_string(win_count_[i]);
                const bool team1 = t != 0;  // sub_4141F8's `a1 ?` branch
                const std::uint8_t c[3] = {static_cast<std::uint8_t>(team1 ? 252 : 255),
                                           static_cast<std::uint8_t>(team1 ? 80 : 255),
                                           static_cast<std::uint8_t>(team1 ? 80 : 255)};
                front_font_.draw_outlined(sdl_renderer_.get(), line, rx,
                                          ry0 + rystep * static_cast<float>(row), c[0], c[1], c[2],
                                          0, 0, 0);
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
                // Outline colour = sub_416867(i) (batch_0x415C1F.cpp:394): in
                // solo mode player 1 (the BLACK bomberman, index 1) gets a WHITE
                // outline (byte_49D38F) so its dark ink stays legible; everyone
                // else gets black (byte_495390[0]). (Team rows + header + outcome
                // are always black.)
                const std::uint8_t ol = i == 1 ? 255 : 0;
                front_font_.draw_outlined(sdl_renderer_.get(), line, rx,
                                          ry0 + rystep * static_cast<float>(row), c[0], c[1], c[2],
                                          ol, ol, ol);
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
                // Pre-clinch line (batch_0x4293E5.cpp:1262-1263):
                // getstring(dword_46497C + 120) formatted with the FLAT target
                // v60 = dword_464A7C — NOT the remaining count. The strings are
                // 120 "(Match winner must score %u victories)" / 121 "(... %u
                // kills)", so the id keys off win_by_kills (a non-team feature),
                // not team mode, and always shows the total goal.
                std::string fmt =
                    options_.win_by_kills
                        ? assets_.getstring(121, "(Match winner must score %u kills)")
                        : assets_.getstring(120, "(Match winner must score %u victories)");
                outcome = fmt_u(fmt, static_cast<unsigned>(win_target_));
                oc[0] = 168;
                oc[1] = 168;
                oc[2] = 164;  // byte_49A624: RGB555 (20,20,20) grey
            } else {
                // Clinch line (batch_0x4293E5.cpp:1300-1310): win_by_kills ->
                // getstring(36) "PLAYER %u WINS THE MATCH!" with the winning
                // player NUMBER (v73+1); else getstring(35) "%s WINS THE MATCH!"
                // with the winner name. The native has NO team-specific win
                // string here — the former `team_mode ? "TEAM %u WINS"` gate
                // AND that fallback text were both invented (id 36 is "PLAYER
                // %u", and the selector is win_by_kills, not team mode).
                if (options_.win_by_kills) {
                    std::string fmt = assets_.getstring(36, "PLAYER %u WINS THE MATCH!");
                    outcome = fmt_u(fmt, static_cast<unsigned>(clinched_player + 1));
                } else {
                    std::string fmt = assets_.getstring(35, "%s WINS THE MATCH!");
                    outcome = fmt_s(fmt, "P" + std::to_string(clinched_player + 1));
                }
                oc[0] = 96;
                oc[1] = 252;
                oc[2] = 252;  // byte_497F8F: RGB555 (10,31,31) cyan
            }
            front_font_.draw_outlined(sdl_renderer_.get(), outcome, ox, oy, oc[0], oc[1], oc[2], 0,
                                      0, 0);
        }

        SDL_RenderPresent(sdl_renderer_.get());
        SDL_Delay(2);
    }
    return result;
}

// A random GLUE<n> backdrop (sub_4148E5 @0x4148E5): getvalue(16) = glue count,
// rand() % count, load GLUE<n>.PCX. Both pre-match screens share it. The pick is
// a presentation LCG (setup_lcg_), never State::rng.
// pick_glue moved to a shared free function in bomber/game/frontend_util.hpp so
// every pre-match screen (and the screens being lifted out of this file) share
// the one presentation-LCG advance — see that header. Call sites below pass
// setup_lcg_ + values_ explicitly.

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
    // Refresh-boundary pacing (see refresh_period_ns): the wheel advances one
    // spin step per wheel.tick(), so a blind SDL_Delay(2) free-running at
    // 300-500 Hz on Windows spun it far too fast. Pace to the real refresh.
    platform::FrameClock frame_clock(window_.get());
    while (!wheel.done()) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_EVENT_QUIT) return AppInput::Quit;
            if (ev.type != SDL_EVENT_KEY_DOWN) continue;
            const SDL_Keycode k = ev.key.key;
            if (k == SDLK_F1) {
                // doc §5: F1 opens the SAME generic *.BM help browser
                // (sub_41431C) every other F1 site opens — the old fixed
                // OPTIONS.BM cut here was a stale stand-in (chrome audit
                // 2026-07-12, fix list item 9).
                AppInput help = present_help_browser();
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
        frame_clock.pace();
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
            case SlotInputType::Joystick: in.players[i] = gamepads_.read(setup_sub_[i]); break;
            default: break;  // Off/Computer/Other: neutral — AI or absence owns the slot
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
        int col = i / 2;  // getvalue(115 + i/2): 5 columns, VALUELST 10/110/210/310/410
        int row = i & 1;  // getvalue(113 + i&1): 2 rows, VALUELST 6/26
        float x = static_cast<float>(values_.column_or(115 + col, 0, 10 + 100 * col));
        float y = static_cast<float>(values_.column_or(113 + row, 0, 6 + 20 * row));

        std::string line = assets_.getstring(37, "S:%d K:%d");
        splice_next(line, win_count_[i]);
        splice_next(line, kill_count_[i]);
        std::uint8_t c[3];
        assets_.slot_color(i, c);
        // In-match "S:x K:y" score overlay: sub_41696C (batch_0x420D4E.cpp's
        // per-frame count-alive loop) — ink sub_41672F(i) == slot_color, outline
        // sub_416867(i): black in team mode, else white for player 1 (the black
        // bomberman) and black for everyone else. (Not in the demo/golden path —
        // run_app draws it, run_demo does not.)
        const std::uint8_t ol = (!is_team_mode() && i == 1) ? 255 : 0;
        front_font_.draw_outlined(sdl_renderer_.get(), line, x, y, c[0], c[1], c[2], ol, ol, ol);

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

void GameApp::draw_fps_overlay(int fps) {
    if (!show_fps_ || !front_font_.loaded()) return;
    // Three compact lines hard in the top-right corner, stacked: fps, then the
    // cadence state, then the vsync state. Right-aligned and drawn at a reduced
    // SCALE via FontTextures::draw's `scale` (dst-rect only — NEVER
    // SDL_SetRenderScale, which perturbed the whole render transform). Small
    // enough that all three sit ABOVE the match clock rather than over it. GREEN
    // marks the native-feel state of each lever; a manual 1-px black outline
    // keeps them legible over the field.
    constexpr float kS = 0.7f;
    const float right = static_cast<float>(kScreenW) - 3.0f;
    const float lh = static_cast<float>(front_font_.line_height()) * kS;
    auto line = [&](const std::string& s, float y, bool hot) {
        const float x = right - static_cast<float>(front_font_.measure(s)) * kS;
        front_font_.draw(sdl_renderer_.get(), s, x - 1, y, 0, 0, 0, kS);
        front_font_.draw(sdl_renderer_.get(), s, x + 1, y, 0, 0, 0, kS);
        front_font_.draw(sdl_renderer_.get(), s, x, y - 1, 0, 0, 0, kS);
        front_font_.draw(sdl_renderer_.get(), s, x, y + 1, 0, 0, 0, kS);
        front_font_.draw(sdl_renderer_.get(), s, x, y, hot ? 120 : kDialogInkR, hot ? 240 : kDialogInkG,
                         hot ? 120 : kDialogInkB, kS);
    };
    char buf[24];
    std::snprintf(buf, sizeof(buf), "%d FPS", fps);
    float y = 2.0f;
    line(buf, y, uncap_fps_);
    y += lh;
    line(native_cadence_ ? "NATIVE" : "20HZ", y, native_cadence_);
    y += lh;
    line(uncap_fps_ ? "UNCAP" : "VSYNC", y, uncap_fps_);
}

AppInput GameApp::run_match() {
    start_match(next_seed_++);
    const std::uint64_t tick_ns = 1'000'000'000ull / sim::kTicksPerSecond;
    std::uint64_t last = SDL_GetTicksNS();
    std::uint64_t acc = 0;
    int over_ticks = -1;
    // Frame pacing (docs/re/in-match-shell.md's per-frame tick driver,
    // sub_42A191/sub_41E61E: the original is a DirectDraw flip loop — one
    // input read + at most one tick per DISPLAYED frame, the flip block IS
    // the throttle). GameApp::init() requests vsync (SDL_SetRenderVSync, its
    // comment explains why), but on Windows windowed mode SDL_RenderPresent
    // does NOT reliably block: DWM gives the swapchain a multi-frame flip
    // queue, so presents return instantly in bursts (measured 4-12 ms frame
    // deltas) until the queue fills, then stall (20-25 ms). The sim
    // accumulator crossings then land on that jerky CPU-side train and ticks
    // get assigned to frames in 2/4-frame beats instead of the steady
    // 3-frames-per-tick a 20 Hz sim on a 60 Hz display needs — measured with
    // the same live-run rig as the 2026-07-10 input-latency audit: 4-41% of
    // tick-to-tick gaps were a frame off (visible micro-stutter), whether or
    // not the old blind SDL_Delay(2) throttle ran after present. The fix is
    // explicit pacing: sleep until the next display-refresh boundary after
    // each present (SDL_DelayNS, target advanced by the measured refresh
    // period). When present genuinely blocks on vblank the target is already
    // reached and the sleep is a no-op (the resync branch keeps the target
    // phase-locked to the real vblank train); when it doesn't block, the
    // sleep supplies exactly the cadence vsync failed to. Input latency is
    // unchanged versus a truly-blocking vsync — one SDL_PollEvent + one
    // collect_inputs() sample per displayed frame either way, the original's
    // own acquisition bound — and no fixed extra delay sits on that path.
    // Refresh-rate mismatch (59.94 Hz panel reported as 60, VRR) only drifts
    // the target phase; the resync branch absorbs it. Unknown refresh falls
    // back to 60 Hz, which still bounds the loop (no uncapped free-run on
    // drivers where SDL_SetRenderVSync is a no-op, e.g. dummy video).
    std::uint64_t period_ns = 1'000'000'000ull / 60;
    if (const SDL_DisplayMode* mode =
            SDL_GetCurrentDisplayMode(SDL_GetDisplayForWindow(window_.get()));
        mode && mode->refresh_rate_numerator > 0 && mode->refresh_rate_denominator > 0) {
        period_ns = 1'000'000'000ull * mode->refresh_rate_denominator /
                    static_cast<std::uint64_t>(mode->refresh_rate_numerator);
    }
    std::uint64_t pace_target_ns = SDL_GetTicksNS() + period_ns;
    // F8 FPS-indicator state: count presented frames and refresh the shown
    // figure ~4x/second (a 250 ms window) so the number is readable, not a
    // blur. Purely for the top-right overlay; nothing gameplay reads it.
    std::uint64_t fps_frames = 0;
    std::uint64_t fps_window_start_ns = SDL_GetTicksNS();
    int shown_fps = 0;
    // Per-player "action key seen down at a frame sample since the last
    // consumed tick" — the frame-cadence tap capture; see the sampling
    // comment inside the loop. Only action1/action2 are ever set.
    struct TapLatch {
        bool action1 = false, action2 = false;
    };
    std::array<TapLatch, sim::kMaxPlayers> tap_latch{};
    // Round-end / linger bookkeeping for ONE advanced 50 ms tick. Called from
    // both the fixed-tick catch-up loop and the F9 native-cadence path (once per
    // 50 ms systems pass). Returns true when the post-round linger has elapsed
    // and run_match should hand back to the Results flow.
    auto advance_round_end = [&]() -> bool {
        const sim::State& s = sim_.state();
        // Campaign hazard-clear grace timer (docs/re/campaign.md "Round pacing"
        // clause 3, sub_4016DA's dword_4646C0): fires once when every hazard has
        // been dead kHazardClearTicks ticks — an independent early-out.
        if (over_ticks < 0 && campaign_active_ &&
            s.hazard_clear_timer == sim::kHazardClearTicks) {
            over_ticks = 3 * sim::kTicksPerSecond;
        }
        // Team-aware round-over: "one SIDE left" (docs/re/ai.md TEAM follow-up);
        // sides_remaining() degenerates to alive_count() in a solo match.
        if (over_ticks < 0 && (sim::sides_remaining(s) <= 1 || s.ticks_left == 0)) {
            over_ticks = 3 * sim::kTicksPerSecond;
            if (s.ticks_left == 0) {
                std::printf("time up — draw!\n");
            } else {
                for (int i = 0; i < sim::kMaxPlayers; ++i)
                    if (s.players[i].present && s.players[i].alive)
                        std::printf("player %d wins!\n", i);
            }
        }
        return over_ticks > 0 && --over_ticks == 0;
    };
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
            if (attract_ &&
                (ev.type == SDL_EVENT_KEY_DOWN || ev.type == SDL_EVENT_MOUSE_BUTTON_DOWN ||
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
                last = SDL_GetTicksNS();
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

        // Frame-cadence action-key capture (docs/re/in-match-shell.md "Input
        // acquisition"). The ORIGINAL samples its live key-state array
        // byte_4A2BA0 (maintained by sub_433E14 from DirectInput BUFFERED
        // records — sub_43B5EC/sub_43B700/sub_4449B0's GetDeviceData drain)
        // once per DISPLAYED FRAME: sub_41F29B shuffles the key bytes
        // (+54=+56, then +56=0; pseudo.c 22976-22979) and re-reads them via
        // sub_41E61E (23037) in the same per-frame callback, so its
        // edge-gated bomb drop can only miss a tap shorter than ONE display
        // frame (~14-16 ms). Feeding the sim a state sample taken only once
        // per 50 ms tick widened that loss window ~3x — a normal human tap
        // (~30-40 ms) could fall entirely between two tick samples and the
        // bomb press silently vanished. Restore the original's cadence:
        // sample the mapped inputs here, once per rendered frame (this loop
        // is the port's equivalent of the flip-loop callback), and latch
        // action-key downs until the next tick consumes them. Directions are
        // deliberately NOT latched: they are level-driven (the original
        // integrates held time in ms, so a sub-tick tap moved a few px at
        // most — stretching it to a full 50 ms tick budget would overshoot
        // the original far more than dropping it does), while action1/2 are
        // EDGE-consumed — capture-or-lose — which is exactly what the frame
        // sampling exists to capture.
        const sim::TickInputs frame_in = collect_inputs();
        for (int i = 0; i < sim::kMaxPlayers; ++i) {
            tap_latch[i].action1 = tap_latch[i].action1 || frame_in.players[i].action1;
            tap_latch[i].action2 = tap_latch[i].action2 || frame_in.players[i].action2;
        }

        std::uint64_t now = SDL_GetTicksNS();
        const std::uint64_t delta_ns = now - last;
        acc += delta_ns;
        last = now;
        // One loop iteration == one SDL_RenderPresent below; tally it and
        // recompute the shown rate once the 250 ms window elapses.
        ++fps_frames;
        if (const std::uint64_t span = now - fps_window_start_ns; span >= 250'000'000ull) {
            shown_fps = static_cast<int>(fps_frames * 1'000'000'000ull / span);
            fps_frames = 0;
            fps_window_start_ns = now;
        }
        if (native_cadence_) {
            // F9 native-cadence path: advance the sim ONE displayed frame on the
            // measured wall-clock delta. Simulation::frame runs the movement/AI
            // pass at frame rate and drains the 50 ms systems pass off its own
            // accumulator, so this is the original's per-frame gameplay driver
            // (sub_42A191) — low input latency, fps-scaled granularity — but
            // NON-DETERMINISTIC (real delta). Consume the action-key taps this
            // frame; run the round-end bookkeeping once per 50 ms tick advanced.
            std::int32_t delta_ms = static_cast<std::int32_t>(delta_ns / 1'000'000ull);
            if (delta_ms < 1) delta_ms = 1;
            if (delta_ms > 4 * sim::kMsPerTick) delta_ms = 4 * sim::kMsPerTick;
            sim::TickInputs in = frame_in;
            for (int i = 0; i < sim::kMaxPlayers; ++i) {
                in.players[i].action1 = in.players[i].action1 || tap_latch[i].action1;
                in.players[i].action2 = in.players[i].action2 || tap_latch[i].action2;
                tap_latch[i].action1 = false;
                tap_latch[i].action2 = false;
            }
            const std::uint64_t tick_before = sim_.state().tick;
            sim_.frame(in, delta_ms);
            sounds_.on_tick(sim_.state());
            // Pose countdowns age once per SIM TICK, not per displayed frame:
            // pass whether this frame actually crossed a tick (else kick/punch/
            // pickup poses play ~9x too fast in native cadence).
            // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
            renderer_->on_events(sim_.state(), sim_.state().tick != tick_before);
            renderer_->advance_tick(sim_.state());  // NOLINT(bugprone-unchecked-optional-access)
            tally_kills(sim_.state().events, kill_count_);
            for (std::uint64_t t = tick_before; t < sim_.state().tick; ++t)
                if (advance_round_end()) return AppInput::MatchOver;
            acc = 0;  // the fixed-tick accumulator is dormant on this path
        } else {
        // Long-stall guard (spiral-of-death / teleport clamp). A window drag,
        // alt-tab, asset stall, or a debugger break can hand us a multi-hundred-
        // ms delta; without a cap the `while` below fires that many catch-up
        // ticks in one frame — the sim lurches (entities snap-teleport across
        // the board, past the 32 px interp snap threshold) and, worse, the loop
        // can wedge trying to out-run real time. Cap the queue at a few ticks'
        // worth: excess wall-time is DROPPED (the match briefly runs in slow
        // motion) rather than fast-forwarded. Does not touch determinism — the
        // sim still advances one deterministic tick per crossing; only how many
        // crossings a single frailty-induced hitch produces is bounded.
        constexpr std::uint64_t kMaxCatchupTicks = 4;
        if (acc > kMaxCatchupTicks * tick_ns) acc = kMaxCatchupTicks * tick_ns;
        while (acc >= tick_ns) {
            acc -= tick_ns;
            // Consume the frame-sampled latch on the FIRST tick of a catch-up
            // burst only (a later tick in the same burst re-reads the live
            // state, matching the original's one-edge-check-per-update under
            // a slow frame — its clamped ms delta produces exactly one
            // sub_41E61E read per displayed frame too).
            sim::TickInputs in = frame_in;
            for (int i = 0; i < sim::kMaxPlayers; ++i) {
                in.players[i].action1 = in.players[i].action1 || tap_latch[i].action1;
                in.players[i].action2 = in.players[i].action2 || tap_latch[i].action2;
                tap_latch[i].action1 = false;
                tap_latch[i].action2 = false;
            }
            sim_.tick(in);
            sounds_.on_tick(sim_.state());
            renderer_->on_events(sim_.state());  // NOLINT(bugprone-unchecked-optional-access)
            // Roll the renderer's inter-tick snapshots forward for THIS tick,
            // inside the catch-up loop — so a frame that advances the sim two
            // ticks still leaves interp `prev` at the penultimate tick (a clean
            // 1-tick lerp span) instead of two ticks back (the snap/double-speed
            // jitter). Tick-keyed, so draw_frame's own trailing call is a no-op
            // on the live path and still primes the demo/screenshot path.
            renderer_->advance_tick(sim_.state());  // NOLINT(bugprone-unchecked-optional-access)
            // §1's kill tally (sub_421B0F): a GameApp-side pass over this
            // tick's events, separate from the renderer's own on_events walk
            // (renderer_ never mutates GameApp state — CLAUDE.md's libs/game
            // boundary). Cumulative for the whole match (see kill_count_'s
            // doc comment); reset only in reset_match_scores().
            tally_kills(sim_.state().events, kill_count_);

            if (advance_round_end()) return AppInput::MatchOver;
        }
        }  // end else: the deterministic fixed-tick accumulator path

        audio_.update_music();
        // Gold Bomberman twinkle (docs/re/goldman-roulette.md §6): tell the
        // renderer which player/team is the pending gold winner every frame —
        // gold_player_ only changes between rounds, but this is a cheap int
        // pair and keeps the renderer decoupled from GameApp's own state.
        // NOLINTNEXTLINE(bugprone-unchecked-optional-access) — emplaced in init()
        renderer_->set_gold_player(gold_player_, is_team_mode());
        // Tell the renderer which animation clock to use (F9): per-frame walk/
        // fidget phase advance in native cadence, once-per-tick otherwise.
        // NOLINTNEXTLINE(bugprone-unchecked-optional-access) — emplaced in init()
        renderer_->set_native_cadence(native_cadence_);
        // F9: glide fraction for the 50 ms-stepped entities (flying/sliding
        // bombs, rovers) = how far into the current 50 ms tick this frame falls.
        // NOLINTNEXTLINE(bugprone-unchecked-optional-access) — emplaced in init()
        renderer_->set_entity_interp(
            native_cadence_ ? static_cast<float>(sim_.systems_accum_ms()) /
                                  static_cast<float>(sim::kMsPerTick)
                            : 1.0f);
        // Inter-tick interpolation fraction (renderer.hpp's draw_frame doc):
        // acc < tick_ns after the catch-up loop, so this is in [0,1) — how far
        // into the current 50 ms tick this displayed frame falls. The original
        // needed no such blend because its gameplay driver itself ran per
        // displayed frame on the ms delta (sub_42A191); our fixed 20 Hz sim
        // recovers that on-screen fluidity here, cosmetically.
        // Native-cadence mode renders the sim's live state directly (alpha=1 =>
        // player_interp/interp_pos return the current position, no lerp): the
        // sim already ran at frame rate this frame, so there is nothing to blend.
        const float interp_alpha =
            native_cadence_ ? 1.0f : static_cast<float>(acc) / static_cast<float>(tick_ns);
        // NOLINTNEXTLINE(bugprone-unchecked-optional-access) — emplaced in init()
        renderer_->draw_frame(sim_.state(), interp_alpha);
        // The player-row HUD strip (docs/re/in-match-shell.md "The player
        // row") needs GameApp's own win_count_/kill_count_/front_font_, none
        // of which Renderer owns — drawn as a GameApp-side overlay on top of
        // Renderer's frame, same layering the original has (sub_420F07 draws
        // it every tick, after the field/world but the clock/hurry HUD is
        // logically part of the same pass).
        draw_player_row(sim_.state());
        draw_fps_overlay(shown_fps);
        SDL_RenderPresent(sdl_renderer_.get());
        // Refresh-boundary pacer — see the pacing comment at the top of this
        // function. No-op when present already blocked past the target;
        // supplies the missing block (and re-phases the target) when it
        // didn't.
        // Pace target: the refresh period by default, or the sim's sub-frame
        // period when F8's uncapped mode is armed (see uncap_fps_). At the
        // sub-frame rate every canonical frame player_interp can distinguish
        // reaches the screen — capping any higher would only re-show sub-frames
        // (there are just kSubFrames per tick), so this is the useful ceiling,
        // not a hard free-run. The else-branch resync makes a mid-match toggle
        // self-correct within a frame.
        const std::uint64_t pace_period_ns = uncap_fps_ ? tick_ns / sim::kSubFrames : period_ns;
        std::uint64_t after_present_ns = SDL_GetTicksNS();
        if (after_present_ns < pace_target_ns) {
            SDL_DelayNS(pace_target_ns - after_present_ns);
            pace_target_ns += pace_period_ns;
        } else {
            pace_target_ns = after_present_ns + pace_period_ns;
        }
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
