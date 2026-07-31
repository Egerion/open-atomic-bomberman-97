#pragma once

// The colour-set index (0-9) a player's on-screen sprite/UI ink should use.
// SDL-free, pure — the presentation layer (libs/game) is the only caller, but
// the mapping itself has no rendering dependency, so it lives here where the
// headless test suite can pin it directly.

#include <cstdint>

#include "bomber/sim/constants.hpp"

namespace bomber::match {

// CONFIRMED from BM95.EXE (round init `sub_4214BC`, pseudo.c ~23916-23927):
// under Team Play the original overwrites each player's draw-colour byte (+60,
// the same byte the body blit, bomb-spawn and flame-owner colours all read) from
// the per-slot team byte, instead of leaving it as that player's slot index —
// team A -> colour 0 (white), team B -> colour 2 (red), the game's own existing
// slots rather than a bespoke team palette. Everything that inherits a player's
// colour follows: bombs, flames, carried-bomb icon, death animation.
//
// `team == 0` is "no team assigned" (Team Play off) and keeps the slot-indexed
// colour, matching the original's non-team branch.
inline int team_render_colour(std::uint8_t team, int slot) {
    if (team != 0) return team == 2 ? 2 : 0;
    if (slot < 0 || slot >= sim::kMaxPlayers) return 0;
    return slot;
}

// The round-start OWN-COLOUR REVEAL window (VALUELST id 32, default 40 frames
// = 2 s; docs/re/facts.md "Round-start own-colour reveal"). While it runs,
// `sub_41F29B` blits a player's BODY in their own slot colour rather than the
// draw-colour byte, so everyone can find their bomberman before the team colours
// take over. Only visible under Team Play.
//
// BODY ONLY: bombs and flames keep the colour stamped at creation, so callers
// must NOT route those here. The disease colour-strobe outranks this window, and
// that branch belongs to the caller (it draws from a presentation RNG, rule 6).
inline int round_start_body_colour(std::uint64_t round_tick, std::int64_t reveal_ticks,
                                   std::uint8_t team, int slot) {
    if (static_cast<std::int64_t>(round_tick) < reveal_ticks) {
        if (slot < 0 || slot >= sim::kMaxPlayers) return 0;
        return slot;
    }
    return team_render_colour(team, slot);
}

}  // namespace bomber::match
