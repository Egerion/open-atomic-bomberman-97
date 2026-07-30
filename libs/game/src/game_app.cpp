#include "bomber/game/game_app.hpp"

#include <algorithm>  // std::max_element, std::clamp
#include <array>      // run_match's per-player tap latch
#include <chrono>     // random_boot_seed
#include <cmath>      // std::lround (boot loading-bar percent readout)
#include <cstdint>    // load_window_icon's .ICO byte parsing
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <functional>  // boot-loading progress callback (AssetStore::load / audio_.init)
#include <iterator>    // std::size
#include <random>      // random_boot_seed
#include <string>

#include "bomber/assets/install.hpp"
#include "bomber/game/anim_pace.hpp"
#include "bomber/game/bmscreen.hpp"
#include "bomber/game/dialog_chrome.hpp"
#include "bomber/audio/round_music.hpp"  // round_music_id (shared with MatchRunner)
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
#include "bomber/game/screens/lobby_screen.hpp"
#include "bomber/game/screens/net_overlay.hpp"  // F3 panel + the netdiag.log record
#include "bomber/game/screens/net_setup_link.hpp"
#include "bomber/game/screens/netplay_connect_screen.hpp"
#include "bomber/game/screens/options_screens.hpp"
#include "bomber/game/screens/results_screens.hpp"
#include "bomber/game/screens/scheme_filename_prompt.hpp"
#include "bomber/game/screens/setup_screen.hpp"
#include "bomber/game/screens/video_settings_screen.hpp"
#include "bomber/game/sprites.hpp"
#include "bomber/match/match_factory.hpp"
#include "bomber/net/rematch_session.hpp"   // net::RematchSession (the post-match rendezvous)
#include "bomber/net/rollback_session.hpp"  // net::RollbackSession (run_netplay_match)
#include "bomber/net/round_rotation.hpp"    // net::round_seed / round_tick_base (round rotation)
#include "bomber/net/setup_session.hpp"     // net::SetupSession (present_net_setup + rotation)
#include "bomber/net/transport.hpp"         // net::Transport (the socket/star/relay seam)
#include "bomber/net/udp_transport.hpp"     // net::UdpTransport (run_netplay_match)
#include "bomber/platform/frame_clock.hpp"

#if defined(BOMBER_HAS_LOBBY)
// The lobby control plane, owned by present_net_online so it OUTLIVES the
// waiting room — the F2 chat overlay keeps using it through the setup screens.
#include "bomber/net/build_hash.hpp"
#include "bomber/net/lobby_client.hpp"
#include "bomber/net/lobby_flow.hpp"
#endif

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

// The "Loading data..." bar spans TWO port phases that together are the
// equivalent of the original's single MASTER.ALI read (sub_41D695): the ANI/
// PCX DECODE (AssetStore::load) and the per-player RECOLOR (build_player_sets
// — the port bakes recolored sprite sets where the original remapped at blit
// time, so it is extra "loading data" work with no original dialog of its
// own). Both feed one continuous 0 -> 1 bar under the same caption; decode
// owns the first slice, recolor the rest. The split is a rough work estimate,
// not RE'd — the only contract is the bar stays monotonic and the window is
// pumped throughout.
constexpr float kBootDataDecodeShare = 0.55f;

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
// The bar ANIMATES for real (2026-07-24): the port now threads a per-asset
// progress callback through AssetStore::load()/build_player_sets() (the
// "Loading data..." phase) and AudioEngine::init() (the "Loading sound..."
// phase), so `fraction` climbs 0 -> 1 as the work happens and the window is
// pumped between repaints (GameApp::draw_boot_loading). This RESTORES the
// original's live readout — sub_41D695 calls sub_412E33(100*read/total) once
// per MASTER.ALI entry, and sub_4287B9 steps a coarse 5/20/40/60/80/100 as it
// preloads the SOUNDLST groups (docs/re/frontend-flow.md "The percent-bar
// dialog"). It replaces the earlier "presented already-complete 100%"
// simplification, which — combined with the un-pumped synchronous load —
// left the window frozen at a full bar for the whole multi-second decode on a
// real (esp. DATA_HD) install (the reported "freeze + bar starts at 100%").
//
// Font: FONT6 is CONFIRMED ready before BOTH flashes (docs/re/
// frontend-flow.md "FONT6 timing" — sub_414DF4 pins it via sub_431E9C(6)
// before sub_41095A ever calls the loading dialogs). WINZ.PCX is likewise
// loaded by sub_414DF4 itself ("winz.plt"), so GameApp::init pre-warms both
// before the first flash.
void draw_boot_loading_dialog(SDL_Renderer* ren, const FontTextures& font, const Sprite* winz,
                              const char* caption, float fraction) {
    fraction = std::clamp(fraction, 0.0f, 1.0f);
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

    // Two-tone bar (sub_43D1C0 x2, docs/re/frontend-flow.md "Two-tone bar"):
    // the filled segment is width = 3*pct (== 300*fraction) in byte_49A624 ->
    // idx 178 -> (168,168,164); the unfilled remainder 3*(100-pct) is black.
    // Paint the whole 300-px track black first, then the filled prefix on top.
    const float track_x = win.x + 31.0f;
    const float track_y = win.y + 5.5f * h + 1.0f;
    SDL_FRect track{track_x, track_y, 300.0f, h - 1.0f};
    SDL_SetRenderDrawColor(ren, 0, 0, 0, 255);
    SDL_RenderFillRect(ren, &track);
    SDL_FRect bar{track_x, track_y, 300.0f * fraction, h - 1.0f};
    SDL_SetRenderDrawColor(ren, 168, 168, 164, 255);  // byte_49A624 -> idx 178
    SDL_RenderFillRect(ren, &bar);

    // "%d" readout (0..100, tracking the bar) — yellow (byte_49D37A -> idx
    // 182), y = 3.5*fontheight, horizontally centered.
    std::string pct_str = std::to_string(static_cast<int>(std::lround(fraction * 100.0f)));
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
        const std::size_t xoff = pal + std::size_t{256} * 4;                // XOR (colour) bitmap
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
    // The node name's random fallback reads MESSAGES.TXT (ids 500..548), which
    // load_assets() only just parsed — so it cannot live beside the file read
    // in load_config().
    seed_default_node_name();

    // The "Loading data..." bar continues across build_presentation's
    // per-player recolor (kBootDataDecodeShare split), so audio init — the
    // second flash, "Loading sound..." — runs LAST, after ALL sprite data is
    // decoded AND recolored. This matches the original's order (sub_41D695
    // "data" fully done before sub_42896E "sound") and keeps the whole heavy
    // graphics pipeline behind one responsive, animated bar instead of
    // freezing after the flashes (the recolor used to run un-pumped with no
    // dialog at all).
    build_presentation(ren);
    load_sound(game);
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

bool GameApp::capture_run() const {
    // The four capture entry points. `demo` itself is NOT in this list: it is
    // already pinned separately and setting it is how `--demo` requests exactly
    // this, but --demo-shots / --bm-shot / --menu-shot are equally captures and
    // were the ones being missed.
    return opts_.demo || opts_.demo_ticks > 0 || !opts_.demo_shots.empty() ||
           !opts_.bm_shot_name.empty() || !opts_.menu_shot_out.empty();
}

// The two GAMEPLAY options.ini values a capture run pins (load_config). Both are
// deliberate scenario parameters of the scripted demo match, in exactly the same
// class as its fixed seed (0xB0BB1E5) and its fixed LCG literals — chosen for
// what they make the match cover, not read from whatever the player last saved.
// They are the values the five frames in tests/visual/shots.txt were captured
// under, so this pin makes those frames reproducible rather than replacing them.
//
// Random Start ON is also the ORIGINAL's own default (VALUELST id 40 = 1,
// docs/valuelst-map.md), so a capture plays the spawn assignment a fresh install
// plays. Conveyor Speed 2 (High, 450 — ids 190-192) is the setting under which
// the scripted match actually exercises level 10's conveyor loop: the first bomb
// rides the belt across the board, so the belt draw and the belt-carried bomb are
// covered by the pins instead of being dead code the harness never reaches.
namespace {
constexpr bool kCaptureRandomStart = true;
constexpr int kCaptureConveyorSpeed = 2;
}  // namespace

bool GameApp::resolve_install_paths(fs::path& game, fs::path& scheme_path) {
    // SDL_GetBasePath is the exe's own folder; libs/assets is SDL-free so it
    // cannot ask for it itself. Without this the auto-detect only ever saw the
    // working directory, so the exe worked on the developer's machine (whose
    // absolute install path is one of the hardcoded probes) and nowhere else.
    const char* base = SDL_GetBasePath();  // SDL3: static string, do not free
    game = !opts_.game_dir.empty() ? opts_.game_dir
                                   : assets::default_game_dir(base ? fs::path(base) : fs::path{});
    if (game.empty() || !fs::is_directory(game / "DATA")) {
        // This is the ONLY failure a first-time user hits, and it used to be
        // invisible: bomber_game is a WIN32 (GUI-subsystem) binary, so a
        // double-clicked exe has no console and this text went nowhere — the
        // game just silently did not appear. Say it in a window too, and say
        // what to actually do about it. stderr is kept for CLI/CI runs.
        const std::string searched =
            game.empty() ? std::string("(no install found)") : game.string();
        const std::string msg =
            "Open Bomberman needs the data files from an original\n"
            "Atomic Bomberman (1997) installation. It could not find one.\n\n"
            "Looked at: " +
            searched +
            "\n\n"
            "Point it at your install in any ONE of these ways:\n"
            "  - copy the install folder next to this exe, named BOMBRMAN, or\n"
            "  - put a file named gamedir.txt next to this exe, holding the\n"
            "    install path on a single line (plain UTF-8, no BOM), or\n"
            "  - set the BOMBER_GAME_DIR environment variable, or\n"
            "  - pass the path as the first argument.\n\n"
            "The folder is the one containing DATA\\, e.g.\n"
            "  C:\\Program Files (x86)\\INTRPLAY\\BOMBRMAN";
        std::fprintf(stderr, "%s\n", msg.c_str());
        // NEVER on a capture run. A message box is MODAL, and the visual golden
        // harness runs --demo-shots with no install path precisely so it can
        // SKIP on the exit code — with a box in the way it waits for a click
        // that will never come, so a machine without the game hangs the test
        // instead of skipping it. A capture run has no user to inform anyway.
        if (!capture_run())
            SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Open Bomberman", msg.c_str(), nullptr);
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
        // Conveyor Speed ("conveyor_speed=") is PINNED on a capture, for the
        // same reason team_play and playtime below are: it feeds the sim
        // (Tuning::conveyor_speed, ids 190-192), so leaving it on the file made
        // the visual golden depend on a Options-screen setting the player can
        // change between two runs of the same build. It is not theoretical —
        // it is one half of why the harness went red (see kCaptureRandomStart).
        conveyor_speed_index_ =
            capture_run() ? std::optional<int>{kCaptureConveyorSpeed} : loaded_opts.conveyor_speed;
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
        // Random Start ("random_start=") is PINNED on a capture too. It shuffles
        // which of the scheme's spawn slots each player index gets (the 200-pair
        // swap, sub_421793), so it rewrites the scripted demo match from tick 1
        // — every pinned frame moves. This was THE reason visual_golden went
        // red: the pins were captured while the install's options.ini said
        // random_start=1, a later session saved random_start=0, and the same
        // source then rendered five different frames on the same machine.
        options_.random_start = capture_run()
                                    ? kCaptureRandomStart
                                    : loaded_opts.random_start.value_or(values_.at_or(40, 1) != 0);
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
        // Row 2's node name has its OWN install-root file, read here the way
        // sub_40C08C reads it from the boot init sub_40C74C — NOT an options.ini
        // key (docs/re/results-and-options.md §3 row 2). An absent/empty file
        // leaves it blank until seed_default_node_name() draws the original's
        // random fallback, which needs MESSAGES.TXT and so runs after
        // load_assets().
        node_name_path_ = game / "nodename.ini";
        node_name_loaded_ = assets::load_node_name(node_name_path_);
        options_.node_name = node_name_loaded_;
        // Row 14's four modem fields (display-only; getstring(264) — chrome
        // audit 2026-07-12): straight from options.ini's modem keys, defaults
        // = the shipped install's values.
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
        // A capture run pins all three to their faithful defaults instead of
        // honouring the file (see capture_run()). show_fps draws an overlay over
        // every captured frame; native_cadence drives the sim off the wall clock,
        // which is non-deterministic by construction; vsync/uncap changes the
        // pacing a capture has no reason to inherit. None of them belong in a
        // pixel pin, and all three come from the machine the capture happens to
        // run on.
        const bool capture = capture_run();
        uncap_fps_ = capture ? false : !loaded_opts.vsync.value_or(true);
        native_cadence_ = capture ? false : loaded_opts.native_cadence.value_or(false);
        show_fps_ = capture ? false : loaded_opts.show_fps.value_or(false);
        // "soft_scaling=" — the fourth Video Settings key, and the one with the
        // widest reach into a captured frame: it swaps nearest for linear
        // sampling on EVERY classic texture, so a saved `soft_scaling=1` would
        // re-hash all five tests/visual pins at once (the same failure mode
        // random_start/conveyor_speed/team_play/playtime/show_fps/native_cadence/
        // vsync each had). scale_filter_for() is the pin, and it is unit-tested
        // (tests/game/test_scale_filter.cpp) rather than trusted as a ternary.
        soft_scaling_ = capture ? false : loaded_opts.soft_scaling.value_or(false);
        // Set BEFORE load_assets() uploads anything, so the boot textures are
        // created with the right sampling mode instead of being re-stamped.
        set_scale_filter(scale_filter_for(soft_scaling_, capture));
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
#ifdef SDL_PLATFORM_WINDOWS
    // Renderer backend preference — a PACING fix, not a fidelity one, and the
    // single biggest lever on F8's uncapped mode (measured 2026-07-29, this
    // machine, 1280x960 windowed, 60 Hz panel, 2-player match, 5 runs each):
    //
    //                     achieved fps                 adjacent-frame jitter
    //   direct3d11   155.5 164.5 177.0 168.2 156.6     0.63-1.31 ms
    //   opengl       179.7 180.0 180.0 180.0 180.0     0.001-0.023 ms
    //
    // The cause is not our drawing, which costs a flat 0.11 ms of the 5.556 ms
    // sub-frame budget in every configuration measured (window size, roster
    // size and audio all change it by less than the run-to-run noise). It is
    // SDL_RenderPresent blocking: SDL's D3D11 backend calls
    // SetMaximumFrameLatency(dxgiDevice, 1) (SDL_render_d3d11.c), so with vsync
    // off in a DWM-composited window each present has to wait for the previous
    // flip the compositor is still holding at its own 60 Hz. Present then
    // wanders between 0.9 ms and 5.8 ms over seconds — a single 6 s sample went
    // 182, 180, 132, 100, 142, 180, 180, 152, 120 fps per half-second with the
    // CPU work dead flat throughout, which is exactly the "hits 180, drops to
    // ~140, recovers" the drop was reported as. The GL backend does not take
    // that per-frame latency wait and holds 180.0 flat.
    //
    // Fullscreen bypasses the compositor and is fine on BOTH backends (178.2
    // d3d11 / 176.4 opengl), and the vsync path is exactly 60.0 on both — so
    // this only ever moves the windowed uncapped case, which is the broken one.
    // Comma-separated: SDL walks the list and falls back on creation failure,
    // so a machine with no usable GL still gets D3D11. An explicitly-set
    // SDL_RENDER_DRIVER always wins, for A/B and for anyone the GL path fails.
    // tests/visual's pins are byte-identical under both backends (verified by
    // running visual_golden under each), so this does not touch the goldens.
    if (!SDL_getenv("SDL_RENDER_DRIVER"))
        SDL_SetHint(SDL_HINT_RENDER_DRIVER, "opengl,direct3d11,direct3d12,software");
#endif
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
    // Which backend actually won the preference list above. Cheap, once, and the
    // first thing worth knowing about any frame-rate report — the D3D11 and GL
    // paths differ by ~15 fps and three orders of magnitude of frame jitter in
    // uncapped windowed mode, so "it drops below 180" is unanswerable without it.
    if (const char* name = SDL_GetRendererName(ren)) std::fprintf(stderr, "renderer: %s\n", name);
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
    SDL_SetRenderLogicalPresentation(ren, kScreenW, kScreenH,
                                     capture_run() ? SDL_LOGICAL_PRESENTATION_LETTERBOX
                                                   : SDL_LOGICAL_PRESENTATION_STRETCH);
    if (fullscreen_ && !capture_run()) SDL_SetWindowFullscreen(win, true);
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
    assets_.load_frontend_winz(ren, game);
    draw_boot_loading("Loading data...", 0.0f);

    // Animate the "Loading data..." bar across the (multi-second, on a DATA_HD
    // install) synchronous decode: AssetStore::load reports 0 -> 1 as each
    // ANI/PCX group lands, and draw_boot_loading pumps the OS event queue +
    // repaints between chunks so the window never goes "not responding". The
    // decode owns the first kBootDataDecodeShare of the bar; the recolor in
    // build_presentation continues it to 1.0.
    if (!assets_.load(ren, game, [this](float f) {
            draw_boot_loading("Loading data...", f * kBootDataDecodeShare);
        }))
        return false;
    seqs_.resolve(assets_);

    // Initial joystick enumeration (docs/re/setup-screens.md joystick pane,
    // sub_429628). Hotplug events refresh this again in present_setup/run_match
    // so a stick plugged in after boot still shows up without a restart.
    gamepads_.refresh();
    return true;
}

void GameApp::build_presentation(SDL_Renderer* ren) {
    base_tuning_ = match::build_match_config(scheme_, 2, 0, &values_).tuning;
    // Seed setup-screen slot colours from VALUELST for any colour without a .RMP
    // tail (a loaded .RMP keeps its own authoritative tail), then build the
    // per-player recolored sprite sets (authentic .RMP remap where available).
    // The recolor is the second half of the "Loading data..." work (see
    // kBootDataDecodeShare): report its 0 -> 1 into the tail of the same bar,
    // pumping + repainting per player so this heavy step stays responsive too
    // (it previously ran with no dialog and no event pump at all).
    assets_.set_color_fallbacks(base_tuning_.color_rgb, 10);
    assets_.build_player_sets(base_tuning_.color_rgb, [this](float f) {
        draw_boot_loading("Loading data...",
                          kBootDataDecodeShare + f * (1.0f - kBootDataDecodeShare));
    });
    seqs_.resolve(assets_);  // re-resolve: player sprite sets exist now

    renderer_.emplace(ren, assets_, seqs_, values_);
    screen_.emplace(assets_, audio_);
    // front_font_ (FONT6.FON glyph textures for the dialog chrome and the .BM
    // help/credits screens) was already built above, before the boot LOADING
    // dialogs — matching sub_41095A's real init order. assets_.load() reloads
    // the same FONT6.FON into assets_.frontend_font() (harmless — identical
    // file), so no second build() is needed here.
}

void GameApp::load_sound(const fs::path& game) {
    // The second boot LOADING flash: "Loading sound..." (getstring 200, now
    // that MESSAGES.TXT is loaded), shown before the sound-preload step
    // (sub_42896E/sub_4287B9) — here, audio_.init(). Skipped in --demo mode,
    // matching that the demo path never touches audio. Runs LAST (after
    // build_presentation) so the whole "Loading data..." bar — decode AND
    // recolor — completes first, faithful to sub_41D695 preceding sub_42896E.
    // audio_.init reports 0 -> 1 across the audio-device/SOUNDLST bring-up and
    // draw_boot_loading pumps between repaints, so this flash animates too and
    // stays responsive.
    if (opts_.demo) return;
    const std::string cap = assets_.getstring(200, "Loading sound...");
    draw_boot_loading(cap.c_str(), 0.0f);
    if (!audio_.init(game, [this, &cap](float f) { draw_boot_loading(cap.c_str(), f); }))
        std::fprintf(stderr, "audio unavailable, continuing silent\n");
}

void GameApp::draw_boot_loading(const char* caption, float fraction) {
    // Automated capture modes (--demo/--demo-shots/--bm-shot/--menu-shot) run
    // scripted with no interactive window and hash frames drawn LATER by
    // run_demo/run_bm_shot/run_menu_shot, so the boot dialog is pure overhead
    // there (nothing to keep responsive; its vsync-blocked presents would only
    // slow the capture) and never reaches the hashed output. Skip it — the
    // progress callbacks then no-op and the load stays silent. Interactive
    // boots get the animated, pumped dialog below.
    if (opts_.demo || !opts_.bm_shot_name.empty() || !opts_.menu_shot_out.empty()) return;
    // Keep the OS message queue drained so the window never enters the "not
    // responding" state during the (multi-second, on a DATA_HD install)
    // synchronous asset/sound preload — the port's equivalent of the original
    // pumping Windows messages between its sub_412E33 percent repaints. The
    // global SDL_EventFilter (sdl_event_filter, installed in init_video) runs
    // synchronously inside SDL_PollEvent's pump, so Alt+Enter/F11 fullscreen
    // still works mid-load; everything else is discarded (boot has no
    // interactive screen yet — a stray window-close is simply ignored until
    // the normal event loop starts, which is imminent once loading finishes).
    SDL_Event ev;
    while (SDL_PollEvent(&ev)) {
    }
    draw_boot_loading_dialog(sdl_renderer_.get(), front_font_, &assets_.frontend_pcx("WINZ"),
                             caption, fraction);
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
// VICTORY" reading): at Round end sub_42741E(0x46A) = 1130 ("draw") replaces
// the stage track before the survivor test — so DRAW, the RESULTS tally, AND
// VICTORY/TEAM all play under 1130; nothing restarts 1020 anywhere in the
// outcome tier. A looping track (start_music), replacing the menu/stage music.
//
// "UNCONDITIONALLY" was too strong and is CORRECTED 2026-07-28
// (docs/re/sound-engine.md §9): the 1130 start at 0x42A6DD sits behind TWO
// gates that the round-loop exit passes through first.
//   * ATTRACT (0x42A6CB, `dword_464938`) — an attract round skips the whole
//     outcome tier. The port already bypasses it (run_app's attract_ branch).
//   * CAMPAIGN (0x42A63B, `dword_46489C`) — this one the port was getting
//     wrong. A campaign round end takes an entirely separate arm that shows at
//     most one modal (`sub_414340`, and only when the pacing flag dword_464894
//     is 2) and then goes STRAIGHT back into the round init sub_410B6E for the
//     next stage. It never reaches 1130, and it never reaches DRAW, the RESULTS
//     tally or VICTORY either.
constexpr int kDrawMusicId = 1130;  // 0x46A — DRAW.RSS, DRAW *and* RESULTS *and* VICTORY backdrop
// The SETUP-SCREENS track, started ONCE per Play entry at the head of
// sub_42A3F6 (0x42A436) — before sub_410F81, and so before both the goldman
// wheel and the player-select screen, which merely INHERIT it. Owned here
// because run_app's StartMatch handler is this port's sub_42A3F6 head; the
// screens themselves must not restart it (docs/re/sound-engine.md §9).
constexpr int kWinMusicId = 1020;  // 0x3FC — WIN.RSS, setup-screens backdrop (NOT victory)
// NETWORK.RSS, the net front end's own track: sub_42B0CE (START NET GAME,
// 0x42B11B) and sub_42B47D (JOIN NET GAME, 0x42B4C0) each start it as their
// first act. Nothing switches it back explicitly — the menu's outer loop
// (sub_42B9CE @0x42B9EA) re-arms its music flag on every re-entry and restarts
// 1010, which is what reclaims the loop when either screen returns.
constexpr int kNetMusicId = 1040;  // 0x410 — NETWORK.RSS
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
    // round_end = true: DRAW is NOT presented by sub_42A088's own wait loop.
    // sub_42A3F6 calls sub_42A088("draw", 0) — argument ZERO, i.e. show the
    // picture and return immediately (0x42A710) — and then runs its OWN loop at
    // 0x42A73A. That loop gives Escape no accept sting and gives the 6 s
    // auto-advance no nav blip. See ScreenDef::round_end.
    return ScreenDef{"DRAW", {}, kResultsDwellMs, /*skippable*/ true, WaitLoop::RoundEnd};
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
    // WaitLoop::TimedCut because "reads NO game key" is also an AUDIO fact: with
    // no key loop there is no nav blip and no accept sting, and the timeout is a
    // sleep expiring rather than a synthesized Enter, so it stings nothing
    // either. This screen is completely silent apart from the 2000 winner voice
    // the caller fires under it.
    return ScreenDef{victory_background_name(team_mode, player, team),
                     {},
                     /*dwell_ms*/ 3000,
                     /*skippable*/ false,
                     WaitLoop::TimedCut};
}
// The main-menu model (sub_42B9CE) — the MenuItem struct, the seven-row
// kMenuItems table (the original's selection-index dispatch order), and
// kMenuCount moved to
// screens/menu_screen.cpp with present_menu.

// Cursor anchor over MAINMENU.PCX — CONFIRMED getvalue(700/701/702) (sub_42B9CE:
// X comes from getvalue(700), Y from getvalue(701), the Y-step from
// getvalue(702); the bomb-
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

int GameApp::run_netplay() {
    // CLI netplay entry (--host/--join, ADR-0010 §3.3 step 5): ONE 2-player UDP
    // lockstep match run in place of the front-end. The CLI carries --seed on
    // BOTH peers, so there is no discovery and NO seed handshake here — bind +
    // set_peer straight from opts_ and hand off to the shared match core
    // (run_netplay_match), which agrees the CONFIG over the wire before it
    // starts (exchange_cli_netplay_config; the seed alone is not enough, because
    // the board it derives depends on both peers running the same libs/match).
    // The MENU path (present_net_host/join) runs the
    // SeedHandshake instead; both funnel into that same core. CANNOT be
    // runtime-tested here (no display / second instance) — validated live.
    const bool host = opts_.net_role == 1;
    const int local_seat = host ? 0 : 1;

    // Real UDP link (raw sockets, SDL-free). BOTH peers bind a KNOWN local port
    // and set_peer() to the other's known port: without a handshake the CLI has
    // no way to learn the peer, so requiring a fixed port + address on each side
    // (via the args) is the only symmetric MVP that works with this dumb,
    // discovery-less wire — see main.cpp's --host/--join usage.
    net::UdpTransport transport;
    if (!transport.bind(opts_.net_local_port)) {
        std::fprintf(stderr, "netplay: bind failed (local port %u)\n",
                     static_cast<unsigned>(opts_.net_local_port));
        return 1;
    }
    if (!transport.set_peer(opts_.net_peer_host, opts_.net_peer_port)) {
        std::fprintf(stderr, "netplay: cannot resolve peer %s:%u\n", opts_.net_peer_host.c_str(),
                     static_cast<unsigned>(opts_.net_peer_port));
        return 1;
    }
    std::printf("netplay: %s  bound_port=%u  peer=%s:%u  seed=0x%08X  seat=%d\n",
                host ? "HOST" : "GUEST", static_cast<unsigned>(transport.local_port()),
                opts_.net_peer_host.c_str(), static_cast<unsigned>(opts_.net_peer_port),
                static_cast<unsigned>(opts_.net_seed), local_seat);

    run_netplay_match(transport, opts_.net_role, opts_.net_seed);
    return 0;  // CLI always exits 0 after the single match (window-close included)
}

sim::MatchConfig GameApp::canonical_netplay_config(std::uint32_t seed) const {
    // CANONICAL 2-human MatchConfig, built from the shared seed alone and
    // independent of each machine's options.ini / level pick.
    //
    // IT IS NO LONGER DERIVED ON BOTH PEERS. Only the HOST calls this; the guest
    // receives the resulting bytes through exchange_cli_netplay_config, because
    // "the same seed gives the same board" holds only while both peers run the
    // same libs/match — and nothing on the CLI wire could check that.
    // Deterministic lockstep needs an identical seed AND identical config
    // (ADR-0010 seed/roster parity), so this deliberately IGNORES the live
    // per-machine options_/selected_level_/team_play_/gold state and builds only
    // from the shared seed + the default scheme_ + the install VALUELST (values_
    // — the same 1997 file on both installs). Mirrors start_match's config build,
    // minus every per-machine overlay; random_start off so the spawn assignment is
    // fixed too.
    //
    // THIS IS NO LONGER THE ONLINE PATH. It used to be built inline by
    // run_netplay_match_seats for EVERY netplay caller, which is exactly why an
    // online match had no map choice, no AI and no roster; the interactive paths
    // now agree a real config through present_net_setup. What is left here serves
    // only `--host`/`--join`, which are scripted and have no screens to drive.
    sim::MatchConfig cfg = match::build_match_config(scheme_, 2, seed, &values_,
                                                     /*random_start=*/false);
    for (int i = 0; i < sim::kMaxPlayers; ++i) {
        cfg.active[i] = i < 2;  // seats 0,1 are the two humans; the rest OFF
        cfg.ai[i] = false;      // both driven by wire input, never the AISystem
        cfg.team[i] = 0;        // solo (no team mode)
    }
    // Fixed stage from the SHARED seed (both peers resolve the same built-in via
    // the default registry — not ctx assets.levels(), whose custom maps could
    // differ per machine), then overlay its EXTRA<n>.RES actors: hashed setup
    // inputs like the cell grid, deterministic given the same seed + install.
    const int stage = match::pick_stage(base_tuning_, seed);
    cfg.tuning.level_index = stage;
    const auto actors =
        assets::extra::load_for_board(opts_.game_dir, stage, sim::kGridWidth, sim::kGridHeight);
    match::apply_actors(cfg, actors, seed);
    return cfg;
}

bool GameApp::exchange_cli_netplay_config(net::UdpTransport& transport, bool is_host,
                                          std::uint32_t seed, sim::MatchConfig& out_cfg) {
    // THE CLI'S CONFIG EXCHANGE. It exists because "both peers derive the same
    // board from the same seed" is a promise the code cannot keep across builds.
    // canonical_netplay_config() runs the WHOLE derivation locally — the .SCH
    // fill, pick_stage, EXTRA<n>.RES and match::apply_actors — and all of that
    // lives in libs/match, OUTSIDE the reach of every guard we have: build_hash's
    // scenarios hand-build a MatchConfig and never call apply_actors, and the CLI
    // path consults build_hash (or any version field) at NO point anyway — it has
    // no handshake at all. So a board-derivation change shipped to one peer and
    // not the other used to produce two different boards and a tick-0 desync with
    // nothing on the wire to catch it. That is not hypothetical: dropping
    // apply_actors' actor-tile blanking (docs/re/facts.md "Stage actors do not
    // clear the tile they sit on") moves up to 43% of a stage's bricks and moves
    // neither the goldens nor build_hash.
    //
    // The fix is to stop deriving the board twice, exactly as the interactive
    // lobby path already does (present_net_setup): the HOST derives it and ships
    // the whole serialized sim::MatchConfig (match_config_codec.hpp), the guest
    // uses those bytes verbatim. Then no future change to libs/match can make two
    // peers disagree, whether or not anyone remembers to move a digest.
    //
    // It is also SELF-ENFORCING against an unpatched partner, which is why this
    // needs no kWireProtocolVersion bump (nothing in this path reads one): a peer
    // built before this change sends no setup traffic and answers none, so a new
    // HOST never collects its ack and a new GUEST never receives a config —
    // either way the exchange times out and the match is REFUSED here, loudly,
    // instead of starting on two different boards.

    // Shorter than SetupSession's 30 s default: --host/--join are SCRIPTED, so
    // there is no human editing a roster on the far side to wait for.
    constexpr int kCliSetupTimeoutMs = 15000;
    constexpr std::uint64_t kCliSetupSettleMs = 300;

    const std::uint16_t local_seats = is_host ? std::uint16_t{0b01} : std::uint16_t{0b10};
    const std::uint16_t guest_seats = is_host ? std::uint16_t{0b10} : std::uint16_t{0};
    net::SetupSession session(transport, is_host, local_seats, guest_seats, kCliSetupTimeoutMs);
    if (is_host) session.confirm(canonical_netplay_config(seed));

    while (session.phase() != net::SetupSession::Phase::Final) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev))
            if (ev.type == SDL_EVENT_QUIT) return false;
        session.step(static_cast<std::int64_t>(SDL_GetTicks()));
        if (session.failed()) {
            std::fprintf(stderr,
                         "netplay: match-config exchange timed out after %d ms — the peer is gone "
                         "or is running a build from before the CLI exchanged configs.\n",
                         kCliSetupTimeoutMs);
            return false;
        }
        SDL_Delay(2);
    }
    out_cfg = session.final_config();

    if (!is_host) {
        // SETTLE, for the reason present_net_setup's guest settles: our ack may
        // be the datagram that is lost, and SetupSession only re-acks when the
        // host's next burst arrives — which we would never see if we left the
        // instant we decoded. Safe against the one-pump-at-a-time rule because
        // RollbackSession re-sends its whole input window every pump.
        const std::uint64_t settle_until = SDL_GetTicks() + kCliSetupSettleMs;
        while (SDL_GetTicks() < settle_until) {
            SDL_Event sev;
            while (SDL_PollEvent(&sev))
                if (sev.type == SDL_EVENT_QUIT) return false;
            session.step(static_cast<std::int64_t>(SDL_GetTicks()));
            SDL_Delay(2);
        }
    }
    std::printf("netplay: match config agreed (%s), stage %d\n", is_host ? "sent" : "received",
                out_cfg.tuning.level_index);
    return true;
}

AppInput GameApp::run_netplay_match(net::UdpTransport& transport, int role, std::uint32_t seed) {
    // The ADR-0010 role-derived CLI entry (--host/--join): host owns seat 0, guest
    // seat 1, and the config is the HOST's canonical_netplay_config(seed) shipped
    // over the wire — never derived twice (see exchange_cli_netplay_config).
    const bool is_host = role == 1;
    sim::MatchConfig cfg;
    if (!exchange_cli_netplay_config(transport, is_host, seed, cfg)) return AppInput::Back;
    return run_netplay_match_seats(transport, is_host ? 0b01u : 0b10u, /*all_seats=*/0b11u, is_host,
                                   cfg);
}

namespace {

// THE BETWEEN-ROUNDS GATE for an online match (net_round_gate.hpp), implemented
// over the SAME net::SetupSession the pre-match setup stage already uses: round
// N+1's config is just another host confirmation, so the whole rotation needs no
// new MsgType and no protocol-version bump.
//
//   HOST  — pre-builds the next round's config, confirms it the moment the local
//           player accepts the outcome screen (the original's host-only
//           screen-advance, docs/re/network-screens.md §7 kind 32), and keeps the
//           screen up until the guest has acknowledged those exact bytes.
//   GUEST — never dismisses (`sub_40C06A() == 1`); its screen ends when the
//           host's confirmation FOR THIS ROUND arrives.
//
// "For this round" is checked against net::round_seed(): a round's config carries
// its own seed, derived identically on both peers from the match seed + the round
// index, so a blob replayed out of an earlier round can never be mistaken for the
// next one. That is the whole identity check — the config itself still travels in
// full, because a guest whose .SCH/EXTRA/VALUELST differ would build a different
// board from the same seed (setup_session.hpp, "the final may not be approximate").
//
// The host also publishes ONE heartbeat preview: SetupSession's guest liveness
// clock only advances on inbound setup traffic, so without it a host that reads
// the scoreboard for longer than the session timeout would look, to the guest,
// exactly like a host that had quit.
// N-PEER NOTE: `ready()` on the host is SetupSession::peer_acked(), which is now
// "every guest acked", not "somebody did" — so a 3+ peer round gate holds the
// scoreboard up until the whole table has the next round's bytes.
class RoundRotationGate final : public NetRoundGate {
public:
    RoundRotationGate(net::SetupSession& session, bool host, sim::MatchConfig next,
                      std::uint32_t expect_seed)
        : session_(&session), next_(std::move(next)), expect_seed_(expect_seed), host_(host) {
        if (host_) session_->publish(net::SetupPreviewFrame{});  // liveness only; never displayed
    }

    void pump() override { session_->step(static_cast<std::int64_t>(SDL_GetTicks())); }
    bool readonly() const override { return !host_; }

    void accept() override {
        if (!host_ || confirmed_) return;
        session_->confirm(next_);
        confirmed_ = true;
    }

    bool ready() const override {
        if (host_) return confirmed_ && session_->peer_acked();
        return session_->has_final_config() && session_->final_config().seed == expect_seed_;
    }

    bool failed() const override { return session_->failed(); }

    // The agreed next-round config: our own bytes on the host, the host's exact
    // decoded bytes on the guest (match_config_codec.hpp). Only valid once
    // ready() — the caller checks that first.
    const sim::MatchConfig& next_config() const { return host_ ? next_ : session_->final_config(); }

private:
    net::SetupSession* session_;
    sim::MatchConfig next_;
    std::uint32_t expect_seed_;
    bool host_;
    bool confirmed_ = false;
};

// After the gate opens, the guest has sent exactly ONE ack and is about to hand
// the socket to the match session. If that ack was lost the host would sit until
// its own timeout, so keep pumping briefly — the same 300 ms settle, for the same
// reason, as present_net_setup's own exit (whose comment carries the full
// argument for why swallowing a few of the peer's early input datagrams here is
// harmless: RollbackSession re-sends its whole unconfirmed window every pump).
constexpr std::uint64_t kRoundHandoffSettleMs = 300;

// THE POST-MATCH GATE — the same NetRoundGate seam the between-ROUNDS rotation
// uses (screens/net_round_gate.hpp), driving net::RematchSession instead of a
// config exchange, because a decided match has no next round to agree on. It has
// TWO PHASES over one session, and the split is the whole design:
//
//   PHASE A — the clinch RESULTS scoreboard. Each peer dismisses its OWN, exactly
//     as before this change: the outcome was computed identically on both sides
//     with no traffic, so there is nothing to agree. The gate is present only to
//     PUMP — which is what keeps the host's liveness flowing while it reads the
//     board, and what drains the socket of the round that just ended.
//
//   PHASE B — the VICTORY screen, the LAST thing before the setup stage. Here the
//     original's rule applies (docs/re/network-screens.md §7): the machine
//     driving the game dismisses the shared screen and a client follows. It has
//     to, because the guest's next SetupSession starts a liveness timeout the
//     moment it is built — a guest that walked into the roster screen ahead of a
//     host still reading VICTORY would time out and report THE HOST LEFT THE
//     GAME. Which is the disconnect this whole change removes, thirty seconds
//     later.
//
// Escape is exempt on both screens (asset_screen.cpp / results_screens.cpp both
// route it around the gate): leaving the session is always the local player's own
// call, and it simply means no rematch.
class RematchGate final : public NetRoundGate {
public:
    explicit RematchGate(net::RematchSession& session) : session_(&session) {}

    void pump() override { session_->step(static_cast<std::int64_t>(SDL_GetTicks())); }

    // Phase A: nobody is read-only. Phase B: the guest follows the host.
    bool readonly() const override { return final_phase_ && !session_->is_host(); }

    void accept() override {
        if (final_phase_)
            session_->accept();  // HOST: announce the walk back to the setup screens
        else
            local_accepted_ = true;
    }

    bool ready() const override { return final_phase_ ? session_->ready() : local_accepted_; }
    bool failed() const override { return session_->failed(); }

    // Called between the two screens. One session spans both so its liveness
    // clock never restarts and never has a gap in it.
    void begin_final_phase() { final_phase_ = true; }

private:
    net::RematchSession* session_;
    bool final_phase_ = false;
    bool local_accepted_ = false;
};

// A peer that has been told the round ends at tick X may still be short of it,
// and it can only get there on OUR input window. Keep pumping the (now
// non-simulating) session this long before the between-rounds gate takes the
// socket. Longer than the round-handoff settle above because it covers a real
// catch-up, not just one lost ack.
constexpr std::uint64_t kAbandonSettleMs = 600;

// The round counter feeding net::round_tick_base is walked across every match
// played on one transport, so it has to be kept inside the range that function
// documents as wrap-free (100 << 22 is "an order of magnitude inside uint32").
// Both peers count the same rounds — the rotation is agreed — so both wrap on
// the same round and stay in the same tick space. By the time 1024 rounds have
// been played, a straggler from round 0 is many hours dead.
constexpr int kNetRoundBaseWrap = 1024;

// THE EVIDENCE A DEAD SESSION LEAVES BEHIND.
//
// "It suddenly cut out" was unanswerable because by the time the player has
// alt-tabbed to say so, the window and everything on it are gone. So the reason
// and the last numbers are written to netdiag.log next to the executable — and
// written from a DESTRUCTOR, not from each of run_netplay_match_seats' several
// exits, because the one path guaranteed to matter is the one nobody remembered
// to instrument. A session that dies leaves a record however it died.
//
// The reason itself is latched as the match runs. Unknown means the function
// left by a path that had no opinion, which is itself worth seeing in the log
// rather than being papered over with a plausible guess.
class NetSessionRecorder {
public:
    NetSessionRecorder(std::uint16_t local_seats, std::uint16_t all_seats, bool is_host) {
        summary_.timestamp = net_log_timestamp();
        summary_.local_seats = local_seats;
        summary_.all_seats = all_seats;
        summary_.is_host = is_host;
    }
    NetSessionRecorder(const NetSessionRecorder&) = delete;
    NetSessionRecorder& operator=(const NetSessionRecorder&) = delete;
    NetSessionRecorder(NetSessionRecorder&&) = delete;
    NetSessionRecorder& operator=(NetSessionRecorder&&) = delete;
    ~NetSessionRecorder() {
        // Stamped at the END, so the line carries when the session died rather
        // than when it started — which is the question being asked of it.
        summary_.timestamp = net_log_timestamp();
        append_net_session_log(summary_);
    }

    // One session per ROUND, so the newest snapshot is the one that was live
    // when whatever happened happened.
    void snapshot(const net::RollbackSession& s, int round) {
        summary_.stats = s.stats();
        summary_.round = round;
    }
    void latch(net::SessionEndReason r) { summary_.reason = r; }
    void note(std::string n) { summary_.note = std::move(n); }
    const net::SessionSummary& summary() const { return summary_; }

private:
    net::SessionSummary summary_;
};

}  // namespace

AppInput GameApp::run_netplay_match_seats(net::Transport& transport, std::uint16_t local_seats,
                                          std::uint16_t all_seats, bool is_host,
                                          const sim::MatchConfig& cfg, int* round_base,
                                          bool* rematch) {
    // The match-running CORE shared by the CLI (run_netplay), the direct connect
    // screens, and the online lobby: given an ALREADY-connected transport, the
    // seats THIS peer owns, and the AGREED config, build a byte-identical arena
    // and run the MATCH — a best-of-N sequence of ROUNDS, exactly like the local
    // Play flow (docs/re/in-match-shell.md "The round-end shell": `sub_42A3F6`
    // loops back into `sub_410B6E` until somebody clinches) — through the SAME
    // MatchRunner a local match uses.
    //
    // Seat bitmask (bit s == seat s, matching LockstepSession::fill_seats).
    // `all_seats` is EVERY network seat the caller was given — the server's
    // seat_assign online, 0b11 for the CLI/LAN pairs — and `transport` is
    // whatever carries them: a bare socket for a pair, a StarHubTransport on the
    // hub of a >2-seat match (ADR-0011 decisions 2+4). Neither the session nor
    // anything below it needs to know which: RollbackSession already accepts
    // arbitrary masks and the star is a Transport like any other.
    //
    // AI slots are NOT in this mask — they are simulated identically on every
    // peer from the shared config and their input is never exchanged
    // (rollback_session.hpp), so the roster can still hold ten PLAYERS over
    // fewer seats.

    // Fresh MATCH tally (the local path's reset_match_scores, minus its
    // win_target_ write): win_target_ is the LEVEL & ROUNDS screen's WINS row and
    // was already agreed during present_net_setup — the host committed its own,
    // the guest committed the mirrored preview (map_select_screen.cpp) — so
    // stamping getvalue(310) over it here would silently shorten the match on
    // both peers. The CLI (--host/--join) has no level screen and keeps whatever
    // default init() seeded.
    win_count_.fill(0);
    kill_count_.fill(0);

    // is_team_mode()/draw_player_row read the team GATE, not just the per-slot
    // bytes, and the agreed config is the only authority for it online (team play
    // off leaves every cfg.team[] at 0 — build_config's `cfg.team.fill(0)`).
    // Restored after the MATCH so a following LOCAL game keeps the player's own
    // Options setting; team_play_ is a mirror of options_.team_play and never
    // reaches options.ini on its own, so nothing is persisted either way.
    const bool saved_team_play = team_play_;

    // The MATCH seed. Round 0's config carries it and every later round derives
    // its own from it (net::round_seed) — identically on both peers, with no
    // traffic — so the two never disagree about which round they are entering.
    const std::uint32_t match_seed = cfg.seed;
    sim::MatchConfig round_cfg = cfg;
    // Where this match's rounds sit in the shared tick space (see the parameter's
    // doc comment): 0 for a one-match caller, the running total for a session
    // that keeps the transport alive across matches.
    const int base_round = round_base != nullptr ? *round_base : 0;
    if (rematch != nullptr) *rematch = false;

    // Diagnostics (screens/net_overlay.hpp): whatever happens below — including
    // the window closing mid-round — this object writes one netdiag.log line on
    // the way out with the reason and the session's last numbers.
    NetSessionRecorder recorder(local_seats, all_seats, is_host);

    AppInput result = AppInput::Advance;
    for (int round = 0;; ++round) {
        if (round_base != nullptr) *round_base = (base_round + round) % kNetRoundBaseWrap;
        // The config arrives whole (present_net_setup's confirmation on the host,
        // SetupSession::final_config()'s exact bytes on the guest, or the
        // canonical build on the CLI; for round > 0, the same confirm/ack
        // exchange run by RoundRotationGate below) — grid, actors, warps, spawns,
        // roster, tuning and seed all included, so nothing here re-derives
        // anything per machine.
        const int stage = round_cfg.tuning.level_index;
        sim_ = sim::Simulation(round_cfg);

        // Presentation setup — mirrors MatchRunner::start_match's tail (which
        // run() SKIPS for a netplay match, since we seed sim_ from the agreed
        // config here): stage art + a live music track + a fresh renderer/HUD +
        // sound state. Re-run every round because a RANDOM level rotates the
        // stage between rounds exactly as it does locally (the host's
        // build_config resolves it from that round's seed).
        //
        // "disable_game_music is presentation-only (never sim), so netplay just
        // keeps music on" — that stood here and was wrong twice over. Being
        // presentation-only is exactly why the option can be honoured online: it
        // needs no agreement with the peer, it moves no hash, and each player's
        // own options.ini is the only thing that should decide whether their
        // machine plays music. The original agrees — the guard at 0x410E88 is a
        // bare test of dword_4648C0 with no network arm (docs/re/sound-engine.md
        // §9), and the ONE place the round-music path does consult sub_40C06A is
        // inside sub_4293E5, choosing GENERIC over the per-level track, never
        // whether music plays at all. So this shares round_music_id() with the
        // local path rather than keeping a second, divergent copy.
        if (assets_.load_stage(stage)) seqs_.resolve_stage(assets_, stage);
        // Not gated on the art loading, for the same reason as the local path.
        const int track = round_music_id(stage, options_.disable_game_music,
                                         [this](int id) { return audio_.has_track(id); });
        if (track == kRoundMusicSilent)
            audio_.stop_music();
        else
            audio_.start_music(track);
        // Untimed HUD from the AGREED config, not from this peer's own
        // options.ini. Play Time travels as part of the host's Tuning, so the
        // CLOCK was already the host's — but this flag was hardcoded false, so
        // a host playing "Infinite" left both peers watching the 99999 s
        // stand-in count down (~27 h) instead of hiding the clock. The mirror
        // case was worse: a guest whose own options.ini said Infinite hid the
        // clock on a match that really did end on time.
        renderer_->reset_match(  // NOLINT(bugprone-unchecked-optional-access)
            is_unlimited_game_seconds(round_cfg.tuning.game_seconds));
        sounds_.reset();

        // PRESENTATION roster, derived from the AGREED config so both peers show
        // the same thing. It drives MatchRunner::collect_inputs (which only ever
        // reads a KEYBOARD/JOYSTICK slot) and the in-round player-row HUD; the
        // match roster itself is the config's own active/ai/team.
        //   * a LOCAL seat reads keyboard key-set 0 (the arrow keys), so the human
        //     at this machine plays with arrows regardless of which seat they own;
        //   * an AI slot is COMPUTER — collect_inputs leaves it neutral and the
        //     deterministic AISystem drives it identically on both peers;
        //   * every other active slot is type 4 OTHER, the original's own marker
        //     for "someone else's player" (docs/re/network-screens.md §7,
        //     sub_40D372). collect_inputs leaves it neutral too and the rollback
        //     session overwrites it from the wire before every tick().
        setup_type_.fill(static_cast<int>(SlotInputType::Off));
        setup_sub_.fill(0);
        setup_team_.fill(0);
        // Driven off the MASK, not a single seat index: today exactly one bit is
        // set (2-seat rooms), but reading the mask means a future peer that owns
        // two seats gets its second one mapped to key-set 1 with no change here.
        int key_set = 0;
        for (int s = 0; s < sim::kMaxPlayers; ++s) {
            const auto slot = static_cast<std::size_t>(s);
            if (!round_cfg.active[slot]) continue;
            if (round_cfg.ai[slot])
                setup_type_[slot] = static_cast<int>(SlotInputType::Computer);
            else if ((local_seats & (1u << s)) != 0u) {
                setup_type_[slot] = static_cast<int>(SlotInputType::Keyboard);
                setup_sub_[slot] = key_set++;
            } else
                setup_type_[slot] = static_cast<int>(SlotInputType::Other);
            // MatchConfig::team[] is the +84 byte shifted up by one
            // (build_config's "sim team 0 = solo side" note); shift it back for
            // the presentation roster the HUD/outcome helpers read.
            setup_team_[slot] = round_cfg.team[slot] != 0 ? round_cfg.team[slot] - 1 : 0;
        }
        team_play_ = std::any_of(round_cfg.team.begin(), round_cfg.team.end(),
                                 [](std::uint8_t t) { return t != 0; });

        // Drive the SAME MatchRunner as a local match (reusing all its render /
        // present / pacing / round-end): the seam's net_session routes each fixed
        // tick through the ROLLBACK session. Rollback gives ZERO input delay (it
        // predicts the peer's input and re-simulates on a miss) so the match feels
        // local even over the wire — the input-delay lockstep this replaced added
        // a fixed ~200 ms of lag. max_prediction=8 ticks caps how far the display
        // may run ahead of the peer.
        // Peer-drop policy (ADR-0011 Risks): past a hard silence window a seat is
        // declared dropped. With the RE'd Options row 12 ON the HOST announces the
        // handoff and every peer moves that seat to the deterministic AISystem at
        // the same tick, so the match plays on; with it OFF the drop ends the
        // match loudly instead of hanging. This is that option's FIRST consumer —
        // it reached CFG.INI and the Options screen and stopped there until now.
        // The window is deliberately LONG (600 pumps = 30 s at 20 Hz): crossing it
        // is irreversible with row 12 off, and the counter resets on any input, so
        // a peer that returns inside it costs nothing. An earlier 2.5 s killed a
        // match whenever someone dragged their window — Windows blocks the message
        // pump for the whole drag, so the peer just stops existing for as long as
        // the mouse is held. This must mean "genuinely gone", not "briefly busy".
        //
        // ONE SESSION PER ROUND, over the same socket — but NOT restarting the
        // tick count: round N is based at net::round_tick_base(N) so a datagram
        // straggling out of round N-1 carries a tick below this round's
        // confirmed_ and is dropped by the session's existing guard, instead of
        // being filed as a far-future input or compared as a phantom peer hash
        // (rollback_session.hpp's `start_tick`).
        const net::DropPolicy drop{options_.lost_net_revert_ai, is_host, /*timeout_ticks=*/600};
        net::RollbackSession session(sim_, local_seats, all_seats, /*max_prediction=*/8, transport,
                                     drop,
                                     net::round_tick_base((base_round + round) % kNetRoundBaseWrap));
        MatchRunnerState mrs = match_runner_state();
        mrs.net_session = &session;
        mrs.net_local_seats = local_seats;
        mrs.net_is_host = is_host;
        NetLeave left = NetLeave::None;
        mrs.net_leave = &left;
        result = MatchRunner(sctx(), mrs).run();
        // Whatever this round ended as, the log line should carry ITS numbers.
        recorder.snapshot(session, round);

        if (session.desynced()) {
            std::fprintf(stderr,
                         "netplay: DESYNC at tick %u — peers diverged (config/seed mismatch?)\n",
                         session.desync_tick());
            recorder.latch(net::SessionEndReason::Desync);
            // ON SCREEN as well as in the log: this used to be a stderr line on a
            // GUI build nobody sees, so from the player's side the match simply
            // stopped for no stated reason.
            if (present_net_session_end(sctx(), recorder.summary()) == AppInput::Quit)
                result = AppInput::Quit;
            break;
        }
        if (session.aborted()) {
            // Options row 12 off: a peer went silent and the match ends rather
            // than handing its seat to the AI. Say so — not a normal round end.
            std::fprintf(stderr,
                         "netplay: a player dropped; match ended (turn on \"Lost net players "
                         "revert to AIs\" to play on)\n");
            recorder.latch(net::SessionEndReason::PeerDropped);
            recorder.note("silence past the 600-pump timeout with Options row 12 off");
            if (present_net_session_end(sctx(), recorder.summary()) == AppInput::Quit)
                result = AppInput::Quit;
            break;
        }
        if (result != AppInput::MatchOver) {
            // The window closed under the match. No modal — there is nothing left
            // to show it on — but the log line is exactly why this case is worth
            // latching: it is the one the player cannot report themselves.
            recorder.latch(net::SessionEndReason::WindowClosed);
            break;
        }
        // THE LOCAL PLAYER WALKED OUT — Ctrl+Q's faithful forfeit, or the
        // double-Esc bail-out from a match that stopped responding. Checked FIRST
        // and read from an explicit flag, not inferred: the old test for Ctrl+Q
        // ("more than one side alive and time left") sat BELOW the abandon branch,
        // so a bail-out pressed after an abandon had been agreed would have been
        // read as a draw and rotated into another round instead of leaving.
        //
        // Everything a bail-out needs happens by simply LEAVING THIS LOOP, which
        // is what makes it unilateral: `break` runs the RollbackSession destructor,
        // returns Advance with *rematch still false, so run_netplay_session
        // returns, the enclosing present_net_* frame drops its UdpTransport (whose
        // destructor closes the socket), and run_app maps NetHost/NetJoin+Advance
        // back to the main menu. No confirmation, no message, nothing waited on.
        if (left != NetLeave::None) {
            const bool stalled = left == NetLeave::Stalled;
            result = AppInput::Advance;  // straight out to the menu
            recorder.latch(stalled ? net::SessionEndReason::LeftStalled
                                   : net::SessionEndReason::LeftSession);
            // The numbers at the moment they gave up are the whole value of the
            // record: "left-stalled with depth 8/8" is a bug report, where a bare
            // "left" is a player who stopped enjoying themselves.
            const net::NetStats& ns = session.stats();
            char note[128];
            std::snprintf(note, sizeof(note),
                          stalled ? "double-Esc bail-out; depth=%d/%d stalls=%u rephase=%u"
                                  : "Ctrl+Q forfeit; depth=%d/%d stalls=%u rephase=%u",
                          ns.prediction_depth, ns.max_prediction,
                          static_cast<unsigned>(ns.stall_pumps),
                          static_cast<unsigned>(ns.rephase_holds));
            recorder.note(note);
            break;
        }
        // AN ABANDONED ROUND (somebody pressed Esc). The decision was the HOST's
        // and it travelled as MatchCtlKind::EndRound, so both peers stopped at
        // the same tick and both take this branch — it is not a local reading of
        // a local keypress. The round is a DRAW BY DECREE: nothing below asks the
        // frozen state who won, which is what makes the abandon immune to the two
        // peers' last speculative ticks differing.
        //
        // round_ended(), NOT end_round_scheduled(): the round must actually have
        // REACHED the agreed tick. An abandon is announced a second or so ahead
        // of itself, and Ctrl+Q inside that window still has to mean "leave the
        // session" rather than being swallowed into a draw the player never got
        // to see.
        const bool abandoned = session.round_ended();
        if (abandoned) {
            // The peer may still be short of the agreed tick, and only OUR input
            // window can get it there. advance() no longer simulates once
            // round_ended() — it just receives, re-announces and re-sends — so
            // this is a pure catch-up pump.
            const std::uint64_t settle_until = SDL_GetTicks() + kAbandonSettleMs;
            while (SDL_GetTicks() < settle_until) {
                SDL_Event sev;
                while (SDL_PollEvent(&sev))
                    if (sev.type == SDL_EVENT_QUIT) {
                        team_play_ = saved_team_play;
                        recorder.latch(net::SessionEndReason::WindowClosed);
                        return AppInput::Quit;
                    }
                session.advance(sim::TickInputs{}, static_cast<std::int64_t>(SDL_GetTicks()));
                SDL_Delay(2);
            }
            recorder.snapshot(session, round);  // the catch-up pump moved the numbers
            // Latched, not final: the match usually carries on into another
            // round, and any later exit overwrites this. It matters for the case
            // where it does NOT — an Esc at the DRAW or scoreboard right after —
            // so the log says "somebody abandoned" rather than a bare "left".
            recorder.latch(net::SessionEndReason::RoundAbandoned);
            std::printf("netplay: round %d abandoned at tick %u — draw\n", round,
                        static_cast<unsigned>(session.end_round_tick()));
        }
        // (The Ctrl+Q forfeit used to be DEDUCED here, from "MatchOver but more
        // than one side is alive and the clock has time left". It is now stated
        // outright by the branch above, alongside the double-Esc bail-out it
        // shares a teardown with — see that branch for why the deduction had to
        // go rather than merely move.)

        // THE OUTCOME, computed with no traffic at all: both peers ran the same
        // deterministic sim over the same inputs, so round_winner()/the tally/the
        // clinch agree by construction — there is nothing here for the host to
        // announce. (The Goldman wheel and the campaign round-pacing overrides the
        // LOCAL results tail also runs are both local-only in the original —
        // `sub_4034BC` is gated `!sub_40C06A()`, docs/re/goldman-roulette.md §2 —
        // so an online match legitimately skips them.)
        const int w = abandoned ? -1 : round_winner();
        // Same team mirror as the local tail (sub_421B56) — both peers run it
        // over identical state, so the tallies stay identical too.
        if (w >= 0) award_round_win(w);
        const int clinched = w >= 0 ? match_clinch() : -1;
        if (clinched >= 0) {
            // MATCH win — the same clinch tier the local path shows: the RESULTS
            // scoreboard carrying the "WINS THE MATCH!" line with the 2000 winner
            // voice under it, then VICTORY<n>/TEAM<n>. Both peers reach this
            // independently and identically (same sim, same tally), so the
            // OUTCOME needs no agreement.
            //
            // WHAT DOES need agreeing is what happens NEXT. This used to be the
            // end of the road: the loop broke, the caller returned, and the
            // transport the peers had punched a path for was destroyed — so two
            // people who had just finished a game and wanted another one were
            // back at the lobby. Instead both peers now walk back to the SETUP
            // screens over the SAME link (run_netplay_session), and the
            // RematchGate below is the door: it keeps the host's liveness flowing
            // under these two screens and makes the exit from VICTORY the host's
            // call, so the guest's next SetupSession is never built into silence.
            net::RematchSession rematch_session(transport, is_host);
            RematchGate gate(rematch_session);
            audio_.start_music(kDrawMusicId);        // 1130 under RESULTS/VICTORY (doc §2)
            audio_.play_random_in_range(2000, 2299);  // winner voice — clinch only
            ScoreboardState csbs = scoreboard_state();
            // Phase A: pump only — each peer still dismisses its own board.
            if (rematch != nullptr) csbs.net_gate = &gate;
            result = ScoreboardScreen(sctx(), csbs).run();
            if (result == AppInput::Quit) break;
            gate.begin_final_phase();  // Phase B: the host dismisses, the guest follows
            result = present_asset_screen(
                sctx(), victory_screen(is_team_mode(), clinched, setup_team_[clinched]),
                rematch != nullptr ? &gate : nullptr);
            if (result == AppInput::Quit) break;
            // Advance means the gate opened (the host walked back to setup and
            // said so); Back means this player pressed Escape and is done. A
            // failed gate is the peer having vanished — also done, quietly: the
            // match itself is complete either way.
            if (rematch != nullptr && result == AppInput::Advance && gate.ready())
                *rematch = true;
            result = AppInput::Advance;
            recorder.latch(net::SessionEndReason::MatchCompleted);
            break;
        }

        // NOT DECIDED — another round. The host builds it (from the SAME screens'
        // state that produced round 0, so the roster and the level choice carry
        // over; a RANDOM level rotates because the seed moved) and confirms it
        // through the gate; the guest takes the host's exact bytes.
        net::SetupSession rotate(transport, is_host, local_seats,
                                 static_cast<std::uint16_t>(all_seats & ~local_seats));
        const std::uint32_t next_seed = net::round_seed(match_seed, round + 1);
        RoundRotationGate gate(rotate, is_host,
                               is_host ? MatchRunner(sctx(), match_runner_state())
                                             .build_config(next_seed)
                                       : sim::MatchConfig{},
                               next_seed);

        audio_.start_music(kDrawMusicId);  // 1130 under DRAW *and* RESULTS (doc §2)
        if (w < 0) {
            // DRAW is a PREFIX to the tally, not an alternative (raw
            // 0x42A875-0x42A88B falls through into RESULTS) — so a drawn round
            // shows DRAW.PCX first here too. It is deliberately NOT gated: no
            // ticks run under either screen, and the tally behind it IS the
            // synchronisation point, so the two peers dismissing DRAW at
            // different moments costs nothing but each waiting on the tally
            // instead. (The original instead broadcasts a second advance for
            // this screen — kind 32 payload 904 — which our wire has no need of
            // once the config exchange is the barrier.)
            audio_.play_random_in_range(kDrawStingLo, kDrawStingHi);
            ScreenDef ds = draw_screen();
            // ADVANCING IS THE HOST'S. The host waits for its own Enter (dwell 0);
            // a GUEST is never asked to press anything and simply auto-advances on
            // DRAW's own 6 s dwell — the original's `sub_42A3F6` auto-advance — into
            // the scoreboard, where it already waits for the host's next-round
            // confirmation. Requiring Enter on BOTH machines here is what the owner
            // hit: two people staring at DRAW.PCX, each waiting for the other.
            //
            // Deliberately NOT gated on the rotation gate: that gate's ready() is
            // "the next round is agreed", and consuming it here would flash the
            // scoreboard past before either player could read it.
            if (is_host) ds.dwell_ms = 0;
            result = present_asset_screen(sctx(), ds);
            if (result != AppInput::Advance) {
                recorder.latch(result == AppInput::Quit ? net::SessionEndReason::WindowClosed
                                                        : net::SessionEndReason::LeftSession);
                if (result != AppInput::Quit) result = AppInput::Advance;  // Esc: abandon
                break;
            }
        }
        ScoreboardState sbs = scoreboard_state();
        sbs.net_gate = &gate;
        result = ScoreboardScreen(sctx(), sbs).run();
        if (result == AppInput::Quit) {
            recorder.latch(net::SessionEndReason::WindowClosed);
            break;
        }
        if (!gate.ready()) {
            // Escape (either peer abandoning the match) or the link died under
            // the screen — either way there is no agreed next round.
            if (gate.failed()) {
                std::fprintf(stderr, "netplay: lost the peer between rounds; match ended\n");
                recorder.latch(net::SessionEndReason::PeerLostBetweenRounds);
                // The screen the player was looking at gave no hint of this: the
                // scoreboard simply stopped accepting Enter. Say it out loud.
                if (present_net_session_end(sctx(), recorder.summary()) == AppInput::Quit) {
                    team_play_ = saved_team_play;
                    return AppInput::Quit;
                }
            } else {
                recorder.latch(net::SessionEndReason::LeftSession);
                recorder.note("left at the between-rounds scoreboard");
            }
            result = AppInput::Advance;
            break;
        }
        round_cfg = gate.next_config();
        std::printf("netplay: round %d over at tick %u; next round seed 0x%08X stage %d\n", round,
                    static_cast<unsigned>(session.confirmed_tick()),
                    static_cast<unsigned>(round_cfg.seed), round_cfg.tuning.level_index);
        // Cover a lost ack before the match session takes the socket back.
        const std::uint64_t settle_until = SDL_GetTicks() + kRoundHandoffSettleMs;
        while (SDL_GetTicks() < settle_until) {
            SDL_Event sev;
            while (SDL_PollEvent(&sev))
                if (sev.type == SDL_EVENT_QUIT) {
                    team_play_ = saved_team_play;
                    return AppInput::Quit;
                }
            gate.pump();
            SDL_Delay(2);
        }
    }

    team_play_ = saved_team_play;
    return result;
}

AppInput GameApp::run_netplay_session(net::Transport& transport, std::uint16_t local_seats,
                                      std::uint16_t all_seats, bool is_host, std::uint32_t seed,
                                      const sim::MatchConfig& cfg, ChatOverlay* chat) {
    // ONE CONNECTED TRANSPORT, MANY MATCHES. The connect step (the lobby punch or
    // the direct seed handshake) ran once, above; from here the link is simply
    // reused — match, setup, match, setup — for as long as both players want to
    // keep going. Nothing in this loop touches the matchmaker, which matters
    // rather than being a nicety: the server reaps a lobby about 30 s into a
    // match (HeartbeatInterval 10 x HeartbeatMiss 3), so the control plane is
    // already gone by the time the first match ends. `chat` is likewise dead
    // after the first setup stage — present_net_online closes it before the
    // match — so later stages simply run without it.
    //
    // WHO DECIDES. Both peers know the match is decided with no traffic (same
    // sim, same tally, same clinch). What they agree over the wire is the
    // TRANSITION: run_netplay_match_seats' RematchGate holds the VICTORY screen
    // until the HOST dismisses it and announces MatchCtlKind::Rematch, so nobody
    // walks into the next setup stage alone. A peer that pressed Escape there
    // gets `rematch == false` and this loop ends — which is the old behaviour,
    // now a deliberate choice rather than the only option.
    sim::MatchConfig match_cfg = cfg;
    std::uint32_t match_seed = seed;
    int round_base = 0;
    while (true) {
        bool rematch = false;
        const AppInput r = run_netplay_match_seats(transport, local_seats, all_seats, is_host,
                                                   match_cfg, &round_base, &rematch);
        if (r == AppInput::Quit || !rematch) return r;
        // The tick space walks on across the match boundary for exactly the
        // reason it walks on across a round boundary (round_rotation.hpp): a
        // datagram still in flight from the last round must not land inside the
        // first round of the next match. `round_base` came back holding the last
        // round played, so the next match starts one past it.
        round_base = (round_base + 1) % kNetRoundBaseWrap;
        // A fresh seed for the next match's board. HOST-ONLY in effect — the
        // guest's copy is never read (present_net_setup's guest arm ignores it
        // and takes the host's whole confirmed config), so the two need not
        // agree on this number at all. Stepped by the golden-ratio constant
        // rather than +1 so a rematch is not simply the next board in the
        // sequence the match just played through.
        match_seed += 0x9E3779B9u;
        const AppInput setup = present_net_setup(transport, is_host, local_seats, all_seats,
                                                 match_seed, match_cfg, chat);
        if (setup == AppInput::Quit) return AppInput::Quit;
        if (setup != AppInput::Advance) return AppInput::Advance;  // left the setup stage
    }
}

AppInput GameApp::present_net_setup(net::Transport& transport, bool is_host,
                                    std::uint16_t local_seats, std::uint16_t all_seats,
                                    std::uint32_t seed, sim::MatchConfig& out_cfg,
                                    ChatOverlay* chat) {
    // THE ONLINE SETUP STAGE (docs/re/network-screens.md §7). The original has no
    // net-only setup UI at all: both network screens commit into sub_42A3F6, so a
    // net game's roster/AI and map come from the ORDINARY sub_410F81 / sub_406DDE
    // screens with the host driving and guests read-only. That is exactly what
    // this runs — the same SetupScreen and MapSelectScreen menu row 0 uses, handed
    // a NetSetupLink.
    //
    // AS MANY SEATS AS THE LOBBY SEATED. The host collects a per-seat ack mask
    // and only reaches Phase::Final once EVERY other seat has acknowledged its
    // exact bytes (setup_session.hpp), so over a star "the config is agreed"
    // means the whole table has it — not merely whoever answered first. The wire
    // seats are locked on the roster screen (net_setup_roster.hpp's SEAT
    // LOCKING), which already reads both masks bit by bit; AI slots fill the
    // rest.
    const auto remote_seats = static_cast<std::uint16_t>(all_seats & ~local_seats);
    net::SetupSession session(transport, is_host, local_seats, remote_seats);
    NetSetupLink link;
    link.session = &session;
    link.local_seats = local_seats;
    link.remote_seats = remote_seats;
    link.host = is_host;

    if (is_host) {
        // The wire seats must hold exactly the roster the seat masks can carry
        // before the host sees the screen (net_setup_link.hpp "SEAT LOCKING").
        net_setup_seed_host_roster(link, setup_type_, setup_sub_);
        const AppInput roster =
            SetupScreen(sctx(), setup_state(), campaign_state(), match_backdrop(), link, chat)
                .run();
        if (roster == AppInput::Quit) return AppInput::Quit;
        if (roster != AppInput::Advance) return AppInput::Back;  // Esc: whole flow aborts (§7)
        const AppInput level = MapSelectScreen(sctx(), map_select_state(), link, chat).run();
        if (level == AppInput::Quit) return AppInput::Quit;
        if (level != AppInput::Advance) return AppInput::Back;

        // The SAME build the local PLAY path does, from the SAME screens — the
        // whole point of reusing them. Then confirm: the resolved config goes over
        // the wire in full (match_config_codec.hpp), so the guest never rebuilds a
        // board from an index and cannot desync on tick 0.
        out_cfg = MatchRunner(sctx(), match_runner_state()).build_config(seed);
        session.confirm(out_cfg);

        // Wait for the ack on Phase::Final, NOT has_final_config() — the latter is
        // already true here (we just confirmed) and would start the match before
        // the guest holds the bytes.
        // The [WAIT] composition (§6): the GLUE backdrop these pre-match screens
        // already use, plus the pinned getstring(80) spinner prompt. Nothing new
        // is drawn for a state the original never had a picture for.
        const std::string glue = pick_glue(setup_lcg_, values_);
        platform::FrameClock frame_clock(window_.get());
        unsigned spin = 0;
        while (!net_setup_final(link)) {
            SDL_Event ev;
            while (SDL_PollEvent(&ev)) {
                if (ev.type == SDL_EVENT_QUIT) return AppInput::Quit;
                if (chat != nullptr && chat->handle_event(ev, sctx())) continue;
                if (ev.type == SDL_EVENT_KEY_DOWN && !ev.key.repeat &&
                    ev.key.key == SDLK_ESCAPE) {
                    audio_.play(20);
                    return AppInput::Back;
                }
            }
            net_setup_pump(link);
            if (chat != nullptr) chat->pump();
            if (net_setup_failed(link))
                return run_net_notice(sctx(), "NETWORK ERROR", "THE OTHER PLAYER LEFT") ==
                               AppInput::Quit
                           ? AppInput::Quit
                           : AppInput::Back;
            audio_.update_music();
            SDL_SetRenderDrawColor(sdl_renderer_.get(), 0, 0, 0, 255);
            SDL_RenderClear(sdl_renderer_.get());
            if (const Sprite& bg = assets_.frontend_pcx(glue); bg.tex) {
                SDL_FRect d{0, 0, static_cast<float>(bg.w), static_cast<float>(bg.h)};
                SDL_RenderTexture(sdl_renderer_.get(), bg.tex, nullptr, &d);
            }
            draw_net_wait_prompt(sctx(), spin);
            if (chat != nullptr) chat->draw(sctx());
            SDL_RenderPresent(sdl_renderer_.get());
            frame_clock.pace();
        }
        return AppInput::Advance;
    }

    // GUEST — mirror whichever of the two shared screens the host is on. The
    // preview's `rounds` field is the screen cue (net_setup_link.hpp's sentinel),
    // standing in for the original's kind-32 sub_40F064(901/902) advance
    // commands, which our protocol does not carry.
    const bool saved_team_play = team_play_;
    AppInput result = AppInput::Back;
    while (true) {
        const AppInput r =
            net_setup_on_level_screen(link)
                ? MapSelectScreen(sctx(), map_select_state(), link, chat).run()
                : SetupScreen(sctx(), setup_state(), campaign_state(), match_backdrop(), link, chat)
                      .run();
        if (r == AppInput::Quit) {
            team_play_ = saved_team_play;
            return AppInput::Quit;
        }
        if (net_setup_final(link)) {
            out_cfg = session.final_config();
            result = AppInput::Advance;
            // SETTLE, the same reason NetplayConnectScreen's kSettleMs exists:
            // our ack may be the datagram that gets lost, and SetupSession only
            // re-acks when the host's next chunk burst arrives — which we would
            // never see if we left the instant we decoded. Keep pumping briefly
            // so a lost ack recovers instead of stranding the host until its
            // 30 s timeout. Safe against the one-pump-at-a-time rule: the host
            // re-sends its whole input window from `confirmed_` on every pump
            // (RollbackSession::send_local), which cannot advance while our
            // inputs are missing — so anything swallowed here comes again.
            // Nothing is drawn: the last presented frame stays up.
            const std::uint64_t settle_until = SDL_GetTicks() + 300;
            while (SDL_GetTicks() < settle_until) {
                SDL_Event sev;
                while (SDL_PollEvent(&sev))
                    if (sev.type == SDL_EVENT_QUIT) {
                        team_play_ = saved_team_play;
                        return AppInput::Quit;
                    }
                net_setup_pump(link);
                if (chat != nullptr) chat->pump();  // don't go silent on the lobby either
                SDL_Delay(2);
            }
            break;
        }
        if (net_setup_failed(link)) {
            team_play_ = saved_team_play;
            // Timeout with no config: the host closed the game or the path died.
            return run_net_notice(sctx(), "NETWORK ERROR", "THE HOST LEFT THE GAME") ==
                           AppInput::Quit
                       ? AppInput::Quit
                       : AppInput::Back;
        }
        if (r == AppInput::Back) break;  // Esc: leave the setup stage
        // Otherwise the host simply moved between the two screens — follow it.
    }
    team_play_ = saved_team_play;
    return result;
}

// Default local UDP port for the menu-driven host/join flow (named constant per
// the task) and the fixed host seed the handshake announces. A per-session
// RANDOM host seed is a future nicety — a fixed value keeps the arena
// deterministic and needs no RNG here.
namespace {
constexpr std::uint16_t kNetDefaultPort = 8000;
constexpr std::uint32_t kNetHostSeed = 0x1234u;

#if defined(BOMBER_HAS_LOBBY)
// The DEPLOYED matchmaker (services/matchmaker running on Fly.io): the game
// reaches the public lobby with no flags and no local server. Override at
// runtime without a rebuild via --matchmaker / BOMBER_MATCHMAKER_URL, which is
// also how you point at a local instance:
//   go build -o mm.exe ./services/matchmaker && ./mm.exe
//
// wss://, because the signaling DOES carry credentials — the host_token that
// authorises StartMatch, the lobby code that is the whole authn for a "private"
// lobby, and the relay alloc_id (SECURITY.md S1). In the clear, a passive
// observer on the path gets host authority over the lobby, so the default must
// be the encrypted one. Built with TLS since BOMBER_LOBBY_TLS defaulted ON
// (cmake/BomberIXWebSocket.cmake); a build without it refuses a wss:// URL
// outright rather than downgrading, which is why the fallback below is a
// COMPILE-time choice and not a runtime one.
#if defined(BOMBER_HAS_LOBBY_TLS)
constexpr char kDefaultMatchmakerUrl[] = "wss://open-bomberman-matchmaker.fly.dev/ws";
#else
constexpr char kDefaultMatchmakerUrl[] = "ws://open-bomberman-matchmaker.fly.dev/ws";
#endif
constexpr std::uint16_t kDefaultStunPort = 8081;  // PROTOCOL.md §2's UDP echo port
// Where the HOW MANY PLAYERS list opens. 2 is the smallest lobby the server
// accepts and what every online match was pinned to before the host could
// choose, so the default keeps the old behaviour one Enter away.
constexpr int kDefaultLobbySeats = 2;

std::string env_or_empty(const char* name) {
#ifdef _MSC_VER
    char* buf = nullptr;
    std::size_t len = 0;
    if (_dupenv_s(&buf, &len, name) == 0 && buf != nullptr) {
        std::string v(buf);
        std::free(buf);
        return v;
    }
    return {};
#else
    const char* v = std::getenv(name);
    return v != nullptr ? std::string(v) : std::string();
#endif
}

// Pull the host out of "ws://host:port/path" — the matchmaker serves the UDP
// STUN echo from the SAME box as the WebSocket, so the URL's host is the right
// default for it. Returns an empty string if the URL has no recognisable host.
std::string url_host(const std::string& url) {
    const std::size_t scheme = url.find("://");
    const std::size_t start = scheme == std::string::npos ? 0 : scheme + 3;
    const std::size_t end = url.find_first_of(":/", start);
    return url.substr(start, end == std::string::npos ? std::string::npos : end - start);
}
#endif  // BOMBER_HAS_LOBBY
}  // namespace

AppInput GameApp::present_net_host() {
    // NETWORK.RSS (1040) is started by the MENU DISPATCH, not here — see the
    // AppState::NetHost/NetJoin arms in run_app. It has to be, because this
    // function can reach present_net_join() through the lobby menu's JoinDirect
    // row, and starting the track in both bodies would restart it from the top
    // on that hop (the same defect the goldman wheel -> player select hand-off
    // had). The original has no such nesting: sub_42B0CE and sub_42B47D are two
    // separate menu rows, each starting 1040 as its own first act.
    // START NET GAME (menu row 1) now opens the NETWORK GAME menu (ADR-0011
    // Phase 1d): the online lobby entry points plus the ADR-0010 direct/LAN
    // rows, which need no server and must keep working. Menu row 2 (JOIN NET
    // GAME -> present_net_join) is deliberately left as the unchanged direct-IP
    // join. On a lobby-off build the online rows are simply not listed.
#if defined(BOMBER_HAS_LOBBY)
    constexpr bool kOnlineAvailable = true;
#else
    constexpr bool kOnlineAvailable = false;
#endif
    switch (LobbyScreen(sctx()).run_menu(kOnlineAvailable)) {
        case LobbyMenuChoice::WindowClosed: return AppInput::Quit;
        case LobbyMenuChoice::HostDirect: return present_net_direct_host();
        case LobbyMenuChoice::JoinDirect: return present_net_join();
#if defined(BOMBER_HAS_LOBBY)
        case LobbyMenuChoice::HostOnline: return present_net_online(/*host=*/true);
        case LobbyMenuChoice::HostPublic:
            // Same room and same flow — the visibility only changes what the
            // server advertises, so a public lobby is still joinable by code.
            return present_net_online(/*host=*/true, /*browse=*/false, /*is_public=*/true);
        case LobbyMenuChoice::JoinOnline: return present_net_online(/*host=*/false);
        case LobbyMenuChoice::BrowsePublic:
            // The browser hands back a code, so this is the JOIN arm with the
            // typed-code prompt swapped for a picked row (ADR-0011 Phase 3).
            return present_net_online(/*host=*/false, /*browse=*/true);
#else
        // Never listed without the lobby — fall through to the cancel arm.
        case LobbyMenuChoice::HostOnline:
        case LobbyMenuChoice::HostPublic:
        case LobbyMenuChoice::JoinOnline:
        case LobbyMenuChoice::BrowsePublic:
#endif
        case LobbyMenuChoice::Cancel: break;
    }
    return AppInput::Advance;  // cancelled → back to the main menu
}

AppInput GameApp::present_net_direct_host() {
    // HOST LAN GAME: bind kNetDefaultPort, run the seed handshake as host, then —
    // once the peer is connected — the SHARED setup stage (roster/AI + map) as the
    // host, and finally the match as seat 0. Esc/timeout return to the menu; a
    // window close during connect/setup/match propagates Quit. (This is the
    // ADR-0010 body that used to sit directly on menu row 1; the only change is
    // that a LAN game now picks its map and AI slots like the online one, since
    // net::SetupSession needs no matchmaker and rides the same UDP socket.)
    net::UdpTransport transport;
    NetplayConnectResult r =
        NetplayConnectScreen(sctx()).run_host(transport, kNetDefaultPort, kNetHostSeed);
    if (r.window_closed) return AppInput::Quit;
    if (!r.connected) return AppInput::Advance;  // cancelled/timed out → back to the menu
    sim::MatchConfig cfg;
    // A direct/LAN game is a PAIR by construction — one address, one peer — so
    // its seat topology is the literal 0b11 the online path now derives.
    const AppInput setup = present_net_setup(transport, /*is_host=*/true, /*local_seats=*/0b01u,
                                             /*all_seats=*/0b11u, r.seed, cfg);
    if (setup == AppInput::Quit) return AppInput::Quit;
    if (setup != AppInput::Advance) return AppInput::Advance;
    // A SESSION, not a single match: finishing one returns both peers to these
    // same setup screens over this same socket (run_netplay_session).
    return run_netplay_session(transport, /*local_seats=*/0b01u, /*all_seats=*/0b11u,
                               /*is_host=*/true, r.seed, cfg);
}

#if defined(BOMBER_HAS_LOBBY)
std::string GameApp::matchmaker_url() const {
    if (!opts_.matchmaker_url.empty()) return opts_.matchmaker_url;  // --matchmaker
    const std::string env = env_or_empty("BOMBER_MATCHMAKER_URL");
    return env.empty() ? std::string(kDefaultMatchmakerUrl) : env;
}

std::string GameApp::matchmaker_stun_host() const {
    if (!opts_.matchmaker_stun_host.empty()) return opts_.matchmaker_stun_host;
    const std::string env = env_or_empty("BOMBER_MATCHMAKER_STUN_HOST");
    return env.empty() ? url_host(matchmaker_url()) : env;
}

std::uint16_t GameApp::matchmaker_stun_port() const {
    if (opts_.matchmaker_stun_port != 0) return opts_.matchmaker_stun_port;
    // Same env name tests/net/test_lobby_live.cpp already uses, so one exported
    // variable configures both the live test and the game.
    const std::string env = env_or_empty("BOMBER_MATCHMAKER_STUN_PORT");
    const long p = env.empty() ? 0 : std::strtol(env.c_str(), nullptr, 10);
    return (p > 0 && p <= 65535) ? static_cast<std::uint16_t>(p) : kDefaultStunPort;
}

AppInput GameApp::present_net_online(bool host, bool browse, bool is_public) {
    // HOST PRIVATE/PUBLIC GAME / JOIN BY CODE / BROWSE PUBLIC GAMES: the ADR-0011 online
    // path. A guest first names the lobby it wants — typing the 6-char code, or
    // picking a row in the public browser, which yields the very same code — then
    // both sides bind ONE socket, sit in the waiting room, and, once the server's
    // StartMatch arrives and the peers punch a direct path, run the match with the
    // SERVER's authoritative seed and seat mask (never a locally derived
    // host?0:1). The browser is deliberately just another way to fill in `code`:
    // everything below it is the unchanged join path.
    LobbyScreen screen(sctx());

    LobbyScreen::OnlineConfig ocfg;
    ocfg.server_url = matchmaker_url();
    ocfg.stun_host = matchmaker_stun_host();
    ocfg.stun_port = matchmaker_stun_port();
    // The original's "Node Name" (options row 2, sub_40FE34) IS the per-machine
    // net identity — reuse it when the player has set one.
    ocfg.player_name = options_.node_name.empty() ? std::string("PLAYER") : options_.node_name;

    // HOW BIG A LOBBY. `max_seats` is a CreateLobby field the server mints seats
    // from and cannot be changed once the room exists (PROTOCOL.md §3, clamped
    // to 2..10), so the host is asked BEFORE anything is created. This used to be
    // a hard-coded 2 — the single reason an online game could never be more than
    // a pair, since the whole seat topology below flows from the seats the server
    // hands out. Guests never see this: they take whatever the room has.
    int max_seats = kDefaultLobbySeats;
    std::string code;
    if (host) {
        bool closed = false;
        if (!screen.run_seat_count(max_seats, closed))
            return closed ? AppInput::Quit : AppInput::Advance;
    }
    if (browse) {
        // The browser needs a bound socket of its own (LobbyFlow owns one either
        // way) but never punches with it, so it is scoped to the browse and
        // closed before the match socket below is opened.
        net::UdpTransport browse_transport;
        if (!browse_transport.bind(0)) {
            std::fprintf(stderr, "lobby: cannot open a UDP socket\n");
            return AppInput::Advance;
        }
        bool closed = false;
        if (!screen.run_public_browser(ocfg, browse_transport, code, closed))
            return closed ? AppInput::Quit : AppInput::Advance;
    } else if (!host) {
        bool closed = false;
        if (!screen.run_code_entry(code, closed))
            return closed ? AppInput::Quit : AppInput::Advance;
    }

    // Bound BEFORE the flow is built: LobbyFlow reuses this exact socket for the
    // STUN probe, the hole punch and the match, so the NAT binding the peers
    // punched is the one gameplay flows through (lobby_flow.hpp).
    net::UdpTransport transport;
    if (!transport.bind(0)) {
        std::fprintf(stderr, "lobby: cannot open a UDP socket\n");
        return AppInput::Advance;
    }

    // The control connection and the flow are owned HERE, not by the waiting
    // room, because they have to outlive it: the F2 lobby chat (chat_overlay.hpp,
    // PROTOCOL.md §7) keeps working through the setup screens below, and the
    // matchmaker reaps a member that stops heart-beating. Both are torn down on
    // the way out of this function, before the match takes the socket.
    net::LobbyFlow::Config lcfg;
    lcfg.server_url = ocfg.server_url;
    lcfg.stun_host = ocfg.stun_host;
    lcfg.stun_port = ocfg.stun_port;
    lcfg.player_name = ocfg.player_name;
    lcfg.build_hash = net::build_hash();  // the cross-build door: the server rejects mismatches

    net::LobbyClient client;
    net::LobbyFlow flow(lcfg, transport, client);
    ChatOverlay chat(&flow);
    if (host)
        flow.host_lobby(ocfg.player_name, is_public, max_seats);
    else
        flow.join_lobby(code);

    const LobbyRoomResult r = screen.run_online(flow, chat, host, code, host ? max_seats : 0);
    if (r.window_closed) return AppInput::Quit;
    if (!r.ready) return AppInput::Advance;  // left the lobby / failed → back to the menu

    // THE MATCH TRANSPORT IS THE FLOW'S, NOT THE BARE SOCKET. LobbyFlow::
    // transport() hands back whatever the connect step actually produced: the
    // socket itself for a punched pair, the StarHubTransport on the hub of a
    // >2-seat match (fan-out + guest↔guest reflection), or the RelayedTransport
    // when the punch failed. Passing `transport` directly — as this did — worked
    // only for the first of the three.
    net::Transport& link = flow.transport();

    // The punch is done and the link is connected: run the SHARED roster/map
    // screens over it (host drives, guests watch) and start the match on the
    // config every peer agreed. present_net_setup stops pumping its session
    // before returning, so the match session has the transport to itself
    // (setup_session.hpp's one obligation — whichever polls first eats the
    // datagram; on the hub that same poll is what keeps the star reflecting).
    sim::MatchConfig cfg;
    const AppInput setup = present_net_setup(link, r.is_host, r.local_seats_mask, r.all_seats_mask,
                                             r.seed, cfg, &chat);
    // Chat stops at the door of the match: the overlay is a lobby thing, and the
    // WS link closes with `client` when this function returns anyway.
    chat.close();
    if (setup == AppInput::Quit) return AppInput::Quit;
    if (setup != AppInput::Advance) return AppInput::Advance;
    // The chat is already closed, so later setup stages run without it — see
    // run_netplay_session on why nothing below here may depend on the server.
    return run_netplay_session(link, r.local_seats_mask, r.all_seats_mask, r.is_host, r.seed, cfg);
}
#endif  // BOMBER_HAS_LOBBY

AppInput GameApp::present_net_join() {
    // JOIN NET GAME (menu row 2): prompt for the host address (prefilled
    // 127.0.0.1:kNetDefaultPort), connect, run the handshake as guest (adopting
    // the host's seed), then watch the host's roster/map screens read-only and
    // run the match as seat 1 with the config the host confirmed.
    //
    // NETWORK.RSS (1040) — started by the menu dispatch, see present_net_host's
    // note for why it is not started here.
    net::UdpTransport transport;
    NetplayConnectResult r = NetplayConnectScreen(sctx()).run_join(transport, kNetDefaultPort);
    if (r.window_closed) return AppInput::Quit;
    if (!r.connected) return AppInput::Advance;
    sim::MatchConfig cfg;
    const AppInput setup = present_net_setup(transport, /*is_host=*/false, /*local_seats=*/0b10u,
                                             /*all_seats=*/0b11u, r.seed, cfg);
    if (setup == AppInput::Quit) return AppInput::Quit;
    if (setup != AppInput::Advance) return AppInput::Advance;
    return run_netplay_session(transport, /*local_seats=*/0b10u, /*all_seats=*/0b11u,
                               /*is_host=*/false, r.seed, cfg);
}

ScreenContext GameApp::sctx() {
    // *screen_ is emplaced in init() before run_app() drives any screen, so it is
    // always engaged here (same invariant as renderer_, see the file-top note).
    return ScreenContext{.assets = assets_,
                         .audio = audio_,
                         .sounds = sounds_,
                         .keyboard = keyboard_,
                         .gamepads = gamepads_,
                         .front_font = front_font_,
                         .cursor_blink = cursor_blink_,
                         .asset_screen = *screen_,  // NOLINT(bugprone-unchecked-optional-access)
                         .seqs = seqs_,
                         .values = values_,
                         .sdl = sdl_renderer_.get(),
                         .window = window_.get()};
}

OptionsEditState GameApp::options_state() {
    // The Options cluster's shared-state seam (ADR-0009 §4): the non-service
    // members its three runner screens read/write, bundled by reference so the
    // runners need no GameApp&. Built fresh on demand, same as sctx().
    return OptionsEditState{.options = options_,
                            .options_dirty = options_dirty_,
                            .scheme = scheme_,
                            .setup_lcg = setup_lcg_,
                            .game_dir = opts_.game_dir,
                            .gold_player = gold_player_,
                            .team_play = team_play_,
                            .conveyor_speed_index = conveyor_speed_index_};
}

MenuState GameApp::menu_state() {
    // The main menu's shared-state seam (ADR-0009 §6): the non-service members
    // present_menu + roll_attract_match read/write (the cursor + idle clock +
    // editor trigger counter, the attract roster/level/team + LCG, the F10
    // video toggles, and the four editor members) — bundled by reference so
    // MenuScreen needs no GameApp&. Built fresh on demand, same as sctx()/
    // options_state()/editor_state(). Field order MUST track MenuState's.
    return MenuState{.menu_index = menu_index_,
                     .menu_idle_since_ms = menu_idle_since_ms_,
                     .editor_trigger_count = editor_trigger_count_,
                     .attract = attract_,
                     .attract_saved = attract_saved_,
                     .attract_lcg = attract_lcg_,
                     .setup_type = setup_type_,
                     .setup_sub = setup_sub_,
                     .setup_team = setup_team_,
                     .selected_level = selected_level_,
                     .team_play = team_play_,
                     .uncap_fps = uncap_fps_,
                     .native_cadence = native_cadence_,
                     .show_fps = show_fps_,
                     .soft_scaling = soft_scaling_,
                     .options_dirty = options_dirty_,
                     .setup_lcg = setup_lcg_,
                     .scheme = scheme_,
                     .game_dir = opts_.game_dir,
                     .scheme_path = opts_.scheme};
}

MapSelectState GameApp::map_select_state() {
    // The LEVEL & ROUNDS screen's shared-state seam (ADR-0009 §7): the
    // non-service members present_map_select reads/writes (the committed level +
    // win-target, the shared preview LCG, the read-only win-by-kills option, and
    // the pending gold player Escape forfeits) — bundled by reference so
    // MapSelectScreen needs no GameApp&. Built fresh on demand, same as sctx()/
    // menu_state(). Field order MUST track MapSelectState's.
    return MapSelectState{.selected_level = selected_level_,
                          .win_target = win_target_,
                          .setup_lcg = setup_lcg_,
                          .options = options_,
                          .gold_player = gold_player_};
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
    return SetupState{.setup_type = setup_type_,
                      .setup_sub = setup_sub_,
                      .setup_team = setup_team_,
                      .scheme = scheme_,
                      .setup_lcg = setup_lcg_,
                      .campaign_trigger_count = campaign_trigger_count_,
                      .team_play = team_play_,
                      .gold_player = gold_player_,
                      .campaign_active = campaign_active_,
                      .campaign_stages = campaign_stages_,
                      .campaign_stage_index = campaign_stage_index_};
}

MatchBackdrop GameApp::match_backdrop() {
    // The match-coupled backdrop seam (ADR-0009 §8): the live Renderer + sim
    // State the campaign confirm/banner/complete dialogs and the in-round help
    // modal draw as their frozen backdrop, kept SEPARATE from the front-end-
    // service-only ScreenContext. renderer_ is emplaced in init() before run_app
    // drives any screen (same invariant as *screen_ in sctx(), see file-top note).
    return MatchBackdrop{.renderer = *renderer_,  // NOLINT(bugprone-unchecked-optional-access)
                         .state = sim_.state()};
}

CampaignState GameApp::campaign_state() {
    // The campaign flow's shared-state seam (ADR-0009 §8): the non-service
    // members present_campaign_picker + load_campaign_stage read/write — bundled
    // by reference so CampaignPickerScreen (and the free load_campaign_stage)
    // need no GameApp&. Built fresh on demand, same as sctx()/menu_state().
    // Field order MUST track CampaignState's.
    return CampaignState{.campaign_active = campaign_active_,
                         .campaign_stages = campaign_stages_,
                         .campaign_stage_index = campaign_stage_index_,
                         .campaign_banner = campaign_banner_,
                         .setup_type = setup_type_,
                         .setup_sub = setup_sub_,
                         .setup_team = setup_team_,
                         .setup_lcg = setup_lcg_,
                         .scheme = scheme_,
                         .game_dir = opts_.game_dir};
}

ScoreboardState GameApp::scoreboard_state() {
    // The RESULTS scoreboard's shared-state seam (ADR-0009 §8): the non-service
    // members present_scoreboard reads (the frozen round's sim::State, the
    // win/kill tally + roster it rows, the win-by-kills option, and the
    // demo/roster flags auto_advance_results() consults) — bundled by reference/
    // value so ScoreboardScreen needs no GameApp&. Built fresh on demand, same as
    // sctx()/campaign_state(). Field order MUST track ScoreboardState's.
    return ScoreboardState{.state = sim_.state(),
                           .win_count = win_count_,
                           .kill_count = kill_count_,
                           .win_target = win_target_,
                           .setup_team = setup_team_,
                           .team_play = team_play_,
                           .setup_type = setup_type_,
                           .options = options_,
                           .demo = opts_.demo,
                           .demo_ticks = opts_.demo_ticks,
                           .demo_shots = opts_.demo_shots};
}

GoldmanState GameApp::goldman_state() {
    // The Goldman wheel's shared-state seam (ADR-0009 §8): the three non-service
    // members present_goldman_wheel writes (the wheel's presentation LCG and the
    // pending gold-player/prize pair) — bundled by non-const reference so
    // GoldmanWheelScreen needs no GameApp&. Built fresh on demand, same as
    // sctx()/scoreboard_state(). Field order MUST track GoldmanState's.
    return GoldmanState{.goldman_lcg = goldman_lcg_,
                        .gold_player = gold_player_,
                        .gold_prize = gold_prize_};
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
    return MatchRunnerState{.sim = sim_,
                            .renderer = *renderer_,  // NOLINT(bugprone-unchecked-optional-access)
                            .next_seed = next_seed_,
                            .kill_count = kill_count_,
                            .uncap_fps = uncap_fps_,
                            .native_cadence = native_cadence_,
                            .show_fps = show_fps_,
                            .show_netstats = show_netstats_,
                            .scheme = scheme_,
                            .base_tuning = base_tuning_,
                            .options = options_,
                            .conveyor_speed_index = conveyor_speed_index_,
                            .setup_type = setup_type_,
                            .setup_sub = setup_sub_,
                            .setup_team = setup_team_,
                            .win_count = win_count_,
                            .team_play = team_play_,
                            .campaign_active = campaign_active_,
                            .attract = attract_,
                            .gold_player = gold_player_,
                            .gold_prize = gold_prize_,
                            .selected_level = selected_level_,
                            .campaign_stages = campaign_stages_,
                            .campaign_stage_index = campaign_stage_index_,
                            .game_dir = opts_.game_dir,
                            .demo = opts_.demo};
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

void GameApp::award_round_win(int winner) {
    ::bomber::game::award_round_win(win_count_, winner, team_play_, sim_.state(), setup_team_);
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
    attract_ = false;  // the original clears dword_464938 here
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
                    // The SETUP-SCREENS track (1020), started ONCE for the whole
                    // Play flow — this line is sub_42A3F6's own 0x42A436, which
                    // sits between the campaign reset above and the call to
                    // sub_410F81 below and is the ONLY site in the binary that
                    // ever names 1020 (exhaustive: six music call sites in the
                    // whole image, docs/re/sound-engine.md §9).
                    //
                    // It lives here, not in the screens, because start_music is
                    // a genuine restart — sub_42741E frees the handle and
                    // reloads from sample 0, with no same-id no-op — so the
                    // goldman wheel and the player-select screen each calling it
                    // meant WIN.RSS audibly jumped back to the top as the wheel
                    // handed over. Both of those screens carried a comment
                    // saying they inherit the track and start no music of their
                    // own; now they actually do.
                    audio_.start_music(kWinMusicId);
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
                    // mirroring sub_410F81, which returns right after the call
                    // whenever dword_464A68 is set).
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
                // The outcome-tier backdrop, CAMPAIGN-GATED. sub_42A3F6's round
                // loop tests dword_46489C at 0x42A63B and a campaign round end
                // branches away entirely: at most one modal (sub_414340, and
                // only when the pacing flag dword_464894 is 2), then straight
                // back into the round init sub_410B6E for the next stage. It
                // never reaches the 1130 start at 0x42A6DD.
                //
                // SCOPE, stated plainly: the same branch means a campaign round
                // end shows no DRAW, no RESULTS tally and no VICTORY either, and
                // the port DOES show all three. That is a real divergence and it
                // is NOT fixed here — reshaping the campaign round end touches
                // the stage advance, the gold-player assignment and the
                // scoreboard, which is its own change with its own tests. What
                // this gate does fix is the invented CUE: those screens no
                // longer swap the track out from under a campaign, so they run
                // on the stage music the round init left playing, which is what
                // the original is actually doing at that moment.
                const auto start_outcome_music = [this] {
                    if (campaign_active_) return;
                    audio_.start_music(kDrawMusicId);
                };
                int w = round_winner();
                // Round-pacing clauses 4-5 override (docs/re/campaign.md
                // "Round pacing", sub_4016DA, PORTED 2026-07-09): in campaign
                // mode, a round where every human/joystick slot is dead is
                // force-ended and REPLAYED regardless of what round_winner()
                // says — even an AI side "winning" (w>=0, no human alive)
                // does not count. Route it exactly like a plain draw (below)
                // so it neither tallies a win nor advances the stage.
                if (w >= 0 && campaign_no_human_survivor()) w = -1;
                // Tally the round win — and under Team Play mirror it onto the
                // winner's teammates (sub_421B56 @ 0x421B56, called from
                // 0x42A919), so every member of the winning team holds the TEAM
                // total the scoreboard row and the clinch both read.
                if (w >= 0) award_round_win(w);
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
                // write) when sub_4219B0 returns anything but -1, i.e. a ROUND SURVIVOR
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
                    // inside the branch taken when v73 is not -1) — THEN cuts to VICTORY.
                    // The port formerly skipped the scoreboard and jumped straight
                    // to VICTORY (and mis-fired 2000 on every round win too).
                    start_outcome_music();  // 1130 under RESULTS/VICTORY (doc §2), not in campaign
                    audio_.play_random_in_range(2000, 2299);  // winner voice — clinch only
                    ev = present_scoreboard();  // the clinch scoreboard (WINS THE MATCH!)
                    // Then VICTORY<player>.PCX / TEAM<0/1>.PCX (frontend-flow.md
                    // "VICTORY" §3, aTeamU vs aVictoryU).
                    if (ev != AppInput::Quit)
                        ev = present_screen(
                            victory_screen(is_team_mode(), clinched, setup_team_[clinched]));
                    // Campaign stage advance (docs/re/campaign.md
                    // "Advances through campaign stages automatically",
                    // sub_401312/sub_40133F, both gated on dword_46489C): a
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
                        ++campaign_stage_index_;  // the original bumps dword_4648B0 here
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
                    // when the clinch index is a real player; a non-clinching
                    // RESULTS pass (index -1, batch_0x4293E5.cpp:1260-1272) plays no
                    // "we have a winner" cue. (The port formerly fired it every
                    // round win.)
                    start_outcome_music();  // 1130 under RESULTS (doc §2), not in campaign
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
                    start_outcome_music();  // 1130 under DRAW (doc §2), not in campaign
                    audio_.play_random_in_range(kDrawStingLo, kDrawStingHi);
                    // sub_42A3F6's DRAW loop only auto-advances (6 s) for an
                    // all-AI/attract roster; a human match waits for Enter. A
                    // 0 dwell means "no auto-advance" in the Screen model
                    // (screen.cpp:47), so zero it out when a human is playing.
                    ScreenDef ds = draw_screen();
                    if (!auto_advance_results()) ds.dwell_ms = 0;
                    ev = present_screen(ds);
                    // DRAW FALLS THROUGH INTO THE RESULTS TALLY — it is a PREFIX,
                    // not an alternative (docs/re/in-match-shell.md "DRAW is a
                    // prefix to RESULTS", raw 0x42A875-0x42A88B: the DRAW wait
                    // loop ends with NO jump and execution lands in LABEL_102,
                    // which loads RESULTS.PCX; the RESULTS-only path is the
                    // jump to LABEL_102 taken when a survivor EXISTS). So a drawn
                    // round shows both screens and dismisses both wait loops.
                    // The port showed DRAW alone until this was pinned.
                    if (ev != AppInput::Quit && ev != AppInput::Back) ev = present_scoreboard();
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
            // The two netplay rows (START/JOIN NET GAME): the connect screens run
            // the seed handshake then a 2-player UDP lockstep match, returning
            // Quit (window closed) or Advance (match over / cancelled) — next()
            // routes both leaves back to the menu (increment 5c, ADR-0010).
            // NETWORK.RSS (1040), the track that was unreachable in this port
            // until 2026-07-28. These two arms ARE the original's menu rows 1
            // and 2, and each of its two net screens starts 1040 as its first
            // act: sub_42B0CE @0x42B11B and sub_42B47D @0x42B4C0 (an exhaustive
            // sweep finds six music call sites in the whole image and two of
            // them are these — docs/re/sound-engine.md §9). Nothing switches
            // back explicitly: present_menu() restarts 1010 on every menu entry,
            // which is exactly how the original reclaims the loop.
            case AppState::NetHost:
                audio_.start_music(kNetMusicId);
                ev = present_net_host();
                break;
            case AppState::NetJoin:
                audio_.start_music(kNetMusicId);
                ev = present_net_join();
                break;
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
    if (ev.key.key == kNetOverlayToggleKey) {
        // F3 — the in-match netplay diagnostic panel (screens/net_overlay.hpp
        // carries the key survey). Handled here with the other overlay levers so
        // it works whatever screen is up and never leaks into one; MatchRunner
        // only draws it when a netplay session is actually running, so pressing
        // it in a local match is a harmless no-op rather than an empty panel.
        show_netstats_ = !show_netstats_;
        std::fprintf(stderr, "netplay overlay: %s\n", show_netstats_ ? "on" : "off");
        return false;  // presentation shortcut; never leak F3 into a screen
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
    const bool enabling = !assets_.hd_enabled();
    // The per-player HD sprite sets are built lazily the first time HD is turned
    // on (build_player_sets skips them at boot to save the ~16x-heavier HD
    // memory while HD is off). ensure_player_hd_sets() builds them once and
    // reports whether it did; when it did, re-resolve the SequenceSet so the
    // per-player Sprites pick up their freshly-built tex_hd (the shared sets'
    // HD was already resolved at boot). resolve() only rebuilds the
    // stage-independent sequences, leaving the current stage's tiles intact.
    if (enabling && assets_.ensure_player_hd_sets()) seqs_.resolve(assets_);
    assets_.set_hd_enabled(enabling);
    SDL_SetWindowTitle(window_.get(), assets_.hd_enabled() ? "Atomic Bomberman [HD]"
                                                           : "Atomic Bomberman [Classic]");
    std::fprintf(stderr, "artwork mode: %s\n", assets_.hd_enabled() ? "HD" : "classic");
}

void GameApp::seed_default_node_name() {
    // sub_40C74C's absent-NODENAME.INI fallback: `getstring(500 + rand() %
    // getvalue(47))`, getvalue(47) = 49 (MESSAGES ids 500..548). The pick is
    // presentation-only, so it runs on the shared front-end LCG, never
    // State::rng (ADR-0004) — one draw, once per install, since flush_node_name
    // then makes the name permanent exactly like sub_40C140 does.
    if (!options_.node_name.empty()) return;
    // The demo/screenshot harness must not draw from a pinned LCG nor rewrite
    // the install; it never reaches the Options screen or the lobby either.
    if (opts_.demo) return;
    const int count = static_cast<int>(values_.at_or(47, 49));
    setup_lcg_ = setup_lcg_ * 1664525u + 1013904223u;
    const int pick = count > 0 ? static_cast<int>((setup_lcg_ >> 16) % static_cast<unsigned>(count))
                               : 0;
    options_.node_name = assets_.getstring(500 + pick, "Bomberman");
}

void GameApp::flush_node_name() {
    // sub_40C140, reached from the shutdown hook sub_40C4DB (docs/re/
    // network-screens.md §3): the node name is written back on a normal exit, so
    // an edited name persists and a randomly-assigned one becomes permanent
    // after the first run. The original rewrites unconditionally; skipping a
    // write whose bytes would be identical is the same outcome without touching
    // the user's install on every launch.
    if (opts_.demo || node_name_path_.empty()) return;
    if (options_.node_name.empty() || options_.node_name == node_name_loaded_) return;
    try {
        assets::save_node_name(node_name_path_, options_.node_name);
        node_name_loaded_ = options_.node_name;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "nodename.ini save failed: %s\n", e.what());
    }
}

void GameApp::flush_options() {
    // The net identity lives in its OWN file with its OWN shutdown hook in the
    // original; flushed here so run()'s five exit paths keep one call, but
    // ahead of the options_dirty_ gate below, which governs options.ini alone.
    flush_node_name();
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
    to_write.soft_scaling = soft_scaling_;
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
        // --demo-players N (dev-only, README animation capture): an all-COMPUTER
        // roster. Absent (0) leaves setup_type_ at its 1-human + 1-AI default, so
        // the visual goldens keep rendering the exact same scripted match.
        if (opts_.demo_players > 0)
            for (int i = 0; i < sim::kMaxPlayers; ++i)
                setup_type_[i] = static_cast<int>(i < opts_.demo_players ? SlotInputType::Computer
                                                                         : SlotInputType::Off);
        start_match(opts_.demo_seed);  // 0xB0BB1E5 unless --demo-seed overrides
        int rc = run_demo();
        flush_options();
        return rc;
    }
    // Netplay (increment 5b): --host/--join run ONE 2-player UDP match instead
    // of the front-end. Checked after the headless capture paths (they never set
    // net_role) and before run_app.
    if (opts_.net_role != 0) {
        int rc = run_netplay();
        flush_options();
        return rc;
    }
    int rc = run_app();
    flush_options();
    return rc;
}

}  // namespace bomber::game
