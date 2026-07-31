#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "bomber/game_util/campaign_round_end.hpp"

// The CAMPAIGN round end (docs/re/campaign.md "Round end", sub_42A3F6 @0x42A63B
// + sub_4016DA). Every case here is a screen-flow decision that used to live
// inside an SDL loop and could only be checked by playing a hidden mode through
// a five-keypress easter egg — which is precisely why the port shipped the wrong
// one: a campaign round end went through the ordinary DRAW/RESULTS/VICTORY tier
// the original never shows, and the stage only advanced once a whole best-of-N
// match had been clinched.

using namespace bomber::game;

namespace {

// The three shipped campaign files are small; three stages is enough to
// exercise "advance", "replay" and "that was the last one".
constexpr int kStages = 3;

CampaignRoundEnd plan_at(CampaignVerdict verdict, bool no_human, int stage) {
    return campaign_round_end(true, verdict, no_human, stage, kStages);
}

}  // namespace

TEST_CASE("a NON-campaign round end still runs the ordinary outcome tier, untouched") {
    // 0x42A642's `je 0x42A6A9`. This is the whole guarantee that normal play is
    // unaffected: with the flag clear the plan says "outcome tier" and nothing
    // else, whatever the other inputs read.
    for (CampaignVerdict v : {CampaignVerdict::Running, CampaignVerdict::StageClear,
                              CampaignVerdict::RoundOver}) {
        for (bool no_human : {false, true}) {
            const CampaignRoundEnd plan = campaign_round_end(false, v, no_human, 1, kStages);
            CHECK(plan.run_outcome_tier);
            CHECK_FALSE(plan.show_unsuccessful);
            CHECK_FALSE(plan.show_complete);
            CHECK_FALSE(plan.next_stage);
            CHECK(plan.next_stage_index == 1);  // the stage index is left alone
        }
    }
}

TEST_CASE("a campaign round end never runs the outcome tier, whatever the verdict") {
    for (CampaignVerdict v : {CampaignVerdict::Running, CampaignVerdict::StageClear,
                              CampaignVerdict::RoundOver}) {
        CHECK_FALSE(plan_at(v, false, 0).run_outcome_tier);
    }
}

TEST_CASE("verdict 1 (stage clear) advances with NO banner at all") {
    // 0x42A65E's `jne 0x42A68B` skips the modal for any verdict but 2, then
    // sub_410B6E advances the stage regardless.
    const CampaignRoundEnd plan = plan_at(CampaignVerdict::StageClear, false, 0);
    CHECK_FALSE(plan.show_unsuccessful);
    CHECK(plan.next_stage);
    CHECK_FALSE(plan.show_complete);
    CHECK(plan.next_stage_index == 1);
}

TEST_CASE("verdict 2 shows the 1240/1245 banner AND still advances the stage") {
    // The banner is not an alternative to the advance: 0x42A686 falls straight
    // into the sub_410B6E call at 0x42A68B.
    const CampaignRoundEnd plan = plan_at(CampaignVerdict::RoundOver, false, 0);
    CHECK(plan.show_unsuccessful);
    CHECK(plan.next_stage);
    CHECK(plan.next_stage_index == 1);
}

TEST_CASE("no human survivor REPLAYS the same stage (clause 5's -- cancels the ++)") {
    const CampaignRoundEnd plan = plan_at(CampaignVerdict::RoundOver, true, 1);
    CHECK(plan.show_unsuccessful);  // clause 5 forces verdict 2, so the banner shows
    CHECK(plan.next_stage);
    CHECK_FALSE(plan.show_complete);
    CHECK(plan.next_stage_index == 1);  // NOT 2 — the same stage runs again
}

TEST_CASE("a replay of the LAST stage replays it rather than ending the campaign") {
    // The decrement guarantees this: sub_40133F's `++dword_4648B0 <
    // dword_45E014` can only fail when the index really did advance, so a
    // wipe-out on the final stage can never be mistaken for finishing it.
    const CampaignRoundEnd plan = plan_at(CampaignVerdict::RoundOver, true, kStages - 1);
    CHECK(plan.next_stage);
    CHECK_FALSE(plan.show_complete);
    CHECK(plan.next_stage_index == kStages - 1);
}

TEST_CASE("clearing the last stage shows Congratulations and leaves for the menu") {
    // sub_40133F @0x401372: the ++ has run and lost the comparison, so
    // getstring(1220)/(1225) goes up and dword_464A68 = 10 sends the loop tail
    // at 0x42AFF8 out to the menu instead of round again.
    const CampaignRoundEnd plan = plan_at(CampaignVerdict::StageClear, false, kStages - 1);
    CHECK(plan.show_complete);
    CHECK_FALSE(plan.next_stage);
    CHECK_FALSE(plan.show_unsuccessful);
    CHECK(plan.next_stage_index == kStages);
}

TEST_CASE("running out of clock on the last stage banners AND congratulates") {
    // Both modals, in that order — the original runs the 0x42A660 block and then
    // calls into sub_40133F, which puts its own up.
    const CampaignRoundEnd plan = plan_at(CampaignVerdict::RoundOver, false, kStages - 1);
    CHECK(plan.show_unsuccessful);
    CHECK(plan.show_complete);
    CHECK_FALSE(plan.next_stage);
}

TEST_CASE("verdict 0 is an abort: no banner, no advance, straight out") {
    // 0x42A64B jumps clean past the arm. The port only observes this after a
    // Ctrl+Q/Esc forfeit, which in the original has already set the menu
    // sentinel — so the stage must not move and no screen may appear.
    const CampaignRoundEnd plan = plan_at(CampaignVerdict::Running, false, 1);
    CHECK_FALSE(plan.run_outcome_tier);
    CHECK_FALSE(plan.show_unsuccessful);
    CHECK_FALSE(plan.show_complete);
    CHECK_FALSE(plan.next_stage);
    CHECK(plan.next_stage_index == 1);
}

TEST_CASE("an aborted round does not end the campaign even on the last stage") {
    const CampaignRoundEnd plan = plan_at(CampaignVerdict::Running, false, kStages - 1);
    CHECK_FALSE(plan.show_complete);
    CHECK(plan.next_stage_index == kStages - 1);
}

TEST_CASE("an empty stage list cannot produce a next stage") {
    const CampaignRoundEnd plan = campaign_round_end(true, CampaignVerdict::StageClear, false, 0, 0);
    CHECK(plan.show_complete);
    CHECK_FALSE(plan.next_stage);
}

TEST_CASE("campaign_verdict: a running round with time left has no verdict") {
    CHECK(campaign_verdict(90, false, false) == CampaignVerdict::Running);
}

TEST_CASE("campaign_verdict: clause 2 is the CLOCK, at one whole second or less") {
    // sub_410578 @0x410578 returns the REMAINING WHOLE SECONDS (dword_4601A4),
    // or the 1001 "no limit" sentinel; `cmp eax,1 / jg` @0x4016F2 makes the
    // threshold <= 1, not < 1.
    CHECK(campaign_verdict(2, false, false) == CampaignVerdict::Running);
    CHECK(campaign_verdict(1, false, false) == CampaignVerdict::RoundOver);
    CHECK(campaign_verdict(0, false, false) == CampaignVerdict::RoundOver);
    CHECK(campaign_verdict(1001, false, false) == CampaignVerdict::Running);  // unlimited
}

TEST_CASE("campaign_verdict: clause 3 OVERWRITES a clock-out in the same pass") {
    // Order is the contract: 0x401701's block runs after 0x4016F7's write, so a
    // stage cleared on the very last second is a CLEAR, not a failure.
    CHECK(campaign_verdict(0, true, false) == CampaignVerdict::StageClear);
    CHECK(campaign_verdict(90, true, false) == CampaignVerdict::StageClear);
}

TEST_CASE("campaign_verdict: clause 5 overwrites clause 3 in turn") {
    // 0x40178C is the last write in the routine: dying with the board already
    // cleared is still a wipe-out, and the stage replays.
    CHECK(campaign_verdict(90, true, true) == CampaignVerdict::RoundOver);
    CHECK(campaign_verdict(90, false, true) == CampaignVerdict::RoundOver);
}

TEST_CASE("campaign_pacing carries clause 5's decrement alongside the verdict") {
    // The two are latched together on purpose: clause 5 @0x401786 writes BOTH,
    // and re-deriving the second one later (after the port's death-animation
    // linger, during which the sim is still ticking) would let a late death turn
    // an advance into a replay.
    const CampaignPacing clock_out = campaign_pacing(0, false, false);
    CHECK(clock_out.verdict == CampaignVerdict::RoundOver);
    CHECK_FALSE(clock_out.no_human_survivor);

    const CampaignPacing wipeout = campaign_pacing(90, false, true);
    CHECK(wipeout.verdict == CampaignVerdict::RoundOver);
    CHECK(wipeout.no_human_survivor);

    // And the two really do lead to different stages.
    CHECK(campaign_round_end(true, clock_out.verdict, clock_out.no_human_survivor, 1, kStages)
              .next_stage_index == 2);
    CHECK(campaign_round_end(true, wipeout.verdict, wipeout.no_human_survivor, 1, kStages)
              .next_stage_index == 1);
}

TEST_CASE("a default-constructed pacing is 'still running' — the per-stage clear") {
    // sub_40151B @0x401548 zeroes dword_464894 before every campaign round, so
    // the runner resets to exactly this.
    const CampaignPacing fresh;
    CHECK(fresh.verdict == CampaignVerdict::Running);
    CHECK_FALSE(fresh.no_human_survivor);
}

TEST_CASE("campaign_verdict: killing every opponent is NOT a round end") {
    // The regression this file exists for. There is no survivor-count clause in
    // sub_4016DA and the round loop's own player-count guard is unreachable
    // while dword_46489C is set (sub_421969 @0x421977 returns a constant 2), so
    // "I am the only side left" says nothing about whether the stage is over.
    // Eight of the seventeen shipped stages have ai_count 0, which under the old
    // survivor rule made a lone human the only side before the first tick.
    CHECK(campaign_verdict(90, false, false) == CampaignVerdict::Running);
}
