#include "bomber/game/game_app.hpp"

#include <algorithm>  // std::max_element
#include <array>      // run_match's per-player tap latch
#include <cctype>     // std::isalnum (present_editor's filename sanitizer)
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
#include "bomber/game/hud_format.hpp"
#include "bomber/game/screens/debug_info_screen.hpp"
#include "bomber/game/screens/help_screens.hpp"
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
constexpr int kBootMusicId = 1000;  // 0x3E8 — TITLE.RSS, the continuous boot track
constexpr int kMenuMusicId = 1010;  // 0x3F2 — MENU.RSS, started on menu entry
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
    AppInput action;  // resolved when Enter selects this row
    bool live;        // false = a documented stub row (no handler yet, inert)
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
    {AppInput::StartMatch, true},   // 0 Play
    {AppInput::OpenNetwork, true},  // 1 START NET GAME -> network help
    {AppInput::OpenNetwork, true},  // 2 JOIN NET GAME -> network help
    {AppInput::OpenOptions, true},  // 3 Options (sub_4080DC) — was misbound to row 1
    {AppInput::OpenCredits, true},  // 4 Credits
    {AppInput::Advance, false},     // 5 Help browser (handled inline, entry unused)
    {AppInput::Quit, true},         // 6 Quit
};
constexpr int kMenuCount = static_cast<int>(std::size(kMenuItems));

// Cursor anchor over MAINMENU.PCX — CONFIRMED getvalue(700/701/702) (sub_42B9CE:
// v11=getvalue(700)=X, v1=getvalue(701)=Y, getvalue(702)=Y-step; the bomb-
// trigger sprite is blitted at x=X, y=Y + Ystep*row). Read live from VALUELST
// (columns of the multi-value row 700, whose own legend reads "X, Y - first item
// / YS - y-spacing"); these fallbacks are that install's values (332,140,38) so
// a stripped VALUELST still positions sanely. The idle/attract timeout uses
// getvalue(92) (sub_42B9CE), distinct from the waited-screen getvalue(12).
constexpr int kMenuCursorXFallback = 332;    // getvalue(700)
constexpr int kMenuCursorYFallback = 140;    // getvalue(701)
constexpr int kMenuCursorStepFallback = 38;  // getvalue(702)

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
    return ScreenContext{assets_,   audio_,      sounds_,       keyboard_,
                         gamepads_, front_font_, cursor_blink_, values_,
                         sdl_renderer_.get(), window_.get()};
}

AppInput GameApp::present_screen(const ScreenDef& def) {
    // Enter the screen (resets its clock/counter; music is NOT touched here —
    // the caller owns the continuous track, sub_42A088 only presents an image).
    screen_->enter(def, SDL_GetTicks());  // NOLINT(bugprone-unchecked-optional-access)
    AppInput result = AppInput::Advance;
    bool waiting = true;
    // Refresh-boundary pacing (see refresh_period_ns): even a static screen
    // spins this loop uncapped on Windows without it — the same DWM
    // non-blocking present as the animated loops.
    platform::FrameClock frame_clock(window_.get());
    while (waiting) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_EVENT_QUIT) return AppInput::Quit;
            if (ev.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN) {
                // Gamepad advance (mirrors present_menu's pad->synthetic-key
                // injection): present_screen's on_key treats every accept key
                // as advance, so a controller button synthesizes Enter and
                // walks the logo/title chain the same as a keyboard accept.
                SDL_Event synth{};
                synth.type = SDL_EVENT_KEY_DOWN;
                synth.key.key = SDLK_RETURN;
                SDL_PushEvent(&synth);
                continue;
            }
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
        screen_->update(now);                  // NOLINT(bugprone-unchecked-optional-access)
        if (screen_->done()) waiting = false;  // NOLINT(bugprone-unchecked-optional-access)

        audio_.update_music();
        SDL_SetRenderDrawColor(sdl_renderer_.get(), 0, 0, 0, 255);
        SDL_RenderClear(sdl_renderer_.get());
        screen_->draw(sdl_renderer_.get());  // NOLINT(bugprone-unchecked-optional-access)
        SDL_RenderPresent(sdl_renderer_.get());
        frame_clock.pace();
    }

    // No transition out: sub_42A088 CUTS between screens — it sets the palette
    // (sub_41522D, instant; the >>2 is the 8->6-bit VGA palette conversion, NOT
    // a fade loop), blits (sub_429FF1), and flips (sub_41043C). There is no wipe
    // anywhere in the front end (docs/re/frontend-flow.md "HEADWIPE.ANI is
    // dead art"), so the next screen simply replaces this one.
    return result;
}

AppInput GameApp::present_bm_screen(const std::string& bm_name) {
    return BmTextScreen(sctx()).run(bm_name);
}

AppInput GameApp::present_help_browser() {
    return HelpBrowserScreen(sctx()).run();
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
        renderer_->draw_frame(sim_.state());  // NOLINT(bugprone-unchecked-optional-access) — last
                                              // sim frame, frozen, no tick here
        browser.draw(sdl_renderer_.get());
        SDL_RenderPresent(sdl_renderer_.get());
        SDL_Delay(2);
    }
    return AppInput::Advance;
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
    // The GLUE pick doubles as the backdrop the two modal sub-screens
    // restore each frame (sub_415CA4's saved-backdrop memcpy holds this
    // same picture) — keep the name for them.
    const std::string glue = pick_glue();
    opt.enter(options_, glue);
    AppInput result = AppInput::Advance;
    while (!opt.done()) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_EVENT_QUIT) return AppInput::Quit;
            if (ev.type != SDL_EVENT_KEY_DOWN) continue;
            if (ev.key.key == SDLK_F1) {
                // CONFIRMED (pseudo.c 9298-9299, 9384-9388): the nav-blip SFX
                // 20 fires unconditionally for ANY real key, F1 included,
                // before sub_4080DC dispatches to sub_41431C — the earlier
                // port silently skipped this for F1 specifically.
                audio_.play(20);
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
            if (opt.open_keyremap()) present_keyremap_screen(glue);
            // "Scheme File" (row 8, §3 CORRECTED 2026-07-13): push
            // sub_407582's *.SCH picker the same modal way; a selection
            // updates the snapshot row AND the live scheme_.
            if (opt.open_scheme_picker()) present_scheme_picker(opt, glue);
        }
        audio_.update_music();
        // cursor1 blink inputs: wall clock (seconds, like the original's
        // time_()) + VALUELST 690's {base, spread} columns — see
        // cursor_indicator.hpp for the sub_413BD6 pacing model.
        opt.tick(SDL_GetTicks() / 1000ull, static_cast<int>(values_.column_or(690, 0, 2)),
                 static_cast<int>(values_.column_or(690, 1, 2)));
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
    // doc §2: "Cleared to -1 by: ... the Options-screen Gold Bomberman
    // toggle" AND, per the 2026-07-09 gold sweep, the Team Play toggle
    // (pseudo.c 9310-9311/9334-9335/9410-9412/9436-9437) — ANY PRESS of
    // either row forfeits a pending gold player in the original, inline at
    // toggle time, not gated on the net before/after value (an even number
    // of presses back to the original value still clears it), which is why
    // this sits OUTSIDE the changed() gate below and uses the touch flags
    // instead of a snapshot diff.
    if (opt.gold_forfeiting_row_touched()) gold_player_ = -1;
    if (opt.changed()) {
        options_ = opt.snapshot();
        team_play_ = options_.team_play;
        conveyor_speed_index_ = options_.conveyor_speed_index;
        options_dirty_ = true;
    }
    return result;
}

void GameApp::present_keyremap_screen(const std::string& backdrop) {
    // The key-remap UI (docs/re/results-and-options.md §2, sub_407B9D),
    // 1:1 rebuild 2026-07-13: MOUSE-DRIVEN widget grid (keyremap_screen.hpp's
    // file doc has the full pin list). Runs its own event pump so raw
    // scancode captures and clicks never leak into the Options cursor
    // underneath; each frame re-blits the Options screen's GLUE backdrop
    // (sub_415CA4's saved-backdrop restore — the picture, not the rows).
    KeyRemapScreen remap(assets_, front_font_);
    std::array<KeySet, kKeyboardSets> current{keyboard_.key_set(0), keyboard_.key_set(1)};
    remap.enter(current, backdrop);
    // sub_431178/sub_431360 bracket: the system cursor yields to the widget
    // library's own 8x8 arrow (drawn by remap.draw()) for this screen only.
    SDL_HideCursor();
    while (!remap.done()) {
        remap.tick(SDL_GetTicks());
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_EVENT_QUIT) {
                // Window close mid-screen: discard this visit's edits (the
                // app-level Quit is re-raised by the Options pump).
                SDL_ShowCursor();
                return;
            }
            if (ev.type == SDL_EVENT_KEY_DOWN) {
                // F1 (0x13B) -> the generic *.BM help browser (sub_41431C),
                // same dispatch as the Options screen's own F1 — but NOT
                // while capturing (F1 must be bindable) or under the NOTE
                // modal (whose own key loop just blips on it).
                if (ev.key.key == SDLK_F1 && !remap.capturing() && !remap.showing_note()) {
                    if (present_help_browser() == AppInput::Quit) {
                        SDL_ShowCursor();
                        return;
                    }
                    continue;
                }
                remap.on_key(ev.key.key, ev.key.scancode, audio_);
                continue;
            }
            // Mouse, converted into the 640x480 logical space (same
            // SDL_RenderCoordinatesFromWindow pattern as the editor canvas).
            if (ev.type == SDL_EVENT_MOUSE_MOTION) {
                float lx = 0, ly = 0;
                SDL_RenderCoordinatesFromWindow(sdl_renderer_.get(), ev.motion.x, ev.motion.y,
                                                &lx, &ly);
                remap.on_mouse_move(lx, ly);
            } else if (ev.type == SDL_EVENT_MOUSE_BUTTON_DOWN &&
                       ev.button.button == SDL_BUTTON_LEFT) {
                float lx = 0, ly = 0;
                SDL_RenderCoordinatesFromWindow(sdl_renderer_.get(), ev.button.x, ev.button.y,
                                                &lx, &ly);
                remap.on_mouse_down(lx, ly);
            } else if (ev.type == SDL_EVENT_MOUSE_BUTTON_UP &&
                       ev.button.button == SDL_BUTTON_LEFT) {
                float lx = 0, ly = 0;
                SDL_RenderCoordinatesFromWindow(sdl_renderer_.get(), ev.button.x, ev.button.y,
                                                &lx, &ly);
                remap.on_mouse_up(lx, ly, audio_);
            }
        }
        // sub_407AD9's raw keyboard-state poll — binds a key already held
        // when the 500 ms arm delay elapses (the event path alone misses it).
        if (remap.capturing()) remap.poll_capture();
        audio_.update_music();
        SDL_SetRenderDrawColor(sdl_renderer_.get(), 0, 0, 0, 255);
        SDL_RenderClear(sdl_renderer_.get());
        remap.draw(sdl_renderer_.get());
        SDL_RenderPresent(sdl_renderer_.get());
        SDL_Delay(2);
    }
    SDL_ShowCursor();
    // Apply live (KeyboardMapper reads collect_inputs() every match tick) and
    // mark dirty for the write-on-exit flush — never write options.ini here.
    const auto& edited = remap.edited();
    keyboard_.set_key_set(0, edited[0]);
    keyboard_.set_key_set(1, edited[1]);
    options_dirty_ = true;
}

void GameApp::present_scheme_picker(OptionsScreen& opt, const std::string& backdrop) {
    // sub_407582 (§3 row 8) — the SAME routine the editor's "edit an
    // existing scheme" path calls (pseudo.c 5501), so this reuses the SAME
    // SchemeFilePicker component: "*.SCH" glob over DATA/SCHEMES, rows
    // "%s: %s" (filename + the file's -N name, aSS), header getstring(721).
    SchemeFilePicker picker(assets_, front_font_);
    picker.enter(opts_.game_dir / "DATA" / "SCHEMES", backdrop);
    if (picker.empty()) {
        // Empty glob (pseudo.c 8467-8473): sub_414340 with getstring(95)
        // "NOTE!" on top, getstring(720) "No Scheme files found!" below, in
        // byte_49A390's ink — LUT offset 0x5000 -> idx 248 -> (164,0,0),
        // the SAME dark red as the quit-confirm prompt (docs/re/
        // frontend-flow.md "COLOR.PAL" table). sub_414340's own key loop:
        // nav blip on any key, close on Enter/Space/Esc.
        const std::string top = assets_.getstring(95, "NOTE!");
        const std::string bottom = assets_.getstring(720, "No Scheme files found!");
        const std::string ok = assets_.getstring(27, " Ok ");
        while (true) {
            SDL_Event ev;
            while (SDL_PollEvent(&ev)) {
                if (ev.type == SDL_EVENT_QUIT) return;
                if (ev.type != SDL_EVENT_KEY_DOWN) continue;
                audio_.play(20);
                if (ev.key.key == SDLK_RETURN || ev.key.key == SDLK_KP_ENTER ||
                    ev.key.key == SDLK_SPACE || ev.key.key == SDLK_ESCAPE)
                    return;
            }
            audio_.update_music();
            SDL_SetRenderDrawColor(sdl_renderer_.get(), 0, 0, 0, 255);
            SDL_RenderClear(sdl_renderer_.get());
            const Sprite& bg = assets_.frontend_pcx(backdrop);
            if (bg.tex) {
                SDL_FRect d{0, 0, static_cast<float>(bg.w), static_cast<float>(bg.h)};
                SDL_RenderTexture(sdl_renderer_.get(), bg.tex, nullptr, &d);
            }
            // Ink = byte_49A390 = DARK RED (164,0,0): sub_407582's empty-glob box
            // is sub_414340(getstring(95)|getstring(720), byte_49D37A,
            // byte_49A390) (batch_0x4074DC.cpp:180-184); a3 (byte_49A390) is the
            // foreground/ink = (164,0,0) warning red (docs/re/frontend-flow.md),
            // NOT white. (Restores the correct red.)
            draw_acknowledge_dialog(sdl_renderer_.get(), front_font_,
                                    &assets_.frontend_pcx("WINZ"), top, bottom, ok, 164, 0, 0);
            SDL_RenderPresent(sdl_renderer_.get());
            SDL_Delay(2);
        }
    }
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
    if (picker.cancelled()) return;
    // sub_407582's selection write-back (pseudo.c 8457-8463): the display
    // line is cut at its FIRST '.' (strchr, which also drops the ": <name>"
    // suffix in one stroke), copied into byte_4648C4, then uppercased
    // (sub_412A3B = strupr).
    std::string name = picker.selected().filename().string();
    if (auto dot = name.find('.'); dot != std::string::npos) name.erase(dot);
    for (auto& c : name) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    opt.set_scheme_filename(name);
    // The original re-parses byte_4648C4 at the next Play-flow entry
    // (sub_410F81 -> sub_4046CC -> sub_403EEE); reloading immediately keeps
    // scheme_ and the displayed row in lockstep with no hidden latency.
    reload_scheme_from_name(name);
}

bool GameApp::reload_scheme_from_name(const std::string& name) {
    // Accept the name with or without an extension ("BASIC" from the picker
    // / a hand-edited "BASIC.SCH" from options.ini alike).
    std::string want = name;
    if (auto dot = want.find('.'); dot != std::string::npos) want.erase(dot);
    for (auto& c : want) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    if (want.empty()) return false;
    std::filesystem::path schemes_dir = opts_.game_dir / "DATA" / "SCHEMES";
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
        scheme_ = assets::sch::load(found);
    } catch (const std::exception&) {
        return false;
    }
    return true;
}

std::string GameApp::present_scheme_filename_prompt(const std::string& seed) {
    // sub_42E938 line-edit for the save-as target (sub_4028D2 exit,
    // batch_0x402150.cpp:645-648): getstring(736) "Enter schemefilename (or
    // press <Enter>):", max 30 chars, seeded with the source filename. Enter on
    // the seed (or an empty box) keeps the seed; Escape cancels back to the seed
    // too — the original writes byte_4648C4 either way, so both return `seed`.
    // Returns the chosen stem (no extension; the caller sanitises + appends .SCH,
    // the sub_40497C force-extension step). Interactive-only (never the demo).
    const std::string label =
        assets_.getstring(736, "Enter schemefilename (or press <Enter>):");
    std::string entry = seed;
    std::string result = seed;
    SDL_StartTextInput(window_.get());
    bool waiting = true;
    while (waiting) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_EVENT_QUIT) {
                waiting = false;  // result stays `seed`
                break;
            }
            if (ev.type == SDL_EVENT_TEXT_INPUT) {
                if (ev.text.text && entry.size() < 30) entry += ev.text.text;  // 30-char cap
            } else if (ev.type == SDL_EVENT_KEY_DOWN) {
                if (ev.key.key == SDLK_BACKSPACE) {
                    if (!entry.empty()) entry.pop_back();
                } else if (ev.key.key == SDLK_RETURN || ev.key.key == SDLK_KP_ENTER) {
                    audio_.play(10);  // accept sting
                    result = entry.empty() ? seed : entry;  // "or press <Enter>" keeps the seed
                    waiting = false;
                } else if (ev.key.key == SDLK_ESCAPE) {
                    audio_.play(20);  // nav blip
                    result = seed;  // cancel: keep the source filename
                    waiting = false;
                }
            }
        }
        audio_.update_music();
        SDL_SetRenderDrawColor(sdl_renderer_.get(), 0, 0, 0, 255);
        SDL_RenderClear(sdl_renderer_.get());
        draw_text_entry_dialog(sdl_renderer_.get(), front_font_, 180.0f, label, entry, "Done",
                               "Cancel");
        SDL_RenderPresent(sdl_renderer_.get());
        SDL_Delay(2);
    }
    SDL_StopTextInput(window_.get());
    return result;
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
        // The SOURCE filename stem (byte_4648C4's seed) when editing an existing
        // scheme — the faithful save-as prompt defaults to it so a save writes
        // BACK to the source, not a name-derived sibling. Empty for a New scheme.
        std::string source_stem;
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
                    source_stem = picker.selected().stem().string();  // byte_4648C4 seed
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

        // §5: exit writes through sub_403C16 — our assets::sch::write() — on a
        // confirmed save. Written schemes go to the install's DATA/SCHEMES dir
        // (the SAME place the game loads them), NEVER the repo. Faithful save-as
        // (sub_4028D2 exit, batch_0x402150.cpp:645-649): the original pops a
        // getstring(736) filename text-entry SEEDED with the source filename
        // (byte_4648C4) and writes to whatever it holds — so editing an existing
        // scheme and accepting the prompt overwrites the SOURCE. The port used
        // to derive the name from the -N field, which turned "edit BASIC.SCH ->
        // save" into a stray sibling instead of an update. Seed with the source
        // stem when editing existing, else the -N name, else "EDITED".
        if (editor.save_requested()) {
            assets::sch::Scheme out = editor.grid().to_scheme();
            const std::string seed =
                !source_stem.empty() ? source_stem
                                     : (out.name.empty() ? std::string("EDITED") : out.name);
            std::string file_stem = present_scheme_filename_prompt(seed);
            for (auto& c : file_stem)
                if (!std::isalnum(static_cast<unsigned char>(c))) c = '_';
            if (file_stem.empty()) file_stem = "EDITED";  // never write a bare ".SCH"
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
    if (picker.cancelled() || picker.empty())
        return;  // sub_4015C6's error-dialog path (port: silent)

    try {
        assets::res::Campaign parsed = assets::res::load_campaign(picker.selected());
        if (parsed.stages.empty())
            return;  // "Couldn't open..." / zero-stage file: leave state untouched
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
    // path, case-insensitively (DOS filenames are case-insensitive), and
    // load it — the same reload_scheme_from_name the Options scheme picker
    // and init()'s schemefilename= resolution use.
    if (!reload_scheme_from_name(stage.scheme)) return false;

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
        renderer_->draw_frame(sim_.state());  // NOLINT(bugprone-unchecked-optional-access) — last
                                              // frame as backdrop, like the stage banner
        // sub_414340 paints the WINZ 9-patch too (its sub_41726B call @
        // pseudo.c 17070) and draws its lines via sub_41696C (outlined).
        draw_dialog_chrome(sdl_renderer_.get(), win, &assets_.frontend_pcx("WINZ"));
        draw_dialog_text(sdl_renderer_.get(), front_font_, top_line,
                         win.x + (win.w - top_w) / 2.0f, win.y + h + 32.0f, kDialogInkR,
                         kDialogInkG, kDialogInkB);  // byte_49D38F
        draw_dialog_text(sdl_renderer_.get(), front_font_, bottom_line,
                         win.x + (win.w - bottom_w) / 2.0f, win.y + h + 32.0f + h + 2.0f,
                         kDialogInkR, kDialogInkG, kDialogInkB);
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
        renderer_->draw_frame(sim_.state());  // NOLINT(bugprone-unchecked-optional-access) — last
                                              // frame as backdrop, like the help modal
        front_font_.draw(sdl_renderer_.get(), campaign_banner_, 220.0f, 200.0f, 255, 255, 255);
        front_font_.draw(sdl_renderer_.get(), prepare, 220.0f, 224.0f, 255, 220, 80);
        SDL_RenderPresent(sdl_renderer_.get());
        SDL_Delay(2);
    }
}

AppInput GameApp::present_campaign_complete() {
    // sub_40133F's stage-exhausted branch (batch_0x401010.cpp:288-296): when
    // `++dword_4648B0 >= dword_45E014` the original pops a blocking sub_414340
    // acknowledge modal — getstring(1220) "Congratulations!" over getstring(1225)
    // "You made it through the whole campaign!", ink byte_49A390 = (164,0,0) dark
    // red (batch_0x401010.cpp:289/294, a3/foreground; frontend-flow.md) — then
    // returns to the menu. The port used to
    // clear campaign state silently. Waits for Enter/Space/Escape (nav blip on
    // any key), like every other sub_414340 modal; Quit if the window closed.
    const std::string top = assets_.getstring(1220, "Congratulations!");
    const std::string bottom = assets_.getstring(1225, "You made it through the whole campaign!");
    const std::string ok = assets_.getstring(27, " Ok ");
    while (true) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_EVENT_QUIT) return AppInput::Quit;
            if (ev.type != SDL_EVENT_KEY_DOWN) continue;
            audio_.play(20);  // nav blip on any key
            if (ev.key.key == SDLK_RETURN || ev.key.key == SDLK_KP_ENTER ||
                ev.key.key == SDLK_SPACE || ev.key.key == SDLK_ESCAPE)
                return AppInput::Advance;
        }
        audio_.update_music();
        SDL_SetRenderDrawColor(sdl_renderer_.get(), 0, 0, 0, 255);
        SDL_RenderClear(sdl_renderer_.get());
        renderer_->draw_frame(sim_.state());  // NOLINT(bugprone-unchecked-optional-access) — backdrop
        draw_acknowledge_dialog(sdl_renderer_.get(), front_font_, &assets_.frontend_pcx("WINZ"), top,
                                bottom, ok, 164, 0, 0);  // byte_49A390 = (164,0,0) dark red
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

    // Escape ADVANCES one screen like every other accept key (present_screen
    // maps Escape->Back, but sub_42B060 treats it as an advance): a single
    // Escape on IPLOGO must step to HSLOGO, not short-circuit the whole boot
    // chain into the menu. So Back falls through to the next present_screen
    // here — only Quit (window close) short-circuits.
    AppInput ev = present_screen(logo_screen("IPLOGO"));
    if (ev == AppInput::Quit) return ev;
    ev = present_screen(logo_screen("HSLOGO"));
    if (ev == AppInput::Quit) return ev;

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
    // selects, Escape quits. On entry the original plays sub_42741E(0x3F2)
    // (the CONFIRMED menu track 1010, 0x3F2 == MENU.RSS; the RE brief's
    // 0x3FC/1020 was the round/results path sub_42A3F6, not this). This
    // switches the looping music to the menu track and keeps it playing while
    // in the menu. Returns the AppInput the highlighted row resolves to, or
    // Quit on window close.
    //
    // v14 (docs/re/frontend-flow.md "sub_42B9CE") is NOT a run-once-per-process
    // flag: sub_42B9CE has an OUTER while(1) (one iteration per menu visit,
    // pseudo.c ~30744) wrapping an INNER while(1) (the per-frame input-poll
    // loop, ~30768). `v14 = 1` is set once per OUTER iteration, right before
    // the inner loop starts; the inner loop's `if (v14) { sub_42741E(0x3F2);
    // v14 = 0; }` just stops it from re-firing on every polled FRAME within
    // that one visit. Every switch case at the bottom of the outer loop
    // (Play/setup cancel, Credits, Options, Quit-confirm-cancel, Results,
    // idle-timeout->attract, ...) falls through back to the top of the outer
    // loop, which re-arms v14=1, so sub_42741E(0x3F2) — a full free +
    // reload-from-disk + restart-from-sample-0 (sub_4273A4, no same-id
    // no-op) — fires again on EVERY return to the menu. present_menu() is
    // called once per Menu (re-)entry from run_app's dispatcher, i.e. once
    // per outer-loop iteration, so an unconditional start_music() call here
    // is the faithful port: it also switches back from the round/results
    // tracks (1020/1130) to 1010, which is exactly the audible "menu music
    // reclaims the loop when you back out" behaviour of the original. A
    // once-per-process gate (menu_music_started_, removed) broke that: after
    // the first call it never started 1010 again, so returning from a match
    // or the results screen left 1020/1130 stuck looping forever.
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
    // Refresh-boundary pacing (see refresh_period_ns / run_match): the blind
    // SDL_Delay(2) this replaced let the loop free-run at 300-500 Hz on
    // Windows (present does not block), so the `frame`-driven trigger cursor
    // animated far too fast. Pace to one animation step per real refresh.
    platform::FrameClock frame_clock(window_.get());
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
            // Pad navigation (sub_4102B7 @ 14286-14333): whenever the key
            // queue is empty the original's getkey polls every joystick and
            // SYNTHESIZES key codes from it — axis-threshold crossings become
            // up (328)/down (336) and any button rising edge becomes Enter
            // (13) — so the whole menu (and the quit confirm on top of it,
            // which reads the same getkey) is pad-navigable. SDL gives us
            // dpad/button edges directly; re-inject them as the synthetic
            // keys so every key path above/below (blips, dialog, rows) is
            // shared rather than duplicated.
            if (ev.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN) {
                SDL_Event synth{};
                synth.type = SDL_EVENT_KEY_DOWN;
                switch (ev.gbutton.button) {
                    case SDL_GAMEPAD_BUTTON_DPAD_UP: synth.key.key = SDLK_UP; break;
                    case SDL_GAMEPAD_BUTTON_DPAD_DOWN: synth.key.key = SDLK_DOWN; break;
                    case SDL_GAMEPAD_BUTTON_SOUTH:
                    case SDL_GAMEPAD_BUTTON_EAST:
                    case SDL_GAMEPAD_BUTTON_WEST:
                    case SDL_GAMEPAD_BUTTON_NORTH:
                    case SDL_GAMEPAD_BUTTON_START: synth.key.key = SDLK_RETURN; break;
                    default: synth.key.key = SDLK_UNKNOWN; break;
                }
                if (synth.key.key != SDLK_UNKNOWN) SDL_PushEvent(&synth);
            }
            if (ev.type != SDL_EVENT_KEY_DOWN) continue;

            // F10 opens the PORT-ONLY Video Settings panel (see
            // present_video_settings). Not an RE'd key — a port entry point that
            // keeps the modern video/cadence toggles off the faithful Options
            // screen. Ignored while the quit-confirm modal is up.
            if (!quit_confirm && ev.key.key == SDLK_F10) {
                present_video_settings();
                continue;
            }

            if (quit_confirm) {
                // sub_41456C's key loop: any real key blips (20); Yes accepts
                // (Y/y/Enter/Space), No cancels (N/n/Q/q/Escape — the Q pair
                // is 17253-17267, missing from frontend-flow.md's old list) —
                // every other key is ignored and the dialog stays up. The
                // dialog itself resolves SILENTLY (no accept sting on either
                // answer — the old play(10)s here were invented).
                audio_.play(20);
                switch (ev.key.key) {
                    case SDLK_Y:
                    case SDLK_RETURN:
                    case SDLK_KP_ENTER:
                    case SDLK_SPACE:
                        // sub_412987 on Yes: FREE the music (sub_427342) first,
                        // THEN the 2600 exit sting — MENU.RSS must not keep
                        // looping under it — then Sleep(0xFA0) so the sting is
                        // audible rather than cut off by window teardown.
                        audio_.stop_music();
                        audio_.play_random_in_range(kQuitStingLo, kQuitStingHi);  // 2600 group
                        SDL_Delay(4000);
                        return AppInput::Quit;
                    case SDLK_N:
                    case SDLK_Q:  // 0x51 'Q' / 0x71 'q' cancel too (17253-17267)
                    case SDLK_ESCAPE:
                        quit_confirm = false;
                        // The cancel path exits through sub_42B9CE's OUTER
                        // loop: cursor home to row 0 (case 6 -> v10 = 0,
                        // 30920-30922) and MENU.RSS reloaded from sample 0
                        // (the outer loop re-arms v14 -> sub_42741E(0x3F2)).
                        menu_index_ = 0;
                        audio_.start_music(kMenuMusicId);
                        break;
                    default: break;
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
                audio_.play(20);  // the any-real-key blip fires for each press (30787-30790)
                if (++editor_trigger_count_ > 5) {
                    editor_trigger_count_ = 0;
                    audio_.play(10);  // accept sting (SFX 10), §5
                    present_editor();
                    // Return through the outer loop re-arms v14 -> MENU.RSS
                    // reloads from sample 0 (goto LABEL_2 at 30882).
                    audio_.start_music(kMenuMusicId);
                    // Time spent in the editor must NOT count toward the 30 s
                    // attract idle trigger — reseed the idle clock on return.
                    menu_idle_since_ms_ = SDL_GetTicks();
                }
                continue;  // Ctrl+E itself never falls into the row switch
            }
            editor_trigger_count_ = 0;  // any other key resets the counter

            // Menu hotkeys (sub_42B9CE's raw-code dispatch; every one rides
            // the any-key blip 20 first): Ctrl+Q (raw 17) behaves exactly
            // like Escape (30861-30867); Alt+O (280) jumps to and selects
            // Options (30806-30812); F1 (315) selects row 5 = the help
            // browser (30826-30832); Alt+A (286) starts an attract demo
            // match directly (30813 -> the 30888-30894 attract path —
            // frontend-flow.md's old "run the current selection" label for
            // 286 was wrong); Alt+D (288) pops the hidden debug-info window
            // (sub_413D45, 30819-30822).
            const bool menu_alt = (ev.key.mod & SDL_KMOD_ALT) != 0;
            if (ev.key.key == SDLK_Q && (ev.key.mod & SDL_KMOD_CTRL) != 0) {
                audio_.play(20);
                audio_.play(10);
                menu_index_ = 6;
                quit_confirm = true;
                continue;
            }
            if (menu_alt && ev.key.key == SDLK_O) {
                audio_.play(20);
                audio_.play(10);
                // case 3's fall-through resets the cursor to row 0 for the
                // NEXT menu visit (30910-30912).
                AppInput opt_sel = kMenuItems[3].action;
                menu_index_ = 0;
                return opt_sel;
            }
            if (ev.key.key == SDLK_F1) {
                audio_.play(20);
                audio_.play(10);
                menu_index_ = 5;
                if (present_help_browser() == AppInput::Quit) return AppInput::Quit;
                audio_.start_music(kMenuMusicId);  // outer-loop v14 re-arm
                menu_idle_since_ms_ = SDL_GetTicks();  // help time is not idle
                continue;
            }
            if (menu_alt && ev.key.key == SDLK_A) {
                audio_.play(20);
                menu_index_ = 0;  // the attract path's own v10 = 0 (30894)
                roll_attract_match();
                return AppInput::StartMatch;
            }
            if (menu_alt && ev.key.key == SDLK_D) {
                audio_.play(20);
                if (present_debug_info_modal() == AppInput::Quit) return AppInput::Quit;
                menu_idle_since_ms_ = SDL_GetTicks();  // debug-window time is not idle
                continue;
            }

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
                    audio_.play(20);  // nav blip on the key (SFX 20)
                    audio_.play(10);  // accept sting selecting Quit (SFX 10)
                    menu_index_ = 6;  // v10 = 6, matches the cursor landing on Quit
                    quit_confirm = true;
                    break;
                case SDLK_RETURN:
                case SDLK_KP_ENTER:
                case SDLK_SPACE: {
                    // sub_42B9CE plays the any-key blip 20 FIRST (30787-30790,
                    // for every real key including Enter/Space), then the
                    // accept sting (SFX 10, sub_427961(10)) for BOTH Enter
                    // (13) and Space (32) on EVERY row — there is no "inert
                    // row" concept in the original; each row 0..6 is a live
                    // dispatch.
                    audio_.play(20);  // any-real-key blip
                    audio_.play(10);  // accept sting (SOUNDLST 10, menuexit)
                    // Row 5 = the generic help-file browser (sub_41431C, §4 —
                    // CORRECTED from the old "Roulette" label, see kMenuItems'
                    // comment above). sub_42B9CE's row switch calls it DIRECTLY
                    // (case 5: sub_41431C(); break;), staying inside the menu
                    // loop, unlike rows 0-4/6 which return through the AppState
                    // flow — so this row is handled here inline and
                    // never touches AppInput/next() (task brief: prefer not to
                    // add new AppInputs for this leaf).
                    if (menu_index_ == 5) {
                        if (present_help_browser() == AppInput::Quit) return AppInput::Quit;
                        // The inline return path falls through sub_42B9CE's
                        // outer loop -> v14 re-arm -> MENU.RSS reloads from
                        // sample 0.
                        audio_.start_music(kMenuMusicId);
                        menu_idle_since_ms_ = SDL_GetTicks();  // help time is not idle
                        break;
                    }
                    // A row we have not built yet (Editor) still plays the
                    // accept sting to stay faithful, but has no leaf to jump to, so
                    // it simply stays put instead of dead-ending on an unbuilt
                    // screen. (Documented inert stub — the accept is real, the
                    // destination is a deferred effort.)
                    if (!kMenuItems[menu_index_].live) break;
                    AppInput sel = kMenuItems[menu_index_].action;
                    // Row 3 (Options) is one of the rows whose fall-through
                    // resets the cursor to row 0 for the next menu visit
                    // (case 3 -> v10 = 0, 30910-30912); Play/Credits/Help/
                    // editor keep the row (verified faithful list).
                    if (menu_index_ == 3) menu_index_ = 0;
                    // Quit selected from the menu (Enter/Space on row 6): the SAME
                    // sub_412987 dispatch Escape reaches, so it pops the SAME confirm
                    // dialog rather than quitting outright.
                    if (sel == AppInput::Quit) {
                        quit_confirm = true;
                        break;
                    }
                    // Hand the selection to the flow by a CUT — no wipe. The
                    // original's menu dispatch (sub_42B9CE) calls the selected
                    // handler directly; the next screen's own first frame
                    // replaces the menu. A HEADWIPE.ANI wipe used to play here
                    // (~3.5 s: 211 frames, one per vsynced frame — the "long
                    // pause into the player-setup screen" report), but that
                    // file is dead art the original never even loads: it is
                    // absent from MASTER.ALI, and the decompile contains no
                    // headwipe string or transition call site anywhere. See
                    // docs/re/frontend-flow.md "HEADWIPE.ANI is dead art".
                    return sel;
                }
                default:
                    // The any-real-key blip fires for EVERY key sub_42B9CE
                    // reads, mapped or not (30787-30790) — an unbound letter
                    // still clicks. Gate on the press edge so SDL's key
                    // repeats don't buzz.
                    if (!ev.key.repeat) audio_.play(20);
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
            menu_index_ = 0;  // the attract path homes the cursor (v10 = 0, 30894)
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
        // "V1.0" version string, every menu frame (pseudo.c 30779:
        // sub_41696C(root, aV10, x=0, W=50, y=0, byte_49A624, black)) — the
        // ink is the general grey (168,168,164), the literal is hardcoded in
        // the binary (aV10, pseudo.c 1622), not a MESSAGES.TXT entry.
        front_font_.draw_outlined(sdl_renderer_.get(), "V1.0", 0, 0, 168, 168, 164, 0, 0, 0,
                                  50.0f);
        // Animated "bomb trigger green" cursor at the CONFIRMED anchor
        // (sub_42B9CE: x=getvalue(700), y=getvalue(701)+getvalue(702)*row; frame
        // = counter % statecnt). Anchor read live from VALUELST row 700's
        // columns, with this install's values as fallback. The sequence comes
        // from TRIGANIM.ANI (the file MASTER.ALI actually loads — the
        // original's menu resolves the name from the same global pool the
        // in-match trigger bomb uses; TRIGBOMB.ANI's 7-step twin is dead art,
        // docs/re/facts.md "ANI sequence-name audit"); if it is absent we
        // draw a pulsing highlight bar instead so the selection stays visible.
        {
            // The confirm dialog is modal in the original (sub_41456C blocks
            // sub_42B9CE's own loop), so the menu frame under it is FROZEN —
            // hold the cursor's animation phase while it is up.
            if (!quit_confirm) ++frame;
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
        // The Quit confirm modal (sub_412987 -> sub_41456C, RE-PINNED
        // 2026-07-10 — docs/re/frontend-flow.md "Escape/Quit-row confirm
        // dialog"): the SAME sub_43C734 chrome as the loading dialog — the
        // WINZ.PCX 9-patch (sub_41726B @ pseudo.c 17200), NOT a flat grey —
        // sized from the prompt extent (v29=max(prompt-width,80),
        // v30=v29+64=width, v32=4*fontheight+64+fontheight=height for this
        // one-line prompt), centered on screen (both axes — see the chrome
        // comment's X-placement TODO(RE)). Prompt at y=fontheight+32
        // (window-relative, centered) in sub_412987's OWN ink byte_49A390 —
        // LUT offset 0x5000 -> idx 248 -> (164,0,0), a dark red (pseudo.c
        // 16027: `v0 = byte_49A390` is the a3/foreground argument;
        // correcting this pass's earlier white) — outlined black via
        // sub_41696C; two sub_432298 buttons at the pinned
        // y=height-32-fontheight-6, x=width/2-80 (Yes) / width/2+22 (No).
        // Behaviour (Y/Enter/Space confirm, N/Escape cancel, sound path, 4s
        // exit delay) is UNCHANGED — chrome-only pass.
        if (quit_confirm) {
            std::string prompt = assets_.getstring(10, "Are you sure you want to exit?");
            std::string yes_label = assets_.getstring(26, " Yes ");
            std::string no_label = assets_.getstring(25, " No ");
            // Prompt ink = byte_49A390 = DARK RED (164,0,0): sub_412987's quit
            // confirm is sub_41456C(getstring(10), 0, byte_49A390)
            // (batch_0x411CF8.cpp), and byte_49A390 resolves to LUT offset
            // 0x5000 -> (164,0,0) — a distinct WARNING RED, NOT the white
            // byte_49D38F/kDialogInk (docs/re/frontend-flow.md "byte_49A390").
            // sub_41456C's a3 slot is the foreground/ink. (This restores the
            // correct red after a wrong same-day change to white.)
            draw_confirm_dialog(sdl_renderer_.get(), front_font_, &assets_.frontend_pcx("WINZ"),
                                prompt, "", yes_label, no_label, 164, 0, 0);
        }
        SDL_RenderPresent(sdl_renderer_.get());
        frame_clock.pace();
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
std::string GameApp::pick_glue() {
    setup_lcg_ = setup_lcg_ * 1664525u + 1013904223u;
    int glue_n = static_cast<int>(values_.column_or(16, 0, 7));  // getvalue(16)
    if (glue_n < 1) glue_n = 1;
    return "GLUE" +
           std::to_string(static_cast<int>((setup_lcg_ >> 16) % static_cast<unsigned>(glue_n)));
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
    // TEAM default — CORRECTED 2026-07-09 (docs/re/setup-screens.md "TEAM
    // default — CORRECTED"): sub_410F81 unconditionally calls sub_4046CC()
    // first thing, which (CD present) calls sub_403EEE(), which itself
    // unconditionally calls sub_4049C0() before anything else. sub_4049C0
    // sets `dword_46481C[12*j+8] = j & 1` for j in [0,10) (pseudo.c line
    // 6716) — i.e. every slot's TEAM byte resets to an ALTERNATING 0/1/0/1
    // pattern by slot parity every time this screen loads, not to a flat 0.
    // sub_403EEE's own file-parse loop only ever overwrites a slot's COLOUR
    // (dword_46481C+0/+4) from disk, never TEAM, unless a rare "-S
    // slot,x,y,team" 5-field profile line is present (pseudo.c line 6427) —
    // a hidden colour-profile file this port doesn't implement — so in
    // practice the alternating default always stands here. Getting this
    // wrong (old behaviour: every slot defaulted to 0) meant Team Play ON
    // without anyone pressing 'T' put every player on the SAME side: (a)
    // everybody got the team-1/WHITE 0.RMP override instead of half going
    // red (render_colour, docs/re/player-colour.md), and (b)
    // sides_remaining() read <=1 from tick 0, clinching the round instantly.
    reset_setup_teams(setup_team_);
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
    // Column 3 of each layout row is the CLIP WIDTH handed to the text
    // primitive (sub_41696C's max-width arg; setup-screens.md's earlier
    // "colour" label for this column was wrong — colour never comes from
    // VALUELST on this screen): header 200, slot rows 150, joystick heading
    // 170, joystick rows 320.
    const float hw = static_cast<float>(values_.column_or(705, 3, 200));
    const float lw = static_cast<float>(values_.column_or(710, 3, 150));
    const float jhw = static_cast<float>(values_.column_or(715, 3, 170));
    const float jlw = static_cast<float>(values_.column_or(720, 3, 320));
    // Footer anchor (VALUELST 790 — the file's own note: "goes on a lot of
    // different screens"): getstring(330) "Press F1 for help", centred on x
    // via sub_4172BA's `x = cx - (w+2)/2`, cyan ink byte_497F8F (96,252,252).
    const float fcx = static_cast<float>(values_.column_or(790, 0, 320));
    const float ffy = static_cast<float>(values_.column_or(790, 1, 440));
    const float ffw = static_cast<float>(values_.column_or(790, 3, 300));
    // Bomber-dude cursor blink base + random spread, seconds (VALUELST 690 =
    // {2,2}; getvalue(691) is column 1 of the same row).
    const int blink_base = static_cast<int>(values_.column_or(690, 0, 2));
    const int blink_spread = static_cast<int>(values_.column_or(690, 1, 2));

    int cursor = 0;

    // One frame of the screen (sub_410F81's per-frame body, pseudo.c
    // 15146-15267): backdrop, header, slot rows, joystick pane, footer — all
    // text through the 4-pass-outline primitive (sub_41696C) — and the
    // bomber-dude cursor LAST (the original queues sprites and flushes them
    // after the text, so the cursor lands on top). A lambda so the F1 help
    // browser below composites over the identical frame.
    auto draw_frame = [&]() {
        SDL_SetRenderDrawColor(sdl_renderer_.get(), 0, 0, 0, 255);
        SDL_RenderClear(sdl_renderer_.get());
        const Sprite& bg = assets_.frontend_pcx(glue);
        if (bg.tex) {
            SDL_FRect dst{0.0f, 0.0f, static_cast<float>(bg.w), static_cast<float>(bg.h)};
            SDL_RenderTexture(sdl_renderer_.get(), bg.tex, nullptr, &dst);
        }
        // Header (msg 50): white ink / black outline (byte_49D38F over
        // byte_495390[0], pseudo.c 15154-15160).
        front_font_.draw_outlined(sdl_renderer_.get(), assets_.getstring(50, "Available players:"),
                                  hx, hy, 255, 255, 255, 0, 0, 0, hw);
        for (int i = 0; i < 10; ++i) {
            const int t = setup_type_[i];
            std::string type;
            switch (t) {
                case 1: type = assets_.getstring(221, "COMPUTER"); break;
                case 2: type = fmt_u(assets_.getstring(222, "KEYBOARD %u"), setup_sub_[i]); break;
                case 3: type = fmt_u(assets_.getstring(223, "JOYSTICK %u"), setup_sub_[i]); break;
                case 4: type = assets_.getstring(224, "OTHER"); break;
                default: type = assets_.getstring(220, "OFF"); break;
            }
            // One combined "Player %u: %s" splice (msg 51) — the original
            // sprintf's the slot number and the type text in ONE call
            // (pseudo.c 15169-15195); the old two-piece concat left a literal
            // "%s" on screen with the install's real MESSAGES.TXT.
            const std::string line = fmt_us(assets_.getstring(51, "Player %u: %s"), i + 1, type);
            // Ink = the slot's authentic colour via sub_41672F(i) (the .RMP
            // tail quantised min(c/3,31) -> RGB555 -> LUT), which
            // AssetStore::slot_color reproduces. CONFIRMED never the team
            // red/white override: sub_410F81 saves+zeroes dword_464964 around
            // this lookup (pseudo.c 15191-15204) — only the separate TEAM
            // marker below is team-inked. And the ink is NEVER state-dimmed
            // or selection-boosted (no OFF/COM dimming exists; the old
            // selected-row +70 nudge was invented — the cursor sprite alone
            // marks the selection).
            std::uint8_t sc[3];
            assets_.slot_color(i, sc);
            // Outline: black for every slot EXCEPT index 1 — the BLACK
            // player's row gets a WHITE outline (sub_416867, pseudo.c
            // 18496-18503) so it stays legible over a dark glue backdrop.
            const Uint8 oc = i == 1 ? 255 : 0;
            float lx_end = front_font_.draw_outlined(sdl_renderer_.get(), line, lx,
                                                     ly + lys * static_cast<float>(i), sc[0],
                                                     sc[1], sc[2], oc, oc, oc, lw);
            if (team_play_) {
                // Team marker: getstring(230), drawn for EVERY slot whenever
                // Team Play is on (gated on the GLOBAL dword_464964, pseudo.c
                // ~15212 — NOT on this slot's own team byte). CONFIRMED
                // unformatted (no sprintf before the two sub_4124A4(230)
                // reads at ~15221/15223) — the COLOUR alone tells the teams
                // apart, via sub_4141F8(team): team byte != 0 -> byte_49D0DA
                // red (252,80,80), else byte_49D38F white — the same split as
                // the in-match sprite override (docs/re/player-colour.md).
                std::string marker = "  " + assets_.getstring(230, "TEAM");
                const bool team1 = setup_team_[i] != 0;  // sub_4141F8's `a1 ?` branch
                front_font_.draw_outlined(
                    sdl_renderer_.get(), marker, lx_end, ly + lys * static_cast<float>(i),
                    static_cast<Uint8>(team1 ? 252 : 255), static_cast<Uint8>(team1 ? 80 : 255),
                    static_cast<Uint8>(team1 ? 80 : 255), 0, 0, 0);
            }
        }
        // Joystick pane (getvalue 715/720): heading msg 40, then one line per
        // detected stick (msg 41 "Joy %u - %s", the stick's own name in the
        // %s — sub_429A61(i)) or, if none, the single msg-42 line. ALL of it
        // plain white ink / black outline (pseudo.c 15227-15263) — the old
        // grey (200,200,200)/(150,150,150) tints were invented.
        front_font_.draw_outlined(sdl_renderer_.get(), assets_.getstring(40, "JOYSTICKS"), jhx,
                                  jhy, 255, 255, 255, 0, 0, 0, jhw);
        const int joy_count = gamepads_.count();
        if (joy_count == 0) {
            front_font_.draw_outlined(sdl_renderer_.get(), assets_.getstring(42, "none"), jlx, jly,
                                      255, 255, 255, 0, 0, 0, jlw);
        } else {
            for (int j = 0; j < joy_count; ++j) {
                const std::string jline =
                    fmt_us(assets_.getstring(41, "Joy %u - %s"), j, gamepads_.name(j));
                front_font_.draw_outlined(sdl_renderer_.get(), jline, jlx,
                                          jly + jlys * static_cast<float>(j), 255, 255, 255, 0, 0,
                                          0, jlw);
            }
        }
        // Footer (sub_413FB9 -> getstring(330), local play only): centred,
        // cyan/black. Replaces the invented key-legend line.
        const std::string help = assets_.getstring(330, "Press F1 for help");
        const float help_w = static_cast<float>(front_font_.measure(help));
        front_font_.draw_outlined(sdl_renderer_.get(), help, fcx - (help_w + 2.0f) / 2.0f, ffy, 96,
                                  252, 252, 0, 0, 0, ffw);
        // The bomber-dude row cursor (sub_413BD6, called at pseudo.c
        // 15205-15211): MISC.ANI "cursor1", hotspot-anchored at
        // (getvalue(710) - 15, row_y + 16) — this screen alone uses -15; the
        // options/level screens use -20. The +16 y nudge is pinned
        // EMPIRICALLY from a 1:1 native capture of the level screen
        // (2026-07-12; VALUELST 736 = 170, measured sprite rows 155..186 →
        // anchor = row_y + 16): the dude's feet stand just under the row
        // text's baseline. The decompile loses the +16 to register mangling
        // at every call site, so the capture is the authority. Idle step 0 +
        // timed blink: cursor_indicator.hpp.
        Anim cur = resolve_sequence(assets_.misc(), "cursor1");
        if (!cur.steps.empty()) {
            const std::size_t st = cursor_blink_.step(SDL_GetTicks() / 1000ull, cur.steps.size(),
                                                      blink_base, blink_spread);
            const Sprite& sp = cur.steps[anim_step_index(st, cur.steps.size())];
            if (sp.tex) {
                SDL_FRect d{lx - 15.0f - static_cast<float>(sp.hx),
                            ly + lys * static_cast<float>(cursor) + 16.0f -
                                static_cast<float>(sp.hy),
                            static_cast<float>(sp.w), static_cast<float>(sp.h)};
                SDL_RenderTexture(sdl_renderer_.get(), sp.tex, nullptr, &d);
            }
        }
    };

    // sub_414340 error modal (batch_0x410401.cpp ~1152/1176): the start
    // guards below pop a WINZ-9-patch acknowledge box — the reason line over
    // getstring(96), dark-red ink, dismissed by Enter/Space/Escape (nav-blip
    // on any key) — drawn over the frozen setup frame. Returns Quit if the
    // window closed while it was up, else Advance (the screen stays open).
    auto show_error = [&](const std::string& reason) -> AppInput {
        const std::string sub = assets_.getstring(96, "Cannot start the game!");
        const std::string ok = assets_.getstring(27, " Ok ");
        while (true) {
            SDL_Event mev;
            while (SDL_PollEvent(&mev)) {
                if (mev.type == SDL_EVENT_QUIT) return AppInput::Quit;
                if (mev.type != SDL_EVENT_KEY_DOWN) continue;
                audio_.play(20);  // nav blip on any key
                if (mev.key.key == SDLK_RETURN || mev.key.key == SDLK_KP_ENTER ||
                    mev.key.key == SDLK_SPACE || mev.key.key == SDLK_ESCAPE)
                    return AppInput::Advance;
            }
            audio_.update_music();
            draw_frame();
            // Ink = byte_49A390 = DARK RED (164,0,0): the setup start-guard
            // errors are sub_414340(getstring(46/48/45)|getstring(96), color1,
            // byte_49A390) (batch_0x410401.cpp:1161/1175), and byte_49A390
            // resolves to (164,0,0) warning red (docs/re/frontend-flow.md), NOT
            // white. (Restores the correct red.)
            draw_acknowledge_dialog(sdl_renderer_.get(), front_font_, &assets_.frontend_pcx("WINZ"),
                                    reason, sub, ok, 164, 0, 0);
            SDL_RenderPresent(sdl_renderer_.get());
            SDL_Delay(2);
        }
    };
    // Start guard 1 (sub_42223E, batch_0x410401.cpp 1148-1168): at least two
    // ACTIVE slots, or in TEAM mode at least two DISTINCT team values among
    // the active slots — else the game refuses to start.
    auto count_ok = [&]() {
        if (team_play_) {
            bool seen0 = false, seen1 = false;
            for (int i = 0; i < 10; ++i) {
                if (setup_type_[i] == 0) continue;  // OFF slots don't count
                (setup_team_[i] ? seen1 : seen0) = true;
            }
            return seen0 && seen1;
        }
        int active = 0;
        for (int i = 0; i < 10; ++i)
            if (setup_type_[i] != 0) ++active;
        return active >= 2;
    };
    // Start guard 2 (sub_422085, batch_0x410401.cpp 1172): two ACTIVE HUMAN
    // slots (KEYBOARD=2 or JOYSTICK=3) may not share the same input type AND
    // sub-index — same keyboard set or same stick. CPU (1)/OTHER (4)/OFF (0)
    // are excluded.
    auto dup_controller = [&]() {
        for (int i = 0; i < 10; ++i) {
            if (setup_type_[i] != 2 && setup_type_[i] != 3) continue;
            for (int j = i + 1; j < 10; ++j) {
                if (setup_type_[j] != 2 && setup_type_[j] != 3) continue;
                if (setup_type_[i] == setup_type_[j] && setup_sub_[i] == setup_sub_[j]) return true;
            }
        }
        return false;
    };

    bool waiting = true;
    // Enter/Space accept debounce (mirrors present_map_select's 1 s
    // accept_after_ms, 8100/8220-8228): ignore an accept for 1 s after entry
    // so a held Enter carried from the previous screen can't blast the start.
    std::uint64_t accept_after_ms = SDL_GetTicks() + 1000;
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
                // CUMULATIVE, not consecutive (sub_410F81 pseudo.c 840,
                // 1193-1197): ONLY the 'C' handler touches this counter — no
                // other key resets it — so 5 total 'C' presses across the
                // visit arm the picker. (The old any-other-key reset below
                // required 5 CONSECUTIVE presses, which the original never
                // demanded.)
                if (++campaign_trigger_count_ == 5) {
                    campaign_trigger_count_ = 0;
                    audio_.play(10);  // accept sting (SFX 10), mirrors the editor trigger
                    present_campaign_picker();
                }
                continue;
            }

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
            // Enter/Space (batch_0x410401.cpp 1148-1182: Space, 0x20, routes
            // through the SAME accept path as Enter) leaves this screen and
            // proceeds to match init / the LEVEL screen — but only past the
            // debounce and the two start guards.
            if (k == SDLK_RETURN || k == SDLK_KP_ENTER || k == SDLK_SPACE) {
                audio_.play(20);  // any-key blip first (sub_427961(20))
                if (SDL_GetTicks() < accept_after_ms) continue;  // held-Enter debounce
                // Guard 2 first (sub_422085): same-controller humans -> error
                // getstring(45) over getstring(96).
                if (dup_controller()) {
                    if (show_error(assets_.getstring(
                            45, "Two players cannot use the same controls!")) == AppInput::Quit)
                        return AppInput::Quit;
                    continue;
                }
                // Guard 1 (sub_42223E): too few players/teams -> error
                // getstring(46) (solo) or getstring(48) (team) over
                // getstring(96).
                if (!count_ok()) {
                    const std::string reason =
                        team_play_ ? assets_.getstring(48, "You need at least two teams!")
                                   : assets_.getstring(46, "You need at least two players!");
                    if (show_error(reason) == AppInput::Quit) return AppInput::Quit;
                    continue;
                }
                audio_.play(10);  // accept sting
                waiting = false;
                break;
            }
            audio_.play(20);  // any real key blips first (sub_427961(20))
            if (k == SDLK_UP)
                cursor = (cursor + 9) % 10;  // 328
            else if (k == SDLK_DOWN)
                cursor = (cursor + 1) % 10;                    // 336
            else if (k == SDLK_RIGHT)
                cycle_input_type(cursor);                      // 333 sub_421E80
            else if (k == SDLK_LEFT || k == SDLK_0 || k == SDLK_O) {  // 331 / '0' / 'o' (111)
                setup_type_[cursor] = 0;                       // sub_421E33(i,0,0)
                setup_sub_[cursor] = 0;
            } else if (k == SDLK_T) {  // 'T' team toggle (+84)
                // batch_0x410401.cpp 1206-1223: only an ACTIVE slot toggles;
                // an OFF slot buzzes (SFX 40) and does nothing.
                if (setup_type_[cursor] != 0)
                    setup_team_[cursor] = setup_team_[cursor] ? 0 : 1;
                else
                    audio_.play(40);
            } else if (k == SDLK_F1) {
                // sub_410F81 15432-15436: key 0x13B (F1) dispatches the SAME
                // generic *.BM help browser as menu row 5 / the options
                // screen / the in-round key (one routine, sub_41431C),
                // composited over this screen like every sub_41431C site.
                HelpBrowser browser(assets_, front_font_);
                browser.enter(values_.at_or(15, 1) != 0);
                while (!browser.done()) {
                    SDL_Event hev;
                    while (SDL_PollEvent(&hev)) {
                        if (hev.type == SDL_EVENT_QUIT) return AppInput::Quit;
                        if (hev.type == SDL_EVENT_KEY_DOWN) browser.on_key(hev.key.key, audio_);
                    }
                    if (browser.viewing() && browser.viewer().done()) browser.close_viewer();
                    audio_.update_music();
                    draw_frame();
                    browser.draw(sdl_renderer_.get());
                    SDL_RenderPresent(sdl_renderer_.get());
                    SDL_Delay(2);
                }
            }
        }
        audio_.update_music();
        draw_frame();
        SDL_RenderPresent(sdl_renderer_.get());
        SDL_Delay(2);
    }
    return AppInput::Advance;
}

// The 11 built-in level names (VALUELST 450-460 / getvalue(150+n)); RANDOM is
// getstring(149). These GENERIC fallbacks are ours — the real names live in the
// user's MESSAGES.TXT and load at runtime via getstring, never committed.
const char* GameApp::level_fallback(int idx) {
    static const char* kNames[] = {"NEW TRADITIONALIST",
                                   "CLASSIC GREEN ACRES",
                                   "HOCKEY RINK",
                                   "ANCIENT EGYPT",
                                   "COAL MINE",
                                   "BEACH",
                                   "ALIENS",
                                   "HAUNTED HOUSE",
                                   "UNDER THE OCEAN",
                                   "DEEP FOREST GREEN",
                                   "INNER CITY TRASH"};
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
    // WORKING COPIES (sub_406DDE 8092-8093: dword_45E0B8/45E0B4 seeded from
    // the committed globals on entry): edits touch only these; Enter/Space
    // commits them (LABEL_101, 8261-8271) and Escape DISCARDS them — the old
    // in-place member edits leaked cancelled changes into the next visit.
    int level = selected_level_;
    int wins = win_target_;
    // Enter/Space debounce (8100/8220-8228): accept is IGNORED until 1 s
    // (sub_4148AC() = 1 locally) after entry or the last value change — the
    // original's guard against a held Enter from the previous screen
    // committing instantly.
    std::uint64_t accept_after_ms = SDL_GetTicks() + 1000;
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

    // Cursor blink + footer anchors, same VALUELST sources as the sibling
    // screens (sub_413BD6 / sub_413FB9).
    const int blink_base = static_cast<int>(values_.column_or(690, 0, 2));
    const int blink_spread = static_cast<int>(values_.column_or(690, 1, 2));
    const float fcx = static_cast<float>(values_.column_or(790, 0, 320));
    const float ffy = static_cast<float>(values_.column_or(790, 1, 440));
    const float lw = static_cast<float>(values_.column_or(735, 3, 300));  // clip width

    // One frame of the screen (sub_406DDE's per-frame body 8107-8153) — a
    // lambda so the F1 help browser composites over the identical frame.
    auto draw_frame = [&]() {
        // Re-roll the sample-block pattern on entry and whenever the LEVEL
        // changes (sub_406AA3's v35 re-arm) — never every frame.
        if (level != pattern_level) {
            pattern_level = level;
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
                    int n = level;
                    if (n < 0) {  // RANDOM: re-pick per cell (pinned quirk)
                        setup_lcg_ = setup_lcg_ * 1664525u + 1013904223u;
                        n = static_cast<int>((setup_lcg_ >> 16) % static_cast<unsigned>(max_n));
                    }
                    tile_of[static_cast<std::size_t>(i)][static_cast<std::size_t>(j)] = n;
                }
            }
            field_stage = level;
            if (field_stage < 0) {
                setup_lcg_ = setup_lcg_ * 1664525u + 1013904223u;
                field_stage = static_cast<int>((setup_lcg_ >> 16) % static_cast<unsigned>(max_n));
            }
        }

        SDL_SetRenderDrawColor(sdl_renderer_.get(), 0, 0, 0, 255);
        SDL_RenderClear(sdl_renderer_.get());
        const Sprite& bg = assets_.frontend_pcx(glue);
        if (bg.tex) {
            SDL_FRect dst{0.0f, 0.0f, static_cast<float>(bg.w), static_cast<float>(bg.h)};
            SDL_RenderTexture(sdl_renderer_.get(), bg.tex, nullptr, &dst);
        }
        // Sample-block preview panel (sub_406AA3, level&rounds audit
        // 2026-07-12): border fill = the general WHITE byte_49D38F —
        // (240,248,252), NOT the old invented (40,40,60) — at (378,80,
        // 224x202); the field swatch is a 1:1 CROP of FIELDn.PCX starting 48
        // rows down (the `&v17[12*640]` int-indexing = 48 scanlines; NOT a
        // stretch — sub_4428B4 is a plain rect copy), 220x198 at (380,82),
        // which lines the backdrop's own board grid up under the drawn
        // tiles; then the 5x5 solid/brick grid at native 40x36 cells.
        {
            const float bx = static_cast<float>(px - 22);
            const float by = static_cast<float>(py - 20);
            const float bw = static_cast<float>(pxsize * sim::kTileW + 24);
            const float bh = static_cast<float>(pysize * sim::kTileH + 22);
            SDL_SetRenderDrawColor(sdl_renderer_.get(), 240, 248, 252, 255);
            SDL_FRect border{bx, by, bw, bh};
            SDL_RenderFillRect(sdl_renderer_.get(), &border);
            if (field_stage >= 0) {
                const AssetStore::StagePreview& fprev = assets_.stage_preview(field_stage);
                if (fprev.field) {
                    const float sw = static_cast<float>(pxsize * sim::kTileW + 20);
                    const float sh = static_cast<float>(pysize * sim::kTileH + 18);
                    SDL_FRect src{0.0f, 48.0f, sw, sh};
                    SDL_FRect panel{static_cast<float>(px - 20), static_cast<float>(py - 18), sw,
                                    sh};
                    SDL_RenderTexture(sdl_renderer_.get(), fprev.field, &src, &panel);
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
                                   static_cast<float>(sim::kTileW),
                                   static_cast<float>(sim::kTileH)};
                    SDL_RenderTexture(sdl_renderer_.get(), sp.tex, nullptr, &cell);
                }
            }
        }
        // Both text rows in the SAME white ink + black outline (sub_41696C;
        // byte_49D38F decodes to (240,248,252) — LUT 0x7FFF -> idx 72,
        // level&rounds audit), clip getvalue(738)=300. The original marks the
        // active row with the cursor sprite ALONE — the old per-row colour
        // highlight and the grey key legend were invented.
        const std::string level_name =
            level < 0 ? assets_.getstring(149, "Random Each Game")
                      : assets_.getstring(150 + level, level_fallback(level));
        const std::string level_line = fmt_s(assets_.getstring(210, "%s"), level_name);
        front_font_.draw_outlined(sdl_renderer_.get(), level_line, lx, ly, 240, 248, 252, 0, 0, 0,
                                  lw);
        // Wins line: getstring(211) "%u %s to win match" with the %s picked
        // by the "win by kills" option — getstring(208) "Wins" /
        // getstring(209) "Kills" (pseudo.c 8124-8127; the old single-%u
        // splice left a literal "%s" on screen with the real MESSAGES.TXT).
        const std::string wins_word =
            assets_.getstring(options_.win_by_kills ? 209 : 208,
                              options_.win_by_kills ? "Kills" : "Wins");
        const std::string wins_line =
            fmt_us(assets_.getstring(211, "%u %s to win match"), wins, wins_word);
        front_font_.draw_outlined(sdl_renderer_.get(), wins_line, lx, ly + lys, 240, 248, 252, 0,
                                  0, 0, lw);
        // Footer (sub_413FB9): centred cyan "Press F1 for help".
        const std::string help = assets_.getstring(330, "Press F1 for help");
        const float help_w = static_cast<float>(front_font_.measure(help));
        front_font_.draw_outlined(sdl_renderer_.get(), help, fcx - (help_w + 2.0f) / 2.0f, ffy, 96,
                                  252, 252, 0, 0, 0);
        // The bomber-dude row cursor (sub_413BD6 at 8140-8141): (getvalue(735)
        // - 20, row_y + 16) — the +16 is the empirically pinned anchor nudge
        // (measured off THIS screen's native capture; see options_screen.cpp).
        Anim cur = resolve_sequence(assets_.misc(), "cursor1");
        if (!cur.steps.empty()) {
            const std::size_t st = cursor_blink_.step(SDL_GetTicks() / 1000ull, cur.steps.size(),
                                                      blink_base, blink_spread);
            const Sprite& sp = cur.steps[anim_step_index(st, cur.steps.size())];
            if (sp.tex) {
                SDL_FRect d{lx - 20.0f - static_cast<float>(sp.hx),
                            ly + lys * static_cast<float>(row) + 16.0f -
                                static_cast<float>(sp.hy),
                            static_cast<float>(sp.w), static_cast<float>(sp.h)};
                SDL_RenderTexture(sdl_renderer_.get(), sp.tex, nullptr, &d);
            }
        }
    };

    bool waiting = true;
    while (waiting) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_EVENT_QUIT) return AppInput::Quit;
            if (ev.type != SDL_EVENT_KEY_DOWN) continue;
            const SDL_Keycode k = ev.key.key;
            if (k == SDLK_ESCAPE) {
                // sub_406DDE's own Esc handler (pseudo.c 8186-8191) is called
                // straight from sub_410F81's TAIL (pseudo.c 15516, gated
                // `if (!dword_464A68)`) with NO loop back to the player
                // screen afterwards — so this aborts the WHOLE Play flow to
                // the menu, exactly like the Goldman wheel's own Esc (doc §5),
                // NOT "back one screen" to present_setup. It also forfeits any
                // pending gold player (`dword_46492C = -1`, doc §2's "Cleared
                // to -1 by" list). The WORKING level/wins copies are simply
                // dropped (8092-8093 re-seed on the next entry) — the
                // committed selections stay untouched.
                audio_.play(20);
                gold_player_ = -1;
                return AppInput::Back;
            }
            if (k == SDLK_F1) {
                // 0x13B -> sub_41431C (pseudo.c 8208-8216): the same generic
                // *.BM help browser, composited over this screen.
                audio_.play(20);
                HelpBrowser browser(assets_, front_font_);
                browser.enter(values_.at_or(15, 1) != 0);
                while (!browser.done()) {
                    SDL_Event hev;
                    while (SDL_PollEvent(&hev)) {
                        if (hev.type == SDL_EVENT_QUIT) return AppInput::Quit;
                        if (hev.type == SDL_EVENT_KEY_DOWN) browser.on_key(hev.key.key, audio_);
                    }
                    if (browser.viewing() && browser.viewer().done()) browser.close_viewer();
                    audio_.update_music();
                    draw_frame();
                    browser.draw(sdl_renderer_.get());
                    SDL_RenderPresent(sdl_renderer_.get());
                    SDL_Delay(2);
                }
                continue;
            }
            if (k == SDLK_RETURN || k == SDLK_KP_ENTER || k == SDLK_SPACE) {
                // Accept (LABEL_101, 8261-8271) — Space accepts too, and the
                // key first rides the any-key blip 20 (8176), then the accept
                // sting 10. Ignored inside the 1 s debounce window
                // (8220-8228): the key still blips, nothing commits.
                audio_.play(20);
                if (SDL_GetTicks() < accept_after_ms) continue;
                audio_.play(10);
                selected_level_ = level;  // commit the working copies
                win_target_ = wins;
                waiting = false;
                break;
            }
            audio_.play(20);
            if (k == SDLK_UP || k == SDLK_DOWN)
                row = (row + 1) % 2;  // 2 rows: either arrow toggles
            else if (row == 0 && k == SDLK_LEFT) {  // --level, wrap below -1
                if (--level < -1) level = level_count - 1;
                accept_after_ms = SDL_GetTicks() + 1000;  // debounce re-arm (8325)
            } else if (row == 0 && k == SDLK_RIGHT) {  // ++level, wrap above count-1 to -1
                if (++level >= level_count) level = -1;
                accept_after_ms = SDL_GetTicks() + 1000;
            } else if (row == 1 && k == SDLK_LEFT) {  // wins -1
                if (--wins < 1) wins = 1;
                accept_after_ms = SDL_GetTicks() + 1000;
            } else if (row == 1 && k == SDLK_RIGHT) {  // wins +1
                if (++wins > 100) wins = 100;
                accept_after_ms = SDL_GetTicks() + 1000;
            } else if (row == 1 && k == SDLK_PAGEUP) {
                // wins +5 (batch_0x405B3A.cpp 1097-1128, code 372) — WINS row
                // only, clamp 1..100. Rebound from the old invented Ctrl+Left/
                // Right, which the original never used.
                wins += 5;
                if (wins > 100) wins = 100;
                accept_after_ms = SDL_GetTicks() + 1000;
            } else if (row == 1 && k == SDLK_PAGEDOWN) {  // wins -5 (code 371), WINS row only
                wins -= 5;
                if (wins < 1) wins = 1;
                accept_after_ms = SDL_GetTicks() + 1000;
            }
        }

        audio_.update_music();
        draw_frame();
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
