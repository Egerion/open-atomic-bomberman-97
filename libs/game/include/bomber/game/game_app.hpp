#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "bomber/assets/campaign.hpp"
#include "bomber/assets/reslist.hpp"
#include "bomber/assets/sch.hpp"
#include "bomber/audio/audio_engine.hpp"
#include "bomber/audio/sound_director.hpp"
#include "bomber/frontend/campaign_state.hpp"
#include "bomber/frontend/map_select_state.hpp"
#include "bomber/frontend/match_backdrop.hpp"
#include "bomber/frontend/match_runner_state.hpp"
#include "bomber/frontend/menu_state.hpp"
#include "bomber/frontend/options_state.hpp"
#include "bomber/frontend/results_state.hpp"
#include "bomber/frontend/setup_state.hpp"
#include "bomber/game/app_options.hpp"
#include "bomber/game_util/app_flow.hpp"
#include "bomber/game_util/campaign_round_end.hpp"
#include "bomber/game_util/cursor_indicator.hpp"
#include "bomber/input/gamepad.hpp"
#include "bomber/input/input.hpp"
#include "bomber/netplay/netplay_runner.hpp"
#include "bomber/netplay/netplay_state.hpp"
#include "bomber/render/asset_store.hpp"
#include "bomber/render/renderer.hpp"
#include "bomber/render/sdl.hpp"
#include "bomber/render/sequences.hpp"
#include "bomber/sim/simulation.hpp"
#include "bomber/sim/tuning.hpp"
#include "bomber/ui/bmscreen.hpp"  // FontTextures
#include "bomber/ui/screen.hpp"
#include "bomber/ui/screen_context.hpp"

// THE APPLICATION SHELL, and only that: SDL and window lifetime, the loaded
// services every screen borrows, the settings this session is editing, and the
// AppState loop that hands control to one screen at a time. Nothing here draws.
// Its four heavier jobs are their own units under src/ (app_settings,
// boot_loading, capture_runs, global_hotkeys), each taking a by-reference slot
// bundle built here on demand, the same seam ScreenContext uses.

namespace bomber::game {

struct SettingsSlots;
struct DataLoadSlots;
struct CaptureSlots;
struct HotkeySlots;

// NOLINT: a singleton root object, not a hashed sim or hot-path type. The suggested
// member reorder must be done by hand with the member-initializer order tracking it,
// risking a transcription bug for a one-time 34-byte saving on one instance.
class GameApp {  // NOLINT(clang-analyzer-optin.performance.Padding)
public:
    explicit GameApp(AppOptions opts) : opts_(std::move(opts)) {}
    ~GameApp();

    int run();  // to completion; returns the process exit code

private:
    bool init();
    void seed_front_end_rngs();
    // The install dir (into opts_.game_dir) + the scheme path, or fail with usage.
    bool resolve_install_paths(std::filesystem::path& scheme_path);
    bool init_video();
    int run_capture();  // whichever of the four capture flags this run carries
    static bool SDLCALL sdl_event_filter(void* userdata, SDL_Event* event);

    SettingsSlots settings_slots();
    DataLoadSlots data_load_slots();
    CaptureSlots capture_slots();
    HotkeySlots hotkey_slots();

    // The shared-services bundle every extracted screen is handed (ADR-0008) and the
    // ADR-0009 per-cluster state seams. Field order MUST track each struct's.
    ScreenContext sctx();
    OptionsEditState options_state();
    MenuState menu_state();
    MapSelectState map_select_state();
    SetupState setup_state();
    MatchBackdrop match_backdrop();
    CampaignState campaign_state();
    ScoreboardState scoreboard_state();
    GoldmanState goldman_state();
    MatchRunnerState match_runner_state();
    // Netplay takes BUILDERS rather than built bundles: its round loop replaces sim_
    // between rounds, so a bundle captured once would describe a dead match.
    NetplaySeams netplay_seams();
    NetplayState netplay_state();
    NetplayRunner netplay();

    // The front-end shell (docs/adr/0004): Boot -> Logo -> Title -> Menu -> Match ->
    // Results -> Menu. The pure transition graph is app_flow.hpp; these are the thin
    // SDL side, documented at their definitions. std::optional = "the window closed".
    int run_app();
    std::optional<AppInput> run_state(AppState state);
    std::optional<AppInput> run_menu_arm();
    std::optional<AppInput> run_results_arm();
    std::optional<AppInput> run_play_flow();
    std::optional<bool> run_prematch_screens();
    std::optional<AppInput> run_campaign_round_end(const CampaignRoundEnd& plan);
    AppInput run_outcome_tier();
    AppInput run_draw_tier();

    // Thin forwarders to the extracted screen classes, kept so the flow above reads as
    // a vocabulary rather than as constructor calls. Each screen's citations are on
    // the class that owns it.
    AppInput present_screen(const ScreenDef& def);
    AppInput present_bm_screen(const std::string& bm_name);
    AppInput present_options_screen();
    AppInput present_campaign_banner();
    AppInput present_campaign_complete();
    AppInput present_campaign_unsuccessful();
    AppInput present_scoreboard();
    AppInput present_goldman_wheel();
    AppInput present_setup();
    AppInput present_map_select();
    AppInput present_menu();
    AppInput run_boot_attract();
    AppInput run_match();
    // A GameApp method, not just a MatchRunner private, because the --demo path
    // builds a match through it before ticking sim_ directly.
    void start_match(std::uint32_t seed);
    // Attract-mode exit (sub_422552, "Menu re-entry restores everything"), called on
    // EVERY path back to the menu after an attract match. Idempotent, so callers need
    // not know whether the round ended naturally or was aborted.
    void restore_from_attract();

    // Forwarders to the free functions of the same names in match_outcome.hpp,
    // promoted there so ScoreboardScreen and MatchRunner can call the SAME logic
    // without a GameApp&. The citations are on those.
    int round_winner() const;
    bool is_team_mode() const;
    int match_clinch() const;
    void award_round_win(int winner);
    bool auto_advance_results() const;
    void reset_match_scores();

    AppOptions opts_;

    int menu_index_ = 0;  // highlighted main-menu row (persists across visits)
    // The two hidden triggers' same-key repeat counters, here rather than as locals
    // because they must survive their screen's per-frame event pump. Ctrl+E six
    // consecutive times opens the editor (sub_42B9CE pseudo.c 30876-30883, `++counter
    // > 5`); 'C' FIVE times opens the campaign picker (sub_410F81 pseudo.c
    // 15357-15365, `== 5` — five, not a sixth).
    int editor_trigger_count_ = 0;
    int campaign_trigger_count_ = 0;

    // Best-of-N match state (sub_42A3F6), presentation-only — never sim::State, never
    // hashed. win_target_'s in-class 2 covers only the dev fast-path;
    // reset_match_scores() seeds the real default and LEVEL & ROUNDS owns it.
    std::array<int, sim::kMaxPlayers> win_count_{};
    int win_target_ = 2;
    // CUMULATIVE for the whole match, despite the original's name for it ("round-kill
    // count", sub_421B0F's field): docs/re/results-and-options.md §1 is explicit that
    // it rides across rounds in the same 152-byte record as the win count.
    std::array<int, sim::kMaxPlayers> kill_count_{};
    std::optional<int> num_to_win_match_;  // options.ini "num_to_win_match="

    // The roster the PLAYER INPUT screen edits (sub_410F81): input type (byte +16, see
    // SlotInputType), input sub-index (+17), team (+84). setup_team_'s all-0 default
    // matches "every slot OFF"; present_setup re-derives the real one on every entry
    // (alternating slot & 1, sub_4049C0).
    std::array<int, sim::kMaxPlayers> setup_type_{2, 1};
    std::array<int, sim::kMaxPlayers> setup_sub_{};
    std::array<int, sim::kMaxPlayers> setup_team_{};
    int selected_level_ = -1;  // -1 = RANDOM (pick_stage), else 0..10 (sub_406DDE)

    // The front end's presentation-only LCGs, NEVER sim::State::rng (ADR-0004). The
    // literals are construction-time placeholders ONLY: init() overwrites all four
    // from random_boot_seed(), mirroring the original's boot-time `time_(); srand_();`
    // (sub_41095A pseudo.c 14610-14611/14639-14640). Leaving them fixed was the bug
    // where a fresh process always picked the same RANDOM level and brick fill.
    std::uint32_t setup_lcg_ = 0x5E7C0DE5u;    // glue pick, preview swatch
    std::uint32_t attract_lcg_ = 0x0A77AC70u;  // attract roster + stage rolls
    std::uint32_t goldman_lcg_ = 0x60D1BEEFu;  // the wheel's 5 draws
    std::uint32_t next_seed_ = 0xB0BB1E5;      // per-round match seed

    // ATTRACT MODE (docs/re/frontend-flow.md, sub_42B9CE's idle path). The idle clock
    // is getvalue(92)=30s, a different id and timer from a Screen's getvalue(12)=7s
    // dwell; 0 = not yet initialised for this menu visit.
    std::uint64_t menu_idle_since_ms_ = 0;
    bool attract_ = false;          // dword_464938
    AttractSaved attract_saved_{};  // sub_4224E2's snapshot, put back by sub_422552

    // Campaign mode (docs/re/campaign.md), armed only by present_setup's 'C'x5 trigger
    // plus a successful *.cam pick. While active the stage's scheme and roster REPLACE
    // setup_type_/selected_level_, and run_app auto-advances between stages.
    bool campaign_active_ = false;                             // dword_46489C
    std::vector<assets::res::CampaignStage> campaign_stages_;  // dword_45E010
    int campaign_stage_index_ = 0;                             // dword_4648B0
    std::string campaign_banner_;                              // getstring 1235, "(%s)"
    CampaignPacing campaign_pacing_;  // dword_464894 (+ the 0x401786 decrement)

    // The Goldman wheel's pending winner (dword_46492C, docs/re/goldman-roulette.md
    // §2): -1 = none pending, else a player index — or, in team mode, a RAW 0/1 TEAM
    // id, our port's team-id space rather than the original's internal 0/2 encoding.
    // Set from match_clinch(), never from the per-round winner.
    int gold_player_ = -1;
    int gold_prize_ = -1;  // the last spin's award; never reset on consumption (§4)

    assets::sch::Scheme scheme_;
    assets::res::ValueList values_;
    sim::Tuning base_tuning_;  // VALUELST-applied (colors, stage rotation, taunts)

    // The options.ini snapshot this session is editing in memory: every Options row
    // edits it and sets the dirty flag, and flush_settings() at exit is the ONLY
    // writer (app_settings.hpp).
    OptionsSnapshot options_{};
    bool options_dirty_ = false;
    // Mirrors options_.conveyor_speed_index (dword_464930); a separate optional so "no
    // key, ever" falls back to Tuning's confirmed 1 = medium, not OptionsSnapshot's.
    std::optional<int> conveyor_speed_index_;
    // The game-type-level team GATE (dword_464964), separate from each slot's own
    // setup_team_[] byte: start_match zeroes every MatchConfig::team[] when this is
    // false. There is no MatchConfig::team_play field — team[]'s all-zero state IS the
    // hashed gate (docs/re/setup-screens.md "Roster/level -> match").
    bool team_play_ = false;
    std::filesystem::path options_path_;
    std::filesystem::path node_name_path_;  // the net identity's OWN file
    std::string node_name_loaded_;          // as read at boot, to skip a no-op rewrite

    // PORT-ONLY levers, none of them one of the original's 22 options.ini keys. The
    // first five persist through the same read-modify-write machinery; show_netstats_
    // deliberately does NOT, which is why is_capture_run() says nothing about it: no
    // saved value can reach a capture at all, stronger than a capture-time pin.
    bool fullscreen_ = false;
    bool uncap_fps_ = false;       // F8: vsync off + sub-frame pacing (~180 fps)
    bool native_cadence_ = false;  // F9: per-frame wall-clock sim; NON-DETERMINISTIC
    bool show_fps_ = true;         // F7: the fps/cadence indicator
    bool soft_scaling_ = false;    // F10 row: linear instead of nearest upscale
    bool show_netstats_ = false;   // F3: the in-match netplay diagnostic panel

    std::optional<sdl::VideoSubsystem> video_;
    sdl::WindowPtr window_;
    sdl::RendererPtr sdl_renderer_;

    AssetStore assets_;
    SequenceSet seqs_;
    AudioEngine audio_;
    SoundDirector sounds_{audio_};
    std::optional<Renderer> renderer_;
    KeyboardMapper keyboard_;
    GamepadMapper gamepads_;  // refreshed at boot and on every hotplug

    std::optional<Screen> screen_;
    CursorIndicator cursor_blink_;  // sub_413BD6's dword_460559/46055D pair
    FontTextures front_font_;       // FONT6.FON glyphs for the .BM screens

    sim::Simulation sim_;
};

}  // namespace bomber::game
