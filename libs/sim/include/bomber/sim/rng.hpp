#pragma once

#include <cstdint>

#include "bomber/sim/state.hpp"

// The simulation's only source of randomness: a xorshift32 stream stored in
// State::rng. The ORDER of draws is part of the determinism contract — never
// add, remove, or reorder draws inside a tick without updating the golden
// hashes (tests/test_golden.cpp) deliberately.

namespace bomber::sim {

inline std::uint32_t next_random(State& state) {
    std::uint32_t x = state.rng;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    state.rng = x;
    return x;
}

// Uniform value in [0, n) drawn from the sim RNG (n == 0 draws nothing).
inline std::uint32_t random_below(State& state, std::uint32_t n) {
    return n == 0 ? 0 : next_random(state) % n;
}

}  // namespace bomber::sim
