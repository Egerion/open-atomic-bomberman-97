// bomber_game — the playable front end, rendering the deterministic sim with the
// original game's assets (loaded at runtime from the player's own install).
//
//   bomber_game [game_dir] [scheme.sch]      boot -> title -> menu -> match
//   --match                                  skip the front end, boot into a match
//   --demo <ticks> <out.bmp>                 scripted headless run, save the last frame
//   --demo-shots <label:tick,...> <outdir>   the visual golden harness: one run, a
//                                            named frame at each requested tick
//   --demo-players <N> / --demo-seed <n>     dev-only modifiers for the --demo paths
//   --bm-shot <NAME> <out.bmp> [scroll]      capture one `.BM` text screen
//   --menu-shot <out.bmp>                    capture the main-menu composite
//   --host|--join <localPort> <peerHost> <peerPort> [--seed <n>]
//                                            one 2-player UDP lockstep match, seat 0|1
//   --matchmaker <ws-url> [--matchmaker-stun <host[:port]>]
//                                            the ONLINE lobby's signaling server
//
// --host/--join have no discovery: both peers bind a fixed local port, name the
// OTHER's host:port, and must pass the SAME seed or their arenas differ.
//   bomber_game --host 8000 127.0.0.1 8001 --seed 0x1234
//   bomber_game --join 8001 127.0.0.1 8000 --seed 0x1234
// --matchmaker takes plain ws:// for a locally built services/matchmaker, which has
// no certificate; the deployed default is wss://.

#include <SDL3/SDL_main.h>

#include <cctype>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <utility>
#include <vector>

#include "bomber/game/app_options.hpp"
#include "bomber/game/game_app.hpp"

namespace {

using bomber::game::AppOptions;
using bomber::game::GameApp;

// Parses "label:tick,label:tick,..." into (label, tick) pairs. A malformed entry
// (missing ':' or a non-numeric tick) is skipped rather than aborting the whole
// run — the caller simply never reaches that tick's save point.
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
// each one used to re-derive that shape inline. Naming the cursor lets a flag
// handler ASK for its operands (`take()`) and lets the dispatcher ask once
// whether they exist (`has(N)`), which is what collapses the chain into one arm
// per flag.
class ArgCursor {
public:
    ArgCursor(int argc, char** argv) : argc_(argc), argv_(argv) {}

    bool done() const { return i_ >= argc_; }
    std::string next() { return argv_[i_++]; }  // the flag word itself, consumed
    // Are there at least `n` operands left after the word just consumed?
    bool has(int n) const { return i_ + n - 1 < argc_; }
    const char* take() { return argv_[i_++]; }
    // Peek without consuming — only --bm-shot's optional trailing scroll count
    // needs it, since it is told apart from the next positional by being numeric.
    const char* peek() const { return i_ < argc_ ? argv_[i_] : nullptr; }

private:
    int argc_;
    char** argv_;
    int i_ = 1;  // argv[0] is the program name
};

// "host[:port]"; a bare host leaves the port at its default of 8081.
void set_stun_endpoint(AppOptions& opts, const std::string& s) {
    const std::size_t colon = s.rfind(':');
    if (colon == std::string::npos) {
        opts.matchmaker_stun_host = s;
        return;
    }
    opts.matchmaker_stun_host = s.substr(0, colon);
    opts.matchmaker_stun_port = static_cast<std::uint16_t>(std::atoi(s.c_str() + colon + 1));
}

void set_netplay_peer(AppOptions& opts, bool is_host, ArgCursor& args) {
    opts.net_role = is_host ? 1 : 2;
    opts.net_local_port = static_cast<std::uint16_t>(std::atoi(args.take()));
    opts.net_peer_host = args.take();
    opts.net_peer_port = static_cast<std::uint16_t>(std::atoi(args.take()));
}

// The headless capture flags and the boot mode. Returns false when `a` is not one
// of them, or when its operands are missing — the caller then files it as a
// positional, which is the original behaviour: a truncated flag falls through to
// the game_dir/scheme slots rather than erroring.
bool apply_capture_flag(const std::string& a, AppOptions& opts, ArgCursor& args) {
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
        opts.demo = true;  // reuse the demo path's audio skip / headless intent
        opts.bm_shot_name = args.take();
        opts.bm_shot_out = args.take();
        // The optional scroll count is told apart from the next positional by
        // being numeric, so it is peeked before it is consumed.
        const char* scroll = args.peek();
        if (scroll && std::isdigit(static_cast<unsigned char>(scroll[0])))
            opts.bm_shot_scroll = std::atoi(args.take());
    } else if (a == "--menu-shot" && args.has(1)) {
        opts.demo = true;
        opts.menu_shot_out = args.take();
    } else if (a == "--match") {
        opts.boot_match = true;
    } else {
        return false;
    }
    return true;
}

// The netplay flags. Same "false means positional" contract as above.
bool apply_net_flag(const std::string& a, AppOptions& opts, ArgCursor& args) {
    if ((a == "--host" || a == "--join") && args.has(3)) {
        set_netplay_peer(opts, a == "--host", args);
    } else if (a == "--matchmaker" && args.has(1)) {
        opts.matchmaker_url = args.take();
    } else if (a == "--matchmaker-stun" && args.has(1)) {
        set_stun_endpoint(opts, args.take());
    } else if (a == "--seed" && args.has(1)) {
        opts.net_seed = static_cast<std::uint32_t>(std::strtoul(args.take(), nullptr, 0));
    } else {
        return false;
    }
    return true;
}

// The two groups handle disjoint flag words, so trying them in order is the same
// single chain the parser used to be.
bool apply_flag(const std::string& a, AppOptions& opts, ArgCursor& args) {
    return apply_capture_flag(a, opts, args) || apply_net_flag(a, opts, args);
}

AppOptions parse_options(int argc, char** argv) {
    AppOptions opts;
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
