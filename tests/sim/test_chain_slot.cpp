// Bomb-capacity accounting across chain-ownership transfer.
//
// The original has NO per-player bomb counter: placement capacity is a live
// SCAN — sub_4245DA counts the active bomb slots whose owner word (+62)
// equals the player, and the drop/spooge gates compare max_bombs (+86)
// against that count every time (sub_41F29B ~23336/23345). The chain
// transfer (sub_42331C flame walk, pseudo.c 25644 copies the exploding bomb's
// +62 owner word straight into the chained bomb's own +62) overwrites the
// SAME +62 word the scan matches on, so chaining someone
// else's bomb moves the placement slot too: the victim's capacity frees
// IMMEDIATELY at transfer time, and the chained bomb counts against the
// CHAINER until it explodes (next tick, via the sub_423209 queue).
//
// Our port keeps `bombs_placed` as a running counter instead of a scan, so
// the transfer must move the slot along with the owner word (flames.cpp
// spread_to). Before that fix the victim's counter leaked one slot per
// chained bomb — permanently, while alive — which is the reported
// "had 4 extra bombs, suddenly dropped to a single bomb" collapse.
// docs/re/facts.md "Bomb capacity is a derived live-bomb count".

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "helpers.hpp"

using namespace bomber::sim;
using namespace bomber::sim::test;

namespace {

Fixed cx(int tx) { return tx * kTileWF + kTileWF / 2; }
Fixed cy(int ty) { return ty * kTileHF + kTileHF / 2; }

// A resting bomb owned by `owner` at (tx,ty) with the given fuse/reach.
Bomb resting(std::uint8_t owner, int tx, int ty, std::int32_t fuse, std::int32_t reach) {
    Bomb b;
    b.active = true;
    b.owner = owner;
    b.x = cx(tx);
    b.y = cy(ty);
    b.fuse = fuse;
    b.fuse_init = fuse;
    b.flame = reach;
    return b;
}

// All-blank board, players parked in opposite corners, no floor spawns.
MatchConfig blank_config() {
    MatchConfig cfg = open_config();
    for (int y = 0; y < kGridHeight; ++y)
        for (int x = 0; x < kGridWidth; ++x) cfg.cells[y][x] = Cell::Blank;
    return cfg;
}

}  // namespace

TEST_CASE("chaining an opponent's bombs returns their placement slots (the 5->1 collapse)") {
    Simulation s(blank_config());
    State& st = s.state();
    Player& chainer = st.players[0];  // at (0,0)
    Player& victim = st.players[1];   // at (14,10)
    victim.max_bombs = 5;

    // The victim's four bombs in a row on y=5, long fuses (they only ever
    // explode by being chained), reach 1 so each link chains the next.
    for (int i = 0; i < 4; ++i) st.bombs.push_back(resting(1, 4 + i, 5, 10000, 1));
    victim.bombs_placed = 4;
    // The chainer's igniter next to the first one, about to blow.
    st.bombs.push_back(resting(0, 3, 5, 2, 1));
    chainer.bombs_placed = 1;

    // Tick 1: fuse 2 -> 1. Tick 2: igniter explodes; its east arm reaches the
    // victim's first bomb -> ownership AND the placement slot transfer NOW
    // (sub_4245DA counts by the owner word the transfer just rewrote), while
    // the chained bomb itself only detonates NEXT tick (sub_423209 queue).
    run(s, 2);
    CHECK(st.players[0].bombs_placed == 1);  // igniter slot freed, chained slot gained
    CHECK(st.players[1].bombs_placed == 3);  // freed at transfer time, pre-explosion

    // Each subsequent tick detonates one link, whose arm chains the next.
    run(s, 4);
    CHECK(st.bombs.empty());
    CHECK(st.players[0].bombs_placed == 0);
    CHECK(st.players[1].bombs_placed == 0);  // no leak: full capacity restored
    CHECK(st.players[1].alive);

    // The victim can actually use the restored capacity: walk-free drop check.
    TickInputs in;
    in.players[1].action1 = true;
    s.tick(in);
    CHECK(st.players[1].bombs_placed == 1);  // a fresh bomb went down
}

TEST_CASE("a chained bomb counts against the chainer until it explodes") {
    Simulation s(blank_config());
    State& st = s.state();
    Player& chainer = st.players[0];
    chainer.max_bombs = 1;

    st.bombs.push_back(resting(1, 4, 5, 10000, 1));  // opponent's bomb
    st.players[1].bombs_placed = 1;
    st.bombs.push_back(resting(0, 3, 5, 2, 1));  // chainer's igniter
    chainer.bombs_placed = 1;

    run(s, 2);  // igniter explodes, transfer happens, chained bomb still live
    // The transferred bomb occupies the chainer's only slot (original: the
    // owner-word scan counts it against them), so a drop is refused this tick.
    CHECK(st.players[0].bombs_placed == 1);
    TickInputs in;
    in.players[0].action1 = true;
    s.tick(in);  // chained bomb detonates at the TOP of this tick's bomb pass...
    // ...but the player pass runs before the chain drain (sub_41F29B precedes
    // the sub_42331C drain in the frame), so the drop attempt still sees the
    // occupied slot and is refused.
    CHECK(st.players[0].bombs_placed == 0);
    // Next press succeeds: the slot came back when the chained bomb exploded.
    s.tick(TickInputs{});  // release the key edge
    s.tick(in);
    CHECK(st.players[0].bombs_placed == 1);
}

// The user-reported flow end to end: diarrhea auto-drops the whole capacity
// onto the field; an opponent's blast chains the pooped cluster (each
// transferred link chains the next, already opponent-owned); the disease then
// expires. Capacity must be fully restored — before the slot-move fix the
// victim's bombs_placed leaked one slot per cross-owner chained bomb,
// permanently, which read as "my bombs were reset when the disease ended".
TEST_CASE("diarrhea cluster chained by an opponent restores full capacity after recovery") {
    MatchConfig cfg = blank_config();
    cfg.tuning.disease_frames[static_cast<int>(Disease::Diarrhea)] = 40;  // short bout
    Simulation s(cfg);
    State& st = s.state();
    Player& v = st.players[1];  // victim walks a short line while pooping
    v.max_bombs = 5;
    v.x = cx(4);
    v.y = cy(5);
    infect(v, Disease::Diarrhea, 40);

    // March east for a few ticks: the auto-drop poops a bomb on each new tile
    // until all 5 slots are on the field.
    TickInputs east;
    east.players[1].right = true;
    int poop_ticks = 0;
    while (v.bombs_placed < 5 && poop_ticks < 200) {
        s.tick(east);
        ++poop_ticks;
    }
    CHECK(v.bombs_placed == 5);
    // Make the pooped bombs immune to their own fuses within the test window
    // so only the opponent's chain can set them off (isolates the transfer).
    for (auto& b : st.bombs)
        if (b.owner == 1) b.fuse = 10000;

    // The opponent's igniter next to the first pooped bomb.
    const Bomb& first = st.bombs.front();
    Bomb ign;
    ign.active = true;
    ign.owner = 0;
    ign.x = first.x - kTileWF;  // one tile west of the first poop
    ign.y = first.y;
    ign.fuse = 2;
    ign.fuse_init = 2;
    ign.flame = 1;
    st.bombs.push_back(ign);
    st.players[0].bombs_placed = 1;

    // Park the victim far from the blast row and let the bout expire on the
    // next tick — BEFORE the chain frees any slot, so recovery happens first
    // and no fresh poop lands after the park (isolates "capacity after
    // recovery" from further auto-drops).
    v.x = cx(14);
    v.y = cy(10);
    v.disease_timer = 1;
    run(s, 60);  // disease expires, then the chain resolves one link per tick
    CHECK(v.alive);
    CHECK(!v.sick(Disease::Diarrhea));  // recovered
    CHECK(st.bombs.empty());            // the field cleared
    CHECK(v.max_bombs == 5);            // never actually reset...
    CHECK(v.bombs_placed == 0);         // ...and no leaked slots: 5 placeable again
    CHECK(st.players[0].bombs_placed == 0);
}

TEST_CASE("self-chains never move slots (control)") {
    Simulation s(blank_config());
    State& st = s.state();
    Player& p = st.players[0];
    p.max_bombs = 3;

    st.bombs.push_back(resting(0, 4, 5, 10000, 1));
    st.bombs.push_back(resting(0, 5, 5, 10000, 1));
    st.bombs.push_back(resting(0, 3, 5, 2, 1));
    p.bombs_placed = 3;

    run(s, 6);  // ignite + two chain links
    CHECK(st.bombs.empty());
    CHECK(p.bombs_placed == 0);  // exactly restored, no over/under-count
}
