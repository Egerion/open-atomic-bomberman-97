#pragma once

#include <cstdint>

#include "bomber/game_util/results.hpp"  // victory_background_name
#include "bomber/ui/screen.hpp"          // ScreenDef / WaitLoop

// THE OUTCOME TIER's shared vocabulary (sub_42A3F6): the music/sting ids and the
// two ScreenDef factories DRAW.PCX and VICTORY<n>.PCX are built from. One header
// rather than a definition per driver, because the tier has TWO — the local
// Results case and the netplay round loop — and an online draw that skipped the
// tie-game sting would be a divergence nothing tests.

namespace bomber::game {

// sub_42741E(0x46A) replaces the stage track BEFORE the survivor test, so DRAW,
// the RESULTS tally and VICTORY/TEAM all play under it and nothing restarts 1020
// anywhere in the tier (superseding an earlier "1020 under VICTORY" reading). NOT
// unconditional — CORRECTED 2026-07-28: an ATTRACT round skips the whole tier, and
// a CAMPAIGN round end takes a separate arm that reaches neither 1130 nor DRAW,
// RESULTS or VICTORY (docs/re/sound-engine.md §9).
inline constexpr int kDrawMusicId = 1130;  // 0x46A — DRAW.RSS

// sub_42A3F6's attract auto-advance, modelled as a bounded Screen dwell so an
// unattended machine returns to the menu on its own.
inline constexpr std::uint32_t kResultsDwellMs = 6000;

// WaitLoop::RoundEnd because DRAW is NOT presented by sub_42A088's own wait loop:
// sub_42A3F6 calls sub_42A088("draw", 0) — argument ZERO, show and return
// immediately — then runs its OWN loop at 0x42A73A, which gives Escape no accept
// sting and the 6 s auto-advance no nav blip.
inline ScreenDef draw_screen() {
    return ScreenDef{"DRAW", {}, kResultsDwellMs, /*skippable*/ true, WaitLoop::RoundEnd};
}

// The two one-shot sub_427BFB voice groups, spanning SOUNDLST's own contiguous
// runs so play_sting can pick any loaded take. Both go through the UNCOUNTED
// voice: they used to go through the counted 5-voice pool, where a busy results
// transition could drop the sting outright, and the original's sting path cannot
// be refused.
inline constexpr int kDrawStingLo = 1700;  // "tie game/draw game"
inline constexpr int kDrawStingHi = 1999;
inline constexpr int kWinnerStingLo = 2000;  // "we have a winner"
inline constexpr int kWinnerStingHi = 2299;

// VICTORY<player>.PCX / TEAM<0/1>.PCX (results.hpp's victory_background_name has
// the name-resolution citation). sub_42A3F6's tail is a CUT then a hard
// sub_413CB0(3000) — a blocking sleep that reads NO game key — so the screen shows
// for exactly 3 s and cannot be skipped. WaitLoop::TimedCut because "reads no key"
// is also an AUDIO fact: no nav blip, no accept sting, and the timeout is a sleep
// expiring rather than a synthesized Enter.
inline ScreenDef victory_screen(bool team_mode, int player, int team) {
    return ScreenDef{victory_background_name(team_mode, player, team),
                     {},
                     /*dwell_ms*/ 3000,
                     /*skippable*/ false,
                     WaitLoop::TimedCut};
}

}  // namespace bomber::game
