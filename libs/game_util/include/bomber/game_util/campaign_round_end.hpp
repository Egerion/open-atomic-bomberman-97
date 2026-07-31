#pragma once

#include <cstdint>

// The CAMPAIGN round end, as pure decisions (docs/re/campaign.md "Round end").
//
// A campaign round does NOT go through the outcome tier. `sub_42A3F6`'s round
// loop tests `dword_46489C` at 0x42A63B and branches away first: no 1130 music,
// no DRAW, no RESULTS tally, no VICTORY, no win award (`sub_421B56` @0x42A919),
// no gold-player write — all of those live in the normal arm at 0x42A6A9 and
// beyond. The campaign arm does exactly two things: at most ONE modal, then the
// round init `sub_410B6E` @0x42A68B, which advances the stage and banners it.

namespace bomber::game {

// `dword_464894`, written by `sub_4016DA`. Cleared to 0 per stage at 0x401548
// (inside `sub_40151B`, the per-stage starter).
enum class CampaignVerdict : std::uint8_t {
    // Nothing has ended the round; 0x42A64B jumps straight to the loop
    // continuation. The port only observes it on a Ctrl+Q/Esc abort, which the
    // original resolves through the menu sentinel `dword_464A68` instead
    // (0x42A56F/0x42A579) — out to the menu, no modal, no stage advance.
    Running = 0,
    // 0x401730 "stage clear": every rover/ghost dead for the grace window. The
    // one verdict that gets NO banner.
    StageClear = 1,
    // 0x4016F7 (clock ran down) or 0x40178C (no human left alive). The only
    // verdict the 1240/1245 modal is shown for (0x42A657).
    RoundOver = 2,
};

// `sub_4016DA`'s three writes, IN THE ORDER THE ROUTINE PERFORMS THEM — the
// order is load-bearing, because a later clause OVERWRITES an earlier one in the
// same pass.
//
//   clause 2 @0x4016ED-0x4016F7:  `if (sub_410578() <= 1) dword_464894 = 2`
//   clause 3 @0x401701-0x401730:  accumulate `dword_4646C0` while
//                                 `dword_464820 == 0`; on grace, = 1
//   clause 5 @0x401786-0x40178C:  after the 0x40174D slot loop falls through
//                                 (no live non-COMPUTER slot):
//                                 `--dword_4648B0; dword_464894 = 2`
//
// CORRECTION 2026-07-30 — clause 2 is the ROUND CLOCK, not a survivor count.
// `sub_410578` @0x410578 is a getter over the round timer:
// `(dword_4601A8 == 1001) ? 1001 : dword_4601A4`, the configured limit in
// seconds (1001 = "unlimited", the sentinel `sub_41087D` @0x41088B tests) or the
// remaining seconds. A campaign round has NO survivor-count end at all:
// `sub_421969` @0x421977 returns a CONSTANT 2 while dword_46489C is set, so the
// loop's player-count guard at 0x42A62C can never fire. Killing every AI
// opponent does not end a stage; clearing the monsters or the clock does.
inline CampaignVerdict campaign_verdict(int seconds_left, bool hazards_clear_elapsed,
                                        bool no_human_survivor) {
    CampaignVerdict verdict = CampaignVerdict::Running;
    if (seconds_left <= 1) verdict = CampaignVerdict::RoundOver;
    if (hazards_clear_elapsed) verdict = CampaignVerdict::StageClear;
    if (no_human_survivor) verdict = CampaignVerdict::RoundOver;
    return verdict;
}

// Two values because clause 5 writes twice: the verdict (2, same as a clock-out)
// and the `--dword_4648B0` at 0x401786 that cancels the increment `sub_40133F`
// is about to make. They are latched TOGETHER, at the instant the round ends,
// because our runner lingers for the death animations while the original's loop
// leaves on that pass — a human dying to a leftover flame during the linger must
// not retroactively turn a stage advance into a replay.
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
    // tier. Everything below is false whenever this is true.
    bool run_outcome_tier = true;
    // 0x42A657-0x42A686: `sub_414340` with getstring(1240)="Oh Well!" over
    // getstring(1245)="Campaign unsuccessful!", verdict 2 only.
    bool show_unsuccessful = false;
    // `sub_40133F`'s stage-exhausted branch (0x401372-0x40139D): the
    // "Congratulations!" modal, then `dword_464A68 = 10`, which the tail at
    // 0x42AFF8 reads as "leave for the menu".
    bool show_complete = false;
    // The other half of `sub_40133F`: a next stage exists, so its scheme/roster
    // load and the "Prepare to begin Campaign!" banner run.
    bool next_stage = false;
    // `dword_4648B0` AFTER `sub_40133F`'s `++` at 0x40135F — and after clause
    // 5's `--` at 0x401786, which cancels it so the SAME stage replays.
    int next_stage_index = 0;
};

// The tail at 0x42A63B. `no_human_survivor` is passed separately from the
// verdict because clause 5 writes TWO things.
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
    // condition (`++dword_4648B0 < dword_45E014`), so the index advances even on
    // the run where the comparison then fails and the campaign is over. Clause
    // 5's decrement already ran inside `sub_4016DA`, so a round with no human
    // survivor nets zero and replays — which is also why the exhausted branch
    // can never be reached on a replay.
    const int next = no_human_survivor ? stage_index : stage_index + 1;
    plan.next_stage_index = next;
    plan.show_complete = next >= stage_count;
    plan.next_stage = next < stage_count;
    return plan;
}

}  // namespace bomber::game
