// Same-tick coincidence pins for the end-to-end tick-order audit
// (docs/re/facts.md "Per-tick call order — END-TO-END", 2026-07-11).
//
// The original checks flame death and powerup pickup after EVERY committed
// pixel step inside the mover (sub_41EC84, pseudo.c 22699-22717) — before the
// same turn's bomb actions and before the next bomb phase — and again at the
// head of the player's next turn (sub_41F29B 22915-22926). These tests pin
// the in-move half, which our port previously lacked, plus the sub_41DE63
// bounce/warp kill immunity shared by every kill path.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "helpers.hpp"

using namespace bomber::sim;
using namespace bomber::sim::test;

namespace {

// Walk player 0 east until it enters `tile_x`, returning the 1-based tick
// number of the arrival (0 if it never arrives). Runs on a THROWAWAY copy of
// the sim so callers can schedule same-tick coincidences in the real run.
int probe_arrival_tick(const MatchConfig& cfg, int tile_x, int max_ticks = 100) {
    Simulation probe(cfg);
    TickInputs right;
    right.players[0].right = true;
    for (int t = 1; t <= max_ticks; ++t) {
        probe.tick(right);
        if (probe.state().players[0].tile_x() == tile_x) return t;
    }
    return 0;
}

// A resting bomb whose fuse expires on tick `fuse` (our tick_fuses decrements
// once per tick starting the tick after insertion here — insertion happens
// between ticks, so a fuse of N explodes during the N-th tick() call).
Bomb make_bomb(int tx, int ty, int flame, int fuse, std::uint8_t owner = 1) {
    Bomb b;
    b.active = true;
    b.owner = owner;
    b.x = tx * kTileWF + kTileWF / 2;
    b.y = ty * kTileHF + kTileHF / 2;
    b.flame = flame;
    b.fuse = fuse;
    b.fuse_init = fuse;
    return b;
}

}  // namespace

TEST_CASE("mid-walk pickup beats a same-tick flame arm (token no longer shields the tile)") {
    // Player walks onto a token the SAME tick a flame arm reaches that tile.
    // Original (sub_41EC84 pickup, then the bomb pass next frame): the token
    // is already in the player's pocket when the arm arrives, so the arm
    // finds an empty tile, passes THROUGH, ignites it, and kills the player.
    // The old port did the opposite: the arm burned the token (arm-stop rule)
    // and the player survived, empty-handed.
    MatchConfig cfg = open_config();
    Simulation s(cfg);
    s.state().floor[0][2] = PowerupType::Flame;

    const int arrive = probe_arrival_tick(cfg, 2);
    REQUIRE(arrive > 0);
    // Bomb 2 tiles further east; reach 2 covers (3,0) and (2,0). Explodes on
    // the arrival tick (fuse decremented once per tick from the first tick).
    s.state().bombs.push_back(make_bomb(4, 0, 2, arrive));
    s.state().players[1].max_bombs = 2;  // owner slot bookkeeping stays sane
    ++s.state().players[1].bombs_placed;
    const std::int32_t flame_before = s.state().players[0].flame;

    TickInputs right;
    right.players[0].right = true;
    bool picked = false;
    for (int t = 1; t <= arrive; ++t) {
        s.tick(right);
        for (const auto& e : s.state().events)
            if (e.type == Event::Type::PowerupPicked && e.player == 0) picked = true;
    }
    CHECK(picked);                       // the pickup happened (per-pixel, step 1)
    CHECK(s.state().flame[0][2] > 0);    // the arm ignited the emptied tile...
    CHECK(!s.state().players[0].alive);  // ...and the head check killed the player
    // The token WAS applied before the death: the death scatter returns the
    // surplus (flame back at its start-with baseline, one Flame token
    // re-scattered onto the board) — the old order would have left the token
    // BURNED (PowerupBurned, zero Flame tokens anywhere, player alive).
    CHECK(s.state().players[0].flame == flame_before);
    int flame_tokens = 0;
    for (int y = 0; y < kGridHeight; ++y)
        for (int x = 0; x < kGridWidth; ++x)
            if (s.state().floor[y][x] == PowerupType::Flame) ++flame_tokens;
    CHECK(flame_tokens == 1);
}

TEST_CASE("a picked-up powerup is usable the same tick (pickup precedes the bomb actions)") {
    // ExtraBomb picked up mid-walk raises the capacity BEFORE the same
    // turn's drop block (sub_41F29B LABEL_246 runs after the mover).
    MatchConfig cfg = open_config();
    const int arrive = probe_arrival_tick(cfg, 2);
    REQUIRE(arrive > 0);

    Simulation s(cfg);
    s.state().floor[0][2] = PowerupType::ExtraBomb;
    s.state().players[0].max_bombs = 1;

    // Spend the whole capacity first: drop at spawn, then walk east.
    s.tick(press1(0));
    REQUIRE(s.state().bombs.size() == 1);

    TickInputs right;
    right.players[0].right = true;
    // The spawn-drop tick did not move the player, so arrival is `arrive`
    // walking ticks after it.
    for (int t = 1; t < arrive; ++t) s.tick(right);
    TickInputs last = right;
    last.players[0].action1 = true;  // fresh press on the arrival tick
    s.tick(last);

    CHECK(s.state().players[0].max_bombs == 2);  // token applied this tick...
    CHECK(s.state().bombs.size() == 2);          // ...and already spendable this tick
}

TEST_CASE("walking into a flame on its last tick of life kills (per-pixel check sees pre-aging value)") {
    // The in-move check reads the flame BEFORE this tick's aging pass; the
    // head check reads it after. A flame at value 1 kills a player stepping
    // in this tick, and is gone for a player stepping in next tick.
    MatchConfig cfg = open_config();
    Simulation s(cfg);
    Player& p = s.state().players[0];
    // One pixel west of tile 2's boundary (tile 2 starts at pixel 80), on the
    // row-0 centreline.
    p.x = 78 * kScale;
    p.y = 18 * kScale;
    s.state().flame[0][2] = 1;
    s.state().flame_owner[0][2] = 1;

    TickInputs right;
    right.players[0].right = true;
    s.tick(right);
    CHECK(!s.state().players[0].alive);

    // Control: same setup, but the player only reaches the tile NEXT tick —
    // by then the flame has aged to 0 and the step is safe.
    Simulation c(cfg);
    Player& q = c.state().players[0];
    q.x = 78 * kScale;
    q.y = 18 * kScale;
    c.state().flame[0][2] = 1;
    c.state().flame_owner[0][2] = 1;
    c.tick(TickInputs{});  // stand still: flame ages to 0
    c.tick(right);         // now walk in
    CHECK(c.state().players[0].alive);
}

TEST_CASE("a player killed mid-walk performs no bomb action that tick") {
    // The original's mid-move kill returns straight into the death branch —
    // LABEL_246 never runs. The old port completed the turn (bomb placed)
    // and only killed the player in the later field pass.
    MatchConfig cfg = open_config();
    Simulation s(cfg);
    Player& p = s.state().players[0];
    p.x = 78 * kScale;
    p.y = 18 * kScale;
    s.state().flame[0][2] = 5;
    s.state().flame_owner[0][2] = 1;

    TickInputs in;
    in.players[0].right = true;
    in.players[0].action1 = true;  // press the bomb key the same tick
    s.tick(in);
    CHECK(!s.state().players[0].alive);
    CHECK(s.state().bombs.empty());  // the drop never happened
}

TEST_CASE("a bouncing or warping player is immune to flame (sub_41DE63 states 5/6/7)") {
    // The head flame check goes through the shared kill funnel, which
    // early-outs for a mid-trampoline-hop or mid-warp victim — the same
    // guard drop_wall already applies.
    MatchConfig cfg = open_config();

    Simulation b(cfg);
    b.state().players[0].bounce = 10;  // mid-hop (past the apex: no RNG draw)
    b.state().flame[0][0] = 5;
    b.state().flame_owner[0][0] = 1;
    b.tick(TickInputs{});
    CHECK(b.state().players[0].alive);

    Simulation w(cfg);
    Player& pw = w.state().players[0];
    pw.warp = 5;  // mid warp-in (past the midpoint relocation)
    pw.warp_to_x = 0;
    pw.warp_to_y = 0;
    w.state().flame[0][0] = 5;
    w.state().flame_owner[0][0] = 1;
    w.tick(TickInputs{});
    CHECK(w.state().players[0].alive);

    // Control: no bounce/warp, same flame — dies.
    Simulation n(cfg);
    n.state().flame[0][0] = 5;
    n.state().flame_owner[0][0] = 1;
    n.tick(TickInputs{});
    CHECK(!n.state().players[0].alive);
}

TEST_CASE("a rover cannot stomp a bouncing or warping player either") {
    // Same sub_41DE63 guard on the campaign landing kill
    // (docs/re/campaign.md clause 4).
    MatchConfig cfg = open_config();
    Simulation s(cfg);
    s.state().campaign_hazards_active = true;
    Player& victim = s.state().players[0];
    victim.bounce = 10;

    Rover r;
    r.alive = true;
    r.kind = RoverKind::Rover;
    // One pixel west of the victim's tile (0,0) is out of grid; approach from
    // tile (1,0) instead: place the rover just inside tile 1, one pixel east
    // of the boundary, walking WEST (godir 3) into tile 0. Mid-tile on both
    // axes' centres is not required for the kill — any pixel landing in the
    // victim's tile triggers it — but keep off the tile-centre pixel so the
    // turn logic (and its RNG draws) never fires.
    r.x = 43 * kScale;  // 3px into tile 1; steps 40,41,42... westward
    r.y = 18 * kScale;
    r.dir = 3;  // west
    r.speed = 0;  // budget 100/tick -> exactly 1 pixel per tick
    s.state().rovers.push_back(r);

    for (int t = 0; t < 6; ++t) s.tick(TickInputs{});  // rover crosses into tile 0
    CHECK(s.state().players[0].alive);  // immune mid-hop

    // Control: same approach, no bounce — stomped.
    Simulation c(cfg);
    c.state().campaign_hazards_active = true;
    Rover r2 = r;
    c.state().rovers.push_back(r2);
    for (int t = 0; t < 6; ++t) c.tick(TickInputs{});
    CHECK(!c.state().players[0].alive);
}
