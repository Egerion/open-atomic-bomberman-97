// Tick orchestration. The step ORDER below is part of the determinism
// contract (and mirrors the original main loop, sub_42A191): players act,
// bombs move, fuses burn, the field ages, players collide with the field,
// diseases spread, then the clock and the closing walls.

#include "bomber/sim/simulation.hpp"

#include <algorithm>
#include <utility>

#include "bomber/sim/rng.hpp"
#include "grid.hpp"
#include "systems/bombs.hpp"
#include "systems/diseases.hpp"
#include "systems/enclosure.hpp"
#include "systems/flames.hpp"
#include "systems/movement.hpp"
#include "systems/powerups.hpp"

namespace bomber::sim {

namespace detail {
State build_state(const MatchConfig& config);  // setup.cpp
}

namespace {

// Step 1: one player's turn — stun, movement (with the reversed-controls
// disease), bomb dropping (edge-gated, spooger, auto-drop diseases), and the
// action2 priority chain: throw > grab > trigger-detonate > punch.
void player_turn(State& s, int i, const PlayerInput& in, MovementSystem& movement,
                 BombSystem& bombs) {
    Player& p = s.players[i];

    if (p.stun > 0) {
        --p.stun;
        p.prev_action1 = in.action1;
        p.prev_action2 = in.action2;
        return;
    }

    // Reversed-controls disease flips the pressed direction (humans only in
    // the original; our sim drives every player through inputs).
    bool up = in.up, down = in.down, left = in.left, right = in.right;
    if (p.sick(Disease::Reversed)) {
        std::swap(up, down);
        std::swap(left, right);
    }

    Direction want = p.facing;
    bool moving = true;
    if (up) want = Direction::Up;
    else if (down) want = Direction::Down;
    else if (left) want = Direction::Left;
    else if (right) want = Direction::Right;
    else moving = false;

    if (moving) {
        Fixed bx = p.x, by = p.y;
        movement.move(p, want);
        if (p.x == bx && p.y == by) bombs.try_kick(p, want);
    }

    // Diarrhea and super force a bomb out every tick; constipation blocks
    // dropping entirely. A manual bomb press is edge-triggered, matching the
    // original's `+56 && !+54` gate. Spooger sprays a line ahead only when a
    // bomb is already underfoot — so the first press lays one underfoot and a
    // second press (while standing on it) fires the run.
    bool auto_drop = p.sick(Disease::Diarrhea) || p.sick(Disease::Super);
    bool pressed = in.action1 && !p.prev_action1;
    if (!p.sick(Disease::Constipation)) {
        if (auto_drop) {
            bombs.drop(p, static_cast<std::uint8_t>(i));
        } else if (pressed) {
            if (p.spooge && grid::bomb_at(s, p.tile_x(), p.tile_y()))
                bombs.spooge_ahead(p, static_cast<std::uint8_t>(i));
            else
                bombs.drop(p, static_cast<std::uint8_t>(i));
        }
    }
    if (in.action2 && !p.prev_action2) {
        bool acted = false;
        if (p.carrying) {
            bombs.throw_carried(p, i);
            acted = true;
        }
        if (!acted && p.grab) acted = bombs.try_grab(p, i);
        if (!acted && p.trigger) acted = bombs.detonate_triggered(i);
        if (!acted && p.punch) bombs.try_punch(p, static_cast<std::uint8_t>(i));
    }
    p.prev_action1 = in.action1;
    p.prev_action2 = in.action2;
}

// Step 5: flames kill players; floor powerups get picked up (with the
// pre-pickup cure roll and the skull dispatch).
void field_vs_players(State& s, PowerupSystem& powerups, DiseaseSystem& diseases) {
    for (int i = 0; i < kMaxPlayers; ++i) {
        Player& p = s.players[i];
        if (!p.present || !p.alive) continue;
        int tx = p.tile_x(), ty = p.tile_y();
        if (s.flame[ty][tx] > 0) {
            p.alive = false;
            if (p.carrying) {
                // The carried bomb dies with the carrier; free the owner's slot.
                p.carrying = false;
                if (s.players[p.carried_owner].bombs_placed > 0)
                    --s.players[p.carried_owner].bombs_placed;
            }
            s.events.push_back({Event::Type::PlayerDied, static_cast<std::int8_t>(i),
                                static_cast<std::int8_t>(tx), static_cast<std::int8_t>(ty), 0});
            continue;
        }
        PowerupType t = s.floor[ty][tx];
        if (t != PowerupType::None) {
            diseases.maybe_cure_on_pickup(p);
            if (t == PowerupType::Disease) {
                diseases.assign_random(i, 1);
            } else if (t == PowerupType::SuperDisease) {
                diseases.assign_random(i, 3);
            } else {
                powerups.apply(p, t);
                s.events.push_back({Event::Type::PowerupPicked, static_cast<std::int8_t>(i),
                                    static_cast<std::int8_t>(tx), static_cast<std::int8_t>(ty),
                                    static_cast<std::int8_t>(t)});
            }
            s.floor[ty][tx] = PowerupType::None;
        }
    }
}

void run_tick(State& s, const TickInputs& inputs) {
    s.events.clear();

    // Systems are cheap stack objects wired to the shared state; their
    // construction order is irrelevant, the CALL order below is not.
    FlameSystem flames{s};
    DiseaseSystem diseases{s};
    PowerupSystem powerups{s};
    BombSystem bombs{s, flames, powerups};
    MovementSystem movement{s};
    EnclosureSystem enclosure{s, flames};

    // 1. Players: movement, bomb drop, throw/grab/trigger/punch.
    for (int i = 0; i < kMaxPlayers; ++i) {
        Player& p = s.players[i];
        if (!p.present || !p.alive) continue;
        player_turn(s, i, inputs.players[i], movement, bombs);
    }

    // 2. Kicked bombs slide; airborne bombs fly.
    bombs.advance_bombs();

    // 3. Fuses (paused while a bomb is airborne).
    bombs.tick_fuses();

    // 4. Flames fade, bricks finish crumbling (revealing powerups).
    flames.age_flames_and_bricks();

    // 5. Flames kill players; floor powerups get picked up.
    field_vs_players(s, powerups, diseases);

    // 5b. Diseases: spread on contact, age the freshness gate, and expire.
    diseases.spread_and_age();

    // 6. Match clock, and walls closing in during the hurry phase.
    if (s.ticks_left > 0 && --s.ticks_left == 0)
        s.events.push_back({Event::Type::TimeUp, -1, -1, -1, 0});
    enclosure.update();

    // 7. Compact dead bombs (stable order — deterministic).
    s.bombs.erase(std::remove_if(s.bombs.begin(), s.bombs.end(),
                                 [](const Bomb& b) { return !b.active; }),
                  s.bombs.end());

    ++s.tick;
}

}  // namespace

Simulation::Simulation(const MatchConfig& config) : state_(detail::build_state(config)) {}

void Simulation::tick(const TickInputs& inputs) { run_tick(state_, inputs); }

std::uint64_t Simulation::hash() const { return state_hash(state_); }

bool tile_blocked(const State& s, int tx, int ty) { return !grid::tile_open(s, tx, ty); }

bool tile_has_bomb(const State& s, int tx, int ty) {
    return grid::bomb_at(s, tx, ty) != nullptr;
}

int alive_count(const State& s) {
    int n = 0;
    for (const auto& p : s.players)
        if (p.present && p.alive) ++n;
    return n;
}

int enclose_total(int depth) { return EnclosureSystem::total(depth); }

bool enclose_pos(int index, int depth, int* x, int* y) {
    return EnclosureSystem::position(index, depth, x, y);
}

}  // namespace bomber::sim
