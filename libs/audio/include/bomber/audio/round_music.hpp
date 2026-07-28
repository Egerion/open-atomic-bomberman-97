#pragma once

// The ROUND-INIT music decision, isolated from SDL and from the screen classes
// so it can be pinned by a test and so the local and netplay round loops cannot
// drift apart (they did: the netplay loop ignored the player's option entirely).
//
// The original, in the tail of the round init sub_410B6E (docs/re/
// sound-engine.md §9):
//
//     0x410E88   if (dword_4648C0 == 0) goto start          ; the option is OFF
//     0x410E91       sub_427342()                           ; ON  -> FREE the music
//     0x410E96       goto after
//     0x410E98   start: sub_4293E5()                        ; OFF -> the round's tune
//
// Two things this shape settles, both of which the port had wrong:
//
//   * `dword_4648C0` is options.ini's `disable_game_music=` (the writer at
//     0x40650D parses that exact key, the reader at 0x405F6F writes it back).
//     The guard is a bare test of it — there is NO network arm, no campaign arm,
//     no "only when a stage loaded" arm. The option is the player's, not the
//     mode's.
//   * The ON arm SILENCES the round outright rather than leaving the previous
//     track running, so the setup-screens track (1020) does not bleed into
//     gameplay.
//
// And which track the OFF arm picks is sub_4293E5's own business: SOUNDLST
// 1100+level, or 1120 ("generic") when the level names no track.

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
