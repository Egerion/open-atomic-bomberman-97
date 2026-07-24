// Locks the reverse-engineered ANI pacing model (sub_41DAA7): the displayed
// step is counter % statecnt, and the STAT HEAD timing u16 (head0, 0x001E /
// 0xFFFF) is inert — it can never change which frame is shown. See
// docs/re/facts.md "ANI per-step timing (STAT HEAD u16) — CONFIRMED INERT".

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <cstddef>
#include <cstdint>
#include <vector>

#include "bomber/game/anim_pace.hpp"

using bomber::game::anim_step_index;

TEST_CASE("frame selection wraps modulo the step count") {
    // A 4-step looping sequence, exactly like the original's counter % statecnt.
    CHECK(anim_step_index(0, 4) == 0);
    CHECK(anim_step_index(3, 4) == 3);
    CHECK(anim_step_index(4, 4) == 0);   // wrap
    CHECK(anim_step_index(9, 4) == 1);   // second cycle
    CHECK(anim_step_index(4000, 4) == 0);
}

TEST_CASE("a single-step sequence always shows step 0") {
    for (std::size_t c = 0; c < 100; ++c) CHECK(anim_step_index(c, 1) == 0);
}

TEST_CASE("an empty animation is safe (no modulo-by-zero)") {
    CHECK(anim_step_index(0, 0) == 0);
    CHECK(anim_step_index(12345, 0) == 0);
}

TEST_CASE("the STAT HEAD timing field cannot affect frame selection") {
    // Model a sequence whose per-step head0 values are the two the field ever
    // takes (0x001E / 0xFFFF), interleaved. The frame shown must depend only on
    // (counter, statecnt); head0 is not even reachable from the selector. This
    // is the whole CONFIRMED-INERT finding, encoded as a regression guard.
    const std::vector<std::uint16_t> head0 = {0x001E, 0xFFFF, 0x001E, 0xFFFF, 0x001E};
    const std::size_t n = head0.size();
    for (std::size_t counter = 0; counter < 5 * n; ++counter) {
        const std::size_t shown = anim_step_index(counter, n);
        CHECK(shown == counter % n);          // pure wrap
        CHECK(shown < n);                     // always in range
        // Flipping any step's timing value would change nothing: the selector
        // never consults head0, so the mapping above is total and stable.
        (void)head0[shown];
    }
}
