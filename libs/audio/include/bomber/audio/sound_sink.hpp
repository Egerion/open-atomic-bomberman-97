#pragma once

#include <cstdint>

// The BM95 play primitives, as an interface, so the event -> id half can be
// pinned by an SDL-free suite (docs/re/sound-engine.md §4):
//   play           <- sub_427961  group pick, counted against the voice cap
//   play_exact     <- sub_4278F2  one named slot, no group walk, counted
//   play_debounced <- sub_427ABB  play() with the 3-frame same-group debounce
// `chance`/`roll` are cosmetic draws — presentation RNG, NEVER State::rng
// (CLAUDE.md determinism rule 6).
//
// The two `frame` parameters point OPPOSITE ways, which is worth keeping
// straight: `play_debounced` READS it, `play_exact` WRITES it — stamping the
// frame into the slot's play count retires that slot from its group's rotation.
// See SoundBank::pick_exact.

namespace bomber::game {

class SoundSink {
public:
    virtual ~SoundSink() = default;
    virtual void play(int id) = 0;
    virtual void play_exact(int id, std::uint64_t frame) = 0;
    virtual void play_debounced(int id, std::uint64_t frame) = 0;
    virtual bool chance(int n) = 0;
    virtual int roll(int n) = 0;
};

}  // namespace bomber::game
