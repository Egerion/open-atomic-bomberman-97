#pragma once

#include <cstdint>

// The four BM95 play primitives, as an interface.
//
// `SoundDirector` decides WHICH SOUNDLST group a sim event names; `AudioEngine`
// decides which clip in that group is heard and mixes it. Splitting the two
// behind this boundary lets the event -> id mapping be pinned by an SDL-free
// doctest suite (tests/audio/test_sound_director.cpp), which is what stops a
// removed cue from quietly coming back.
//
// The names mirror the binary's own routines (docs/re/sound-engine.md §4):
//   play           <- sub_427961  group pick, counted against the voice cap
//   play_exact     <- sub_4278F2  one named slot, no group walk, counted
//   play_debounced <- sub_427ABB  play() with the 3-frame same-group debounce
// `chance`/`roll` are the cosmetic draws — presentation RNG, NEVER State::rng
// (root CLAUDE.md determinism rule 6).
//
// TWO of the three take a `frame`, for different reasons, and the difference is
// worth keeping straight: `play_debounced` READS it (has this group played in
// the last 3 frames?), while `play_exact` WRITES it — `sub_4278F2` ends by
// STAMPING the frame counter into the slot's play count instead of bumping it,
// which retires that slot from its group's rotation. See SoundBank::pick_exact.

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
