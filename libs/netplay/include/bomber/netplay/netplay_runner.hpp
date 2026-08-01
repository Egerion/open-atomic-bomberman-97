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
// (ADR-0009, the decomposition the netplay half never got). Three public
// entries: run_cli (--host/--join), present_network_menu (menu row 1) and
// present_direct_join (menu row 2).
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
class LobbyFlow;     // the lobby control plane; only the guarded leaves touch it
class PathFailover;  // the mid-match dead-path detector + relay switch
}  // namespace bomber::net

namespace bomber::game {

class ChatOverlay;

// What the ONLINE SETUP STAGE settled. `config` is only meaningful when `input`
// is Advance, which a return value says where an out-parameter cannot.
struct NetSetupResult {
    AppInput input = AppInput::Back;
    sim::MatchConfig config;
};

// Round 0 of the FIRST match of a session. The seed is carried ALONGSIDE the
// config rather than read back out of it: the two agree today, but the guest's
// seed is the lobby's and its config is the host's, and a session that walked
// those two numbers apart would rotate rounds nobody agreed on.
struct NetMatchStart {
    std::uint32_t seed = 0;
    sim::MatchConfig config;
};

class NetplayRunner {
public:
    NetplayRunner(NetplaySeams seams, NetplayState state)
        : seams_(std::move(seams)), state_(state) {}

    // ADR-0010 §3.3 step 5: ONE 2-player UDP match run in place of the front end.
    // Returns an AppInput like its two sibling entries: Back means the CLI never
    // got a working link (bind/resolve/config-exchange failure — the caller maps
    // it to exit code 1); anything else is the match's own verdict, which the
    // CLI exit deliberately does not distinguish. GOLDEN-SAFE: only reachable
    // via net_role, which no test/golden/demo path sets.
    AppInput run_cli();

    AppInput present_network_menu();
    AppInput present_direct_join();

private:
    // The CLI's config, built from the shared seed alone. Called on the HOST ONLY
    // — the guest takes the result over the wire (exchange_cli_config), because
    // two builds of libs/match can derive two different boards from one seed and
    // the CLI wire has nothing that would notice.
    sim::MatchConfig canonical_config(std::uint32_t seed) const;
    // The CLI's own config exchange, over the SAME net::SetupSession the lobby
    // path uses. Returns false on timeout (which is exactly what an unpatched
    // partner produces, since it neither sends nor answers setup traffic) or on a
    // window close.
    bool exchange_cli_config(net::UdpTransport& transport, bool is_host, std::uint32_t seed,
                             sim::MatchConfig& out_cfg);
    AppInput run_cli_match(net::UdpTransport& transport, int role, std::uint32_t seed);

    // A whole NETPLAY SESSION over one connected transport: setup -> match ->
    // setup -> match -> ... Finishing a match returns BOTH peers to the
    // roster/map screens with the link intact — the transport used to die with
    // the first match, so a rematch meant re-punching through a lobby the
    // matchmaker had already reaped. `start` is round 0 of the FIRST match.
    //
    // `failover` is the mid-match dead-path engine (online 2-seat direct matches
    // only); the LAN/CLI paths have no control plane and pass nothing.
    AppInput run_session(net::Transport& transport, NetSeats seats, const NetMatchStart& start,
                         ChatOverlay* chat = nullptr, net::PathFailover* failover = nullptr);

    // THE ONLINE SETUP STAGE (docs/re/network-screens.md §7, ADR-0011): the real
    // roster/AI and map screens between the connect step and the match, over the
    // SAME transport. The HOST drives them and publishes a preview after each
    // edit; a GUEST renders the same two screens READ-ONLY, buzzing SFX 40 at any
    // edit key, and adopts the confirmed config.
    //
    // Returns Advance with `config` filled (Phase::Final — EVERY peer holds it),
    // Back if the stage was left/timed out, or Quit on a window close. STOPS
    // pumping the setup session before returning: the match session drains the
    // same transport and whichever polls first eats the datagram
    // (setup_session.hpp's one obligation).
    //
    // `chat` is nullptr on the direct/LAN paths, which have no matchmaker to chat
    // over. `seats.all` matters as much as `seats.local`: the host waits for an
    // ack from EACH of the others before Phase::Final, so a >2-peer star cannot
    // start the match while somebody is still reassembling the config.
    NetSetupResult present_setup(net::Transport& transport, NetSeats seats, std::uint32_t seed,
                                 ChatOverlay* chat = nullptr);
    NetSetupResult present_setup_host(const NetSetupLink& link, std::uint32_t seed,
                                      ChatOverlay* chat);
    // The [WAIT] composition (§6) the host sits on between confirming the config
    // and every guest holding it — the event half and the draw half separately,
    // so neither the loop nor either half carries the other's branching.
    AppInput await_setup_acks(const NetSetupLink& link, ChatOverlay* chat);
    // Engaged = something ended the wait; empty = keep waiting.
    std::optional<AppInput> pump_wait_events(ChatOverlay* chat);
    void draw_wait_frame(const std::string& glue, unsigned& spin, ChatOverlay* chat);
    NetSetupResult present_setup_guest(const NetSetupLink& link, ChatOverlay* chat);
    // Run the screen the host is currently on, READ-ONLY off the same link.
    AppInput mirror_host_screen(const NetSetupLink& link, ChatOverlay* chat);
    // Keep pumping after decoding the final config, so a lost ack recovers before
    // the match session takes the socket. False = the window closed.
    bool settle_guest_ack(const NetSetupLink& link, ChatOverlay* chat);

    AppInput present_direct_host();

    // The online lobby leaf (ADR-0011). `browse` only changes where a GUEST's
    // lobby code comes from — the PUBLIC GAMES browser instead of the typed-code
    // prompt.
    //
    // DECLARED unconditionally but DEFINED only under BOMBER_HAS_LOBBY (this and
    // the five below it): that define is PUBLIC on bomber::net, which the netplay
    // target links PRIVATEly, so it is visible while compiling this package but
    // NOT to a consumer including this header — guarding the declarations would
    // give NetplayRunner two different definitions across translation units (an
    // ODR violation).
    AppInput present_online(bool host, bool browse = false, bool is_public = false);
    // Everything after the waiting room said Ready.
    AppInput run_online_session(net::LobbyFlow& flow, ChatOverlay& chat,
                                const LobbyRoomResult& room);
    // The pre-connect prompts. `ok` false means the player backed out and `abort`
    // is what the leaf should return.
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
    LobbyScreen::OnlineConfig online_config() const;
    std::string matchmaker_url() const;
    std::string matchmaker_stun_host() const;
    std::uint16_t matchmaker_stun_port() const;

    NetplaySeams seams_;
    NetplayState state_;
};

}  // namespace bomber::game
