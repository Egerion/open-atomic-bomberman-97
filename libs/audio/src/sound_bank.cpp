#include "bomber/audio/sound_bank.hpp"

#include <algorithm>
#include <limits>

namespace bomber::game {
namespace {

// `sub_427F1B`'s first pass: squeeze the holes out of [lo, hi] so survivors sit
// consecutively from `lo`. This is what lets a call site name a base the author
// never used — SOUNDLST has no id 700, yet `sub_427961(700)` is the death taunt,
// because compaction has moved 701 down into it. Stable, as the binary is.
void compact(std::vector<std::string>& slots, int lo, int hi) {
    const int end = std::min(hi, static_cast<int>(slots.size()) - 1);
    int write = lo;
    for (int read = lo; read <= end; ++read)
        if (!slots[read].empty()) {
            if (read != write) slots[write].swap(slots[read]);
            ++write;
        }
    for (; write <= end; ++write) slots[write].clear();
}

}  // namespace

std::vector<SoundBank::CullRange> SoundBank::cull_table() {
    std::vector<CullRange> t{
        {200, 299, 3},     // exploding bombs
        {400, 499, 7},     // powerup pickup voices
        {700, 999, 7},     // post-death taunts
        {1200, 1299, 2},   // "huge string of bombs" taunts
        {1400, 1699, 7},   // "you are now AWESOME" milestone voices
        {2300, 2599, 8},   // generic disease voices
        {2700, 2799, 5},   // "hurry up!" callouts
    };
    // 0x4286FA-0x428760: `for (k = 0; k < 9; ++k) cull(3000 + 50*k, 3049 + 50*k, 4)`
    // — the nine per-disease voice blocks.
    for (int k = 0; k < 9; ++k) t.push_back({3000 + 50 * k, 3049 + 50 * k, 4});
    return t;
}

void SoundBank::load(const assets::res::SoundList& list, std::uint32_t seed) {
    lcg_ = seed;
    last_frame_.clear();
    slots_.clear();
    plays_.clear();
    if (list.names.empty()) return;

    const int max_id = list.names.rbegin()->first;
    slots_.assign(static_cast<std::size_t>(max_id) + 1, std::string{});
    plays_.assign(static_cast<std::size_t>(max_id) + 1, 0);
    for (const auto& [id, name] : list.names)
        if (id >= 0) slots_[static_cast<std::size_t>(id)] = name;

    // The load-time cull (`sub_42814B`). The original also re-rolls these
    // subsets every 30 minutes (VALUELST id 7's SOUNDLST re-load); the port
    // culls at load only, deliberately (docs/re/sound-engine.md).
    for (const CullRange& r : cull_table()) {
        if (r.lo >= static_cast<int>(slots_.size())) continue;
        compact(slots_, r.lo, r.hi);
        for (int n = group_size(r.lo); n > r.keep; n = group_size(r.lo)) {
            const int victim = r.lo + static_cast<int>(next_rand() % static_cast<unsigned>(n));
            for (int i = victim; i < r.lo + n - 1; ++i)
                slots_[static_cast<std::size_t>(i)].swap(slots_[static_cast<std::size_t>(i) + 1]);
            slots_[static_cast<std::size_t>(r.lo + n - 1)].clear();
        }
    }
}

int SoundBank::group_size(int id) const {
    if (id < 0 || id >= static_cast<int>(slots_.size())) return 0;
    int n = 0;
    for (int i = id; i < static_cast<int>(slots_.size()) && !slots_[static_cast<std::size_t>(i)].empty();
         ++i)
        ++n;
    return n;
}

std::vector<int> SoundBank::group(int id) const {
    std::vector<int> out;
    const int n = group_size(id);
    out.reserve(static_cast<std::size_t>(n));
    for (int i = 0; i < n; ++i) out.push_back(id + i);
    return out;
}

int SoundBank::play_count(int id) const {
    if (id < 0 || id >= static_cast<int>(plays_.size())) return -1;
    return plays_[static_cast<std::size_t>(id)];
}

const std::string* SoundBank::name(int id) const {
    if (id < 0 || id >= static_cast<int>(slots_.size())) return nullptr;
    const std::string& s = slots_[static_cast<std::size_t>(id)];
    return s.empty() ? nullptr : &s;
}

int SoundBank::pick(int id) {
    const int n = group_size(id);
    if (n == 0) return -1;

    // Least-played-first: take the group minimum, then draw uniformly until a
    // member sitting at it comes up. An equal-use shuffle, NOT a uniform pick.
    // The 200-draw ceiling is the binary's — on exhaustion it plays the last
    // draw, so a large group degrades to uniform rather than hanging.
    const std::size_t base = static_cast<std::size_t>(id);
    int least = plays_[base];
    for (std::size_t j = 1; j < static_cast<std::size_t>(n); ++j)
        least = std::min(least, plays_[base + j]);

    std::size_t chosen = base;
    for (int k = 0; k < kPickTries; ++k) {
        chosen = base + static_cast<std::size_t>(next_rand() % static_cast<unsigned>(n));
        if (plays_[chosen] == least) break;
    }
    ++plays_[chosen];
    return static_cast<int>(chosen);
}

int SoundBank::pick_debounced(int id, std::uint64_t frame) {
    auto it = last_frame_.find(id);
    // `sub_427ABB`: skip when `last + 3 > now`. The binary's escape hatch for a
    // counter that has gone backwards (last > now, i.e. a new match) is modelled
    // by the same comparison on unsigned frames plus the not-yet-seen case.
    if (it != last_frame_.end() && it->second <= frame &&
        it->second + static_cast<std::uint64_t>(kDebounceFrames) > frame)
        return -1;
    const int chosen = pick(id);
    if (chosen >= 0) last_frame_[id] = frame;
    return chosen;
}

int SoundBank::pick_exact(int id, std::uint64_t frame) {
    if (name(id) == nullptr) return -1;
    // `counts[id] = dword_464994` (0x427950) — an ASSIGNMENT, not an increment;
    // the header explains what that costs the group. The clamp is the port's
    // only addition: `plays_` is int and the caller hands us a 64-bit tick, and
    // a saturated counter is indistinguishable from any other very large one.
    constexpr std::uint64_t kMax = static_cast<std::uint64_t>(std::numeric_limits<int>::max());
    plays_[static_cast<std::size_t>(id)] = static_cast<int>(frame < kMax ? frame : kMax);
    return id;
}

std::uint32_t SoundBank::next_rand() {
    lcg_ = lcg_ * 1664525u + 1013904223u;
    return lcg_ >> 16;
}

}  // namespace bomber::game
