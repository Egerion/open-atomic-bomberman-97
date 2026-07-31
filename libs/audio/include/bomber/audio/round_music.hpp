#pragma once

// The ROUND-INIT music decision, isolated from SDL and from the screen classes
// so the local and netplay round loops cannot drift apart (they did: netplay
// ignored the player's option entirely).
//
// Two things the round-init tail of sub_410B6E settles, both of which the port
// had wrong (docs/re/sound-engine.md §9):
//
//   * the guard is a BARE test of options.ini's `disable_game_music=` — no
//     network arm, no campaign arm, no "only when a stage loaded" arm. The
//     option is the player's, not the mode's.
//   * its ON arm SILENCES the round outright rather than leaving the previous
//     track running, so the setup-screens track does not bleed into gameplay.

namespace bomber::game {

// The per-level in-round stage track (sub_4293E5 reads names[1100 + level]).
inline constexpr int kStageMusicBase = 1100;
// ...and its fallback when that slot is empty (0x460, GENERIC.RSS).
inline constexpr int kStageMusicFallback = 1120;
// Not an id: "free the music, the round is silent" — sub_427342's arm.
inline constexpr int kRoundMusicSilent = -1;

// `has_track(id)` answers "does SOUNDLST name this id?" (AudioEngine::has_track).
// Templated on the predicate so this header stays free of the SDL-backed engine.
template <class HasTrack>
int round_music_id(int stage, bool disable_game_music, HasTrack has_track) {
    if (disable_game_music) return kRoundMusicSilent;
    const int per_level = kStageMusicBase + stage;
    return has_track(per_level) ? per_level : kStageMusicFallback;
}

}  // namespace bomber::game
