#pragma once

#include <cstddef>
#include <cstdint>

// The death-ANIMATION index: the original's `actor[+4]`, and the one number the
// drawn corpse and the death overlay SOUND have to agree on.
//
// In BM95 there is no agreement problem to solve, because there is only one
// value: `sub_41DE63` rolls `rand() % getvalue(105) + 1` (VALUELST id 105,
// authored 24, comment "how many different death animations do we have? (die 1
// through die 24)"), stores it at `actor[+4]` (0x41DF04), and the death handler
// `sub_41DCB2` then uses that ONE field twice — the tail of `sub_41F29B` builds
// `die green %d` from it to draw the corpse, and 0x41DDE4 plays
// `sub_4278F2(340 + actor[+4])` to sound it. It is even replicated to peers as
// its own 16-bit network field (`sub_41DE04`), so every machine draws and hears
// the same death. See docs/re/sound-engine.md §10.
//
// The port cannot copy that shape directly. The index is cosmetic here (nothing
// in `libs/sim` reads it), so root CLAUDE.md rule 6 forbids drawing it from
// `State::rng`, and it is not carried on the `PlayerDied` event. Two sides need
// it: `Renderer::on_events` (libs/game) picks the "die green N" sequence, and
// `SoundDirector` (libs/audio) picks the overlay slot.
//
// THE RULE THIS HEADER EXISTS TO ENFORCE: neither side rolls. Both call the
// pure function below on inputs they already share — the sim tick the death
// event was emitted on and the victim's slot, both hashed sim state that
// reaches the two modules through the same `const State&`. There is no stored
// choice, no call order to get right, and no lifetime to manage, so the sound
// physically cannot describe a corpse other than the one drawn: the same two
// integers go in on both sides of the same tick. (Before this, the audio side
// rolled its own `roll(24)` and was right one time in 24.)
//
// The derivation is deliberately arithmetic rather than random. It is a
// PORT-ONLY stand-in for `rand() % 24 + 1` — flagged as such, not presented as
// an extraction — and it buys back the property the original gets for free from
// replicating the field: because tick and slot are hashed sim state, two peers
// in a netplay match draw and hear the SAME death animation, which an
// independent presentation LCG per machine could never guarantee.
//
// SDL-free and header-only. It lives in libs/audio because that is the lowest
// module both consumers can see (libs/game depends on libs/audio, never the
// reverse), so the audio half stays headlessly testable.

namespace bomber::game {

// VALUELST id 105, authored 24 in the shipped install. The port does not read
// it live: `SoundDirector` talks only to `SoundSink` and has no values handle,
// and the renderer's sprite pool is asset-driven (see `death_anim_slot`).
// The shipped assets agree exactly — XPLODE1..17.ANI contribute 24 sequences,
// `die green 1` .. `die green 24`, in ascending order.
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
// `pool` is what the install actually shipped: with the stock 24 sequences this
// is exactly `death_anim_index() - 1`, i.e. anim N is `die green N`; the modulo
// only does anything on a partial install, where it keeps a legal sprite on
// screen instead of blanking the corpse.
constexpr std::size_t death_anim_slot(std::uint64_t tick, int player, std::size_t pool) {
    if (pool == 0) return 0;
    return static_cast<std::size_t>(death_anim_index(tick, player) - 1) % pool;
}

// `sub_4278F2`'s argument. NOT a group base — the overlay addresses one slot
// directly, which is why 340+N reaches clips authored for other cues (the
// trampoline and bombhit blocks sit inside the span this reserved). That
// collision is the original's own and is reproduced deliberately; see §10.
constexpr int death_overlay_sound(int anim_index) {
    return kDeathOverlayBase + anim_index;
}

}  // namespace bomber::game
