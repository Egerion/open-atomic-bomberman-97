// Kicked-bomb nuances — reverse-engineered from the bomb updater sub_42331C
// (kicked-slide block) cross-checked against sub_42708D (flame-at), sub_405654
// (stage-actor-at-tile) and sub_42464B (the kick handler). Audit findings (#8):
//   1. A bomb sliding onto a flaming tile EXPLODES (sub_42331C runs the
//      sub_42708D flame check each pixel-step) — FIXED here.
//   2. The mid-slide "re-steer" reads a DIRARROW/conveyor stage actor's godir
//      (sub_405654 scans the level-actor registry), NOT a resting player. We
//      do not model dirarrows yet, so there is nothing faithful to add; this
//      is deferred to ROADMAP #7 (conveyors/arrows).
//   3. Kicked-bomb speed is the fixed VALUELST id 300 (sub_42464B sets bomb
//      +112 = getvalue(300)); NOT a per-player base — already faithful.
// See docs/re/facts.md "Kick nuances".

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "helpers.hpp"

using namespace bomber::sim;
using namespace bomber::sim::test;

namespace {

// Drop a resting, sliding bomb at tile (tx,ty) heading dir. White-box: we set
// the motion directly to isolate the slide from the kick trigger (kick input
// is already covered in test_jelly).
void push_sliding_bomb(Simulation& s, int tx, int ty, Direction dir,
                       std::uint8_t owner = 0) {
    Bomb b;
    b.active = true;
    b.owner = owner;
    b.x = tx * kTileWF + kTileWF / 2;
    b.y = ty * kTileHF + kTileHF / 2;
    b.fuse = 10000;  // long fuse: we want to observe the slide, not a timeout
    b.flame = 2;
    b.moving = true;
    b.dir = dir;
    s.state().bombs.push_back(b);
}

}  // namespace

TEST_CASE("a bomb sliding into a flame tile explodes") {
    Simulation s(open_config());
    // Both spawned players stay ALIVE at their far corners (0,0)/(14,10),
    // clear of the row-0 bomb lane — a 2-side quorum, so the chain drain that
    // forcibly detonates the flame-caught bomb is not frozen (bombs F1).
    park_players_clear(s);

    push_sliding_bomb(s, 2, 0, Direction::Right);
    // A long-lived flame at (4,0) owned by player 1 (>0 so the slide's flame
    // check trips as the bomb enters it). Value 200 easily outlasts the couple
    // of ticks it takes the bomb to arrive (flames age 1/tick).
    s.state().flame[0][4] = 200;
    s.state().flame_owner[0][4] = 1;

    // Slide east; on entering (4,0) it must detonate (become inactive, then be
    // compacted out) rather than sail through the fire.
    bool gone = false;
    for (int i = 0; i < 30 && !gone; ++i) {
        s.tick(TickInputs{});
        gone = s.state().bombs.empty();
    }
    CHECK(gone);
    // The detonation left flame owned by the bomb's owner (0) on the field.
    bool flame_from_blast = false;
    for (int x = 0; x < kGridWidth && !flame_from_blast; ++x)
        if (s.state().flame_owner[0][x] == 0 && s.state().flame[0][x] > 0)
            flame_from_blast = true;
    CHECK(flame_from_blast);
}

// A jelly bomb sliding into flame BOUNCES (reverses, keeps sliding) exactly
// like it does off a wall, instead of stopping — but it is still QUEUED for
// a forced detonation one tick later (docs/re/facts.md "Chain-reaction
// timing"; the queue push happens unconditionally, before the jelly/non-
// jelly stop-vs-bounce branch, pseudo.c 25545-25554).
TEST_CASE("a jelly bomb sliding into flame bounces, but still chain-detonates") {
    Simulation s(open_config());
    park_players_clear(s);  // 2-side quorum: the deferred detonation is not frozen (bombs F1)

    push_sliding_bomb(s, 2, 0, Direction::Right);
    s.state().bombs[0].jelly = true;
    // A long-lived flame at (4,0); bouncing off it must not cancel the
    // deferred detonation the entry already queued.
    s.state().flame[0][4] = 200;
    s.state().flame_owner[0][4] = 1;

    bool bounced = false;
    for (int i = 0; i < 30 && !bounced; ++i) {
        s.tick(TickInputs{});
        for (const auto& e : s.state().events)
            if (e.type == Event::Type::JellyBounced) bounced = true;
    }
    CHECK(bounced);
    REQUIRE(!s.state().bombs.empty());
    CHECK(s.state().bombs[0].moving);              // reversed, did NOT stop
    CHECK(s.state().bombs[0].dir == Direction::Left);

    // The queued detonation still fires, one tick after entering the flame.
    bool gone = false;
    for (int i = 0; i < 30 && !gone; ++i) {
        s.tick(TickInputs{});
        gone = s.state().bombs.empty();
    }
    CHECK(gone);
}

TEST_CASE("a bomb sliding through open ground does NOT explode early") {
    // Control for the flame case: no flame in the lane -> the bomb just parks.
    Simulation s(open_config());
    s.state().players[0].alive = false;
    s.state().players[1].alive = false;
    s.state().cells[0][6] = Cell::Solid;  // a wall to stop it, no flame

    push_sliding_bomb(s, 2, 0, Direction::Right);
    run(s, 30);
    REQUIRE(s.state().bombs.size() == 1);
    CHECK(!s.state().bombs[0].moving);
    CHECK(s.state().bombs[0].tile_x() == 5);  // parked flush at the wall
}

TEST_CASE("a bomb kicked into a fresh explosion is caught by the flame") {
    // End-to-end: a real kick, then the bomb rolls into flame from another
    // bomb's blast and goes off, instead of sliding past it.
    Simulation s(open_config());
    // player 1 stays alive at (14,10) for the 2-side quorum (bombs F1); it is
    // far clear of the row-0 lane the kicked bomb travels.
    Player& p = s.state().players[0];
    p.kick = true;
    p.flame = 1;
    p.x = 2 * kTileWF + kTileWF / 2;  // stand at (2,0)
    p.y = kTileHF / 2;

    s.tick(press1(0));  // lay a bomb at (2,0)
    REQUIRE(s.state().bombs.size() == 1);

    TickInputs left;
    left.players[0].left = true;
    run(s, 12, left);   // step off to the west
    TickInputs right;
    right.players[0].right = true;
    run(s, 8, right);   // walk back east -> kick the bomb east
    REQUIRE(s.state().bombs[0].moving);

    // Put a long-lived flame directly in the sliding bomb's path; it must
    // detonate on entry instead of gliding through (value 200 outlasts travel).
    s.state().flame[0][6] = 200;
    s.state().flame_owner[0][6] = 1;
    bool gone = false;
    for (int i = 0; i < 40 && !gone; ++i) {
        s.tick(TickInputs{});
        gone = s.state().bombs.empty();
    }
    CHECK(gone);
}

TEST_CASE("kick nuances stay deterministic across replays") {
    MatchConfig cfg = open_config();
    Simulation a(cfg), b(cfg);
    a.state().players[0].kick = true;
    b.state().players[0].kick = true;
    TickInputs in;
    for (int t = 0; t < 200; ++t) {
        in.players[0].action1 = t % 29 == 0;
        in.players[0].right = (t / 4) % 2 == 0;
        in.players[0].down = (t / 9) % 2 == 0;
        a.tick(in);
        b.tick(in);
    }
    CHECK(a.hash() == b.hash());
}

// ---- Core-feel audit 2026-07-10 (facts.md "Core-feel audit" §1/§4) --------

TEST_CASE("the kick fires on the walk-up's ARRIVAL tick (sub_41EC84 !v35 in-loop probe)") {
    Simulation s(open_config());
    s.state().players[1].alive = false;
    Player& p = s.state().players[0];
    p.kick = true;
    // Stand 15 px west of the (1,0) centre; a resting bomb waits at (2,0).
    p.x = 1 * kTileWF + kTileWF / 2 - 15 * kScale;
    p.y = kTileHF / 2;
    push_sliding_bomb(s, 2, 0, Direction::Right);
    s.state().bombs[0].moving = false;  // resting target

    TickInputs right;
    right.players[0].right = true;
    // Tick 1: 10 px (budget 923 spends 10 iterations) -> 5 px short; no kick.
    s.tick(right);
    CHECK(!s.state().bombs[0].moving);
    // Tick 2: reaches the centre and is pinned there; the SAME tick's probe
    // (the original's in-loop v35 == 0 check) kicks the bomb — not tick 3, as
    // the old post-stall gate had it.
    s.tick(right);
    CHECK(s.state().bombs[0].moving);
    CHECK(s.state().bombs[0].dir == Direction::Right);
}

TEST_CASE("a bomb sliding across the player's face is snapped and REDIRECTED (sub_42464B)") {
    Simulation s(open_config());
    s.state().players[1].alive = false;
    Player& p = s.state().players[0];
    p.kick = true;
    p.x = 2 * kTileWF + kTileWF / 2;  // centred at (2,0), will face east
    p.y = kTileHF / 2;
    // A bomb sliding WEST, currently on the tile directly ahead (3,0); the
    // tile beyond it (4,0) is open, so the kick handler accepts it.
    push_sliding_bomb(s, 3, 0, Direction::Left);

    TickInputs right;
    right.players[0].right = true;
    s.tick(right);
    REQUIRE(s.state().bombs.size() == 1);
    const Bomb& b = s.state().bombs[0];
    CHECK(b.moving);                     // still sliding — never stopped
    CHECK(b.dir == Direction::Right);    // ... but now AWAY from the player
    CHECK(b.tile_x() == 3);              // snapped onto the ahead tile's centre
}

TEST_CASE("re-kicking a bomb already sliding the same way is a silent no-op") {
    Simulation s(open_config());
    s.state().players[1].alive = false;
    Player& p = s.state().players[0];
    p.kick = true;
    p.x = 2 * kTileWF + kTileWF / 2;
    p.y = kTileHF / 2;
    push_sliding_bomb(s, 3, 0, Direction::Right);  // already fleeing east

    TickInputs right;
    right.players[0].right = true;
    s.tick(right);
    // No BombKicked event: sub_42464B only plays the kick sound when the
    // direction changes or the bomb was resting.
    for (const auto& e : s.state().events) CHECK(e.type != Event::Type::BombKicked);
    CHECK(s.state().bombs[0].dir == Direction::Right);
    CHECK(s.state().bombs[0].moving);
}

TEST_CASE("kick + action2 stops own sliding bombs at the next tile centre (sub_4247C5)") {
    Simulation s(open_config());
    s.state().players[0].kick = true;
    s.state().players[1].alive = false;
    // An own bomb mid-slide, 15 px short of the (2,0) centre, heading east.
    push_sliding_bomb(s, 2, 0, Direction::Right, /*owner=*/0);
    s.state().bombs[0].x -= 15 * kScale;

    // press2 flags the bomb (+57); the SAME tick's slide (kicked speed + the
    // flat +100*kSubFrames bonus, bombs F2 = 19 px) carries it onto the (2,0)
    // centre and the pending-stop snaps it there — it halts ON the centre, not
    // 15 px back where the key was pressed. (With the pre-F2 10 px/tick this
    // took two ticks; the deferred-flag mechanic is unchanged, only faster.)
    s.tick(press2(0));
    run(s, 2);
    const Bomb& b = s.state().bombs[0];
    CHECK(!b.moving);  // halted ON the centre, not where the key was pressed
    CHECK(b.x == 2 * kTileWF + kTileWF / 2);
    CHECK(!b.stop_pending);
}

TEST_CASE("kick + action2 does NOT stop jelly bombs (sub_4247C5 skips kind 2)") {
    Simulation s(open_config());
    s.state().players[0].kick = true;
    s.state().players[1].alive = false;
    push_sliding_bomb(s, 2, 0, Direction::Right, /*owner=*/0);
    s.state().bombs[0].jelly = true;

    s.tick(press2(0));
    CHECK(!s.state().bombs[0].stop_pending);
    run(s, 4);
    CHECK(s.state().bombs[0].moving);  // still ping-ponging along
}
