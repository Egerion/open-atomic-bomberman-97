#pragma once

#include <cstddef>
#include <cstdint>

// The death-ANIMATION index: the original's `actor[+4]` (`sub_41DE63` rolls
// `rand() % getvalue(105) + 1`), and the one number the drawn corpse and the
// death overlay SOUND have to agree on. See docs/re/sound-engine.md §10.
//
// THE RULE THIS HEADER EXISTS TO ENFORCE: neither consumer rolls. BM95 has one
// field and reads it twice; the port has two modules and may not use State::rng
// for a cosmetic draw (rule 6), so both call the pure function below on the
// death tick and the victim's slot — hashed state they already share. No stored
// choice and no call order, so the sound cannot describe a corpse other than the
// one drawn. (The audio side once rolled its own roll(24): right 1 in 24.)
//
// Arithmetic rather than random is a PORT-ONLY stand-in for `rand() % 24 + 1`,
// not an extraction. It buys back what the original gets from replicating the
// field: two netplay peers hear the death they each drew.

namespace bomber::game {

// VALUELST id 105, authored 24. Not read live: SoundDirector has no values
// handle. The shipped assets agree — XPLODE1..17.ANI contribute exactly 24
// `die green N` sequences in ascending order.
inline constexpr int kDeathAnimCount = 24;

// The literal added at 0x41DDE4: the overlay plays SOUNDLST slot 340 + anim.
inline constexpr int kDeathOverlayBase = 340;

// The 1-based animation index for a death emitted at `tick` by victim `player`
// — the port's stand-in for `actor[+4]`. Always in [1, kDeathAnimCount].
constexpr int death_anim_index(std::uint64_t tick, int player) {
    const std::uint64_t slot = player > 0 ? static_cast<std::uint64_t>(player) : 0u;
    const std::uint64_t v = tick + slot * 7u;
    return static_cast<int>(v % static_cast<std::uint64_t>(kDeathAnimCount)) + 1;
}

// The same index as a 0-BASED offset into the renderer's `die*` sprite pool.
// The modulo only bites on a PARTIAL install, where it keeps a legal sprite on
// screen instead of blanking the corpse.
constexpr std::size_t death_anim_slot(std::uint64_t tick, int player, std::size_t pool) {
    if (pool == 0) return 0;
    return static_cast<std::size_t>(death_anim_index(tick, player) - 1) % pool;
}

// `sub_4278F2`'s argument, and NOT a group base: addressing one slot directly is
// why 340+N reaches clips authored for other cues. The original's own collision,
// reproduced deliberately — see docs/re/sound-engine.md §10.
constexpr int death_overlay_sound(int anim_index) {
    return kDeathOverlayBase + anim_index;
}

}  // namespace bomber::game
