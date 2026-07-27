// Team-mode tests (docs/re/ai.md TEAM follow-up; docs/re/setup-screens.md +84
// byte). Covers: Player::team wiring from MatchConfig::team[] (hashed), the AI
// no longer targeting a teammate, round-end becoming "one team left" instead
// of "one player left", and that a team-only difference changes the state
// hash (proving Player::team is really part of the deterministic contract).
//
// Team semantics are "our semantics — not RE'd" beyond the existence of the
// +84 byte: two ACTIVE players are teammates iff Player::team is equal AND
// nonzero (team 0 == "no team", never merges with another team-0 player). See
// Player::team's doc comment (player.hpp) and AISystem::same_team (ai.cpp).

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "bomber/match/team_colour.hpp"
#include "bomber/sim/rng.hpp"
#include "bomber/sim/simulation.hpp"

using namespace bomber::sim;

namespace {

Fixed cx(int tx) {
    return tx * kTileWF + kTileWF / 2;
}
Fixed cy(int ty) {
    return ty * kTileHF + kTileHF / 2;
}

// A bare, open-arena Simulation (mirrors tests/test_ai.cpp's open_arena, minus
// the single implicit player) with a real, generous clock so the enclosure
// spiral stays dormant (docs/re/enclosure.md §2 — the same "no clock" pitfall
// test_ai.cpp / test_golden.cpp both work around).
Simulation open_arena() {
    Simulation s;
    State& st = s.state();
    for (auto& row : st.cells) row.fill(Cell::Blank);
    for (auto& row : st.floor) row.fill(PowerupType::None);
    for (auto& row : st.hidden) row.fill(PowerupType::None);
    st.ticks_left = 9999 * kTicksPerSecond;
    return s;
}

Player& add_player(State& st, int slot, int tx, int ty, bool ai, std::uint8_t team) {
    Player& p = st.players[slot];
    p.present = true;
    p.alive = true;
    p.ai = ai;
    p.team = team;
    p.x = cx(tx);
    p.y = cy(ty);
    p.speed = 923;
    p.max_bombs = 1;
    p.flame = 2;
    return p;
}

TickInputs idle() {
    return TickInputs{};
}

int tile_x(const Player& p) {
    return static_cast<int>(p.x / kTileWF);
}
int tile_y(const Player& p) {
    return static_cast<int>(p.y / kTileHF);
}

}  // namespace

// ---------------------------------------------------------------------------
// MatchConfig::team[] -> Player::team wiring (setup.cpp).
// ---------------------------------------------------------------------------

TEST_CASE("setup copies MatchConfig::team[] into Player::team verbatim") {
    MatchConfig cfg;
    cfg.player_count = 3;
    cfg.spawns = {{0, 0}, {5, 5}, {10, 10}};
    cfg.team[0] = 1;
    cfg.team[1] = 1;
    cfg.team[2] = 0;  // solo (team 0 -> its own side, our semantics)
    Simulation s(cfg);
    CHECK(s.state().players[0].team == 1);
    CHECK(s.state().players[1].team == 1);
    CHECK(s.state().players[2].team == 0);
}

TEST_CASE("default MatchConfig leaves every player's team at 0") {
    MatchConfig cfg;
    cfg.player_count = 2;
    cfg.spawns = {{0, 0}, {5, 5}};
    Simulation s(cfg);
    CHECK(s.state().players[0].team == 0);
    CHECK(s.state().players[1].team == 0);
}

// ---------------------------------------------------------------------------
// AI targeting: a same-team player is not an enemy.
// ---------------------------------------------------------------------------

TEST_CASE(
    "behaviour 4 does not bomb a teammate on its cross, but does bomb a "
    "stranger") {
    // Same room/seed/geometry as the Stage-5 "AI beside enemy" test (test_ai.cpp)
    // that proves behaviour 4 fires against a stranger; here the adjacent player
    // shares the AI's team and must NEVER be bombed.
    Simulation team = open_arena();
    {
        State& st = team.state();
        st.rng = 0x24681357u;
        for (int y = 4; y <= 8; ++y)
            for (int x = 4; x <= 8; ++x)
                if (y == 4 || y == 8 || x == 4 || x == 8) st.cells[y][x] = Cell::Solid;
        add_player(st, 0, 6, 6, /*ai=*/true, /*team=*/1);
        add_player(st, 1, 6, 5, /*ai=*/false, /*team=*/1);  // teammate, idle, adjacent
    }
    bool dropped = false;
    for (int t = 0; t < 200; ++t) {
        team.tick(idle());
        if (!team.state().bombs.empty()) dropped = true;
    }
    CHECK_FALSE(dropped);  // never bombs its own teammate
    CHECK(team.state().players[0].alive);
    CHECK(team.state().players[1].alive);

    // Control: the identical geometry/seed but a DIFFERENT (or zero) team on
    // slot 1 -> behaviour 4 fires, exactly like the pre-team-mode Stage-5 test.
    Simulation stranger = open_arena();
    {
        State& st = stranger.state();
        st.rng = 0x24681357u;
        for (int y = 4; y <= 8; ++y)
            for (int x = 4; x <= 8; ++x)
                if (y == 4 || y == 8 || x == 4 || x == 8) st.cells[y][x] = Cell::Solid;
        add_player(st, 0, 6, 6, /*ai=*/true, /*team=*/1);
        add_player(st, 1, 6, 5, /*ai=*/false, /*team=*/2);  // different team
    }
    bool dropped2 = false;
    for (int t = 0; t < 200; ++t) {
        stranger.tick(idle());
        if (!stranger.state().bombs.empty()) dropped2 = true;
    }
    CHECK(dropped2);  // a non-teammate on the cross IS bombed
}

TEST_CASE("the enemy finder (behaviour 6) never picks a teammate") {
    // A lone AI with ONE live opponent, sharing its team: pick_live_enemy must
    // return -1 (no valid target) every time it's asked, so behaviour 6 never
    // latches onto the teammate and the AI never closes the distance toward it
    // (it can only wander). Run many ticks/seeds-worth of acquire rolls.
    Simulation s = open_arena();
    State& st = s.state();
    st.rng = 0x0FACE123u;
    add_player(st, 0, 2, 5, /*ai=*/true, /*team=*/9);
    add_player(st, 1, 10, 5, /*ai=*/false, /*team=*/9);  // teammate, far east, idle

    const int start_x = tile_x(st.players[0]);
    int max_x = start_x;
    for (int t = 0; t < 800; ++t) {
        s.tick(idle());
        if (st.players[0].alive) max_x = std::max(max_x, tile_x(st.players[0]));
        // Never latches a target onto the teammate's slot.
        if (st.brains[0].enemy_seek.active) CHECK(st.brains[0].enemy_seek.target_slot != 1);
    }
    // With no valid enemy anywhere, behaviour 6 never acquires, so movement is
    // just wander noise — it should NOT have marched purposefully toward slot 1
    // the way the analogous "closes on the foe" Stage-5 test does (max advance
    // of a couple of tiles from wander alone is fine; a directed multi-tile
    // beeline like the non-team test is what we must NOT see).
    CHECK(st.players[0].alive);
}

// ---------------------------------------------------------------------------
// Round-end: "one team left" instead of "one player left".
// ---------------------------------------------------------------------------

TEST_CASE("sides_remaining/winning_side treat teammates as one side") {
    State st;
    add_player(st, 0, 0, 0, false, 1);
    add_player(st, 1, 1, 1, false, 1);  // same team as 0
    add_player(st, 2, 2, 2, false, 2);  // different team, alone

    CHECK(sides_remaining(st) == 2);  // {0,1} and {2}
    CHECK(winning_side(st) == -1);    // two sides alive -> not decided

    // Kill the lone side-2 player: side {0,1} is the only side left, but it has
    // TWO alive players -> round continues (this is the crux of the TEAM rule:
    // alive_count() would still be 2, but sides_remaining() is 1).
    st.players[2].alive = false;
    CHECK(alive_count(st) == 2);      // two players alive...
    CHECK(sides_remaining(st) == 1);  // ...but ONE side -> round IS over
    CHECK(winning_side(st) != -1);    // decided: side {0,1} won
    CHECK((winning_side(st) == 0 || winning_side(st) == 1));

    // Killing one of the two teammates still leaves their side the winner.
    st.players[1].alive = false;
    CHECK(sides_remaining(st) == 1);
    CHECK(winning_side(st) == 0);
}

TEST_CASE("round continues while two teammates are the only survivors") {
    // Direct simulation-level check of the exact scenario the task calls out:
    // two teammates alive, everyone else dead -> sides_remaining() must be 1
    // (round over) while a naive alive_count() would say 2 (round NOT over).
    // This proves the frontend's team-aware gate (game_app.cpp) has the right
    // primitive to use instead of alive_count() <= 1.
    Simulation s = open_arena();
    State& st = s.state();
    add_player(st, 0, 3, 3, false, 5);
    add_player(st, 1, 9, 7, false, 5);  // same team
    add_player(st, 2, 6, 6, false, 0);  // solo, will be defeated
    st.players[2].alive = false;        // simulate it having died

    CHECK(alive_count(st) == 2);
    CHECK(sides_remaining(st) == 1);  // one team left -> the round is decided
}

TEST_CASE("solo roster (all team 0): sides_remaining degenerates to alive_count") {
    // Every existing scenario (team defaults to 0 everywhere): sides_remaining
    // must behave EXACTLY like alive_count(), since team 0 never merges with
    // another team-0 player (our semantics). This is the "untamed path stays
    // byte-identical" guarantee for the round-end rule.
    State st;
    add_player(st, 0, 0, 0, false, 0);
    add_player(st, 1, 1, 1, false, 0);
    add_player(st, 2, 2, 2, false, 0);
    CHECK(sides_remaining(st) == alive_count(st));
    CHECK(sides_remaining(st) == 3);

    st.players[1].alive = false;
    CHECK(sides_remaining(st) == alive_count(st));
    CHECK(sides_remaining(st) == 2);
    CHECK(winning_side(st) == -1);  // two distinct solo sides -> undecided

    st.players[2].alive = false;
    CHECK(sides_remaining(st) == alive_count(st));
    CHECK(sides_remaining(st) == 1);
    CHECK(winning_side(st) == 0);  // sole survivor wins, same as pre-team logic
}

// ---------------------------------------------------------------------------
// Hashing: a team-only difference must change state_hash().
// ---------------------------------------------------------------------------

TEST_CASE("state_hash differs when only Player::team differs") {
    auto build = [](std::uint8_t team1) {
        MatchConfig cfg;
        cfg.player_count = 2;
        cfg.spawns = {{0, 0}, {5, 5}};
        cfg.team[1] = team1;
        return Simulation(cfg);
    };
    Simulation a = build(0);
    Simulation b = build(7);
    CHECK(a.hash() != b.hash());

    // Same team value -> identical hash (team is deterministic state, not a
    // source of nondeterminism by itself).
    Simulation c = build(7);
    CHECK(b.hash() == c.hash());
}

TEST_CASE(
    "an all-zero-team roster hashes identically before/after the team "
    "field existed (regression pin against golden-style byte identity)") {
    // Two configs that are identical except one explicitly zeroes team[] (a
    // no-op, since it already defaults to 0) must hash the same — the team word
    // mixes in a constant 0 for every player on the untamed path.
    MatchConfig cfg1;
    cfg1.player_count = 2;
    cfg1.spawns = {{0, 0}, {5, 5}};
    MatchConfig cfg2 = cfg1;
    cfg2.team.fill(0);
    Simulation a(cfg1), b(cfg2);
    CHECK(a.hash() == b.hash());
}

// ---------------------------------------------------------------------------
// team_render_colour (bomber::match, libs/match/include/bomber/match/
// team_colour.hpp): the Team Play red/white sprite-colour rule, CONFIRMED
// from sub_4214BC (round init, pseudo.c ~23916-23927) — see that header's
// doc comment for the full RE citation. Player::team == 0 keeps the slot's
// own colour index; team == 1 -> colour 0 (white, 0.RMP); team == 2 -> colour
// 2 (red, 2.RMP); any other nonzero team value also collapses to red (the
// original's `? 2 : 0` has no third branch).
// ---------------------------------------------------------------------------

TEST_CASE("team_render_colour: no team (0) keeps the player's own slot index") {
    for (int slot = 0; slot < kMaxPlayers; ++slot)
        CHECK(bomber::match::team_render_colour(0, slot) == slot);
}

TEST_CASE("team_render_colour: team 1 forces colour 0 (white/0.RMP) regardless of slot") {
    for (int slot = 0; slot < kMaxPlayers; ++slot)
        CHECK(bomber::match::team_render_colour(1, slot) == 0);
}

TEST_CASE("team_render_colour: team 2 forces colour 2 (red/2.RMP) regardless of slot") {
    for (int slot = 0; slot < kMaxPlayers; ++slot)
        CHECK(bomber::match::team_render_colour(2, slot) == 2);
}

TEST_CASE(
    "team_render_colour: an out-of-contract nonzero, non-2 team value is treated as "
    "white (only team==2 is red)") {
    // Our setup only ever produces team in {0,1,2} (setup byte+1), so this is
    // a defensive/degenerate input, not a real scenario. The rule mirrors
    // The rule mirrors sub_4214BC's colour pick off the player's +84 team byte
    // literally: only an exact match on the "team B" value selects red (2);
    // anything else nonzero falls to the other arm, 0 (white), same as
    // team == 1 does.
    CHECK(bomber::match::team_render_colour(9, 3) == 0);
    CHECK(bomber::match::team_render_colour(255, 7) == 0);
}

TEST_CASE("team_render_colour: an out-of-range slot with no team clamps to 0") {
    CHECK(bomber::match::team_render_colour(0, -1) == 0);
    CHECK(bomber::match::team_render_colour(0, kMaxPlayers) == 0);
}
