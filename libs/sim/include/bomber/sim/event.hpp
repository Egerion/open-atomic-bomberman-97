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
        Infected,      // picked up / caught a disease (data = Disease kind)
        BombStopped,   // kicked bomb hit an obstacle and stopped (SOUNDLST 130)
        JellyBounced,  // jelly bomb reversed off an obstacle while sliding (SOUNDLST 135)
        // Stage actors (docs/re/stage-actors.md). SoundDirector maps these:
        // TrampolineBounce -> SOUNDLST 350, WarpUsed -> SOUNDLST 1330.
        TrampolineBounce,  // player stepped onto a trampoline and launched a hop
        WarpUsed,          // player entered a warphole (reserved; warphole deferred)
        // Campaign rover/ghost hazards (docs/re/campaign.md "Per-tick mover").
        // `player` is the rover's INDEX into State::rovers for all three (not
        // a player slot) unless noted otherwise.
        RoverSpawned,       // a rover/ghost was placed on the board; data = RoverKind
        RoverDied,          // stepped into an active flame; data = flame owner's player slot
        RoverKilledPlayer,  // killed a human/network player on its landing tile;
                            // `player` is the VICTIM's player slot (not a rover index)
                            // so the presentation can reuse the normal death path;
                            // data = the rover's index into State::rovers
        // Per-level tile regeneration (docs/re/facts.md "Per-level tile
        // regeneration", Haunted House/Cemetery). x,y = the tile. NOTE: the
        // original has NO dedicated sound or animation for this — sub_426704
        // writes the cell straight to Brick and the normal per-tile redraw
        // path blits the level's standard TILE<n>_BRICK art, same as any
        // other brick. This event exists only so the presentation can redraw/
        // react to the change without diffing the grid every frame; it does
        // not imply a distinct visual.
        TileRegrew,
        // A bomb drop was refused because the player stands on a WARPHOLE
        // (sub_41F29B drop block: actor type 1 short-circuits the placement
        // and plays SOUNDLST 40/41 "enrt" instead — never during auto-drop,
        // which is refused silently; the sim emits this only for the audible
        // case). facts.md "Core-feel audit" §3.
        DropRefused,
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
