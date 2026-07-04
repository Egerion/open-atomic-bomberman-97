#pragma once

#include <cstddef>

// Animation frame selection, mirroring the original ANI player.
//
// In BM95 the "ANI player" is just three accessors over a sequence descriptor
// (`sub_41DA5C` = statecnt, `sub_41DAA7`/`sub_41DB41` = frame + offsets). The
// displayed step is always `counter % statecnt` — an entity owns a step counter
// and advances it itself (per pixel-step for movers, every frame / counter/3 /
// once for stage extras), then the frame shown wraps modulo the step count.
//
// The STAT HEAD timing u16 (`SeqStep::head0`, only ever 0x001E or 0xFFFF) is
// parsed into the step record at +0 but NEVER read by any engine code path — it
// is inert authoring metadata and must not influence pacing. See
// docs/re/facts.md "ANI per-step timing (STAT HEAD u16) — CONFIRMED INERT".

namespace bomber::game {

// The step index to display for a looping animation given an advancing counter.
// `statecnt` is the number of steps (== Anim::steps.size()). Returns 0 for an
// empty animation. Deliberately depends only on (counter, statecnt): head0 is
// never a parameter, which is the whole point of the CONFIRMED-INERT finding.
constexpr std::size_t anim_step_index(std::size_t counter, std::size_t statecnt) {
    return statecnt ? counter % statecnt : 0;
}

}  // namespace bomber::game
