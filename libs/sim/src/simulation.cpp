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
#include "systems/stage_actors.hpp"

namespace bomber::sim {

namespace detail {
State build_state(const MatchConfig& config);  // setup.cpp
}

namespace {

// Step 1: one player's turn — stun, movement (with the reversed-controls
// disease), bomb dropping (edge-gated, spooger, auto-drop diseases), and the
// action2 priority chain: throw > grab > trigger-detonate > punch.
void player_turn(State& s, int i, const PlayerInput& in, BombSystem& bombs,
                 StageActorSystem& stage) {
    Player& p = s.players[i];

    if (p.stun > 0) {
        --p.stun;
        p.prev_action1 = in.action1;
        p.prev_action2 = in.action2;
        return;
    }

    // A trampoline hop is a state-gated in-place bounce (sub_41EC84/sub_41DE63):
    // movement input and bomb actions are ignored until the hop finishes, and
    // the player cannot be pushed. Tick it down and skip the rest of the turn.
    if (stage.bouncing(p)) {
        stage.tick_bounce(p);
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

    // Opposite-key resolution, faithful to the original input decoder
    // (sub_41E61E, LABEL_58): collect the four direction flags in GODIR order
    // (0=Up,1=Right,2=Down,3=Left); if more than one is pressed and at least
    // one leads to an open tile, drop the pressed dirs that are blocked; then
    // the LAST surviving index wins. That last-index bias (Left beats Right,
    // Down beats Up) plus the per-pixel mover makes a player held against a wall
    // with two opposite keys vibrate in place — flip facing every tick — which
    // is the original's beloved "crazy back-and-forth" (only vs a left wall for
    // L+R or a bottom wall for U+D; the other side just slides off). No RNG, no
    // new hashed field, but trajectories change → golden must be recaptured.
    static constexpr int DX[4] = {0, 1, 0, -1};
    static constexpr int DY[4] = {-1, 0, 1, 0};
    const bool godir_pressed[4] = {up, right, down, left};
    bool dir[4] = {up, right, down, left};
    if (godir_pressed[0] + godir_pressed[1] + godir_pressed[2] + godir_pressed[3] > 1) {
        const int ptx = p.tile_x(), pty = p.tile_y();
        auto passable = [&](int g) {
            return grid::tile_open(s, ptx + DX[g], pty + DY[g]) &&
                   !grid::bomb_at(s, ptx + DX[g], pty + DY[g]);
        };
        int open = 0;
        for (int g = 0; g < 4; ++g)
            if (dir[g] && passable(g)) ++open;
        if (open > 0)
            for (int g = 0; g < 4; ++g)
                if (dir[g] && !passable(g)) dir[g] = false;
    }
    int want_godir = -1;
    for (int g = 0; g < 4; ++g)
        if (dir[g]) want_godir = g;

    static constexpr Direction kGodir[4] = {Direction::Up, Direction::Right,
                                            Direction::Down, Direction::Left};
    Direction want = want_godir >= 0 ? kGodir[want_godir] : p.facing;
    bool moving = want_godir >= 0;

    // Movement, with any conveyor under the player folded in: a belt speeds/
    // slows a walking player and pushes a standing one along its direction
    // (StageActorSystem::move_on_actor, port of sub_41F29B's actor branches).
    // The belt-only push (no input) is handled inside move_on_actor.
    {
        Fixed bx = p.x, by = p.y;
        stage.move_on_actor(p, want_godir, moving);
        // A player pushed head-on into a restable bomb kicks it, same as a
        // walked-into bomb: only when the (belt-forced or input) move stalled.
        if (moving && p.x == bx && p.y == by) bombs.try_kick(p, want, i);
    }
    // A step that settled on a trampoline tile launches an in-place hop.
    stage.trampoline_after_move(p, i);

    // The bomb key (action1) is context-sensitive, exactly like the original
    // (sub_41F29B, gated on +56 && !+54): while carrying a grabbed bomb you
    // hold it and THROW the instant the key is released (`+37 && !+56`);
    // otherwise a fresh press on your OWN resting bomb GRABS it (blue glove,
    // +92) or sprays a SPOOGER line (+93), and on an empty tile it just DROPS.
    // Grab and spooger are the "double-tap" Ege remembers: press 1 lays a bomb
    // underfoot, press 2 (now standing on it) grabs or sprays. Diarrhea/super
    // auto-drop every tick; constipation blocks dropping.
    bool auto_drop = p.sick(Disease::Diarrhea) || p.sick(Disease::Super);
    bool pressed = in.action1 && !p.prev_action1;
    if (!p.sick(Disease::Constipation)) {
        if (p.carrying) {
            if (!in.action1) bombs.throw_carried(p, i);  // release to throw
        } else if (auto_drop) {
            bombs.drop(p, static_cast<std::uint8_t>(i));
        } else if (pressed) {
            const Bomb* under = grid::bomb_at(s, p.tile_x(), p.tile_y());
            const bool own = under && !under->moving &&
                             under->owner == static_cast<std::uint8_t>(i);
            if (p.grab && own)
                bombs.try_grab(p, i);
            else if (p.spooge && under)
                bombs.spooge_ahead(p, static_cast<std::uint8_t>(i));
            else
                bombs.drop(p, static_cast<std::uint8_t>(i));
        }
    }
    // The action key (action2) punches the bomb ahead (+91) and/or detonates a
    // trigger bomb (+95). Grab and throw moved to the bomb key above; the
    // mutual exclusions mean a player never holds punch and trigger at once.
    if (in.action2 && !p.prev_action2) {
        if (p.punch) bombs.try_punch(p, static_cast<std::uint8_t>(i));
        if (p.trigger) bombs.detonate_triggered(i);
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
            if (t == PowerupType::Random) {
                // Random (sub_41E21E case 0xC): reroll uniformly over the 12
                // real kinds — Random itself is excluded by the modulus — and
                // retry (max 200) while the roll is scheme-forbidden; then
                // dispatch as the rolled kind (a skull is a legal outcome).
                // One RNG draw per attempt; the count is part of the contract.
                PowerupType rolled = PowerupType::None;
                for (int tries = 0; tries < 200; ++tries) {
                    auto k = static_cast<PowerupType>(random_below(s, 12));
                    if (!s.forbidden[static_cast<int>(k)]) {
                        rolled = k;
                        break;
                    }
                }
                t = rolled;  // None only if every kind is forbidden
            }
            if (t == PowerupType::None) {
                // fully-forbidden Random: the token is consumed with no effect
            } else if (t == PowerupType::Disease) {
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
    StageActorSystem stage{s, movement};
    EnclosureSystem enclosure{s, flames};

    // 1. Players: movement (with conveyor/trampoline actors), bomb drop,
    //    throw/grab/trigger/punch. The conveyor push is part of the move budget
    //    and the trampoline hop is triggered on settling, so both live inside
    //    the player turn (mirroring sub_41F29B, which does movement + the actor
    //    branches in one pass). No new tick step: the actor effects are folded
    //    into step 1 exactly where the original applies them.
    for (int i = 0; i < kMaxPlayers; ++i) {
        Player& p = s.players[i];
        if (!p.present || !p.alive) continue;
        player_turn(s, i, inputs.players[i], bombs, stage);
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
