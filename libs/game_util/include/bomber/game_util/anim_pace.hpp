#pragma once

#include <cstddef>

// Animation frame selection, mirroring the original ANI player: three accessors
// over a sequence descriptor (`sub_41DA5C` = statecnt, `sub_41DAA7`/`sub_41DB41`
// = frame + offsets). Each entity owns a step counter and advances it itself
// (per pixel-step for movers, every frame / counter/3 / once for stage extras);
// the frame shown wraps modulo the step count.
//
// The STAT HEAD timing u16 (`SeqStep::head0`, only ever 0x001E or 0xFFFF) is
// parsed into the step record at +0 but NEVER read by any engine code path — it
// is inert authoring metadata and must not influence pacing. See facts.md "ANI
// per-step timing (STAT HEAD u16) — CONFIRMED INERT".

namespace bomber::game {

// Deliberately depends only on (counter, statecnt): head0 is never a parameter,
// which is the whole point of the CONFIRMED-INERT finding above.
constexpr std::size_t anim_step_index(std::size_t counter, std::size_t statecnt) {
    return statecnt ? counter % statecnt : 0;
}

}  // namespace bomber::game
