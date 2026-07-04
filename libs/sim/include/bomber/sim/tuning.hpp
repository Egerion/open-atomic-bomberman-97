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
    std::int32_t kicked_bomb_speed = 1000; // id 300
    std::int32_t punched_bomb_speed = 1300;// id 301
    std::int32_t game_seconds = 150;       // id 100
    std::int32_t taunt_chance = 5;         // id 95, 1-in-N post-death taunt
    std::int32_t hurry_seconds = 60;       // id 101, walls start closing in
    std::int32_t enclosement_depth = 1;    // id 27: 0 none, 1 = 2 rings, 2 = 4, 3 = all
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

    std::int32_t pickup_pause = 2;         // id 665: movement pause when grabbing, ticks
    std::int32_t powers_lost_min = 1;      // id 670: min powers dropped on a head hit
    std::int32_t powers_lost_rand = 3;     // id 671: modulus of the extra random drops
    std::int32_t head_stun_frames = 16;    // CONFIRMED hardcoded 16 (sub_421F7E sets the
                                           // +58 countdown; not a VALUELST id)
    std::int32_t punch_arc_first = 65;     // id 660: three-tile punch arc height, px
    std::int32_t punch_arc_hop = 20;       // id 661: subsequent one-tile hops
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
    // to a player's move budget per tick (fixed 20 Hz makes the original's
    // frame/dword_46494C factor == 1). id 189 = how many speeds exist; the
    // board's conveyor-speed selector (dword_464930, 0..2) picks one:
    //   190 = 250 (low), 191 = 350 (medium), 192 = 450 (high).
    std::int32_t conveyor_speed_count = 3; // id 189
    std::int32_t conveyor_speeds[3] = {250, 350, 450}; // ids 190,191,192
    // Which of the three the current board uses. OUR TUNABLE: the original
    // reads dword_464930 (set per-board / editor-cyclable); we have not yet RE'd
    // where a board persists its index, so we default to "low" (index 0 = 250)
    // and expose it for MatchConfig to override once the board field is mapped.
    std::int32_t conveyor_speed_index = 0;
    // Trampoline in-place bounce length, ticks. OUR TUNABLE: the original times
    // the hop by the bounce ANI (player state +78==5), not a VALUELST id; 20
    // ticks (~1s at 20 Hz) is a faithful-feeling placeholder pending the ANI
    // frame count. During the bounce, movement is ignored (state-gated).
    std::int32_t trampoline_bounce_frames = 20;

    // The belt contribution actually applied per tick, resolving the selector.
    std::int32_t conveyor_speed() const {
        int i = conveyor_speed_index;
        if (i < 0) i = 0;
        if (i > 2) i = 2;
        return conveyor_speeds[i];
    }
    // Note: player movement (corner assist / lane centering) is not tunable —
    // it is a faithful port of sub_41EC84 and needs no threshold constant.

    // Diseases (VALUELST 120..138; see docs/re/facts.md "Disease system").
    // Nine diseases, one duration each at ids 130..138.
    std::int32_t disease_frames[kDiseaseKinds] = {300, 300, 300, 300, 300, 300, 300, 300, 300};
    std::int32_t disease_freshness = 10;   // id 129: ticks before a disease can pass again
    std::int32_t disease_cure_chance = 10; // id 125: 1-in-N cure per fresh powerup
    bool diseases_time_limited = true;     // id 121: wears off after its duration
    bool diseases_multiply = true;         // id 123: on contact both keep it
    bool diseases_curable = true;          // id 124: a fresh powerup can cure

    // Feeds one parsed VALUELST pair. Returns true if the id was consumed.
    bool apply(int id, std::int64_t value) {
        auto v = static_cast<std::int32_t>(value);
        switch (id) {
            case 41: fuse_frames = v; return true;
            case 42: start_speed = v; return true;
            case 90: skate_speed_bonus = v; return true;
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
            case 121: diseases_time_limited = v != 0; return true;
            case 123: diseases_multiply = v != 0; return true;
            case 124: diseases_curable = v != 0; return true;
            case 125: disease_cure_chance = v; return true;
            case 129: disease_freshness = v; return true;
            default: break;
        }
        if (id >= 130 && id < 139) { disease_frames[id - 130] = v; return true; }
        if (id >= 50 && id < 50 + kPowerupKinds) { start_with[id - 50] = v; return true; }
        if (id >= 550 && id < 550 + kPowerupKinds) { limits[id - 550] = v; return true; }
        if (id >= 400 && id < 400 + kPowerupKinds) { spawn_counts[id - 400] = v; return true; }
        if (id >= 1150 && id < 1161) { level_enabled[id - 1150] = v; return true; }
        if (id >= 200 && id < 250) {
            int k = id - 200;
            if (k % 5 < 3) color_rgb[k / 5][k % 5] = v;
            return true;
        }
        return false;
    }
};

}  // namespace bomber::sim
