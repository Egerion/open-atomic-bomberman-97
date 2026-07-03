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
            (static_cast<std::uint64_t>(p.bombs_placed) << 56));
        if (p.carrying)
            mix(static_cast<std::uint64_t>(static_cast<std::uint32_t>(p.carried_fuse)) |
                (static_cast<std::uint64_t>(p.carried_flame) << 32) |
                (static_cast<std::uint64_t>(p.carried_owner) << 48));
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
            (static_cast<std::uint64_t>(b.owner) << 48) |
            (static_cast<std::uint64_t>(static_cast<std::uint32_t>(b.fly_ticks) & 0x3F) << 56));
    }
    return h;
}

}  // namespace bomber::sim
