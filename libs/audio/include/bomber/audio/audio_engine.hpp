#pragma once

#include <SDL3/SDL.h>

#include <cstdint>
#include <filesystem>
#include <functional>  // boot "Loading sound..." progress callback
#include <initializer_list>
#include <map>

#include "bomber/assets/reslist.hpp"
#include "bomber/assets/rss.hpp"

// Sound engine: a pool of SDL3 audio streams fed with the original headerless
// PCM (.RSS) clips. Degrades to silence when no audio device is available
// (headless CI). Sound selection is cosmetic and never touches sim state.

namespace bomber::game {

class AudioEngine {
public:
    AudioEngine() = default;

    // Raw-owning: an SDL audio-subsystem refcount plus kStreams + 1
    // SDL_AudioStreams (each opened alongside its own logical device). The
    // destructor is the only place these are released, so the type is
    // non-copyable (a copy would double-free the shared handles). Nothing moves
    // it either — it is a GameApp value member, referenced everywhere else — so
    // the move operations stay implicitly suppressed rather than = default'd.
    ~AudioEngine();
    AudioEngine(const AudioEngine&) = delete;
    AudioEngine& operator=(const AudioEngine&) = delete;

    // Opens the device streams and indexes SOUNDLST. False = stay silent.
    // `progress` (optional) reports a coarse 0 -> 1 fraction across the device/
    // SOUNDLST bring-up so the boot "Loading sound..." dialog can animate + pump
    // the window, mirroring sub_4287B9's fixed 5/20/40/60/80/100 percent steps.
    bool init(const std::filesystem::path& game_dir,
              const std::function<void(float)>& progress = {});

    bool enabled() const { return ok_; }

    // Starts (or switches) looping background music by SOUNDLST id.
    void start_music(int id);

    // Stops the looping music outright — the port of sub_427342's "free the
    // music handle". Used by the round init when "Disable music during
    // gameplay" is set: the original SILENCES the round (the setup-screens
    // track must not bleed into it), it does not merely skip starting the
    // stage track (docs/re/in-match-shell.md §2 round init).
    void stop_music();

    // True when SOUNDLST names this id (regardless of whether its .RSS file
    // actually loads) — lets a caller pick a documented fallback id (e.g. the
    // in-round per-level stage track 1100+level falling back to 1120,
    // docs/re/in-match-shell.md §2) the way sub_4293E5 does, instead of
    // start_music() silently going quiet on an unnamed id.
    bool has_track(int id) const { return names_.names.contains(id); }

    // Call regularly: re-queues the track shortly before it runs out (loop).
    void update_music();

    // Cosmetic 1-in-n chance (presentation-layer RNG, never the sim's).
    bool chance(int n);

    // Cosmetic uniform draw in [0, n) (presentation-layer RNG, never the
    // sim's). For callers that need to LATCH a random choice across several
    // calls instead of re-picking every time — e.g. the wall-slam SFX, which
    // the original draws once per enclosure arm (`dword_462244 = rand() % 3`,
    // docs/re/facts.md "Wall-slam SFX") and replays for every dropped tile.
    int roll(int n);

    // Plays one of the given SOUNDLST ids (round-robin variety).
    void play_one_of(std::initializer_list<int> ids);

    // Plays a random sound out of every SOUNDLST entry with lo <= id <= hi.
    // The original groups its effects and voice lines into such id ranges.
    void play_random_in_range(int lo, int hi);

    void play(int id);

private:
    std::uint32_t next_rand();
    const assets::rss::Sound* get(int id);

    // Concurrent SFX voices. 8 was too few for Bomberman's chain reactions: a
    // multi-kill emits several Explosion + PlayerDied events in adjacent ticks,
    // and once all voices are busy `play()` steals the closest-to-finishing one
    // — cutting off overlapping death screams (the user-reported "AI players
    // scream less than the original on death"). The original's DirectSound voice
    // cap lives in the (untransliterated) sound-engine init, so this is an
    // approximation >= the old 8 that stops the starvation; raise/pin it if the
    // exact binary cap is ever recovered. (2026-07-22 oracle audio audit.)
    static constexpr int kStreams = 32;
    SDL_AudioStream* streams_[kStreams]{};
    SDL_AudioStream* music_stream_ = nullptr;
    assets::rss::Sound music_;
    assets::res::SoundList names_;
    std::map<int, assets::rss::Sound> cache_;
    std::filesystem::path sound_dir_;
    std::uint32_t counter_ = 0;
    std::uint32_t lcg_ = 0x1234ABCD;
    // True once this instance's init() succeeded at SDL_InitSubSystem(AUDIO), so
    // the destructor undoes only a subsystem bring-up we actually own.
    bool audio_inited_ = false;
    bool ok_ = false;
};

}  // namespace bomber::game
