// Conveyor + trampoline player mechanics. Faithful port of the actor branches
// in sub_41F29B (conveyor budget) and sub_41EC84 (step-on trampoline). See
// docs/re/stage-actors.md §3 (conveyor) and §4 (trampoline).

#include "systems/stage_actors.hpp"

#include "grid.hpp"

namespace bomber::sim {

namespace {

// The actor under a tile, or None if empty/out of grid. Mirrors sub_405654.
ActorType actor_at(const State& s, int tx, int ty) {
    if (!grid::in_grid(tx, ty)) return ActorType::None;
    return s.actor_type[ty][tx];
}

}  // namespace

bool StageActorSystem::move_on_actor(Player& p, int want_godir, bool moving) {
    const Fixed bx = p.x, by = p.y;
    const int tx = p.tile_x(), ty = p.tile_y();
    const ActorType act = actor_at(s_, tx, ty);
    const bool conveyor = (act == ActorType::Conveyor);
    const int belt_dir = conveyor ? s_.actor_dir[ty][tx] : -1;
    const std::int32_t belt = s_.tuning.conveyor_speed();

    if (moving) {
        // Case (b) — the player has an input direction. The belt only nudges
        // the budget; it never overrides the chosen direction.
        std::int32_t extra = 0;
        if (conveyor) {
            if (want_godir == belt_dir)
                extra += belt;  // moving WITH the belt: speed bonus
            else if (want_godir == ((belt_dir + 2) & 3))
                extra -= belt;  // moving AGAINST the belt: speed penalty
            // perpendicular: no change
        }
        movement_.move(p, grid::from_godir(want_godir), extra);
    } else if (conveyor) {
        // Case (a) — no input. The belt FORCES its direction and pushes. If the
        // step is fully blocked (wall ahead) the original reverts godir to -1
        // (no facing change, no visible push); our stepper simply produces no
        // shift, and we leave facing untouched to match that revert.
        const Fixed fx = p.x, fy = p.y;
        const Direction saved_facing = p.facing;
        movement_.move(p, grid::from_godir(belt_dir), belt);
        if (p.x == fx && p.y == fy)
            p.facing = saved_facing;  // blocked: revert the forced facing
    }
    return p.x != bx || p.y != by;
}

bool StageActorSystem::trampoline_after_move(Player& p, int player_index) {
    if (p.bounce > 0) return false;  // already bouncing
    const int tx = p.tile_x(), ty = p.tile_y();
    if (actor_at(s_, tx, ty) != ActorType::Trampoline) return false;

    // The original triggers the hop only when the stepper has CENTRED the
    // player on the tile (v35 == -1). Reproduce that with a tile-centre test so
    // a player merely gliding across the edge of a trampoline is not launched
    // until it settles on the middle.
    if (p.x != grid::tile_center_x(tx) || p.y != grid::tile_center_y(ty)) return false;

    p.bounce = s_.tuning.trampoline_bounce_frames;
    s_.events.push_back({Event::Type::TrampolineBounce,
                         static_cast<std::int8_t>(player_index),
                         static_cast<std::int8_t>(tx), static_cast<std::int8_t>(ty), 0});
    return true;
}

}  // namespace bomber::sim
