#pragma once

#include <cstdint>

#include "bomber/sim/constants.hpp"

// Gameplay tuning values. Field defaults mirror the original VALUELST.RES;
// apply() lets a loader feed parsed "id,value" pairs so modified VALUELST
// files keep working. IDs are documented in docs/valuelst-map.md.

namespace bomber::sim {

struct Tuning {
    // Timing reference: original runs a nominal 20 frames/second (id 25/30).
    std::int32_t fuse_frames = 40;         // id 41
    std::int32_t start_speed = 923;        // id 42, 1/100 px per frame
    std::int32_t skate_speed_bonus = 150;  // id 90
    // Clogs (Goldman wheel booby prize, inventory slot 13) speed PENALTY:
    // subtracted per clogs count in the same walk-speed term as skate_speed_
    // bonus is added, before disease scaling (sub_41F29B, docs/re/
    // goldman-roulette.md §9.1: "v20 - v22*v21"). Wheel-only — never a
    // normal-play pickup (§9.2).
    std::int32_t clogs_speed_penalty = 150;  // id 91
    std::int32_t kicked_bomb_speed = 1000; // id 300
    std::int32_t punched_bomb_speed = 1300;// id 301
    std::int32_t game_seconds = 150;       // id 100
    std::int32_t taunt_chance = 5;         // id 95, 1-in-N post-death taunt
    std::int32_t hurry_seconds = 60;       // id 101, walls start closing in
    std::int32_t enclosement_depth = 1;    // id 27: 0 none, 1 = 2 rings, 2 = 4, 3 = all
    // id 46 ("when a wall segment closes in on a bomb, does it set the bomb
    // off? 0 - destroy, 1 - detonate ... this is a default; otherwise the
    // settings override it") = the OPTIONS-screen "Stomped Bombs Detonate"
    // toggle's seed value: dword_464940 = getvalue(46) at init (sub_41095A),
    // then options.ini "stomped_bombs_detonate=" / the Options row overrides.
    // Consumed by EnclosureSystem::drop_wall. See docs/re/facts.md
    // "Options toggles: stomped_bombs_detonate / diseases_destroyable".
    std::int32_t wall_detonates = 1;       // id 46: closing wall detonates (1) or eats (0) bombs

    // Starting inventory (ids 50..62), indexed by PowerupType.
    std::int32_t start_with[kPowerupKinds] = {1, 2, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    // Per-player accumulation limits (ids 550..562), 0 = unlimited.
    std::int32_t limits[kPowerupKinds] = {8, 8, 0, 1, 4, 1, 1, 1, 1, 1, 1, 0, 0};
    // How many of each powerup get hidden under bricks (ids 400..412).
    // Negative N means: |N| attempts, each with a 1-in-10 chance.
    std::int32_t spawn_counts[kPowerupKinds] = {10, 10, 3, 4, 8, 2, 2, 1, -2, -4, 1, -4, -2};

    // Player color remap targets (ids 200..247, stride 5): percent RGB the
    // green armour pixels get scaled to, in original player order.
    std::int32_t color_rgb[10][3] = {
        {100, 100, 100}, {20, 20, 20},  {100, 0, 0},   {0, 0, 100},  {0, 100, 0},
        {100, 100, 0},   {0, 100, 100}, {100, 0, 100}, {100, 50, 0}, {50, 0, 100},
    };

    // Which of the 11 stages take part in random stage selection (ids
    // 1150..1160; the original disables hockey rink and coal mine by default).
    std::int32_t level_enabled[11] = {1, 1, 0, 1, 1, 1, 0, 1, 1, 1, 1};

    // Which of the 11 built-in stages this match is playing (0..10, same
    // index space as level_enabled/kNames in game_app.cpp's level_fallback).
    // NOT itself a VALUELST id — it is the "current level" selector
    // (dword_46499C in the original) that the per-level blocks below
    // (regen_seconds, ice_delay_ms) are indexed by. bomber::match/game_app
    // set this from the resolved stage index (docs/re/facts.md "Per-level
    // tile regeneration", "Ice / input-lag"). Defaults to 0 ("new
    // traditionalist"), which has zero regen and zero ice delay, so every
    // hand-built MatchConfig (tests, golden) that never sets this keeps its
    // old inert behaviour.
    std::int32_t level_index = 0;

    // Per-level tile regeneration (ids 340..350, one per stage): seconds
    // between regen ATTEMPTS on that stage; 0 = the mechanic never runs
    // there. Only level index 7 ("haunted house", the file's own comment
    // calls it "cemetary/mortuary") is non-zero in the shipped VALUELST.
    // Consumed by TileRegenSystem. See docs/re/facts.md "Per-level tile
    // regeneration".
    std::int32_t regen_seconds[11] = {0, 0, 0, 0, 0, 0, 0, 4, 0, 0, 0};
    // id 695: the Manhattan-distance "clear radius" (in tiles) that must be
    // free of any live player around a candidate regen tile. Companion to
    // regen_seconds; consumed by the SAME system.
    std::int32_t regen_clear_radius = 4;

    // Per-level "ice delay" (ids 450..460, one per stage), milliseconds of
    // input-response lag applied to HUMAN players' desired movement
    // direction. Only level index 2 ("hockey rink") is non-zero in the
    // shipped VALUELST. id 449 itself is a documented sentinel ("just to
    // prevent invalid access") the original never reads — the real
    // per-level block starts at 450, so this array is NOT id-450-based in
    // the id-minus-base sense used elsewhere; see apply() below. Consumed
    // by MovementSystem::ice_delay. See docs/re/facts.md "Ice / input-lag".
    std::int32_t ice_delay_ms[11] = {0, 0, 250, 0, 0, 0, 0, 0, 0, 0, 0};

    std::int32_t pickup_pause = 2;         // id 665: movement pause when grabbing, ticks
    std::int32_t powers_lost_min = 1;      // id 670: min powers dropped on a head hit
    std::int32_t powers_lost_rand = 3;     // id 671: modulus of the extra random drops
    std::int32_t head_stun_frames = 16;    // CONFIRMED hardcoded 16 (sub_421F7E sets the
                                           // +58 countdown; not a VALUELST id)
    std::int32_t punch_arc_first = 65;     // id 660: three-tile punch arc height, px
    std::int32_t punch_arc_hop = 20;       // id 661: subsequent one-tile hops
    // "Fire In The Hole" taunt (docs/re/id-audit.md item 1, sub_41F29B pseudo.c
    // ~23362-23368, the plain single-bomb-drop path): id 651 ("what constitutes
    // 'many' dropped bombs") gates a player's CURRENT bomb-count powerup level
    // (max_bombs); id 650 ("the chance (1 in N) of the Clear/Fire In The Hole
    // audio playing") is the roll denominator. Consumed presentation-side only
    // (SoundDirector) — see that file's BombPlaced case for the full citation,
    // including the register-provenance caveat on the 651 comparison.
    std::int32_t taunt_many_bombs = 4;     // id 651
    std::int32_t taunt_many_chance = 4;    // id 650
    std::int32_t jelly_turn_chance = 3;    // id 667: flying jelly veers ±90°, 1-in-N per boundary
    // Duds (sub_422EDE / sub_422C13): only regular bombs fizzle, gated by a
    // global timer that re-arms base + rand(spread) ticks ahead.
    std::int32_t dud_gate_base = 180;      // id 320
    std::int32_t dud_gate_rand = 180;      // id 321
    std::int32_t dud_chance = 3;           // id 322: 1-in-N when the gate is open
    std::int32_t dud_frames = 120;         // id 323: fizzle duration
    std::int32_t flame_frames = 10;        // id 10 (flame anim cycle); confirmed as the
                                           // flame lifetime by disasm of 0x426d06
    std::int32_t brick_burn_frames = 10;   // id 20 (disintegration anim length)

    // Conveyor speeds (VALUELST ids 189..192, sub_41F29B via getvalue(190+idx);
    // see docs/re/stage-actors.md §3). The belt adds this many 1/100-px units
    // to a player's move budget per tick — the SAME budget units as player speed
    // (id 42), spent 100-per-pixel, because the original scales both the walk
    // and the belt by the identical dword_464958/dword_46494C frame ratio which
    // is ~1 at the locked 20 Hz. id 189 = how many speeds exist:
    //   190 = 250 (low), 191 = 350 (medium), 192 = 450 (high).
    std::int32_t conveyor_speed_count = 3; // id 189
    std::int32_t conveyor_speeds[3] = {250, 350, 450}; // ids 190,191,192
    // The "Conveyor Speed" GAME OPTION selector (dword_464930, 0/1/2), NOT a
    // per-board field. CONFIRMED: the binary's hardcoded default is 1 (medium)
    // (pseudo.c 14652); it is otherwise loaded from the options struct
    // (pseudo.c 12655 <- options.ini "conveyor_speed=") or editor-cycled. This
    // install's options.ini sets 2 (high). Default 1 here; MatchConfig overrides
    // it from parsed options. Clamped to [0, count-1] by conveyor_speed().
    std::int32_t conveyor_speed_index = 1;

    // The belt contribution actually applied per tick, resolving the selector.
    // The original clamps dword_464930 to [0, getvalue(189)-1]; getvalue(189)=3,
    // so the index is in [0,2] and always addresses the 3-element speed table.
    std::int32_t conveyor_speed() const {
        int hi = conveyor_speed_count - 1;
        if (hi > 2) hi = 2;   // never index past the 3-slot 190/191/192 table
        if (hi < 0) hi = 0;
        int i = conveyor_speed_index;
        if (i < 0) i = 0;
        if (i > hi) i = hi;
        return conveyor_speeds[i];
    }
    // Note: player movement (corner assist / lane centering) is not tunable —
    // it is a faithful port of sub_41EC84 and needs no threshold constant.

    // Trampoline in-place bounce length, frames. CONFIRMED VALUELST id 680 = 30
    // ("how many frames do you bounce on a trampoline?"), read by the bounce-
    // state branch of sub_41F29B (player state +78==5, pseudo.c ~23160: the +40
    // frame counter resets at getvalue(680); the hop apex is at getvalue(680)/2).
    // id 681 (=35, "pixels vertically per frame") is the hop arc height — a
    // PRESENTATION value the integer sim doesn't need. See stage-actors.md §4.
    std::int32_t trampoline_bounce_frames = 30;  // id 680

    // Computer-AI tunables (VALUELST 900..920; see docs/re/ai.md §6 and
    // docs/adr/0005). These feed ONLY the AI decision/danger paths, which are
    // per-tick scratch and non-AI-inert — Tuning is excluded from state_hash(),
    // so they never touch the golden. Defaults are the confirmed VALUELST values.
    std::int32_t ai_personalities = 1;     // id 900: brain-init spread (=1 -> personality 0)
    std::int32_t fire_god_lookahead = 15;  // id 910: closing-wall danger look-ahead tiles
    std::int32_t ai_blast_chance = 5;      // id 915: blast-bricks drop 1-in-N (Stage 4)
    std::int32_t ai_powerup_range = 4;     // id 920: powerup-seek BFS depth+range (Stage 3)

    // Diseases (VALUELST 120..138; see docs/re/facts.md "Disease system").
    // Nine diseases, one duration each at ids 130..138.
    std::int32_t disease_frames[kDiseaseKinds] = {300, 300, 300, 300, 300, 300, 300, 300, 300};
    std::int32_t disease_freshness = 10;   // id 129: ticks before a disease can pass again
    std::int32_t disease_cure_chance = 10; // id 125: 1-in-N cure per fresh powerup
    bool diseases_time_limited = true;     // id 121: wears off after its duration
    bool diseases_multiply = true;         // id 123: on contact both keep it
    bool diseases_curable = true;          // id 124: a fresh powerup can cure
    // id 120 ("can diseases be blown up like all other powerups?",
    // gbl_diseases_can_be_destroyed) = the OPTIONS-screen "Diseases Can Be
    // Destroyed" toggle's seed value: dword_464990 = getvalue(120) at init
    // (sub_41095A), then options.ini "diseases_destroyable=" / the Options row
    // overrides. When FALSE a destroyed floor skull is not lost — a fresh one
    // relocates to a random free tile (sub_4255B2(2)); the destruction itself
    // is unconditional. Consumed by FlameSystem::spread_to and
    // BombSystem::slide. See docs/re/facts.md "Options toggles".
    bool diseases_destroyable = true;      // id 120

    // Campaign rover/ghost tunables (docs/re/campaign.md "Per-tick mover" /
    // "Round pacing"). id 1205 ("human-avoidance bias") is confirmed NOT
    // read anywhere in the mover (sub_401B5C) — correctly left unconsumed;
    // see campaign.md's clause 2 for the full negative-result citation.
    std::int32_t rover_turn_chance = 3;    // id 1200: 1-in-N chance to turn at an open intersection
    std::int32_t campaign_ai_kill_score = 250;  // id 1300: points for killing an AI (campaign only)
    std::int32_t rover_kill_score = 15;    // id 1310: points for killing a rover (campaign only)
    std::int32_t ghost_kill_score = 25;    // id 1320: points for killing a ghost (campaign only)

    // Feeds one parsed VALUELST pair. Returns true if the id was consumed.
    bool apply(int id, std::int64_t value) {
        auto v = static_cast<std::int32_t>(value);
        switch (id) {
            case 41: fuse_frames = v; return true;
            case 42: start_speed = v; return true;
            case 90: skate_speed_bonus = v; return true;
            case 91: clogs_speed_penalty = v; return true;
            case 300: kicked_bomb_speed = v; return true;
            case 301: punched_bomb_speed = v; return true;
            case 100: game_seconds = v; return true;
            case 95: taunt_chance = v; return true;
            case 101: hurry_seconds = v; return true;
            case 27: enclosement_depth = v; return true;
            case 46: wall_detonates = v; return true;
            case 660: punch_arc_first = v; return true;
            case 665: pickup_pause = v; return true;
            case 670: powers_lost_min = v; return true;
            case 671: powers_lost_rand = v; return true;
            case 661: punch_arc_hop = v; return true;
            case 650: taunt_many_chance = v; return true;
            case 651: taunt_many_bombs = v; return true;
            case 667: jelly_turn_chance = v; return true;
            case 320: dud_gate_base = v; return true;
            case 321: dud_gate_rand = v; return true;
            case 322: dud_chance = v; return true;
            case 323: dud_frames = v; return true;
            case 20: brick_burn_frames = v; return true;
            case 10: flame_frames = v; return true;
            case 189: conveyor_speed_count = v; return true;
            case 190: conveyor_speeds[0] = v; return true;
            case 191: conveyor_speeds[1] = v; return true;
            case 192: conveyor_speeds[2] = v; return true;
            case 680: trampoline_bounce_frames = v; return true;  // trampoline bounce frames
            case 900: ai_personalities = v; return true;   // AI tunables (docs/re/ai.md §6)
            case 910: fire_god_lookahead = v; return true;
            case 915: ai_blast_chance = v; return true;
            case 920: ai_powerup_range = v; return true;
            case 120: diseases_destroyable = v != 0; return true;
            case 121: diseases_time_limited = v != 0; return true;
            case 123: diseases_multiply = v != 0; return true;
            case 124: diseases_curable = v != 0; return true;
            case 125: disease_cure_chance = v; return true;
            case 129: disease_freshness = v; return true;
            case 1200: rover_turn_chance = v; return true;
            case 1300: campaign_ai_kill_score = v; return true;
            case 1310: rover_kill_score = v; return true;
            case 1320: ghost_kill_score = v; return true;
            case 695: regen_clear_radius = v; return true;
            default: break;
        }
        if (id >= 130 && id < 139) { disease_frames[id - 130] = v; return true; }
        if (id >= 50 && id < 50 + kPowerupKinds) { start_with[id - 50] = v; return true; }
        if (id >= 550 && id < 550 + kPowerupKinds) { limits[id - 550] = v; return true; }
        if (id >= 400 && id < 400 + kPowerupKinds) { spawn_counts[id - 400] = v; return true; }
        if (id >= 1150 && id < 1161) { level_enabled[id - 1150] = v; return true; }
        if (id >= 340 && id <= 350) { regen_seconds[id - 340] = v; return true; }
        // id 449 (the "prevent invalid access" sentinel) is deliberately NOT
        // consumed here — no getvalue(449) call site exists in the original;
        // the real per-level ice block is 450..460.
        if (id >= 450 && id <= 460) { ice_delay_ms[id - 450] = v; return true; }
        if (id >= 200 && id < 250) {
            int k = id - 200;
            if (k % 5 < 3) color_rgb[k / 5][k % 5] = v;
            return true;
        }
        return false;
    }
};

}  // namespace bomber::sim
