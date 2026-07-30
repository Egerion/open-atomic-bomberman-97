#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "bomber/assets/reslist.hpp"

// The original's sound SELECTION engine, transliterated (docs/re/sound-engine.md).
//
// BM95 never plays "the clip at SOUNDLST id N". Every gameplay/front-end call
// site names a GROUP BASE and `sub_427961` chooses one member of the group for
// it. A group is the run of CONSECUTIVE occupied slots starting at that base —
// the id table is a flat array indexed by SOUNDLST id, and the run ends at the
// first empty slot. So `sub_427961(20)` picks between "letter1"/"letter2" and
// `sub_427961(2800)` picks one of the eleven "ATOMIC BOMBERMAN!" takes.
//
// Two things then shape what a session actually hears:
//   * a LOAD-TIME CULL (`sub_427F1B`) compacts each large authored block down
//     to its base and then deletes RANDOM members until at most `keep` remain,
//     so 282 death taunts become 7 for that launch and a fresh launch has a
//     different seven;
//   * the pick itself is LEAST-PLAYED-FIRST, not uniform: each slot carries a
//     play counter and the draw is rejection-sampled until it lands on a member
//     whose counter equals the group minimum. Every member is heard once before
//     any is heard twice, in a fresh random order each cycle.
//
// SDL-free on purpose: this is the whole decision half of the engine, so it can
// be unit-tested under the headless preset. AudioEngine owns one and does the
// mixing. DETERMINISM: this class has its OWN generator and is presentation-only
// — it must never be handed sim::State::rng (root CLAUDE.md, rule 6).

namespace bomber::game {

class SoundBank {
public:
    // Number of rejection draws `pick` will spend looking for a least-played
    // member before giving up and using the last one drawn — `sub_427961`'s
    // literal 0xC8 loop bound.
    static constexpr int kPickTries = 200;

    // Re-trigger debounce for `pick_debounced`, in game frames (`sub_427ABB`
    // compares against `dword_464994 + 3`).
    static constexpr int kDebounceFrames = 3;

    // One entry of the cull table `sub_42814B` runs after parsing SOUNDLST.
    struct CullRange {
        int lo;
        int hi;
        int keep;
    };

    // The cull table, read off `sub_42814B` 0x42858E-0x428760: seven literal
    // (base, end, keep) triples followed by a nine-iteration loop over the
    // per-disease voice blocks at 3000 + 50*k. Every `keep` here is the
    // NORMAL-memory arm; the binary substitutes 1 for all of them when the
    // low-memory flag (`dword_464824`, the LOWMEM.BM boot path) is set. The
    // port has no low-memory mode, so only the normal arm is transliterated.
    static std::vector<CullRange> cull_table();

    // Builds the slot table from SOUNDLST and applies the load-time cull.
    // `seed` drives the cull and every later pick.
    //
    // Slots are populated for every id SOUNDLST names, WITHOUT checking that
    // the .RSS file exists — the original does the same (its own log reports
    // all 1051 authored entries loaded although three of them name missing
    // files), so a group can hold a member that resolves to silence. Filtering
    // them out would quietly change the group sizes the original picks over.
    void load(const assets::res::SoundList& list, std::uint32_t seed);

    // `sub_427961`'s selection half: the group starting at `id`, least-played
    // first. Returns the chosen slot id, or -1 when `id` names no group.
    //
    // Charges the pick to the chosen slot's play counter EVEN IF the caller
    // then drops the voice — the original increments unconditionally after
    // resolving the name, past the point where the concurrency cap can refuse
    // to start it (`sub_427961` 0x427AB0 runs whatever `sub_427859` did).
    int pick(int id);

    // `sub_427ABB`: `pick`, but refuses to re-trigger the same group within
    // kDebounceFrames. Returns -1 when the debounce swallows the call. Only the
    // jelly-bounce SFX uses this in the original (`sub_423776`).
    int pick_debounced(int id, std::uint64_t frame);

    // `sub_4278F2`'s selection half: NO group walk at all — the caller named a
    // slot and gets that slot, or -1 if it is empty.
    //
    // The sting in the tail is the play counter. `sub_427961` ends
    // `counts[pick] += 1` (0x427AB0); `sub_4278F2` ends `counts[id] =
    // dword_464994` (0x427950-0x427956) — the SAME array, but ASSIGNED the
    // game-frame counter. Since `pick` is least-played-first, a slot left
    // holding a frame number in the thousands can never again equal its group's
    // minimum, so an exact play effectively RETIRES that member from every
    // ordinary pick on its group for the rest of the session. (Effectively, not
    // absolutely: `pick`'s 200-draw ceiling still plays whatever the last draw
    // was, so a group that is entirely retired degrades to uniform rather than
    // going silent. The original's escape is VALUELST id 7's 30-minute SOUNDLST
    // re-load, which is deliberately not ported.)
    //
    // Audible consequence in the shipped data: a death whose anim index lands in
    // 10-13 plays one of the trampoline clips as its overlay (the authoring
    // collision in docs/re/sound-engine.md §10) and thereby narrows the
    // trampoline group for the rest of the session. That session-long loss of
    // variety is a real, hearable behaviour of the original, not an artifact.
    //
    // It is an assignment and not a max(), so a stamp with a SMALL frame can
    // also lower a counter — which is exactly what the binary does; the port
    // does not "improve" it.
    int pick_exact(int id, std::uint64_t frame);

    // The clip base name in a slot, or nullptr when the slot is empty. Slot ids
    // are only meaningful for ids `pick` returned: the cull COMPACTS blocks, so
    // after load a slot may hold a name SOUNDLST authored at a different id.
    const std::string* name(int id) const;

    // Members of the group starting at `id`, in slot order (introspection for
    // tests and for the debug survey; not used on the play path).
    std::vector<int> group(int id) const;

    // A slot's play counter (`dword_463088[id]`), or -1 for an out-of-range id.
    // Introspection only — it exists so a test can tell the two tails apart:
    // `pick` bumps this by one, `pick_exact` stamps a frame number into it.
    int play_count(int id) const;

    int size() const { return static_cast<int>(slots_.size()); }

    // Presentation-side generator. Public because AudioEngine's cosmetic
    // `chance`/`roll` helpers share it — one stream of cosmetic randomness,
    // entirely separate from the sim's.
    std::uint32_t next_rand();

private:
    int group_size(int id) const;

    std::vector<std::string> slots_;
    std::vector<int> plays_;
    std::map<int, std::uint64_t> last_frame_;  // group base -> frame of last play
    std::uint32_t lcg_ = 0x1234ABCDu;
};

}  // namespace bomber::game
