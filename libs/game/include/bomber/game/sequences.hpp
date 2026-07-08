#pragma once

#include "bomber/game/asset_store.hpp"
#include "bomber/game/sprites.hpp"
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

struct SequenceSet {
    Anim brick, solid, burn;
    Anim bomb[kLocalPlayers];
    Anim bomb_dud[kLocalPlayers];      // DUDS.ANI "bomb regular green dud" (fizzle)
    Anim bomb_trigger[kLocalPlayers];  // TRIGBOMB.ANI "bomb trigger green" (armed remote)
    // Animated floor-powerup art (POWERS.ANI "power <name>"), indexed by
    // sim::PowerupType. Shared/uncoloured. Empty entries fall back to POW*.PCX.
    Anim powerup_anim[sim::kPowerupKinds];
    FlameSet flames[kLocalPlayers];
    Anim stand[kLocalPlayers][4], walk[kLocalPlayers][4];  // [player][direction]
    // Warp/teleport animation. CONFIRMED from the binary: sub_41F29B warp states
    // 6 and 7 both strcpy the literal sequence name "spin" (@0x45a213) and draw
    // the player with it; "spin" lives in WALK.ANI (the 5th sequence, after the 4
    // walk dirs). Direction-independent, per-player recolored like walk/stand.
    Anim spin[kLocalPlayers];
    Anim kick[kLocalPlayers][4], punch[kLocalPlayers][4];  // action poses, [player][direction]
    Anim walkbomb[kLocalPlayers][4], standbomb[kLocalPlayers][4];  // carrying a bomb, [player][direction]
    Anim cornerhead[kLocalPlayers][kCornerheadVariants];   // idle fidgets, direction-independent
    Anim shadow;
    Anim digits;    // KFONT 'numeric font': glyphs 0-9 + colon
    Anim infinity;  // KFONT 'infinity' — drawn instead of digits on an untimed round
                    // (docs/re/in-match-shell.md §3: dword_4601A8 == 1001, aInfinity)
    Anim hurry;
    // Stage-actor floor art (docs/re/stage-actors.md). conveyor/dirarrow indexed
    // by godir (0=Up/north,1=Right/east,2=Down/south,3=Left/west); trampoline
    // and warphole are direction-independent. Empty entries simply draw nothing.
    // EXTRAS.ANI: "extra arrow <dir>" (dirarrow), "extra warp 1" (warphole),
    // "extra trampoline"; CONVEYOR.ANI: "extra conveyor <dir>".
    Anim conveyor[4];
    Anim dirarrow[4];
    Anim warphole;
    Anim trampoline;

    // Resolves the stage-independent sequences (call again after
    // build_player_sets so the recolored copies get picked up).
    void resolve(const AssetStore& a);

    // Resolves the per-stage tile/burn sequences.
    void resolve_stage(const AssetStore& a, int stage);
};

}  // namespace bomber::game
