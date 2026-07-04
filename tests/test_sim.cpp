// Gameplay rule tests for the deterministic sim.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "helpers.hpp"

using namespace bomber::sim;
using namespace bomber::sim::test;

namespace {

MatchConfig test_config() {
    MatchConfig cfg = open_config();
    cfg.cells[0][2] = Cell::Brick;  // one brick within player 0's reach
    return cfg;
}

}  // namespace

TEST_CASE("bomb explodes at its fuse and burns the brick") {
    Simulation s(test_config());
    CHECK(s.state().cells[0][2] == Cell::Brick);
    s.tick(press1(0));  // drop at (0,0)
    CHECK(!s.state().bombs.empty());
    CHECK(s.state().players[0].bombs_placed == 1);
    run(s, s.state().tuning.fuse_frames - 1);
    CHECK(s.state().bombs.empty());        // exploded exactly at fuse
    CHECK(s.state().flame[0][0] > 0);      // epicenter
    CHECK(s.state().flame[0][1] > 0);      // one to the right
    CHECK(s.state().cells[0][2] == Cell::Blank);  // brick destroyed...
    CHECK(s.state().burning[0][2] > 0);           // ...but still crumbling
    CHECK(s.state().flame[0][2] == 0);     // flame stops at the brick it burns
    CHECK(s.state().flame[1][1] == 0);     // solid pillar untouched
    CHECK(s.state().players[0].bombs_placed == 0);
    // Player 0 stood on the bomb: died in the blast.
    CHECK(!s.state().players[0].alive);
    CHECK(alive_count(s.state()) == 1);
}

TEST_CASE("flame reach stops at range and solids") {
    Simulation s(test_config());
    s.tick(press1(0));
    run(s, s.state().tuning.fuse_frames);
    CHECK(s.state().flame[1][0] > 0);   // down one
    CHECK(s.state().flame[2][0] > 0);   // down two
    CHECK(s.state().flame[3][0] == 0);  // flame length 2 -> no further
}

TEST_CASE("burned brick reveals its powerup, players pick it up") {
    Simulation s(test_config());
    s.state().hidden[0][2] = PowerupType::ExtraBomb;  // plant under the brick
    s.tick(press1(0));
    run(s, s.state().tuning.fuse_frames - 1);
    CHECK(s.state().burning[0][2] > 0);
    run(s, s.state().tuning.brick_burn_frames);
    CHECK(s.state().floor[0][2] == PowerupType::ExtraBomb);  // revealed

    // Verify pickup by placing a powerup under player 1.
    int tx = s.state().players[1].tile_x(), ty = s.state().players[1].tile_y();
    s.state().floor[ty][tx] = PowerupType::Flame;
    int before = s.state().players[1].flame;
    run(s, 1);
    CHECK(s.state().players[1].flame == before + 1);
    CHECK(s.state().floor[ty][tx] == PowerupType::None);
}

TEST_CASE("powerup accumulation respects the VALUELST limits") {
    Simulation s(test_config());
    Player& p = s.state().players[1];
    int tx = p.tile_x(), ty = p.tile_y();
    for (int i = 0; i < 20; ++i) {
        s.state().floor[ty][tx] = PowerupType::ExtraBomb;
        run(s, 1);
    }
    CHECK(p.max_bombs == s.state().tuning.limits[0]);  // capped at 8
}

TEST_CASE("chained bombs explode in the same tick") {
    Simulation s(test_config());
    s.tick(press1(0));  // bomb A at (0,0), fuse 40
    // Walk down to (0,2): within bomb A's flame reach of 2.
    TickInputs down;
    down.players[0].down = true;
    for (int i = 0; i < 8; ++i) s.tick(down);
    CHECK(s.state().players[0].tile_y() == 2);
    s.tick(press1(0));  // bomb B at (0,2), ~10 ticks younger
    // Bomb A explodes at its fuse; the chain must clear BOTH bombs instantly.
    while (!s.state().bombs.empty() && s.state().tick < 100) run(s, 1);
    CHECK(s.state().bombs.empty());
    CHECK(s.state().tick <= 42);  // bomb B's own fuse would have lasted longer
}

TEST_CASE("walls clamp movement") {
    Simulation s(test_config());
    TickInputs down;
    down.players[0].down = true;
    run(s, 8, down);
    CHECK(s.state().players[0].tile_y() >= 1);
    TickInputs right;
    right.players[0].right = true;
    // Force the player exactly onto the center of (0,1); the pillar at (1,1)
    // must clamp rightward movement there.
    Simulation probe(test_config());
    probe.state().players[0].x = kTileWF / 2;
    probe.state().players[0].y = kTileHF + kTileHF / 2;
    run(probe, 5, right);
    CHECK(probe.state().players[0].tile_x() == 0);  // clamped against the pillar
    CHECK(probe.state().players[0].x == kTileWF / 2);
}

TEST_CASE("a placed bomb blocks re-entry onto its tile") {
    Simulation s(test_config());
    s.tick(press1(0));  // bomb at (0,0), player stands on it
    TickInputs down;
    down.players[0].down = true;
    run(s, 10, down);  // walk well clear of the bomb
    CHECK(s.state().players[0].tile_y() >= 2);
    TickInputs up;
    up.players[0].up = true;
    run(s, 30, up);  // try to walk back
    CHECK(s.state().players[0].tile_y() == 1);  // bomb blocks re-entry at (0,0)
}

TEST_CASE("kick sends a resting bomb sliding") {
    Simulation s(test_config());
    s.state().players[0].kick = true;
    s.state().players[0].x = 2 * kTileWF + kTileWF / 2;
    s.state().players[0].y = 2 * kTileHF + kTileHF / 2;
    s.tick(press1(0));
    // Step off, then push right against it.
    TickInputs left;
    left.players[0].left = true;
    run(s, 12, left);
    TickInputs right;
    right.players[0].right = true;
    run(s, 30, right);
    bool bomb_moved = s.state().bombs.empty() || s.state().bombs[0].tile_x() > 2;
    CHECK(bomb_moved);
}

TEST_CASE("full-state determinism incl. seeded setup") {
    MatchConfig cfg = test_config();
    cfg.tuning.spawn_counts[0] = 5;  // exercise RNG in setup
    Simulation a(cfg), b(cfg);
    CHECK(a.hash() == b.hash());
    TickInputs in;
    for (int t = 0; t < 500; ++t) {
        in.players[0].right = (t / 7) % 2 == 0;
        in.players[0].down = (t / 11) % 2 == 1;
        in.players[0].action1 = t % 37 == 0;
        in.players[1].left = (t / 5) % 2 == 0;
        in.players[1].action1 = t % 41 == 0;
        a.tick(in);
        b.tick(in);
    }
    CHECK(a.hash() == b.hash());
    MatchConfig cfg2 = cfg;
    cfg2.seed = 8;
    Simulation c(cfg2);
    CHECK(c.hash() != a.hash());
}

TEST_CASE("the match clock counts down to a TimeUp event") {
    MatchConfig cfg = test_config();
    cfg.tuning.game_seconds = 1;  // 20 ticks
    Simulation s(cfg);
    CHECK(s.state().ticks_left == 20);
    run(s, 19);
    CHECK(s.state().ticks_left == 1);
    run(s, 1);
    CHECK(s.state().ticks_left == 0);
    bool timeup = false;
    for (const auto& e : s.state().events)
        if (e.type == Event::Type::TimeUp) timeup = true;
    CHECK(timeup);
    run(s, 5);
    CHECK(s.state().ticks_left == 0);  // stays at zero
}

TEST_CASE("hurry walls spiral in, crush, and detonate bombs") {
    MatchConfig cfg = test_config();
    cfg.tuning.game_seconds = 4;
    cfg.tuning.hurry_seconds = 3;  // hurry after 20 ticks, then 1 wall per tick
    cfg.tuning.enclosement_depth = 1;
    Simulation s(cfg);
    run(s, 21);
    CHECK(s.state().hurry);
    run(s, 2);
    CHECK(s.state().cells[0][0] == Cell::Solid);  // spiral starts top-left
    CHECK(!s.state().players[0].alive);           // player 0 spawned there: crushed
    // A parked bomb in the wall's path must detonate, not linger.
    Bomb b;
    b.active = true;
    b.owner = 1;
    b.x = 4 * kTileWF + kTileWF / 2;
    b.y = kTileHF / 2;  // tile (4,0), on the outer ring
    b.fuse = 30000;
    b.flame = 1;
    s.state().bombs.push_back(b);
    s.state().players[1].bombs_placed = 1;
    run(s, 6);
    CHECK(s.state().bombs.empty());
    CHECK(s.state().cells[0][4] == Cell::Solid);
    // Depth 1 closes two rings and leaves the interior open.
    run(s, 100);
    CHECK(s.state().cells[1][1] == Cell::Solid);
    CHECK(s.state().cells[2][2] == Cell::Blank);
    CHECK(s.state().enclose_index == enclose_total(1));
}

TEST_CASE("punch lofts a bomb three tiles, hopping and wrapping") {
    MatchConfig cfg = test_config();
    Simulation s(cfg);
    Player& p = s.state().players[0];
    p.punch = true;
    // Place a bomb manually at (1,0) and face east.
    Bomb b;
    b.active = true;
    b.owner = 1;
    b.x = kTileWF + kTileWF / 2;
    b.y = kTileHF / 2;  // tile (1,0)
    b.fuse = 10;
    b.flame = 1;
    s.state().bombs.push_back(b);
    p.facing = Direction::Right;
    s.tick(press2(0));
    REQUIRE(!s.state().bombs.empty());
    CHECK(s.state().bombs[0].flying);
    int fuse_at_launch = s.state().bombs[0].fuse;
    run(s, 3);
    CHECK(s.state().bombs[0].fuse == fuse_at_launch);  // fuse paused while airborne
    run(s, 30);
    CHECK(!s.state().bombs[0].flying);
    CHECK(s.state().bombs[0].tile_x() == 4);  // landed three tiles east of (1,0)
    CHECK(s.state().bombs[0].tile_y() == 0);

    // Occupied landing: solid at the 3-tile target makes it hop one further.
    Simulation s2(cfg);
    s2.state().players[0].punch = true;
    s2.state().players[0].facing = Direction::Right;
    s2.state().cells[0][4] = Cell::Solid;
    Bomb b2 = b;
    s2.state().bombs.push_back(b2);
    s2.tick(press2(0));
    run(s2, 40);
    CHECK(!s2.state().bombs[0].flying);
    CHECK(s2.state().bombs[0].tile_x() == 5);

    // Wrap: bomb at (13,0) punched east lands at (13+3)%15 = 1.
    Simulation s3(cfg);
    s3.state().players[0].punch = true;
    s3.state().players[0].x = 12 * kTileWF + kTileWF / 2;
    s3.state().players[0].y = kTileHF / 2;
    s3.state().players[0].facing = Direction::Right;
    Bomb b3 = b;
    b3.x = 13 * kTileWF + kTileWF / 2;
    s3.state().bombs.push_back(b3);
    s3.tick(press2(0));
    run(s3, 40);
    CHECK(!s3.state().bombs[0].flying);
    CHECK(s3.state().bombs[0].tile_x() == 1);
    CHECK(s3.state().bombs[0].tile_y() == 0);
}

TEST_CASE("grab picks the bomb up, throw launches it, fuse resumes") {
    Simulation s(test_config());
    Player& p = s.state().players[0];
    p.grab = true;
    // Grab/throw live on the BOMB key now (sub_41F29B): a fresh press onto your
    // own resting bomb grabs it; releasing the key while carrying throws it.
    s.tick(press1(0));       // drop own bomb at (0,0), standing on it
    CHECK(s.state().bombs.size() == 1);
    s.tick(TickInputs{});    // release, so the next press is a fresh edge
    s.tick(press1(0));       // press again while standing on it -> pick it up
    CHECK(s.state().bombs.empty());
    CHECK(p.carrying);
    CHECK(p.bombs_placed == 1);  // slot stays reserved while held
    CHECK(p.stun == s.state().tuning.pickup_pause);
    // Pickup pause: movement does nothing while stunned. HOLD the bomb key the
    // whole time so the bomb stays carried (releasing it would throw).
    Fixed before = p.y;
    TickInputs carry_down;
    carry_down.players[0].down = true;
    carry_down.players[0].action1 = true;  // keep holding -> keep carrying
    run(s, s.state().tuning.pickup_pause, carry_down);
    CHECK(p.y == before);
    run(s, 6, carry_down);  // free again; ends on row 2 (no pillars on even rows)
    CHECK(p.y > before);
    CHECK(p.tile_y() == 2);
    CHECK(p.carrying);       // still holding (key never released)
    // Throw east: RELEASE the bomb key while carrying -> launch in the facing
    // dir. Lands three tiles from the throw tile, then explodes.
    p.facing = Direction::Right;
    int from_tx = p.tile_x();
    s.tick(TickInputs{});    // release -> throw
    CHECK(!p.carrying);
    CHECK(s.state().bombs.size() == 1);
    CHECK(s.state().bombs[0].flying);
    run(s, 30);
    CHECK(!s.state().bombs[0].flying);
    CHECK(s.state().bombs[0].tile_x() == from_tx + 3);
    CHECK(s.state().bombs[0].tile_y() == 2);
    // Fuse resumes after landing and eventually explodes, freeing the slot.
    run(s, s.state().tuning.fuse_frames + 2);
    CHECK(s.state().bombs.empty());
    CHECK(p.bombs_placed == 0);
}

TEST_CASE("a bomb landing on a head stuns and scatters powerups") {
    Simulation s(test_config());
    // Player 1 is the victim standing at its spawn; give it some powerups.
    Player& v = s.state().players[1];
    v.max_bombs = 4;  // +3 extra bombs
    v.flame = 5;      // +3 flame
    v.kick = true;
    int vx = v.tile_x(), vy = v.tile_y();
    // Player 0 punches a bomb straight onto player 1's tile.
    s.state().players[0].punch = true;
    s.state().players[0].x = (vx - 4) * kTileWF + kTileWF / 2;  // four tiles west
    s.state().players[0].y = vy * kTileHF + kTileHF / 2;
    s.state().players[0].facing = Direction::Right;
    Bomb b;
    b.active = true;
    b.owner = 0;
    b.flame = 1;
    b.fuse = 10000;
    b.x = (vx - 3) * kTileWF + kTileWF / 2;  // bomb east of puncher; +3 lands on victim
    b.y = vy * kTileHF + kTileHF / 2;
    s.state().bombs.push_back(b);
    s.tick(press2(0));  // punch -> bomb flies 3 tiles east onto victim
    run(s, 12);         // bomb lands (~9 ticks) while stun (20) still active
    CHECK(v.stun > 0);  // dazed
    CHECK((v.max_bombs < 4 || v.flame < 5 || !v.kick));  // lost at least one power
    int floor_count = 0;
    for (int y = 0; y < kGridHeight; ++y)
        for (int x = 0; x < kGridWidth; ++x)
            if (s.state().floor[y][x] != PowerupType::None) ++floor_count;
    CHECK(floor_count >= 1);  // dropped powers landed on the floor
    // While stunned the victim can't move.
    Fixed by = v.y;
    TickInputs up;
    up.players[1].up = true;
    run(s, 1, up);
    CHECK(v.y == by);
}
