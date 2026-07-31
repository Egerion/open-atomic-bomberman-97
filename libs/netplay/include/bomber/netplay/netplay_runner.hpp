#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <utility>

#include "bomber/game_util/app_flow.hpp"  // AppInput
#include "bomber/netplay/lobby_screen.hpp"
#include "bomber/netplay/netplay_match.hpp"
#include "bomber/netplay/netplay_state.hpp"
#include "bomber/netui/net_setup_link.hpp"
#include "bomber/sim/match_config.hpp"

// NETPLAY ORCHESTRATION — everything between "the player picked a network row"
// and "a match is running over a connected link", extracted out of GameApp
// (ADR-0009, the decomposition the netplay half never got).
//
// GameApp used to CONTAIN this: eleven methods, ~1000 lines, the largest of them
// a 438-line best-of-N match driver taking seven parameters including two
// out-params, sitting in the app shell next to the fullscreen toggle and the
// options writer. None of it is app-shell work. The shell now DISPATCHES here —
// three call sites, one line each — and this class owns the flow:
//
//   run_cli()              --host/--join: bind, exchange the config, one match.
//   present_network_menu() menu row 1: the NETWORK GAME list, then whichever of
//                          the four leaves below it selects.
//   present_direct_join()  menu row 2: the unchanged direct-IP join.
//
// Below those, the shape is a funnel: every leaf ends up at the SAME setup stage
// (present_setup) over whatever transport it managed to build, and then at the
// SAME session loop (run_session) — setup, match, setup, match — until somebody
// stops wanting another one. The match itself lives in netplay_match.hpp.
//
// Two seams, stored BY VALUE (cheap reference bundles): NetplaySeams (the
// per-screen state builders GameApp already owned) + NetplayState (the shell
// members the netplay flow reads and writes).

namespace bomber::net {
class Transport;     // the abstract seam: a bare socket, the star hub, or the relay
class UdpTransport;  // the concrete socket the CLI and the direct/LAN rows bind
}  // namespace bomber::net

namespace bomber::game {

class ChatOverlay;

// What the ONLINE SETUP STAGE settled. Returned rather than written through an
// out-parameter, because `config` is only meaningful when `input` is Advance and
// a return value says that where a reference argument cannot.
struct NetSetupResult {
    AppInput input = AppInput::Back;
    sim::MatchConfig config;
};

// Round 0 of the FIRST match of a session, as the connect step and the setup
// stage above it settled between them. The seed is carried ALONGSIDE the config
// rather than read back out of it: the two agree today, but the guest's seed is
// the lobby's and its config is the host's, and a session that walked those two
// numbers apart would rotate rounds nobody agreed on.
struct NetMatchStart {
    std::uint32_t seed = 0;
    sim::MatchConfig config;
};

class NetplayRunner {
public:
    NetplayRunner(NetplaySeams seams, NetplayState state)
        : seams_(std::move(seams)), state_(state) {}

    // CLI netplay entry (--host/--join, ADR-0010 §3.3 step 5): ONE 2-player UDP
    // lockstep match run in place of the front-end. Returns a process exit code.
    // GOLDEN-SAFE: only reachable via net_role, which no test/golden/demo path
    // sets.
    int run_cli();

    // Menu row 1 (START NET GAME) opens the NETWORK GAME menu (LobbyScreen): the
    // online lobby entry points plus the ADR-0010 direct/LAN rows.
    AppInput present_network_menu();

    // Menu row 2 (JOIN NET GAME): the UNCHANGED direct-IP join, so the no-server
    // path keeps working exactly as it did.
    AppInput present_direct_join();

private:
    // The CLI's config, built from the shared seed alone — the default scheme +
    // the install VALUELST, ignoring every per-machine options/level/team/gold
    // overlay, 2 humans in seats 0/1 and the stage picked from the seed. It
    // exists ONLY for `--host`/`--join`, which are scripted, non-interactive
    // entries (ADR-0010 §3.3: no discovery, no seed handshake, both peers pass
    // --seed on the command line) with no second machine to drive a setup
    // screen. Called on the HOST ONLY — the guest takes the result over the wire
    // (exchange_cli_config below), because two builds of libs/match can derive
    // two different boards from one seed and the CLI wire has nothing that would
    // notice. Every INTERACTIVE path — the lobby rows and the direct HOST LAN
    // GAME / JOIN BY IP rows — runs present_setup instead and agrees a real
    // config, which is the whole point of the setup stage.
    sim::MatchConfig canonical_config(std::uint32_t seed) const;
    // The CLI's own config exchange, over the SAME net::SetupSession the lobby
    // path uses: the HOST derives canonical_config(seed) and ships the
    // serialized bytes; the guest adopts them verbatim into `out_cfg`. Board
    // derivation (match::build_match_config + match::apply_actors) therefore
    // happens ONCE per match instead of once per peer — the CLI path checks no
    // build_hash and no protocol version, so a libs/match change shipped to one
    // side only used to desync silently on tick 0. Returns false on timeout
    // (which is exactly what an unpatched partner produces, since it neither
    // sends nor answers setup traffic) or on a window close.
    bool exchange_cli_config(net::UdpTransport& transport, bool is_host, std::uint32_t seed,
                             sim::MatchConfig& out_cfg);
    // The CLI's match hand-off: host owns seat 0, guest seat 1, and the config is
    // the HOST's canonical_config(seed) shipped over the wire — never derived
    // twice (see exchange_cli_config).
    AppInput run_cli_match(net::UdpTransport& transport, int role, std::uint32_t seed);

    // A whole NETPLAY SESSION over one connected transport: setup -> match ->
    // setup -> match -> ... The connect step (lobby punch or direct handshake)
    // happens once, and finishing a match returns BOTH peers to the roster/map
    // screens with the link intact, which is the entire point — the transport
    // used to die with the first match, so a rematch meant re-punching through
    // the lobby, and by then the matchmaker has reaped the room anyway (it drops
    // a lobby ~30 s into a match). Nothing below this line needs the control
    // plane.
    //
    // `start` is round 0 of the FIRST match, already agreed by the caller's own
    // present_setup. Later matches agree their own through this loop. Returns
    // exactly what run_netplay_match/present_setup last returned.
    AppInput run_session(net::Transport& transport, NetSeats seats, const NetMatchStart& start,
                         ChatOverlay* chat = nullptr);

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
    // Returns Advance with `config` filled (Phase::Final — EVERY peer holds it),
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
    // `seats.all` matters as much as `seats.local`: the host waits for an ack
    // from EACH of the others before Phase::Final, so a >2-peer star cannot
    // start the match while somebody is still reassembling the config
    // (setup_session.hpp).
    NetSetupResult present_setup(net::Transport& transport, NetSeats seats, std::uint32_t seed,
                                 ChatOverlay* chat = nullptr);
    // HOST: the two shared screens, then the confirmation and the wait for it to
    // land on every other seat.
    NetSetupResult present_setup_host(const NetSetupLink& link, std::uint32_t seed,
                                      ChatOverlay* chat);
    // The [WAIT] composition (§6) the host sits on between confirming the config
    // and every guest holding it — the event half and the draw half separately,
    // so neither the loop nor either half carries the other's branching.
    AppInput await_setup_acks(const NetSetupLink& link, ChatOverlay* chat);
    // One pump of the [WAIT] screen's events. Engaged = something ended the wait
    // (a window close, or the local player's Escape); empty = keep waiting.
    std::optional<AppInput> pump_wait_events(ChatOverlay* chat);
    void draw_wait_frame(const std::string& glue, unsigned& spin, ChatOverlay* chat);
    // GUEST: mirror whichever of the two shared screens the host is on, and
    // adopt the config it confirms.
    NetSetupResult present_setup_guest(const NetSetupLink& link, ChatOverlay* chat);
    // Run the screen the host is currently on, READ-ONLY off the same link.
    AppInput mirror_host_screen(const NetSetupLink& link, ChatOverlay* chat);
    // Keep pumping after decoding the final config, so a lost ack recovers
    // before the match session takes the socket. False = the window closed.
    bool settle_guest_ack(const NetSetupLink& link, ChatOverlay* chat);

    // The ADR-0010 direct-UDP host (bind kNetDefaultPort + the seed handshake) —
    // reached from the NETWORK GAME menu's HOST LAN GAME row.
    AppInput present_direct_host();
    // The online lobby leaf (ADR-0011 Phase 1d): resolve the matchmaker URL, bind
    // the one socket LobbyFlow reuses for STUN/punch/match, run the waiting room,
    // and on Phase::Ready run the match with the SERVER's seed + seat mask.
    // `browse` (Phase 3) only changes where a GUEST's lobby code comes from — the
    // PUBLIC GAMES browser instead of the typed-code prompt; the waiting room and
    // the match hand-off below it are the same code either way.
    //
    // DECLARED unconditionally but DEFINED only under BOMBER_HAS_LOBBY: that
    // define is PUBLIC on bomber::net, which libs/game links PRIVATEly, so it is
    // visible while compiling bomber_game_core but NOT to a consumer including
    // this header — guarding the declarations would give NetplayRunner two
    // different definitions across translation units (an ODR violation). Nothing
    // outside the guarded call site in present_network_menu references these, so
    // a lobby-off build simply never emits or needs them.
    AppInput present_online(bool host, bool browse = false, bool is_public = false);
    // The pre-connect prompts: HOW MANY PLAYERS on the host, the typed code or
    // the public browser on a guest. `ok` false means the player backed out and
    // `abort` is what the leaf should return.
    struct LobbyChoice {
        AppInput abort = AppInput::Advance;
        std::string code;
        int max_seats = 0;
        bool ok = false;
    };
    LobbyChoice choose_lobby(LobbyScreen& screen, const LobbyScreen::OnlineConfig& ocfg, bool host,
                             bool browse);
    // Matchmaker endpoint resolution, in the documented precedence order:
    // --matchmaker / --matchmaker-stun CLI flags, then BOMBER_MATCHMAKER_URL /
    // BOMBER_MATCHMAKER_STUN_HOST / BOMBER_MATCHMAKER_STUN_PORT, then the
    // compile-time defaults in netplay_runner.cpp, which name the DEPLOYED
    // matchmaker — so the online rows work with no flags.
    std::string matchmaker_url() const;
    std::string matchmaker_stun_host() const;
    std::uint16_t matchmaker_stun_port() const;

    NetplaySeams seams_;
    NetplayState state_;
};

}  // namespace bomber::game
