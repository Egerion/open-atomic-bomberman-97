#pragma once

#include <cstdint>

#include "bomber/game_util/results.hpp"  // victory_background_name
#include "bomber/ui/screen.hpp"          // ScreenDef / WaitLoop

// THE OUTCOME TIER's shared vocabulary (sub_42A3F6): the music/sting ids and the
// two ScreenDef factories that DRAW.PCX and VICTORY<n>.PCX are built from.
//
// Promoted out of game_app.cpp's anonymous namespace because the tier has TWO
// drivers, not one: run_app's Results case for a LOCAL match and NetplayRunner's
// round loop for an ONLINE one. They must show the same screens and play the
// same cues — an online draw that skipped the tie-game sting, or a victory
// screen built with a different dwell, would be a divergence nothing tests.
// Keeping the definitions in one header is what makes that structural rather
// than a thing to remember.

namespace bomber::game {

// The outcome-tier music (sub_42A3F6), CORRECTED by
// docs/re/in-match-shell.md §2 (supersedes game_app.cpp's earlier "1020 under
// VICTORY" reading): at Round end sub_42741E(0x46A) = 1130 ("draw") replaces
// the stage track before the survivor test — so DRAW, the RESULTS tally, AND
// VICTORY/TEAM all play under 1130; nothing restarts 1020 anywhere in the
// outcome tier. A looping track (start_music), replacing the menu/stage music.
//
// "UNCONDITIONALLY" was too strong and is CORRECTED 2026-07-28
// (docs/re/sound-engine.md §9): the 1130 start at 0x42A6DD sits behind TWO
// gates that the round-loop exit passes through first.
//   * ATTRACT (0x42A6CB, `dword_464938`) — an attract round skips the whole
//     outcome tier. The port already bypasses it (run_app's attract_ branch).
//   * CAMPAIGN (0x42A63B, `dword_46489C`) — this one the port was getting
//     wrong. A campaign round end takes an entirely separate arm that shows at
//     most one modal (`sub_414340`, and only when the pacing flag dword_464894
//     is 2) and then goes STRAIGHT back into the round init sub_410B6E for the
//     next stage. It never reaches 1130, and it never reaches DRAW, the RESULTS
//     tally or VICTORY either.
inline constexpr int kDrawMusicId =
    1130;  // 0x46A — DRAW.RSS, DRAW *and* RESULTS *and* VICTORY backdrop

// Results: DRAW (no survivor / time up) or VICTORY<player> (one survivor). The
// original draws these with sub_42A088(name, 0) then a bespoke "any key, or 6 s
// in attract" loop (sub_42A3F6); we model it as a normal Screen with a bounded
// dwell so an unattended machine returns to the menu on its own.
inline constexpr std::uint32_t kResultsDwellMs = 6000;  // sub_42A3F6 attract auto-advance

inline ScreenDef draw_screen() {
    // DRAW.PCX. The draw sting is a ONE-SHOT group play (sub_427BFB(1700) picks a
    // random member of the contiguous "tie game/draw game" SOUNDLST run at 1700),
    // fired once by run_app on entering Results via audio_.play_sting —
    // NOT looped: a screen carries no music id, so nothing restarts the sting.
    // WaitLoop::RoundEnd: DRAW is NOT presented by sub_42A088's own wait loop.
    // sub_42A3F6 calls sub_42A088("draw", 0) — argument ZERO, i.e. show the
    // picture and return immediately (0x42A710) — and then runs its OWN loop at
    // 0x42A73A. That loop gives Escape no accept sting and gives the 6 s
    // auto-advance no nav blip. See ScreenDef::wait / the WaitLoop enum.
    return ScreenDef{"DRAW", {}, kResultsDwellMs, /*skippable*/ true, WaitLoop::RoundEnd};
}
// The SOUNDLST "tie game/draw game" voice group begins at 1700 (the file's own
// "; tie game/draw game" comment) and runs contiguously to its "1999 is the last
// tie game/draw game sound" bound; sub_427BFB(1700) plays a random member once.
// We span the full 1700..1999 group so play_sting can pick any loaded take
// (GUMP1/GEN11*/ZAA*/…), matching the original's variety.
inline constexpr int kDrawStingLo = 1700;
inline constexpr int kDrawStingHi = 1999;
// The "we have a winner" group, same shape: base 2000, SOUNDLST's own "2299 is
// the last we-have-a-winner sound" bound. sub_42A3F6 plays it with sub_427BFB
// too — so, like the draw sting, it goes through the UNCOUNTED voice. Both used
// to go through the counted 5-voice pool, where a busy results transition could
// drop the sting outright; the original's sting path cannot be refused.
inline constexpr int kWinnerStingLo = 2000;
inline constexpr int kWinnerStingHi = 2299;
// VICTORY<player>.PCX / TEAM<0/1>.PCX — the original resolves "victory%u"/
// "team%u" against the winner index / clinching team (sub_42A3F6 aVictoryU/
// aTeamU); see results.hpp's victory_background_name for the full RE
// citation. The "we have a winner" voice group (2000) is played by run_app's
// Results handler, under this screen, per §1 (fires as soon as v73 is
// computed). (ScreenDef.background owns its own std::string copy, so this is
// safe.)
inline ScreenDef victory_screen(bool team_mode, int player, int team) {
    // sub_42A3F6's VICTORY tail is sub_42A088(name, 0) (a CUT) then a hard
    // sub_413CB0(3000) — a fixed 3 s blocking sleep that pumps only OS messages
    // and reads NO game key. So the VICTORY/TEAM PCX shows for exactly 3 s and
    // cannot be skipped (unlike the 6 s keypress-skippable port model this
    // replaces). Non-skippable + 3000 ms reproduces both (Quit still exits).
    // WaitLoop::TimedCut because "reads NO game key" is also an AUDIO fact: with
    // no key loop there is no nav blip and no accept sting, and the timeout is a
    // sleep expiring rather than a synthesized Enter, so it stings nothing
    // either. This screen is completely silent apart from the 2000 winner voice
    // the caller fires under it.
    return ScreenDef{victory_background_name(team_mode, player, team),
                     {},
                     /*dwell_ms*/ 3000,
                     /*skippable*/ false,
                     WaitLoop::TimedCut};
}

}  // namespace bomber::game
