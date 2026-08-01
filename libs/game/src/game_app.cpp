#include "bomber/game/game_app.hpp"

#include <chrono>
#include <cstdlib>
#include <random>
#include <string>
#include <system_error>

#include "app_settings.hpp"
#include "bomber/assets/install.hpp"
#include "bomber/frontend/asset_screen.hpp"
#include "bomber/frontend/boot_screen.hpp"
#include "bomber/frontend/campaign_screens.hpp"
#include "bomber/frontend/map_select_screen.hpp"
#include "bomber/frontend/match_runner.hpp"
#include "bomber/frontend/menu_screen.hpp"
#include "bomber/frontend/options_screens.hpp"
#include "bomber/frontend/outcome_tier.hpp"  // the DRAW/VICTORY screens + their cues
#include "bomber/frontend/results_screens.hpp"
#include "bomber/frontend/setup_screen.hpp"
#include "bomber/game_util/log.hpp"
#include "bomber/game_util/match_outcome.hpp"
#include "bomber/ui/help_screens.hpp"  // BmTextScreen
#include "boot_loading.hpp"
#include "capture_runs.hpp"
#include "global_hotkeys.hpp"
#include "window_icon.hpp"

namespace bomber::game {

namespace fs = std::filesystem;

// The `renderer_`/`screen_` optionals are engaged exactly once, unconditionally,
// at the end of a successful init(), and every method that dereferences them runs
// only from run(), which the app mains call strictly after a successful init() —
// the invariant every bare `*renderer_`/`*screen_` below leans on.

namespace {

// A fresh per-process seed for the front end's presentation-only LCGs. The
// original seeds its single C rand() from the wall clock exactly once at boot —
// `time_(); srand_();` twice inside sub_41095A (pseudo.c 14610-14611 and again at
// 14639-14640) — so every "random" pick downstream of it varies from launch to
// launch (docs/re/facts.md "Per-match brick fill" cites the same pair). The clock
// is mixed in so the result still varies on a std::random_device implementation
// that is itself deterministic, a known quirk of some older toolchains.
std::uint32_t random_boot_seed() {
    static std::random_device rd;
    auto clock_bits = static_cast<std::uint32_t>(
        std::chrono::high_resolution_clock::now().time_since_epoch().count());
    std::uint32_t seed = static_cast<std::uint32_t>(rd()) ^ clock_bits;
    return seed != 0 ? seed : 1u;  // an LCG step never yields 0 from a 0 seed here,
                                   // but avoid a literal 0 seed on principle
}

// The SETUP-SCREENS track (0x3FC WIN.RSS — NOT victory music, docs/re/
// in-match-shell.md §2), started ONCE per Play entry at sub_42A3F6's 0x42A436,
// before sub_410F81 and so before both the goldman wheel and the player-select
// screen, which merely INHERIT it. It belongs to the flow rather than to a screen
// because start_music is a genuine restart — sub_42741E frees the handle and
// reloads from sample 0 with no same-id no-op — so a screen starting it again made
// WIN.RSS audibly jump back to the top as the wheel handed over.
constexpr int kWinMusicId = 1020;
constexpr int kNetMusicId = 1040;  // 0x410 NETWORK.RSS; see run_app's two net arms

}  // namespace

GameApp::~GameApp() {
    // run() flushes before returning; this is the backstop for any other exit path.
    // The write does filesystem I/O that can throw and destructors are implicitly
    // noexcept, so swallow — losing an options write on a failing disk beats
    // std::terminate.
    try {
        flush_settings(settings_slots());
    } catch (...) {  // NOLINT(bugprone-empty-catch) — see above
    }
}

bool GameApp::init() {
    seed_front_end_rngs();
    fs::path scheme_path;
    if (!resolve_install_paths(scheme_path)) return false;
    if (!load_settings(settings_slots(), scheme_path)) return false;
    if (!init_video()) return false;
    if (!load_game_data(data_load_slots())) return false;
    // Reads MESSAGES.TXT, which the data load only just parsed, so it cannot live
    // beside the nodename.ini read in load_settings().
    seed_default_node_name(settings_slots());
    return true;
}

void GameApp::seed_front_end_rngs() {
    // First, before anything could read one, as the original reseeds before its own
    // config/subsystem init runs.
    setup_lcg_ = random_boot_seed();
    attract_lcg_ = random_boot_seed();
    goldman_lcg_ = random_boot_seed();
    next_seed_ = random_boot_seed();
    // A capture pins all four so two runs of the SAME scripted match are
    // byte-identical. The demo path does not currently read any of them — it never
    // visits the menu, attract, wheel or campaign code — but pinning removes that
    // as an assumption the harness would have to re-verify if the script grew.
    if (opts_.demo) setup_lcg_ = attract_lcg_ = goldman_lcg_ = next_seed_ = 0xD3701234u;
}

bool GameApp::resolve_install_paths(fs::path& scheme_path) {
    // SDL_GetBasePath is the exe's own folder; libs/assets is SDL-free so it
    // cannot ask for it itself. Without this the auto-detect only ever saw the
    // working directory, so the exe worked on the developer's machine (whose
    // absolute install path is one of the hardcoded probes) and nowhere else.
    const char* base = SDL_GetBasePath();  // SDL3: static string, do not free
    fs::path game = !opts_.game_dir.empty()
                        ? opts_.game_dir
                        : assets::default_game_dir(base ? fs::path(base) : fs::path{});
    if (!game.empty() && fs::is_directory(game / "DATA")) {
        opts_.game_dir = game;
        scheme_path =
            !opts_.scheme.empty() ? opts_.scheme : game / "DATA" / "SCHEMES" / "BASIC.SCH";
        return true;
    }
    // This is the ONLY failure a first-time user hits, and it used to be invisible:
    // bomber_game is a WIN32 (GUI-subsystem) binary, so a double-clicked exe has no
    // console and this text went nowhere — the game just silently did not appear.
    // stderr is kept for CLI/CI runs; the box is for everyone else.
    const std::string searched = game.empty() ? std::string("(no install found)") : game.string();
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
    log_warn("%s", msg.c_str());
    // NEVER on a capture run. A message box is MODAL, and the visual golden
    // harness runs --demo-shots with no install path precisely so it can SKIP on
    // the exit code — with a box in the way a machine without the game hangs the
    // test instead of skipping it. A capture has no user to inform anyway.
    if (!is_capture_run(opts_))
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Open Bomberman", msg.c_str(), nullptr);
    return false;
}

namespace {

// Renderer backend preference — a PACING fix, not a fidelity one, and the single
// biggest lever on F8's uncapped mode (measured 2026-07-29, the reference Win11
// box, 1280x960 windowed, 60 Hz panel, 2-player match, 5 runs each):
//
//                     achieved fps                 adjacent-frame jitter
//   direct3d11   155.5 164.5 177.0 168.2 156.6     0.63-1.31 ms
//   opengl       179.7 180.0 180.0 180.0 180.0     0.001-0.023 ms
//
// The cause is not our drawing, a flat 0.11 ms of the 5.556 ms sub-frame budget in
// every configuration measured. It is SDL_RenderPresent blocking: SDL's D3D11
// backend calls SetMaximumFrameLatency(dxgiDevice, 1), so with vsync off in a
// DWM-composited window each present waits for the previous flip the compositor is
// still holding at its own 60 Hz. Fullscreen bypasses the compositor and is fine on
// BOTH backends, and the vsync path is exactly 60.0 on both, so this only moves the
// windowed uncapped case. SDL walks the list and falls back on creation failure, so
// a machine with no usable GL still gets D3D11, and an explicitly-set
// SDL_RENDER_DRIVER always wins. tests/visual's pins are byte-identical under both.
void prefer_low_latency_backend() {
#ifdef SDL_PLATFORM_WINDOWS
    if (!SDL_getenv("SDL_RENDER_DRIVER"))
        SDL_SetHint(SDL_HINT_RENDER_DRIVER, "opengl,direct3d11,direct3d12,software");
#endif
}

}  // namespace

bool GameApp::init_video() {
    video_.emplace();
    if (!video_->ok()) {
        log_warn("SDL_Init: %s", SDL_GetError());
        return false;
    }
    prefer_low_latency_backend();
    SDL_Window* win = nullptr;
    SDL_Renderer* ren = nullptr;
    // Window title matches the original (sub_41095A -> sub_43E5CC(aAtomicBomberma)).
    // SDL_WINDOW_RESIZABLE is a PORT ENHANCEMENT — the original is a hardcoded
    // 640x480 window with no resize or fullscreen path in the binary at all. The
    // sim's logical resolution stays exactly 640x480; the presentation mode below
    // scales the frame to whatever the player picks.
    if (!SDL_CreateWindowAndRenderer("Atomic Bomberman", kScreenW * 2, kScreenH * 2,
                                     SDL_WINDOW_RESIZABLE, &win, &ren)) {
        log_warn("SDL_CreateWindowAndRenderer: %s", SDL_GetError());
        return false;
    }
    window_.reset(win);
    sdl_renderer_.reset(ren);
    // Which backend won: the first thing worth knowing about any frame-rate report.
    if (const char* name = SDL_GetRendererName(ren)) log_info("renderer: %s", name);
    if (SDL_Surface* icon = load_window_icon(opts_.game_dir / "BM95.ICO")) {
        SDL_SetWindowIcon(window_.get(), icon);
        SDL_DestroySurface(icon);
    }
    // Interactive runs get STRETCH, which fills the panel edge to edge exactly as
    // the original does on the reference machine (user-verified side by side
    // 2026-07-12; the earlier LETTERBOX kept 4:3 with black bars, the reported
    // mismatch). A capture keeps LETTERBOX — the two scalers' output differs even at
    // an exact 2x 4:3 window and every existing pin was taken under it — and never
    // goes fullscreen, since a 3840x2160 fullscreen demo run silently re-hashed
    // every pinned shot once.
    const bool capture = is_capture_run(opts_);
    SDL_SetRenderLogicalPresentation(
        ren, kScreenW, kScreenH,
        capture ? SDL_LOGICAL_PRESENTATION_LETTERBOX : SDL_LOGICAL_PRESENTATION_STRETCH);
    if (fullscreen_ && !capture) SDL_SetWindowFullscreen(win, true);
    SDL_SetEventFilter(&GameApp::sdl_event_filter, this);  // for the window's lifetime
    // The original is a DirectDraw flip loop with no frame-rate id anywhere in
    // VALUELST: its front-end loops poll once per iteration with no throttle and
    // the flip IS the throttle. Every screen that free-runs a cosmetic frame
    // counter per render iteration (the menu's trigger cursor, the Goldman spin,
    // the logo timing, attract idle) was therefore advancing at this loop's
    // uncapped rate until the present was synced to the refresh. There is no
    // faithful millisecond constant to substitute — "one step per displayed frame"
    // is the relationship, and vsync is what makes it true here too.
    SDL_SetRenderVSync(ren, uncap_fps_ ? 0 : 1);
    return true;
}

bool GameApp::sdl_event_filter(void* userdata, SDL_Event* event) {
    auto* app = static_cast<GameApp*>(userdata);
    return handle_global_hotkey(app->hotkey_slots(), *event);
}

SettingsSlots GameApp::settings_slots() {
    return SettingsSlots{.opts = opts_,
                         .assets = assets_,
                         .scheme = scheme_,
                         .values = values_,
                         .keyboard = keyboard_,
                         .options = options_,
                         .options_dirty = options_dirty_,
                         .conveyor_speed_index = conveyor_speed_index_,
                         .num_to_win_match = num_to_win_match_,
                         .team_play = team_play_,
                         .fullscreen = fullscreen_,
                         .uncap_fps = uncap_fps_,
                         .native_cadence = native_cadence_,
                         .show_fps = show_fps_,
                         .soft_scaling = soft_scaling_,
                         .setup_lcg = setup_lcg_,
                         .options_path = options_path_,
                         .node_name_path = node_name_path_,
                         .node_name_loaded = node_name_loaded_};
}

DataLoadSlots GameApp::data_load_slots() {
    return DataLoadSlots{.opts = opts_,
                         .ren = sdl_renderer_.get(),
                         .assets = assets_,
                         .seqs = seqs_,
                         .front_font = front_font_,
                         .gamepads = gamepads_,
                         .audio = audio_,
                         .renderer = renderer_,
                         .screen = screen_,
                         .base_tuning = base_tuning_,
                         .scheme = scheme_,
                         .values = values_};
}

CaptureSlots GameApp::capture_slots() {
    return CaptureSlots{.opts = opts_,
                        .sdl = sdl_renderer_.get(),
                        .assets = assets_,
                        .front_font = front_font_,
                        .values = values_,
                        .sim = sim_,
                        .renderer = *renderer_,
                        .sounds = sounds_};
}

HotkeySlots GameApp::hotkey_slots() {
    return HotkeySlots{.assets = assets_,
                       .seqs = seqs_,
                       .window = window_.get(),
                       .sdl = sdl_renderer_.get(),
                       .fullscreen = fullscreen_,
                       .uncap_fps = uncap_fps_,
                       .native_cadence = native_cadence_,
                       .show_fps = show_fps_,
                       .show_netstats = show_netstats_,
                       .options_dirty = options_dirty_};
}

ScreenContext GameApp::sctx() {
    return ScreenContext{.assets = assets_,
                         .audio = audio_,
                         .sounds = sounds_,
                         .keyboard = keyboard_,
                         .gamepads = gamepads_,
                         .front_font = front_font_,
                         .cursor_blink = cursor_blink_,
                         .asset_screen = *screen_,
                         .seqs = seqs_,
                         .values = values_,
                         .sdl = sdl_renderer_.get(),
                         .window = window_.get()};
}

OptionsEditState GameApp::options_state() {
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
    return MapSelectState{.selected_level = selected_level_,
                          .win_target = win_target_,
                          .setup_lcg = setup_lcg_,
                          .options = options_,
                          .gold_player = gold_player_};
}

SetupState GameApp::setup_state() {
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
    // The frozen backdrop the campaign dialogs and the in-round help modal draw over
    // — match-runtime members, so kept out of the service-only ScreenContext.
    return MatchBackdrop{.renderer = *renderer_,
                         .state = sim_.state()};
}

CampaignState GameApp::campaign_state() {
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
    return GoldmanState{
        .goldman_lcg = goldman_lcg_, .gold_player = gold_player_, .gold_prize = gold_prize_};
}

MatchRunnerState GameApp::match_runner_state() {
    return MatchRunnerState{.sim = sim_,
                            .renderer = *renderer_,
                            .next_seed = next_seed_,
                            .kill_count = kill_count_,
                            .campaign_pacing = campaign_pacing_,
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

NetplaySeams GameApp::netplay_seams() {
    return NetplaySeams{.sctx = [this] { return sctx(); },
                        .match_runner_state = [this] { return match_runner_state(); },
                        .scoreboard_state = [this] { return scoreboard_state(); },
                        .setup_state = [this] { return setup_state(); },
                        .campaign_state = [this] { return campaign_state(); },
                        .match_backdrop = [this] { return match_backdrop(); },
                        .map_select_state = [this] { return map_select_state(); }};
}

NetplayState GameApp::netplay_state() {
    return NetplayState{.sim = sim_,
                        .renderer = *renderer_,
                        .win_count = win_count_,
                        .kill_count = kill_count_,
                        .setup_type = setup_type_,
                        .setup_sub = setup_sub_,
                        .setup_team = setup_team_,
                        .team_play = team_play_,
                        .setup_lcg = setup_lcg_,
                        .win_target = win_target_,
                        .scheme = scheme_,
                        .base_tuning = base_tuning_,
                        .options = options_,
                        .game_dir = opts_.game_dir,
                        .net_role = opts_.net_role,
                        .net_local_port = opts_.net_local_port,
                        .net_peer_host = opts_.net_peer_host,
                        .net_peer_port = opts_.net_peer_port,
                        .net_seed = opts_.net_seed,
                        .matchmaker_url = opts_.matchmaker_url,
                        .matchmaker_stun_host = opts_.matchmaker_stun_host,
                        .matchmaker_stun_port = opts_.matchmaker_stun_port};
}

NetplayRunner GameApp::netplay() {
    return NetplayRunner(netplay_seams(), netplay_state());
}

void GameApp::start_match(std::uint32_t seed) {
    MatchRunner(sctx(), match_runner_state()).start_match(seed);
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

AppInput GameApp::present_campaign_banner() {
    return CampaignBannerScreen(sctx(), campaign_state(), match_backdrop()).run();
}

AppInput GameApp::present_campaign_complete() {
    return CampaignCompleteScreen(sctx(), match_backdrop()).run();
}

AppInput GameApp::present_campaign_unsuccessful() {
    return CampaignUnsuccessfulScreen(sctx(), match_backdrop()).run();
}

AppInput GameApp::present_scoreboard() {
    return ScoreboardScreen(sctx(), scoreboard_state()).run();
}

AppInput GameApp::present_goldman_wheel() {
    return GoldmanWheelScreen(sctx(), goldman_state()).run();
}

AppInput GameApp::present_setup() {
    return SetupScreen(sctx(), setup_state(), campaign_state(), match_backdrop()).run();
}

AppInput GameApp::present_map_select() {
    return MapSelectScreen(sctx(), map_select_state()).run();
}

AppInput GameApp::present_menu() {
    return MenuScreen(sctx(), menu_state()).run();
}

AppInput GameApp::run_boot_attract() {
    return BootScreen(sctx()).run();
}

AppInput GameApp::run_match() {
    return MatchRunner(sctx(), match_runner_state()).run();
}

// Each forwarder qualifies the call so it names the free function, not itself.
int GameApp::round_winner() const {
    return ::bomber::game::round_winner(sim_.state());
}

bool GameApp::is_team_mode() const {
    return ::bomber::game::is_team_mode(team_play_, sim_.state(), setup_team_);
}

int GameApp::match_clinch() const {
    return ::bomber::game::match_clinch(sim_.state(), team_play_, setup_team_,
                                        options_.win_by_kills, kill_count_, win_count_,
                                        win_target_);
}

void GameApp::award_round_win(int winner) {
    ::bomber::game::award_round_win(win_count_, winner, team_play_, sim_.state(), setup_team_);
}

bool GameApp::auto_advance_results() const {
    return ::bomber::game::auto_advance_results(opts_.demo, opts_.demo_ticks, opts_.demo_shots,
                                                setup_type_);
}

void GameApp::reset_match_scores() {
    ::bomber::game::reset_match_scores(win_count_, kill_count_, win_target_, values_,
                                       num_to_win_match_);
}

void GameApp::restore_from_attract() {
    if (!attract_) return;
    setup_type_ = attract_saved_.type;
    setup_sub_ = attract_saved_.sub;
    setup_team_ = attract_saved_.team;
    selected_level_ = attract_saved_.level;
    team_play_ = attract_saved_.team_play;
    attract_ = false;  // the original clears dword_464938 here
}

int GameApp::run_app() {
    AppState state = opts_.boot_match ? AppState::Match : AppState::Boot;
    while (!is_terminal(state)) {
        const std::optional<AppInput> ev = run_state(state);
        if (!ev) return 0;  // the window closed under whichever screen was up
        state = next(state, *ev);
    }
    return 0;
}

// One arm per state, and nothing else: every arm is a call. Two states own a
// multi-screen loop rather than a single present_screen — Boot runs the linear
// IPLOGO->HSLOGO->TITLE chain (no attract re-run; the title's timeout falls through
// to the menu) and Menu resolves the highlighted row into a concrete AppInput.
std::optional<AppInput> GameApp::run_state(AppState state) {
    switch (state) {
        case AppState::Boot: return run_boot_attract();
        // Pass-throughs: run_boot_attract already presented the logos and title, so
        // walking the graph on keeps next() authoritative over the boot hops.
        case AppState::Logo:
        case AppState::Title: return AppInput::Advance;
        case AppState::Menu: return run_menu_arm();
        case AppState::Match: return run_match();
        case AppState::Results: return run_results_arm();
        case AppState::Options: return present_options_screen();
        case AppState::Controllers: return present_bm_screen("INPUT");
        case AppState::Network: return present_bm_screen("NETWORK");
        // These two ARE the original's menu rows 1 and 2, and each of its net screens
        // starts 1040 as its first act (sub_42B0CE @0x42B11B, sub_42B47D @0x42B4C0 —
        // an exhaustive sweep finds six music call sites in the whole image and two of
        // them are these, docs/re/sound-engine.md §9). Nothing switches back
        // explicitly: present_menu() restarts 1010 on every menu entry, which is how
        // the original reclaims the loop.
        case AppState::NetHost:
            audio_.start_music(kNetMusicId);
            return netplay().present_network_menu();
        case AppState::NetJoin:
            audio_.start_music(kNetMusicId);
            return netplay().present_direct_join();
        case AppState::Credits: return present_bm_screen("CREDITS");
        case AppState::Quit: break;
    }
    return AppInput::Advance;
}

std::optional<AppInput> GameApp::run_menu_arm() {
    const AppInput ev = present_menu();
    if (ev != AppInput::StartMatch) return ev;
    // PLAY: the pre-match flow decides whether a match actually starts.
    return run_play_flow();
}

// WHICH TIER a round end belongs to is one decision, taken here and taken once:
// campaign_round_end.hpp (pinned headlessly) answers it and the two calls below are
// its SDL sides. They share nothing, which is why they are two functions and not one
// with a flag threaded through it — the shape that used to hide a music gate, a
// round_winner() override and a stage advance in the VICTORY branch.
std::optional<AppInput> GameApp::run_results_arm() {
    const CampaignRoundEnd plan = campaign_round_end(
        campaign_active_, campaign_pacing_.verdict, campaign_pacing_.no_human_survivor,
        campaign_stage_index_, static_cast<int>(campaign_stages_.size()));
    if (plan.run_outcome_tier) return run_outcome_tier();
    return run_campaign_round_end(plan);
}

// THE CAMPAIGN ARM of a round end (docs/re/campaign.md "Round end" has the full
// branch table). sub_42A3F6's round loop tests dword_46489C at 0x42A63B and leaves
// the ordinary outcome tier ENTIRELY: no 1130 music, no DRAW, no tally, no VICTORY,
// no win award, no gold-player write. At most ONE modal, and only when the pacing
// verdict dword_464894 is 2 (0x42A657), then the ROUND INIT sub_410B6E at 0x42A68B.
//
// Note WHERE the stage advance sits: after EVERY campaign round, not after a won
// best-of-N match. The port used to hang it off the VICTORY branch, so a campaign
// only moved on once someone had clinched win_target_ rounds — a shape the original
// does not have, because a campaign round never reaches the tier that counts wins.
std::optional<AppInput> GameApp::run_campaign_round_end(const CampaignRoundEnd& plan) {
    if (plan.show_unsuccessful && present_campaign_unsuccessful() == AppInput::Quit)
        return std::nullopt;
    campaign_stage_index_ = plan.next_stage_index;
    // sub_410B6E's tail: load the stage sub_40133F's ++ just selected, show its
    // banner and play the next round. A scheme the install cannot resolve is treated
    // as "out of stages" (port convenience, unpinned) rather than starting a match
    // on a stale board.
    if (plan.next_stage && load_campaign_stage(campaign_stage_index_, campaign_state())) {
        reset_match_scores();
        if (present_campaign_banner() == AppInput::Quit) return std::nullopt;
        return AppInput::CampaignContinue;
    }
    if (plan.show_complete || plan.next_stage) {
        // sub_40133F's stage-exhausted branch: "Congratulations!" (1220/1225), then
        // dword_464A68 = 10, i.e. out to the menu.
        if (present_campaign_complete() == AppInput::Quit) return std::nullopt;
    }
    // Verdict 0 lands here too, with neither modal: the original only reaches its
    // round-loop tail with dword_464894 still 0 after a Ctrl+Q forfeit, which has
    // already written the menu sentinel dword_464A68 = 2 at 0x42A579. Our Esc/Ctrl+Q
    // abort is the same act, so it leaves the same way. dword_46489C itself is left
    // stale in the original and overwritten by sub_42A3F6's entry reset on the next
    // Play; the port clears it both here and there (campaign.md "Campaign-exit key").
    campaign_active_ = false;
    campaign_stages_.clear();
    campaign_stage_index_ = 0;
    return AppInput::Advance;
}

// The three-tier sub_42A3F6 results tail (docs/re/frontend-flow.md "results flow"):
// a round win bumps that player's tally, the match is decided once the tally reaches
// win_target_, and otherwise a survivor shows the RESULTS scoreboard while a draw
// (round_winner() folds no-survivor and time-up) shows DRAW — both replaying.
//
// Results MUSIC — CORRECTED per docs/re/in-match-shell.md §2: sub_42741E(0x46A) =
// 1130 starts UNCONDITIONALLY on round-loop exit, BEFORE the survivor test, so DRAW,
// the tally AND VICTORY/TEAM all play under it. 1020 is the SETUP-SCREENS track and
// is never restarted in this tier; this file previously read it as VICTORY's own.
//
// Reachable ONLY with campaign mode off. Returns the flow-graph event, INCLUDING
// Quit: a window close here walks next(Results, Quit) to the terminal state like any
// other rather than short-circuiting run_app.
AppInput GameApp::run_outcome_tier() {
    AppInput ev = AppInput::Advance;
    const int w = round_winner();
    // Under Team Play the win mirrors onto the winner's teammates (sub_421B56, called
    // from 0x42A919), so every member holds the TEAM total the scoreboard row and the
    // clinch both read.
    if (w >= 0) award_round_win(w);
    // The match-over check (§1 v73). The clinching slot can differ from the round
    // winner under win_by_kills — a team's kill leader need not be this round's sole
    // survivor — so VICTORY names whoever match_clinch() returns, not `w`.
    const int clinched = w >= 0 ? match_clinch() : -1;
    const bool match_over = clinched >= 0;
    // Gold player (docs/re/goldman-roulette.md §2, pseudo.c 30004-30022): sub_42A3F6
    // reaches the RESULTS tier, and its unconditional dword_46492C write, only when a
    // ROUND SURVIVOR exists — a DRAW takes the separate DRAW.PCX branch and never
    // touches dword_46492C, so a pending gold player survives a drawn round. The
    // value written is v73, the MATCH-CLINCH winner, so it changes only when a match
    // is decided and reverts to "none pending" on every clinch-less pass.
    if (w >= 0)
        gold_player_ = assign_gold_player(options_.goldman, is_team_mode(), clinched, setup_team_);
    audio_.start_music(kDrawMusicId);  // 1130, under every arm below (doc §2)
    if (match_over) {
        // sub_42A3F6 still renders the RESULTS scoreboard on the clinching round (with
        // the "WINS THE MATCH!" outcome line) and plays the 2000 "we have a winner"
        // voice UNDER it — the ONLY site that voice fires (batch_0x4293E5.cpp:1298,
        // the branch taken when v73 is not -1) — THEN cuts to VICTORY. The port
        // formerly skipped the scoreboard and mis-fired 2000 on every round win.
        audio_.play_sting(kWinnerStingLo, kWinnerStingHi);
        ev = present_scoreboard();
        if (ev != AppInput::Quit)
            ev = present_screen(victory_screen(is_team_mode(), clinched, setup_team_[clinched]));
    } else if (w >= 0) {
        ev = present_scoreboard();  // running scores, and NO winner voice
    } else {
        ev = run_draw_tier();
    }
    // Fold the screen's dismissal into the flow-graph event: an undecided round's
    // Advance becomes RoundContinue, so next(Results, RoundContinue) loops straight
    // back into Match and the next round reuses the same roster/level/win-target.
    // Back (Escape) abandons the match to the menu; a decided match's Advance ends it
    // there too. next() stays the single authority over the state walk.
    if (!match_over && ev == AppInput::Advance) ev = AppInput::RoundContinue;
    return ev;
}

// DRAW (no survivor / time-up): nobody scores and a round replays. It FALLS THROUGH
// INTO THE RESULTS TALLY — a PREFIX, not an alternative (docs/re/in-match-shell.md
// "DRAW is a PREFIX to the RESULTS tally", raw 0x42A875-0x42A88B: the DRAW wait loop
// ends with NO jump and lands in LABEL_102, which loads RESULTS.PCX). So a drawn
// round shows both screens; the port showed DRAW alone until this was pinned.
AppInput GameApp::run_draw_tier() {
    audio_.play_sting(kDrawStingLo, kDrawStingHi);
    // sub_42A3F6's DRAW loop only auto-advances (6 s) for an all-AI/attract roster; a
    // human match waits for Enter, and a 0 dwell is how the Screen model spells "no
    // auto-advance" (screen.cpp:47).
    ScreenDef ds = draw_screen();
    if (!auto_advance_results()) ds.dwell_ms = 0;
    const AppInput ev = present_screen(ds);
    if (ev == AppInput::Quit || ev == AppInput::Back) return ev;
    return present_scoreboard();
}

// THE PRE-MATCH FLOW reached from Play (sub_42A3F6's head): the campaign-flag reset,
// the setup-screens track, the attract short-circuit, the Goldman wheel, then the
// PLAYER INPUT screen (sub_410F81) and the LEVEL & ROUNDS screen (sub_406DDE).
//
// CORRECTED: Escape does NOT back up one step at a time — sub_406DDE is called from
// sub_410F81's own TAIL (pseudo.c 15504-15517) with nothing after that call but
// `sub_401312()` and return, so an Esc on EITHER screen aborts the WHOLE Play flow
// straight back to the menu. There is no "go back to the player screen" path.
std::optional<AppInput> GameApp::run_play_flow() {
    // `dword_46489C = 0` is the LITERAL FIRST statement of sub_42A3F6 (pseudo.c
    // 29692), unconditionally, every time Play is entered fresh from the menu. There
    // is no dedicated campaign-exit KEY anywhere in the binary — grepping every
    // read/write of dword_46489C finds exactly two writes, this reset and
    // sub_4015C6's `=1` — so this entry reset IS the mechanism that keeps a previous
    // campaign run from leaking into a fresh one. Without it, aborting mid-campaign
    // leaves a stale campaign armed for the NEXT Play, which silently skips the
    // level screen and resumes the abandoned stage.
    campaign_active_ = false;
    campaign_stages_.clear();
    campaign_stage_index_ = 0;
    // Reset the tally FIRST; the level screen owns the win target and adjusts it.
    reset_match_scores();
    audio_.start_music(kWinMusicId);
    // ATTRACT short-circuit (docs/re/frontend-flow.md "Attract mode" point 1,
    // sub_410F81 pseudo.c 15125-15143): present_menu() already rolled the demo roster
    // and set attract_, so this goes STRAIGHT to the match — no wheel (doc §2's
    // `!dword_464938` gate leaves a real pending prize untouched for the next
    // non-attract Play rather than forfeiting it), no player screen, no LEVEL &
    // ROUNDS. Calling run_match DIRECTLY bypasses the Match/Results walk so Results
    // never renders — doc point 2's "Round end skips ALL outcome screens".
    if (attract_) {
        if (run_match() == AppInput::Quit) return std::nullopt;
        restore_from_attract();
        return AppInput::Advance;  // Menu -> (stays) Menu
    }
    // The Goldman wheel (docs/re/goldman-roulette.md §2) runs at the head of every
    // non-attract Play entry, gated on the goldman option and a gold player actually
    // pending from a previous match. An Esc abort forfeits the whole Play flow,
    // mirroring sub_410F81 returning right after the call whenever dword_464A68 is
    // set.
    if (options_.goldman && gold_player_ >= 0) {
        const AppInput wheel = present_goldman_wheel();
        if (wheel == AppInput::Quit) return std::nullopt;
        if (wheel == AppInput::Back) return AppInput::Advance;
    }
    const std::optional<bool> confirmed = run_prematch_screens();
    if (!confirmed) return std::nullopt;
    return *confirmed ? AppInput::StartMatch : AppInput::Advance;
}

// Written as a `while (!started)` loop that could never run twice — every path
// either sets `started` or breaks — so it is straight line here, which is also
// what the RE comment says it is.
std::optional<bool> GameApp::run_prematch_screens() {
    const AppInput setup = present_setup();
    if (setup == AppInput::Quit) return std::nullopt;
    if (setup == AppInput::Back) return false;
    // Campaign mode SKIPS the LEVEL & ROUNDS screen entirely (docs/re/campaign.md,
    // sub_406DDE's `if (!dword_46489C)` gate): present_setup's own 'C'x5 trigger
    // already picked a stage and seeded the roster.
    if (campaign_active_) return true;
    const AppInput lvl = present_map_select();
    if (lvl == AppInput::Quit) return std::nullopt;
    if (lvl == AppInput::Back) return false;
    return true;
}

int GameApp::run_capture() {
    if (!opts_.bm_shot_name.empty()) return run_bm_capture(capture_slots());
    if (!opts_.menu_shot_out.empty()) return run_menu_capture(capture_slots());
    if (!opts_.demo_shot_dir.empty()) {
        std::error_code ec;
        fs::create_directories(opts_.demo_shot_dir, ec);  // ignore: SDL_SaveBMP reports failure
    }
    // --demo-players N (dev-only, README animation capture): an all-COMPUTER
    // roster. Absent (0) leaves the 1-human + 1-AI default, so the visual goldens
    // keep rendering the exact same scripted match.
    if (opts_.demo_players > 0)
        for (int i = 0; i < sim::kMaxPlayers; ++i)
            setup_type_[i] = static_cast<int>(i < opts_.demo_players ? SlotInputType::Computer
                                                                     : SlotInputType::Off);
    start_match(opts_.demo_seed);  // 0xB0BB1E5 unless --demo-seed overrides
    return run_demo_capture(capture_slots());
}

int GameApp::run() {
    if (const char* env = std::getenv("BOMBER_BOOT_MATCH"); env && *env) opts_.boot_match = true;
    if (!init()) return opts_.game_dir.empty() ? 2 : 1;
    // A capture never reaches the front end; netplay's CLI entry replaces it. The
    // capture check comes first because those paths never set net_role.
    int rc = 0;
    if (!opts_.bm_shot_name.empty() || !opts_.menu_shot_out.empty() || opts_.demo)
        rc = run_capture();
    else if (opts_.net_role != 0)
        // Back = the CLI never got a working link (bind/resolve/exchange).
        // Anything else — a finished match, a window close — exits 0, as the
        // CLI always has.
        rc = netplay().run_cli() == AppInput::Back ? 1 : 0;
    else
        rc = run_app();
    flush_settings(settings_slots());
    return rc;
}

}  // namespace bomber::game
