// FNV-1a digest of the gameplay state. Every field that influences gameplay
// MUST be mixed in here; events are derived per-tick outputs and are excluded.
// The exact byte layout is part of the golden-hash contract
// (tests/test_golden.cpp) — change it only deliberately.

#include "bomber/sim/simulation.hpp"

namespace bomber::sim {

std::uint64_t state_hash(const State& s) {
    std::uint64_t h = 1469598103934665603ULL;
    auto mix = [&h](std::uint64_t v) {
        for (int i = 0; i < 8; ++i) {
            h ^= (v >> (i * 8)) & 0xFF;
            h *= 1099511628211ULL;
        }
    };
    mix(s.tick);
    mix(s.rng);
    mix(static_cast<std::uint64_t>(static_cast<std::uint32_t>(s.ticks_left)));
    mix(static_cast<std::uint64_t>(s.hurry) |
        (static_cast<std::uint64_t>(static_cast<std::uint32_t>(s.enclose_index)) << 8) |
        (static_cast<std::uint64_t>(static_cast<std::uint32_t>(s.enclose_timer)) << 40));
    mix(s.dud_gate);
    for (int y = 0; y < kGridHeight; ++y) {
        for (int x = 0; x < kGridWidth; ++x) {
            mix(static_cast<std::uint64_t>(s.cells[y][x]) |
                (static_cast<std::uint64_t>(s.hidden[y][x]) << 8) |
                (static_cast<std::uint64_t>(s.floor[y][x]) << 16) |
                (static_cast<std::uint64_t>(s.flame[y][x]) << 24) |
                (static_cast<std::uint64_t>(s.burning[y][x]) << 32) |
                (static_cast<std::uint64_t>(s.flame_owner[y][x]) << 40));
        }
    }
    // Stage-actor layout (docs/re/stage-actors.md): static per match but
    // gameplay-affecting like cells, so it must be hashed. Packed one word per
    // tile: low byte = actor_type, next = actor_dir, then the warphole exit
    // tile (warp_dest_x, warp_dest_y). The warp bytes are 0 on non-warp tiles,
    // so a board with no warpholes hashes identically to before this field
    // existed (the golden scenarios place no actors → unchanged).
    for (int y = 0; y < kGridHeight; ++y) {
        for (int x = 0; x < kGridWidth; ++x) {
            mix(static_cast<std::uint64_t>(static_cast<std::uint8_t>(s.actor_type[y][x])) |
                (static_cast<std::uint64_t>(s.actor_dir[y][x]) << 8) |
                (static_cast<std::uint64_t>(s.warp_dest_x[y][x]) << 16) |
                (static_cast<std::uint64_t>(s.warp_dest_y[y][x]) << 24));
        }
    }
    for (const auto& p : s.players) {
        if (!p.present) {
            mix(0xEE);
            continue;
        }
        mix(static_cast<std::uint64_t>(static_cast<std::uint32_t>(p.x)) |
            (static_cast<std::uint64_t>(static_cast<std::uint32_t>(p.y)) << 32));
        mix(static_cast<std::uint64_t>(p.alive) | (static_cast<std::uint64_t>(p.max_bombs) << 8) |
            (static_cast<std::uint64_t>(p.flame) << 16) |
            (static_cast<std::uint64_t>(p.skates) << 24) |
            (static_cast<std::uint64_t>(p.speed) << 32) |
            (static_cast<std::uint64_t>(p.kick) << 48) |
            (static_cast<std::uint64_t>(p.punch) << 49) |
            (static_cast<std::uint64_t>(p.grab) << 50) |
            (static_cast<std::uint64_t>(p.carrying) << 51) |
            (static_cast<std::uint64_t>(p.spooge) << 52) |
            (static_cast<std::uint64_t>(p.goldflame) << 53) |
            (static_cast<std::uint64_t>(p.trigger) << 54) |
            (static_cast<std::uint64_t>(p.jelly) << 55) |
            (static_cast<std::uint64_t>(p.bombs_placed) << 56) |
            // Stage-actor re-entry latches (#7): gameplay state (they gate re-
            // warp / re-bounce), so hashed. Both 0 on boards with no warpholes/
            // trampolines → golden scenarios unchanged. See stage-actors.md §4-5.
            (static_cast<std::uint64_t>(p.tramp_latch) << 62) |
            (static_cast<std::uint64_t>(p.warp_latch) << 63));
        // Trigger-bomb allowance (player byte +85): its own word so the counter
        // is not truncated. Part of the hashed contract now that #9 caps it.
        mix(static_cast<std::uint64_t>(static_cast<std::uint32_t>(p.trigger_placed)));
        // Trampoline bounce countdown (Player::bounce, #7), warp countdown
        // (Player::warp) and the pending warp destination tile (warp_to_x/y,
        // captured at step-on): all gate/drive an in-flight warp or bounce, so
        // they are gameplay state and MUST be hashed. Packed into one word —
        // bounce (≤30) / warp (≤18) / dest x,y (≤14) each fit a byte. ALL are 0
        // on boards with no trampolines/warpholes, so this word is mix(0) there,
        // byte-identical to before warp_to_* existed → golden scenarios (no
        // actors) unchanged. See docs/re/stage-actors.md §4-5.
        mix(static_cast<std::uint64_t>(static_cast<std::uint32_t>(p.bounce) & 0xFF) |
            (static_cast<std::uint64_t>(static_cast<std::uint32_t>(p.warp) & 0xFF) << 8) |
            (static_cast<std::uint64_t>(static_cast<std::uint32_t>(p.warp_to_x) & 0xFF) << 16) |
            (static_cast<std::uint64_t>(static_cast<std::uint32_t>(p.warp_to_y) & 0xFF) << 24));
        if (p.carrying)
            mix(static_cast<std::uint64_t>(static_cast<std::uint32_t>(p.carried_fuse)) |
                (static_cast<std::uint64_t>(p.carried_flame) << 32) |
                (static_cast<std::uint64_t>(p.carried_owner) << 48) |
                // Carried bomb kind (set on the thrown bomb in throw_carried) —
                // hashed state while held, not just at throw time.
                (static_cast<std::uint64_t>(p.carried_jelly) << 56) |
                (static_cast<std::uint64_t>(p.carried_trigger) << 57));
        mix(static_cast<std::uint64_t>(static_cast<std::uint32_t>(p.stun)) |
            (static_cast<std::uint64_t>(static_cast<std::uint32_t>(p.move_budget)) << 32));
        std::uint32_t dbits = 0;
        for (int k = 0; k < kDiseaseKinds; ++k)
            if (p.disease[k]) dbits |= (1u << k);
        mix(static_cast<std::uint64_t>(dbits) |
            (static_cast<std::uint64_t>(static_cast<std::uint32_t>(p.disease_timer)) << 16) |
            (static_cast<std::uint64_t>(static_cast<std::uint32_t>(p.disease_fresh)) << 40));
    }
    for (const auto& b : s.bombs) {
        mix(static_cast<std::uint64_t>(static_cast<std::uint32_t>(b.x)) |
            (static_cast<std::uint64_t>(static_cast<std::uint32_t>(b.y)) << 32));
        mix(static_cast<std::uint64_t>(static_cast<std::uint32_t>(b.fuse)) |
            (static_cast<std::uint64_t>(b.flame) << 32) |
            (static_cast<std::uint64_t>(b.moving) << 40) |
            (static_cast<std::uint64_t>(b.flying) << 41) |
            // Bomb warphole latch (stage-actors.md §6): 0 on non-warp boards →
            // golden E (no warpholes) unchanged.
            (static_cast<std::uint64_t>(b.warp_latch) << 42) |
            (static_cast<std::uint64_t>(b.owner) << 48) |
            (static_cast<std::uint64_t>(static_cast<std::uint32_t>(b.fly_ticks) & 0x3F) << 56));
        mix(static_cast<std::uint64_t>(static_cast<std::uint32_t>(b.dud_left)));
    }
    return h;
}

}  // namespace bomber::sim
