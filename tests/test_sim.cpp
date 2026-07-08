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
    // CONFIRMED enclosure timing (sub_426818, docs/re/enclosure.md): the "HURRY!"
    // banner/sound fires at remaining <= hurry_seconds, but the WALLS start
    // closing 5 s later (remaining <= hurry_seconds - 5) and drop ONE tile every
    // 250 ms = 5 ticks at 20 Hz.
    MatchConfig cfg = test_config();
    cfg.tuning.game_seconds = 10;  // 200 ticks
    cfg.tuning.hurry_seconds = 8;  // banner at remaining<=8s (tick 40); walls at 3s (tick 140)
    cfg.tuning.enclosement_depth = 1;
    Simulation s(cfg);
    run(s, 40);
    CHECK(s.state().hurry);                // banner fired at moment 1 (tick 40)
    CHECK(s.state().enclose_index == 0);   // ...but the walls have NOT begun
    CHECK(s.state().cells[0][0] != Cell::Solid);
    run(s, 99);                            // up to tick 139: still just the banner
    CHECK(s.state().enclose_index == 0);
    run(s, 1);                             // tick 140: walls arm (remaining 3s)
    CHECK(s.state().enclose_index == 0);   // arm frame drops nothing (gate shut)
    run(s, 4);
    CHECK(s.state().enclose_index == 0);   // still nothing at arm+4
    run(s, 1);                             // arm+5: first wall drops
    CHECK(s.state().enclose_index == 1);
    CHECK(s.state().cells[0][0] == Cell::Solid);  // spiral starts top-left
    CHECK(!s.state().players[0].alive);           // player 0 spawned there: crushed
    // Cadence: exactly one more wall every 5 ticks.
    run(s, 4);
    CHECK(s.state().enclose_index == 1);
    run(s, 1);
    CHECK(s.state().enclose_index == 2);          // (1,0) closed at arm+10
    CHECK(s.state().cells[0][1] == Cell::Solid);
    // A parked bomb in the wall's path must detonate, not linger. Tile (4,0) is
    // the 5th outer-ring cell (index 4), so it closes at arm + 5*(4+1) = arm+25.
    Bomb b;
    b.active = true;
    b.owner = 1;
    b.x = 4 * kTileWF + kTileWF / 2;
    b.y = kTileHF / 2;  // tile (4,0), on the outer ring
    b.fuse = 30000;
    b.flame = 1;
    s.state().bombs.push_back(b);
    s.state().players[1].bombs_placed = 1;
    run(s, 20);                                   // reach and pass (4,0)'s drop
    CHECK(s.state().bombs.empty());
    CHECK(s.state().cells[0][4] == Cell::Solid);
    // Depth 1 closes two rings and leaves the interior open. 88 cells * 5 ticks.
    run(s, 88 * 5);
    CHECK(s.state().cells[1][1] == Cell::Solid);
    CHECK(s.state().cells[2][2] == Cell::Blank);
    CHECK(s.state().enclose_index == enclose_total(1));
}

TEST_CASE("enclosure interval is a fixed 5 ticks (250 ms at 20 Hz)") {
    // Pins the CONFIRMED cadence directly: independent of depth/ring count, the
    // per-wall interval is 250 ms / (1000/20) = 5 ticks (sub_426818 `+= 250`,
    // NOT a spread-to-fit heuristic). docs/re/enclosure.md §3.
    MatchConfig cfg = test_config();
    cfg.tuning.game_seconds = 20;   // 400 ticks
    cfg.tuning.hurry_seconds = 20;  // banner at 20s (tick 0); walls at 15s -> tick 100
    cfg.tuning.enclosement_depth = 3;  // all rings — proves interval != f(n)
    Simulation s(cfg);
    run(s, 100);                    // reach the wall-start (remaining 15s)
    REQUIRE(s.state().enclose_interval == 5);   // armed with the fixed interval
    // Walk the index forward and confirm it steps exactly once per 5 ticks.
    int last = s.state().enclose_index;
    for (int k = 0; k < 6; ++k) {
        run(s, 4);
        CHECK(s.state().enclose_index == last);   // no drop in the first 4
        run(s, 1);
        CHECK(s.state().enclose_index == last + 1);  // one drop on the 5th
        ++last;
    }
}

TEST_CASE("the HURRY banner precedes the walls by 5 seconds (two distinct moments)") {
    // The banner/sound (Hurry event) fires at remaining <= hurry_seconds; the
    // walls do not start dropping until remaining <= hurry_seconds - 5.
    // Confirmed: HUD block ~29533 (dword_464984) vs sub_426818 (dword_45BE9C).
    MatchConfig cfg = test_config();
    cfg.tuning.game_seconds = 10;   // 200 ticks
    cfg.tuning.hurry_seconds = 8;   // banner at remaining<=8s (tick 40); walls at 3s (tick 140)
    cfg.tuning.enclosement_depth = 1;
    Simulation s(cfg);
    run(s, 40);                     // remaining 8s: the banner window opens
    CHECK(s.state().hurry);         // banner/sound event fired
    bool hurry_evt = false;
    for (const auto& e : s.state().events)
        if (e.type == Event::Type::Hurry) hurry_evt = true;
    CHECK(hurry_evt);               // the Hurry event fired on this exact tick
    run(s, 60);                     // remaining 5s: still inside the banner window
    CHECK(s.state().enclose_index == 0);          // ...but no wall has dropped
    CHECK(s.state().cells[0][0] != Cell::Solid);
    run(s, 45);                     // past tick 145: walls have started
    CHECK(s.state().enclose_index >= 1);
    CHECK(s.state().cells[0][0] == Cell::Solid);
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
    b.fuse = 10000;  // long enough to survive the punch flight + landing checks below
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

TEST_CASE("a head hit can drop goldflame (kind 8)") {
    // sub_421F7E rolls rand()%15 over ALL kinds and accepts any whose per-kind
    // count exceeds getvalue(50+kind). Goldflame (kind 8, start-with id 58 = 0)
    // is a valid droppable kind when the flag is set. Set the victim's ONLY
    // surplus to goldflame (every other stat at its baseline) and force enough
    // drops that the uniform roll lands on kind 8 (200 tries/drop, so missing is
    // astronomically unlikely when it is the sole accepted kind).
    MatchConfig cfg = test_config();
    cfg.tuning.powers_lost_min = 4;  // several drops → the roll certainly hits kind 8
    Simulation s(cfg);
    Player& v = s.state().players[1];
    v.max_bombs = s.state().tuning.start_with[0];  // baseline: no surplus
    v.flame = s.state().tuning.start_with[1];      // baseline: no surplus
    v.kick = false;
    v.goldflame = true;  // the sole surplus kind
    int vx = v.tile_x(), vy = v.tile_y();
    s.state().players[0].punch = true;
    s.state().players[0].x = (vx - 4) * kTileWF + kTileWF / 2;
    s.state().players[0].y = vy * kTileHF + kTileHF / 2;
    s.state().players[0].facing = Direction::Right;
    Bomb b;
    b.active = true;
    b.owner = 0;
    b.flame = 1;
    b.fuse = 10000;
    b.x = (vx - 3) * kTileWF + kTileWF / 2;
    b.y = vy * kTileHF + kTileHF / 2;
    s.state().bombs.push_back(b);
    s.tick(press2(0));  // punch → flight
    run(s, 12);         // land on the victim's head
    CHECK(v.stun > 0);
    CHECK(!v.goldflame);  // the goldflame flag was removed as a dropped power
    bool gold_on_floor = false;
    for (int y = 0; y < kGridHeight; ++y)
        for (int x = 0; x < kGridWidth; ++x)
            if (s.state().floor[y][x] == PowerupType::Goldflame) gold_on_floor = true;
    CHECK(gold_on_floor);  // scattered as a Goldflame token
}
