#pragma once

#include "bomber/game/asset_store.hpp"
#include "bomber/game/sprites.hpp"

// All sequences the renderer needs, resolved once at startup (and per stage).

namespace bomber::game {

// The nine flame pieces of one player-colored set: the epicenter, the four
// middle arms, and the four tips.
struct FlameSet {
    Anim center, mid_h[2], mid_v[2], tip_n, tip_s, tip_w, tip_e;
};

struct SequenceSet {
    Anim brick, solid, burn;
    Anim bomb[kLocalPlayers];
    FlameSet flames[kLocalPlayers];
    Anim stand[kLocalPlayers][4], walk[kLocalPlayers][4];  // [player][direction]
    Anim shadow;
    Anim digits;  // KFONT 'numeric font': glyphs 0-9 + colon
    Anim hurry;

    // Resolves the stage-independent sequences (call again after
    // build_player_sets so the recolored copies get picked up).
    void resolve(const AssetStore& a);

    // Resolves the per-stage tile/burn sequences.
    void resolve_stage(const AssetStore& a, int stage);
};

}  // namespace bomber::game
