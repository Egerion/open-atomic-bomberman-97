#pragma once

#include "bomber/render/asset_store.hpp"
#include "bomber/render/sprites.hpp"
#include "bomber/sim/constants.hpp"

// All sequences the renderer needs, resolved once at startup (and per stage).

namespace bomber::game {

// The 13 direction-independent "cornerhead" idle-fidget variants, spread across
// CORNER0..7.ANI (sub_41F29B picks rand()%13 while boxed-in).
inline constexpr int kCornerheadVariants = 13;

// The nine flame pieces of one player-colored set: the epicenter, the four
// middle arms, and the four tips.
struct FlameSet {
    Anim center, mid_h[2], mid_v[2], tip_n, tip_s, tip_w, tip_e;
};

// An empty entry is always legal and always means "draw nothing / fall back":
// sequences.cpp leaves one empty when a partial install lacks the file, and
// every draw site guards for it. The RE corrections on which FILE backs which
// pose live on the AssetStore accessors, not here.
struct SequenceSet {
    Anim brick, solid, burn;
    Anim bomb[kLocalPlayers];
    Anim bomb_dud[kLocalPlayers];      // DUDS.ANI "bomb regular green dud" (fizzle)
    Anim bomb_trigger[kLocalPlayers];  // TRIGANIM.ANI "bomb trigger green" (armed remote)
    // BOMBS.ANI "bomb jelly green": sub_42331C composes "bomb %s green" from the
    // kind table off_45BE44, so jelly has its OWN wobble sequence — we
    // previously drew jelly bombs with the regular pulse.
    Anim bomb_jelly[kLocalPlayers];
    Anim powerup_anim[sim::kPowerupKinds];  // POWERS.ANI "power <name>", shared
    // POWERS.ANI "power clog" — its own slot rather than an extra powerup_anim
    // entry, because clogs is never a sim inventory kind
    // (docs/re/goldman-roulette.md §8/§9.2/§9.4).
    Anim clogs_anim;
    FlameSet flames[kLocalPlayers];
    Anim stand[kLocalPlayers][4], walk[kLocalPlayers][4];  // [player][direction]
    // CONFIRMED: sub_41F29B warp states 6 and 7 both strcpy the literal name
    // "spin" (@0x45a213), which lives in WALK.ANI. Direction-independent.
    Anim spin[kLocalPlayers];
    Anim kick[kLocalPlayers][4];                                   // KICK.ANI "kick <dir>"
    Anim punch[kLocalPlayers][4];                                  // PUNBOMB1..4.ANI "punch <dir>"
    Anim walkbomb[kLocalPlayers][4], standbomb[kLocalPlayers][4];  // carrying a bomb
    Anim pickup[kLocalPlayers][4];  // PUP1..4.ANI "pickup <dir>", action-state 4
    Anim cornerhead[kLocalPlayers][kCornerheadVariants];  // direction-independent fidgets
    Anim shadow;
    Anim digits;    // KFONT "numeric font": glyphs 0-9 + colon
    Anim infinity;  // KFONT "infinity" — drawn instead of digits on an untimed round
    Anim hurry;
    Anim eliminated_marker;  // MISC.ANI "xxx", over a dead player's score entry
    // Stage-actor floor art (docs/re/stage-actors.md). conveyor/dirarrow/ghost/
    // rover are indexed by GODIR (0=north, 1=east, 2=south, 3=west) to match
    // actor_dir; trampoline and warphole are direction-independent.
    Anim conveyor[4];
    Anim dirarrow[4];
    Anim warphole;
    Anim trampoline;
    // ALIENS1.ANI "rover <dir>"/"ghost <dir>" (sub_4518D0). CORRECTED
    // 2026-07-09: called cut content until the sequences turned up under this
    // file's name (docs/re/facts.md "ANI sequence-name audit").
    Anim ghost[4];
    Anim rover[4];

    // Resolves the stage-independent sequences (call again after
    // build_player_sets so the recolored copies get picked up).
    void resolve(const AssetStore& a);

    // Resolves the per-stage tile/burn sequences.
    void resolve_stage(const AssetStore& a, int stage);
};

}  // namespace bomber::game
