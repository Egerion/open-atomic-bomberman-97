// Tick orchestration. The step ORDER below is part of the determinism
// contract. It is the original main loop (sub_42A191) ROTATED to start at the
// player pass — the original's frame starts at the clock/bomb pass instead,
// but the two cuts produce the same infinite event stream (docs/re/facts.md
// "Per-tick call order — END-TO-END"): players act (dying or picking up per
// pixel step, exactly like the original's mover tail), rovers move, the clock
// ticks, queued chain-detonations resolve, bombs move, fuses burn, the field
// ages, the walls close, players collide with the field (the original's
// next-turn head checks), diseases spread.

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

// Flame-death + pickup resolution at one player's CURRENT tile. The original
// runs this exact pair in TWO places (docs/re/facts.md "Per-tick call order —
// END-TO-END"): after every committed pixel step inside the mover
// (sub_41EC84, pseudo.c 22699-22717) and at the head of each player's next
// turn (sub_41F29B 22915-22926) — flame FIRST, pickup second, both times.
// Returns true if the player died.
bool resolve_player_field(State& s, int i, PowerupSystem& powerups, DiseaseSystem& diseases) {
    Player& p = s.players[i];
    if (!p.present || !p.alive) return false;
    const int tx = p.tile_x(), ty = p.tile_y();
    if (s.flame[ty][tx] > 0) {
        // The kill goes through the shared funnel sub_41DE63, which early-outs
        // while the victim is mid-trampoline-hop or mid-warp (movement states
        // 5/6/7) — the same immunity already ported for the wall crush
        // (EnclosureSystem::drop_wall) and the rover landing kill. An immune
        // player falls through to the pickup below, exactly like the
        // original's `!sub_41DE63(...)` continuation (pseudo.c 22915-22917).
        if (p.bounce == 0 && p.warp == 0) {
            p.alive = false;
            if (p.carrying) {
                // The carried bomb dies with the carrier; free the owner's slot.
                p.carrying = false;
                if (s.players[p.carried_owner].bombs_placed > 0)
                    --s.players[p.carried_owner].bombs_placed;
            }
            // Death powerup scatter (sub_41DBFE via the shared death funnel
            // sub_41DE63): the player's surplus over its start-with loadout
            // rains back onto random floor tiles. Draws State::rng in kind
            // order (docs/re/facts.md "Death powerup scatter") — GOLDEN.
            powerups.death_scatter(p);
            // Killer attribution (docs/re/results-and-options.md §1): the
            // flame that killed this player was stamped with its owner in
            // FlameSystem::spread_to (s.flame_owner), still valid here since
            // this runs the same tick the flame is present. Self-kill (owner
            // == victim) is left explicit in the event, not collapsed to -1 —
            // event.hpp's convention distinguishes "no killer" from "self".
            s.events.push_back({Event::Type::PlayerDied, static_cast<std::int8_t>(i),
                                static_cast<std::int8_t>(tx), static_cast<std::int8_t>(ty),
                                static_cast<std::int8_t>(s.flame_owner[ty][tx])});
            return true;
        }
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

// Step 1: one player's turn — stun, movement (with the reversed-controls
// disease and the per-pixel flame-death/pickup checks), bomb dropping
// (edge-gated, spooger, auto-drop diseases), and the action2 priority chain:
// throw > grab > trigger-detonate > punch.
void player_turn(State& s, int i, const PlayerInput& in, BombSystem& bombs, StageActorSystem& stage,
                 MovementSystem& movement, PowerupSystem& powerups, DiseaseSystem& diseases) {
    Player& p = s.players[i];

    // Unit vectors in godir order (0=Up,1=Right,2=Down,3=Left). Hoisted to the
    // top so BOTH the input decode and the kick probe below share them; the
    // (g±1)&3 corner rotations depend on this exact ordering.
    static constexpr int DX[4] = {0, 1, 0, -1};
    static constexpr int DY[4] = {-1, 0, 1, 0};
    static constexpr Direction kGodir[4] = {Direction::Up, Direction::Right, Direction::Down,
                                            Direction::Left};

    // Head-hit stun (Player::stun == the original's WORD +58, sub_421F7E) and
    // grab pickup-pause (Player::pickup_pause == the original's state +78==4
    // window, gated by getvalue(665)/our tuning.pickup_pause) are TWO
    // independent counters in the original — see the doc comments on both
    // fields and facts.md "Player state machine (+78) — COMPLETE". They used
    // to share one field (`stun`), which meant a grab clobbered an
    // in-progress head-stun countdown (or vice versa); split 2026-07-11.
    // Either counter clears v113 in the original (sub_41F29B ~22981-23027),
    // blocking ONLY new-input acquisition — the sub_41E61E / AI-decide call
    // that would set a new direction (+46) and the bomb-key bytes (+56/+57) —
    // plus one cosmetic standing-anim pick (~23086). Neither gates the mover:
    // a blocked-but-alive player leaves +46 at its per-tick -1 reset (22980),
    // so it takes the IDLE movement branch (23413), and the per-pixel stepper
    // still runs whenever a STAGE ACTOR drives it — a conveyor keeps carrying
    // it (23417 sets +46 to the belt dir before calling sub_41EC84, whose body
    // is itself gated on +46 != -1 at 22572), the belt-forced kick still
    // probes, and a warphole/trampoline step-on still fires. Only issuing a
    // NEW direction or bomb action is blocked; an off-belt player simply
    // stands (Bomberman has no free momentum to coast). Both counters
    // decrement independently and unconditionally every alive tick (22982-90
    // for +58; the state-4 anim-frame counter for pickup-pause) — DECREMENT
    // both and fall through, but force the resolved input to neutral
    // (want_godir = -1 below) and skip the bomb-action block if EITHER is
    // active — exactly matching a skipped sub_41E61E, which leaves +46 = -1
    // and the reset key bytes 0 so no edge-gated action can fire.
    const bool stunned = p.stun > 0 || p.pickup_pause > 0;
    if (p.stun > 0) --p.stun;
    if (p.pickup_pause > 0) --p.pickup_pause;

    // A trampoline hop is a state-gated flight (sub_41F29B state 5 / sub_41DE63):
    // movement input and bomb actions are ignored until the hop finishes, and the
    // player cannot be pushed. tick_bounce ticks it down AND, at the apex, teleports
    // the player to a random nearby open tile (the "fly + random land" — it is NOT
    // an in-place bounce; see docs/re/stage-actors.md §4). The apex relocation draws
    // RNG, so it runs here inside the state gate, before any other per-tick draw.
    //
    // CONFIRMED (pseudo.c ~23198/23248, the v86==5 branch's `goto LABEL_246` and
    // the fall-through from states 6/7 into the same label): the original's
    // LABEL_246 — which includes the carried-bomb throw-release check (`if (+37)
    // { if (v112 || !+56) throw }`) — is NOT skipped during a bounce or warp; it
    // runs every tick regardless of +78. Because input is fully blocked the whole
    // time (+56 stays 0, never re-set), `!+56` is true from the very first tick,
    // so a player who enters a bounce/warp WHILE CARRYING has the bomb thrown at
    // their current tile almost immediately, not held through the whole flight.
    // Our port takes an early return here (skipping the rest of player_turn, incl.
    // the throw block in the bomb-key section below) for the whole bounce/warp
    // duration, so release any carried bomb up front to match — the "warp/bounce
    // while carrying" illegal-in-our-port-only combination the state-machine audit
    // flagged (facts.md "Player state machine (+78) — COMPLETE").
    if (stage.bouncing(p)) {
        if (p.carrying) bombs.throw_carried(p, i);
        stage.tick_bounce(p, i);
        p.prev_action1 = in.action1;
        p.prev_action2 = in.action2;
        return;
    }

    // A warp is likewise state-gated (player states 6=warp-out, 7=warp-in): the
    // original ignores movement/input and makes the player invulnerable for the
    // whole 18-tick warp, relocating it to the exit at the out→in midpoint. Tick
    // it down (which performs the midpoint relocation) and skip the turn. This is
    // the fix for the "stuck on entering a warp" report: the prior instantaneous
    // teleport skipped these phases; now the player warps and, once warp==0, moves
    // again. See docs/re/stage-actors.md §5. Carried-bomb release: see the bounce
    // branch above (same LABEL_246 fall-through argument applies to states 6/7).
    if (stage.warping(p)) {
        if (p.carrying) bombs.throw_carried(p, i);
        stage.tick_warp(p);
        p.prev_action1 = in.action1;
        p.prev_action2 = in.action2;
        return;
    }

    // Input decode -> want_godir (0=Up,1=Right,2=Down,3=Left, -1 = none). A
    // stunned player acquires NO new direction: the v113 gate skips sub_41E61E,
    // so its +46 stays at the per-tick -1 reset. Force want_godir = -1 and skip
    // the whole opposite-key / reversed-disease decode for it — the mover then
    // takes the idle branch (stage actors still drive it), never a keyed one.
    int want_godir = -1;
    if (!stunned) {
        const bool up = in.up, down = in.down, left = in.left, right = in.right;

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
        for (int g = 0; g < 4; ++g)
            if (dir[g]) want_godir = g;

        // Reversed-controls disease (sub_41F29B ~23049): applied to the RESOLVED
        // godir — `(g + 2) & 3` — AFTER the opposite-key filter ran on the RAW
        // pressed dirs, and BEFORE the ice buffer (the delayed samples store the
        // reversed value). Humans only: the `+16 != 1` gate exempts computer
        // players, whose chosen direction reaches the mover unflipped. The old
        // port swapped the input flags pre-resolution, which fed the passability
        // filter the flipped dirs — divergent under multi-key input.
        if (want_godir >= 0 && p.sick(Disease::Reversed) && !p.ai)
            want_godir = (want_godir + 2) & 3;
    }

    // Ice / input-lag (Hockey Rink, VALUELST ids 450-460; docs/re/facts.md
    // "Ice / input-lag", sub_41F29B ~23058-23078): replaces this tick's
    // resolved direction with a delayed sample from the player's own
    // history for HUMAN players on that level. A faithful no-op everywhere
    // else (returns want_godir unchanged, touches no state) — see
    // MovementSystem::ice_delay's doc comment.
    const int eff_godir = movement.ice_delay(p, want_godir);
    bool moving = eff_godir >= 0;

    // Movement, with any conveyor under the player folded in: a belt speeds/
    // slows a walking player and pushes a standing one along its direction
    // (StageActorSystem::move_on_actor, port of sub_41F29B's actor branches).
    // The belt-only push (no input) is handled inside move_on_actor.
    //
    // The per-pixel field callback is the port of sub_41EC84's post-commit
    // tail (pseudo.c 22699-22717): flame death then pickup at every pixel
    // step. A mid-move kill abandons the rest of the budget AND the rest of
    // this turn (the original returns straight into the death branch,
    // skipping the kick probe, the step-on latches and LABEL_246's bomb
    // actions). A mid-move pickup is usable the SAME tick — it lands before
    // the bomb-action block below. docs/re/facts.md "Per-tick call order —
    // END-TO-END" finding 1.
    {
        FieldCtx fctx{&s, i, &powerups, &diseases};
        stage.move_on_actor(p, eff_godir, moving, &on_move_pixel, &fctx);
        if (!p.alive) {
            p.prev_action1 = in.action1;
            p.prev_action2 = in.action2;
            return;
        }
        // Kick probe (sub_41EC84 `!v35` branch): the kick fires whenever the
        // player sits EXACTLY on the tile centre along the travel axis with a
        // bomb directly ahead — evaluated inside the pixel loop, so a player
        // WALKING into a bomb kicks it on the ARRIVAL tick (the mover pins
        // them at the centre; the same-tick loop iteration with v35 == 0
        // dispatches sub_424708), not one tick later as the old stall gate
        // did. Post-move "centred along the axis" is the same predicate: a
        // blocked player cannot end the tick anywhere else, and a re-probe
        // while parked matches the original's every-iteration re-kick (a
        // same-direction re-kick is a silent no-op in try_kick). The belt-
        // forced case (no input) probes along the belt — the original's mover
        // runs identically there with +46 = the belt dir. facts.md
        // "Core-feel audit" §1.
        int probe = moving ? eff_godir : -1;
        if (!moving) {
            const int ptx = p.tile_x(), pty = p.tile_y();
            if (grid::in_grid(ptx, pty) && s.actor_type[pty][ptx] == ActorType::Conveyor)
                probe = s.actor_dir[pty][ptx];
        }
        if (probe >= 0) {
            const int px = static_cast<int>(p.x / kScale), py = static_cast<int>(p.y / kScale);
            const int sx = ((px % kTileW) + kTileW) % kTileW - kTileW / 2;
            const int sy = ((py % kTileH) + kTileH) % kTileH - kTileH / 2;
            if (sx * DX[probe] + sy * DY[probe] == 0) bombs.try_kick(p, kGodir[probe], i);
        }
    }
    // A settle on a trampoline centre launches an in-place hop; a settle on a
    // warphole centre teleports to the linked exit (no RNG). Both use a one-shot
    // latch (cleared on leaving the tile) so a player parked on the tile fires
    // exactly once, mirroring the original's centring trigger (v35 == -1).
    stage.trampoline_after_move(p, i);
    stage.warphole_after_move(p, i);

    // The bomb key (action1) drives THREE independent blocks, in the original's
    // exact order (sub_41F29B LABEL_246). We mirror the byte semantics: +56 =
    // "bomb key down this frame", +54 = "…last frame"; the drop block is edge-
    // gated on `+56 && !+54`; +37 = a carried (grabbed) bomb.
    //
    // SKIPPED entirely while `stunned` (EITHER p.stun>0 head-hit OR
    // p.pickup_pause>0 grab-pause — see the field split above): the v113 gate
    // that skips sub_41E61E leaves the bomb-key bytes +56/+57 at their per-tick
    // 0 reset, so every edge-gated action (manual drop / throw / grab / spooge /
    // punch / trigger / kick-stop) is un-triggered — the same as the prior full
    // early-return did, so this is inert on the golden and on the grab-pickup-
    // pause tests (which pin the carried bomb as thrown only AFTER the pause;
    // see test_diarrhea_throw.cpp). The original's disease auto-drop and
    // carried-bomb throw, which ride the disease flags / a released key rather
    // than a NEW key press, do keep firing during a head-hit stun in sub_41F29B
    // (LABEL_246 is reached with +56=0, states 1/2/3/4/20-39 all fall through to
    // it); reproducing that narrow edge for the STANDING (non-bounce/warp) case
    // is deliberately deferred — it never fires in any current scenario/test.
    // The bounce/warp case of the SAME LABEL_246-always-runs fact IS fixed (see
    // the `if (p.carrying) bombs.throw_carried(...)` calls above the early
    // returns) because it was a clean, narrowly-scoped state-transition guard;
    // this standing-stun case is a broader reordering of the whole block and
    // stays out of scope here. facts.md "Player state machine (+78) — COMPLETE".
    //
    // (1) Auto-drop diseases (diarrhea +135 / super +137): the original FORCES an
    //     edge every frame — `+56 = 1; +54 = 0; v112 = 1` — so the drop block
    //     below fires each tick. We reproduce that by overriding the effective
    //     key state under auto-drop. v112 also unconditionally releases a carried
    //     bomb (block 2) and suppresses the spooger (block 4).
    if (!stunned) {
        const bool auto_drop = p.sick(Disease::Diarrhea) || p.sick(Disease::Super);
        const bool a1_now = auto_drop ? true : in.action1;        // +56
        const bool a1_last = auto_drop ? false : p.prev_action1;  // +54
        const bool drop_edge = a1_now && !a1_last;                // the +56 && !+54 gate

        // (2) Throw block (`+37`): a carried bomb is thrown when auto-drop forces it
        //     (v112) OR the key is released (`!+56`). NOT gated by constipation — a
        //     constipated player can still throw what it holds. This is why diarrhea
        //     + grab THROWS serially: v112 fires the throw every frame, then the drop
        //     block (below) re-grabs the next bomb underfoot — the original's loop.
        if (p.carrying) {
            if (auto_drop || !a1_now) bombs.throw_carried(p, i);
        }
        // (3) Action key (action2): stop own sliding bombs (+89 kick), punch the
        //     bomb ahead (+91), and/or detonate a trigger bomb (+95) — three
        //     independent ifs in sub_41F29B's exact order, all edge-gated
        //     (`+57 && !+55`). The KICK-flag handler (sub_4247C5) flags every one
        //     of the player's sliding non-jelly bombs to halt at the next tile
        //     centre. PUNCH additionally requires `!+56` (the bomb key not down)
        //     so it never swings mid-drop / mid-auto-drop; TRIGGER has no such
        //     gate. Grab/throw live on action1. This block runs BEFORE the drop
        //     block (4), matching LABEL_246's order.
        if (in.action2 && !p.prev_action2) {
            if (p.kick) bombs.stop_own_sliding(i);
            if (p.punch && !a1_now) bombs.try_punch(p, static_cast<std::uint8_t>(i));
            if (p.trigger) bombs.detonate_triggered(i);
        }
        // (4) Drop block (`+56 && !+54 && !+134`): constipation (+134) blocks it. On
        //     the edge, GRAB the OWN bomb underfoot (+92), else spray a SPOOGER
        //     line (+93, suppressed while auto-dropping — `!v112`; also own-bomb
        //     gated), else DROP. Both gates are sub_422E48 + `owner == self`
        //     (motion 0 AND 1 qualify — a player can grab their own bomb mid-
        //     slide; flying/carried are exempt via bomb_at). Grab/spooger are the
        //     "double-tap": press 1 drops a bomb underfoot, press 2 grabs/sprays.
        if (drop_edge && !p.sick(Disease::Constipation)) {
            const Bomb* under = grid::bomb_at(s, p.tile_x(), p.tile_y());
            const bool own = under && under->owner == static_cast<std::uint8_t>(i);
            if (p.grab && own)
                bombs.try_grab(p, i);
            else if (p.spooge && !auto_drop && own)
                bombs.spooge_ahead(p, static_cast<std::uint8_t>(i));
            else
                bombs.drop(p, static_cast<std::uint8_t>(i));
        }
    }
    p.prev_action1 = in.action1;
    p.prev_action2 = in.action2;
}

// The head checks: flames kill players; floor powerups get picked up (with
// the pre-pickup cure roll and the skull dispatch). This is the original's
// NEXT-turn head pair (sub_41F29B 22915-22926) at our rotation's cut point —
// it runs for every player (moved or not), in slot order, after the walls.
void field_vs_players(State& s, PowerupSystem& powerups, DiseaseSystem& diseases) {
    for (int i = 0; i < kMaxPlayers; ++i) resolve_player_field(s, i, powerups, diseases);
}

void run_tick(State& s, const TickInputs& inputs) {
    s.events.clear();

    // Systems are cheap stack objects wired to the shared state; their
    // construction order is irrelevant, the CALL order below is not.
    DiseaseSystem diseases{s};
    PowerupSystem powerups{s};
    FlameSystem flames{s, powerups};
    BombSystem bombs{s, flames, powerups};
    MovementSystem movement{s};
    StageActorSystem stage{s, movement};
    EnclosureSystem enclosure{s, flames};
    TileRegenSystem tile_regen{s};
    AISystem ai{s};
    RoverSystem rovers{s};

    // 1. Players: movement (with conveyor/trampoline actors), bomb drop,
    //    throw/grab/trigger/punch. The conveyor push is part of the move budget
    //    and the trampoline hop is triggered on settling, so both live inside
    //    the player turn (mirroring sub_41F29B, which does movement + the actor
    //    branches in one pass). No new tick step: the actor effects are folded
    //    into step 1 exactly where the original applies them.
    for (int i = 0; i < kMaxPlayers; ++i) {
        Player& p = s.players[i];
        if (!p.present || !p.alive) continue;
        // A computer player resolves its own input here (ADR-0005 §5): AISystem
        // runs at the TOP of the loop, immediately before this player's
        // player_turn — the exact slot where the original calls sub_40A1C6
        // instead of reading DirectInput (sub_41F29B, gated on the +16==1 tag).
        // This keeps the AI's RNG draws interleaved with movement in slot order,
        // as the original interleaves the brain and the mover per player. Humans
        // and replays pass their externally-supplied input through unchanged.
        // No new tick STEP: this is a refinement of step 1 only, so no golden
        // step-order dependency shifts (the steps below are untouched).
        //
        // The original gates the WHOLE dispatch (including draws A/B) on
        // `v113 && !dword_4621E0` (sub_41F29B ~23028), where v113 is cleared by
        // a head-hit stun (`+58 > 0`, ~22982), a grab's pickup-pause (state
        // +78==4 within its getvalue(665) window, ~23017-23025; see
        // Player::pickup_pause), OR the flight states 5/6/7 (`if (v111[39] ==
        // 6 || == 7 || == 5) v113 = 0`, ~23015-23016) — as well as the
        // entering/dying/dead modes -- present/alive above already covers
        // dying/dead. An AI blocked by ANY of these must draw NOTHING this
        // tick, the same as the original skipping the call outright (RESOLVED,
        // docs/re/ai.md §2/§7; extended to pickup_pause + bounce/warp
        // 2026-07-11 with the state-machine audit — facts.md "Player state
        // machine (+78) — COMPLETE"). player_turn also no-ops a blocked
        // player's INPUT (it forces want_godir = -1 and skips the bomb-action
        // block; bounce/warp early-return outright) while STILL running the
        // idle mover, so skipping ai.decide here changes only the RNG stream,
        // never gameplay -- but that stream is the whole contract.
        PlayerInput in = inputs.players[i];
        if (p.ai && p.stun == 0 && p.pickup_pause == 0 && p.bounce == 0 && p.warp == 0)
            ai.decide(i, in);
        player_turn(s, i, in, bombs, stage, movement, powerups, diseases);
    }

    // 2. Campaign rover/ghost hazards: drive the mover 1 tick (spawn/wander/
    // flame-death/landing-tile kill) and the "all hazards dead" grace timer
    // (docs/re/campaign.md "Round pacing", sub_4016DA). The original calls
    // sub_401F76 from the campaign callback IMMEDIATELY AFTER the player pass
    // (sub_42A191 29527 -> 29528-29529), BEFORE the next frame's bomb pass —
    // so a rover never reacts to flames lit later in the same gap
    // (docs/re/facts.md "Per-tick call order — END-TO-END" finding 3). No-op
    // (zero RNG draws, zero cost) when s.rovers is empty — see
    // RoverSystem::tick.
    rovers.tick();

    // 3. Match clock. The original updates it at the top of the frame
    // (sub_4105D2, sub_42A191 29518) — after the rovers, before the bomb
    // pass, in rotation terms — and the enclosure below reads the value
    // updated this same gap.
    if (s.ticks_left > 0 && --s.ticks_left == 0)
        s.events.push_back({Event::Type::TimeUp, -1, -1, -1, 0});

    // 4. Drain the chain-detonation queue (docs/re/facts.md "Chain-reaction
    // timing", sub_423209/dword_462200): a flame arm that reached another
    // bomb, a trigger-button press, a flying bomb landing on flame, or a
    // sliding bomb entering flame all QUEUE their target instead of
    // exploding it synchronously. The original drains this queue once, at
    // the very top of its per-frame bomb pass (`sub_42331C`'s
    // `dword_462210 != dword_464994` guard) — the first bomb phase after
    // the player pass in the rotated stream — so a trigger-button press
    // (queued during step 1, above) is caught by THIS drain with no player
    // move in between, while a flame-arm/slide/landing hit (queued during
    // bombs.advance_bombs / tick_fuses, below — i.e. during the original's
    // per-bomb-slot loop, which runs AFTER its own drain already fired) is
    // only caught by the NEXT tick's drain — one chain LINK per tick, not
    // the whole chain at once.
    flames.drain_chain_queue();

    // 5. Kicked bombs slide; airborne bombs fly.
    bombs.advance_bombs();

    // 6. Fuses (paused while a bomb is airborne).
    bombs.tick_fuses();

    // NOTE (documented deviation, facts.md "Per-tick call order" accepted
    // deviations): the original interleaves steps 5/6 PER BOMB SLOT — slot
    // k's slide runs after slot j<k's explosion within the same frame. Our
    // phase split makes same-tick bomb-vs-bomb coincidences uniformly
    // "movement first"; matching the original exactly would require the
    // 100-slot first-fit allocator.

    // 7. Flames fade, bricks finish crumbling (revealing powerups). The
    // original ages the grid right after its bomb pass (sub_426D06,
    // sub_42A191 29525) — a flame lit this tick ages once this tick.
    flames.age_flames_and_bricks();

    // 8. Walls closing in during the hurry phase, AFTER the flame aging and
    // BEFORE the head checks (sub_426818 at 29526: after sub_426D06, before
    // sub_420F07) — a wall crush beats a same-tick flame kill (no killer
    // credit) and destroys an un-picked-up token under the dropping wall.
    // Tile regeneration runs immediately before the wall stepper, mirroring
    // the original: sub_426704 (regen) is called from WITHIN sub_426818,
    // right before its own arm/disarm/drop logic (docs/re/facts.md
    // "Per-level tile regeneration"). A no-op on every level but Haunted
    // House.
    tile_regen.update();
    enclosure.update();

    // 9. The head checks: flames kill players, floor powerups get picked up —
    // the original's NEXT player pass's turn-head pair (sub_41F29B
    // 22915-22926) at this rotation's cut point.
    field_vs_players(s, powerups, diseases);

    // 10. Diseases: spread on contact, age the freshness gate, and expire.
    // The original runs these right after the head checks inside each
    // player's turn (sub_41F29B 22927-22975).
    diseases.spread_and_age();

    // 11. Compact dead bombs (stable order — deterministic).
    s.bombs.erase(
        std::remove_if(s.bombs.begin(), s.bombs.end(), [](const Bomb& b) { return !b.active; }),
        s.bombs.end());

    ++s.tick;
}

}  // namespace

Simulation::Simulation(const MatchConfig& config) : state_(detail::build_state(config)) {}

void Simulation::tick(const TickInputs& inputs) {
    run_tick(state_, inputs);
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
