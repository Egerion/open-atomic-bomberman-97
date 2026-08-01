#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

// The command line, parsed (apps/game/main.cpp), plus the one predicate derived
// from it that the whole shell asks.

namespace bomber::game {

struct AppOptions {
    std::filesystem::path game_dir;  // empty: auto-detect (bomber::assets)
    std::filesystem::path scheme;    // empty: DATA/SCHEMES/BASIC.SCH

    bool demo = false;               // headless scripted run + screenshot(s)
    int demo_ticks = 0;              // single-shot mode: run this many ticks
    std::filesystem::path demo_out;  // single-shot mode: BMP output path
    // Visual golden harness (--demo-shots): (label, tick) pairs captured within ONE
    // scripted run. A non-empty list OVERRIDES the single-shot fields above — the
    // run advances to the highest requested tick, saving a frame at each one.
    // tests/visual/README.md has the recapture procedure.
    std::vector<std::pair<std::string, int>> demo_shots;
    std::filesystem::path demo_shot_dir;
    // Dev-only knobs used to render the README's match animation. tests/visual/
    // passes NEITHER, so its pinned frames stay independent of them.
    int demo_players = 0;                  // 0 = keep the default roster
    std::uint32_t demo_seed = 0xB0BB1E5u;  // the historic --demo match seed

    std::string bm_shot_name;  // one-frame sub_41302D `.BM` capture, e.g. "CREDITS"
    std::filesystem::path bm_shot_out;
    int bm_shot_scroll = 0;               // lines scrolled down before capture
    std::filesystem::path menu_shot_out;  // one-frame main-menu composite capture

    bool boot_match = false;  // dev fast-path; also env BOMBER_BOOT_MATCH

    // Netplay CLI (ADR-0010 §3.3 step 5): a non-zero role runs ONE networked
    // 2-player UDP match instead of the front end. No discovery on this path — both
    // peers name each other's host:port and pass the SAME seed, which with the
    // canonical config gives byte-identical arenas.
    int net_role = 0;                  // 0 = none, 1 = host (seat 0), 2 = guest (seat 1)
    std::uint16_t net_local_port = 0;  // host binds this; guest binds ephemeral
    std::string net_peer_host;         // dotted IPv4 or name
    std::uint16_t net_peer_port = 0;
    std::uint32_t net_seed = 0x1234u;  // must match on both peers

    // Online lobby endpoints (ADR-0011 Phase 1d). Empty falls through to the
    // BOMBER_MATCHMAKER_* env vars, then to netplay_runner.cpp's compile-time
    // defaults, which name the DEPLOYED matchmaker — so the online rows work with no
    // flags. The STUN host defaults to the URL's own host (PROTOCOL.md §2).
    std::string matchmaker_url;
    std::string matchmaker_stun_host;
    std::uint16_t matchmaker_stun_port = 0;  // 0 = unset
};

// Is this run a PIXEL CAPTURE rather than someone playing? Captures must be
// hermetic — identical inputs, identical pixels, on any machine — so anything in
// the player's options.ini that could move a pixel is pinned for them
// (app_settings.cpp).
//
// This is ONE predicate because it was not. `playtime` was pinned for exactly this
// reason, then the three PORT-ONLY Video Settings keys were added later and nobody
// pinned them, so simply playing with the FPS readout on silently broke all five
// visual pins and an investigation blamed an innocent commit before the real cause
// was found. `demo` is listed even though setting it is how --demo asks for this:
// the other three entry points are equally captures and were the ones being missed.
inline bool is_capture_run(const AppOptions& o) {
    return o.demo || o.demo_ticks > 0 || !o.demo_shots.empty() || !o.bm_shot_name.empty() ||
           !o.menu_shot_out.empty();
}

}  // namespace bomber::game
