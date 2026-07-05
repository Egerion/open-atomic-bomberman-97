// Focused checks for the trampoline stage actor (type 3). Faithful to the
// step-on branch in sub_41EC84 and the FLIGHT in sub_41F29B state 5 (guarded by
// sub_41DE63): landing on a trampoline centre launches a hop that ignores input
// and cannot be pushed, and at the APEX teleports the player to a random nearby
// open tile ("fly + random land"). See docs/re/stage-actors.md §4.

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

// Ticks a launched hop to just BEFORE the apex relocation. The hop launches on
// the settle tick (bounce set to 30, tick_bounce not yet run); tick_bounce then
// counts 30->0, and the apex fires the tick bounce hits len/2 (=15). So the last
// pre-apex tick leaves bounce == 16.
constexpr int kBounceLen = 30;
constexpr int kApexBounce = kBounceLen / 2;  // 15

// No grounded bomb sits on (tx,ty) — the test-side mirror of sub_422E48 /
// grid::bomb_at (which lives in a private sim header the tests can't include).
bool no_bomb_on(const State& s, int tx, int ty) {
    for (const auto& b : s.bombs)
        if (b.active && !b.flying && b.tile_x() == tx && b.tile_y() == ty) return false;
    return true;
}

}  // namespace

TEST_CASE("stepping onto a trampoline centre launches a bounce") {
    Simulation s(open_config());
    State& st = s.state();
    st.actor_type[0][0] = ActorType::Trampoline;  // (0,0)
    Player& p = st.players[0];
    p.x = kTileWF / 2;  // already centred on the trampoline
    p.y = kTileHF / 2;

    s.tick(TickInputs{});  // settle on the tile

    // The hop lasts VALUELST id 680 = 30 frames (the bounce-state counter in
    // sub_41F29B ~23160), a confirmed value. See §4.
    CHECK(p.bounce == st.tuning.trampoline_bounce_frames);
    CHECK(st.tuning.trampoline_bounce_frames == kBounceLen);
    CHECK(saw_bounce(s, 0));
}

TEST_CASE("a bouncing player ignores movement input before the apex") {
    Simulation s(open_config());
    State& st = s.state();
    st.actor_type[0][0] = ActorType::Trampoline;
    Player& p = st.players[0];
    p.x = kTileWF / 2;
    p.y = kTileHF / 2;

    s.tick(TickInputs{});  // launch
    REQUIRE(p.bounce == kBounceLen);
    const int bx = p.x, by = p.y;

    TickInputs east;
    east.players[0].right = true;
    run(s, 3, east);  // try to walk away early in the hop (well before the apex)

    CHECK(p.bounce == kBounceLen - 3);  // counting down, input ignored
    CHECK(p.x == bx);                   // pinned in place before the apex
    CHECK(p.y == by);
    CHECK(p.tile_x() == 0);
}

TEST_CASE("at the apex the player flies to a random nearby open tile") {
    Simulation s(open_config());
    State& st = s.state();
    // Place the trampoline on a guaranteed-blank interior tile (even/even, so not
    // on open_config's (odd,odd) pillar lattice) with open room on every side.
    const int tx = 4, ty = 4;
    REQUIRE(st.cells[ty][tx] == Cell::Blank);
    st.actor_type[ty][tx] = ActorType::Trampoline;
    Player& p = st.players[0];
    p.x = kTileWF * tx + kTileWF / 2;
    p.y = kTileHF * ty + kTileHF / 2;

    s.tick(TickInputs{});  // launch
    REQUIRE(p.bounce == kBounceLen);

    // Tick to exactly the apex: bounce counts 30 -> 15; the relocation fires the
    // tick bounce reaches 15.
    while (p.bounce > kApexBounce) s.tick(TickInputs{});
    CHECK(p.bounce == kApexBounce);

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
        p.x = kTileWF * tx + kTileWF / 2;
        p.y = kTileHF * ty + kTileHF / 2;
        s.tick(TickInputs{});
        while (p.bounce > kApexBounce) s.tick(TickInputs{});
        return std::pair<int, int>{p.tile_x(), p.tile_y()};
    };
    // Same seed -> identical landing (replay determinism); this also proves the
    // relocation runs on State::rng, not a cosmetic stream.
    CHECK(land(7) == land(7));
}

TEST_CASE("the relocation draws the sim RNG (and only at a real hop)") {
    // With a trampoline, one hop consumes RNG at the apex; the rng word must
    // change across the apex tick. Without a trampoline the same run leaves the
    // RNG stream untouched by the (never-entered) bounce path — the golden no-op.
    auto rng_after_apex = [](bool with_tramp) {
        Simulation s(open_config());
        State& st = s.state();
        const int tx = 4, ty = 4;
        if (with_tramp) st.actor_type[ty][tx] = ActorType::Trampoline;
        Player& p = st.players[0];
        p.x = kTileWF * tx + kTileWF / 2;
        p.y = kTileHF * ty + kTileHF / 2;
        // Run a fixed number of ticks spanning where the apex would be.
        run(s, 1 + (kBounceLen - kApexBounce), TickInputs{});
        return st.rng;
    };
    const std::uint32_t with = rng_after_apex(true);
    const std::uint32_t without = rng_after_apex(false);
    // The trampoline run drew extra RNG at the apex, so its stream diverges.
    CHECK(with != without);
}

TEST_CASE("a boxed-in trampoline keeps the player put but still ends the hop") {
    // If NO nearby tile qualifies (all four cardinals + diagonals blocked), the
    // 100-attempt loop finds nothing and the player stays on the trampoline —
    // the hop still runs its full duration. Wall the trampoline in on a 3x3 box.
    Simulation s(open_config());
    State& st = s.state();
    const int tx = 4, ty = 4;
    for (int dy = -2; dy <= 2; ++dy)
        for (int dx = -2; dx <= 2; ++dx)
            if (dx || dy) st.cells[ty + dy][tx + dx] = Cell::Solid;  // box within reach
    st.actor_type[ty][tx] = ActorType::Trampoline;
    Player& p = st.players[0];
    p.x = kTileWF * tx + kTileWF / 2;
    p.y = kTileHF * ty + kTileHF / 2;
    const int sx = p.x, sy = p.y;

    s.tick(TickInputs{});                        // launch
    run(s, kBounceLen, TickInputs{});            // tick the whole hop

    CHECK(p.bounce == 0);       // hop ended on schedule
    CHECK(p.x == sx);           // no open landing tile -> stayed put
    CHECK(p.y == sy);
}

TEST_CASE("a player parked on a trampoline bounces once, not forever") {
    // The latch (Player::tramp_latch) stops a stationary centred player from
    // re-launching every tick. With the boxed-in trampoline the player never
    // leaves the tile, so exactly one hop fires and the latch stays set.
    Simulation s(open_config());
    State& st = s.state();
    const int tx = 4, ty = 4;
    for (int dy = -2; dy <= 2; ++dy)
        for (int dx = -2; dx <= 2; ++dx)
            if (dx || dy) st.cells[ty + dy][tx + dx] = Cell::Solid;
    st.actor_type[ty][tx] = ActorType::Trampoline;
    Player& p = st.players[0];
    p.x = kTileWF * tx + kTileWF / 2;
    p.y = kTileHF * ty + kTileHF / 2;

    int bounces = 0;
    for (int t = 0; t < kBounceLen * 3; ++t) {
        s.tick(TickInputs{});
        if (saw_bounce(s, 0)) ++bounces;
    }
    CHECK(bounces == 1);            // exactly one hop, then it rests (latched)
    CHECK(p.bounce == 0);
    CHECK(p.tramp_latch);          // still latched (never left the tile)
}

TEST_CASE("the bounce ends after its duration and movement resumes") {
    // Boxed so the player stays on the trampoline, then knock a hole in the wall
    // to prove walking resumes once the hop is over.
    Simulation s(open_config());
    State& st = s.state();
    const int tx = 4, ty = 4;
    for (int dy = -2; dy <= 2; ++dy)
        for (int dx = -2; dx <= 2; ++dx)
            if (dx || dy) st.cells[ty + dy][tx + dx] = Cell::Solid;
    st.actor_type[ty][tx] = ActorType::Trampoline;
    Player& p = st.players[0];
    p.x = kTileWF * tx + kTileWF / 2;
    p.y = kTileHF * ty + kTileHF / 2;

    s.tick(TickInputs{});                          // launch (bounce = 30)
    run(s, kBounceLen, TickInputs{});              // tick the hop to 0
    CHECK(p.bounce == 0);

    st.cells[ty][tx + 1] = Cell::Blank;            // open the wall to the east
    const int x0 = p.x;
    TickInputs east;
    east.players[0].right = true;
    s.tick(east);
    CHECK(p.x > x0);                               // moved: no longer state-gated
}

TEST_CASE("a trampoline does not fire until the player is centred") {
    Simulation s(open_config());
    State& st = s.state();
    st.actor_type[0][0] = ActorType::Trampoline;
    Player& p = st.players[0];
    // Off-centre on the trampoline tile: gliding across, not settled.
    p.x = kTileWF / 2 - 700;  // 7px left of centre, still tile (0,0)
    p.y = kTileHF / 2;

    s.tick(TickInputs{});

    CHECK(p.bounce == 0);       // not launched yet
    CHECK_FALSE(saw_bounce(s, 0));
}
