#pragma once

#include <array>
#include <cstdint>

#include "bomber/sim/constants.hpp"
#include "bomber/sim/types.hpp"

namespace bomber::sim {

// One player's complete gameplay state. Deliberately a plain aggregate: EVERY
// field here is deterministic state and must be covered by state_hash()
// (determinism contract rule 4). Comments name the original's byte offset where
// one is known, since that is what makes the port checkable against the binary.
struct Player {
    bool present = false;
    bool alive = false;
    // +16 == 1: computer-controlled (ADR-0005). AISystem fills this player's
    // PlayerInput before player_turn; otherwise the externally-supplied
    // (human/replay) input passes through. docs/re/ai.md §7.
    bool ai = false;
    // +84, toggled by 'T' on the PLAYER INPUT screen (sub_4223E7/sub_422437,
    // docs/re/setup-screens.md). It gates AI targeting (ai.md §3.4/§5.3) and
    // round-end, so it is gameplay state, not presentation config.
    //
    // Our convention, NOT RE'd beyond the byte's existence: two players are on
    // the same side only when they share the same NONZERO value. Team 0 means "no
    // team" and never merges, so two team-0 players are always distinct sides
    // (AISystem::same_team, sides_remaining).
    std::uint8_t team = 0;
    Fixed x = 0, y = 0;  // centre position in field pixels x 100
    Direction facing = Direction::Down;
    std::int32_t speed = 0;        // movement budget per tick (VALUELST id 42 + skates)
    std::int32_t move_budget = 0;  // carried sub-pixel budget; 100 per 1px step (sub_41EC84)
    std::int32_t max_bombs = 1;
    std::int32_t bombs_placed = 0;
    std::int32_t flame = 2;  // cells beyond the epicentre
    bool goldflame = false;  // +94: reach becomes max(gridW,gridH) at drop time
    std::int32_t skates = 0;
    // Goldman wheel booby prize (inventory slot 13, docs/re/goldman-roulette.md
    // §9): a speed penalty, the mirror image of `skates`. Born-with ONLY — §9.2
    // establishes there is no normal-play pickup path, so PowerupSystem has no
    // apply/remove case for it and setup.cpp folds it into `speed` directly.
    std::int32_t clogs = 0;
    bool kick = false;
    bool punch = false;
    bool grab = false;
    bool spooge = false;  // lays a line of bombs ahead (Spooger)
    bool trigger = false;
    // +85 (sub_41EB13/sub_41E21E): a Trigger pickup resets this to 0 and each
    // placed trigger bomb increments it, capped at max_bombs. NEVER decremented,
    // not even on detonation, so once exhausted further placements fall back to
    // normal timed bombs until the next Trigger pickup refills the budget.
    std::int32_t trigger_placed = 0;
    // Head-hit stun (WORD +58). sub_421F7E overwrites it with 16 unconditionally,
    // sets the state word +78 to 3 and zeroes the anim counter +80. It gates ONLY
    // new-input acquisition — the mover keeps running (docs/re/player-turn.md §1).
    //
    // CONFIRMED a SEPARATE counter from pickup_pause below: the two write
    // different fields in sub_41F29B and neither resets the other, so a head hit
    // landing mid-pickup-pause ticks both down independently. One field used to
    // do double duty for both, which meant a grab clobbered an in-progress
    // head-stun countdown and vice versa (split 2026-07-11, facts.md "Player
    // state machine (+78) — COMPLETE").
    std::int32_t stun = 0;
    // Grab pickup-pause — player STATE +78 == 4, not the +58 word above.
    // CONFIRMED distinct (sub_41F29B ~23017-23025): while state == 4 each tick
    // compares the state's own elapsed-frame counter +80 against getvalue(665).
    // The comparison is NON-STRICT (`+80 <= getvalue(665)`, confirmed at
    // 0x41FA55 where the skip branch is a "greater" jump), so the window is
    // getvalue(665) + 1 ticks and BombSystem::try_grab seeds it accordingly.
    // Within the window it clears the new-input flag AND forces the bomb-key-down
    // byte +56 to 1, so the tail's throw check — which needs +56 clear — does not
    // fire while the forced hold is active.
    //
    // sub_424AF4, the grab-attach primitive, never touches +58 (pseudo.c
    // ~26018-26025: it only links the bomb/player pointers, sets the bomb's motion
    // word to 3 and plays the SFX), which is why the two are always independent.
    std::int32_t pickup_pause = 0;
    // Trampoline hop (state +78 == 5): a countdown from
    // tuning.trampoline_bounce_frames (VALUELST id 680 = 30) to 0. During the
    // flight movement input is ignored and the player cannot be pushed; the APEX
    // fires once at bounce == 15 and TELEPORTS the player to a random nearby open
    // tile (StageActorSystem::tick_bounce, the 0x4203a7 loop).
    // docs/re/stage-actors.md §4.
    std::int32_t bounce = 0;
    // Warphole two-phase warp (states 6 = warp-out, 7 = warp-in; sub_41F29B
    // ~23155/23215, step-on in sub_41EC84 ~22590). CONFIRMED timing: warp-out
    // animates until its frame counter passes 8 (9 ticks), then the player is
    // relocated to the linked exit and warp-in runs another 9 — 18 ticks total,
    // state-gated and un-pushable throughout (sub_41DE63 returns 0 for 6/7). The
    // countdown drives all of it, relocating at the midpoint. No RNG: the exit is
    // pre-resolved into State::warp_dest_* at setup. The prior
    // instantaneous-teleport model left the player stuck by never running the
    // out/in phases the renderer expects. docs/re/stage-actors.md §5.
    std::int32_t warp = 0;
    // Pending warp destination (+20/+24), captured at STEP-ON rather than re-read
    // at the midpoint: the per-pixel loop can slide the player a few px off the
    // warphole within the trigger tick, so a midpoint tile lookup would miss the
    // entry tile. Doubles as AI behaviour 4's travelled-distance snapshot (ai.md
    // finding 1) — setup.cpp seeds it to the spawn tile.
    std::int32_t warp_to_x = 0, warp_to_y = 0;
    // Carried bomb (grab glove); its fuse is frozen. The original never recreates
    // a grabbed bomb — it just flips the motion state to 3 — so both halves of the
    // bomb's +60 dword (colour byte AND owner word) survive the carry untouched,
    // which is why colour is preserved here alongside owner.
    bool carrying = false;
    std::int32_t carried_fuse = 0, carried_flame = 2;
    bool carried_jelly = false, carried_trigger = false;
    std::uint8_t carried_owner = 0;
    std::uint8_t carried_colour = 0;
    bool jelly = false;
    // +54/+55: the previous frame's EFFECTIVE key values, which the bomb-action
    // tail edge-tests against (docs/re/player-turn.md §2).
    bool prev_action1 = false;
    bool prev_action2 = false;
    // Per-kind active flags sharing one countdown; a healthy player has
    // disease_timer == 0. disease_fresh gates re-spreading (VALUELST id 129).
    std::array<bool, kDiseaseKinds> disease{};
    std::int32_t disease_timer = 0;
    std::int32_t disease_fresh = 0;

    // Ice / input-lag ring buffer (VALUELST ids 450-460, Hockey Rink; facts.md
    // "Ice / input-lag"). The original keeps a 30-slot per-player history of the
    // desired direction (dword_4621C8) and, for HUMAN players only, feeds the
    // mover the freshest sample already old enough to meet the level's threshold
    // — a fixed input-response lag, not a friction change.
    //
    // Index 0 is the most recent SUB-FRAME's want_godir (-1 = none, 0..3 =
    // Up/Right/Down/Left) and index k is k sub-frames ago, NOT k ticks: the buffer
    // is pushed once per sub-frame, so its 30 slots span ~167 ms at kSubFrames =
    // 9. Written and read only by MovementSystem::ice_delay, and only where
    // ice_delay_ms > 0, so it stays all-zero on every other level.
    static constexpr int kIceHistoryLen = 30;  // mirrors the original's 30-slot buffer
    std::array<std::int8_t, kIceHistoryLen> ice_history{};

    int tile_x() const { return static_cast<int>(x / kTileWF); }
    int tile_y() const { return static_cast<int>(y / kTileHF); }
    bool sick(Disease d) const { return disease[static_cast<int>(d)]; }
};

}  // namespace bomber::sim
