// Enclosure fidelity fix — F1: "Stomped Bombs Detonate" (wall_detonates) ON
// crushes only the FIRST grounded bomb found per drop event, not every bomb
// sharing the crushed tile.
//
// docs/re/audit/enclosure.md Finding 1: `sub_426818`'s bomb-crush loop
// (native/src/game/batch_0x42583B.cpp lines 769-820) queues the found bomb
// by calling sub_423209 on it with -1 as the second argument (pseudo.c 27262)
// and then jumping unconditionally to LABEL_47 (pseudo.c 27263) — there is no
// path back to the top of that search loop on the ON branch, so it runs at
// most once per drop
// event. `EnclosureSystem::drop_wall`'s ON branch used to have no `break`,
// so it queued EVERY matching bomb on the tile. Two grounded (non-flying)
// bombs sharing one tile is not reachable via this port's own placement/kick
// collision rules (docs/re/audit/enclosure.md Finding 1's own
// Medium-confidence-on-reachability note) — constructed here white-box,
// directly via State::bombs, to pin the control-flow fix regardless.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "helpers.hpp"

using namespace bomber::sim;
using namespace bomber::sim::test;

namespace {

// Arms the walls and lands the FIRST drop event exactly on tile (0,0),
// mirroring test_sim.cpp's "hurry walls spiral in" fixture: game_seconds=10
// (ticks_left starts at 200) + hurry_seconds=8 arms the walls at tick 121
// (remaining seconds <= 3, non-strict) and drops the first event at arm+5 =
// tick 126 (spiral().events[0] is always (0,0), the top-left corner).
MatchConfig stomp_config() {
    MatchConfig cfg = open_config();
    cfg.tuning.game_seconds = 10;
    cfg.tuning.hurry_seconds = 8;
    cfg.tuning.enclosement_depth = 1;
    return cfg;
}

Bomb grounded_bomb(int owner, int tx, int ty, std::int32_t fuse) {
    Bomb b;
    b.active = true;
    b.owner = static_cast<std::uint8_t>(owner);
    b.x = tx * kTileWF + kTileWF / 2;
    b.y = ty * kTileHF + kTileHF / 2;
    b.fuse = fuse;
    b.flame = 1;
    return b;
}

// The crushed tile (0,0) is player 0's spawn, and the first drop event would
// otherwise kill him there — leaving one side alive, which freezes every fuse
// (the round-decided freeze, helpers.hpp). Park both players in the middle,
// well inside the depth-1 ring and clear of a flame-1 blast at (0,0).
void park_players_off_the_ring(State& st) {
    st.players[0].x = 6 * kTileWF + kTileWF / 2;
    st.players[0].y = 5 * kTileHF + kTileHF / 2;
    st.players[1].x = 8 * kTileWF + kTileWF / 2;
    st.players[1].y = 5 * kTileHF + kTileHF / 2;
}

bool exploded_this_tick(const State& st) {
    for (const Event& e : st.events)
        if (e.type == Event::Type::Explosion) return true;
    return false;
}

}  // namespace

TEST_CASE("wall_detonates ON queues only the FIRST of two grounded bombs sharing a tile") {
    MatchConfig cfg = stomp_config();
    cfg.tuning.wall_detonates = 1;
    Simulation s(cfg);
    State& st = s.state();

    st.bombs.push_back(grounded_bomb(0, 0, 0, 30000));
    st.bombs.push_back(grounded_bomb(1, 0, 0, 30000));
    st.players[0].bombs_placed = 1;
    st.players[1].bombs_placed = 1;

    run(s, 126);  // arm (tick 121) + one interval (5 ticks): first event lands on (0,0)
    REQUIRE(st.cells[0][0] == Cell::Solid);
    // sub_423209 only QUEUES a detonation (one-tick defer, docs/re/facts.md
    // "Chain-reaction timing") -- nothing is removed synchronously by the
    // drop itself, ON or OFF. (Whether the queued bomb actually detonates on
    // the FOLLOWING tick is bombs.cpp's fuse-drain, a separate system this
    // test deliberately does not exercise -- scoping the assertion to
    // exactly what drop_wall's bomb-crush loop itself does keeps this test
    // independent of that other system's own behaviour/tests.)
    REQUIRE(st.bombs.size() == 2);
    // Only the FIRST match (iteration order == insertion order here) gets
    // its fuse force-fired to 1; the second is left completely untouched by
    // this drop event -- its fuse only reflects 126 ticks of the bomb's OWN
    // ordinary countdown from 30000, nowhere near the forced value the first
    // bomb got.
    CHECK(st.bombs[0].fuse == 1);
    CHECK(st.bombs[1].fuse > 1000);
}

TEST_CASE("wall_detonates OFF still eats every bomb on the crushed tile (unchanged by this fix)") {
    // The OFF branch's loop-to-exhaustion (sub_424841's fall-through
    // re-search, with no jump out to LABEL_47) is untouched by this fix -- pinned here
    // alongside the ON case so a future edit can't silently break the
    // documented ON/OFF asymmetry.
    MatchConfig cfg = stomp_config();
    cfg.tuning.wall_detonates = 0;
    Simulation s(cfg);
    State& st = s.state();

    st.bombs.push_back(grounded_bomb(0, 0, 0, 30000));
    st.bombs.push_back(grounded_bomb(1, 0, 0, 30000));
    st.players[0].bombs_placed = 1;
    st.players[1].bombs_placed = 1;

    run(s, 126);
    REQUIRE(st.cells[0][0] == Cell::Solid);
    CHECK(st.bombs.empty());  // both silently eaten, no explosion
    CHECK(st.players[0].bombs_placed == 0);
    CHECK(st.players[1].bombs_placed == 0);
}

// --- A closing wall landing on a FIZZLING DUD ------------------------------
//
// The dud marker lives in the bomb record's state dword (sub_422EDE writes 2
// there), not in the motion word, so the grounded-bomb finder sub_422E48 —
// which rejects only a zero state or motion 2/3 — finds a fizzling dud exactly
// like a live grounded bomb, and sub_426818's crush loop has no dud branch.
// On the ON branch the queue drain sub_42331C force-writes elapsed-fuse =
// fuse-duration onto the record; only the elapsed-fuse INCREMENT in that
// function is gated on "not a dud", the "elapsed >= duration" test right after
// it is not, so the dud detonates on the next frame's pass like any other
// crushed bomb. On the OFF branch sub_424841 zeroes the record, again with no
// dud special-casing. facts.md "Enclosure wall crushes a fizzling dud".

TEST_CASE("wall_detonates ON detonates a crushed FIZZLING dud on the next tick") {
    MatchConfig cfg = stomp_config();
    cfg.tuning.wall_detonates = 1;
    Simulation s(cfg);
    State& st = s.state();
    park_players_off_the_ring(st);

    // Plant the dud on the very tick the wall lands, so it is still MID-fizzle
    // when crushed (tick_fuses() runs before enclosure.update(), so a dud
    // planted earlier would have burned its window down first).
    run(s, 125);
    REQUIRE(st.cells[0][0] != Cell::Solid);  // the drop has not happened yet
    // dud_left well above the one tick the forced detonation needs, and a fuse
    // far too long to go off on its own — so anything that explodes here can
    // only be the forced path.
    Bomb dud = grounded_bomb(0, 0, 0, 30000);
    dud.dud_left = 60;
    st.bombs.push_back(dud);
    st.players[0].bombs_placed = 1;

    s.tick(TickInputs{});  // tick 126: arm (tick 121) + one interval -> (0,0)
    REQUIRE(st.cells[0][0] == Cell::Solid);
    REQUIRE(st.bombs.size() == 1);
    // The crush both forces the fuse AND clears the fizzle; without the clear
    // the fuse would sit at 1 while tick_fuses() kept draining dud_left, and
    // the blast would arrive up to dud_frames (120 ticks = 6 s) later, out of
    // an already-solid wall.
    CHECK(st.bombs[0].fuse == 1);
    CHECK(st.bombs[0].dud_left == 0);

    s.tick(TickInputs{});
    CHECK(exploded_this_tick(st));
    CHECK(st.bombs.empty());
    CHECK(st.players[0].bombs_placed == 0);
}

TEST_CASE("wall_detonates OFF still silently eats a FIZZLING dud") {
    // sub_424841 has no dud branch either: the OFF branch absorbs a fizzling
    // dud exactly as it absorbs a live bomb, with no blast at all.
    MatchConfig cfg = stomp_config();
    cfg.tuning.wall_detonates = 0;
    Simulation s(cfg);
    State& st = s.state();
    park_players_off_the_ring(st);

    run(s, 125);
    Bomb dud = grounded_bomb(0, 0, 0, 30000);
    dud.dud_left = 60;
    st.bombs.push_back(dud);
    st.players[0].bombs_placed = 1;

    s.tick(TickInputs{});
    REQUIRE(st.cells[0][0] == Cell::Solid);
    CHECK(st.bombs.empty());
    CHECK(st.players[0].bombs_placed == 0);
    s.tick(TickInputs{});
    CHECK_FALSE(exploded_this_tick(st));  // absorbed, never detonated
}
