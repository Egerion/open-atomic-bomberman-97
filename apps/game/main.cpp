// bomber_game — playable front-end: renders the deterministic sim with the
// original game's assets (loaded at runtime from the player's own install).
//
//   bomber_game [game_dir] [scheme.sch]
//     Boots the front-end: logos -> title (attract loop) -> navigable menu ->
//     match -> results (DRAW / VICTORY<player>) -> menu.
//     Menu: Up/Down (or W/S) highlight, Enter/Space select, Esc quits.
//       Start -> match, Quit -> exit; Options/Controllers/Network/Credits are
//       stubs pending the .BM text-screen parser.
//     Player 0: arrows + Right Ctrl (bomb)     Player 1: WASD + Left Ctrl (bomb)
//     In a screen: Enter/Space advances; Esc backs out (quits from title/menu).
//
//   bomber_game --match [game_dir] [scheme.sch]
//     Dev fast-path: skip the front-end and boot straight into a match (same
//     as env BOMBER_BOOT_MATCH=1). Returns to the menu when the match ends.
//
//   bomber_game --demo <ticks> <out.bmp> [game_dir] [scheme.sch]
//     Headless verification: scripted inputs, renders frames, saves the last.
//
//   bomber_game --demo-shots <label:tick,label:tick,...> <outdir> [game_dir] [scheme.sch]
//     Visual golden harness (tests/visual/): runs the SAME scripted demo
//     match as --demo, saving a named "<label>.bmp" under <outdir> at each
//     requested tick instead of one final frame. All shots share one run, so
//     they stay consistent with each other. See tests/visual/README.md.
//
//   bomber_game --bm-shot <NAME> <out.bmp> [scroll] [game_dir] [scheme.sch]
//     Headless capture of one `.BM` text screen (the sub_41302D viewer):
//     renders NAME.BM (e.g. CREDITS) over the MAINMENU backdrop, scrolled
//     `scroll` lines down, and saves it as a BMP. The front-end analogue of
//     --demo for eyeballing / regression-checking the credits & help layout.
//
//   bomber_game --host <localPort> <peerHost> <peerPort> [--seed <n>] [game_dir] [scheme.sch]
//   bomber_game --join <localPort> <peerHost> <peerPort> [--seed <n>] [game_dir] [scheme.sch]
//     Netplay (ADR-0010 §3.3 step 5): run ONE 2-player UDP deterministic-
//     lockstep match instead of the front-end. --host drives seat 0, --join
//     seat 1; the local player uses the arrow keys + Right Ctrl either way.
//     There is no discovery/handshake yet, so BOTH peers bind a FIXED local
//     port and name each OTHER's host:port, and BOTH pass the SAME --seed
//     (decimal or 0x-hex, default 0x1234) so their arenas are byte-identical.
//     Two instances on one machine:
//       bomber_game --host 8000 127.0.0.1 8001 --seed 0x1234
//       bomber_game --join 8001 127.0.0.1 8000 --seed 0x1234
//
//   bomber_game --matchmaker <ws-url> [--matchmaker-stun <host[:port]>]
//     Point the ONLINE lobby (menu: Start Network Game -> HOST PRIVATE GAME /
//     JOIN BY CODE, ADR-0011) at a signaling server, e.g.
//     "ws://127.0.0.1:8080/ws" for a locally built services/matchmaker. Falls
//     back to $BOMBER_MATCHMAKER_URL, then to the compile-time placeholder in
//     game_app.cpp. --matchmaker-stun overrides the UDP STUN echo endpoint,
//     which otherwise defaults to the URL's host on port 8081.

#include <SDL3/SDL_main.h>

#include <cctype>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <utility>
#include <vector>

#include "bomber/game/game_app.hpp"

namespace {

// Parses "label:tick,label:tick,..." into (label, tick) pairs. A malformed
// entry (missing ':' or a non-numeric tick) is skipped rather than aborting
// the whole run — the caller simply never reaches that tick's save point.
std::vector<std::pair<std::string, int>> parse_demo_shots(const std::string& spec) {
    std::vector<std::pair<std::string, int>> shots;
    std::size_t start = 0;
    while (start <= spec.size()) {
        std::size_t comma = spec.find(',', start);
        std::string entry = spec.substr(start, comma == std::string::npos ? comma : comma - start);
        std::size_t colon = entry.find(':');
        if (colon != std::string::npos && colon > 0) {
            std::string label = entry.substr(0, colon);
            int tick = std::atoi(entry.c_str() + colon + 1);
            if (tick > 0) shots.emplace_back(std::move(label), tick);
        }
        if (comma == std::string::npos) break;
        start = comma + 1;
    }
    return shots;
}

}  // namespace

int main(int argc, char** argv) {
    bomber::game::GameApp::Options opts;
    std::vector<std::string> rest;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--demo" && i + 2 < argc) {
            opts.demo = true;
            opts.demo_ticks = std::atoi(argv[++i]);
            opts.demo_out = argv[++i];
        } else if (a == "--demo-shots" && i + 2 < argc) {
            opts.demo = true;
            opts.demo_shots = parse_demo_shots(argv[++i]);
            opts.demo_shot_dir = argv[++i];
        } else if (a == "--bm-shot" && i + 2 < argc) {
            // --bm-shot <NAME> <out.bmp> [scroll]: capture one .BM text screen.
            opts.demo = true;  // reuse the demo path's audio skip / headless intent
            opts.bm_shot_name = argv[++i];
            opts.bm_shot_out = argv[++i];
            if (i + 1 < argc && std::isdigit(static_cast<unsigned char>(argv[i + 1][0])))
                opts.bm_shot_scroll = std::atoi(argv[++i]);
        } else if (a == "--menu-shot" && i + 1 < argc) {
            // --menu-shot <out.bmp>: capture the main-menu composite for pixel
            // comparison against the native oracle's --boot-shot menu render.
            opts.demo = true;  // reuse the demo path's audio skip / headless intent
            opts.menu_shot_out = argv[++i];
        } else if (a == "--match") {
            opts.boot_match = true;  // skip the front-end, boot straight into a match
        } else if ((a == "--host" || a == "--join") && i + 3 < argc) {
            // --host/--join <localPort> <peerHost> <peerPort>: run ONE 2-player
            // UDP netplay match (host = seat 0, join = seat 1). Both peers bind
            // their fixed local port and point at the other's host:port (no
            // discovery). Same --seed on both -> identical arena.
            opts.net_role = (a == "--host") ? 1 : 2;
            opts.net_local_port = static_cast<std::uint16_t>(std::atoi(argv[++i]));
            opts.net_peer_host = argv[++i];
            opts.net_peer_port = static_cast<std::uint16_t>(std::atoi(argv[++i]));
        } else if (a == "--matchmaker" && i + 1 < argc) {
            // --matchmaker <ws-url>: the online lobby's signaling server
            // (ADR-0011). Highest-precedence source; then BOMBER_MATCHMAKER_URL,
            // then the compile-time placeholder in game_app.cpp.
            opts.matchmaker_url = argv[++i];
        } else if (a == "--matchmaker-stun" && i + 1 < argc) {
            // --matchmaker-stun <host[:port]>: the matchmaker's UDP STUN echo
            // (PROTOCOL.md §2). Defaults to the --matchmaker URL's own host and
            // port 8081, so it is usually unnecessary.
            const std::string s = argv[++i];
            const std::size_t colon = s.rfind(':');
            if (colon == std::string::npos) {
                opts.matchmaker_stun_host = s;
            } else {
                opts.matchmaker_stun_host = s.substr(0, colon);
                opts.matchmaker_stun_port =
                    static_cast<std::uint16_t>(std::atoi(s.c_str() + colon + 1));
            }
        } else if (a == "--seed" && i + 1 < argc) {
            // --seed <n>: shared netplay match seed (decimal or 0x-hex). BOTH
            // peers must pass the SAME value for byte-identical arenas.
            opts.net_seed = static_cast<std::uint32_t>(std::strtoul(argv[++i], nullptr, 0));
        } else {
            rest.push_back(a);
        }
    }
    if (!rest.empty()) opts.game_dir = rest[0];
    if (rest.size() > 1) opts.scheme = rest[1];

    bomber::game::GameApp app(std::move(opts));
    return app.run();
}
