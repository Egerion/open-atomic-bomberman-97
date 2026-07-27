#pragma once

// The colour-set index (0-9) a player's on-screen sprite/UI ink should use.
// SDL-free, pure — the presentation layer (libs/game) is the only caller, but
// the mapping itself has no rendering dependency, so it lives here where the
// headless test suite can pin it directly.

#include <cstdint>

#include "bomber/sim/constants.hpp"

namespace bomber::match {

// CONFIRMED from BM95.EXE (pseudo.c ~23916-23927, round init `sub_4214BC`
// @0x4214BC): under Team Play (`dword_464964`) the original overwrites every
// player's draw-colour byte (+60 — the SAME byte the body blit, bomb-spawn
// colour, flame-owner colour, etc. all read, docs/re/player-colour.md) from
// the per-slot team byte (+84) instead of leaving it as that player's own
// slot index. Per player, in the round-init loop:
//
//   dword_464964 (Team Play) set   -> colour byte +60 = 2 when the team byte
//                                     +84 is nonzero, else 0
//   dword_464964 clear             -> colour byte +60 = this player's own slot
//                                     index
//
// i.e. team A -> colour 0 (0.RMP, white), team B -> colour 2 (2.RMP, red) —
// the game's own white/red slots, not a bespoke team palette. This is the fact
// behind the report "Team Play splits the roster into red and white": every
// teammate's sprite (and every object that inherits a player's colour: bombs,
// flames, carried-bomb icon, death animation) is forced to one of these two
// existing colour sets.
//
// Our setup screen maps the 0/1 setup team byte to sim teams 1/2 (both real
// teams under Team Play, docs/re/setup-screens.md "sub_4141F8"), so
// `sim::Player::team == 1` -> colour 0 (white), `== 2` -> colour 2 (red).
// `team == 0` means "no team assigned" (Team Play off), which keeps the
// player's own slot-indexed colour — unchanged behaviour, matching the
// original's non-team branch.
inline int team_render_colour(std::uint8_t team, int slot) {
    if (team != 0) return team == 2 ? 2 : 0;
    if (slot < 0 || slot >= sim::kMaxPlayers) return 0;
    return slot;
}

}  // namespace bomber::match
