#pragma once

#include <array>
#include <cstdint>

#include "bomber/sim/constants.hpp"
#include "bomber/sim/types.hpp"

namespace bomber::sim {

// One player's complete gameplay state. Deliberately a plain aggregate:
// every field is part of the deterministic state and must be covered by
// state_hash() (see CLAUDE.md "Determinism contract").
struct Player {
    bool present = false;
    bool alive = false;
    Fixed x = 0, y = 0;  // center position in field pixels * 100
    Direction facing = Direction::Down;
    std::int32_t speed = 0;       // movement budget added per tick (VALUELST id 42 + skates)
    std::int32_t move_budget = 0; // carried sub-pixel budget; spent 100 per 1px step (sub_41EC84)
    std::int32_t max_bombs = 1;
    std::int32_t bombs_placed = 0;
    std::int32_t flame = 2;       // cells beyond the epicenter
    bool goldflame = false;       // Goldflame flag (+94): reach = max(gridW,gridH) at drop time
    std::int32_t skates = 0;
    bool kick = false;
    bool punch = false;
    bool grab = false;
    bool spooge = false;          // lays a line of bombs ahead (Spooger powerup)
    bool trigger = false;
    // Live trigger-bomb allowance (player byte +85, sub_41EB13/sub_41E21E): a
    // Trigger pickup resets this to 0; each placed trigger bomb increments it
    // and it is capped at max_bombs. Never decremented (not on detonation), so
    // once exhausted further placements fall back to normal timed bombs until
    // the next Trigger pickup refills the budget.
    std::int32_t trigger_placed = 0;
    std::int32_t stun = 0;        // ticks of enforced pause (bomb pickup, head hits)
    // Trampoline hop (player state +78==5, sub_41F29B state 5 / sub_41DE63): a
    // step onto a trampoline launches a FLIGHT lasting tuning
    // .trampoline_bounce_frames ticks (VALUELST id 680 = 30). During the flight
    // movement input is ignored and the player is invulnerable to being pushed;
    // at the APEX (frame count == 680/2 == 15) it is TELEPORTED to a random nearby
    // open tile (StageActorSystem::tick_bounce, the confirmed 0x4203a7 loop). This
    // is a countdown (30→0); the apex fires once at bounce == 15. Hashed. §4.
    std::int32_t bounce = 0;
    // Trampoline one-shot latch: set when a bounce fires, cleared once the player
    // leaves the trampoline tile — so a player parked on the centre bounces once,
    // not every tick (the original re-fires only on the stepper's centring).
    bool tramp_latch = false;
    // Warphole two-phase warp (player states 6=warp-out, 7=warp-in in the
    // original, sub_41F29B ~23155/23215; step-on in sub_41EC84 ~22590). CONFIRMED
    // timing: warp-out animates until its frame counter passes 8 (9 ticks), then
    // the player is relocated to the linked exit and enters warp-in, which also
    // runs 9 ticks before returning to normal. So the whole warp is 18 ticks,
    // during which the player is state-gated (no movement/input) and invulnerable
    // to being pushed (sub_41DE63 returns 0 for states 6/7). This countdown drives
    // that: kWarpTicks..(kWarpTicks/2+1) = warp-out, then relocate at the midpoint,
    // then (kWarpTicks/2)..1 = warp-in. No RNG — the exit is pre-resolved into
    // State::warp_dest_* at setup. Hashed (it gates movement every active tick).
    // Replaces the prior instantaneous-teleport model, which left the player
    // stuck by never running the out/in phases the renderer expects. §5.
    std::int32_t warp = 0;
    // Pending warp destination tile, captured at step-on (original +20/+24,
    // stored by sub_41EC84 right when it sets warp state 6). tick_warp relocates
    // the player HERE at the out→in midpoint. Capturing at step-on (not re-
    // reading warp_dest at the midpoint) is faithful and robust: the original's
    // per-pixel loop can slide the player a few px OFF the warphole within the
    // trigger tick, so a midpoint tile lookup would miss the entry tile. Hashed
    // (part of the in-flight warp). Stays 0 until the first warp and retains the
    // last dest afterward — always 0 on boards with no warpholes, so it does not
    // perturb the golden (no-actor) scenarios. See stage-actors.md §5.
    std::int32_t warp_to_x = 0, warp_to_y = 0;
    // Warphole one-shot latch (docs/re/stage-actors.md §5). Set when a warp
    // STARTS; while set the player will NOT re-warp, and it clears the moment the
    // player is no longer centred on a warphole tile (the exit is itself a
    // warphole, so without this it would ping-pong). Together with `warp` this
    // gives exactly one warp per entry.
    bool warp_latch = false;
    // Carried bomb (picked up with the grab glove); its fuse is frozen.
    bool carrying = false;
    std::int32_t carried_fuse = 0, carried_flame = 2;
    bool carried_jelly = false, carried_trigger = false;
    std::uint8_t carried_owner = 0;
    bool jelly = false;
    bool prev_action1 = false;    // for edge detection (part of state!)
    bool prev_action2 = false;
    // Diseases: per-kind active flags sharing one countdown; a healthy player
    // has disease_timer == 0. disease_fresh gates re-spreading (VALUELST id 129).
    std::array<bool, kDiseaseKinds> disease{};
    std::int32_t disease_timer = 0;
    std::int32_t disease_fresh = 0;

    int tile_x() const { return static_cast<int>(x / kTileWF); }
    int tile_y() const { return static_cast<int>(y / kTileHF); }
    bool sick(Disease d) const { return disease[static_cast<int>(d)]; }
};

}  // namespace bomber::sim
