#pragma once

#include <cstdint>

// The CAMPAIGN round end, as pure decisions (docs/re/campaign.md "Round end").
//
// A campaign round does NOT go through the outcome tier. `sub_42A3F6`'s round
// loop tests the campaign flag `dword_46489C` at 0x42A63B and branches away
// before anything else: no 1130 music, no DRAW, no RESULTS tally, no VICTORY,
// no win award (`sub_421B56` @0x42A919) and no gold-player write — every one of
// those lives in the normal arm at 0x42A6A9 and beyond. What the campaign arm
// does instead is exactly two things: at most ONE modal, then the round init
// `sub_410B6E` @0x42A68B, which advances the stage and puts its own banner up.
//
// Both halves are decided by one variable, the round-pacing verdict
// `dword_464894`, so both halves live here as pure functions the headless suite
// pins (tests/game/test_campaign_round_end.cpp) — this is the branch that used
// to silently route a campaign round into the ordinary Results tier, and a
// screen-flow decision buried in an SDL loop is exactly the kind that regresses
// without anyone noticing.
//
// SDL-free, dependency-free, no sim types: the shell gathers the three inputs
// from the frozen round and this header decides. Presentation only — nothing
// here touches the sim or its RNG (ADR-0003).

namespace bomber::game {

// `dword_464894`, the campaign round-pacing verdict written by `sub_4016DA`
// (docs/re/campaign.md "Round pacing"). Cleared to 0 per stage at 0x401548
// (inside `sub_40151B`, the per-stage starter).
enum class CampaignVerdict : std::uint8_t {
    // Nothing has ended the round yet. The round-loop tail at 0x42A64B jumps
    // straight to the loop continuation for this value, so the original simply
    // keeps playing; the port only ever observes it on a Ctrl+Q/Esc abort,
    // which the original resolves through the menu sentinel `dword_464A68`
    // instead (0x42A56F/0x42A579) — i.e. straight out to the menu, with no
    // modal and no stage advance.
    Running = 0,
    // 0x401730 — "stage clear": every rover/ghost has been dead for the grace
    // window. The one verdict that gets NO banner.
    StageClear = 1,
    // 0x4016F7 (the round clock ran down) or 0x40178C (no human left alive).
    // The only verdict the 1240/1245 modal is shown for (0x42A657).
    RoundOver = 2,
};

// `sub_4016DA`'s three writes to `dword_464894`, in the order the routine
// performs them — the order is load-bearing, because a later clause OVERWRITES
// an earlier one in the same pass.
//
//   clause 2 @0x4016ED-0x4016F7:  `if (sub_410578() <= 1) dword_464894 = 2`
//   clause 3 @0x401701-0x401730:  `if (dword_464820 == 0)` accumulate
//                                 `dword_4646C0`, and once the grace has
//                                 elapsed `dword_464894 = 1`
//   clause 5 @0x401786-0x40178C:  reached only after the 0x40174D slot loop
//                                 falls through (no live non-COMPUTER slot):
//                                 `--dword_4648B0; dword_464894 = 2`
//
// CORRECTION 2026-07-30 — clause 2 is the ROUND CLOCK, not a survivor count.
// `sub_410578` @0x410578 is a two-line getter over the round timer:
// `(dword_4601A8 == 1001) ? 1001 : dword_4601A4`, where `dword_4601A8` is the
// configured limit in seconds (1001 being its "unlimited" sentinel, the same
// one `sub_41087D` @0x41088B tests to answer "clock expired") and
// `dword_4601A4` is the remaining whole seconds. `libs/sim`'s enclosure
// stepper already reads it that way (`EnclosureSystem::update`'s
// `seconds_left`); only campaign.md's own clause list had it as a survivor
// query. This matters: a campaign round has NO survivor-count end at all —
// `sub_421969` @0x421977 returns a CONSTANT 2 while `dword_46489C` is set, so
// the round loop's player-count guard at 0x42A62C can never fire either.
// Killing every AI opponent does not end a campaign stage; clearing the
// monsters or running out of clock does.
inline CampaignVerdict campaign_verdict(int seconds_left, bool hazards_clear_elapsed,
                                        bool no_human_survivor) {
    CampaignVerdict verdict = CampaignVerdict::Running;
    if (seconds_left <= 1) verdict = CampaignVerdict::RoundOver;
    if (hazards_clear_elapsed) verdict = CampaignVerdict::StageClear;
    if (no_human_survivor) verdict = CampaignVerdict::RoundOver;
    return verdict;
}

// Everything `sub_4016DA` leaves behind for the round-loop tail to read. Two
// values rather than one because clause 5 writes twice: the verdict (2, the
// same one a clock-out produces) and the `--dword_4648B0` at 0x401786 that
// cancels the increment `sub_40133F` is about to make. They are latched
// TOGETHER, at the instant the round ends, because our runner then lingers for
// the death animations while the original's loop leaves on that very pass — a
// human dying to a leftover flame during that linger must not retroactively
// turn a stage advance into a replay.
struct CampaignPacing {
    CampaignVerdict verdict = CampaignVerdict::Running;
    bool no_human_survivor = false;  // clause 5 fired
};

inline CampaignPacing campaign_pacing(int seconds_left, bool hazards_clear_elapsed,
                                      bool no_human_survivor) {
    return CampaignPacing{campaign_verdict(seconds_left, hazards_clear_elapsed, no_human_survivor),
                          no_human_survivor};
}

// What the round-loop tail does with that verdict.
struct CampaignRoundEnd {
    // 0x42A642's `je 0x42A6A9`: only a NON-campaign round reaches the outcome
    // tier (1130 + DRAW/RESULTS/VICTORY + the win tally + the wheel award).
    // Everything below is false whenever this is true.
    bool run_outcome_tier = true;
    // 0x42A657-0x42A686: `sub_414340` with getstring(1240)="Oh Well!" on top
    // and getstring(1245)="Campaign unsuccessful!" below, verdict 2 only.
    bool show_unsuccessful = false;
    // `sub_40133F`'s stage-exhausted branch (0x401372-0x40139D): the
    // "Congratulations!" modal, then `dword_464A68 = 10`, which the loop tail
    // at 0x42AFF8 reads as "leave for the menu".
    bool show_complete = false;
    // The other half of `sub_40133F`: a next stage exists, so its scheme/roster
    // load and the "Prepare to begin Campaign!" banner run, and 0x42AFF8 loops
    // back to 0x42A47C for the next round.
    bool next_stage = false;
    // `dword_4648B0` AFTER `sub_40133F`'s `++` at 0x40135F — and after clause
    // 5's `--` at 0x401786, which cancels it so the SAME stage replays.
    int next_stage_index = 0;
};

// The tail at 0x42A63B, given the verdict `sub_4016DA` left behind.
// `no_human_survivor` is passed separately from the verdict because clause 5
// writes TWO things: the verdict (2, same as a clock-out) and the stage
// decrement that makes this round replay rather than advance.
inline CampaignRoundEnd campaign_round_end(bool campaign_active, CampaignVerdict verdict,
                                           bool no_human_survivor, int stage_index,
                                           int stage_count) {
    CampaignRoundEnd plan;
    plan.next_stage_index = stage_index;
    if (!campaign_active) return plan;  // the normal arm, unchanged
    plan.run_outcome_tier = false;
    // Verdict 0 never reaches `sub_410B6E`: 0x42A64B jumps past the whole arm.
    if (verdict == CampaignVerdict::Running) return plan;
    plan.show_unsuccessful = verdict == CampaignVerdict::RoundOver;
    // `sub_410B6E` -> `sub_40133F`: the increment is part of the guard's own
    // condition (`++dword_4648B0 < dword_45E014`), so the index advances even
    // on the run where the comparison then fails and the campaign is over.
    // Clause 5's decrement already ran, inside `sub_4016DA`, so a round with no
    // human survivor nets zero here and replays the same stage — which is also
    // why the exhausted branch can never be reached on a replay.
    const int next = no_human_survivor ? stage_index : stage_index + 1;
    plan.next_stage_index = next;
    if (next >= stage_count)
        plan.show_complete = true;
    else
        plan.next_stage = true;
    return plan;
}

}  // namespace bomber::game
