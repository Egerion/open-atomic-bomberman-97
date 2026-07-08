#pragma once

#include <cstdint>

namespace bomber::sim {

// Something noteworthy that happened during a tick. Events are rebuilt every
// tick and are NOT part of the hashed state — they are derived outputs for
// the presentation layer (sounds, effects) and for tests.
struct Event {
    enum class Type : std::uint8_t {
        BombPlaced,
        BombKicked,
        Explosion,
        BrickDestroyed,
        PowerupRevealed,
        PowerupPicked,
        PowerupBurned,
        PlayerDied,
        TimeUp,
        Hurry,
        WallClosed,
        BombPunched,
        BombBounced,
        BombGrabbed,
        BombThrown,
        HeadHit,
        Infected,     // picked up / caught a disease (data = Disease kind)
        BombStopped,  // kicked bomb hit an obstacle and stopped (SOUNDLST 130)
        JellyBounced, // jelly bomb reversed off an obstacle while sliding (SOUNDLST 135)
        // Stage actors (docs/re/stage-actors.md). SoundDirector maps these:
        // TrampolineBounce -> SOUNDLST 350, WarpUsed -> SOUNDLST 1330.
        TrampolineBounce, // player stepped onto a trampoline and launched a hop
        WarpUsed,         // player entered a warphole (reserved; warphole deferred)
    };
    Type type{};
    std::int8_t player = -1;  // acting/affected player, -1 if n/a
    std::int8_t x = -1, y = -1;
    // Powerup kind for the powerup events. For PlayerDied (docs/re/
    // results-and-options.md §1, sub_421B0F's per-round kill tally): the
    // KILLER's player index, or -1 when there is no attributable killer
    // (enclosure/warphole crush). `data == player` is an explicit SELF-kill
    // (died to their own flame) — the frontend excludes these from the kill
    // tally ("our semantics"; §1 does not pin whether a self-kill counts).
    std::int8_t data = 0;
};

}  // namespace bomber::sim
