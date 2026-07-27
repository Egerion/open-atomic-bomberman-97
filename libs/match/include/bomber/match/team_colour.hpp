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
// slot index:
//
//   if (dword_464964)
//       *(byte*)(v6+60) = *(byte*)(v6+84) ? 2 : 0;
//   else
//       *(byte*)(v6+60) = v8;               // v8 = this player's own slot i
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

// The round-start OWN-COLOUR REVEAL window (VALUELST id 32, default 40
// frames = 2 s), CONFIRMED from BM95.EXE — `docs/re/facts.md` "Round-start
// own-colour reveal". Round init (`sub_4214BC`) arms `dword_4621E8` = 50 ms ×
// getvalue(32) alongside the input freeze; `sub_420F07` counts it down; while
// it is non-zero the player draw routine `sub_41F29B` blits the player's BODY
// in that player's OWN slot index instead of the +60 draw-colour byte, so
// everyone can find their bomberman before the team colours take over.
//
// Only VISIBLE under Team Play: with `team == 0` both branches yield the same
// slot index. Body-only in the original — bombs and flames keep the colour
// they were stamped with at creation, so callers must NOT route those here.
//
// The disease colour-strobe takes precedence over this window in the native's
// three-way branch; that branch is the CALLER's (it uses a presentation-side
// RNG, determinism rule 6), so this helper covers only branches 2 and 3.
//
// Lives here, beside `team_render_colour`, for the same reason: the mapping is
// SDL-free and pure, so the headless suite can pin it without libs/game.
inline int round_start_body_colour(std::uint64_t round_tick, std::int64_t reveal_ticks,
                                   std::uint8_t team, int slot) {
    if (static_cast<std::int64_t>(round_tick) < reveal_ticks) {
        if (slot < 0 || slot >= sim::kMaxPlayers) return 0;
        return slot;
    }
    return team_render_colour(team, slot);
}

}  // namespace bomber::match
