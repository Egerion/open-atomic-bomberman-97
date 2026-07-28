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
    // Computer-controlled (the original's player-type byte +16 == 1; ADR-0005).
    // When set, AISystem fills this player's PlayerInput before player_turn;
    // otherwise the externally-supplied (human/replay) input passes through.
    // Hashed: a gameplay input source. Defaults false, so every existing test
    // and golden scenario leaves the AI path untaken. See docs/re/ai.md §7.
    bool ai = false;
    // Team id (the original's player byte +84, sub_4223E7/sub_422437; toggled
    // by 'T' on the PLAYER INPUT screen — docs/re/setup-screens.md). Copied
    // verbatim from MatchConfig::team[] at setup (setup.cpp). Hashed: it now
    // gates AI targeting (docs/re/ai.md §3.4/§5.3) and round-end (a gameplay
    // decision), so it is deterministic state, not just presentation config.
    // Convention (our semantics — not RE'd beyond the +84 byte's existence):
    // team is only meaningful when at least two ACTIVE players share the same
    // nonzero-or-zero value; a fully-distinct/all-zero roster (every existing
    // scenario) behaves exactly as before this field existed.
    std::uint8_t team = 0;
    Fixed x = 0, y = 0;  // center position in field pixels * 100
    Direction facing = Direction::Down;
    std::int32_t speed = 0;        // movement budget added per tick (VALUELST id 42 + skates)
    std::int32_t move_budget = 0;  // carried sub-pixel budget; spent 100 per 1px step (sub_41EC84)
    std::int32_t max_bombs = 1;
    std::int32_t bombs_placed = 0;
    std::int32_t flame = 2;  // cells beyond the epicenter
    bool goldflame = false;  // Goldflame flag (+94): reach = max(gridW,gridH) at drop time
    std::int32_t skates = 0;
    // Clogs (Goldman wheel booby prize, inventory slot 13, docs/re/
    // goldman-roulette.md §9): a speed-penalty count, the mirror image of
    // `skates`. Born-with only — there is no PowerupSystem::apply/remove
    // case (no normal-play pickup path exists for this kind, §9.2). Set at
    // setup.cpp from MatchConfig::born_with_clogs and folded into `speed`
    // there the same way skates folds in via PowerupSystem::apply.
    std::int32_t clogs = 0;
    bool kick = false;
    bool punch = false;
    bool grab = false;
    bool spooge = false;  // lays a line of bombs ahead (Spooger powerup)
    bool trigger = false;
    // Live trigger-bomb allowance (player byte +85, sub_41EB13/sub_41E21E): a
    // Trigger pickup resets this to 0; each placed trigger bomb increments it
    // and it is capped at max_bombs. Never decremented (not on detonation), so
    // once exhausted further placements fall back to normal timed bombs until
    // the next Trigger pickup refills the budget.
    std::int32_t trigger_placed = 0;
    // Head-hit stun (player WORD +58). sub_421F7E overwrites +58 with 16
    // unconditionally, sets the state word +78 to 3 (the cosmetic "stunned"
    // pose) and zeroes the anim counter +80. Gates
    // ONLY new-input acquisition (the mover keeps running; see simulation.cpp
    // player_turn). CONFIRMED a SEPARATE counter from the grab's pickup-pause
    // below (see `pickup_pause` doc comment) — the two write different fields
    // in sub_41F29B and neither resets the other, so a head-hit landing mid-
    // pickup-pause (or a fresh grab immediately after a stun ends) ticks both
    // down independently. Previously this one field did double duty for both
    // mechanisms (a real conflation bug: a grab used to clobber an in-progress
    // head-stun countdown down to `pickup_pause`, and vice versa) — split
    // 2026-07-11, see facts.md "Player state machine (+78) — COMPLETE".
    std::int32_t stun = 0;
    // Grab/pickup-pause (player state +78==4, "carrying" — NOT the +58 word
    // above). CONFIRMED distinct mechanism (`sub_41F29B` ~23017-23025): while
    // state==4, each tick compares the state's own elapsed-frame counter
    // (+80 — word 40 of the player record, shared with the kick/punch/warp anim
    // timers) against
    // `getvalue(665)` (our `pickup_pause`, id 665, docs/valuelst-map.md); while
    // the counter is still within that window it clears the new-input flag
    // (blocking new input, same as a head-stun) AND FORCES the bomb-key-down
    // byte +56 to 1, so the bomb-action tail's throw check (which needs +56
    // clear) does not fire while the forced hold
    // is active. `sub_424AF4` (the grab-attach primitive called from the drop
    // block right before state is set to 4) never touches +58 — confirmed by
    // reading its body (pseudo.c ~26018-26025): it only links the bomb/player
    // pointers, sets the bomb's own motion word to 3 (carried), and plays the
    // grab SFX. So a grab-triggered pause and a head-hit stun are always two
    // independent counters in the original.
    std::int32_t pickup_pause = 0;
    // Trampoline hop (player state +78==5, sub_41F29B state 5 / sub_41DE63): a
    // step onto a trampoline launches a FLIGHT lasting tuning
    // .trampoline_bounce_frames ticks (VALUELST id 680 = 30). During the flight
    // movement input is ignored and the player is invulnerable to being pushed;
    // at the APEX (frame count == 680/2 == 15) it is TELEPORTED to a random nearby
    // open tile (StageActorSystem::tick_bounce, the confirmed 0x4203a7 loop). This
    // is a countdown (30→0); the apex fires once at bounce == 15. Hashed. §4.
    std::int32_t bounce = 0;
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
    // Carried bomb (picked up with the grab glove); its fuse is frozen.
    bool carrying = false;
    std::int32_t carried_fuse = 0, carried_flame = 2;
    bool carried_jelly = false, carried_trigger = false;
    std::uint8_t carried_owner = 0;
    // The carried bomb's colour (Bomb::colour): the original never recreates
    // a grabbed bomb — it just flips its motion state to 3 — so both halves
    // of its +60 dword (colour byte AND owner word) survive the carry
    // untouched. Preserved here for the same reason carried_owner is.
    std::uint8_t carried_colour = 0;
    bool jelly = false;
    bool prev_action1 = false;  // for edge detection (part of state!)
    bool prev_action2 = false;
    // Diseases: per-kind active flags sharing one countdown; a healthy player
    // has disease_timer == 0. disease_fresh gates re-spreading (VALUELST id 129).
    std::array<bool, kDiseaseKinds> disease{};
    std::int32_t disease_timer = 0;
    std::int32_t disease_fresh = 0;

    // Ice / input-lag ring buffer (VALUELST ids 450-460, Hockey Rink;
    // docs/re/facts.md "Ice / input-lag"). The original keeps a 30-slot
    // per-player history of the desired movement direction (dword_4621C8)
    // and, for HUMAN players only, feeds the mover the OLDEST sample whose
    // age has reached the level's ice-delay threshold instead of the fresh
    // one — a fixed input-response lag, not a physics/friction change.
    // index 0 = most recent tick's want_godir (-1 = no direction, 0..3 =
    // Up/Right/Down/Left); index k = k ticks ago. Only ever written/read by
    // MovementSystem::ice_delay, and only when the current level's
    // ice_delay_ms > 0 (every other level leaves this all-zero, so it hashes
    // as mix(0) there — see hash.cpp). Hashed: it is live gameplay state
    // that determines a future tick's effective movement direction.
    static constexpr int kIceHistoryLen = 30;  // mirrors the original's 30-slot buffer
    std::array<std::int8_t, kIceHistoryLen> ice_history{};

    int tile_x() const { return static_cast<int>(x / kTileWF); }
    int tile_y() const { return static_cast<int>(y / kTileHF); }
    bool sick(Disease d) const { return disease[static_cast<int>(d)]; }
};

}  // namespace bomber::sim
