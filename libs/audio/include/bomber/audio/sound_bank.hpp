#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "bomber/assets/reslist.hpp"

// The original's sound SELECTION engine, transliterated (docs/re/sound-engine.md).
//
// BM95 never plays "the clip at SOUNDLST id N". Every call site names a GROUP
// BASE and `sub_427961` picks a member; a group is the run of CONSECUTIVE
// occupied slots from that base, ending at the first empty one. Two things then
// shape what a session hears: a LOAD-TIME CULL (`sub_427F1B`) that leaves each
// large block a different random subset per launch, and a LEAST-PLAYED-FIRST
// pick, so every member is heard once before any is heard twice.
//
// DETERMINISM: this class has its OWN generator and is presentation-only — it
// must never be handed sim::State::rng (CLAUDE.md rule 6).

namespace bomber::game {

class SoundBank {
public:
    static constexpr int kPickTries = 200;     // sub_427961's literal 0xC8 bound
    static constexpr int kDebounceFrames = 3;  // sub_427ABB's `dword_464994 + 3`

    // NOT an RE fact: a bound on untrusted input. SOUNDLST.RES comes off the
    // player's disk and its ids are parsed with no range check, so `load` sizes
    // its tables from a number a file chooses. The shipped file's highest id is
    // ~3450; this leaves eighteen times that for a modder while capping the
    // allocation at a couple of megabytes. See the comment at the check.
    static constexpr int kMaxSoundId = 65535;

    // One entry of the cull table `sub_42814B` runs after parsing SOUNDLST.
    struct CullRange {
        int lo;
        int hi;
        int keep;
    };

    // The cull table, read off `sub_42814B` 0x42858E-0x428760. Every `keep` is
    // the NORMAL-memory arm; the binary substitutes 1 for all of them under the
    // LOWMEM boot flag, which this port has no equivalent of.
    static std::vector<CullRange> cull_table();

    // Builds the slot table from SOUNDLST and applies the load-time cull.
    //
    // Slots are populated for every id SOUNDLST names WITHOUT checking that the
    // .RSS exists, as the original does, so a group can hold a member that
    // resolves to silence. Filtering those out would change the group SIZES the
    // original picks over.
    void load(const assets::res::SoundList& list, std::uint32_t seed);

    // `sub_427961`'s selection half: the group starting at `id`, least-played
    // first. Returns the chosen slot id, or -1 when `id` names no group.
    //
    // Charges the pick EVEN IF the caller then drops the voice — the original
    // increments past the point where the concurrency cap can refuse it.
    int pick(int id);

    // `sub_427ABB`: `pick` with a kDebounceFrames re-trigger guard on the same
    // group; -1 when the debounce swallows the call. Jelly bounce only.
    int pick_debounced(int id, std::uint64_t frame);

    // `sub_4278F2`'s selection half: NO group walk — the caller named a slot and
    // gets it, or -1 if empty.
    //
    // The sting is in the tail. `pick` ends `counts[chosen] += 1`; this ends
    // `counts[id] = dword_464994`, the same array ASSIGNED the game-frame
    // counter. Since `pick` is least-played-first, a slot holding a frame number
    // in the thousands can never again equal its group's minimum, so an exact
    // play effectively RETIRES that member for the session — and because it is
    // an assignment and not a max(), a small frame can also LOWER a counter.
    // The binary's behaviour, not improved here.
    int pick_exact(int id, std::uint64_t frame);

    // The clip base name in a slot, or nullptr when empty. Slot ids are only
    // meaningful for ids `pick` returned: the cull COMPACTS blocks, so a slot
    // may hold a name SOUNDLST authored at a different id.
    const std::string* name(int id) const;

    // Introspection for tests and the debug survey; not on the play path.
    std::vector<int> group(int id) const;

    // A slot's play counter (`dword_463088[id]`), or -1 out of range. Exists so
    // a test can tell the two tails apart: `pick` bumps it, `pick_exact` stamps.
    int play_count(int id) const;

    int size() const { return static_cast<int>(slots_.size()); }

    // Presentation-side generator, public so AudioEngine's `chance`/`roll` share
    // it — one cosmetic stream, entirely separate from the sim's.
    std::uint32_t next_rand();

private:
    int group_size(int id) const;

    std::vector<std::string> slots_;
    std::vector<int> plays_;
    std::map<int, std::uint64_t> last_frame_;  // group base -> frame of last play
    std::uint32_t lcg_ = 0x1234ABCDu;
};

}  // namespace bomber::game
