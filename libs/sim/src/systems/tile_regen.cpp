#include "systems/tile_regen.hpp"

#include <algorithm>
#include <cstdlib>  // std::abs in manhattan(); do not rely on a transitive include

#include "bomber/sim/rng.hpp"
#include "grid.hpp"

namespace bomber::sim {
namespace {

int manhattan(int ax, int ay, int bx, int by) {
    return std::abs(ax - bx) + std::abs(ay - by);
}

// sub_422351(radius): are all live players MORE than `radius` tiles away from
// (cx,cy)? A player at exactly `radius` FAILS the test — the comparison is
// `<= radius`, matching id 695's own comment "clear cell radius... nobody can be
// within this radius".
//
// Two caveats, both documented in facts.md "Per-level tile regeneration". The
// candidate tile reaches sub_422351 through a register the Watcom-convention
// caller left live from the preceding occupancy checks, so only the radius is
// visible as an explicit argument in the text decompile — the formula is high
// confidence, the register mechanism is inferred. And the present+alive gate is
// OURS: the original's raw 10-slot scan has no visible active check, but
// iterating unconditionally would let unused slots, which default to tile (0,0),
// permanently block regen near the board's top-left corner in any match with
// fewer than 10 players.
bool clear_of_players(const State& s, int cx, int cy, int radius) {
    for (const auto& p : s.players) {
        if (!p.present || !p.alive) continue;
        if (manhattan(p.tile_x(), p.tile_y(), cx, cy) <= radius) return false;
    }
    return true;
}

}  // namespace

void TileRegenSystem::update() {
    State& s = s_;
    const int level = std::clamp(s.tuning.level_index, 0, 10);
    const int regen_seconds = s.tuning.regen_seconds[level];
    if (regen_seconds <= 0) return;  // inert on every level but Haunted House

    const int interval_ticks = regen_seconds * kTicksPerSecond;
    if (s.regen_timer > 0) {
        --s.regen_timer;
        return;
    }
    s.regen_timer = interval_ticks;

    // ONE attempt cycle: up to 100 random candidate tiles, stop at the first
    // that is blank, unoccupied (no bomb, no floor powerup), and clear of
    // every player within the id-695 radius (sub_426704 pseudo.c ~27107).
    // Exactly 2 RNG draws per attempt (x then y), regardless of whether the
    // candidate passes any check — the draw COUNT per attempt is fixed, only
    // the number of attempts (i.e. whether we break early) varies.
    for (int attempt = 0; attempt < 100; ++attempt) {
        const int x = static_cast<int>(random_below(s, static_cast<std::uint32_t>(kGridWidth)));
        const int y = static_cast<int>(random_below(s, static_cast<std::uint32_t>(kGridHeight)));
        if (s.cells[y][x] != Cell::Blank) continue;
        if (grid::bomb_at(s, x, y) != nullptr) continue;
        if (s.floor[y][x] != PowerupType::None) continue;
        if (!clear_of_players(s, x, y, s.tuning.regen_clear_radius)) continue;

        s.cells[y][x] = Cell::Brick;
        s.events.push_back({Event::Type::TileRegrew, -1, static_cast<std::int8_t>(x),
                            static_cast<std::int8_t>(y), 0});
        return;
    }
    // All 100 attempts failed (no eligible tile this cycle) — the timer was
    // already reset above, exactly like the original, which rearms its deadline
    // global dword_464978 unconditionally once the interval has elapsed, win or
    // lose.
}

}  // namespace bomber::sim
