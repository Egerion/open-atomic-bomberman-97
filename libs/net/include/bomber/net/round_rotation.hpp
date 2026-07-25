#pragma once

#include <cstdint>

// MULTI-ROUND ONLINE MATCHES — the two numbers both peers must agree on for
// round N without exchanging a single extra byte.
//
// A match is a sequence of ROUNDS (docs/re/in-match-shell.md "The round-end
// shell": `sub_42A3F6` loops back into `sub_410B6E` for the next round until a
// player reaches the win target). Online, every peer must enter round N with a
// byte-identical `sim::MatchConfig` AND a tick space that cannot collide with
// the round it just left. Neither of those is on the wire, and neither needs to
// be: the round INDEX is derived identically on both peers (they see the same
// deterministic round outcomes, so they agree on the tally that ended the
// round), and the match seed is already inside the config the host confirmed
// before round 0. Both functions below are pure and total.
//
// WHAT STILL TRAVELS. The config itself is NOT re-derived from these numbers on
// each peer — the host builds round N's config and sends the whole thing
// (SetupSession::confirm, setup_session.hpp's "the final may not be
// approximate"), exactly as it did for round 0, because a guest whose .SCH,
// EXTRA<n>.RES or VALUELST differs would otherwise build a different board from
// the same inputs. `round_seed` is what makes that exchange SELF-CHECKING: the
// guest accepts a confirmed config only when its `seed` is the seed this round
// is supposed to have, so a replayed blob from an earlier round can never be
// mistaken for the next one.

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

// The first tick number of round `round_index`. Rounds are strided rather than
// each restarting at 0 so that a datagram straggling out of round N-1 always
// carries a tick BELOW round N's base and is rejected by RollbackSession's
// existing `tick < confirmed_` guard (see that class's `start_tick` doc) instead
// of being filed as a far-future input or compared as a phantom peer hash.
//
// The stride is 2^22 ticks = 4.19 M = ~58 hours at 20 Hz, comfortably past the
// longest round the game can produce (the Options play-time stepper tops out at
// 600 s, and even the 1001 "Infinite" sentinel is capped at 99999 s = 2.0 M
// ticks by MatchRunner::build_config). The level screen clamps the win target to
// 100, so the highest base a match can reach is 100 << 22 ≈ 4.2e8 — an order of
// magnitude inside uint32, with no wrap to reason about.
inline constexpr std::uint32_t kRoundTickStride = 1u << 22;

constexpr std::uint32_t round_tick_base(int round_index) {
    return static_cast<std::uint32_t>(round_index) * kRoundTickStride;
}

}  // namespace bomber::net
