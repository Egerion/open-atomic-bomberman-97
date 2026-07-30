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
#include "bomber/game/app_flow.hpp"
#include "bomber/game/asset_store.hpp"
#include "bomber/audio/audio_engine.hpp"
#include "bomber/game/bmscreen.hpp"
#include "bomber/game/campaign_screen.hpp"
#include "bomber/game/chat_overlay.hpp"
#include "bomber/game/editor_screen.hpp"
#include "bomber/game/cursor_indicator.hpp"
#include "bomber/game/gamepad.hpp"
#include "bomber/game/goldman_screen.hpp"
#include "bomber/game/input.hpp"
#include "bomber/game/keyremap_screen.hpp"
#include "bomber/game/options_screen.hpp"
#include "bomber/game/renderer.hpp"
#include "bomber/game/results.hpp"
#include "bomber/game/screen.hpp"
#include "bomber/game/screen_context.hpp"
#include "bomber/game/screens/campaign_state.hpp"
#include "bomber/game/screens/map_select_state.hpp"
#include "bomber/game/screens/match_backdrop.hpp"
#include "bomber/game/screens/match_runner_state.hpp"
#include "bomber/game/screens/menu_state.hpp"
#include "bomber/game/screens/options_state.hpp"
#include "bomber/game/screens/results_state.hpp"
#include "bomber/game/screens/setup_state.hpp"
#include "bomber/game/sdl.hpp"
#include "bomber/game/sequences.hpp"
#include "bomber/audio/sound_director.hpp"
#include "bomber/sim/match_config.hpp"
#include "bomber/sim/simulation.hpp"

// The playable front-end: owns the SDL window, the asset store, the
// presentation systems, and the match lifecycle around the deterministic sim.

// The netplay transport is only ever named by reference in a couple of method
// signatures (run_netplay_match + the connect-screen entry points); the heavy
// socket header stays out of this widely-included header — game_app.cpp pulls
// in the real bomber/net/udp_transport.hpp.
namespace bomber::net {
class Transport;     // the abstract seam: a bare socket, the star hub, or the relay
class UdpTransport;  // the concrete socket the CLI paths bind for themselves
}  // namespace bomber::net

namespace bomber::game {

// clang-analyzer-optin.performance.Padding (NOLINT below) — a singleton root
// object (one instance for the app's lifetime, apps/game/main.cpp), not a
// hashed sim/hot-path type; clang-tidy's suggested reorder touches ~48
// members by hand in a class this large, which risks a transcription bug
// (member-initializer-list order must track it) for a one-time 34-byte
// saving that has no measurable effect on a singleton.
class GameApp {  // NOLINT(clang-analyzer-optin.performance.Padding)
public:
    struct Options {
        std::filesystem::path game_dir;  // empty: auto-detect (bomber::assets)
        std::filesystem::path scheme;    // empty: DATA/SCHEMES/BASIC.SCH
        bool demo = false;               // headless scripted run + screenshot(s)
        int demo_ticks = 0;               // single-shot legacy mode: run this many ticks
        std::filesystem::path demo_out;   // single-shot legacy mode: BMP output path
        // Visual golden harness (tests/visual/, --demo-shots): (label, tick)
        // pairs captured within ONE scripted run, each saved as
        // "<label>.bmp" under demo_shot_dir. Non-empty overrides the legacy
        // single-shot fields above — the run advances to the highest
        // requested tick, saving a frame every time a requested tick is
        // reached. See tests/visual/README.md for the recapture procedure
        // and the determinism guarantees this depends on.
        std::vector<std::pair<std::string, int>> demo_shots;
        std::filesystem::path demo_shot_dir;
        // Dev-only capture knobs (--demo-players / --demo-seed), used to render
        // the README's match animation: fill N COMPUTER slots and/or replace the
        // fixed demo seed, so a headless run can capture a BUSY all-AI match
        // instead of the scripted 1-human + 1-AI pair (a human slot stands
        // still with no keyboard attached). Both default to "as before", and
        // tests/visual/ passes neither — its pinned frames are untouched.
        int demo_players = 0;                  // 0 = keep the default roster
        std::uint32_t demo_seed = 0xB0BB1E5u;  // the historic --demo match seed
        // Dev fast-path: skip the front-end and boot straight into a match
        // (also via env BOMBER_BOOT_MATCH). The spine still exists; this just
        // starts the app in the Match state for quick iteration.
        bool boot_match = false;
        // Dev capture hook (--bm-shot): render one `.BM` text screen (Credits /
        // Manual / ...) over the MAINMENU backdrop, scrolled `bm_shot_scroll`
        // lines down, and save it as a BMP — the front-end analogue of --demo's
        // match-frame capture, used to eyeball/regress the sub_41302D viewer
        // layout without driving the menu by hand.
        std::string bm_shot_name;               // e.g. "CREDITS"
        std::filesystem::path bm_shot_out;       // BMP output path
        // Dev capture hook (--menu-shot): render the main-menu composite
        // (MAINMENU backdrop + "V1.0" + the row-0 trigger cursor) to a BMP, for
        // pixel comparison against the native oracle's --boot-shot menu render.
        std::filesystem::path menu_shot_out;
        int bm_shot_scroll = 0;                  // lines scrolled down before capture
        // Netplay (increment 5b, ADR-0010 §3.3 step 5): when net_role != 0, run()
        // runs ONE networked 2-player UDP match (run_netplay) instead of the
        // front-end. Both peers pass each other's host:port explicitly (no
        // discovery/handshake in this MVP) and the SAME seed, which — with the
        // canonical config run_netplay builds — gives byte-identical arenas.
        int net_role = 0;                  // 0 = none, 1 = host (seat 0), 2 = guest (seat 1)
        std::uint16_t net_local_port = 0;  // UDP port to bind (host); guest binds ephemeral
        std::string net_peer_host;         // the OTHER peer's host (dotted IPv4 or name)
        std::uint16_t net_peer_port = 0;   // the OTHER peer's UDP port
        std::uint32_t net_seed = 0x1234u;  // shared match seed (must match on both peers)
        // Online lobby endpoints (ADR-0011 Phase 1d). Empty = fall through to the
        // BOMBER_MATCHMAKER_* env vars, then to the compile-time defaults in
        // game_app.cpp, which now name the DEPLOYED matchmaker — so the online
        // rows work with no flags. `--matchmaker <ws-url>` and `--matchmaker-stun
        // <host[:port]>` override (a local instance, or your own host); the STUN
        // host defaults to the URL's own host (the Go server serves the WebSocket
        // and the UDP STUN echo from one box, PROTOCOL.md §2).
        std::string matchmaker_url;
        std::string matchmaker_stun_host;
        std::uint16_t matchmaker_stun_port = 0;  // 0 = unset
    };

    explicit GameApp(Options opts) : opts_(std::move(opts)) {}
    // Flushes options.ini on a normal shutdown if anything changed in memory
    // (docs/re/results-and-options.md §2 "Persistence — CONFIRMED via an
    // exit-time write-back": sub_405DE3 only runs through sub_410EBF's
    // atexit-style hook on normal exit, never per-edit). run() calls this
    // itself before returning; the destructor is a backstop for any other
    // exit path (e.g. a test harness that never calls run()'s tail). The
    // write does filesystem I/O that can throw; destructors are implicitly
    // noexcept, so swallow — losing an options write on a failing disk is
    // strictly better than std::terminate (bugprone-exception-escape).
    ~GameApp() {
        try {
            flush_options();
        } catch (...) {  // NOLINT(bugprone-empty-catch) — see the doc comment above
        }
    }

    // Runs to completion; returns the process exit code.
    int run();

private:
    // init() is a straight-line boot sequence; these are its ordered steps
    // (ADR-0008 god-object decomposition — a pure extract-method split, each
    // runs exactly where it did in the original single function). init() just
    // calls them in order, threading the resolved install paths and the live
    // SDL renderer between the steps that need them.
    bool init();
    // Is this run a PIXEL CAPTURE (--demo / --demo-shots / --bm-shot /
    // --menu-shot) rather than someone playing? Capture runs must be hermetic:
    // identical inputs, identical pixels, on any machine. Anything read from the
    // player's own options.ini that could move a pixel has to be pinned for them.
    //
    // This exists because it was NOT one predicate. `playtime` was pinned here
    // for exactly this reason, then the three PORT-ONLY Video Settings keys were
    // added later and nobody pinned them — so simply playing the game with the
    // FPS readout on (F3) silently broke all five visual pins, and an
    // investigation blamed an innocent commit before the real cause was found.
    bool capture_run() const;
    void seed_front_end_rngs();  // reseed the presentation LCGs (demo pins them)
    // Resolve the install dir + scheme path (out-params), or fail with usage.
    bool resolve_install_paths(std::filesystem::path& game,
                               std::filesystem::path& scheme_path);
    // Load scheme + VALUELST + options.ini into the config members; false on any
    // parse error (the whole load is one try/catch).
    bool load_config(const std::filesystem::path& game,
                     const std::filesystem::path& scheme_path);
    // SDL init + window/renderer creation + logical-presentation/vsync setup;
    // hands back the live renderer (used by the two asset steps below).
    bool init_video(SDL_Renderer*& ren);
    // FONT6 + the "Loading data..." dialog animated across assets_.load()'s
    // decode (the recolor half lives in build_presentation; audio in load_sound).
    bool load_assets(SDL_Renderer* ren, const std::filesystem::path& game);
    // Base tuning + per-player recolor (the "Loading data..." bar's second
    // half) + Renderer/Screen construction.
    void build_presentation(SDL_Renderer* ren);
    // The "Loading sound..." dialog + audio_.init(), run last so all sprite
    // data is decoded AND recolored first (sub_41D695 before sub_42896E).
    void load_sound(const std::filesystem::path& game);
    // Pump the OS event queue (so the window stays responsive) then repaint the
    // boot LOADING dialog at `fraction` (0..1). The progress callback threaded
    // through assets_.load()/build_player_sets()/audio_.init() calls this.
    void draw_boot_loading(const char* caption, float fraction);
    // PORT ENHANCEMENT (not RE'd — the original has no fullscreen concept):
    // the Alt+Enter/F11 fullscreen toggle, wired as a global SDL_EventFilter
    // (installed once in init()) so it works from every one of this file's
    // per-screen SDL_PollEvent loops without touching each of them. Returns
    // false (swallow) for the toggle keys, true (keep) for everything else —
    // matches SDL_EventFilter's contract, called via the static thunk below
    // since SDL needs a plain function pointer + void* userdata.
    bool handle_global_event(const SDL_Event& ev);
    static bool SDLCALL sdl_event_filter(void* userdata, SDL_Event* event);
    // Flips fullscreen_, applies it to the live window, and marks the choice
    // for persistence (options_dirty_ — flush_options() is the sole writer).
    void toggle_fullscreen();
    // Presentation-only global shortcut: Tab selects optional DATA_HD artwork
    // without changing the fixed gameplay coordinate system or simulation.
    void toggle_hd_artwork();
    // Write-on-exit (task requirement 3 / §2): serializes every in-memory
    // option this session has touched back to options.ini, ONLY if something
    // actually changed since load (options_dirty_) and a game_dir is known.
    // Idempotent — safe to call more than once (run() and the destructor both
    // do, in case a subclass/test skips run()'s normal return path). Also
    // flushes nodename.ini (see flush_node_name), so run()'s five exit paths
    // keep ONE settings-writeback call.
    void flush_options();
    // The node name's own write-on-exit hook: the original persists it through
    // a SEPARATE shutdown callback (sub_40C4DB -> sub_40C140) into its own
    // install-root nodename.ini, not through options.ini's writer
    // (docs/re/network-screens.md §3 "Session model").
    void flush_node_name();
    // The absent-NODENAME.INI fallback (sub_40C74C): a random one of the 49
    // names at MESSAGES ids 500..548, `getstring(500 + rand() % getvalue(47))`.
    // Needs the message table, so it runs after load_assets(), not in
    // load_config() where the file itself is read.
    void seed_default_node_name();
    void start_match(std::uint32_t seed);
    int run_demo();
    // Netplay entry (increment 5b, ADR-0010 §3.3 step 5): runs ONE 2-player UDP
    // lockstep match in place of the front-end when opts_.net_role != 0. Uses the
    // CANONICAL MatchConfig (canonical_netplay_config below) so both peers seed
    // sim_ byte-identically with no setup traffic at all, then drives the SAME
    // MatchRunner::run() through a net::RollbackSession (seam net_session).
    // Returns a process exit code. GOLDEN-SAFE: only reachable via net_role, which
    // no test/golden/demo path sets. The CLI path carries --seed on both peers,
    // so it skips the handshake and calls run_netplay_match() directly.
    int run_netplay();
    // The CLI's config: byte-identical on both peers from the shared seed alone —
    // the default scheme_ + the install VALUELST, ignoring every per-machine
    // options_/selected_level_/team_play_/gold overlay, 2 humans in seats 0/1 and
    // the stage picked from the seed. It exists ONLY for `--host`/`--join`, which
    // are scripted, non-interactive entries (ADR-0010 §3.3: no discovery, no
    // handshake, both peers pass --seed on the command line) with no second
    // machine to drive a setup screen. Every INTERACTIVE path — the lobby rows and
    // the direct HOST LAN GAME / JOIN BY IP rows — runs present_net_setup instead
    // and agrees a real config, which is the whole point of the setup stage.
    sim::MatchConfig canonical_netplay_config(std::uint32_t seed) const;
    // The match-running CORE shared by the CLI (run_netplay) and the menu connect
    // screens: given an ALREADY-connected transport, this peer's `role` (1 =
    // host/seat 0, 2 = guest/seat 1), and the agreed `seed`, it runs the canonical
    // config through run_netplay_match_seats. CLI-only now that the direct-IP rows
    // agree a config through present_net_setup.
    AppInput run_netplay_match(net::UdpTransport& transport, int role, std::uint32_t seed);
    // The REAL core, taking the seat ownership as an explicit MASK rather than
    // deriving it from a role (ADR-0011 Phase 1d): the online lobby's server hands
    // each peer an AUTHORITATIVE local_seats_mask in its StartMatch (design §1.6),
    // so the GUI passes that straight through instead of assuming host==seat 0.
    //
    // `cfg` is THE agreed MatchConfig and is used verbatim — the whole board, the
    // roster, the stage index, the tuning and the seed. On the host it is what
    // present_net_setup confirmed; on the guest it is SetupSession::final_config(),
    // i.e. the host's exact bytes (match_config_codec.hpp). It replaces the
    // hard-coded canonical config this used to build, which was a determinism
    // shortcut that cost online play its map choice, its AI slots and its roster.
    //
    // `all_seats` is EVERY network seat in the match, not a literal 0b11: over a
    // star (ADR-0011 decisions 2+4) `transport` fans out to every guest and
    // reflects between them, so the RollbackSession — which already accepts
    // arbitrary masks — carries as many peers as the lobby seated. The online
    // path takes it from the server's seat_assign; the CLI/LAN pairs pass 0b11.
    // AI slots are NOT in the mask: they are simulated identically everywhere
    // from the shared config and their input never crosses the wire.
    // `is_host` gates the peer-drop handoff: only the hub may schedule a silent
    // seat's move to the AI (net::DropPolicy), since a guest must never mutate
    // the hashed State on its own authority.
    //
    // A MATCH, NOT A ROUND. `cfg` seeds round 0; a round that ends without a
    // clinch runs the outcome screen with a between-rounds gate over it
    // (screens/net_round_gate.hpp) and starts the next round on the config the
    // HOST confirms through that gate — the same `sub_42A3F6` best-of-N loop the
    // local Play flow runs, with the host driving the advance exactly as the
    // original's network client does (docs/re/in-match-shell.md "The round-end
    // shell"). Every round's seed and tick base come from net::round_rotation.hpp,
    // so both peers agree on which round they are in with no extra traffic.
    // Returns Advance once the match is decided/abandoned, Quit on a window close.
    //
    // `round_base` is an IN/OUT round counter that survives across MATCHES played
    // over one transport (see run_netplay_session): every round's tick space is
    // net::round_tick_base(*round_base + round), so match 2's round 0 cannot land
    // in the tick space match 1 was still sending into. nullptr = start at 0 and
    // report nothing, which is every single-match caller.
    // `rematch` is set true when the match was DECIDED and both peers agreed
    // (net::RematchSession) to walk back to the setup screens over the same
    // transport instead of tearing it down. nullptr = the caller does not offer a
    // rematch, and the transport is dropped as before.
    AppInput run_netplay_match_seats(net::Transport& transport, std::uint16_t local_seats,
                                     std::uint16_t all_seats, bool is_host,
                                     const sim::MatchConfig& cfg, int* round_base = nullptr,
                                     bool* rematch = nullptr);
    // A whole NETPLAY SESSION over one connected transport: setup -> match ->
    // setup -> match -> ... The connect step (lobby punch or direct handshake)
    // happens once, and finishing a match returns BOTH peers to the roster/map
    // screens with the link intact, which is the entire point — the transport used
    // to die with the first match, so a rematch meant re-punching through the
    // lobby, and by then the matchmaker has reaped the room anyway (it drops a
    // lobby ~30 s into a match). Nothing below this line needs the control plane.
    //
    // `cfg` is round 0 of the FIRST match, already agreed by the caller's own
    // present_net_setup. Later matches agree their own through this loop. Returns
    // exactly what run_netplay_match_seats/present_net_setup last returned.
    AppInput run_netplay_session(net::Transport& transport, std::uint16_t local_seats,
                                 std::uint16_t all_seats, bool is_host, std::uint32_t seed,
                                 const sim::MatchConfig& cfg, ChatOverlay* chat = nullptr);
    // THE ONLINE SETUP STAGE (docs/re/network-screens.md §7, ADR-0011): runs
    // between the connect step (lobby punch or direct seed handshake) and the
    // match, over the SAME transport, so an online game finally gets the real
    // roster/AI and map screens instead of a hard-coded config.
    //
    // HOST — drives the ordinary SetupScreen + MapSelectScreen (the very screens
    // menu row 0 uses), publishing a preview after each edit; on Enter it builds
    // the config through MatchRunner::build_config — the SAME build the local
    // start_match path does from the SAME screens — and confirms it.
    // GUEST — renders those same two screens READ-ONLY from the preview, buzzing
    // SFX 40 at any edit key, and adopts the confirmed config.
    //
    // Returns Advance with `out_cfg` filled (Phase::Final — EVERY peer holds it),
    // Back if the stage was left/timed out (the reason is already shown on the
    // acknowledge modal), or Quit on a window close. STOPS pumping the setup
    // session before returning: the match session drains the same transport and
    // whichever polls first eats the datagram (setup_session.hpp's one
    // obligation).
    //
    // `chat` is the lobby-chat overlay (PORT-ONLY, chat_overlay.hpp) composited
    // over both screens and pumped by them, so the conversation started in the
    // waiting room carries on here. nullptr on the direct/LAN paths, which have
    // no matchmaker connection to chat over.
    // `all_seats` is every network seat, as in run_netplay_match_seats: the host
    // waits for an ack from EACH of the others before Phase::Final, so a
    // >2-peer star cannot start the match while somebody is still reassembling
    // the config (setup_session.hpp).
    AppInput present_net_setup(net::Transport& transport, bool is_host,
                               std::uint16_t local_seats, std::uint16_t all_seats,
                               std::uint32_t seed, sim::MatchConfig& out_cfg,
                               ChatOverlay* chat = nullptr);
    // Menu row 1 (START NET GAME) now opens the NETWORK GAME menu (LobbyScreen):
    // the online lobby entry points plus the ADR-0010 direct/LAN rows. Menu row 2
    // (JOIN NET GAME -> present_net_join) stays the UNCHANGED direct-IP join, so
    // the no-server path keeps working exactly as it did.
    AppInput present_net_host();
    AppInput present_net_join();
    // The ADR-0010 direct-UDP host (bind kNetDefaultPort + the seed handshake) —
    // the old present_net_host body, now reached from the NETWORK GAME menu's
    // HOST LAN GAME row.
    AppInput present_net_direct_host();
    // The online lobby leaf (ADR-0011 Phase 1d): resolve the matchmaker URL, bind
    // the one socket LobbyFlow reuses for STUN/punch/match, run the waiting room,
    // and on Phase::Ready run the match with the SERVER's seed + seat mask.
    // `browse` (Phase 3) only changes where a GUEST's lobby code comes from — the
    // PUBLIC GAMES browser instead of the typed-code prompt; the waiting room and
    // the match hand-off below it are the same code either way.
    //
    // DECLARED unconditionally but DEFINED only under BOMBER_HAS_LOBBY: that
    // define is PUBLIC on bomber::net, which libs/game links PRIVATEly, so it is
    // visible while compiling bomber_game_core but NOT to apps/game including
    // this header — guarding the declarations would give GameApp two different
    // definitions across translation units (an ODR violation). Nothing outside
    // the guarded call site in game_app.cpp references these, so a lobby-off
    // build simply never emits or needs them.
    AppInput present_net_online(bool host, bool browse = false, bool is_public = false);
    // Matchmaker endpoint resolution, in the documented precedence order:
    // --matchmaker / --matchmaker-stun CLI flags, then BOMBER_MATCHMAKER_URL /
    // BOMBER_MATCHMAKER_STUN_HOST / BOMBER_MATCHMAKER_STUN_PORT, then the
    // compile-time placeholder constants in game_app.cpp.
    std::string matchmaker_url() const;
    std::string matchmaker_stun_host() const;
    std::uint16_t matchmaker_stun_port() const;
    // --bm-shot capture: draw one `.BM` screen over MAINMENU and SaveBMP it.
    int run_bm_shot();
    int run_menu_shot();
    // Reads back the current backbuffer and writes it as a BMP. Shared by
    // run_demo()'s legacy single-shot path and its --demo-shots multi-shot
    // path. Returns false (and leaves stderr diagnostics to the caller) on
    // an SDL failure.
    bool save_screenshot(const std::filesystem::path& out) const;

    // The front-end screen/state-machine shell (docs/adr/0004): drives
    // Boot -> Logo -> Title -> Menu -> Match -> Results -> Menu around the
    // existing match loop. The pure transition graph lives in app_flow.hpp; the
    // methods below are the thin SDL side (render, audio, input) per state.
    int run_app();
    // Builds a fresh ScreenContext (the shared-services bundle) from this app's
    // stable members, so an extracted screen class can run without threading
    // GameApp's whole member set (ADR-0008 god-object decomposition). Cheap —
    // a value bundle of references/pointers; valid any time after init().
    ScreenContext sctx();
    // The Options cluster's shared-state seam (ADR-0009 §4): a by-reference
    // bundle of the non-service members present_options_screen and its two
    // sub-screen runners read/write (options_/scheme_/setup_lcg_/... — see
    // options_state.hpp). Built fresh on demand like sctx(), so the runner
    // classes need no GameApp&.
    OptionsEditState options_state();
    // The main menu's shared-state seam (ADR-0009 §6): the non-service members
    // present_menu + roll_attract_match read/write (menu_index_/the attract
    // roster+level+LCG/the F10 video toggles/the four editor members — see
    // menu_state.hpp), bundled by reference so MenuScreen needs no GameApp&.
    // Built fresh on demand like sctx()/options_state()/editor_state().
    MenuState menu_state();
    // The LEVEL & ROUNDS screen's shared-state seam (ADR-0009 §7): the
    // non-service members present_map_select reads/writes (selected_level_/
    // win_target_/setup_lcg_/options_/gold_player_ — see map_select_state.hpp),
    // bundled by reference so MapSelectScreen needs no GameApp&. Built fresh on
    // demand like sctx()/menu_state().
    MapSelectState map_select_state();
    // The PLAYER INPUT screen's shared-state seam (ADR-0009 §7): the non-service
    // members present_setup + cycle_input_type read/write (the setup_type_/sub_/
    // team_ roster/setup_lcg_/campaign_trigger_count_/team_play_/gold_player_ + the
    // campaign_active_/stages_/stage_index_ Esc tears down — see setup_state.hpp),
    // bundled by reference so SetupScreen needs no GameApp&. Built fresh on demand
    // like sctx()/map_select_state().
    SetupState setup_state();
    // The match-coupled backdrop seam (ADR-0009 §8): the live Renderer + sim
    // State the campaign confirm/banner/complete dialogs and the in-round help
    // modal draw as their frozen backdrop — kept SEPARATE from the front-end-
    // service-only ScreenContext (match-runtime members, not presentation
    // services). Built fresh on demand like sctx(); NOLINTs the renderer_
    // optional deref exactly as sctx() does *screen_.
    MatchBackdrop match_backdrop();
    // The campaign flow's shared-state seam (ADR-0009 §8): the non-service
    // members present_campaign_picker + load_campaign_stage read/write
    // (campaign_active_/campaign_stages_/campaign_stage_index_/campaign_banner_/
    // the setup_type_/sub_/team_ roster/setup_lcg_/scheme_/opts_.game_dir — see
    // campaign_state.hpp), bundled by reference so CampaignPickerScreen and the
    // free load_campaign_stage need no GameApp&. Built fresh on demand like
    // sctx()/map_select_state().
    CampaignState campaign_state();
    // The RESULTS scoreboard's shared-state seam (ADR-0009 §8): the non-service
    // members present_scoreboard reads (the frozen round's sim::State, the
    // win/kill tally + roster + options it rows, and the demo/roster flags
    // auto_advance_results() consults — see results_state.hpp), bundled so
    // ScoreboardScreen needs no GameApp&. Built fresh on demand like sctx()/
    // campaign_state().
    ScoreboardState scoreboard_state();
    // The Goldman wheel's shared-state seam (ADR-0009 §8): the three non-service
    // members present_goldman_wheel writes (goldman_lcg_/gold_player_/gold_prize_
    // — see results_state.hpp), bundled by reference so GoldmanWheelScreen needs
    // no GameApp&. Built fresh on demand like sctx()/scoreboard_state().
    GoldmanState goldman_state();
    // The match runtime's shared-state seam (ADR-0009 §10): the non-service
    // members run_match + start_match + collect_inputs + draw_player_row +
    // draw_fps_overlay read/write (the ticked sim_/renderer_, the round seed, the
    // kill tally, the F7/F8/F9 live levers, and the read-only MatchConfig inputs —
    // see match_runner_state.hpp), bundled by reference so MatchRunner needs no
    // GameApp&. Built fresh on demand like sctx()/goldman_state().
    MatchRunnerState match_runner_state();
    // Runs one asset-driven Screen (logo/title/results) to completion. sub_42A088
    // CUTS between screens (palette + blit + flip, no wipe), so there is no
    // transition out here — the next screen simply replaces this one. Returns the
    // AppInput that ended it (Advance on key/timeout, Back on Escape, Quit on
    // window close).
    AppInput present_screen(const ScreenDef& def);
    // Front-end frame pacing now lives in bomber::platform::FrameClock
    // (engine-base layer, ADR-0008); the menu/setup/Goldman loops own one and
    // call pace(). run_match keeps its own inline pacer (coupled to the F9
    // cadence toggle) until a later stage migrates it too.
    // Runs a `.BM` text-screen (Credits / Options / Network / Controllers help)
    // to completion via the BmScreen viewer: draws MAINMENU as the backdrop with
    // the parsed .BM text+images over it, scrolls on the arrow/page keys, and
    // exits on Enter/Escape (sub_41302D). Returns Back on Escape else Advance
    // (both route the leaf back to the menu), or Quit on window close.
    AppInput present_bm_screen(const std::string& bm_name);
    // The interactive Options screen (Team Play / Conveyor Speed): random
    // GLUE<n> backdrop, FONT6 text, Up/Down select a row, Left/Right change
    // its value, Enter/Esc leave (docs/re/frontend-flow.md "Interactive
    // settings ... DEFERRED" — this is that follow-up). Persists to
    // options.ini via bomber::assets::save_options only when a setting
    // actually changed. F1 opens the generic *.BM help browser
    // (present_help_browser) — CORRECTED 2026-07-08: sub_4080DC's own F1
    // dispatch calls sub_41431C (§4), the SAME browser row 5 opens, not a
    // fixed OPTIONS.BM cut. Returns Advance (both Enter/Esc route the leaf
    // back to the menu, mirroring the other .BM-backed leaves) or Quit on
    // window close.
    AppInput present_options_screen();
    // Case-insensitive DATA/SCHEMES/<name>.SCH resolve (name given with or
    // without an extension) + assets::sch::load into scheme_. Returns false
    // (scheme_ untouched) when the name doesn't resolve or the file is
    // corrupt.
    bool reload_scheme_from_name(const std::string& name);
    // load_campaign_stage moved out of GameApp into a free function in
    // screens/campaign_state.hpp (ADR-0009 §8): it is called by BOTH
    // present_campaign_picker (now CampaignPickerScreen) AND run_app's Results
    // auto-advance handler, so it takes a CampaignState (built by
    // campaign_state()) instead of being a private method only one of them
    // could reach. present_campaign_confirm likewise moved to
    // CampaignConfirmScreen (screens/campaign_screens.hpp) — only the picker
    // called it, so no GameApp forwarder remains.
    // The campaign stage-start banner (docs/re/campaign.md "Stage banner"):
    // a blocking two-line dialog, "(<stage name>)" (getstring 1235="(%s)")
    // over "Prepare to begin Campaign!" (getstring 1230), shown once per
    // stage transition (both the first stage, from present_campaign_picker,
    // and every auto-advance in run_app's Results handler). Dismissed by any
    // key or a short dwell; presentation-only — a separate sub_414340 call
    // from present_campaign_confirm's above (different getstring ids,
    // different content), but the same dialog FAMILY.
    AppInput present_campaign_banner();
    // The "Congratulations! You made it through the whole campaign!" acknowledge
    // modal (sub_40133F stage-exhausted branch, getstring 1220/1225) shown once
    // the last campaign stage is cleared, before returning to the menu.
    AppInput present_campaign_complete();
    // The IPLOGO -> HSLOGO -> TITLE boot presentation (sub_42B060). LINEAR — no
    // attract re-run: each screen advances on a key OR the getvalue(12) = 7 s
    // timeout, and the title's Advance (key or timeout) returns so run_app drops
    // into the menu (sub_42B060 synthesizes Enter on timeout and returns; the
    // caller enters sub_42B9CE). Returns Advance to enter the menu, or Back/Quit
    // to short-circuit.
    AppInput run_boot_attract();
    // The navigable main menu (sub_42B9CE): MAINMENU.PCX + an up/down highlight
    // over the item rows, Enter selects, Escape quits. Resolves the highlighted
    // row into a concrete AppInput (StartMatch / OpenOptions / ... / Quit).
    //
    // ALSO owns the ATTRACT-MODE idle timer (docs/re/frontend-flow.md "Attract
    // mode", sub_42B9CE's idle path pseudo.c 30887-30894): getvalue(92) = 30 s
    // (gated > 5, per the file's own legend — < 5 disables attract) of NO
    // key/mouse/pad input resets `menu_idle_since_ms_`'s deadline; hitting it
    // sets attract_, calls roll_attract_match() (the sub_4224E2 save + the
    // roster/stage rolls), and returns StartMatch exactly as if row 0 (Play)
    // had been selected — matching the original's own force of the selected
    // row back to 0. This is
    // presentation-level gating around the EXISTING Menu->StartMatch edge in
    // app_flow.hpp; no new AppState/AppInput was needed (task brief: prefer
    // the existing StartMatch edge). run_app's StartMatch handler checks
    // attract_ and skips the goldman wheel / present_setup / present_map_select
    // (doc: "neither the player screen nor the LEVEL & ROUNDS screen is
    // shown"), going straight into run_match with the rolled roster/stage.
    AppInput present_menu();
    // roll_attract_match (the sub_4224E2 attract entry — the sub_422552 save +
    // the roster/stage rolls) moved into MenuScreen (screens/menu_screen.hpp)
    // with present_menu; it was only ever called by present_menu's idle/Alt+A
    // branches. restore_from_attract stays here because run_app calls it.
    // Attract-mode exit (sub_422552, doc "Menu re-entry restores everything"):
    // writes attract_saved_ back over setup_type_/setup_sub_/setup_team_/
    // selected_level_/team_play_ and clears attract_. Called on EVERY path
    // back to the menu after an attract match — both a natural round end
    // (doc point 2's "Round end skips ALL outcome screens": DRAW/RESULTS/
    // VICTORY never render, so run_app's Results branch is bypassed entirely
    // for an attract round) and an input-triggered abort (doc point 3: "ANY
    // key/mouse/pad input ... aborts immediately back to the menu", run_match
    // below). Idempotent no-op if attract_ is already false.
    void restore_from_attract();
    // Runs one match to its end (one player left or time up). Returns Quit if
    // the window closed mid-match, else MatchOver.
    //
    // In attract_ mode this ALSO returns MatchOver the instant ANY key,
    // mouse-button, or gamepad-button input arrives (docs/re/frontend-flow.md
    // "Attract mode" point 3 / doc's abort requirement, mirroring sub_42A3F6's
    // round-loop tail, which jumps to LABEL_34 as soon as a keypress has set
    // dword_464938) — a
    // real (non-attract) match only reacts to the specific keys already wired
    // above (Ctrl+Q, Esc, F1), so this abort check is additive and attract_-
    // gated, never firing for a human-played round. run_app's StartMatch
    // caller calls restore_from_attract() unconditionally once this returns,
    // whether the round ended naturally or was aborted (doc point 2 "Round
    // end skips ALL outcome screens" applies to BOTH exits — attract never
    // reaches Results).
    AppInput run_match();

    // The match-outcome predicates below are thin forwarders to the free
    // functions of the same names in bomber/game/match_outcome.hpp, where they
    // were promoted VERBATIM (ADR-0009 §10) so the extracted ScoreboardScreen and
    // MatchRunner can call the SAME clinch/outcome logic run_app uses without a
    // GameApp&. Kept as methods for run_app, this file's last remaining caller;
    // the full RE citations live on the free functions in that header.
    //
    // The winner of the round just ended (sole survivor index, or -1 for a draw).
    int round_winner() const;
    // Campaign clauses 4-5: no human/joystick player survives this round.
    bool campaign_no_human_survivor() const;
    // At least two ACTIVE players share a MatchConfig team (dword_464964).
    bool is_team_mode() const;
    // The §1 v73 match-clinch check; the clinching player's index, or -1.
    int match_clinch() const;
    // Tally the round win, mirroring it onto the winner's teammates (sub_421B56).
    void award_round_win(int winner);
    // The DRAW/RESULTS screens may auto-advance after their dwell (all-AI/demo).
    bool auto_advance_results() const;
    // Reset the per-match win tally + read the win target getvalue(310) at the
    // start of a fresh match (Menu -> StartMatch). Best-of-N, N = 2 by default.
    void reset_match_scores();
    // The between-round RESULTS scoreboard (sub_42A3F6): RESULTS.PCX + the
    // running per-player win counts at the getvalue(785) list positions. Shown
    // after a round that did not end the match; returns the dismiss input.
    AppInput present_scoreboard();
    // Screen 1 of the pre-match flow — PLAYER INPUT TYPE SELECTION (sub_410F81):
    // the 10-slot input-type list (OFF / COMPUTER / KEYBOARD) at getvalue 705-713,
    // each slot tinted with its intrinsic colour (VALUELST 200-247), a per-slot
    // team flag ('T'). Right cycles a slot's type, Left/'0' set it OFF. Returns
    // Advance to go on to the level screen, Back to cancel to the menu, Quit on
    // window close. (docs/re/setup-screens.md.)
    AppInput present_setup();
    // The Goldman Roulette wheel (docs/re/goldman-roulette.md), sub_4034BC:
    // run at the head of the Play flow, before present_setup(), whenever
    // goldman is on, we're not in attract, it's a local game, AND a gold
    // player is pending (gold_player_ >= 0, doc §2's re-entry gate — the
    // wheel is a silent no-op with no pending winner). Awards +1 born-with
    // inventory (MatchConfig::born_with_extra, doc §4) to the gold player
    // (whole team in team mode) at every subsequent round init for the
    // following match. Returns Advance to continue into present_setup, Back
    // if Esc aborted the wheel (the caller must then skip the whole Play
    // flow and forfeit the gold player, doc §2/§5), Quit on window close.
    AppInput present_goldman_wheel();
    // Screen 2 — LEVEL & ROUNDS (sub_406DDE, the VALUELST "OPTIONS SCREEN"): the
    // RANDOM + 11 named levels and the win target, at getvalue 735-738. Left/Right
    // cycle the highlighted row, Up/Down switch rows, Enter commits the level
    // (selected_level_) + win target (win_target_), Escape backs to present_setup.
    // Returns Advance to start the match, Back to the player screen, Quit on close.
    AppInput present_map_select();

    int menu_index_ = 0;  // highlighted main-menu row (persists across visits)
    // The hidden scheme-editor trigger's same-key repeat counter (§5,
    // sub_42B9CE pseudo.c 30876-30883): raw key code 5 (Ctrl+E) increments
    // it; ANY OTHER key resets it to 0; `++counter > 5` (the 6th consecutive
    // press) opens the editor. Lives here (not a local in present_menu)
    // because it must persist across that function's per-frame event pump.
    int editor_trigger_count_ = 0;
    // The hidden campaign picker's same-key repeat counter (docs/re/
    // campaign.md §4, sub_410F81 pseudo.c 15357-15365): raw key 'C' (0x43)
    // increments it; ANY OTHER key resets it to 0; the 5th CONSECUTIVE press
    // (`== 5`, not editor_trigger_count_'s `> 5` — the doc pins "5 consecutive
    // 'C'", not a 6th) opens the campaign picker. Lives here for the same
    // reason editor_trigger_count_ does: it must persist across
    // present_setup's per-frame event pump. The original also gates this on
    // "not net mode" (sub_40C06A()); this port has no netplay (ADR-0003
    // defers it), so that guard is always-true here and simply omitted.
    int campaign_trigger_count_ = 0;

    // Multi-round match state (sub_42A3F6): best-of-N. win_count_ tallies round
    // wins per player; reaching win_target_ ends the MATCH (VICTORY). A draw
    // scores nobody and replays. Presentation-only state — never sim::State,
    // never hashed. win_target_ is seeded from options.ini's "num_to_win_match="
    // (docs/re/results-and-options.md §3/§5) when present, else getvalue(310),
    // by reset_match_scores(), and then owned by the LEVEL & ROUNDS screen
    // (present_map_select, WINS row 1..100, docs/re/setup-screens.md); the
    // in-class 2 only covers the dev fast-path (--match / BOMBER_BOOT_MATCH),
    // which skips the pre-match screens entirely. A round that does not decide
    // the match routes Results -> Match via AppInput::RoundContinue through the
    // pure flow graph (app_flow.hpp) — run_app folds the scoreboard/draw
    // dismissal into that event; there is no side-channel state override.
    std::array<int, sim::kMaxPlayers> win_count_{};
    int win_target_ = 2;
    // Kill tally (docs/re/results-and-options.md §1, sub_421B0F's field):
    // the RESULTS row shows this alongside the match win count. §1's
    // "Reproduction status" paragraph is explicit that this counter, like the
    // win count, is "carried across rounds within one match" — i.e. despite
    // being called the "round-kill count", it is CUMULATIVE for the whole
    // match (packed in the same per-player 152-byte record as the win count),
    // NOT reset every round. So this resets only in reset_match_scores() (a
    // fresh match), exactly like win_count_. Tallied from the sim's
    // PlayerDied events (Event::data = killer index, event.hpp) once per tick
    // in run_match via results.hpp's tally_kills() — self-kills are excluded
    // (our semantics; §1 does not pin this — see results.hpp's doc comment).
    std::array<int, sim::kMaxPlayers> kill_count_{};
    // options.ini "num_to_win_match=" (§3/§5), read once in init(). Seeds
    // reset_match_scores()'s win_target_ default when getvalue(310) is
    // absent; the LEVEL & ROUNDS screen's WINS row still overrides per-match.
    std::optional<int> num_to_win_match_;

    // Per-slot input type chosen in the PLAYER INPUT screen (sub_410F81):
    // 0 = OFF, 1 = COMPUTER, 2 = KEYBOARD, 3 = JOYSTICK (human) — the original's
    // player byte +16 (docs/re/setup-screens.md). Default: P1 keyboard + P2
    // computer. SlotInputType (input.hpp) names these.
    std::array<int, sim::kMaxPlayers> setup_type_{2, 1};
    // Per-slot input SUB-index (the original's +17): for KEYBOARD, which key-set
    // (0 or 1, both bound to the single physical KeyboardMapper); for JOYSTICK,
    // which CONNECTED gamepad index (GamepadMapper::read(sub)).
    std::array<int, sim::kMaxPlayers> setup_sub_{};
    // Per-slot TEAM (the original's +84, toggled by 'T'): 0 or 1. Fed into the
    // config's non-hashed MatchConfig::team[]; team MODE itself is deferred.
    // Defaulted here to all-0 (matches "every slot OFF" at construction /
    // campaign roster reset); present_setup() re-derives the REAL default
    // (alternating slot & 1, sub_4049C0) on every entry to the setup screen
    // — see that function's comment.
    std::array<int, sim::kMaxPlayers> setup_team_{};
    // The level chosen on the LEVEL screen (sub_406DDE dword_45E0B8/464998):
    // -1 = RANDOM (keep pick_stage over the enabled rotation), else 0..10 = a
    // specific built-in level whose stage index start_match uses directly.
    int selected_level_ = -1;
    // Presentation RNG for the glue pick (and the LEVEL & ROUNDS preview
    // swatch's per-cell tile re-roll). The literal below is only a
    // construction-time placeholder: GameApp::init() overwrites it (and the
    // three sibling LCGs in this file) with a real per-process seed from
    // random_boot_seed() (game_app.cpp), matching the original's boot-time
    // `time_(); srand_();` (sub_41095A, pseudo.c 14610-14611/14639-14640 —
    // the same wall-clock reseed docs/re/facts.md "Per-match brick fill"
    // already cites). A hardcoded literal here would replay the exact same
    // "random" sequence on every launch; `next_seed_` below has the same
    // shape and feeds `match::pick_stage`, so leaving it constant was the
    // root cause of the reported "RANDOM level always picks the same map"
    // bug. Presentation-only: never bomber::sim::State::rng.
    std::uint32_t setup_lcg_ = 0x5E7C0DE5u;

    // ATTRACT MODE (docs/re/frontend-flow.md "Attract mode", sub_42B9CE's idle
    // path + sub_410F81's attract branch, dword_464938). Presentation/config-
    // only, like campaign_active_ below — never sim::State, never hashed; the
    // sim runs the demo match through the ordinary start_match seed path, so
    // determinism (ADR-0003) is untouched.
    //
    // `menu_idle_since_ms_` is present_menu's own idle clock, reset to "now"
    // on every real key/mouse/pad event it sees — separate from a Screen's
    // getvalue(12)=7s dwell (this is getvalue(92)=30s, a different id/timer).
    // present_menu compares elapsed time against it every frame and fires
    // attract once it exceeds getvalue(92)*1000 ms (gated > 5 s, doc: "< 5
    // disables attract"). 0 is a sentinel meaning "not yet initialised for
    // this menu visit" — present_menu seeds it to the current tick on entry.
    std::uint64_t menu_idle_since_ms_ = 0;
    // dword_464938: true for the duration of an attract demo match. Set by
    // roll_attract_match() (present_menu's idle-timeout branch), read by
    // run_app's StartMatch handler (skip the goldman wheel / present_setup /
    // present_map_select / the Results outcome screens) and by run_match
    // (abort on any input). Cleared by restore_from_attract().
    bool attract_ = false;
    // The roster/level/team snapshot roll_attract_match() saves before
    // overwriting them for the demo roster (sub_4224E2), restored by
    // restore_from_attract() (sub_422552) — doc: "Menu re-entry restores
    // everything", so the player's own pre-attract choices survive untouched.
    // The AttractSaved type moved to screens/menu_state.hpp (ADR-0009's
    // AttractState seam) so MenuScreen's roll_attract_match can name it too.
    AttractSaved attract_saved_{};
    // Dedicated presentation LCG (never State::rng) for the two attract rolls
    // (roster-count, stage) — same shape as setup_lcg_/goldman_lcg_. Reseeded
    // per-process by GameApp::init() (random_boot_seed(), see setup_lcg_'s
    // comment above); the literal is only the construction-time placeholder.
    std::uint32_t attract_lcg_ = 0x0A77AC70u;

    // Campaign mode (docs/re/campaign.md, dword_46489C): armed only by the
    // 'C'x5 trigger + a successful *.cam pick on present_setup
    // (present_campaign_picker). Presentation/config-only, like
    // setup_type_/selected_level_ above — never sim::State, never hashed.
    // While active, campaign_stages_[campaign_stage_index_]'s scheme/roster
    // REPLACE the manual setup_type_/selected_level_ values for the
    // duration (the doc's "replacing the normal manual level-pick and
    // roster-pick screens"), and run_app's Menu/Results handlers skip
    // present_map_select() and auto-advance dword_4648B0 between stages
    // instead of returning to the menu.
    bool campaign_active_ = false;                             // dword_46489C
    std::vector<assets::res::CampaignStage> campaign_stages_;  // parsed .CAM (dword_45E010)
    int campaign_stage_index_ = 0;                             // dword_4648B0
    // Stage display banner text, "(<stage name>)" (sub_40133F, getstring
    // 1235="(%s)" — docs/re/campaign.md "Stage banner"), set by
    // load_campaign_stage each time a campaign stage is (re)loaded. Drawn by
    // present_setup for one frame-cycle at stage start alongside getstring
    // 1230="Prepare to begin Campaign!"; empty when campaign mode is off.
    std::string campaign_banner_;

    // The Goldman wheel's pending gold player (dword_46492C, docs/re/goldman-
    // roulette.md §2): -1 = none pending, else a player index (solo) or a
    // RAW 0/1 team id (team mode — our port's team-id space, unlike the
    // original's internal 0/2 encoding; see doc §2) whose match-win, under
    // the goldman option, arms the next Play entry's wheel spin. Default -1
    // (boot init, doc's "Cleared to -1 by ... boot init 14661").
    //
    // ASSIGNMENT (doc §2, pseudo.c 30004-30022): written in run_app's
    // Results case on every SURVIVOR round (w >= 0 — a draw never reaches
    // the original's dword_46492C write and leaves this untouched), from
    // match_clinch()'s v73 — the MATCH-CLINCH winner (win_target_/
    // win_by_kills reached), NOT the per-round winner `w`. In team mode it's
    // setup_team_[clinched], the raw team byte of the clinching player
    // (mirrors present_scoreboard's own clinched_player -> setup_team_[]
    // lookup). Cleared here on the documented events this file owns (Esc on
    // the wheel, Esc on present_setup, the Options-screen Gold Bomberman
    // toggle).
    int gold_player_ = -1;
    // The prize awarded by the last successful (non-aborted) wheel spin, or
    // -1 (doc §4: "dword_45E02C is never reset on consumption"). Consumed by
    // start_match() into MatchConfig::born_with_extra every round while
    // gold_player_ stays the same match's winner.
    int gold_prize_ = -1;
    // Presentation RNG seed for the wheel's 5 draws; reseeded per-process by
    // GameApp::init() (random_boot_seed(), see setup_lcg_'s comment above).
    std::uint32_t goldman_lcg_ = 0x60D1BEEFu;

    Options opts_;

    assets::sch::Scheme scheme_;
    assets::res::ValueList values_;
    sim::Tuning base_tuning_;  // VALUELST-applied (colors, stage rotation, taunts)

    // The full options.ini snapshot this session is editing in memory
    // (docs/re/results-and-options.md §2/§3). Loaded once in init(); every
    // Options-screen row edits `options_` (via OptionsSnapshot round-trips in
    // present_options_screen) and sets options_dirty_ rather than writing the
    // file — flush_options() (run()'s tail / the destructor) is the ONLY
    // writer, matching the confirmed exit-time write-back semantics.
    OptionsSnapshot options_{};
    bool options_dirty_ = false;
    // The Conveyor Speed game-option index actually applied to a fresh match's
    // Tuning (dword_464930). Mirrors options_.conveyor_speed_index once
    // loaded/edited; kept as a separate optional so "never set" (no
    // options.ini key, ever) still falls back to Tuning's own confirmed
    // default (1 = medium) rather than OptionsSnapshot's arbitrary default.
    std::optional<int> conveyor_speed_index_;
    // Team Play toggle (dword_464964). Mirrors options_.team_play; the
    // game-type-level GATE, separate from each slot's own setup_team_[]
    // (+84) byte. start_match() zeroes every slot's MatchConfig::team[] when
    // this is false, regardless of what setup_team_[] holds (there is no
    // separate MatchConfig::team_play field — team[]'s all-zero/non-zero
    // state IS the hashed Player::team gate, docs/re/setup-screens.md
    // "Roster/level -> match"). is_team_mode() also gates on this directly.
    bool team_play_ = false;
    // The install-root options.ini path resolved in init(), used only by
    // flush_options() (the write-on-exit hook, §2). Empty when no game_dir
    // was resolvable (init() already failed in that case).
    std::filesystem::path options_path_;
    // The install-root nodename.ini path (the net identity's OWN file — it is
    // not one of options.ini's 22 keys), plus the value as it was read at boot
    // so flush_node_name() can skip a rewrite that would change nothing.
    std::filesystem::path node_name_path_;
    std::string node_name_loaded_;
    // PORT ENHANCEMENT — "fullscreen=" (see init()'s window-creation comment
    // and toggle_fullscreen()): not one of the original's confirmed 22
    // options.ini keys, since the 1997 binary has no fullscreen mode at all.
    // Loaded once in init(), applied to the window there, flipped by
    // Alt+Enter/F11 (handle_global_event), persisted by flush_options().
    bool fullscreen_ = false;
    // PORT ENHANCEMENT — F8 "uncapped framerate" toggle. Default OFF keeps the
    // vsync-locked, refresh-boundary-paced 60 fps render (smooth, tear-free).
    // ON drops vsync and paces to the sim's SUB-FRAME rate instead
    // (tick_ns / kSubFrames ≈ 5.56 ms → ~180 fps), so every one of the 9
    // canonical sub-frames player_interp exposes reaches the screen instead of
    // only the ~3 a 60 Hz cadence samples. That reproduces the original's
    // uncapped ~184 fps free-run (docs "Canonical frame cadence"): the AI's
    // per-frame direction whims and the creamy motion the 60 fps blend smooths
    // away both come back. Tears on a ≤60 Hz panel exactly as the 1997 build
    // does; clean on a high-refresh display. Not an RE'd behaviour (the binary
    // has no such toggle), so it lives outside any sub_XXXX path. Not persisted
    // — a live A/B lever, reset to OFF each launch.
    bool uncap_fps_ = false;
    // PORT ENHANCEMENT / SPIKE — F9 "native cadence" toggle. Default OFF keeps
    // the deterministic fixed 20 Hz sim + inter-tick interpolation. ON drives
    // the sim through Simulation::frame() once per DISPLAYED frame with the
    // measured wall-clock delta (movement/AI at true frame rate, low latency;
    // 50 ms-quantized systems on their own accumulator) and renders it directly
    // (no interp), reproducing the original's per-frame gameplay driver
    // (sub_42A191). NON-DETERMINISTIC — a live A/B feel lever, never committed
    // as default and never on the tests/oracle path. Best combined with F8's
    // uncapped fps so movement actually runs at ~180 Hz. Reset OFF each launch.
    bool native_cadence_ = false;
    // F7 — draw the FPS / cadence indicator (next to the match clock). Default
    // ON while this is a live A/B feature; will be driven by a persisted setting.
    bool show_fps_ = true;
    // PORT ENHANCEMENT — the F10 panel's "SOFT SCALING" row (scale_filter.hpp,
    // "soft_scaling=" in options.ini). OFF = the crisp nearest-neighbour upscale
    // the port has always used; ON = linear, the smoothed look a modern GPU/
    // display scaler gives the original's 640x480 output. Explicitly NOT a
    // fidelity setting — the 1997 build scales nothing — so it defaults OFF and
    // nobody who does not ask for it sees a different picture. Applied through
    // set_scale_filter() (sprites.hpp), which is live: no reload, no restart.
    // PINNED OFF on a capture run (load_config), because it changes every scaled
    // pixel of every tests/visual frame.
    bool soft_scaling_ = false;
    // F3 — the in-match NETPLAY diagnostic panel (screens/net_overlay.hpp).
    //
    // OFF by default and DELIBERATELY NOT PERSISTED: it is never read from or
    // written to options.ini, so there is no path by which a saved value could
    // reach a capture run — strictly stronger than the capture-time pin
    // uncap_fps_/native_cadence_/show_fps_ need in load_config(), and the reason
    // capture_run() says nothing about it. MatchRunner additionally refuses to
    // draw the panel without a live netplay session, which a capture never has.
    bool show_netstats_ = false;

    std::optional<sdl::VideoSubsystem> video_;
    sdl::WindowPtr window_;
    sdl::RendererPtr sdl_renderer_;

    AssetStore assets_;
    SequenceSet seqs_;
    AudioEngine audio_;
    SoundDirector sounds_{audio_};
    std::optional<Renderer> renderer_;
    KeyboardMapper keyboard_;
    // JOYSTICK <n> slots (docs/re/setup-screens.md type==3). Refreshed once
    // after SDL_INIT_GAMEPAD in init() and again on every hotplug event so the
    // setup screen's joystick pane / type-cycle count stays live.
    GamepadMapper gamepads_;

    // Front-end presentation (constructed after assets_ is loaded in init()).
    std::optional<Screen> screen_;
    // The bomber-dude row cursor's blink state (sub_413BD6's global
    // dword_460559/46055D pair — one instance here stands in for the
    // original's single global; the options screen keeps its own, which only
    // de-phases the blink between screens).
    CursorIndicator cursor_blink_;
    FontTextures front_font_;  // FONT6.FON glyph textures for the .BM screens
    // Per-match seed, advanced each round (`start_match(next_seed_++)`) and
    // fed to `match::build_match_config`'s per-candidate brick fill/spawn
    // shuffle AND `match::pick_stage`'s RANDOM level pick. GameApp::init()
    // reseeds this from random_boot_seed() (see setup_lcg_'s comment above);
    // the literal below is only the construction-time placeholder — leaving
    // it fixed was the bug that made a fresh process's first RANDOM level
    // pick (and first match's brick layout) identical on every launch.
    std::uint32_t next_seed_ = 0xB0BB1E5;

    sim::Simulation sim_;
};

}  // namespace bomber::game
