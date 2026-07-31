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
//   bomber_game --demo-players <N> / --demo-seed <n>
//     Dev-only modifiers for the two --demo paths above (used to render the
//     README's match animation): run the capture with N COMPUTER slots instead
//     of the default 1 human + 1 AI roster — a human slot just stands still in
//     a headless run — and/or start the match from a different seed than the
//     fixed 0xB0BB1E5 (which also re-rolls the level, brick fill and powerups).
//     tests/visual/ passes NEITHER, so the pinned golden frames are unaffected.
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
//     "ws://127.0.0.1:8080/ws" for a locally built services/matchmaker (plain
//     ws:// because a local instance has no certificate; the deployed default
//     is wss://). Falls back to $BOMBER_MATCHMAKER_URL, then to the
//     compile-time default in game_app.cpp. --matchmaker-stun overrides the UDP
//     STUN echo endpoint, which otherwise defaults to the URL's host port 8081.

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

using bomber::game::GameApp;

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

// A cursor over argv. Every flag below is "the flag word, then N operands", and
// each one used to re-derive that shape inline: check `i + N < argc`, then read
// through `argv[++i]` as many times as it needed. Naming the cursor lets a flag
// handler ASK for its operands (`take()`) and lets the dispatcher ask once
// whether they exist (`has(N)`), which is what collapses the chain below into
// one arm per flag.
class ArgCursor {
public:
    ArgCursor(int argc, char** argv) : argc_(argc), argv_(argv) {}

    bool done() const { return i_ >= argc_; }
    // The flag word itself, consumed.
    std::string next() { return argv_[i_++]; }
    // Are there at least `n` operands left after the word just consumed?
    bool has(int n) const { return i_ + n - 1 < argc_; }
    // One operand.
    const char* take() { return argv_[i_++]; }
    // Peek without consuming — only `--bm-shot`'s optional trailing scroll count
    // needs this, since it is told apart from the next positional by being numeric.
    const char* peek() const { return i_ < argc_ ? argv_[i_] : nullptr; }

private:
    int argc_;
    char** argv_;
    int i_ = 1;  // argv[0] is the program name
};

// "host[:port]" for the matchmaker's UDP STUN echo (PROTOCOL.md §2). A bare
// host leaves the port at its default of 8081.
void set_stun_endpoint(GameApp::Options& opts, const std::string& s) {
    const std::size_t colon = s.rfind(':');
    if (colon == std::string::npos) {
        opts.matchmaker_stun_host = s;
        return;
    }
    opts.matchmaker_stun_host = s.substr(0, colon);
    opts.matchmaker_stun_port = static_cast<std::uint16_t>(std::atoi(s.c_str() + colon + 1));
}

// --host/--join <localPort> <peerHost> <peerPort>: run ONE 2-player UDP netplay
// match (host = seat 0, join = seat 1). Both peers bind their fixed local port
// and point at the other's host:port (no discovery). Same --seed on both ->
// identical arena.
void set_netplay_peer(GameApp::Options& opts, bool is_host, ArgCursor& args) {
    opts.net_role = is_host ? 1 : 2;
    opts.net_local_port = static_cast<std::uint16_t>(std::atoi(args.take()));
    opts.net_peer_host = args.take();
    opts.net_peer_port = static_cast<std::uint16_t>(std::atoi(args.take()));
}

// The headless capture flags and the boot mode. Returns false when `a` is not
// one of them, or when its operands are missing — the caller then files it as a
// positional, which is the original behaviour: a truncated flag falls through
// to the game_dir/scheme slots rather than erroring.
bool apply_capture_flag(const std::string& a, GameApp::Options& opts, ArgCursor& args) {
    if (a == "--demo" && args.has(2)) {
        opts.demo = true;
        opts.demo_ticks = std::atoi(args.take());
        opts.demo_out = args.take();
    } else if (a == "--demo-shots" && args.has(2)) {
        opts.demo = true;
        opts.demo_shots = parse_demo_shots(args.take());
        opts.demo_shot_dir = args.take();
    } else if (a == "--demo-players" && args.has(1)) {
        opts.demo_players = std::atoi(args.take());
    } else if (a == "--demo-seed" && args.has(1)) {
        opts.demo_seed = static_cast<std::uint32_t>(std::strtoul(args.take(), nullptr, 0));
    } else if (a == "--bm-shot" && args.has(2)) {
        // --bm-shot <NAME> <out.bmp> [scroll]: capture one .BM text screen. The
        // optional scroll count is told apart from the next positional by being
        // numeric, so it is peeked before it is consumed.
        opts.demo = true;  // reuse the demo path's audio skip / headless intent
        opts.bm_shot_name = args.take();
        opts.bm_shot_out = args.take();
        const char* scroll = args.peek();
        if (scroll && std::isdigit(static_cast<unsigned char>(scroll[0])))
            opts.bm_shot_scroll = std::atoi(args.take());
    } else if (a == "--menu-shot" && args.has(1)) {
        // --menu-shot <out.bmp>: capture the main-menu composite for pixel
        // comparison against the native oracle's --boot-shot menu render.
        opts.demo = true;  // reuse the demo path's audio skip / headless intent
        opts.menu_shot_out = args.take();
    } else if (a == "--match") {
        opts.boot_match = true;  // skip the front-end, boot straight into a match
    } else {
        return false;
    }
    return true;
}

// The netplay flags: the fixed-peer CLI pair, and the online lobby's signaling
// configuration. Same "false means positional" contract as above.
bool apply_net_flag(const std::string& a, GameApp::Options& opts, ArgCursor& args) {
    if ((a == "--host" || a == "--join") && args.has(3)) {
        set_netplay_peer(opts, a == "--host", args);
    } else if (a == "--matchmaker" && args.has(1)) {
        // --matchmaker <ws-url>: the online lobby's signaling server (ADR-0011).
        // Highest-precedence source; then BOMBER_MATCHMAKER_URL, then the
        // compile-time placeholder in game_app.cpp.
        opts.matchmaker_url = args.take();
    } else if (a == "--matchmaker-stun" && args.has(1)) {
        set_stun_endpoint(opts, args.take());
    } else if (a == "--seed" && args.has(1)) {
        // --seed <n>: shared netplay match seed (decimal or 0x-hex). BOTH peers
        // must pass the SAME value for byte-identical arenas.
        opts.net_seed = static_cast<std::uint32_t>(std::strtoul(args.take(), nullptr, 0));
    } else {
        return false;
    }
    return true;
}

// The two groups handle disjoint flag words, so trying them in order is the
// same single chain the parser used to be.
bool apply_flag(const std::string& a, GameApp::Options& opts, ArgCursor& args) {
    return apply_capture_flag(a, opts, args) || apply_net_flag(a, opts, args);
}

GameApp::Options parse_options(int argc, char** argv) {
    GameApp::Options opts;
    ArgCursor args(argc, argv);
    std::vector<std::string> rest;
    while (!args.done()) {
        const std::string a = args.next();
        if (!apply_flag(a, opts, args)) rest.push_back(a);
    }
    if (!rest.empty()) opts.game_dir = rest[0];
    if (rest.size() > 1) opts.scheme = rest[1];
    return opts;
}

}  // namespace

int main(int argc, char** argv) {
    GameApp app(parse_options(argc, argv));
    return app.run();
}
