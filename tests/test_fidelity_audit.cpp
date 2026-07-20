// Dedicated regression tests for the 2026-07-20 SIM-side fidelity audit batch
// (docs/re/fidelity-audit.md, docs/re/audit/*.md). One TEST_CASE per confirmed
// finding, each citing the original sub_XXXX + pseudo.c line in the fix's own
// code comment. RNG-reorder findings that cannot be isolated cleanly here
// (flames F1 arm order, ai F2 stale path cost) are pinned by golden D's
// byte-identical kExpectedRng stream in tests/test_golden.cpp instead.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "helpers.hpp"

using namespace bomber::sim;
using namespace bomber::sim::test;

namespace {

// A hand-built armed bomb resting at tile (tx,ty), owned by `owner`.
Bomb armed_bomb(int tx, int ty, std::uint8_t owner, std::int32_t fuse) {
    Bomb b;
    b.active = true;
    b.owner = owner;
    b.x = tx * kTileWF + kTileWF / 2;
    b.y = ty * kTileHF + kTileHF / 2;
    b.fuse = fuse;
    b.fuse_init = fuse;
    b.flame = 1;
    return b;
}

}  // namespace

// ---------------------------------------------------------------------------
// bombs F1 — round-end bomb freeze (sub_42331C @ pseudo.c 25603, sub_421969)
// ---------------------------------------------------------------------------
TEST_CASE("bombs F1: an armed bomb explodes with 2 alive sides") {
    Simulation s(open_config());  // players at (0,0)/(14,10), both alive -> 2 sides
    s.state().bombs.push_back(armed_bomb(7, 5, /*owner=*/0, /*fuse=*/3));
    ++s.state().players[0].bombs_placed;
    run(s, 6);
    CHECK(s.state().bombs.empty());       // fuse ran, it detonated
    CHECK(s.state().flame[5][7] > 0);     // its explosion flame is on the field
    // Sanity: nobody was near (7,5), so both sides survived the whole time.
    CHECK(s.state().players[0].alive);
    CHECK(s.state().players[1].alive);
}

TEST_CASE("bombs F1: a round decided to <= 1 side FREEZES every armed bomb") {
    Simulation s(open_config());
    s.state().players[1].alive = false;   // only player 0 remains -> 1 side
    s.state().bombs.push_back(armed_bomb(7, 5, /*owner=*/0, /*fuse=*/3));
    ++s.state().players[0].bombs_placed;
    run(s, 20);
    REQUIRE(s.state().bombs.size() == 1);   // never detonated
    CHECK(s.state().bombs[0].fuse == 3);    // the fuse is frozen, not counting down
    CHECK(s.state().flame[5][7] == 0);      // no explosion happened
}

TEST_CASE("bombs F1: the freeze is EXEMPT in campaign (sub_421969 forces 2)") {
    Simulation s(open_config());
    s.state().players[1].alive = false;            // 1 side...
    s.state().campaign_hazards_active = true;      // ...but campaign (dword_46489C)
    s.state().bombs.push_back(armed_bomb(7, 5, /*owner=*/0, /*fuse=*/3));
    ++s.state().players[0].bombs_placed;
    run(s, 6);
    CHECK(s.state().bombs.empty());  // rovers/ghosts aren't players: no freeze, it fires
}

// ---------------------------------------------------------------------------
// bombs F2 — kicked/conveyor flat +100*kSubFrames ground bonus (LABEL_21 25396)
// ---------------------------------------------------------------------------
TEST_CASE("bombs F2: a kicked bomb slides speed + 100*kSubFrames units per tick") {
    Simulation s(open_config());
    Bomb b;
    b.active = true;
    b.owner = 0;
    b.x = 2 * kTileWF + kTileWF / 2;  // (2,0) centre, clear row 0 ahead
    b.y = kTileHF / 2;
    b.fuse = 10000;  // long: observe the slide, not a timeout
    b.flame = 1;
    b.moving = true;
    b.dir = Direction::Right;
    s.state().bombs.push_back(b);

    const Fixed x0 = s.state().bombs[0].x;
    run(s, 1);
    // budget = kicked_bomb_speed(1000) + 100*kSubFrames(900) = 1900 units = 19 px,
    // less than a tile so it is spent in full this tick (the pre-F2 budget was
    // just 1000 = 10 px).
    const Fixed expected = s.state().tuning.kicked_bomb_speed + 100 * kSubFrames;
    CHECK(s.state().bombs[0].x - x0 == expected);
}

// ---------------------------------------------------------------------------
// bombs F3 — a conveyor-carried bomb freezes off-belt (sub_42331C case 0)
// ---------------------------------------------------------------------------
TEST_CASE("bombs F3: a belt bomb FREEZES when it leaves the belt, does not coast") {
    Simulation s(open_config());
    State& st = s.state();
    // A short east belt on (2,0) and (3,0); (4,0)+ is open floor.
    for (int x = 2; x <= 3; ++x) {
        st.actor_type[0][x] = ActorType::Conveyor;
        st.actor_dir[0][x] = 1;  // godir 1 = Right = east
    }
    // A resting bomb on the belt's first tile.
    Bomb b;
    b.active = true;
    b.owner = 0;
    b.x = 2 * kTileWF + kTileWF / 2;
    b.y = kTileHF / 2;
    b.fuse = 10000;
    b.flame = 1;
    b.moving = false;  // motion state 0: only the belt can push it
    st.bombs.push_back(b);

    run(s, 20);
    REQUIRE_FALSE(st.bombs.empty());
    // The original re-checks the tile every tick and never enters kicked motion-
    // state-1, so the bomb stops the instant it clears the belt — it does NOT
    // coast at kicked speed across the whole field (old bug: tile_x near 14).
    CHECK_FALSE(st.bombs[0].moving);   // frozen, not sliding
    CHECK(st.bombs[0].tile_x() >= 3);  // it was carried past the belt start...
    CHECK(st.bombs[0].tile_x() <= 5);  // ...but froze right past the belt's end
}

// ---------------------------------------------------------------------------
// bombs F4 — trigger detonate can't fire on a same-tick-placed bomb (sub_424B41 26036)
// ---------------------------------------------------------------------------
TEST_CASE("bombs F4: a trigger bomb placed THIS tick is not remote-detonable yet") {
    Simulation s(open_config());  // 2 sides, so a real detonation is not frozen
    s.state().players[0].trigger = true;
    s.state().players[0].max_bombs = 5;
    // Same tick: action1 drops a fresh trigger bomb, action2 tries to detonate.
    TickInputs in;
    in.players[0].action1 = true;
    in.players[0].action2 = true;
    s.tick(in);
    // The drop happened; the detonate scan skipped it (created_tick == s.tick).
    REQUIRE(s.state().bombs.size() == 1);
    CHECK(s.state().bombs[0].trigger);
}

TEST_CASE("bombs F4: the SAME trigger bomb detonates one tick later") {
    Simulation s(open_config());
    s.state().players[0].trigger = true;
    TickInputs drop;
    drop.players[0].action1 = true;
    s.tick(drop);  // tick 0: drop the trigger bomb (created_tick 0)
    REQUIRE(s.state().bombs.size() == 1);
    TickInputs det;
    det.players[0].action2 = true;
    s.tick(det);  // tick 1: created_tick 0 < 1 -> detonates via the chain drain
    CHECK(s.state().bombs.empty());
}

// ---------------------------------------------------------------------------
// diseases F2 — clear() leaves disease_fresh alone (sub_41DF4C)
// ---------------------------------------------------------------------------
TEST_CASE("diseases F2: curing a disease leaves disease_fresh untouched") {
    Simulation s(open_config());
    Player& p = s.state().players[0];
    infect(p, Disease::Slow, /*timer=*/1);  // expires next tick
    p.disease_fresh = 20;                    // a stale freshness value
    run(s, 1);                               // the disease expires -> clear()
    CHECK_FALSE(p.sick(Disease::Slow));      // cured
    CHECK(p.disease_timer == 0);
    // sub_41DF4C zeroes only age/duration/flags, never +128 (freshness). The ager
    // decremented it once (20 -> 19) before the expiry cure left it alone.
    CHECK(p.disease_fresh > 0);
}

// ---------------------------------------------------------------------------
// setup/powerups F1 — seed ALL 13 start_with baselines (sub_4214BC 427-428)
// ---------------------------------------------------------------------------
TEST_CASE("setup F1: a nonzero VALUELST start_with for any kind seeds a fresh player") {
    MatchConfig cfg = open_config();
    cfg.tuning.start_with[static_cast<int>(PowerupType::Kick)] = 1;   // id 52
    cfg.tuning.start_with[static_cast<int>(PowerupType::Skate)] = 2;  // id 54
    cfg.tuning.start_with[static_cast<int>(PowerupType::Jelly)] = 1;  // id 60
    Simulation s(cfg);
    const Player& p = s.state().players[0];
    CHECK(p.kick);              // flag kind: baseline > 0 -> set
    CHECK(p.jelly);
    CHECK(p.skates == 2);       // counted kind: takes the value
    // skates fold into the walk speed exactly like a pickup.
    CHECK(p.speed == cfg.tuning.start_speed + 2 * cfg.tuning.skate_speed_bonus);
}

// ---------------------------------------------------------------------------
// ai F1 — behaviour-4 Manhattan gate is distance-from-spawn (sub_40ABED 0x40AC24)
// ---------------------------------------------------------------------------
TEST_CASE("ai F1: setup seeds every player's +20/+24 snapshot to its spawn tile") {
    Simulation s(open_config());  // spawns (0,0) and (14,10)
    CHECK(s.state().players[0].warp_to_x == 0);
    CHECK(s.state().players[0].warp_to_y == 0);
    CHECK(s.state().players[1].warp_to_x == 14);
    CHECK(s.state().players[1].warp_to_y == 10);
}

TEST_CASE("ai F1: a freshly-spawned AI does NOT bomb an adjacent enemy (gate closed)") {
    Simulation s(open_config());
    State& st = s.state();
    // Put the AI at (6,5) with the enemy directly below at (6,6) — inside
    // behaviour 4's 5-tile cross. Snapshot == current tile, so the distance-
    // travelled gate is 0 < 3 and behaviour 4 (bomb-near-enemy) never fires,
    // regardless of its rand()%5. Empty board -> no bricks -> no other drop path.
    Player& ai = st.players[0];
    ai.ai = true;
    ai.x = 6 * kTileWF + kTileWF / 2;
    ai.y = 5 * kTileHF + kTileHF / 2;
    ai.warp_to_x = 6;  // the +20/+24 snapshot == the AI's own tile (just spawned)
    ai.warp_to_y = 5;
    Player& foe = st.players[1];
    foe.x = 6 * kTileWF + kTileWF / 2;
    foe.y = 6 * kTileHF + kTileHF / 2;

    run(s, 1);
    CHECK(st.bombs.empty());  // the AI did not drop next to the enemy at spawn
}
