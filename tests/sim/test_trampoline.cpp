// Focused checks for the trampoline stage actor (type 3). Faithful to the
// step-on branch in sub_41EC84 and the FLIGHT in sub_41F29B state 5 (guarded by
// sub_41DE63): WALKING INTO a trampoline centre launches a hop that ignores
// input and cannot be pushed, and at the APEX teleports the player to a random
// nearby open tile ("fly + random land"). See docs/re/stage-actors.md §4 and
// facts.md "Warphole/trampoline entry predicate".
//
// Note every launch here is a WALK-IN. The original's only trigger is the
// mover's along-axis offset-to-tile-centre reading -1, i.e. the approach to the
// centre from outside it — a player merely standing on the centre is never
// taken. The old suite parked players on the tile and relied on a post-tick
// fallback this port no longer has; `a player left standing on a trampoline is
// NOT launched` below is now the pinned behaviour.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <cstdlib>
#include <utility>

#include "helpers.hpp"

using namespace bomber::sim;
using namespace bomber::sim::test;

namespace {

bool saw_bounce(const Simulation& s, int player) {
    for (const auto& e : s.state().events)
        if (e.type == Event::Type::TrampolineBounce && e.player == player) return true;
    return false;
}

// The hop lasts VALUELST id 680 = 30 counter steps, but the apex block bumps the
// counter an extra step (0x4204af), so it burns 29 ticks of wall clock. The
// trigger tick is the first of them (sub_41F29B runs the mover before the state
// dispatch), so from the launch tick the countdown reads 29 and the flight ends
// 28 ticks later. The apex fires the tick the countdown would reach 15 — and the
// extra bump leaves it at 14, so 15 is a value it never shows.
constexpr int kBounceLen = 30;
constexpr int kApexBounce = kBounceLen / 2;      // 15
constexpr int kAfterLaunch = kBounceLen - 1;     // 29: countdown after the launch tick
constexpr int kTicksAfterLaunch = kAfterLaunch;  // ...and how many ticks are left

TickInputs east_input() {
    TickInputs in;
    in.players[0].right = true;
    return in;
}

// Park player 0 dead-centre on (tx,ty) — the case that must NOT trigger.
void park_on(State& st, int tx, int ty) {
    st.players[0].x = tx * kTileWF + kTileWF / 2;
    st.players[0].y = ty * kTileHF + kTileHF / 2;
}

// Walk player 0 east onto (tx,ty) from the centre of the tile to its west, and
// stop on the tick the hop launches. Returns true if it launched.
bool walk_east_into(Simulation& s, int tx, int ty) {
    park_on(s.state(), tx - 1, ty);
    const TickInputs east = east_input();
    for (int t = 0; t < 20; ++t) {
        s.tick(east);
        if (s.state().players[0].bounce > 0) return true;
    }
    return false;
}

// No grounded bomb sits on (tx,ty) — the test-side mirror of sub_422E48 /
// grid::bomb_at (which lives in a private sim header the tests can't include).
bool no_bomb_on(const State& s, int tx, int ty) {
    for (const auto& b : s.bombs)
        if (b.active && !b.flying && b.tile_x() == tx && b.tile_y() == ty) return false;
    return true;
}

// Wall every tile within reach of the apex loop EXCEPT the westward approach
// lane, so a player can still walk in but no candidate landing qualifies. The
// loop demands a tile that differs from the origin on BOTH axes, so anything on
// the trampoline's own row (dy == 0) can never be picked — leaving the lane open
// costs the test nothing.
void box_in_except_west_lane(State& st, int tx, int ty) {
    for (int dy = -2; dy <= 2; ++dy)
        for (int dx = -2; dx <= 2; ++dx) {
            if (!dx && !dy) continue;
            if (dy == 0 && dx < 0) continue;  // the walk-in lane
            st.cells[ty + dy][tx + dx] = Cell::Solid;
        }
}

}  // namespace

TEST_CASE("walking into a trampoline centre launches a bounce") {
    Simulation s(open_config());
    State& st = s.state();
    const int tx = 4, ty = 4;
    st.actor_type[ty][tx] = ActorType::Trampoline;
    Player& p = st.players[0];

    REQUIRE(walk_east_into(s, tx, ty));

    CHECK(saw_bounce(s, 0));
    CHECK(st.tuning.trampoline_bounce_frames == kBounceLen);
    // The launch tick is already the flight's first frame (sub_41F29B's mover
    // runs before the state dispatch), so the countdown reads 29, not 30.
    CHECK(p.bounce == kAfterLaunch);
}

TEST_CASE("a player left standing on a trampoline is NOT launched") {
    // THE negative case. sub_41EC84 only fires when the along-axis offset reads
    // -1 — the approach to the centre. A player sitting ON the centre reads 0,
    // and one pressing into a wall from beyond it reads > 0; neither is -1, so
    // the original never launches them. This port used to, via a post-tick
    // "standing on one" fallback, which is why warpholes/trampolines felt
    // grabbier here than in the original.
    Simulation s(open_config());
    State& st = s.state();
    const int tx = 4, ty = 4;
    st.actor_type[ty][tx] = ActorType::Trampoline;
    park_on(st, tx, ty);

    run(s, 10, TickInputs{});  // no input at all
    CHECK(st.players[0].bounce == 0);
    CHECK_FALSE(saw_bounce(s, 0));

    // ...and pressing INTO a wall while parked on the centre does not either:
    // the blocked settle-back pins the player at offset 0, never -1.
    st.cells[ty][tx + 1] = Cell::Solid;
    run(s, 10, east_input());
    CHECK(st.players[0].bounce == 0);
    CHECK_FALSE(saw_bounce(s, 0));
}

TEST_CASE("a player crossing the NEIGHBOURING row is not launched") {
    // The predicate consults only the travel axis, but the actor is looked up at
    // the player's own tile — so walking the row above a trampoline is safe no
    // matter how many tile centres are crossed.
    Simulation s(open_config());
    State& st = s.state();
    const int tx = 4, ty = 4;
    st.actor_type[ty][tx] = ActorType::Trampoline;
    // Row ty-1 is odd, so open_config's (odd,odd) pillars sit in it; clear them
    // so the lane the player walks is genuinely open.
    for (int x = 0; x < kGridWidth; ++x) st.cells[ty - 1][x] = Cell::Blank;
    park_on(st, 0, ty - 1);

    run(s, 40, east_input());
    CHECK(st.players[0].tile_x() > tx);  // walked clean past the column
    CHECK(st.players[0].bounce == 0);
    CHECK_FALSE(saw_bounce(s, 0));
}

TEST_CASE("a bouncing player ignores movement input before the apex") {
    Simulation s(open_config());
    State& st = s.state();
    const int tx = 4, ty = 4;
    st.actor_type[ty][tx] = ActorType::Trampoline;
    Player& p = st.players[0];

    REQUIRE(walk_east_into(s, tx, ty));
    const int bx = p.x, by = p.y;

    run(s, 3, east_input());  // try to walk away early in the hop

    CHECK(p.bounce == kAfterLaunch - 3);  // counting down, input ignored
    CHECK(p.x == bx);                     // pinned in place before the apex
    CHECK(p.y == by);
    CHECK(p.tile_x() == tx);
}

TEST_CASE("at the apex the player flies to a random nearby open tile") {
    Simulation s(open_config());
    State& st = s.state();
    // An even/even interior tile (not on open_config's (odd,odd) pillar lattice)
    // with open room on every side.
    const int tx = 4, ty = 4;
    REQUIRE(st.cells[ty][tx] == Cell::Blank);
    st.actor_type[ty][tx] = ActorType::Trampoline;
    Player& p = st.players[0];

    REQUIRE(walk_east_into(s, tx, ty));

    // Tick to the apex: the countdown runs 29 -> 15, and the tick it WOULD show
    // 15 the relocation fires and the original's extra bump takes it to 14.
    while (p.bounce > kApexBounce) s.tick(TickInputs{});
    CHECK(p.bounce == kApexBounce - 1);  // 15 is skipped, not shown

    const int nx = p.tile_x(), ny = p.tile_y();
    // The loop's guarantees (0x4203a7): the landing differs in BOTH axes, is
    // within [-2,+2] of the origin, is an open (Blank) tile, and holds no bomb.
    CHECK(nx != tx);
    CHECK(ny != ty);
    CHECK(std::abs(nx - tx) <= 2);
    CHECK(std::abs(ny - ty) <= 2);
    CHECK(st.cells[ny][nx] == Cell::Blank);
    CHECK(no_bomb_on(st, nx, ny));
}

TEST_CASE("the apex relocation is deterministic for a fixed seed") {
    auto land = [](std::uint32_t seed) {
        MatchConfig cfg = open_config();
        cfg.seed = seed;
        Simulation s(cfg);
        State& st = s.state();
        const int tx = 4, ty = 4;
        st.actor_type[ty][tx] = ActorType::Trampoline;
        Player& p = st.players[0];
        REQUIRE(walk_east_into(s, tx, ty));
        while (p.bounce > kApexBounce) s.tick(TickInputs{});
        return std::pair<int, int>{p.tile_x(), p.tile_y()};
    };
    // Same seed -> identical landing (replay determinism); this also proves the
    // relocation runs on State::rng, not a cosmetic stream.
    CHECK(land(7) == land(7));
}

TEST_CASE("the relocation draws the sim RNG (and only at a real hop)") {
    // With a trampoline, one hop consumes RNG at the apex; without one the same
    // walk leaves the RNG stream untouched by the (never-entered) bounce path —
    // the golden no-op.
    auto rng_after_apex = [](bool with_tramp) {
        Simulation s(open_config());
        State& st = s.state();
        const int tx = 4, ty = 4;
        if (with_tramp) st.actor_type[ty][tx] = ActorType::Trampoline;
        park_on(st, tx - 1, ty);
        // A fixed run, long enough to span the walk-in plus the whole apex half.
        run(s, 24, east_input());
        return st.rng;
    };
    CHECK(rng_after_apex(true) != rng_after_apex(false));
}

TEST_CASE("a boxed-in trampoline keeps the player put but still ends the hop") {
    // If NO nearby tile qualifies, the 100-attempt loop finds nothing and the
    // player stays on the trampoline — the hop still runs its full duration.
    Simulation s(open_config());
    State& st = s.state();
    const int tx = 4, ty = 4;
    st.actor_type[ty][tx] = ActorType::Trampoline;
    box_in_except_west_lane(st, tx, ty);
    Player& p = st.players[0];

    REQUIRE(walk_east_into(s, tx, ty));
    const int sx = p.x, sy = p.y;

    run(s, kTicksAfterLaunch, TickInputs{});

    CHECK(p.bounce == 0);  // hop ended on schedule
    CHECK(p.x == sx);      // no open landing tile -> stayed put
    CHECK(p.y == sy);
}

TEST_CASE("a player left on the trampoline by a hop does not re-launch") {
    // The boxed-in hop drops the player back on the trampoline centre. There is
    // no latch any more, and none is needed: the centre reads offset 0, and only
    // -1 triggers, so the player rests there until it walks in again.
    Simulation s(open_config());
    State& st = s.state();
    const int tx = 4, ty = 4;
    st.actor_type[ty][tx] = ActorType::Trampoline;
    box_in_except_west_lane(st, tx, ty);
    Player& p = st.players[0];

    REQUIRE(walk_east_into(s, tx, ty));
    int bounces = 1;
    for (int t = 0; t < kBounceLen * 3; ++t) {
        s.tick(TickInputs{});
        if (saw_bounce(s, 0)) ++bounces;
    }
    CHECK(bounces == 1);  // exactly one hop, then it rests
    CHECK(p.bounce == 0);
    CHECK(p.tile_x() == tx);
    CHECK(p.tile_y() == ty);
}

TEST_CASE("the bounce ends after its duration and movement resumes") {
    Simulation s(open_config());
    State& st = s.state();
    const int tx = 4, ty = 4;
    st.actor_type[ty][tx] = ActorType::Trampoline;
    box_in_except_west_lane(st, tx, ty);
    Player& p = st.players[0];

    REQUIRE(walk_east_into(s, tx, ty));
    run(s, kTicksAfterLaunch, TickInputs{});
    CHECK(p.bounce == 0);

    st.cells[ty][tx + 1] = Cell::Blank;  // open the wall to the east
    const int x0 = p.x;
    s.tick(east_input());
    CHECK(p.x > x0);  // moved: no longer state-gated
}

TEST_CASE("a trampoline does not fire until the player reaches the centre") {
    Simulation s(open_config());
    State& st = s.state();
    st.actor_type[0][0] = ActorType::Trampoline;
    Player& p = st.players[0];
    // Off-centre on the trampoline tile: gliding across, not yet at the centre.
    p.x = kTileWF / 2 - 700;  // 7px left of centre, still tile (0,0)
    p.y = kTileHF / 2;

    s.tick(TickInputs{});  // no input -> the mover never runs

    CHECK(p.bounce == 0);  // not launched yet
    CHECK_FALSE(saw_bounce(s, 0));
}
