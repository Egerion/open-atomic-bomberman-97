// Head-hit loss bounds (sub_421F7E / sub_42331C flight block).
//
// Pins the faithful per-incident ceiling investigated for the "max_bombs
// collapsed while alive" report (whose actual root cause was the chain-slot
// leak — see test_chain_slot.cpp): one head hit drops
// getvalue(670) + rand % getvalue(671) = 1..3 KINDS (shipped VALUELST:
// 670=1, 671=3), each -1 of one kind, so a single hit can never cost more
// than 3 ExtraBombs. The original has NO per-flight hit latch (pseudo.c
// 25445-25448: sub_421F7E fires, then the bomb advances one tile and keeps
// flying in the same loop) — a re-hit needs the bomb to RE-CROSS the victim
// (jelly veer or field wrap), which our fly()/re-hop chain reproduces; on a
// standard pillars board one flight yields at most one hit.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <algorithm>

#include "helpers.hpp"

using namespace bomber::sim;
using namespace bomber::sim::test;

namespace {

Fixed center_x(int tx) { return tx * kTileWF + kTileWF / 2; }
Fixed center_y(int ty) { return ty * kTileHF + kTileHF / 2; }
int ddx(Direction d) { return d == Direction::Left ? -1 : d == Direction::Right ? 1 : 0; }
int ddy(Direction d) { return d == Direction::Up ? -1 : d == Direction::Down ? 1 : 0; }

int count_headhits(const State& s, int victim) {
    int n = 0;
    for (const auto& e : s.events)
        if (e.type == Event::Type::HeadHit && e.player == victim) ++n;
    return n;
}

// White-box: a bomb already mid-air, aimed `tiles` away in `d`, mirroring
// what BombSystem::launch builds (punched_bomb_speed 1300).
Bomb make_flying(std::uint8_t owner, int from_tx, int from_ty, Direction d, int tiles,
                 bool jelly) {
    Bomb b;
    b.active = true;
    b.owner = owner;
    b.flame = 1;
    b.fuse = 100000;  // never fuse-explodes during the window
    b.jelly = jelly;
    b.x = center_x(from_tx);
    b.y = center_y(from_ty);
    b.flying = true;
    b.dir = d;
    b.from_x = b.x;
    b.from_y = b.y;
    b.to_x = b.x + ddx(d) * tiles * kTileWF;
    b.to_y = b.y + ddy(d) * tiles * kTileHF;
    Fixed dist = tiles * (ddx(d) != 0 ? kTileWF : kTileHF);
    b.fly_total = std::max<std::int32_t>(1, dist / 1300);
    b.fly_ticks = b.fly_total;
    return b;
}

}  // namespace

TEST_CASE("a straight throw onto a lone victim head-hits exactly once, losing at most 3 kinds") {
    Simulation s(open_config());
    Player& v = s.state().players[1];
    v.x = center_x(7);
    v.y = center_y(4);  // (7,4): open — odd,odd tiles are solid pillars
    v.max_bombs = 5;    // 4 surplus ExtraBomb — the only surplus kind
    v.flame = s.state().tuning.start_with[1];
    v.kick = false;
    s.state().bombs.push_back(make_flying(0, 3, 4, Direction::Right, 4, false));

    int hits = 0;
    for (int t = 0; t < 60; ++t) {
        s.tick(TickInputs{});
        hits += count_headhits(s.state(), 1);
    }
    CHECK(v.alive);
    CHECK(hits == 1);
    // powers_lost_min=1 + rand%powers_lost_rand=3 -> 1..3 kinds per hit; with
    // ExtraBomb the sole surplus every accepted roll lands on it.
    CHECK(v.max_bombs >= 5 - 3);
    CHECK(v.max_bombs <= 5 - 1);
}

TEST_CASE("jelly bounce chains never exceed the 1..3-kind loss per head hit") {
    // A flying jelly can re-cross the victim only via veer rolls / wrap; every
    // individual hit still obeys the 670/671 bound. Multi-seed sweep with the
    // victim in a pillar pocket to maximize re-crossings.
    //
    // COVERAGE PIN (2026-07-31): both bounds below are satisfied by hits == 0 —
    // no hit means no drop, so `worst_single_tick_drop <= 3` and
    // `9 - max_bombs <= 3 * hits` both hold trivially. That made this case green
    // for the one regression it exists to catch: a change that stops the flying
    // jelly reaching the victim at all. `total_hits` is therefore accumulated
    // across the sweep and pinned non-zero at the end, so a scenario that stops
    // landing hits fails as the loss of coverage it is rather than passing as a
    // bound that was never tested. It is an AGGREGATE floor deliberately —
    // whether any individual seed lands a hit is a property of the veer rolls,
    // not of the ceiling under test.
    int total_hits = 0;
    for (int seedbump = 0; seedbump < 8; ++seedbump) {
        MatchConfig cfg = open_config();
        cfg.seed = 7 + seedbump * 1000;
        Simulation s(cfg);
        Player& v = s.state().players[1];
        v.x = center_x(2);
        v.y = center_y(2);
        v.max_bombs = 9;
        v.flame = cfg.tuning.start_with[1];
        v.kick = false;
        s.state().bombs.push_back(make_flying(0, 2, 6, Direction::Up, 4, true));

        int hits = 0, worst_single_tick_drop = 0, prev = v.max_bombs;
        for (int t = 0; t < 120; ++t) {
            s.tick(TickInputs{});
            hits += count_headhits(s.state(), 1);
            worst_single_tick_drop = std::max(worst_single_tick_drop, prev - v.max_bombs);
            prev = v.max_bombs;
            if (!v.alive) break;
        }
        CHECK(worst_single_tick_drop <= 3);   // one hit's ceiling
        CHECK(9 - v.max_bombs <= 3 * hits);   // total loss bounded by hits
        total_hits += hits;
    }
    CHECK(total_hits > 0);  // the sweep actually head-hit somebody
}
