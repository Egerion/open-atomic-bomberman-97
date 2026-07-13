#pragma once

#include <cstdint>

// Per-player computer-AI state (ADR-0005 §3, mirroring the original's 68-byte
// brain in docs/re/ai.md §1.1). A plain aggregate like Player/Bomb: every
// field is deterministic gameplay state and MUST be covered by state_hash().
// A Brain on a non-AI (or absent) player stays zero-initialised and hashes to
// a constant, so adding it is a one-time hash-layout growth that leaves non-AI
// scenarios (all golden) byte-stable apart from the new zero words.
//
// Design deltas from the binary, all determinism-neutral (ADR-0005 §3):
//  - Pointers -> indices. The original keeps raw actor/cell pointers (+16/+32);
//    we store the player SLOT or the TILE (snapshot-safe, hashable). Behaviours
//    only ever read the target's tile + liveness, both recoverable each tick.
//  - Timers in TICKS. The original accumulates ms (`+= dword_464958`) and times
//    out at 10*msPerFrame; at the locked 20 Hz that is exactly 10 ticks, stored
//    here as an integer countdown (no wall clock enters the sim, ADR-0003).
//  - Tile granularity. The original packs 16.16 coords (tile in the high word);
//    the AI only ever uses the tile (>>16), so we keep plain tile ints.

namespace bomber::sim {

struct Brain {
    // +0: personality id, seeded rand()%getvalue(900). getvalue(900)==1, so it
    // is always 0; kept for exactness (a non-zero id is a fatal in the original,
    // dead code while 900==1). See docs/re/ai.md §1.
    std::uint8_t personality = 0;

    // +52: AI action state. 9 == "committed to a brick-blast drop" (set by
    // sub_40AD8D, cleared to 0 when the situation clears / in sub_40A76E).
    // Written by Stage 4's blast-bricks behaviour; hashed from Stage 2 on.
    std::uint8_t state_flag = 0;

    // The directed-path goal (docs/re/ai.md §9.1, RESOLVED brain +2/+4/+6/+8):
    //  - has_path_target  <- word +2 (the flag: 1 == a directed goal is active)
    //  - path_target_x/y  <- words +4/+6 (the goal TILE, high half of the coord)
    //  - path_target_cost <- word +8 (danger score of the goal at capture, used
    //    to invalidate a stale target). Set in the flee branch of sub_40B20F.
    bool has_path_target = false;
    std::int16_t path_target_x = 0;
    std::int16_t path_target_y = 0;
    std::int32_t path_target_cost = 0;

    // +64: the persistent wander godir (re-rolled rand()%4 when blocked,
    // sub_40A81F). Godir order 0=Up,1=Right,2=Down,3=Left. Stage 2 (wander
    // fallback) reads/writes this.
    std::int8_t wander_dir = 0;

    // The ranged-powerup pursuit (+24/+28/+32/+36; sub_40BAF5). Filled by
    // Stage 3; present now so the hashed layout is stable across stages. Target
    // stored as a TILE (the powerup cell); timer in wall-clock ms, exactly the
    // original's `+28 += frameDelta` per displayed frame.
    struct PowerSeek {
        bool active = false;
        std::int32_t timer = 0;    // ms elapsed on the pursuit (times out at 10*50)
        std::int16_t tile_x = 0;   // the pursued powerup tile
        std::int16_t tile_y = 0;
        std::int8_t step_dir = 0;  // godir of the next step toward it
    } pow_seek;

    // The enemy pursuit (+10/+12/+16/+20; sub_40B8C2). Filled by Stage 5. Target
    // stored as a player SLOT index (not a pointer); timer in wall-clock ms
    // (`+12 += frameDelta`), same scheme as pow_seek above.
    struct EnemySeek {
        bool active = false;
        std::int32_t timer = 0;       // ms elapsed (times out at 10*50)
        std::int8_t target_slot = 0;  // pursued opponent's player index
        std::int8_t step_dir = 0;     // godir of the next step toward it
    } enemy_seek;
};

}  // namespace bomber::sim
