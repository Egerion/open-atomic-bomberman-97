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
//
// `ai_sys` is non-null for a computer player: the brain re-decides once per
// canonical SUB-FRAME inside the movement loop below (the exact slot where
// the original calls sub_40A1C6 instead of reading DirectInput, once per
// displayed frame — docs/re/facts.md "Canonical frame cadence"). Humans and
// replays pass their externally-supplied tick input through unchanged.
void player_turn(State& s, int i, const PlayerInput& tick_in, AISystem* ai_sys, BombSystem& bombs,
                 StageActorSystem& stage, MovementSystem& movement, PowerupSystem& powerups,
                 DiseaseSystem& diseases) {
    Player& p = s.players[i];

    // Spawn-tile brick clear (sub_41E61E case 4, batch_0x41DAA7.cpp:660-664, and
    // its AI-path equivalent): a player occupying a Brick tile clears it to
    // Blank. Movement is brick-blocked, so a player never steps ONTO a brick
    // during normal play -- this only ever fires at spawn, opening the single
    // spawn tile that a dense scheme's random fill leaves bricked (~90% of the
    // time; the original places a brick there and does NOT clear a pocket --
    // facts.md "Spawn-pocket clear", proven 2026-07-21 by a native fill probe).
    // RADIUS-0: only the occupied tile, NOT a pocket. Verified against the
    // native (aispawn oracle scenario): the spawn brick clears on tick 1 with
    // no bomb, and a boxed-in AI then stalls (behave_walk_path's "nowhere safe
    // to step" branch returns 1, blocking blast-bricks) instead of self-killing
    // -- which is why no wider pocket is needed. Draws no RNG.
    {
        const int ptx = p.tile_x(), pty = p.tile_y();
        if (ptx >= 0 && ptx < kGridWidth && pty >= 0 && pty < kGridHeight &&
            s.cells[pty][ptx] == Cell::Brick)
            s.cells[pty][ptx] = Cell::Blank;
    }

    // The tick's EFFECTIVE input for the bomb-action tail: a human's sample
    // as-is; for an AI, the action-key edges are OR-accumulated from its
    // per-sub-frame decisions in the loop below (the original consumes +56/
    // +57 in the same frame's LABEL_246; our tail runs once per tick, so a
    // press in any sub-frame must survive to it).
    PlayerInput in = tick_in;

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
    // The +58 head-stun countdown decrements once per DISPLAYED frame in the
    // original (22982-22984 runs at the top of every sub_41F29B call, before
    // the state dispatch) — so it lives in the sub-frame loop below (and in
    // the bounce/warp branches, whose frames still execute 22982): a 16-frame
    // stun lasts ~267 ms at the canonical 60 fps, not 800 ms. pickup_pause
    // mirrors the state-4 +80 window, which advances on the 50 ms
    // ms-accumulator like every anim counter — per tick, and (like the old
    // shared-stun code) it blocks the WHOLE tick it is decremented on: gate
    // on the pre-decrement value.
    const bool paused = p.pickup_pause > 0;
    if (p.pickup_pause > 0) --p.pickup_pause;

    // The four LABEL_246 blocks (sub_41F29B 23277-23380), in the original's
    // exact order: auto-drop force (diarrhea +135 / super +137) -> carried-
    // bomb throw (+37) -> action2 edge (kick-stop +89 / punch +91 / trigger
    // +95) -> drop edge (grab +92 / spooge +93 / plain drop), gated !+134.
    // CONFIRMED (facts.md "Player state machine (+78) — COMPLETE" and
    // "Diarrhea/super auto-drop x grab-glove"): LABEL_246 runs on EVERY alive
    // tick regardless of the +78 state — state 5 (bounce) reaches it via an
    // explicit `goto LABEL_246` (23198), states 6/7 (warp out/in) and 20-39
    // fall through LABEL_239 into it, and nothing in the v113 gate (the ONE
    // thing a head-hit stun / grab pause / bounce / warp actually blocks)
    // touches LABEL_246 itself — v113 only guards the EARLIER new-input
    // acquisition call that would set the raw key bytes +56/+57 from the
    // controller. `blocked` mirrors that: while blocked, this tick's
    // effective key bytes start at their per-tick reset of 0 (22976-22979,
    // which the original runs unconditionally every alive tick, so a fresh
    // edge NEVER materialises while acquisition is skipped) UNLESS the
    // auto-drop disease force overrides +56=1 below (that override lives
    // INSIDE LABEL_246 itself, so it fires regardless of `blocked`). This is
    // why a carried bomb is released on the very first blocked tick (`!+56`
    // reads true immediately — the release-throw the user can trigger by
    // walking into a head-stun while carrying) and why the diarrhea/super
    // auto-drop keeps cycling grab/throw/drop straight through a stun or a
    // bounce/warp flight, while a genuine NEW manual action (kick-stop/punch/
    // trigger/drop) cannot fire — its edge needs +56 or +57 actually freshly
    // DOWN, which requires the input acquisition that `blocked` skips.
    // pickup_pause (+78==4's own window) is deliberately NOT modelled here:
    // the original forces +56=1 SUSTAINED (not a fresh edge) for that specific
    // state instead of leaving it at 0 (23017-23025) — a materially different
    // "held" rule from the zero-default `blocked` models below. Every call
    // site therefore still fully SKIPS `bomb_actions` while p.pickup_pause >
    // 0 (matching the pre-existing full-skip for that case exactly — see the
    // pickup-pause doc comment on Player::pickup_pause) and only routes
    // through `bomb_actions` for p.stun > 0 / bounce / warp, which this pass
    // DOES fix.
    //
    // p.prev_action1/2 (the original's +54/+55) are updated HERE, to the
    // EFFECTIVE key values just used (post auto-drop-force, post blocked-
    // zeroing) — not the raw controller input — mirroring the original's
    // literal `+54 = +56` copy at the top of the NEXT tick. This also fixes a
    // latent divergence: the previous port latched the RAW `in.action1/2`
    // unconditionally, which only matched the original whenever auto-drop
    // was inactive (auto-drop's own in-block override made the raw-vs-
    // effective distinction inert while the disease stayed active; it can
    // diverge on the tick a disease is cured with an un-pressed button, which
    // no scenario/test currently exercises — see facts.md).
    auto bomb_actions = [&](bool blocked) {
        const bool auto_drop = p.sick(Disease::Diarrhea) || p.sick(Disease::Super);
        const bool a1_now = auto_drop ? true : (blocked ? false : in.action1);   // +56
        const bool a1_last = auto_drop ? false : p.prev_action1;                 // +54
        const bool a2_now = blocked ? false : in.action2;                        // +57
        const bool a2_last = p.prev_action2;                                     // +55
        const bool drop_edge = a1_now && !a1_last;

        // (2) Throw block (`+37`): a carried bomb is thrown when auto-drop forces
        //     it (v112) OR the key is released (`!+56`) — not gated by
        //     constipation. While blocked (and not auto-dropping) `a1_now` is
        //     always false, so this fires on the FIRST blocked tick.
        if (p.carrying) {
            if (auto_drop || !a1_now) bombs.throw_carried(p, i);
        }
        // (3) Action2 (`+57 && !+55`): stop own sliding bombs, punch, trigger-
        //     detonate. `a2_now` is forced false while blocked, so this never
        //     fires without a fresh real key press.
        if (a2_now && !a2_last) {
            if (p.kick) bombs.stop_own_sliding(i);
            if (p.punch && !a1_now) bombs.try_punch(p, static_cast<std::uint8_t>(i));
            if (p.trigger) bombs.detonate_triggered(i);
        }
        // (4) Drop block (`+56 && !+54 && !+134`): grab own bomb underfoot, else
        //     spooger line (suppressed under auto-drop), else plain drop. Only
        //     THIS block is gated by constipation. While blocked, only the
        //     auto-drop-forced edge can reach it.
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
        p.prev_action1 = a1_now;
        p.prev_action2 = a2_now;
    };

    // A trampoline hop is a state-gated flight (sub_41F29B state 5 / sub_41DE63):
    // movement input and bomb actions are ignored until the hop finishes, and the
    // player cannot be pushed. tick_bounce ticks it down AND, at the apex, teleports
    // the player to a random nearby open tile (the "fly + random land" — it is NOT
    // an in-place bounce; see docs/re/stage-actors.md §4). The apex relocation draws
    // RNG, so it runs here inside the state gate, before any other per-tick draw.
    //
    // CONFIRMED (pseudo.c ~23198/23248, the v86==5 branch's `goto LABEL_246` and
    // the fall-through from states 6/7 into the same label): the original's
    // LABEL_246 — all four blocks, not just the carried-bomb throw-release check
    // — is NOT skipped during a bounce or warp; it runs every tick regardless of
    // +78. Because input is fully blocked the whole time (+56 stays 0, never
    // re-set, unless auto-drop forces it), `!+56` is true from the very first
    // tick, so a player who enters a bounce/warp WHILE CARRYING has the bomb
    // thrown at their current tile almost immediately, not held through the
    // whole flight — and a diarrhea/super auto-drop keeps grabbing/throwing/
    // dropping straight through the flight too. Movement/input are still fully
    // skipped for the whole bounce/warp duration (the "warp/bounce while
    // carrying" illegal-in-our-port-only combination the state-machine audit
    // flagged, facts.md "Player state machine (+78) — COMPLETE") — only the
    // bomb-action block runs, via the shared `bomb_actions` above. If the flight
    // was entered mid pickup-pause (a conveyor-carried grab pushed onto a
    // trampoline/warphole — vanishingly rare, no current scenario reaches it),
    // `bomb_actions` stays fully skipped per its pickup_pause carve-out above;
    // preserve the pre-existing narrow release-on-entry fix (main's illegal-
    // combo guard) for exactly that case so a carried bomb still doesn't ride
    // through the flight untouched.
    if (stage.bouncing(p)) {
        // 22982 (--+58) and the ice block (23058-23078) both sit ABOVE the
        // state dispatch, so a flight frame still burns stun AND pushes this
        // frame's -1 godir into the ice buffer — landing on an icy level then
        // replays neutral input, not a stale pre-flight direction burst
        // (facts.md "Ice / input-lag", flight-push fix 2026-07-12).
        for (int f = 0; f < kSubFrames; ++f) {
            if (p.stun > 0) --p.stun;
            (void)movement.ice_delay(p, -1);
        }
        if (p.pickup_pause > 0) {
            if (p.carrying) bombs.throw_carried(p, i);
            p.prev_action1 = in.action1;
            p.prev_action2 = in.action2;
        } else {
            bomb_actions(/*blocked=*/true);
        }
        stage.tick_bounce(p, i);
        return;
    }

    // A warp is likewise state-gated (player states 6=warp-out, 7=warp-in): the
    // original ignores movement/input and makes the player invulnerable for the
    // whole 18-tick warp, relocating it to the exit at the out→in midpoint. Tick
    // it down (which performs the midpoint relocation) and skip the turn. This is
    // the fix for the "stuck on entering a warp" report: the prior instantaneous
    // teleport skipped these phases; now the player warps and, once warp==0, moves
    // again. See docs/re/stage-actors.md §5. Bomb actions: see the bounce branch
    // above (same LABEL_246 fall-through argument, incl. the pickup-pause carve-
    // out, applies to states 6/7).
    if (stage.warping(p)) {
        // Same per-frame +58 + ice-buffer note as the bounce branch above.
        for (int f = 0; f < kSubFrames; ++f) {
            if (p.stun > 0) --p.stun;
            (void)movement.ice_delay(p, -1);
        }
        if (p.pickup_pause > 0) {
            if (p.carrying) bombs.throw_carried(p, i);
            p.prev_action1 = in.action1;
            p.prev_action2 = in.action2;
        } else {
            bomb_actions(/*blocked=*/true);
        }
        stage.tick_warp(p);
        return;
    }

    // ---- Canonical sub-frame loop (constants.hpp kSubFrames; docs/re/
    // facts.md "Canonical frame cadence"): the original runs input
    // acquisition, the AI brain and the movement-budget accrual once per
    // DISPLAYED frame, not per 50 ms tick. Each iteration below is one
    // canonical 60 fps frame: acquire (the AI re-decides — its whims and
    // timers run at frame rate), decode, push the ice buffer, accrue
    // frame_budget(speed, delta) and step the per-pixel mover. The
    // bomb-action tail (LABEL_246) stays once per tick after the loop: its
    // blocks are edge-gated, so the only cadence difference is the auto-drop
    // diseases' intra-tick attempt density (documented in the same entry).
    FieldCtx fctx{&s, i, &powerups, &diseases};
    // Round-start input freeze (dword_4621E0, sub_41F29B's `v113 &&
    // !dword_4621E0` gate at 23028): while it runs, the AI brain and the
    // human input read are BOTH skipped — same slot as the stun gate below,
    // but without consuming stun (their countdowns are independent). Stage
    // actors, the ice-buffer flow and the LABEL_246 tail (with its inputs
    // dead, so only auto-drop can act) all run normally underneath it.
    const bool frozen = s.input_freeze > 0;
    std::int32_t walk_budget = 0;  // summed accruals -> the PlayerWalking event
    for (int sub = 0; sub < kSubFrames; ++sub) {
        // +58 head-stun: gate first, then decrement — the original's
        // per-frame `if (+58 > 0) { v113 = 0; --+58; }` (22982-22984).
        const bool sub_stunned = p.stun > 0 || paused;
        if (p.stun > 0) --p.stun;
        // A flight entered in an earlier sub-frame (trampoline/warp step-on
        // mid-walk): the original's following frames take the state-5/6/7
        // branch — no acquisition, no movement — while +58 keeps counting and
        // the ice buffer keeps flowing (both sit above the state dispatch);
        // hence continue, not break.
        if (p.bounce > 0 || p.warp > 0) {
            (void)movement.ice_delay(p, -1);
            s.sub_trace[i][sub] = {p.x, p.y, p.facing};
            continue;
        }

        // Acquisition. A computer player's brain runs HERE, once per frame
        // (sub_40A1C6 in sub_41F29B's v113-gated slot — an AI blocked by
        // stun/pickup-pause must draw NOTHING this frame, same as the
        // original skipping the call outright); its action-key presses are
        // OR-latched into `in` for the once-per-tick tail below.
        PlayerInput sub_in = tick_in;
        if (ai_sys && !sub_stunned && !frozen) {
            ai_sys->decide(i, sub_in, kSubFrameMs[sub]);
            in.action1 = in.action1 || sub_in.action1;
            in.action2 = in.action2 || sub_in.action2;
        }

        // Input decode -> want_godir (0=Up,1=Right,2=Down,3=Left, -1 = none).
        // A stunned player acquires NO new direction: the v113 gate skips
        // sub_41E61E, so its +46 stays at the per-frame -1 reset. Force
        // want_godir = -1 and skip the whole opposite-key / reversed-disease
        // decode for it — the mover then takes the idle branch (stage actors
        // still drive it), never a keyed one.
        int want_godir = -1;
        if (!sub_stunned && !frozen) {
            const bool up = sub_in.up, down = sub_in.down, left = sub_in.left,
                       right = sub_in.right;

            // Opposite-key resolution, faithful to the original input decoder
            // (sub_41E61E, LABEL_58): collect the four direction flags in GODIR
            // order (0=Up,1=Right,2=Down,3=Left); if more than one is pressed and
            // at least one leads to an open tile, drop the pressed dirs that are
            // blocked; then the LAST surviving index wins. That last-index bias
            // (Left beats Right, Down beats Up) plus the per-pixel mover makes a
            // player held against a wall with two opposite keys vibrate in place —
            // flip facing every frame — which is the original's beloved "crazy
            // back-and-forth" (only vs a left wall for L+R or a bottom wall for
            // U+D; the other side just slides off). No RNG, no new hashed field,
            // but trajectories change → golden must be recaptured.
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

            // Reversed-controls disease (sub_41F29B ~23049): applied to the
            // RESOLVED godir — `(g + 2) & 3` — AFTER the opposite-key filter ran
            // on the RAW pressed dirs, and BEFORE the ice buffer (the delayed
            // samples store the reversed value). Humans only: the `+16 != 1` gate
            // exempts computer players, whose chosen direction reaches the mover
            // unflipped. The old port swapped the input flags pre-resolution,
            // which fed the passability filter the flipped dirs — divergent under
            // multi-key input.
            if (want_godir >= 0 && p.sick(Disease::Reversed) && !p.ai)
                want_godir = (want_godir + 2) & 3;
        }

        // Ice / input-lag (Hockey Rink, VALUELST ids 450-460; docs/re/facts.md
        // "Ice / input-lag", sub_41F29B ~23058-23078): replaces this frame's
        // resolved direction with a delayed sample from the player's own
        // history for HUMAN players on that level. A faithful no-op everywhere
        // else (returns want_godir unchanged, touches no state) — see
        // MovementSystem::ice_delay's doc comment.
        const int eff_godir = movement.ice_delay(p, want_godir);
        const bool moving = eff_godir >= 0;

        // Walk-state bookkeeping for the PlayerWalking event (emitted once
        // per tick after the loop — see its doc comment in event.hpp): sum
        // the same disease-scaled accrual MovementSystem::move folds in.
        if (moving) {
            Fixed add = frame_budget(p.speed, kSubFrameMs[sub]);
            if (p.sick(Disease::Slow)) add /= 3;
            if (p.sick(Disease::Fast) || p.sick(Disease::Super)) add = 3 * add / 2;
            walk_budget += add;
        }

        // Movement, with any conveyor under the player folded in: a belt
        // speeds/slows a walking player and pushes a standing one along its
        // direction (StageActorSystem::move_on_actor, port of sub_41F29B's
        // actor branches). The belt-only push (no input) is handled inside
        // move_on_actor.
        //
        // The per-pixel field callback is the port of sub_41EC84's post-commit
        // tail (pseudo.c 22699-22717): flame death then pickup at every pixel
        // step. A mid-move kill abandons the rest of the budget AND the rest
        // of this turn (the original returns straight into the death branch,
        // skipping the kick probe, the step-on latches and LABEL_246's bomb
        // actions). A mid-move pickup is usable the SAME tick — it lands
        // before the bomb-action block below. docs/re/facts.md "Per-tick call
        // order — END-TO-END" finding 1.
        stage.move_on_actor(p, eff_godir, moving, kSubFrameMs[sub], &on_move_pixel, &fctx);
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

        // Presentation sub-frame trace (State::sub_trace, derived output like
        // s.events): where this player ended THIS canonical frame — the
        // renderer plays these back so the original's per-frame micro-motion
        // (direction flips up to kSubFrames× per tick) survives to the screen
        // instead of being lerped away between tick endpoints.
        s.sub_trace[i][sub] = {p.x, p.y, p.facing};
    }

    // Presentation walk-state event (see PlayerWalking's doc comment in
    // event.hpp): the pose/leg-cycle keys off the walking DISPATCH, not off
    // displacement. Emitted once per tick with the tick's SUMMED per-frame
    // accruals in whole px, so the renderer's leg cycle advances by exactly
    // the budget the mover burned. Events are unhashed derived outputs — no
    // golden impact.
    if (walk_budget > 0) {
        s.events.push_back({Event::Type::PlayerWalking, static_cast<std::int8_t>(i),
                            static_cast<std::int8_t>(p.tile_x()),
                            static_cast<std::int8_t>(p.tile_y()),
                            static_cast<std::int8_t>(std::clamp<Fixed>(walk_budget / kScale, 1, 127))});
    }
    // A settle on a trampoline centre launches an in-place hop; a settle on a
    // warphole centre teleports to the linked exit (no RNG). Both use a one-shot
    // latch (cleared on leaving the tile) so a player parked on the tile fires
    // exactly once, mirroring the original's centring trigger (v35 == -1).
    stage.trampoline_after_move(p, i);
    stage.warphole_after_move(p, i);

    // The bomb key (action1) and action key (action2) drive the four LABEL_246
    // blocks (auto-drop force, throw, action2, drop) — see the shared
    // `bomb_actions` lambda defined above (with its full citation) for the exact
    // semantics. A head-hit stun (p.stun>0) maps to `blocked=true`: new-input
    // acquisition never ran this tick, so the key bytes are at their per-tick 0
    // reset (unless auto-drop overrides them), exactly matching the original's
    // LABEL_246 reached from ANY player state. p.pickup_pause>0 instead fully
    // skips `bomb_actions` (its own, different, forced-HELD rule — see the
    // carve-out comment above); the two never coincide entering this tail
    // (a fresh grab needs an input edge, which stun already blocks, and
    // PowerupSystem::head_hit clears pickup_pause the instant it sets stun).
    if (p.pickup_pause > 0) {
        p.prev_action1 = in.action1;
        p.prev_action2 = in.action2;
    } else {
        // The round-start freeze reaches LABEL_246 with the key bytes never
        // acquired (their per-frame 0 reset stands), exactly like a stun tick
        // — blocked=true reproduces that; auto-drop still overrides, as the
        // original's v112 force is computed at LABEL_246 itself, outside the
        // acquisition gate.
        bomb_actions(/*blocked=*/p.stun > 0 || frozen);
    }
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
    // Presentation sub-frame trace prefill (State::sub_trace): every sample
    // starts at the player's tick-entry position/facing; player_turn's
    // sub-frame loop overwrites sample f as it moves, and the endpoint
    // restamp at the bottom of this function pins the LAST sample to the
    // tick's true final position. Derived output — never hashed.
    for (int i = 0; i < kMaxPlayers; ++i) {
        const Player& pp = s.players[i];
        for (int f = 0; f < kSubFrames; ++f) s.sub_trace[i][f] = {pp.x, pp.y, pp.facing};
    }

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
        // A computer player resolves its own input INSIDE its player_turn
        // (ADR-0005 §5, updated by the canonical-frame-cadence port): the
        // brain re-decides once per sub-frame in the movement loop — the
        // exact slot where the original calls sub_40A1C6 instead of reading
        // DirectInput (sub_41F29B, gated on the +16==1 tag), once per
        // displayed frame. This keeps the AI's RNG draws interleaved with
        // movement in slot order, as the original interleaves the brain and
        // the mover per player.
        //
        // The original gates the dispatch (including draws A/B) on `v113 &&
        // !dword_4621E0` (sub_41F29B ~23028) — head-hit stun, pickup-pause,
        // and the flight states 5/6/7 all clear v113 (see player_turn's
        // sub-frame loop, which re-evaluates that gate per frame; a blocked
        // AI draws NOTHING that frame, the same as the original skipping the
        // call outright — RESOLVED, docs/re/ai.md §2/§7; facts.md "Player
        // state machine (+78) — COMPLETE"). present/alive above covers the
        // entering/dying/dead modes.
        player_turn(s, i, inputs.players[i], p.ai ? &ai : nullptr, bombs, stage, movement,
                    powerups, diseases);
    }

    // 1b. Round-start input freeze countdown (dword_4621E0: armed to 50ms ×
    //     getvalue(30) = 1000 ms by round init sub_4214BC, decremented by the
    //     measured frame delta at the top of the player-pass entry sub_420F07,
    //     pseudo.c 23642-23645; while nonzero player_turn's acquisition gate
    //     skips the AI brain and the human input decode). The original's gate
    //     opens on the frame at t >= 1000 ms — exactly 1000 ms of dead input.
    //     At tick granularity that boundary needs the decrement AFTER the
    //     pass (ticks 0..19 read 20..1 and stay frozen; tick 20 reads 0):
    //     decrementing before the pass would cut the window one tick short.
    //     docs/re/facts.md "Round-start input freeze".
    if (s.input_freeze > 0) --s.input_freeze;

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

    // bombs F1 (docs/re/audit/bombs.md finding 1; sub_42331C's per-bomb tail
    // gated on `sub_421969() > 1` at pseudo.c 25603 / batch_0x422DDD.cpp:799):
    // once a round is decided down to <= 1 alive SIDE, the original FREEZES
    // every still-armed bomb — the entire fuse-elapsed accrual, the timeout
    // explosion, and the nested flame-arm spread all sit behind that gate, so
    // no fuse counts down, no chain propagates, and no trigger press resolves
    // (the chain-queue drain's forced elapsed=duration write is inert without
    // the same gate) until the round transitions. sub_421969 returns a forced
    // 2 in campaign (dword_46489C), so the freeze is EXEMPT there
    // (rovers/ghosts are not players; the round does not end by elimination) —
    // mirrored by !s.campaign_hazards_active. sides_remaining() generalizes
    // free-for-all (team 0 = distinct sides) and team mode exactly like
    // dword_4621D4/dword_4621DC. Bomb MOVEMENT (advance_bombs, step 5 — the
    // switch cases BEFORE the 25603 gate) is NOT frozen; only the fuse/
    // explosion/chain tail is.
    const bool bombs_frozen = sides_remaining(s) <= 1 && !s.campaign_hazards_active;

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
    // the whole chain at once. Frozen once the round is decided (bombs F1).
    if (!bombs_frozen) flames.drain_chain_queue();

    // 5. Kicked bombs slide; airborne bombs fly. NOT frozen by bombs F1
    //    (movement is the original's switch cases, before the freeze gate).
    bombs.advance_bombs();

    // 6. Fuses (paused while a bomb is airborne). Frozen once the round is
    //    decided down to <= 1 alive side (bombs F1).
    if (!bombs_frozen) bombs.tick_fuses();

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

    // 12. Pin every player's LAST sub-frame sample to the tick's true
    // endpoint: post-loop relocations (trampoline apex teleport, warp
    // midpoint, head-hit scatter) move a player after its own sub-frame loop
    // finished, and the trace playback's final segment must land exactly on
    // the position the next tick starts from (the renderer's per-segment
    // snap threshold then renders such a relocation as a clean snap).
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
