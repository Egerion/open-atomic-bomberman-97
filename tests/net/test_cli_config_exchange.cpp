// THE CROSS-BUILD DOOR FOR BOARD DERIVATION — the hole that `build_hash` does
// not and cannot cover, and how the CLI netplay path closes it.
//
// The problem, concretely. `GameApp::canonical_netplay_config` (the
// `--host`/`--join` entry) derives the whole board LOCALLY ON BOTH PEERS: the
// .SCH brick fill, `pick_stage`, `EXTRA<n>.RES` and `match::apply_actors`. All
// of that lives in libs/match, which is:
//   * outside `build_hash`'s scope — every scenario in build_hash.cpp
//     hand-builds a `MatchConfig` on already-blank cells and never calls
//     `apply_actors`, so a libs/match change leaves the digest byte-identical;
//   * and irrelevant to this path anyway, because the CLI runs NO handshake at
//     all — no `SeedHandshake` (the seed is on both command lines), no
//     `build_hash` exchange, no protocol-version field. Nothing on that wire
//     could refuse a mismatched build even if the digest had moved.
//
// So a board-derivation change shipped to one peer and not the other produced
// two different boards from the same seed and a tick-0 desync with nothing to
// catch it. That is not hypothetical: dropping `apply_actors`' actor-tile
// blanking (docs/re/facts.md "Stage actors do not clear the tile they sit on")
// moves up to 43% of a stage's bricks and moves neither the goldens nor
// `build_hash`.
//
// The closure is STRUCTURAL, not another digest: the CLI now ships the host's
// serialized `sim::MatchConfig` over the same `net::SetupSession` the
// interactive lobby path already uses, so the board is derived ONCE per match
// instead of once per peer. This suite pins the two properties that closure
// rests on, over a real LoopbackLink:
//
//   1. THE GUEST'S OWN DERIVATION IS OVERRULED. Even when the guest would build
//      a materially different board from the same seed — modelled here by the
//      exact old-vs-new `apply_actors` divergence — it ends up holding the
//      HOST's bytes, field for field. That is what makes any future libs/match
//      change unable to desync this path, whether or not anyone remembers to
//      move a digest.
//   2. AN UNPATCHED PARTNER IS REFUSED, NOT TOLERATED. A build from before this
//      change neither sends nor answers setup traffic. Modelled here as a peer
//      that never pumps: the patched side reaches `Phase::Failed` and the caller
//      aborts the match, rather than starting on a board the other side does not
//      have. Both directions are checked.
//
// The GameApp glue itself (SDL window, event pump) is not reachable from a
// headless test — it is validated live, like every other `--host`/`--join`
// behaviour. What IS testable, and what actually carries the guarantee, is the
// SetupSession contract underneath it, which is what this file drives.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <cstdint>
#include <string>
#include <vector>

#include "bomber/assets/extra.hpp"
#include "bomber/match/match_factory.hpp"
#include "bomber/net/setup_session.hpp"
#include "bomber/net/transport.hpp"
#include "bomber/sim/match_config.hpp"
#include "match_config_compare.hpp"

using namespace bomber;  // NOLINT(google-build-using-namespace) — test-local

namespace {

constexpr std::uint16_t kHostSeat = 0b01;
constexpr std::uint16_t kGuestSeat = 0b10;

// A board dense enough that a stage actor's tile is a brick, plus the stock
// actor mix of a real EXTRA<n>.RES: conveyors and dirarrows (which must NOT
// clear) and one warphole (which must).
std::vector<assets::extra::Actor> stock_actors() {
    std::vector<assets::extra::Actor> a;
    auto add = [&](assets::extra::Kind k, int x, int y) {
        assets::extra::Actor act;
        act.kind = k;
        act.x = x;
        act.y = y;
        a.push_back(act);
    };
    add(assets::extra::Kind::Conveyor, 2, 2);
    add(assets::extra::Kind::Conveyor, 4, 2);
    add(assets::extra::Kind::DirArrow, 6, 4);
    add(assets::extra::Kind::DirArrow, 8, 4);
    add(assets::extra::Kind::Trampoline, 10, 6);
    add(assets::extra::Kind::Warphole, 12, 8);
    return a;
}

sim::MatchConfig brick_board() {
    sim::MatchConfig cfg;
    for (int y = 0; y < sim::kGridHeight; ++y)
        for (int x = 0; x < sim::kGridWidth; ++x) cfg.cells[y][x] = sim::Cell::Brick;
    cfg.player_count = 2;
    cfg.seed = 0xC0FFEEu;
    cfg.spawns = {{0, 0}, {sim::kGridWidth - 1, sim::kGridHeight - 1}};
    return cfg;
}

// What a PATCHED build derives: only the warphole clears.
sim::MatchConfig patched_board() {
    sim::MatchConfig cfg = brick_board();
    match::apply_actors(cfg, stock_actors(), 0xC0FFEEu);
    return cfg;
}

// What an UNPATCHED build derived: `place()` blanked every actor tile. Rebuilt
// here from the patched board rather than kept as a copy of the old code, so
// this stays a statement about the DIFFERENCE (five extra cleared tiles) and
// cannot silently agree with the new behaviour.
sim::MatchConfig legacy_board() {
    sim::MatchConfig cfg = patched_board();
    for (const auto& a : stock_actors())
        if (a.kind != assets::extra::Kind::Warphole) cfg.cells[a.y][a.x] = sim::Cell::Blank;
    return cfg;
}

int count_bricks(const sim::MatchConfig& cfg) {
    int n = 0;
    for (int y = 0; y < sim::kGridHeight; ++y)
        for (int x = 0; x < sim::kGridWidth; ++x)
            if (cfg.cells[y][x] == sim::Cell::Brick) ++n;
    return n;
}

// The pump the CLI runs, minus SDL: both peers step until the host has the
// guest's ack, with a bounded clock so a hang is a failure rather than a wait.
bool run_exchange(net::LoopbackLink& link, net::SetupSession& host, net::SetupSession& guest,
                  int max_ms = 20000) {
    for (std::int64_t now = 0; now < max_ms; now += 10) {
        host.step(now);
        guest.step(now);
        link.step();  // advance the link's own delivery clock (latency cases)
        if (host.phase() == net::SetupSession::Phase::Final &&
            guest.phase() == net::SetupSession::Phase::Final)
            break;
        if (host.failed() || guest.failed()) break;
    }
    return host.phase() == net::SetupSession::Phase::Final &&
           guest.phase() == net::SetupSession::Phase::Final;
}

// Every MatchConfig field the wire comparator knows about, named, as one string.
std::string diff_names(const sim::MatchConfig& a, const sim::MatchConfig& b) {
    std::string s;
    for (const auto& n : sim::test::match_config_diffs(a, b)) s += n + " ";
    return s;
}

}  // namespace

TEST_CASE("the divergence this closes is real: the two derivations differ") {
    const sim::MatchConfig patched = patched_board();
    const sim::MatchConfig legacy = legacy_board();

    // Five non-warphole actors: the legacy build opened five tiles the original
    // (and now the port) leaves bricked.
    CHECK(count_bricks(legacy) == count_bricks(patched) - 5);

    // And the warphole's own tile is cleared by the patched derivation — the one
    // case sub_4056CA really does write (docs/re/facts.md "Stage actors do not
    // clear the tile they sit on"). The legacy board shares it by construction
    // (legacy_board() copies the patched board and never touches this cell, and
    // its seed the same way), so neither is re-asserted here.
    CHECK(patched.cells[8][12] == sim::Cell::Blank);

    // Sanity: this is exactly the shape of a tick-0 desync — same seed, same
    // actors, different cells.
    bool differ = false;
    for (int y = 0; y < sim::kGridHeight && !differ; ++y)
        for (int x = 0; x < sim::kGridWidth; ++x)
            if (patched.cells[y][x] != legacy.cells[y][x]) {
                differ = true;
                break;
            }
    CHECK(differ);
}

TEST_CASE("CLI exchange: the guest adopts the HOST's board, not the one it would derive") {
    net::LoopbackLink link;
    net::LoopbackTransport host_t(link, 0), guest_t(link, 1);

    net::SetupSession host(host_t, /*is_host=*/true, kHostSeat, kGuestSeat);
    net::SetupSession guest(guest_t, /*is_host=*/false, kGuestSeat, 0);

    // The host is the patched build; the guest, left to itself, would derive the
    // legacy board. Only the host's derivation is allowed to matter.
    const sim::MatchConfig host_cfg = patched_board();
    const sim::MatchConfig guest_would_derive = legacy_board();
    host.confirm(host_cfg);

    REQUIRE(run_exchange(link, host, guest));
    REQUIRE(guest.has_final_config());

    CHECK_MESSAGE(diff_names(host_cfg, guest.final_config()).empty(),
                  "fields that differ: " << diff_names(host_cfg, guest.final_config()));

    // The point of the whole exercise: what the guest holds is NOT what it would
    // have built for itself.
    CHECK(count_bricks(guest.final_config()) == count_bricks(host_cfg));
    CHECK(count_bricks(guest.final_config()) != count_bricks(guest_would_derive));
    // (guest_would_derive's own (2,2) blank is legacy_board()'s unconditional
    // write, not an outcome — asserting it back would restate the fixture.)
    CHECK(guest.final_config().cells[2][2] == sim::Cell::Brick);   // conveyor, kept
    CHECK(guest.final_config().cells[8][12] == sim::Cell::Blank);  // warphole, cleared
}

TEST_CASE("CLI exchange: the reverse pairing is symmetric — the host's bytes still win") {
    // Same test with the roles' derivations swapped: an OLD host and a NEW guest
    // would also have disagreed, and the exchange resolves it the same way — the
    // host is authoritative, whichever build it is. This is why the closure is
    // structural rather than a statement about one particular fix.
    net::LoopbackLink link;
    net::LoopbackTransport host_t(link, 0), guest_t(link, 1);
    net::SetupSession host(host_t, /*is_host=*/true, kHostSeat, kGuestSeat);
    net::SetupSession guest(guest_t, /*is_host=*/false, kGuestSeat, 0);

    const sim::MatchConfig host_cfg = legacy_board();
    host.confirm(host_cfg);
    REQUIRE(run_exchange(link, host, guest));

    CHECK_MESSAGE(diff_names(host_cfg, guest.final_config()).empty(),
                  "fields that differ: " << diff_names(host_cfg, guest.final_config()));
    CHECK(guest.final_config().cells[2][2] == sim::Cell::Blank);
}

TEST_CASE("CLI exchange survives packet loss, so the refusal below means a DEAD peer") {
    // The failure cases only prove something if a merely lossy link still
    // succeeds — otherwise "timed out" would be the normal outcome and refusing
    // on it would be a bug, not a guard.
    net::LoopbackLink link(/*latency=*/2, /*drop_every=*/3);
    net::LoopbackTransport host_t(link, 0), guest_t(link, 1);
    net::SetupSession host(host_t, /*is_host=*/true, kHostSeat, kGuestSeat);
    net::SetupSession guest(guest_t, /*is_host=*/false, kGuestSeat, 0);

    host.confirm(patched_board());
    REQUIRE(run_exchange(link, host, guest));
    CHECK_MESSAGE(diff_names(patched_board(), guest.final_config()).empty(),
                  "fields that differ: " << diff_names(patched_board(), guest.final_config()));
}

TEST_CASE("an UNPATCHED guest is refused: a peer that sends no setup ack times the host out") {
    // A build from before this change runs no SetupSession on the CLI path at
    // all: it neither answers chunks nor acks. Modelled exactly — the guest side
    // is never pumped.
    net::LoopbackLink link;
    net::LoopbackTransport host_t(link, 0), guest_t(link, 1);
    (void)guest_t;  // bound, but silent: the "old build" end of the wire

    constexpr int kTimeoutMs = 15000;  // GameApp::exchange_cli_netplay_config's
    net::SetupSession host(host_t, /*is_host=*/true, kHostSeat, kGuestSeat, kTimeoutMs);
    host.confirm(patched_board());

    for (std::int64_t now = 0; now <= kTimeoutMs + 100 && !host.failed(); now += 10) host.step(now);

    // REFUSED, not started: the caller aborts on failed() and the match never
    // reaches tick 0 on two different boards.
    CHECK(host.failed());
    CHECK(host.phase() != net::SetupSession::Phase::Final);
    CHECK_FALSE(host.peer_acked());
}

TEST_CASE("an UNPATCHED host is refused: a peer that sends no config times the guest out") {
    net::LoopbackLink link;
    net::LoopbackTransport host_t(link, 0), guest_t(link, 1);
    (void)host_t;  // the "old build" end: it starts the match without a word

    constexpr int kTimeoutMs = 15000;
    net::SetupSession guest(guest_t, /*is_host=*/false, kGuestSeat, 0, kTimeoutMs);

    for (std::int64_t now = 0; now <= kTimeoutMs + 100 && !guest.failed(); now += 10)
        guest.step(now);

    CHECK(guest.failed());
    CHECK_FALSE(guest.has_final_config());
}
