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
    // The brick stays BLOCKING (still Cell::Brick) for the whole crumble —
    // sub_425EFC's cell-type write nets to a no-op at ignition; the cell
    // only flips to Blank once `burning` finishes (docs/re/facts.md "Brick
    // crumble timing"). It is destroyed in the sense that it is now
    // crumbling (unrecoverable) and its hidden powerup, if any, has already
    // revealed — but it does not open up to movement/flame-arms yet.
    CHECK(s.state().cells[0][2] == Cell::Brick);
    CHECK(s.state().burning[0][2] > 0);           // crumbling
    CHECK(s.state().flame[0][2] == 0);     // flame stops at the brick it burns
    CHECK(s.state().flame[1][1] == 0);     // solid pillar untouched
    CHECK(s.state().players[0].bombs_placed == 0);
    // Player 0 stood on the bomb: died in the blast.
    CHECK(!s.state().players[0].alive);
    CHECK(alive_count(s.state()) == 1);

    // Once the crumble timer runs out, the tile finally opens up.
    run(s, s.state().tuning.brick_burn_frames);
    CHECK(s.state().cells[0][2] == Cell::Blank);
    CHECK(s.state().burning[0][2] == 0);
}

TEST_CASE("flame reach stops at range and solids") {
    Simulation s(test_config());
    s.tick(press1(0));
    run(s, s.state().tuning.fuse_frames);
    CHECK(s.state().flame[1][0] > 0);   // down one
    CHECK(s.state().flame[2][0] > 0);   // down two
    CHECK(s.state().flame[3][0] == 0);  // flame length 2 -> no further
}

// docs/re/facts.md "Flame-arm stops": the arm's per-tile occupancy checks
// (sub_42331C 25637-25678) run BEFORE the ignite call, and a hit STOPS the
// arm without igniting that tile at all — it does not burn through.
TEST_CASE("flame arm stops at a floor powerup, without igniting its tile") {
    Simulation s(test_config());
    s.state().floor[0][1] = PowerupType::ExtraBomb;  // one tile right of (0,0)
    s.tick(press1(0));
    run(s, s.state().tuning.fuse_frames - 1);
    CHECK(s.state().flame[0][0] > 0);          // epicentre still burns
    CHECK(s.state().flame[0][1] == 0);         // powerup tile NOT ignited
    CHECK(s.state().floor[0][1] == PowerupType::None);  // but destroyed
    bool burned_event = false;
    for (const auto& e : s.state().events)
        if (e.type == Event::Type::PowerupBurned && e.x == 1 && e.y == 0) burned_event = true;
    CHECK(burned_event);  // events are current-tick only: check right after the blast
}

TEST_CASE("flame arm stops at a bomb it chain-detonates, without igniting past it") {
    Simulation s(open_config());
    s.state().cells[0][1] = Cell::Blank;  // clear path for the arm to reach x=1
    Bomb victim;
    victim.active = true;
    victim.id = 1;  // distinct id: two hand-built bombs coexist (place() isn't used)
    victim.owner = 1;
    victim.x = kTileWF + kTileWF / 2;  // tile (1,0)
    victim.y = kTileHF / 2;
    victim.fuse = 30000;  // never fires on its own within this test
    victim.flame = 0;     // its own chain explosion covers ONLY its own tile
    s.state().bombs.push_back(victim);
    ++s.state().players[1].bombs_placed;

    Bomb igniter;
    igniter.active = true;
    igniter.id = 2;
    igniter.owner = 0;
    igniter.x = kTileWF / 2;  // tile (0,0)
    igniter.y = kTileHF / 2;
    igniter.fuse = 1;
    igniter.flame = 3;  // would reach tile (3,0) if the arm sailed through
    s.state().bombs.push_back(igniter);
    ++s.state().players[0].bombs_placed;

    // A chain reaction is NOT instantaneous (docs/re/facts.md "Chain-reaction
    // timing", sub_423209's deferred queue): the igniter's fuse expires and
    // its arm reaches the victim THIS tick, QUEUEING it; the victim's own
    // explosion (its epicentre + owner transfer) happens the NEXT tick, when
    // the queue drains.
    run(s, 1);  // igniter's fuse expires this tick
    CHECK(s.state().bombs.size() == 1);      // igniter gone, victim still queued
    CHECK(s.state().flame[0][1] == 0);       // victim hasn't gone off yet
    CHECK(s.state().flame[0][2] == 0);       // the igniter's arm did NOT continue past it
    int explosion_count = 0;
    for (const auto& e : s.state().events)
        if (e.type == Event::Type::Explosion) ++explosion_count;
    CHECK(explosion_count == 1);  // only the igniter, so far
    CHECK(s.state().bombs[0].owner == 0);    // ownership already transferred to the igniter

    run(s, 1);  // the queue drains: victim forcibly detonates
    CHECK(s.state().bombs.empty());          // both chain-detonated
    CHECK(s.state().flame[0][1] > 0);        // victim's tile burns (its OWN epicentre)
    CHECK(s.state().flame_owner[0][1] == 0);  // credited to the igniter's owner, not victim's
    explosion_count = 0;
    for (const auto& e : s.state().events)
        if (e.type == Event::Type::Explosion) ++explosion_count;
    CHECK(explosion_count == 1);  // the (formerly) victim, alone, this tick
}

// A chain-triggered bomb's own explosion skips re-casting an arm back toward
// the flame that hit it (bomb+56, pseudo.c 25621: `if (!field56 || k+1 !=
// field56)`, pushed as `((k+2)&3)+1` — the OPPOSITE of the triggering arm's
// direction). The other three directions still fire at full reach.
TEST_CASE("a chain-detonated bomb skips re-blasting back toward its trigger") {
    Simulation s(open_config());
    // Row 0: no pillars there (the (odd,odd) pattern never hits an even row).
    Bomb igniter;
    igniter.active = true;
    igniter.id = 1;
    igniter.owner = 0;
    igniter.x = 5 * kTileWF + kTileWF / 2;  // tile (5,0)
    igniter.y = kTileHF / 2;
    igniter.fuse = 1;
    igniter.flame = 3;  // reaches (6,0),(7,0),(8,0) rightward -> hits the chained bomb
    s.state().bombs.push_back(igniter);
    ++s.state().players[0].bombs_placed;

    Bomb chained;
    chained.active = true;
    chained.id = 2;
    chained.owner = 1;
    chained.x = 8 * kTileWF + kTileWF / 2;  // tile (8,0)
    chained.y = kTileHF / 2;
    chained.fuse = 30000;  // never fires on its own within this test
    chained.flame = 10;    // generous reach in every OTHER direction
    s.state().bombs.push_back(chained);
    ++s.state().players[1].bombs_placed;

    run(s, 1);  // igniter's fuse expires: its right-arm reaches (8,0), queues it
    run(s, 1);  // the queue drains: the chained bomb detonates, skipping Left

    CHECK(s.state().bombs.empty());
    // Left (back toward the igniter) is skipped entirely: tiles the igniter's
    // OWN (shorter, reach-3) left-arm could never have reached — (2,0)-4 away
    // is its limit; (0,0)/(1,0) are strictly farther — stay completely dark.
    CHECK(s.state().flame[0][0] == 0);
    CHECK(s.state().flame[0][1] == 0);
    // The other three directions still fire at the chained bomb's own full
    // reach: Right past where the igniter's own (reach-3, stopped-at-8) arm
    // ever got to, and Down (a fresh direction the igniter's arm never took).
    for (int x = 9; x <= 14; ++x) CHECK(s.state().flame[0][x] > 0);
    for (int y = 1; y <= 10; ++y) CHECK(s.state().flame[y][8] > 0);
}

TEST_CASE("burned brick reveals its powerup, players pick it up") {
    Simulation s(test_config());
    s.state().hidden[0][2] = PowerupType::ExtraBomb;  // plant under the brick
    s.tick(press1(0));
    run(s, s.state().tuning.fuse_frames - 1);
    CHECK(s.state().burning[0][2] > 0);
    // The powerup reveals RIGHT NOW, at ignition (sub_425107, called
    // immediately after the brick starts crumbling) — well before the tile
    // itself opens up (docs/re/facts.md "Brick crumble timing"). It is
    // visible/fading-in but still not collectible: the tile is still Brick,
    // still blocking, so a player cannot reach it yet.
    CHECK(s.state().floor[0][2] == PowerupType::ExtraBomb);
    CHECK(s.state().hidden[0][2] == PowerupType::None);
    CHECK(s.state().cells[0][2] == Cell::Brick);
    run(s, s.state().tuning.brick_burn_frames);
    CHECK(s.state().floor[0][2] == PowerupType::ExtraBomb);  // still there, now collectible

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

TEST_CASE("chained bombs explode within a tick of the trigger, not on bomb B's own fuse") {
    Simulation s(test_config());
    s.tick(press1(0));  // bomb A at (0,0), fuse 40
    // Walk down to (0,2): within bomb A's flame reach of 2.
    TickInputs down;
    down.players[0].down = true;
    for (int i = 0; i < 8; ++i) s.tick(down);
    CHECK(s.state().players[0].tile_y() == 2);
    s.tick(press1(0));  // bomb B at (0,2), ~10 ticks younger
    // Bomb A explodes at its fuse (~tick 40) and QUEUES bomb B, which
    // forcibly detonates the NEXT tick (docs/re/facts.md "Chain-reaction
    // timing", sub_423209's deferred queue — not the same tick as A). Both
    // are gone well before bomb B's own ~50-tick fuse would have fired.
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

// docs/re/facts.md "Scatter occupancy test" (sub_4255B2, pinned from
// pseudo.c 26458-26479): the re-roll predicate rejects a bomb, ANY powerup
// record, or a live PLAYER on the candidate tile — but NOT flame. A scattered
// token can land on a tile that is currently on fire, and never lands on a
// tile a player is standing on. Both cases isolated via a grid where every
// tile is Solid except: the victim's own tile at (5,5) (where the head hit
// happens), the bomb owner's tile at (7,7) (kept apart from the victim so it
// is never mistaken for the head-hit target), and exactly one OTHER blank
// tile at (3,3) whose contents this test controls — the sole legal (or
// contested) scatter candidate.
TEST_CASE("scattered token CAN land on a burning (flamed) tile") {
    MatchConfig cfg;
    for (int y = 0; y < kGridHeight; ++y)
        for (int x = 0; x < kGridWidth; ++x) cfg.cells[y][x] = Cell::Solid;
    cfg.cells[5][5] = Cell::Blank;  // victim's tile
    cfg.cells[7][7] = Cell::Blank;  // bomb owner's tile (apart from the victim)
    cfg.cells[3][3] = Cell::Blank;  // the ONLY other legal candidate tile
    cfg.spawns = {{7, 7}, {5, 5}};
    cfg.player_count = 2;
    cfg.seed = 1;  // this scatter draw sequence lands on (3,3) within budget
    cfg.tuning.powers_lost_min = 1;
    cfg.tuning.powers_lost_rand = 1;
    for (auto& c : cfg.tuning.spawn_counts) c = 0;
    Simulation s(cfg);
    s.state().flame[3][3] = 50;  // candidate tile is ON FIRE
    Player& v = s.state().players[1];
    v.kick = true;  // sole surplus kind: guarantees a droppable hit
    // Force a head hit on the victim: a flying bomb landing on its tile
    // (isolates scatter() from the flight/landing machinery already covered
    // by other tests).
    Bomb b;
    b.active = true;
    b.owner = 0;
    b.flame = 1;
    b.fuse = 10000;
    b.flying = true;
    b.from_x = b.x = 5 * kTileWF + kTileWF / 2;
    b.from_y = b.y = 5 * kTileHF + kTileHF / 2;
    b.to_x = b.x;
    b.to_y = b.y;
    b.dir = Direction::Right;
    b.fly_total = 1;
    b.fly_ticks = 1;
    s.state().bombs.push_back(b);
    run(s, 1);  // the single fly() step lands immediately on the victim's tile
    CHECK(v.stun > 0);
    CHECK(!v.kick);  // the surplus kind was dropped
    // The ONLY legal candidate tile is (3,3): with 100 outer attempts and a
    // board that is Solid everywhere else, the token can ONLY end up there
    // (or be lost) — and since flame does not block placement, it lands.
    CHECK(s.state().floor[3][3] == PowerupType::Kick);
}

TEST_CASE("scattered token NEVER lands on a tile a live player occupies") {
    // Regression-precise setup (seed=1 verified against BOTH revisions of
    // PowerupSystem::scatter): the ONLY three blank tiles are the victim's
    // (5,5), the bomb owner's (7,7), and a third player's (3,3) — all three
    // occupied by a live player. Without the player check, this exact seed
    // places the Kick token on the BOMB OWNER's own tile (7,7); because
    // powerup pickup runs later the same tick, the owner immediately picks
    // it back up — a PowerupPicked event fires and the token never reaches
    // the floor, silently masking the bug if you only inspect `floor`. The
    // fix must show NEITHER a floor token NOR a PowerupPicked event: the
    // token has to be rejected at the SCATTER step, not merely reclaimed.
    MatchConfig cfg;
    for (int y = 0; y < kGridHeight; ++y)
        for (int x = 0; x < kGridWidth; ++x) cfg.cells[y][x] = Cell::Solid;
    cfg.cells[5][5] = Cell::Blank;  // victim's tile
    cfg.cells[7][7] = Cell::Blank;  // bomb owner's tile
    cfg.cells[3][3] = Cell::Blank;  // candidate tile, occupied by a THIRD player
    cfg.spawns = {{7, 7}, {5, 5}, {3, 3}};
    cfg.player_count = 3;
    cfg.seed = 1;
    cfg.tuning.powers_lost_min = 1;
    cfg.tuning.powers_lost_rand = 1;
    for (auto& c : cfg.tuning.spawn_counts) c = 0;
    Simulation s(cfg);
    Player& v = s.state().players[1];
    v.kick = true;
    REQUIRE(s.state().players[2].present);
    REQUIRE(s.state().players[2].alive);
    REQUIRE(s.state().players[2].tile_x() == 3);
    REQUIRE(s.state().players[2].tile_y() == 3);

    Bomb b;
    b.active = true;
    b.owner = 0;
    b.flame = 1;
    b.fuse = 10000;
    b.flying = true;
    b.from_x = b.x = 5 * kTileWF + kTileWF / 2;
    b.from_y = b.y = 5 * kTileHF + kTileHF / 2;
    b.to_x = b.x;
    b.to_y = b.y;
    b.dir = Direction::Right;
    b.fly_total = 1;
    b.fly_ticks = 1;
    s.state().bombs.push_back(b);
    run(s, 1);
    CHECK(v.stun > 0);
    CHECK(!v.kick);  // the surplus kind was still dropped from the victim
    // The token is truly LOST: every one of the three blank tiles is a
    // player's own tile, so every scatter roll either hits Solid (re-roll,
    // no attempt burned) or a player tile (attempt burned, rejected) —
    // never placed, never picked up.
    for (int y = 0; y < kGridHeight; ++y)
        for (int x = 0; x < kGridWidth; ++x)
            CHECK(s.state().floor[y][x] != PowerupType::Kick);
    for (const auto& e : s.state().events)
        CHECK(e.type != Event::Type::PowerupPicked);  // never landed to be picked up
}

// Kill attribution on PlayerDied (docs/re/results-and-options.md §1's
// sub_421B0F kill tally; event.hpp's PlayerDied.data convention). Events are
// derived, unhashed per-tick outputs (CLAUDE.md determinism contract rule 4)
// — enriching them cannot move a golden hash, only carry more information
// out for the presentation layer to consume.
TEST_CASE("PlayerDied carries the flame owner as the killer (bomb-owner kill)") {
    Simulation s(test_config());
    // Player 0's bomb reaches player 1's spawn tile (14,10): drop a bomb with
    // enough flame reach right next to player 1 instead of moving them across
    // the whole board — place player 0 one tile west of player 1 and let its
    // blast reach east.
    Player& victim = s.state().players[1];
    int vx = victim.tile_x(), vy = victim.tile_y();
    s.state().players[0].x = (vx - 1) * kTileWF + kTileWF / 2;
    s.state().players[0].y = vy * kTileHF + kTileHF / 2;
    s.state().players[0].flame = 2;  // reach far enough to cover the victim
    s.tick(press1(0));               // player 0 drops a bomb on its own tile
    run(s, s.state().tuning.fuse_frames - 1);  // exactly at the fuse (see the "bomb explodes" test)
    REQUIRE(!victim.alive);
    bool found = false;
    for (const Event& e : s.state().events) {
        if (e.type != Event::Type::PlayerDied || e.player != 1) continue;
        found = true;
        CHECK(e.data == 0);  // killed by player 0's flame, not a self-kill
    }
    CHECK(found);
}

TEST_CASE("PlayerDied marks a self-kill: data equals the victim's own index") {
    Simulation s(test_config());
    // Player 0 stands on its own bomb; test_config's default flame reach
    // covers the epicenter, so it dies in its own blast (mirrors the "bomb
    // explodes at its fuse" test's incidental self-kill, but asserts the
    // event explicitly here).
    s.tick(press1(0));
    run(s, s.state().tuning.fuse_frames - 1);
    REQUIRE(!s.state().players[0].alive);
    bool found = false;
    for (const Event& e : s.state().events) {
        if (e.type != Event::Type::PlayerDied || e.player != 0) continue;
        found = true;
        CHECK(e.data == 0);         // self-kill: data == the victim's own index
        CHECK(e.data == e.player);  // event.hpp's explicit self-kill convention
    }
    CHECK(found);
}

TEST_CASE("PlayerDied from an enclosure wall crush has no killer (data == -1)") {
    // Mirrors "hurry walls spiral in, crush, and detonate bombs": depth-1
    // enclosure crushes player 0 (spawned at the top-left corner, the
    // spiral's first cell) with no attributable killer.
    MatchConfig cfg = test_config();
    cfg.tuning.game_seconds = 10;
    cfg.tuning.hurry_seconds = 8;
    cfg.tuning.enclosement_depth = 1;
    Simulation s(cfg);
    run(s, 40 + 99 + 1 + 4 + 1);  // banner, walls arm, first wall drops (see enclosure test)
    REQUIRE(!s.state().players[0].alive);
    bool found = false;
    for (const Event& e : s.state().events) {
        if (e.type != Event::Type::PlayerDied || e.player != 0) continue;
        found = true;
        CHECK(e.data == -1);  // no killer: a wall crush, not a flame
    }
    CHECK(found);
}
