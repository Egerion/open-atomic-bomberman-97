// Tick orchestration. The step ORDER below is part of the determinism
// contract. It is the original main loop (sub_42A191) ROTATED to start at the
// player pass — the original's frame starts at the clock/bomb pass instead,
// but the two cuts produce the same infinite event stream (docs/re/facts.md
// "Per-tick call order — END-TO-END").
//
// player_turn is the port of sub_41F29B, one player's per-DISPLAYED-frame pass.
// Its rationale — the two input blockers, the bomb-action tail, the flight
// states, the freeze and the walk event — lives in docs/re/player-turn.md; the
// section pointers below (§1..§8) are into that page.

#include "bomber/sim/simulation.hpp"

#include <algorithm>
#include <array>
#include <utility>

#include "bomber/sim/rng.hpp"
#include "grid.hpp"
#include "systems/ai.hpp"
#include "systems/bombs.hpp"
#include "systems/diseases.hpp"
#include "systems/enclosure.hpp"
#include "systems/flames.hpp"
#include "systems/movement.hpp"
#include "systems/powerups.hpp"
#include "systems/rovers.hpp"
#include "systems/stage_actors.hpp"
#include "systems/tile_regen.hpp"

namespace bomber::sim {

namespace detail {
State build_state(const MatchConfig& config);  // setup.cpp
}

namespace {

// PLUMBING ONLY: never stored in State, never hashed, never touched by RNG.
struct Systems {
    DiseaseSystem& diseases;
    PowerupSystem& powerups;
    FlameSystem& flames;
    BombSystem& bombs;
    MovementSystem& movement;
    StageActorSystem& stage;
    EnclosureSystem& enclosure;
    TileRegenSystem& tile_regen;
    AISystem& ai;
    RoverSystem& rovers;
};

// The sub-frame delta schedule for one turn: the canonical {6,5,...} ×
// kSubFrames for the deterministic 50 ms tick, or a single measured wall-clock
// delta × 1 for the F9 native-cadence pass. `advance_timers` gates the
// once-per-TICK duration counters (docs/re/player-turn.md §1).
struct Cadence {
    const std::int32_t* sched = kSubFrameMs;
    int n_sub = kSubFrames;
    bool advance_timers = true;
};

struct TurnContext {
    Systems& sys;
    Cadence cad;
};

// Death by flame: free the carried bomb's slot, scatter the surplus loadout,
// announce the kill. The scatter draws State::rng in kind order (docs/re/
// facts.md "Death powerup scatter") — GOLDEN.
void kill_by_flame(State& s, int i, PowerupSystem& powerups) {
    Player& p = s.players[i];
    const int tx = p.tile_x(), ty = p.tile_y();
    p.alive = false;
    if (p.carrying) {
        p.carrying = false;
        if (s.players[p.carried_owner].bombs_placed > 0) --s.players[p.carried_owner].bombs_placed;
    }
    powerups.death_scatter(p);
    // Killer attribution (docs/re/results-and-options.md §1): FlameSystem::
    // spread_to stamped the flame's owner, still valid on the tick the flame is
    // present. Self-kill is left explicit rather than collapsed to -1 —
    // event.hpp's convention distinguishes "no killer" from "self".
    s.events.push_back({Event::Type::PlayerDied, static_cast<std::int8_t>(i),
                        static_cast<std::int8_t>(tx), static_cast<std::int8_t>(ty),
                        static_cast<std::int8_t>(s.flame_owner[ty][tx])});
}

// Random (sub_41E21E case 0xC): reroll uniformly over the 12 real kinds —
// Random itself is excluded by the modulus — and retry (max 200) while the roll
// is scheme-forbidden; a skull is a legal outcome. ONE RNG draw per attempt, and
// the count is part of the contract. None only if every kind is forbidden.
PowerupType reroll_random(State& s) {
    for (int tries = 0; tries < 200; ++tries) {
        auto k = static_cast<PowerupType>(random_below(s, 12));
        if (!s.forbidden[static_cast<int>(k)]) return k;
    }
    return PowerupType::None;
}

// Flame-death + pickup resolution at one player's CURRENT tile. The original
// runs this exact pair in TWO places (docs/re/facts.md "Per-tick call order —
// END-TO-END"): after every committed pixel step inside the mover (sub_41EC84,
// pseudo.c 22699-22717) and at the head of each player's next turn (sub_41F29B
// 22915-22926) — flame FIRST, pickup second, both times. Returns true if the
// player died.
bool resolve_player_field(State& s, int i, PowerupSystem& powerups, DiseaseSystem& diseases) {
    Player& p = s.players[i];
    if (!p.present || !p.alive) return false;
    const int tx = p.tile_x(), ty = p.tile_y();

    // The kill goes through the shared funnel sub_41DE63, which early-outs while
    // the victim is mid-trampoline-hop or mid-warp (movement states 5/6/7) — the
    // same immunity as the wall crush and the rover landing kill. An immune
    // player falls through to the pickup, exactly like the original, which falls
    // on through whenever sub_41DE63 reports no kill (pseudo.c 22915-22917).
    const bool flight_immune = p.bounce != 0 || p.warp != 0;
    if (s.flame[ty][tx] > 0 && !flight_immune) {
        kill_by_flame(s, i, powerups);
        return true;
    }

    PowerupType t = s.floor[ty][tx];
    if (t == PowerupType::None) return false;

    diseases.maybe_cure_on_pickup(p);
    if (t == PowerupType::Random) t = reroll_random(s);
    if (t == PowerupType::Disease) {
        diseases.assign_random(i, 1);
    } else if (t == PowerupType::SuperDisease) {
        diseases.assign_random(i, 3);
    } else if (t != PowerupType::None) {  // a fully-forbidden Random: consumed, no effect
        powerups.apply(p, t);
        s.events.push_back({Event::Type::PowerupPicked, static_cast<std::int8_t>(i),
                            static_cast<std::int8_t>(tx), static_cast<std::int8_t>(ty),
                            static_cast<std::int8_t>(t)});
    }
    s.floor[ty][tx] = PowerupType::None;
    return false;
}

// MovementSystem::PixelFn adapter: the per-pixel field check during a walk.
// A Disease pickup can be Swap, which relocates the player mid-move — the
// remaining budget then continues from the new position, exactly like the
// original's in-loop sub_41E21E.
struct FieldCtx {
    State* s;
    int index;
    PowerupSystem* powerups;
    DiseaseSystem* diseases;
};
bool on_move_pixel(void* ctx, Player& /*p*/) {
    auto* c = static_cast<FieldCtx*>(ctx);
    return resolve_player_field(*c->s, c->index, *c->powerups, *c->diseases);
}

// sub_41E61E's multi-key resolution: with more than one direction pressed and at
// least one leading to an open tile, the blocked pressed dirs are dropped.
void drop_blocked_dirs(const State& s, const Player& p, std::array<bool, 4>& dir) {
    if (dir[0] + dir[1] + dir[2] + dir[3] <= 1) return;
    const int ptx = p.tile_x(), pty = p.tile_y();
    auto passable = [&](int g) {
        return grid::tile_open(s, ptx + grid::kDx[g], pty + grid::kDy[g]) &&
               !grid::bomb_at(s, ptx + grid::kDx[g], pty + grid::kDy[g]);
    };
    int open = 0;
    for (int g = 0; g < 4; ++g)
        if (dir[g] && passable(g)) ++open;
    if (open == 0) return;
    for (int g = 0; g < 4; ++g)
        if (dir[g] && !passable(g)) dir[g] = false;
}

// Input decode -> godir (0=Up, 1=Right, 2=Down, 3=Left; -1 = no direction),
// faithful to sub_41E61E. After the multi-key filter above, the LAST surviving
// index wins. That last-index bias (Left beats Right, Down beats Up) plus the
// per-pixel mover makes a player held against a wall with two opposite keys
// vibrate in place — the original's beloved "crazy back-and-forth".
//
// The reversed-controls disease (sub_41F29B ~23049) is applied to the RESOLVED
// godir — `(g + 2) & 3` — AFTER the opposite-key filter ran on the RAW pressed
// dirs, and BEFORE the ice buffer (the delayed samples store the reversed
// value). Humans only: the `+16 != 1` gate exempts computer players. Swapping
// the input flags pre-resolution instead feeds the passability filter the
// flipped dirs, which diverges under multi-key input.
//
// PURE: reads State/Player, mutates nothing, draws NO RNG — which is what makes
// it safe to call from inside player_turn's sub-frame loop.
int decode_godir(const State& s, const Player& p, const PlayerInput& in) {
    std::array<bool, 4> dir = {in.up, in.right, in.down, in.left};
    drop_blocked_dirs(s, p, dir);
    int want_godir = -1;
    for (int g = 0; g < 4; ++g)
        if (dir[g]) want_godir = g;

    if (want_godir >= 0 && p.sick(Disease::Reversed) && !p.ai) want_godir = (want_godir + 2) & 3;
    return want_godir;
}

// The kick probe's direction: the travel axis while walking, else the belt
// direction under a standing player — the original's mover runs identically on a
// conveyor with +46 set to the belt dir, so a belt-forced kick probes along the
// belt. -1 when neither applies.
int kick_probe_dir(const State& s, const Player& p, int eff_godir) {
    if (eff_godir >= 0) return eff_godir;
    const int ptx = p.tile_x(), pty = p.tile_y();
    if (grid::in_grid(ptx, pty) && s.actor_type[pty][ptx] == ActorType::Conveyor)
        return s.actor_dir[pty][ptx];
    return -1;
}

// "Sitting EXACTLY on the tile centre along the travel axis" — the branch
// sub_41EC84 takes when its offset-to-tile-centre temporary for that axis is
// zero, which is what dispatches sub_424708. Pure, no RNG.
bool centred_on_axis(const Player& p, int godir) {
    const int px = static_cast<int>(p.x / kScale), py = static_cast<int>(p.y / kScale);
    const int sx = ((px % kTileW) + kTileW) % kTileW - kTileW / 2;
    const int sy = ((py % kTileH) + kTileH) % kTileH - kTileH / 2;
    return sx * grid::kDx[godir] + sy * grid::kDy[godir] == 0;
}

// The same disease-scaled accrual MovementSystem::move folds in, summed here for
// the PlayerWalking event (docs/re/player-turn.md §8).
Fixed disease_scaled_budget(const Player& p, std::int32_t delta_ms) {
    Fixed add = frame_budget(p.speed, delta_ms);
    if (p.sick(Disease::Slow)) add /= 3;
    if (p.sick(Disease::Fast) || p.sick(Disease::Super)) add = 3 * add / 2;
    return add;
}

// One player slot and the bomb system its actions drive.
struct ActionCtx {
    State& s;
    int index;
    BombSystem& bombs;
};

// Tail block (3): the action2 edge — stop own sliding bombs (+89), punch (+91),
// trigger-detonate (+95). The punch is additionally gated on the bomb key being
// up, which is why a1_now is passed in.
void resolve_action2_edge(ActionCtx a, bool a1_now) {
    Player& p = a.s.players[a.index];
    if (p.kick) a.bombs.stop_own_sliding(a.index);
    if (p.punch && !a1_now) a.bombs.try_punch(p, static_cast<std::uint8_t>(a.index));
    if (p.trigger) a.bombs.detonate_triggered(a.index);
}

// Tail block (4): the drop edge — grab own bomb underfoot (+92), else the
// spooger line (+93, suppressed under auto-drop), else a plain drop.
void resolve_drop_edge(ActionCtx a, bool auto_drop) {
    Player& p = a.s.players[a.index];
    const Bomb* under = grid::bomb_at(a.s, p.tile_x(), p.tile_y());
    const bool own = under && under->owner == static_cast<std::uint8_t>(a.index);
    if (p.grab && own)
        a.bombs.try_grab(p, a.index);
    else if (p.spooge && !auto_drop && own)
        a.bombs.spooge_ahead(p, static_cast<std::uint8_t>(a.index));
    else
        a.bombs.drop(p, static_cast<std::uint8_t>(a.index));
}

// sub_41F29B's bomb-action tail (23277-23380), in the original's exact order:
// auto-drop force (diarrhea +135 / super +137) -> carried-bomb throw (+37) ->
// action2 edge -> drop edge, the last gated on the constipation flag !+134.
//
// Runs on EVERY alive frame regardless of the +78 state, and once per FRAME
// rather than once per tick. `blocked` models a skipped input acquisition; the
// pickup-pause window is a DIFFERENT, forced-HELD rule that callers handle by
// skipping this tail entirely. docs/re/player-turn.md §2 and §3.
void bomb_action_tail(ActionCtx a, bool blocked, const PlayerInput& frame_in) {
    Player& p = a.s.players[a.index];
    const bool auto_drop = p.sick(Disease::Diarrhea) || p.sick(Disease::Super);
    const bool a1_now = auto_drop || (!blocked && frame_in.action1);  // +56
    const bool a1_last = !auto_drop && p.prev_action1;                // +54
    const bool a2_now = !blocked && frame_in.action2;                 // +57
    const bool a2_last = p.prev_action2;                              // +55

    // The throw fires on the auto-drop force OR on the key being released, and is
    // NOT gated by constipation.
    if (p.carrying && (auto_drop || !a1_now)) a.bombs.throw_carried(p, a.index);
    if (a2_now && !a2_last) resolve_action2_edge(a, a1_now);
    if (a1_now && !a1_last && !p.sick(Disease::Constipation)) resolve_drop_edge(a, auto_drop);

    // +54/+55 latch the EFFECTIVE keys just used, not the raw sample — see §2.
    p.prev_action1 = a1_now;
    p.prev_action2 = a2_now;
}

// Step 1: one player's turn — stun, movement (with the reversed-controls disease
// and the per-pixel flame-death/pickup checks), bomb dropping and the action2
// priority chain: throw > grab > trigger-detonate > punch.
//
// The body is a loop over canonical sub-frames because the original runs input
// acquisition, the AI brain, the movement-budget accrual and the bomb-action
// tail once per DISPLAYED frame (docs/re/facts.md "Canonical frame cadence").
// ctx.cad carries that schedule; a computer player's brain re-decides in the
// exact slot where the original calls sub_40A1C6 instead of reading DirectInput.
void player_turn(State& s, int i, const PlayerInput& tick_in, TurnContext& ctx) {
    Player& p = s.players[i];
    AISystem* ai_sys = p.ai ? &ctx.sys.ai : nullptr;
    BombSystem& bombs = ctx.sys.bombs;
    StageActorSystem& stage = ctx.sys.stage;
    MovementSystem& movement = ctx.sys.movement;
    PowerupSystem& powerups = ctx.sys.powerups;
    DiseaseSystem& diseases = ctx.sys.diseases;
    const std::int32_t* sched = ctx.cad.sched;
    const int n_sub = ctx.cad.n_sub;
    const bool advance_timers = ctx.cad.advance_timers;
    const ActionCtx act{s, i, bombs};

    // Grab pickup-pause (+78 == 4). It blocks the WHOLE tick it is decremented
    // on, so the movement gate reads the PRE-decrement value, while the live
    // field additionally catches a pause armed mid-tick. docs/re/player-turn.md
    // §1 — the two-counter split and the per-frame gate are load-bearing.
    const bool paused_entering = p.pickup_pause > 0;
    if (advance_timers && p.pickup_pause > 0) --p.pickup_pause;
    auto paused_now = [&] { return paused_entering || p.pickup_pause > 0; };

    // Trampoline hop (state 5) and warp (states 6/7) share one frame body and
    // differ only in which per-phase timer they tick down. Movement and input are
    // skipped for the whole flight; the bomb-action tail is NOT, and runs once
    // per frame of it. The apex/midpoint relocation inside tick_bounce/tick_warp
    // DRAWS RNG, so it must stay inside this state gate, before any other
    // per-tick draw. docs/re/player-turn.md §4.
    auto flight_turn = [&](bool bouncing) {
        for (int f = 0; f < n_sub; ++f) {
            if (p.stun > 0) --p.stun;
            (void)movement.ice_delay(p, -1);
            if (!paused_now()) bomb_action_tail(act, /*blocked=*/true, tick_in);
        }
        if (paused_entering) {
            // The pickup-pause carve-out skips the tail, so release a carried
            // bomb here rather than let it ride the flight untouched (§4).
            if (p.carrying) bombs.throw_carried(p, i);
            p.prev_action1 = tick_in.action1;
            p.prev_action2 = tick_in.action2;
        }
        if (!advance_timers) return;
        if (bouncing)
            stage.tick_bounce(p);
        else
            stage.tick_warp(p);
    };

    if (stage.bouncing(p)) {
        flight_turn(/*bouncing=*/true);
        return;
    }
    if (stage.warping(p)) {
        flight_turn(/*bouncing=*/false);
        return;
    }

    FieldCtx fctx{&s, i, &powerups, &diseases};
    const bool frozen = s.input_freeze > 0;  // docs/re/player-turn.md §7
    std::int32_t walk_budget = 0;            // summed accruals -> the PlayerWalking event
    for (int sub = 0; sub < n_sub; ++sub) {
        // +58 head-stun: gate first, then decrement — the original's per-frame
        // block at 22982-22984 clears the new-input flag and decrements +58
        // whenever +58 is above zero, in that order.
        const bool sub_stunned = p.stun > 0 || paused_now();
        const bool stun_blocked = p.stun > 0;  // the tail's `blocked` is this half only (§2)
        if (p.stun > 0) --p.stun;

        // A flight entered in an earlier sub-frame: the original's following
        // frames take the state-5/6/7 branch — no acquisition, no movement —
        // while +58 keeps counting and the ice buffer keeps flowing (both sit
        // above the state dispatch); hence continue, not break.
        if (p.bounce > 0 || p.warp > 0) {
            (void)movement.ice_delay(p, -1);
            if (!paused_now()) bomb_action_tail(act, /*blocked=*/true, tick_in);
            s.sub_trace[i][sub] = {p.x, p.y, p.facing};
            continue;
        }

        // Acquisition. A blocked AI must draw NOTHING this frame, same as the
        // original skipping the sub_40A1C6 call outright.
        PlayerInput sub_in = tick_in;
        if (ai_sys && !sub_stunned && !frozen) ai_sys->decide(i, sub_in, sched[sub]);

        // A stunned or input-frozen player acquires NO new direction: the
        // new-input gate skips sub_41E61E, leaving +46 at its unconditional
        // per-frame -1 reset so the mover takes the idle branch (stage actors
        // still drive it). Rebuilding it as -1 fresh each sub-frame IS faithful —
        // docs/re/player-turn.md §6 retracts the "+46 is sticky" audit finding.
        const int want_godir = (sub_stunned || frozen) ? -1 : decode_godir(s, p, sub_in);

        // Ice / input-lag (Hockey Rink, VALUELST ids 450-460; sub_41F29B
        // ~23058-23078): replaces this frame's resolved direction with a delayed
        // sample from the player's own history, for HUMAN players on that level.
        // A faithful no-op everywhere else — see MovementSystem::ice_delay.
        const int eff_godir = movement.ice_delay(p, want_godir);
        const bool moving = eff_godir >= 0;
        if (moving) walk_budget += disease_scaled_budget(p, sched[sub]);

        // Movement, with any conveyor under the player folded in (the belt-only
        // push, with no input, is handled inside move_on_actor). The per-pixel
        // field callback is the port of sub_41EC84's post-commit tail (pseudo.c
        // 22699-22717): flame death then pickup at every pixel step. A mid-move
        // kill abandons the rest of the budget AND the rest of this turn — the
        // original returns straight into the death branch, skipping the kick
        // probe, the step-on latches and the tail. A mid-move pickup is usable
        // the SAME tick. facts.md "Per-tick call order — END-TO-END" finding 1.
        stage.move_on_actor(p, eff_godir, moving, sched[sub], &on_move_pixel, &fctx);
        if (!p.alive) {
            p.prev_action1 = sub_in.action1;
            p.prev_action2 = sub_in.action2;
            return;
        }

        // Kick probe (the branch sub_41EC84 takes when its offset-to-tile-centre
        // temporary is zero, dispatching sub_424708): a player WALKING into a
        // bomb kicks it on the ARRIVAL tick, since the mover pins them at the
        // centre and this same-tick probe then sees the zero offset. Re-probing
        // while parked matches the original's every-iteration re-kick (a
        // same-direction re-kick is a silent no-op in try_kick). facts.md
        // "Core-feel audit" §1.
        const int probe = kick_probe_dir(s, p, eff_godir);
        if (probe >= 0 && centred_on_axis(p, probe)) bombs.try_kick(p, grid::from_godir(probe), i);

        // The tail sits after the mover and its kick probe because the original's
        // does (LABEL_155's dispatch, then LABEL_246), so the "own bomb
        // underfoot" probe and the drop tile read THIS frame's committed
        // position. A pickup-paused player skips it entirely (§2).
        if (!paused_now()) bomb_action_tail(act, stun_blocked || frozen, sub_in);

        // Presentation sub-frame trace (State::sub_trace) — derived output like
        // s.events, never hashed: where this player ended THIS canonical frame,
        // so the original's per-frame micro-motion survives to the screen instead
        // of being lerped away between tick endpoints.
        s.sub_trace[i][sub] = {p.x, p.y, p.facing};
    }

    if (walk_budget > 0) {
        // Whole px on the deterministic path, 1/16 px on the F9 per-frame path —
        // docs/re/player-turn.md §8 for why the unit has to differ.
        const Fixed walk_unit = (n_sub == kSubFrames) ? kScale : kScale / 16;
        s.events.push_back(
            {Event::Type::PlayerWalking, static_cast<std::int8_t>(i),
             static_cast<std::int8_t>(p.tile_x()), static_cast<std::int8_t>(p.tile_y()),
             static_cast<std::int8_t>(std::clamp<Fixed>(walk_budget / walk_unit, 1, 127))});
    }

    // A trampoline/warphole entered during the sub-frame loop ALSO burns its
    // first state frame on this very tick, and before the tail: sub_41F29B runs
    // the mover (and so the step-on trigger) BEFORE the state dispatch. Without
    // this the hop/warp would land one tick late. There is deliberately NO
    // post-tick "standing on one" fallback. docs/re/player-turn.md §5.
    if (advance_timers) {
        if (p.bounce > 0)
            stage.tick_bounce(p);
        else if (p.warp > 0)
            stage.tick_warp(p);
    }

    // The tail runs per frame inside the loop; all that is left is the
    // pickup-paused player's +54/+55 latch, which the skipped tail never did.
    // The gate is the PRE-decrement snapshot, the same one the movement gate
    // uses — the original drives both from the ONE state-4 test at 0x41FA42, so
    // they cannot differ. A pause armed mid-tick needs no latch: the frame that
    // grabbed ran the tail, and the tail latches for itself.
    if (paused_entering) {
        p.prev_action1 = tick_in.action1;
        p.prev_action2 = tick_in.action2;
    }
}

// Tick step 1's body: every present, alive player takes its turn, in SLOT ORDER.
// A computer player resolves its own input INSIDE its player_turn (ADR-0005 §5),
// which keeps the AI's RNG draws interleaved with movement in slot order as the
// original interleaves the brain and the mover per player — so this loop order
// is part of the determinism contract. The present/alive test covers the
// entering/dying/dead modes.
void players_pass(State& s, const TickInputs& inputs, TurnContext& ctx) {
    for (int i = 0; i < kMaxPlayers; ++i) {
        const Player& p = s.players[i];
        if (!p.present || !p.alive) continue;
        player_turn(s, i, inputs.players[i], ctx);
    }
}

// The head checks: flames kill players; floor powerups get picked up (with the
// pre-pickup cure roll and the skull dispatch). This is the original's NEXT-turn
// head pair (sub_41F29B 22915-22926) at our rotation's cut point — it runs for
// every player (moved or not), in slot order, after the walls.
void field_vs_players(State& s, PowerupSystem& powerups, DiseaseSystem& diseases) {
    for (int i = 0; i < kMaxPlayers; ++i) resolve_player_field(s, i, powerups, diseases);
}

enum class TickPhase : std::uint8_t { Full, Players, Systems };

// `phase` splits the tick for the F9 native-cadence mode (ADR-0007):
//   Players  = only the per-frame movement/AI pass, run once per DISPLAYED
//              frame with a single measured wall-clock delta;
//   Systems  = only the 50 ms-quantized systems pass, run off a real-time
//              accumulator so fuse/flame/hurry timers stay on their native grid;
//   Full     = both back-to-back — the deterministic default tick().
void run_tick(State& s, const TickInputs& inputs, Cadence cad = {},
              TickPhase phase = TickPhase::Full) {
    if (phase != TickPhase::Systems) {
        s.events.clear();
        // sub_trace prefill: every sample starts at the tick-entry position;
        // player_turn overwrites sample f as it moves and step 12 pins the last.
        for (int i = 0; i < kMaxPlayers; ++i) {
            const Player& pp = s.players[i];
            for (int f = 0; f < kSubFrames; ++f) s.sub_trace[i][f] = {pp.x, pp.y, pp.facing};
        }
    }

    // Systems are cheap stack objects wired to the shared state; their
    // construction order is irrelevant, the CALL order below is not.
    DiseaseSystem diseases{s};
    PowerupSystem powerups{s};
    FlameSystem flames{s, powerups};
    BombSystem bombs{s, flames, powerups};
    MovementSystem movement{s};
    StageActorSystem stage{s, movement};
    EnclosureSystem enclosure{s};
    TileRegenSystem tile_regen{s};
    AISystem ai{s};
    RoverSystem rovers{s};

    Systems sys{diseases, powerups,  flames,     bombs, movement,
                stage,    enclosure, tile_regen, ai,    rovers};
    TurnContext ctx{sys, cad};

    // 1. Players: movement (with conveyor/trampoline actors), bomb drop,
    //    throw/grab/trigger/punch. The actor effects are folded into this step
    //    exactly where the original applies them (sub_41F29B does movement and
    //    the actor branches in one pass), so they are not a separate tick step.
    if (phase != TickPhase::Systems) players_pass(s, inputs, ctx);
    if (phase == TickPhase::Players) return;  // the 50 ms pass is driven separately

    // 1b. Round-start input freeze countdown. The decrement must come AFTER the
    //     pass or the window is one tick short — docs/re/player-turn.md §7.
    if (s.input_freeze > 0) --s.input_freeze;

    // 2. Campaign rover/ghost hazards: drive the mover 1 tick (spawn/wander/
    // flame-death/landing-tile kill) and the "all hazards dead" grace timer
    // (docs/re/campaign.md "Round pacing", sub_4016DA). The original calls
    // sub_401F76 from the campaign callback IMMEDIATELY AFTER the player pass
    // (sub_42A191 29527 -> 29528-29529), BEFORE the next frame's bomb pass — so
    // a rover never reacts to flames lit later in the same gap (facts.md
    // "Per-tick call order — END-TO-END" finding 3). Zero draws, zero cost when
    // s.rovers is empty.
    rovers.tick();

    // 3. Match clock. The original updates it at the top of the frame
    // (sub_4105D2, sub_42A191 29518) — after the rovers, before the bomb pass,
    // in rotation terms — and the enclosure below reads the value updated in
    // this same gap.
    if (s.ticks_left > 0 && --s.ticks_left == 0)
        s.events.push_back({Event::Type::TimeUp, -1, -1, -1, 0});

    // Once a round is decided down to <= 1 alive SIDE the original FREEZES every
    // still-armed bomb: the fuse accrual, the timeout explosion and the nested
    // flame-arm spread all sit behind sub_42331C's `sub_421969() > 1` gate at
    // pseudo.c 25603, so no fuse counts down, no chain propagates and no trigger
    // press resolves until the round transitions (docs/re/audit/bombs.md finding
    // 1). Bomb MOVEMENT is the switch cases BEFORE that gate and is NOT frozen.
    // sub_421969 returns a forced 2 in campaign, so the freeze is EXEMPT there.
    //
    // The SAME predicate is the TOP-LEVEL gate of the enclosure stepper
    // sub_426818, which wraps its ENTIRE body in it (batch_0x42583B.cpp:678-679),
    // so step 8 freezes on the very same edge. Both sites read the same latched
    // dword_4621D4 the frame's player pass just recomputed and can never disagree
    // within a frame. Flame aging (step 7) has NO such gate and keeps running —
    // that asymmetry is real.
    const bool round_frozen = sides_remaining(s) <= 1 && !s.campaign_hazards_active;

    // 4. Drain the chain-detonation queue (facts.md "Chain-reaction timing",
    // sub_423209/dword_462200). The original drains once, at the very top of its
    // per-frame bomb pass — the first bomb phase after the player pass in the
    // rotated stream. So a trigger press, queued during step 1, is caught by THIS
    // drain with no player move in between, while a flame-arm/slide/landing hit
    // is queued below, after the drain already fired, and waits for the NEXT
    // tick's: one chain LINK per tick, not the whole chain at once.
    if (!round_frozen) flames.drain_chain_queue();

    // 5. Kicked bombs slide; airborne bombs fly. NOT frozen by bombs F1.
    bombs.advance_bombs();

    // 6. Fuses (paused while a bomb is airborne). Frozen by bombs F1.
    if (!round_frozen) bombs.tick_fuses();

    // NOTE (documented deviation, facts.md "Per-tick call order" accepted
    // deviations): the original interleaves steps 5/6 PER BOMB SLOT — slot k's
    // slide runs after slot j<k's explosion within the same frame. Our phase
    // split makes same-tick bomb-vs-bomb coincidences uniformly "movement
    // first"; matching exactly would require the 100-slot first-fit allocator.

    // 7. Flames fade, bricks finish crumbling (revealing powerups). The original
    // ages the grid right after its bomb pass (sub_426D06, sub_42A191 29525) — a
    // flame lit this tick ages once this tick.
    flames.age_flames_and_bricks();

    // 8. Walls closing in during the hurry phase, AFTER the flame aging and
    // BEFORE the head checks (sub_426818 at 29526) — a wall crush beats a
    // same-tick flame kill (no killer credit) and destroys an un-picked-up token
    // under the dropping wall. Tile regeneration runs immediately before the wall
    // stepper because sub_426704 is called from WITHIN sub_426818 (facts.md
    // "Per-level tile regeneration"); a no-op on every level but Haunted House.
    //
    // Both go behind ONE gate because the original has one covering both
    // (docs/re/enclosure.md §8). A DIFFERENT question from TimeUp: the clock
    // hitting zero does NOT stop the spiral — sub_410578 clamps at 0, so the
    // predicate stays true (see §2's "NO ticks_left > 0 guard" note).
    if (!round_frozen) {
        tile_regen.update();
        enclosure.update();
    }

    // 9. The head checks: flames kill players, floor powerups get picked up —
    // the original's NEXT player pass's turn-head pair (sub_41F29B 22915-22926)
    // at this rotation's cut point.
    field_vs_players(s, powerups, diseases);

    // 10. Diseases: spread on contact, age the freshness gate, expire. The
    // original runs these right after the head checks inside each player's turn
    // (sub_41F29B 22927-22975).
    diseases.spread_and_age();

    // 11. Compact dead bombs (stable order — deterministic).
    s.bombs.erase(
        std::remove_if(s.bombs.begin(), s.bombs.end(), [](const Bomb& b) { return !b.active; }),
        s.bombs.end());

    // 12. Pin every player's LAST sub-frame sample to the tick's true endpoint:
    // post-loop relocations (trampoline apex, warp midpoint, head-hit scatter)
    // move a player after its own sub-frame loop finished, and the trace
    // playback's final segment must land exactly where the next tick starts.
    for (int i = 0; i < kMaxPlayers; ++i) {
        const Player& pp = s.players[i];
        s.sub_trace[i][kSubFrames - 1] = {pp.x, pp.y, pp.facing};
    }

    ++s.tick;
}

}  // namespace

Simulation::Simulation(const MatchConfig& config) : state_(detail::build_state(config)) {}

void Simulation::tick(const TickInputs& inputs) {
    run_tick(state_, inputs);
}

void Simulation::frame(const TickInputs& inputs, std::int32_t delta_ms) {
    // F9 native-cadence path (ADR-0007): the movement/AI pass runs ONCE for this
    // displayed frame with the measured wall-clock delta, then the 50 ms-
    // quantized systems pass advances off a real-time accumulator — mirroring the
    // original's per-frame gameplay driver sub_42A191. This whole path is
    // NON-DETERMINISTIC (delta is real wall-clock) and is a live-feel lever only;
    // tick() stays the deterministic entry the tests and the oracle use.
    if (delta_ms < 1) delta_ms = 1;  // never a zero-advance frame
    const std::int32_t sched[1] = {delta_ms};
    // Does this displayed frame cross a 50 ms tick boundary? If so the once-per-
    // tick duration counters in player_turn may advance; otherwise they hold,
    // keeping those durations 20 Hz-paced under per-frame movement.
    const bool crosses_tick = (systems_accum_ms_ + delta_ms) >= kMsPerTick;
    run_tick(state_, inputs, {sched, 1, crosses_tick}, TickPhase::Players);
    systems_accum_ms_ += delta_ms;
    // Spiral-of-death guard: a long stall (window drag, breakpoint, alt-tab) must
    // not fire a hundred systems passes in one frame — cap the queued real time,
    // exactly like run_match's kMaxCatchupTicks.
    if (systems_accum_ms_ > 4 * kMsPerTick) systems_accum_ms_ = 4 * kMsPerTick;
    while (systems_accum_ms_ >= kMsPerTick) {
        systems_accum_ms_ -= kMsPerTick;
        run_tick(state_, inputs, {}, TickPhase::Systems);
    }
}

std::uint64_t Simulation::hash() const {
    return state_hash(state_);
}

bool tile_blocked(const State& s, int tx, int ty) {
    return !grid::tile_open(s, tx, ty);
}

bool tile_has_bomb(const State& s, int tx, int ty) {
    return grid::bomb_at(s, tx, ty) != nullptr;
}

int alive_count(const State& s) {
    int n = 0;
    for (const auto& p : s.players)
        if (p.present && p.alive) ++n;
    return n;
}

// Shared "which side is slot i on" helper for sides_remaining/winning_side.
// Our semantics (docs/re/ai.md TEAM follow-up): team 0 is "no team" and never
// merges with another team-0 player, so team 0 players are always distinct
// sides — an all-zero roster degenerates to exactly alive_count()'s behaviour.
namespace {
bool on_same_side(const Player& a, const Player& b) {
    return a.team != 0 && a.team == b.team;
}
}  // namespace

int sides_remaining(const State& s) {
    int sides = 0;
    std::array<bool, kMaxPlayers> counted{};
    for (int i = 0; i < kMaxPlayers; ++i) {
        const Player& p = s.players[i];
        if (!p.present || !p.alive || counted[i]) continue;
        ++sides;
        counted[i] = true;
        for (int j = i + 1; j < kMaxPlayers; ++j) {
            const Player& q = s.players[j];
            if (q.present && q.alive && on_same_side(p, q)) counted[j] = true;
        }
    }
    return sides;
}

int winning_side(const State& s) {
    int winner = -1;
    for (int i = 0; i < kMaxPlayers; ++i) {
        const Player& p = s.players[i];
        if (!p.present || !p.alive) continue;
        if (winner == -1) {
            winner = i;
            continue;
        }
        // Another live player: only still a win if they share the winner's side.
        if (!on_same_side(s.players[winner], p)) return -1;
    }
    return winner;  // -1 if nobody is alive (mutual wipe-out -> draw)
}

int enclose_total(int depth) {
    return EnclosureSystem::total(depth);
}

bool enclose_pos(int index, int depth, int* x, int* y) {
    return EnclosureSystem::position(index, depth, x, y);
}

}  // namespace bomber::sim
