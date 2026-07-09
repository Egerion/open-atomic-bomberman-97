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
    Anim bomb_dud[kLocalPlayers];  // DUDS.ANI "bomb regular green dud" (fizzle)
    // TRIGANIM.ANI "bomb trigger green" (armed remote; TRIGANIM not TRIGBOMB —
    // see AssetStore::trigbomb_'s doc comment).
    Anim bomb_trigger[kLocalPlayers];
    // BOMBS.ANI "bomb jelly green" — the original's bomb drawer composes
    // "bomb %s green" from the kind table off_45BE44 {"regular","trigger",
    // "jelly"} (sub_42331C, pseudo.c ~25587), so a jelly bomb has its OWN
    // wobble sequence; we previously drew jelly bombs with the regular pulse
    // (docs/re/facts.md "ANI sequence-name audit").
    Anim bomb_jelly[kLocalPlayers];
    // Animated floor-powerup art (POWERS.ANI "power <name>"), indexed by
    // sim::PowerupType. Shared/uncoloured. Empty entries fall back to POW*.PCX.
    Anim powerup_anim[sim::kPowerupKinds];
    // Goldman wheel clogs icon (POWERS.ANI "power clog", CONFIRMED present in
    // the shipped file) — outside sim::PowerupType/kPowerupKinds (clogs is
    // never a sim inventory kind, docs/re/goldman-roulette.md §8/§9.2), so it
    // gets its own slot rather than an extra powerup_anim entry. The original
    // draws this uniformly with the other 5 wheel-slot icons (sub_4034BC
    // pseudo.c 6022-6031, no special-case skip for slot 13, §9.4).
    Anim clogs_anim;
    FlameSet flames[kLocalPlayers];
    Anim stand[kLocalPlayers][4], walk[kLocalPlayers][4];  // [player][direction]
    // Warp/teleport animation. CONFIRMED from the binary: sub_41F29B warp states
    // 6 and 7 both strcpy the literal sequence name "spin" (@0x45a213) and draw
    // the player with it; "spin" lives in WALK.ANI (the 5th sequence, after the 4
    // walk dirs). Direction-independent, per-player recolored like walk/stand.
    Anim spin[kLocalPlayers];
    Anim kick[kLocalPlayers][4];  // KICK.ANI "kick <dir>" action pose, [player][direction]
    // Punch action pose. CORRECTED 2026-07-09 (docs/re/facts.md "ANI
    // sequence-name audit"): backed by PUNBOMB1..4.ANI ("punch <dir>", no
    // "green" suffix) — MASTER.ALI never loads PUNCH.ANI (whose "punch <dir>
    // green" sequences were dead art), so that file was the wrong source.
    Anim punch[kLocalPlayers][4];
    Anim walkbomb[kLocalPlayers][4],
        standbomb[kLocalPlayers][4];  // carrying a bomb, [player][direction]
    // "Picking up a bomb" transitional pose (sub_41F29B action-state 4):
    // PUP1..4.ANI "pickup <dir>", no "green" suffix. Played for ~10 ticks
    // (the sequence's own step count, matching the original's statecnt
    // comparison) right after a BombGrabbed event, before the steady-state
    // walkbomb/standbomb carry pose takes over. Was entirely unwired before
    // this audit (BPICKUP.ANI, the file a naive name-guess would reach for,
    // is also dead art — never loaded by MASTER.ALI).
    Anim pickup[kLocalPlayers][4];
    Anim cornerhead[kLocalPlayers][kCornerheadVariants];  // idle fidgets, direction-independent
    Anim shadow;
    Anim digits;    // KFONT 'numeric font': glyphs 0-9 + colon
    Anim infinity;  // KFONT 'infinity' — drawn instead of digits on an untimed round
                    // (docs/re/in-match-shell.md §3: dword_4601A8 == 1001, aInfinity)
    Anim hurry;
    // MISC.ANI 'xxx' (docs/re/in-match-shell.md "The player row") — overlaid
    // on a round-eliminated player's top-of-screen score entry.
    Anim eliminated_marker;
    // Stage-actor floor art (docs/re/stage-actors.md). conveyor/dirarrow indexed
    // by godir (0=Up/north,1=Right/east,2=Down/south,3=Left/west); trampoline
    // and warphole are direction-independent. Empty entries simply draw nothing.
    // EXTRAS.ANI: "extra arrow <dir>" (dirarrow), "extra warp 1" (warphole),
    // "extra trampoline"; CONVEYOR.ANI: "extra conveyor <dir>".
    Anim conveyor[4];
    Anim dirarrow[4];
    Anim warphole;
    Anim trampoline;

    // Campaign rover/ghost hazard actors (docs/re/campaign.md "Per-tick
    // mover" step 5): the original formats sequence names "rover
    // <north|east|south|west>" / "ghost <...>" (sub_4518D0(buf, aGhostS/
    // aRoverS, dir)). CORRECTED 2026-07-09 (docs/re/facts.md "ANI
    // sequence-name audit"): earlier notes here claimed this was cut content
    // because no file literally named GHOST.ANI/ROVER.ANI exists — but the
    // sequences ship under ALIENS1.ANI instead (8 sequences: ghost/rover x
    // north/east/south/west, CONFIRMED against the install). Shared/
    // uncoloured (no "green" suffix in the names, so no per-player recolour),
    // indexed by godir like conveyor/dirarrow above. Empty when the file is
    // missing -> Renderer::draw_world keeps its plain marker fallback.
    Anim ghost[4];
    Anim rover[4];

    // Resolves the stage-independent sequences (call again after
    // build_player_sets so the recolored copies get picked up).
    void resolve(const AssetStore& a);

    // Resolves the per-stage tile/burn sequences.
    void resolve_stage(const AssetStore& a, int stage);
};

}  // namespace bomber::game
