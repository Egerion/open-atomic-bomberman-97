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
    victim.owner = 1;
    victim.x = kTileWF + kTileWF / 2;  // tile (1,0)
    victim.y = kTileHF / 2;
    victim.fuse = 30000;  // never fires on its own within this test
    victim.flame = 0;     // its own chain explosion covers ONLY its own tile
    s.state().bombs.push_back(victim);
    ++s.state().players[1].bombs_placed;

    Bomb igniter;
    igniter.active = true;
    igniter.owner = 0;
    igniter.x = kTileWF / 2;  // tile (0,0)
    igniter.y = kTileHF / 2;
    igniter.fuse = 1;
    igniter.flame = 3;  // would reach tile (3,0) if the arm sailed through
    s.state().bombs.push_back(igniter);
    ++s.state().players[0].bombs_placed;

    run(s, 1);  // igniter's fuse expires this tick
    CHECK(s.state().bombs.empty());          // both chain-detonated
    CHECK(s.state().flame[0][1] > 0);        // victim's tile burns (its OWN epicentre)
    CHECK(s.state().flame[0][2] == 0);       // the igniter's arm did NOT continue past it
    int explosion_count = 0;
    for (const auto& e : s.state().events)
        if (e.type == Event::Type::Explosion) ++explosion_count;
    CHECK(explosion_count == 2);  // igniter + chained victim
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
    // CONFIRMED enclosure timing (sub_426818, docs/re/enclosure.md §2): the
    // "HURRY!" banner/sound fires the first tick remaining seconds is
    // STRICTLY LESS than hurry_seconds; the WALLS arm the first tick
    // remaining seconds is <= hurry_seconds - 5 (non-strict), and drop one
    // EVENT every 250 ms = 5 ticks at 20 Hz. With game_seconds=10 (ticks_left
    // starts at 200) and hurry_seconds=8: remaining-seconds = ticks_left/20
    // (floor), so "< 8" first holds at ticks_left==159 (tick 41) and "<= 3"
    // first holds at ticks_left==79 (tick 121).
    MatchConfig cfg = test_config();
    cfg.tuning.game_seconds = 10;  // 200 ticks
    cfg.tuning.hurry_seconds =
        8;  // banner at tick 41 (remaining 7s); walls arm tick 121 (remaining 3s)
    cfg.tuning.enclosement_depth = 1;
    Simulation s(cfg);
    run(s, 41);
    CHECK(s.state().hurry);               // banner fired at moment 1 (tick 41)
    CHECK(s.state().enclose_index == 0);  // ...but the walls have NOT begun
    CHECK(s.state().cells[0][0] != Cell::Solid);
    run(s, 79);  // up to tick 120: still just the banner
    CHECK(s.state().enclose_index == 0);
    run(s, 1);                            // tick 121: walls arm (remaining 3s)
    CHECK(s.state().enclose_index == 0);  // arm frame drops nothing (gate shut)
    run(s, 4);
    CHECK(s.state().enclose_index == 0);  // still nothing at arm+4
    run(s, 1);                            // arm+5 (tick 126): first wall drops
    CHECK(s.state().enclose_index == 1);
    CHECK(s.state().cells[0][0] == Cell::Solid);  // spiral starts top-left
    CHECK(!s.state().players[0].alive);           // player 0 spawned there: crushed
    // Cadence: exactly one more EVENT every 5 ticks (see the full ring-0 pin
    // below for where "event" and "new tile" start to diverge).
    run(s, 4);
    CHECK(s.state().enclose_index == 1);
    run(s, 1);
    CHECK(s.state().enclose_index == 2);  // (1,0) closed at arm+10 (tick 131)
    CHECK(s.state().cells[0][1] == Cell::Solid);
    // A parked bomb in the wall's path must detonate, not linger. Tile (4,0)
    // is event index 4 (no phantom repeats before it — those start at the
    // ring's first corner, index 15), so it closes at arm + 5*(4+1) = tick 146.
    Bomb b;
    b.active = true;
    b.owner = 1;
    b.x = 4 * kTileWF + kTileWF / 2;
    b.y = kTileHF / 2;  // tile (4,0), on the outer ring
    b.fuse = 30000;
    b.flame = 1;
    s.state().bombs.push_back(b);
    s.state().players[1].bombs_placed = 1;
    run(s, 15);  // tick 146: (4,0) closes on the bomb
    CHECK(s.state().cells[0][4] == Cell::Solid);
    // sub_423209(bomb, -1) only QUEUES the detonation; sub_42331C's
    // once-per-frame drain (which force-fires it) runs BEFORE sub_426818 each
    // frame, so a bomb queued by THIS frame's wall drop isn't drained until
    // the FOLLOWING frame — a confirmed one-tick defer, not instant. The
    // bomb is still here immediately after the drop...
    CHECK(!s.state().bombs.empty());
    run(s, 1);                       // tick 147: forced fuse=1 expires
    CHECK(s.state().bombs.empty());  // ...gone exactly one tick later
    // Depth 1 closes rings 0 and 1 (VALUELST 27's authored comment, DATA/RES/
    // VALUELST.RES: "1 is 2 rows") and leaves ring 2 open: (1,1) is ring 1's
    // first tile (event index 52), (2,2) is ring 2 and never closes. Run out
    // the rest of the sequence (96 events total for depth 1) and check both.
    run(s, 601 - 147);  // tick 601 = arm+5 + 5*95: the last of 96 events has landed
    CHECK(s.state().cells[1][1] == Cell::Solid);
    CHECK(s.state().cells[2][2] == Cell::Blank);
    CHECK(s.state().enclose_index == enclose_total(1));
}

TEST_CASE("the enclosure spiral's full ring-0 event order, phantoms and all") {
    // A COMPLETE pin of the crux: the exact tile sequence sub_426818's
    // advance/accept-or-turn state machine produces for the outermost ring of
    // a 15x11 board (docs/re/enclosure.md §4), reconstructed by literally
    // re-running that state machine (not a hand-derived ring formula). Walks
    // clockwise from (0,0): the top edge (0,0)..(14,0), a PHANTOM repeat of
    // (14,0) (the turn Right->Down rejects without moving), the right edge
    // (14,1)..(14,10), a phantom repeat of (14,10) (Down->Left), the bottom
    // edge (13,10)..(0,10), a phantom repeat of (0,10) (Left->Up), then the
    // left edge (0,9)..(0,0) — ending with an ORDINARY (non-phantom) second
    // visit to (0,0) itself: the bounds check only excludes tiles outside the
    // ring box, not tiles already visited, so the up-walk runs all the way
    // back to its own start. 52 events, 48 unique tiles, exactly matching
    // EnclosureSystem::total/position for depth 1's first ring (index 0..51;
    // ring 1 starts at index 52).
    static constexpr int kRing0[52][2] = {
        {0, 0},   {1, 0},   {2, 0},   {3, 0},   {4, 0},  {5, 0},  {6, 0},  {7, 0},   {8, 0},
        {9, 0},   {10, 0},  {11, 0},  {12, 0},  {13, 0}, {14, 0}, {14, 0}, {14, 1},  {14, 2},
        {14, 3},  {14, 4},  {14, 5},  {14, 6},  {14, 7}, {14, 8}, {14, 9}, {14, 10}, {14, 10},
        {13, 10}, {12, 10}, {11, 10}, {10, 10}, {9, 10}, {8, 10}, {7, 10}, {6, 10},  {5, 10},
        {4, 10},  {3, 10},  {2, 10},  {1, 10},  {0, 10}, {0, 10}, {0, 9},  {0, 8},   {0, 7},
        {0, 6},   {0, 5},   {0, 4},   {0, 3},   {0, 2},  {0, 1},  {0, 0},
    };
    CHECK(enclose_total(1) == 96);  // ring 0 (52) + ring 1 (44)
    for (int i = 0; i < 52; ++i) {
        int x = -1, y = -1;
        REQUIRE(enclose_pos(i, 1, &x, &y));
        CHECK(x == kRing0[i][0]);
        CHECK(y == kRing0[i][1]);
    }
    // Ring 1 (13x9 box, one step inward) starts right where ring 0 ends.
    int x = -1, y = -1;
    REQUIRE(enclose_pos(52, 1, &x, &y));
    CHECK(x == 1);
    CHECK(y == 1);
}

TEST_CASE("a bouncing or warping player is immune to the closing wall") {
    // sub_421D3F -> sub_41DE63: the shared kill routine used by the crush,
    // flame-death, and rover-landing-kill paths all early-out (return 0, no
    // death) while the victim's movement state is 5 (trampoline hop) or 6/7
    // (warp out/in) — docs/re/campaign.md clause 4, stage-actors.md §592. The
    // wall still solidifies the tile; only the player is spared. White-box:
    // force the state directly (real trampoline/warphole setup is already
    // covered by test_trampoline.cpp / test_stage_actors.cpp) right before
    // the crush tick — bounce=15 decrements to 14 (c=16, past the len/2==15
    // apex) and warp=12 decrements to 11 (nowhere near kWarpMid==9), so
    // neither relocates the player off the tile this same tick.
    MatchConfig cfg = test_config();
    cfg.tuning.game_seconds = 10;
    cfg.tuning.hurry_seconds = 8;
    cfg.tuning.enclosement_depth = 1;

    Simulation s(cfg);
    run(s, 41 + 79 + 1 + 4);  // tick 125: one tick before (0,0) closes
    s.state().players[0].bounce = 15;
    run(s, 1);                                    // tick 126: (0,0) closes
    CHECK(s.state().cells[0][0] == Cell::Solid);  // the wall still drops...
    CHECK(s.state().players[0].alive);            // ...but the bouncing player survives

    Simulation s2(cfg);
    run(s2, 41 + 79 + 1 + 4);
    s2.state().players[0].warp = 12;
    run(s2, 1);
    CHECK(s2.state().cells[0][0] == Cell::Solid);
    CHECK(s2.state().players[0].alive);  // the warping player survives too
}

TEST_CASE("a ring corner replays the wall-slam event before the next new tile") {
    // The "phantom" repeat (docs/re/enclosure.md §4): sub_426818 unconditionally
    // re-drops sub_425E9B(x,y) — and replays the sound (sub_4278F2) — at
    // whatever (x,y) currently is on EVERY 250 ms cadence slot, including a
    // rejected turn that didn't move. The first phantom in the sequence is at
    // event index 15, tile (14,0) (see the ring-0 pin above); event index 16
    // is the first NEW tile past the corner, (14,1). `events` is cleared every
    // tick (state.hpp), so each occurrence must be checked immediately after
    // its own tick, not after a multi-tick jump.
    MatchConfig cfg = test_config();
    cfg.tuning.game_seconds = 10;
    cfg.tuning.hurry_seconds = 8;
    cfg.tuning.enclosement_depth = 1;
    Simulation s(cfg);
    run(s, 41 + 79 + 1 + 4 + 1);  // tick 126: event index 0, (0,0), drops
    run(s, 14 * 5);               // tick 196: event index 14, (14,0), drops (a NEW tile)
    CHECK(s.state().enclose_index == 15);
    CHECK(s.state().cells[0][14] == Cell::Solid);
    auto wall_closed_140_this_tick = [&] {
        int n = 0;
        for (const auto& e : s.state().events)
            if (e.type == Event::Type::WallClosed && e.x == 14 && e.y == 0) ++n;
        return n;
    };
    CHECK(wall_closed_140_this_tick() == 1);  // (14,0)'s first, ordinary WallClosed

    run(s, 4);
    CHECK(s.state().enclose_index == 15);  // no drop yet (mid-cadence)
    run(s, 1);                             // tick 201: event index 15, the PHANTOM repeat
    CHECK(s.state().enclose_index == 16);
    CHECK(s.state().cells[0][14] == Cell::Solid);  // unchanged — already solid
    CHECK(wall_closed_140_this_tick() == 1);       // (14,0)'s SECOND WallClosed, same tile

    run(s, 4);
    CHECK(s.state().enclose_index == 16);  // no drop yet
    run(s, 1);                             // tick 206: event index 16, the first NEW tile
    CHECK(s.state().enclose_index == 17);
    CHECK(s.state().cells[1][14] == Cell::Solid);  // (14,1): past the corner at last
    CHECK(wall_closed_140_this_tick() == 0);       // this tick's event is for (14,1), not (14,0)
}

TEST_CASE("enclosure interval is a fixed 5 ticks (250 ms at 20 Hz)") {
    // Pins the CONFIRMED cadence directly: independent of depth/ring count, the
    // per-wall interval is 250 ms / (1000/20) = 5 ticks (sub_426818 `+= 250`,
    // NOT a spread-to-fit heuristic). docs/re/enclosure.md §3. With
    // game_seconds=20 (ticks_left starts at 400) and hurry_seconds=20, remaining
    // seconds = ticks_left/20 (floor); "<= 15" first holds at ticks_left==319
    // (tick 81).
    MatchConfig cfg = test_config();
    cfg.tuning.game_seconds = 20;  // 400 ticks
    cfg.tuning.hurry_seconds =
        20;  // banner at tick 1 (remaining 19s); walls arm tick 81 (remaining 15s)
    cfg.tuning.enclosement_depth = 3;  // all rings — proves interval != f(n)
    Simulation s(cfg);
    run(s, 81);                                // reach the wall-arm (remaining 15s)
    REQUIRE(s.state().enclose_interval == 5);  // armed with the fixed interval
    // Walk the index forward and confirm it steps exactly once per 5 ticks.
    int last = s.state().enclose_index;
    for (int k = 0; k < 6; ++k) {
        run(s, 4);
        CHECK(s.state().enclose_index == last);  // no drop in the first 4
        run(s, 1);
        CHECK(s.state().enclose_index == last + 1);  // one drop on the 5th
        ++last;
    }
}

TEST_CASE("the HURRY banner precedes the walls by 5 seconds (two distinct moments)") {
    // The banner/sound (Hurry event) fires the first tick remaining < hurry_
    // seconds (strict); the walls do not arm until remaining <= hurry_seconds
    // - 5 (non-strict), 5 whole seconds later. Confirmed: HUD block ~29533
    // (dword_464984, strict `<`/`>` pair) vs sub_426818 (dword_45BE9C, `<=`).
    MatchConfig cfg = test_config();
    cfg.tuning.game_seconds = 10;  // 200 ticks
    cfg.tuning.hurry_seconds =
        8;  // banner at tick 41 (remaining 7s); walls arm tick 121 (remaining 3s)
    cfg.tuning.enclosement_depth = 1;
    Simulation s(cfg);
    run(s, 41);              // remaining 7s: the banner window opens
    CHECK(s.state().hurry);  // banner/sound event fired
    bool hurry_evt = false;
    for (const auto& e : s.state().events)
        if (e.type == Event::Type::Hurry) hurry_evt = true;
    CHECK(hurry_evt);                     // the Hurry event fired on this exact tick
    run(s, 79);                           // tick 120: still inside the banner window (remaining 4s)
    CHECK(s.state().enclose_index == 0);  // ...but no wall has dropped
    CHECK(s.state().cells[0][0] != Cell::Solid);
    run(s, 6);  // tick 126: walls have started
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

// docs/re/facts.md "Head hit" (gate-fidelity follow-up, 2026-07-10): the
// disease audit flagged sub_41F29B's `if (!*((_DWORD*)v111+2))` gate
// (~22904, mirrored locally in sub_41EC84 ~22699) as "adjacent, not acted
// on" because it also wraps the flame-death check and the floor-powerup
// pickup dispatch. Full brace-traced re-read: that DWORD at offset+8 is NOT
// the stun countdown -- it is the player's "already died this round" flag,
// set only by sub_41DCB2 (~21956, `*(_DWORD*)(v5+8) = 1`, itself guarded on
// "not already dead") and cleared only by the round-entry reset (~22874),
// which never re-fires after a death (no mid-round respawn). The REAL
// head-hit stun counter is a SEPARATE WORD field at offset+58 (sub_421F7E's
// `a1[29] = 16`, confirmed by its explicit `_WORD *a1` parameter typing) --
// it is read at pseudo.c ~22982/~23086 and gates only ONE thing: new-input
// acquisition (the `v113` local at ~23028, which skips sub_41E61E/AI so the
// player can't change direction or fire a new action) plus a cosmetic
// standing-animation frame pick. A merely-stunned-but-ALIVE player leaves
// offset+8 at 0, so sub_42708D/sub_41DE63 (flame death) and sub_42542D/
// sub_41E21E (pickup) both still run every tick regardless of stun --
// stun is not flame immunity and does not block pickup. `field_vs_players`
// (simulation.cpp) already matches this: it gates on `!p.alive` only, never
// `p.stun`. No production code changed; this pins the (deliberately)
// stun-independent behaviour so a future patch doesn't "fix" it backwards.
TEST_CASE("a stunned-but-alive player still burns and still picks up floor powerups") {
    Simulation s(open_config());

    // (a) Flame death is not blocked by stun.
    {
        Player& p = s.state().players[0];
        p.stun = 5;  // stand-in for an ongoing head-hit/grab-pause stun
        int tx = p.tile_x(), ty = p.tile_y();
        s.state().flame[ty][tx] = 200;  // active flame underfoot this tick
        run(s, 1);
        CHECK(p.stun > 0);  // still stunned...
        CHECK(!p.alive);    // ...but the flame killed it anyway: not immune
    }

    // (b) Floor-powerup pickup is not blocked by stun.
    {
        Player& p = s.state().players[1];
        p.stun = 5;
        int before = p.flame;
        int tx = p.tile_x(), ty = p.tile_y();
        s.state().floor[ty][tx] = PowerupType::Flame;
        run(s, 1);
        CHECK(p.stun > 0);              // still stunned...
        CHECK(p.flame == before + 1);   // ...but picked the token up anyway
        CHECK(s.state().floor[ty][tx] == PowerupType::None);
    }
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
    run(s, 41 + 79 + 1 + 4 +
               1);  // banner, walls arm, first wall drops (see enclosure test) -> tick 126
    REQUIRE(!s.state().players[0].alive);
    bool found = false;
    for (const Event& e : s.state().events) {
        if (e.type != Event::Type::PlayerDied || e.player != 0) continue;
        found = true;
        CHECK(e.data == -1);  // no killer: a wall crush, not a flame
    }
    CHECK(found);
}
