#pragma once

#include <cstdint>

// MULTI-ROUND ONLINE MATCHES — the two numbers both peers must agree on for
// round N without exchanging a single extra byte. Neither is on the wire and
// neither needs to be: the round INDEX is derived identically on both peers (the
// round outcomes are deterministic, so they agree on the tally), and the match
// seed is inside the config the host confirmed before round 0. Both functions
// are pure and total.
//
// WHAT STILL TRAVELS: the config itself, whole, exactly as for round 0 — a guest
// whose .SCH, EXTRA<n>.RES or VALUELST differs would otherwise build a different
// board from the same inputs (setup_session.hpp). `round_seed` makes that
// exchange SELF-CHECKING: the guest accepts a confirmed config only when its
// `seed` is the one this round is supposed to have, so a replayed blob from an
// earlier round can never be mistaken for the next one.

namespace bomber::net {

// The seed for round `round_index` (0 = the first round of the match) of a match
// whose agreed seed is `match_seed`. Deliberately the SAME +1-per-round walk the
// local path uses (`MatchRunner::run`'s `start_match(next_seed++)`), so an
// online match's board sequence is the one a local match would have had from the
// same starting seed — including the RANDOM-level rotation, which `build_config`
// derives from this very seed via `match::pick_stage`.
constexpr std::uint32_t round_seed(std::uint32_t match_seed, int round_index) {
    return match_seed + static_cast<std::uint32_t>(round_index);
}

// The first tick number of round `round_index`. Rounds are STRIDED rather than
// each restarting at 0, so a datagram straggling out of round N-1 always carries
// a tick BELOW round N's base and is dropped by RollbackSession's existing
// `tick < confirmed_` guard instead of being filed as a far-future input.
//
// 2^22 ticks = ~58 hours at 20 Hz, comfortably past the longest round the game
// can produce (even the "Infinite" sentinel is capped at 99999 s = 2.0 M ticks).
// The win target is clamped to 100, so the highest base is 100 << 22 ~= 4.2e8 —
// an order of magnitude inside uint32, with no wrap to reason about.
inline constexpr std::uint32_t kRoundTickStride = 1u << 22;

constexpr std::uint32_t round_tick_base(int round_index) {
    return static_cast<std::uint32_t>(round_index) * kRoundTickStride;
}

}  // namespace bomber::net
