#include "bomber/game/screens/netplay_runner.hpp"

#include <SDL3/SDL.h>

#include <cstdint>
#include <cstdlib>
#include <string>

#include "bomber/assets/extra.hpp"  // assets::extra::load_for_board
#include "bomber/game/chat_overlay.hpp"
#include "bomber/game/frontend_util.hpp"  // pick_glue
#include "bomber/game/log.hpp"
#include "bomber/game/screens/campaign_state.hpp"
#include "bomber/game/screens/map_select_screen.hpp"
#include "bomber/game/screens/match_runner.hpp"
#include "bomber/game/screens/net_settle.hpp"
#include "bomber/game/screens/netplay_connect_screen.hpp"
#include "bomber/game/screens/setup_screen.hpp"
#include "bomber/game/sprites.hpp"  // Sprite
#include "bomber/match/match_factory.hpp"
#include "bomber/net/setup_session.hpp"  // net::SetupSession (the setup stage + the CLI exchange)
#include "bomber/net/transport.hpp"      // net::Transport (the socket/star/relay seam)
#include "bomber/net/udp_transport.hpp"  // net::UdpTransport (the CLI + direct/LAN rows)
#include "bomber/platform/frame_clock.hpp"

#if defined(BOMBER_HAS_LOBBY)
// The lobby control plane, owned by present_online so it OUTLIVES the
// waiting room — the F2 chat overlay keeps using it through the setup screens.
#include "bomber/net/build_hash.hpp"
#include "bomber/net/lobby_client.hpp"
#include "bomber/net/lobby_flow.hpp"
#endif

namespace bomber::game {

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

// A direct/LAN game is a PAIR by construction — one address, one peer — so its
// seat topology is the literal 0b11 the online path derives from the server.
constexpr std::uint16_t kPairSeatsHost = 0b01u;
constexpr std::uint16_t kPairSeatsGuest = 0b10u;
constexpr std::uint16_t kPairSeatsAll = 0b11u;

// The acknowledge modal mapped onto the setup stage's two outcomes: a window
// close still propagates, anything else means "back to the menu". Three call
// sites carried this as an inline nested ternary.
AppInput notice_then_back(ScreenContext ctx, const char* top, const char* body) {
    return run_net_notice(ctx, top, body) == AppInput::Quit ? AppInput::Quit : AppInput::Back;
}

// What a cancelled pre-connect prompt should return: a window close propagates
// as Quit, an Esc just goes back to the menu. Shared by all three prompts in
// choose_lobby.
AppInput prompt_abort(bool window_closed) {
    return window_closed ? AppInput::Quit : AppInput::Advance;
}

// The setup stage's Esc key — the only one either shared screen reads directly
// (everything else belongs to the screen that is up).
bool is_escape_press(const SDL_Event& ev) {
    return ev.type == SDL_EVENT_KEY_DOWN && !ev.key.repeat && ev.key.key == SDLK_ESCAPE;
}

// How long a guest keeps pumping after decoding the final config, before it
// hands the socket to the match session (see settle_guest_ack).
constexpr std::uint64_t kGuestAckSettleMs = 300;

}  // namespace

int NetplayRunner::run_cli() {
    // CLI netplay entry (--host/--join, ADR-0010 §3.3 step 5): ONE 2-player UDP
    // lockstep match run in place of the front-end. The CLI carries --seed on
    // BOTH peers, so there is no discovery and NO seed handshake here — bind +
    // set_peer straight from the options and hand off to the shared match core
    // (run_cli_match), which agrees the CONFIG over the wire before it
    // starts (exchange_cli_config; the seed alone is not enough, because
    // the board it derives depends on both peers running the same libs/match).
    // The MENU path (present_network_menu/present_direct_join) runs the
    // SeedHandshake instead; both funnel into that same core. CANNOT be
    // runtime-tested here (no display / second instance) — validated live.
    const bool host = state_.net_role == 1;
    const int local_seat = host ? 0 : 1;

    // Real UDP link (raw sockets, SDL-free). BOTH peers bind a KNOWN local port
    // and set_peer() to the other's known port: without a handshake the CLI has
    // no way to learn the peer, so requiring a fixed port + address on each side
    // (via the args) is the only symmetric MVP that works with this dumb,
    // discovery-less wire — see main.cpp's --host/--join usage.
    net::UdpTransport transport;
    if (!transport.bind(state_.net_local_port)) {
        log_warn("netplay: bind failed (local port %u)",
                 static_cast<unsigned>(state_.net_local_port));
        return 1;
    }
    if (!transport.set_peer(state_.net_peer_host, state_.net_peer_port)) {
        log_warn("netplay: cannot resolve peer %s:%u", state_.net_peer_host.c_str(),
                 static_cast<unsigned>(state_.net_peer_port));
        return 1;
    }
    log_info("netplay: %s  bound_port=%u  peer=%s:%u  seed=0x%08X  seat=%d",
             host ? "HOST" : "GUEST", static_cast<unsigned>(transport.local_port()),
             state_.net_peer_host.c_str(), static_cast<unsigned>(state_.net_peer_port),
             static_cast<unsigned>(state_.net_seed), local_seat);

    run_cli_match(transport, state_.net_role, state_.net_seed);
    return 0;  // CLI always exits 0 after the single match (window-close included)
}

sim::MatchConfig NetplayRunner::canonical_config(std::uint32_t seed) const {
    // CANONICAL 2-human MatchConfig, built from the shared seed alone and
    // independent of each machine's options.ini / level pick.
    //
    // IT IS NO LONGER DERIVED ON BOTH PEERS. Only the HOST calls this; the guest
    // receives the resulting bytes through exchange_cli_config, because
    // "the same seed gives the same board" holds only while both peers run the
    // same libs/match — and nothing on the CLI wire could check that.
    // Deterministic lockstep needs an identical seed AND identical config
    // (ADR-0010 seed/roster parity), so this deliberately IGNORES the live
    // per-machine options/level/team/gold state and builds only
    // from the shared seed + the default scheme + the install VALUELST (the same
    // 1997 file on both installs). Mirrors start_match's config build,
    // minus every per-machine overlay; random_start off so the spawn assignment is
    // fixed too.
    //
    // THIS IS NO LONGER THE ONLINE PATH. It used to be built inline by the match
    // core for EVERY netplay caller, which is exactly why an
    // online match had no map choice, no AI and no roster; the interactive paths
    // now agree a real config through present_setup. What is left here serves
    // only `--host`/`--join`, which are scripted and have no screens to drive.
    const ScreenContext ctx = seams_.sctx();
    sim::MatchConfig cfg = match::build_match_config(state_.scheme, 2, seed, &ctx.values,
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
    const int stage = match::pick_stage(state_.base_tuning, seed);
    cfg.tuning.level_index = stage;
    const auto actors =
        assets::extra::load_for_board(state_.game_dir, stage, sim::kGridWidth, sim::kGridHeight);
    match::apply_actors(cfg, actors, seed);
    return cfg;
}

bool NetplayRunner::exchange_cli_config(net::UdpTransport& transport, bool is_host,
                                        std::uint32_t seed, sim::MatchConfig& out_cfg) {
    // THE CLI'S CONFIG EXCHANGE. It exists because "both peers derive the same
    // board from the same seed" is a promise the code cannot keep across builds.
    // canonical_config() runs the WHOLE derivation locally — the .SCH
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
    // lobby path already does (present_setup): the HOST derives it and ships
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

    const std::uint16_t local_seats = is_host ? kPairSeatsHost : kPairSeatsGuest;
    const std::uint16_t guest_seats = is_host ? kPairSeatsGuest : std::uint16_t{0};
    net::SetupSession session(transport, is_host, local_seats, guest_seats, kCliSetupTimeoutMs);
    if (is_host) session.confirm(canonical_config(seed));

    while (session.phase() != net::SetupSession::Phase::Final) {
        if (net_window_closed()) return false;
        session.step(static_cast<std::int64_t>(SDL_GetTicks()));
        if (session.failed()) {
            log_warn(
                "netplay: match-config exchange timed out after %d ms — the peer is gone "
                "or is running a build from before the CLI exchanged configs.",
                kCliSetupTimeoutMs);
            return false;
        }
        SDL_Delay(2);
    }
    out_cfg = session.final_config();

    // SETTLE, for the reason the setup stage's guest settles: our ack may
    // be the datagram that is lost, and SetupSession only re-acks when the
    // host's next burst arrives — which we would never see if we left the
    // instant we decoded. Safe against the one-pump-at-a-time rule because
    // RollbackSession re-sends its whole input window every pump.
    if (!is_host && !net_settle(kCliSetupSettleMs, [&session] {
            session.step(static_cast<std::int64_t>(SDL_GetTicks()));
        }))
        return false;
    log_info("netplay: match config agreed (%s), stage %d", is_host ? "sent" : "received",
             out_cfg.tuning.level_index);
    return true;
}

AppInput NetplayRunner::run_cli_match(net::UdpTransport& transport, int role, std::uint32_t seed) {
    // The ADR-0010 role-derived CLI entry (--host/--join): host owns seat 0, guest
    // seat 1, and the config is the HOST's canonical_config(seed) shipped
    // over the wire — never derived twice (see exchange_cli_config).
    const bool is_host = role == 1;
    sim::MatchConfig cfg;
    if (!exchange_cli_config(transport, is_host, seed, cfg)) return AppInput::Back;
    return run_netplay_match(
        seams_, state_, transport,
        NetSeats{is_host ? kPairSeatsHost : kPairSeatsGuest, kPairSeatsAll, is_host}, cfg);
}

AppInput NetplayRunner::run_session(net::Transport& transport, NetSeats seats,
                                    const NetMatchStart& start, ChatOverlay* chat) {
    // ONE CONNECTED TRANSPORT, MANY MATCHES. The connect step (the lobby punch or
    // the direct seed handshake) ran once, above; from here the link is simply
    // reused — match, setup, match, setup — for as long as both players want to
    // keep going. Nothing in this loop touches the matchmaker, which matters
    // rather than being a nicety: the server reaps a lobby about 30 s into a
    // match (HeartbeatInterval 10 x HeartbeatMiss 3), so the control plane is
    // already gone by the time the first match ends. `chat` is likewise dead
    // after the first setup stage — present_online closes it before the
    // match — so later stages simply run without it.
    //
    // WHO DECIDES. Both peers know the match is decided with no traffic (same
    // sim, same tally, same clinch). What they agree over the wire is the
    // TRANSITION: the match loop's RematchGate holds the VICTORY screen
    // until the HOST dismisses it and announces MatchCtlKind::Rematch, so nobody
    // walks into the next setup stage alone. A peer that pressed Escape there
    // gets `rematch == false` and this loop ends — which is the old behaviour,
    // now a deliberate choice rather than the only option.
    sim::MatchConfig match_cfg = start.config;
    std::uint32_t match_seed = start.seed;
    NetSessionCarry carry;
    while (true) {
        const AppInput r = run_netplay_match(seams_, state_, transport, seats, match_cfg, &carry);
        if (r == AppInput::Quit || !carry.rematch) return r;
        // The tick space walks on across the match boundary for exactly the
        // reason it walks on across a round boundary (round_rotation.hpp): a
        // datagram still in flight from the last round must not land inside the
        // first round of the next match. `carry.round_base` came back holding the
        // last round played, so the next match starts one past it.
        carry.round_base = (carry.round_base + 1) % kNetRoundBaseWrap;
        // A fresh seed for the next match's board. HOST-ONLY in effect — the
        // guest's copy is never read (present_setup's guest arm ignores it
        // and takes the host's whole confirmed config), so the two need not
        // agree on this number at all. Stepped by the golden-ratio constant
        // rather than +1 so a rematch is not simply the next board in the
        // sequence the match just played through.
        match_seed += 0x9E3779B9u;
        const NetSetupResult setup = present_setup(transport, seats, match_seed, chat);
        if (setup.input == AppInput::Quit) return AppInput::Quit;
        if (setup.input != AppInput::Advance) return AppInput::Advance;  // left the setup stage
        match_cfg = setup.config;
    }
}

NetSetupResult NetplayRunner::present_setup(net::Transport& transport, NetSeats seats,
                                            std::uint32_t seed, ChatOverlay* chat) {
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
    net::SetupSession session(transport, seats.host, seats.local, seats.remote());
    NetSetupLink link;
    link.session = &session;
    link.local_seats = seats.local;
    link.remote_seats = seats.remote();
    link.host = seats.host;

    return seats.host ? present_setup_host(link, seed, chat) : present_setup_guest(link, chat);
}

NetSetupResult NetplayRunner::present_setup_host(const NetSetupLink& link, std::uint32_t seed,
                                                 ChatOverlay* chat) {
    const ScreenContext ctx = seams_.sctx();
    // The wire seats must hold exactly the roster the seat masks can carry
    // before the host sees the screen (net_setup_link.hpp "SEAT LOCKING").
    net_setup_seed_host_roster(link, state_.setup_type, state_.setup_sub);
    const AppInput roster = SetupScreen(ctx, seams_.setup_state(), seams_.campaign_state(),
                                        seams_.match_backdrop(), link, chat)
                                .run();
    if (roster == AppInput::Quit) return {AppInput::Quit, {}};
    if (roster != AppInput::Advance) return {AppInput::Back, {}};  // Esc: whole flow aborts (§7)
    const AppInput level = MapSelectScreen(ctx, seams_.map_select_state(), link, chat).run();
    if (level == AppInput::Quit) return {AppInput::Quit, {}};
    if (level != AppInput::Advance) return {AppInput::Back, {}};

    // The SAME build the local PLAY path does, from the SAME screens — the
    // whole point of reusing them. Then confirm: the resolved config goes over
    // the wire in full (match_config_codec.hpp), so the guest never rebuilds a
    // board from an index and cannot desync on tick 0.
    NetSetupResult out;
    out.config = MatchRunner(ctx, seams_.match_runner_state()).build_config(seed);
    link.session->confirm(out.config);
    out.input = await_setup_acks(link, chat);
    return out;
}

std::optional<AppInput> NetplayRunner::pump_wait_events(ChatOverlay* chat) {
    const ScreenContext ctx = seams_.sctx();
    SDL_Event ev;
    while (SDL_PollEvent(&ev)) {
        if (ev.type == SDL_EVENT_QUIT) return AppInput::Quit;
        if (chat != nullptr && chat->handle_event(ev, ctx)) continue;
        if (!is_escape_press(ev)) continue;
        ctx.audio.play(20);
        return AppInput::Back;
    }
    return std::nullopt;  // nothing ended the wait
}

void NetplayRunner::draw_wait_frame(const std::string& glue, unsigned& spin, ChatOverlay* chat) {
    const ScreenContext ctx = seams_.sctx();
    ctx.audio.update_music();
    SDL_SetRenderDrawColor(ctx.sdl, 0, 0, 0, 255);
    SDL_RenderClear(ctx.sdl);
    if (const Sprite& bg = ctx.assets.frontend_pcx(glue); bg.tex) {
        SDL_FRect d{0, 0, static_cast<float>(bg.w), static_cast<float>(bg.h)};
        SDL_RenderTexture(ctx.sdl, bg.tex, nullptr, &d);
    }
    draw_net_wait_prompt(ctx, spin);
    if (chat != nullptr) chat->draw(ctx);
    SDL_RenderPresent(ctx.sdl);
}

AppInput NetplayRunner::await_setup_acks(const NetSetupLink& link, ChatOverlay* chat) {
    // Wait for the ack on Phase::Final, NOT has_final_config() — the latter is
    // already true here (we just confirmed) and would start the match before
    // the guest holds the bytes.
    // The [WAIT] composition (§6): the GLUE backdrop these pre-match screens
    // already use, plus the pinned getstring(80) spinner prompt. Nothing new
    // is drawn for a state the original never had a picture for.
    const ScreenContext ctx = seams_.sctx();
    const std::string glue = pick_glue(state_.setup_lcg, ctx.values);
    platform::FrameClock frame_clock(ctx.window);
    unsigned spin = 0;
    while (!net_setup_final(link)) {
        if (const std::optional<AppInput> ended = pump_wait_events(chat)) return *ended;
        net_setup_pump(link);
        if (chat != nullptr) chat->pump();
        if (net_setup_failed(link))
            return notice_then_back(ctx, "NETWORK ERROR", "THE OTHER PLAYER LEFT");
        draw_wait_frame(glue, spin, chat);
        frame_clock.pace();
    }
    return AppInput::Advance;
}

AppInput NetplayRunner::mirror_host_screen(const NetSetupLink& link, ChatOverlay* chat) {
    // Whichever of the two shared screens the host is on. The preview's `rounds`
    // field is the screen cue (net_setup_link.hpp's sentinel), standing in for
    // the original's kind-32 sub_40F064(901/902) advance commands, which our
    // protocol does not carry. Both screens run READ-ONLY off the same link.
    const ScreenContext ctx = seams_.sctx();
    if (net_setup_on_level_screen(link))
        return MapSelectScreen(ctx, seams_.map_select_state(), link, chat).run();
    return SetupScreen(ctx, seams_.setup_state(), seams_.campaign_state(), seams_.match_backdrop(),
                       link, chat)
        .run();
}

bool NetplayRunner::settle_guest_ack(const NetSetupLink& link, ChatOverlay* chat) {
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
    return net_settle(kGuestAckSettleMs, [&link, chat] {
        net_setup_pump(link);
        if (chat != nullptr) chat->pump();  // don't go silent on the lobby either
    });
}

NetSetupResult NetplayRunner::present_setup_guest(const NetSetupLink& link, ChatOverlay* chat) {
    // GUEST — follow the host through the two shared screens until it confirms a
    // config, leaves, or stops answering.
    //
    // The team GATE is driven off the host's live preview while these screens are
    // up (setup_screen.cpp's net_setup_apply_roster), so it is borrowed for the
    // scope and given back — see TeamPlayScope, which is what makes every exit
    // below, including the ones added by this extraction, restore it.
    const TeamPlayScope team_gate(state_.team_play);
    while (true) {
        const AppInput r = mirror_host_screen(link, chat);
        if (r == AppInput::Quit) return {AppInput::Quit, {}};
        // ORDER IS LOAD-BEARING: final before failed. A session that has just
        // reached Phase::Final and then timed out has still AGREED a config, and
        // reporting "THE HOST LEFT THE GAME" over one both peers hold would throw
        // away a match that was ready to start.
        if (net_setup_final(link)) {
            if (!settle_guest_ack(link, chat)) return {AppInput::Quit, {}};
            return {AppInput::Advance, link.session->final_config()};
        }
        if (net_setup_failed(link))
            // Timeout with no config: the host closed the game or the path died.
            return {notice_then_back(seams_.sctx(), "NETWORK ERROR", "THE HOST LEFT THE GAME"), {}};
        if (r == AppInput::Back) return {AppInput::Back, {}};  // Esc: leave the setup stage
        // Otherwise the host simply moved between the two screens — follow it.
    }
}

AppInput NetplayRunner::present_network_menu() {
    // NETWORK.RSS (1040) is started by the MENU DISPATCH, not here — see the
    // AppState::NetHost/NetJoin arms in run_app. It has to be, because this
    // function can reach present_direct_join() through the lobby menu's JoinDirect
    // row, and starting the track in both bodies would restart it from the top
    // on that hop (the same defect the goldman wheel -> player select hand-off
    // had). The original has no such nesting: sub_42B0CE and sub_42B47D are two
    // separate menu rows, each starting 1040 as its own first act.
    // START NET GAME (menu row 1) now opens the NETWORK GAME menu (ADR-0011
    // Phase 1d): the online lobby entry points plus the ADR-0010 direct/LAN
    // rows, which need no server and must keep working. Menu row 2 (JOIN NET
    // GAME -> present_direct_join) is deliberately left as the unchanged direct-IP
    // join. On a lobby-off build the online rows are simply not listed.
#if defined(BOMBER_HAS_LOBBY)
    constexpr bool kOnlineAvailable = true;
#else
    constexpr bool kOnlineAvailable = false;
#endif
    switch (LobbyScreen(seams_.sctx()).run_menu(kOnlineAvailable)) {
        case LobbyMenuChoice::WindowClosed: return AppInput::Quit;
        case LobbyMenuChoice::HostDirect: return present_direct_host();
        case LobbyMenuChoice::JoinDirect: return present_direct_join();
#if defined(BOMBER_HAS_LOBBY)
        case LobbyMenuChoice::HostOnline: return present_online(/*host=*/true);
        case LobbyMenuChoice::HostPublic:
            // Same room and same flow — the visibility only changes what the
            // server advertises, so a public lobby is still joinable by code.
            return present_online(/*host=*/true, /*browse=*/false, /*is_public=*/true);
        case LobbyMenuChoice::JoinOnline: return present_online(/*host=*/false);
        case LobbyMenuChoice::BrowsePublic:
            // The browser hands back a code, so this is the JOIN arm with the
            // typed-code prompt swapped for a picked row (ADR-0011 Phase 3).
            return present_online(/*host=*/false, /*browse=*/true);
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

AppInput NetplayRunner::present_direct_host() {
    // HOST LAN GAME: bind kNetDefaultPort, run the seed handshake as host, then —
    // once the peer is connected — the SHARED setup stage (roster/AI + map) as the
    // host, and finally the match as seat 0. Esc/timeout return to the menu; a
    // window close during connect/setup/match propagates Quit. (This is the
    // ADR-0010 body that used to sit directly on menu row 1; the only change is
    // that a LAN game now picks its map and AI slots like the online one, since
    // net::SetupSession needs no matchmaker and rides the same UDP socket.)
    net::UdpTransport transport;
    NetplayConnectResult r =
        NetplayConnectScreen(seams_.sctx()).run_host(transport, kNetDefaultPort, kNetHostSeed);
    if (r.window_closed) return AppInput::Quit;
    if (!r.connected) return AppInput::Advance;  // cancelled/timed out → back to the menu
    const NetSeats seats{kPairSeatsHost, kPairSeatsAll, /*host=*/true};
    const NetSetupResult setup = present_setup(transport, seats, r.seed);
    if (setup.input == AppInput::Quit) return AppInput::Quit;
    if (setup.input != AppInput::Advance) return AppInput::Advance;
    // A SESSION, not a single match: finishing one returns both peers to these
    // same setup screens over this same socket (run_session).
    return run_session(transport, seats, NetMatchStart{r.seed, setup.config});
}

AppInput NetplayRunner::present_direct_join() {
    // JOIN NET GAME (menu row 2): prompt for the host address (prefilled
    // 127.0.0.1:kNetDefaultPort), connect, run the handshake as guest (adopting
    // the host's seed), then watch the host's roster/map screens read-only and
    // run the match as seat 1 with the config the host confirmed.
    //
    // NETWORK.RSS (1040) — started by the menu dispatch, see present_network_menu's
    // note for why it is not started here.
    net::UdpTransport transport;
    NetplayConnectResult r =
        NetplayConnectScreen(seams_.sctx()).run_join(transport, kNetDefaultPort);
    if (r.window_closed) return AppInput::Quit;
    if (!r.connected) return AppInput::Advance;
    const NetSeats seats{kPairSeatsGuest, kPairSeatsAll, /*host=*/false};
    const NetSetupResult setup = present_setup(transport, seats, r.seed);
    if (setup.input == AppInput::Quit) return AppInput::Quit;
    if (setup.input != AppInput::Advance) return AppInput::Advance;
    return run_session(transport, seats, NetMatchStart{r.seed, setup.config});
}

#if defined(BOMBER_HAS_LOBBY)
std::string NetplayRunner::matchmaker_url() const {
    if (!state_.matchmaker_url.empty()) return state_.matchmaker_url;  // --matchmaker
    const std::string env = env_or_empty("BOMBER_MATCHMAKER_URL");
    return env.empty() ? std::string(kDefaultMatchmakerUrl) : env;
}

std::string NetplayRunner::matchmaker_stun_host() const {
    if (!state_.matchmaker_stun_host.empty()) return state_.matchmaker_stun_host;
    const std::string env = env_or_empty("BOMBER_MATCHMAKER_STUN_HOST");
    return env.empty() ? url_host(matchmaker_url()) : env;
}

std::uint16_t NetplayRunner::matchmaker_stun_port() const {
    if (state_.matchmaker_stun_port != 0) return state_.matchmaker_stun_port;
    // Same env name tests/net/test_lobby_live.cpp already uses, so one exported
    // variable configures both the live test and the game.
    const std::string env = env_or_empty("BOMBER_MATCHMAKER_STUN_PORT");
    const long p = env.empty() ? 0 : std::strtol(env.c_str(), nullptr, 10);
    return (p > 0 && p <= 65535) ? static_cast<std::uint16_t>(p) : kDefaultStunPort;
}

NetplayRunner::LobbyChoice NetplayRunner::choose_lobby(LobbyScreen& screen,
                                                       const LobbyScreen::OnlineConfig& ocfg,
                                                       bool host, bool browse) {
    LobbyChoice out;
    out.max_seats = kDefaultLobbySeats;
    bool closed = false;

    // HOW BIG A LOBBY. `max_seats` is a CreateLobby field the server mints seats
    // from and cannot be changed once the room exists (PROTOCOL.md §3, clamped
    // to 2..10), so the host is asked BEFORE anything is created. This used to be
    // a hard-coded 2 — the single reason an online game could never be more than
    // a pair, since the whole seat topology below flows from the seats the server
    // hands out. Guests never see this: they take whatever the room has.
    if (host && !screen.run_seat_count(out.max_seats, closed)) return {prompt_abort(closed)};

    // BROWSE PUBLIC GAMES (Phase 3) — just another way to fill in `code`. The
    // browser needs a bound socket of its own (LobbyFlow owns one either way)
    // but never punches with it, so it is scoped to the browse and closed
    // before the match socket is opened.
    if (browse) {
        net::UdpTransport browse_transport;
        if (!browse_transport.bind(0)) {
            log_warn("lobby: cannot open a UDP socket");
            return {AppInput::Advance};
        }
        if (!screen.run_public_browser(ocfg, browse_transport, out.code, closed))
            return {prompt_abort(closed)};
        out.ok = true;
        return out;
    }
    // JOIN BY CODE: the guest types the 6 characters the host read out. A HOST
    // names no room here — it is about to create one.
    if (!host && !screen.run_code_entry(out.code, closed)) return {prompt_abort(closed)};
    out.ok = true;
    return out;
}

AppInput NetplayRunner::present_online(bool host, bool browse, bool is_public) {
    // HOST PRIVATE/PUBLIC GAME / JOIN BY CODE / BROWSE PUBLIC GAMES: the ADR-0011 online
    // path. A guest first names the lobby it wants — typing the 6-char code, or
    // picking a row in the public browser, which yields the very same code — then
    // both sides bind ONE socket, sit in the waiting room, and, once the server's
    // StartMatch arrives and the peers punch a direct path, run the match with the
    // SERVER's authoritative seed and seat mask (never a locally derived
    // host?0:1). The browser is deliberately just another way to fill in `code`:
    // everything below it is the unchanged join path.
    const ScreenContext ctx = seams_.sctx();
    LobbyScreen screen(ctx);

    LobbyScreen::OnlineConfig ocfg;
    ocfg.server_url = matchmaker_url();
    ocfg.stun_host = matchmaker_stun_host();
    ocfg.stun_port = matchmaker_stun_port();
    // The original's "Node Name" (options row 2, sub_40FE34) IS the per-machine
    // net identity — reuse it when the player has set one.
    ocfg.player_name =
        state_.options.node_name.empty() ? std::string("PLAYER") : state_.options.node_name;

    const LobbyChoice choice = choose_lobby(screen, ocfg, host, browse);
    if (!choice.ok) return choice.abort;

    // Bound BEFORE the flow is built: LobbyFlow reuses this exact socket for the
    // STUN probe, the hole punch and the match, so the NAT binding the peers
    // punched is the one gameplay flows through (lobby_flow.hpp).
    net::UdpTransport transport;
    if (!transport.bind(0)) {
        log_warn("lobby: cannot open a UDP socket");
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
        flow.host_lobby(ocfg.player_name, is_public, choice.max_seats);
    else
        flow.join_lobby(choice.code);

    const LobbyRoomResult r =
        screen.run_online(flow, chat, host, choice.code, host ? choice.max_seats : 0);
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
    // config every peer agreed. present_setup stops pumping its session
    // before returning, so the match session has the transport to itself
    // (setup_session.hpp's one obligation — whichever polls first eats the
    // datagram; on the hub that same poll is what keeps the star reflecting).
    const NetSeats seats{r.local_seats_mask, r.all_seats_mask, r.is_host};
    const NetSetupResult setup = present_setup(link, seats, r.seed, &chat);
    // Chat stops at the door of the match: the overlay is a lobby thing, and the
    // WS link closes with `client` when this function returns anyway.
    chat.close();
    if (setup.input == AppInput::Quit) return AppInput::Quit;
    if (setup.input != AppInput::Advance) return AppInput::Advance;
    // The chat is already closed, so later setup stages run without it — see
    // run_session on why nothing below here may depend on the server.
    return run_session(link, seats, NetMatchStart{r.seed, setup.config});
}
#endif  // BOMBER_HAS_LOBBY

}  // namespace bomber::game
