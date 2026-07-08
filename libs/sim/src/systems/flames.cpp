#include "systems/flames.hpp"

#include <algorithm>

#include "grid.hpp"
#include "systems/powerups.hpp"

namespace bomber::sim {

void FlameSystem::burn_powerup_here(int tx, int ty) {
    State& s = s_;
    if (s.floor[ty][tx] == PowerupType::None) return;
    const PowerupType burned = s.floor[ty][tx];
    s.events.push_back({Event::Type::PowerupBurned, -1, static_cast<std::int8_t>(tx),
                        static_cast<std::int8_t>(ty), static_cast<std::int8_t>(burned)});
    s.floor[ty][tx] = PowerupType::None;
    // "Diseases Can Be Destroyed" OFF (dword_464990=0, options.ini
    // diseases_destroyable= / VALUELST 120): a burned skull is not lost —
    // a fresh one relocates to a random free tile. The flame walk's
    // powerup branch (sub_42331C ~25626/25653) runs `if (kind == 2 &&
    // !dword_464990) sub_4255B2(2)` right after the destruction;
    // scatter() IS our sub_4255B2, so order and count of the RNG draws
    // mirror the original. Destroying the token itself is unconditional.
    if (burned == PowerupType::Disease && !s.tuning.diseases_destroyable)
        powerups_.scatter(PowerupType::Disease);
}

bool FlameSystem::ignite_epicentre(int tx, int ty, std::uint8_t owner) {
    // The bomb's own tile (sub_42331C epicentre block, pseudo.c 25619-25636):
    // ALWAYS ignited (it is inherently blank — a bomb cannot rest on
    // solid/brick), THEN any powerup there is destroyed. No stop/occupancy
    // test here; that only applies to the extending arm below. No bomb check
    // either: placement itself is gated on sub_422E48 (no two bombs ever
    // share a tile), so a second bomb can never be sitting on the epicentre.
    State& s = s_;
    if (!grid::in_grid(tx, ty)) return false;
    s.flame[ty][tx] = static_cast<std::uint8_t>(
        std::clamp<std::int32_t>(s.tuning.flame_frames, 1, 255));
    s.flame_owner[ty][tx] = owner;
    burn_powerup_here(tx, ty);
    return true;
}

bool FlameSystem::spread_to(int tx, int ty, std::uint8_t owner) {
    // The extending arm (sub_42331C per-direction loop, pseudo.c 25637-25678).
    // Per tile step, in order: a GROUNDED bomb here stops the arm and chain-
    // detonates it (sub_422E48 @ 25641) — the tile is NOT ignited by this
    // arm at all (the bomb's own explosion will flame it separately this same
    // tick via chain-reaction). A VISIBLE floor powerup here (sub_42542D @
    // 25653, state==2) stops the arm and is destroyed — also not ignited.
    // Only past both checks does the cell-type verdict run: solid stops with
    // no ignite; brick ignites (as "brick burning") and stops; blank ignites
    // and the arm continues. This is facts.md's flagged fidelity gap: our
    // previous port ignited every non-solid/non-brick tile unconditionally,
    // so a flame arm burned straight through bombs and powerups instead of
    // stopping at them (docs/re/facts.md "Options toggles" §"Known remaining
    // fidelity gaps").
    State& s = s_;
    if (!grid::in_grid(tx, ty)) return false;

    if (Bomb* hit = grid::bomb_at(s, tx, ty)) {
        explode(static_cast<std::size_t>(hit - s.bombs.data()));
        return false;  // arm stops at the bomb it chain-detonates
    }
    if (s.floor[ty][tx] != PowerupType::None) {
        burn_powerup_here(tx, ty);
        return false;  // arm stops at the powerup it burns
    }

    Cell& c = s.cells[ty][tx];
    if (c == Cell::Solid) return false;
    if (c == Cell::Brick) {
        c = Cell::Blank;
        s.burning[ty][tx] = static_cast<std::uint8_t>(
            std::clamp<std::int32_t>(s.tuning.brick_burn_frames, 1, 255));
        s.events.push_back({Event::Type::BrickDestroyed, -1, static_cast<std::int8_t>(tx),
                            static_cast<std::int8_t>(ty), 0});
        return false;  // flame stops at the brick it destroys
    }
    if (s.burning[ty][tx] > 0) return false;

    s.flame[ty][tx] = static_cast<std::uint8_t>(
        std::clamp<std::int32_t>(s.tuning.flame_frames, 1, 255));
    s.flame_owner[ty][tx] = owner;
    return true;
}

void FlameSystem::explode(std::size_t bomb_index) {
    State& s = s_;
    Bomb& b = s.bombs[bomb_index];
    if (!b.active) return;
    b.active = false;
    if (s.players[b.owner].bombs_placed > 0) --s.players[b.owner].bombs_placed;

    int cx = b.tile_x(), cy = b.tile_y();
    int reach = b.flame;
    s.events.push_back({Event::Type::Explosion, static_cast<std::int8_t>(b.owner),
                        static_cast<std::int8_t>(cx), static_cast<std::int8_t>(cy), 0});
    ignite_epicentre(cx, cy, b.owner);
    for (Direction d : {Direction::Up, Direction::Down, Direction::Left, Direction::Right}) {
        for (int i = 1; i <= reach; ++i) {
            if (!spread_to(cx + grid::dir_dx(d) * i, cy + grid::dir_dy(d) * i, b.owner)) break;
        }
    }
}

void FlameSystem::age_flames_and_bricks() {
    State& s = s_;
    for (int y = 0; y < kGridHeight; ++y) {
        for (int x = 0; x < kGridWidth; ++x) {
            if (s.flame[y][x] > 0) --s.flame[y][x];
            if (s.burning[y][x] > 0 && --s.burning[y][x] == 0) {
                if (s.hidden[y][x] != PowerupType::None) {
                    s.floor[y][x] = s.hidden[y][x];
                    s.hidden[y][x] = PowerupType::None;
                    s.events.push_back({Event::Type::PowerupRevealed, -1,
                                        static_cast<std::int8_t>(x), static_cast<std::int8_t>(y),
                                        static_cast<std::int8_t>(s.floor[y][x])});
                }
            }
        }
    }
}

}  // namespace bomber::sim
