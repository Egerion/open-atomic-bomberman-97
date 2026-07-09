// Punch feedback + throw sound fidelity — reverse-engineered from the punch
// glove handler sub_424A50 and the carried-bomb release in sub_41F29B (the
// +37 block), cross-checked against the SOUNDLST resource. Findings (#23):
//   1. The punch glove ALWAYS swings: sub_424A50 returns unconditionally, so
//      sub_41F29B sets the punch anim state on every press even with no bomb
//      ahead. Only the launch (sub_424987) and the SOUNDLST 150 SFX
//      (sub_427961(150)) sit inside `if (bomb ahead)`. So an empty swing is
//      animated but silent and launches nothing. We emit BombPunched every
//      press (drives the pose) and flag in the event data whether a bomb was
//      actually hit (so the SoundDirector only plays 150/151 on a real hit).
//   2. Throwing a carried bomb plays NO sound: the +37 release block calls
//      sub_424987 with no sub_427961 anywhere (the SOUNDLST "bmbthrw" ids
//      172-175 are dead assets, never played). BombThrown is still emitted so
//      the throw pose plays; the SoundDirector just maps it to silence.
// See docs/re/facts.md "Punch glove feedback" and "Throw is silent".

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "helpers.hpp"

using namespace bomber::sim;
using namespace bomber::sim::test;

namespace {

// True if the last tick emitted at least one event of this type.
bool has_event(const Simulation& s, Event::Type type) {
    for (const auto& ev : s.state().events)
        if (ev.type == type) return true;
    return false;
}

// The first event of `type` from the last tick (caller ensures it exists).
Event find_event(const Simulation& s, Event::Type type) {
    for (const auto& ev : s.state().events)
        if (ev.type == type) return ev;
    return Event{};
}

// Place a resting bomb centred on tile (tx,ty), owned by `owner`.
void push_resting_bomb(Simulation& s, int tx, int ty, std::uint8_t owner = 1) {
    Bomb b;
    b.active = true;
    b.owner = owner;
    b.x = tx * kTileWF + kTileWF / 2;
    b.y = ty * kTileHF + kTileHF / 2;
    b.fuse = 10000;  // long fuse: we observe the punch, not a timeout
    b.flame = 2;
    s.state().bombs.push_back(b);
}

}  // namespace

TEST_CASE("punching with no bomb ahead still gives feedback but launches nothing") {
    // The empty-swing case the user reported as "does nothing". The glove must
    // still animate (BombPunched emitted so the pose plays) while nothing is
    // launched and no hit is flagged (so the SoundDirector stays silent).
    Simulation s(open_config());
    s.state().players[1].alive = false;
    Player& p = s.state().players[0];
    p.punch = true;
    p.facing = Direction::Down;  // ahead of (0,0) is (0,1): open, no bomb there
    REQUIRE(s.state().bombs.empty());

    s.tick(press2(0));

    CHECK(has_event(s, Event::Type::BombPunched));       // feedback fired
    CHECK(find_event(s, Event::Type::BombPunched).data == 0);  // no bomb hit
    CHECK(s.state().bombs.empty());                      // launched nothing
}

TEST_CASE("punching a bomb ahead launches it and flags the hit") {
    Simulation s(open_config());
    s.state().players[1].alive = false;
    Player& p = s.state().players[0];
    p.punch = true;
    p.facing = Direction::Down;
    push_resting_bomb(s, 0, 1);  // a bomb directly ahead (owned by another)
    REQUIRE(s.state().bombs.size() == 1);
    REQUIRE(!s.state().bombs[0].flying);

    s.tick(press2(0));

    CHECK(has_event(s, Event::Type::BombPunched));
    CHECK(find_event(s, Event::Type::BombPunched).data == 1);  // hit flagged
    REQUIRE(s.state().bombs.size() == 1);
    CHECK(s.state().bombs[0].flying);                    // sent airborne
}

TEST_CASE("punch is edge-gated: holding the button swings once") {
    // The action edge in simulation.cpp (`action2 && !prev_action2`) fires the
    // punch once per press. Holding it must not re-emit BombPunched.
    Simulation s(open_config());
    s.state().players[1].alive = false;
    Player& p = s.state().players[0];
    p.punch = true;
    p.facing = Direction::Down;

    s.tick(press2(0));
    REQUIRE(has_event(s, Event::Type::BombPunched));  // first press swings
    s.tick(press2(0));                                // still held
    CHECK(!has_event(s, Event::Type::BombPunched));   // no second swing
}

TEST_CASE("throwing a carried bomb emits BombThrown") {
    // Grab a resting own bomb (action1 press while standing on it), then release
    // action1 to throw. BombThrown must fire (it drives the throw pose); the
    // SoundDirector maps it to silence, so no sound assertion here — the sim
    // contract is just that the event is emitted.
    Simulation s(open_config());
    s.state().players[1].alive = false;
    Player& p = s.state().players[0];
    p.grab = true;
    p.facing = Direction::Down;

    s.tick(press1(0));                 // drop a bomb underfoot at (0,0)
    REQUIRE(s.state().bombs.size() == 1);
    s.tick(TickInputs{});              // release the key (edge reset)
    s.tick(press1(0));                 // press again -> grab the bomb underfoot
    REQUIRE(s.state().players[0].carrying);

    // Clear the pickup stun while still holding (carrying + key down = no throw
    // yet), then release: the first un-stunned tick with the key up throws.
    bool thrown = false;
    for (int i = 0; i < 6 && !thrown; ++i) {
        s.tick(TickInputs{});          // key released
        thrown = has_event(s, Event::Type::BombThrown);
    }
    CHECK(thrown);
    CHECK(!s.state().players[0].carrying);  // the carried bomb was released
}

// docs/re/facts.md "Flying-bomb landing on powerups": the landing check
// (sub_42331C ~25443, `!sub_425FB9 && !sub_422E48 && !sub_42542D`) treats ANY
// floor powerup as an occupied tile, exactly like a wall or another bomb — a
// flying bomb hops over it instead of landing on (or destroying) it. This
// differs from the sliding-bomb cell-entry probe (sub_4230A5), which plows
// through a visible powerup and squashes it as a side effect.
TEST_CASE("a punched bomb hops over a floor powerup instead of landing on it") {
    Simulation s(open_config());
    s.state().players[1].alive = false;
    Player& p = s.state().players[0];
    p.punch = true;
    p.facing = Direction::Right;
    push_resting_bomb(s, 1, 0);          // directly ahead of (0,0)
    s.state().floor[0][4] = PowerupType::ExtraBomb;  // the 3-tile launch's landing tile

    s.tick(press2(0));                   // punch -> 3-tile launch to (4,0)
    REQUIRE(s.state().bombs.size() == 1);
    REQUIRE(s.state().bombs[0].flying);

    // The powerup must never be consumed while the bomb is still airborne.
    for (int i = 0; i < 40 && s.state().bombs[0].flying; ++i) {
        run(s, 1);
        CHECK(s.state().floor[0][4] == PowerupType::ExtraBomb);
    }
    // It settles somewhere other than the powerup tile (hopped past/around it).
    REQUIRE(!s.state().bombs.empty());
    CHECK_FALSE(s.state().bombs[0].flying);
    CHECK_FALSE((s.state().bombs[0].tile_x() == 4 && s.state().bombs[0].tile_y() == 0));
    // The powerup token itself is untouched: not destroyed, not picked up.
    CHECK(s.state().floor[0][4] == PowerupType::ExtraBomb);
}

// ---- Core-feel audit 2026-07-10 (facts.md "Core-feel audit" §5) ------------

TEST_CASE("a thrown bomb restarts its fuse from scratch (LABEL_246 zeroes elapsed +68)") {
    Simulation s(open_config());
    s.state().players[1].alive = false;
    Player& p = s.state().players[0];
    p.grab = true;
    p.facing = Direction::Down;

    s.tick(press1(0));  // drop underfoot at (0,0): fuse starts at fuse_frames
    REQUIRE(s.state().bombs.size() == 1);
    run(s, 10);         // let the fuse burn well down
    const std::int32_t burnt = s.state().bombs[0].fuse;
    REQUIRE(burnt < s.state().tuning.fuse_frames - 5);

    s.tick(press1(0));  // grab it (standing on own bomb, rising edge)
    REQUIRE(p.carrying);
    bool thrown = false;
    for (int i = 0; i < 6 && !thrown; ++i) {
        s.tick(TickInputs{});  // release past the pickup stun -> throw
        thrown = !s.state().bombs.empty();
    }
    REQUIRE(thrown);
    const Bomb& b = s.state().bombs[0];
    REQUIRE(b.flying);
    // Airborne, fuse frozen — and RESET to the full creation-time duration,
    // not the burnt remnant it froze with at grab time.
    CHECK(b.fuse == s.state().tuning.fuse_frames);
}

TEST_CASE("punch/throw feedback stays deterministic across replays") {
    MatchConfig cfg = open_config();
    Simulation a(cfg), b(cfg);
    a.state().players[0].punch = true;
    b.state().players[0].punch = true;
    a.state().players[0].grab = true;
    b.state().players[0].grab = true;
    TickInputs in;
    for (int t = 0; t < 200; ++t) {
        in.players[0].action1 = t % 17 == 0;
        in.players[0].action2 = t % 11 == 0;
        in.players[0].right = (t / 5) % 2 == 0;
        in.players[0].down = (t / 8) % 2 == 0;
        a.tick(in);
        b.tick(in);
    }
    CHECK(a.hash() == b.hash());
}
