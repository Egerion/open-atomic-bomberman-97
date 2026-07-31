#pragma once

#include <cstdint>

// THE F9 CADENCE LEVER, RESOLVED IN ONE PLACE.
//
// Native cadence (F9, ADR-0007) advances the sim once per DISPLAYED FRAME on the
// measured wall-clock delta (Simulation::frame) — the original's own gameplay
// driver, validated 1:1 against it, and NON-DETERMINISTIC by construction: the
// delta differs on every machine, so two netplay peers running it desync
// immediately. Netplay is fixed-tick 20 Hz whatever the key says.
//
// FOUR call sites consume the lever — the sim advance, the animation clock, the
// entity glide and the interpolation alpha — and the netplay rule used to be
// written at only the first. The other three read the raw flag, so an online
// match with F9 on ran the sim correctly at 20 Hz while the renderer pinned
// `interp_alpha` to 1.0, switching inter-tick interpolation OFF: pure loss, and
// nothing in the game notices when one of four disagrees. Deriving them together
// is what stops that recurring.

namespace bomber::game {

// Everything the presentation derives from the cadence lever for one frame.
struct MatchCadence {
    // Renderer::set_native_cadence — per-frame walk/fidget phase advance.
    bool native = false;
    // Renderer::set_entity_interp — glide fraction for the 50 ms-stepped entities
    // (flying/sliding bombs, rovers). 1.0 means "no glide, draw the stepped pose".
    float entity_interp = 1.0f;
    // Renderer::draw_frame's inter-tick blend, in [0, 1].
    float interp_alpha = 0.0f;
};

// `systems_accum_ms` is only meaningful on the Simulation::frame path — netplay
// never takes it, so a netplay frame must not read it.
inline MatchCadence match_cadence(bool lever, bool netplay, int systems_accum_ms, int ms_per_tick,
                                  std::uint64_t acc_ns, std::uint64_t tick_ns) {
    MatchCadence c;
    c.native = lever && !netplay;
    if (c.native) {
        c.entity_interp = ms_per_tick == 0 ? 1.0f
                                           : static_cast<float>(systems_accum_ms) /
                                                 static_cast<float>(ms_per_tick);
        // The sim already ran at frame rate this frame, so there is nothing to
        // blend: player_interp/interp_pos return the current position at 1.0.
        c.interp_alpha = 1.0f;
        return c;
    }
    c.entity_interp = 1.0f;
    c.interp_alpha =
        tick_ns == 0 ? 0.0f : static_cast<float>(acc_ns) / static_cast<float>(tick_ns);
    // A netplay stall (or a re-phase hold) leaves the accumulator at or above one
    // tick with the sim frozen, which would push alpha past 1 and EXTRAPOLATE
    // entities forward during the pause; hold the last simulated pose instead. A
    // no-op on every other path, where the catch-up loop always drains acc below
    // one tick before we get here.
    if (c.interp_alpha > 1.0f) c.interp_alpha = 1.0f;
    return c;
}

}  // namespace bomber::game
