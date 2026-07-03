#pragma once

#include <SDL3/SDL.h>

#include <cstdint>
#include <filesystem>
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
    // Opens the device streams and indexes SOUNDLST. False = stay silent.
    bool init(const std::filesystem::path& game_dir);

    bool enabled() const { return ok_; }

    // Starts (or switches) looping background music by SOUNDLST id.
    void start_music(int id);

    // Call regularly: re-queues the track shortly before it runs out (loop).
    void update_music();

    // Cosmetic 1-in-n chance (presentation-layer RNG, never the sim's).
    bool chance(int n);

    // Plays one of the given SOUNDLST ids (round-robin variety).
    void play_one_of(std::initializer_list<int> ids);

    // Plays a random sound out of every SOUNDLST entry with lo <= id <= hi.
    // The original groups its effects and voice lines into such id ranges.
    void play_random_in_range(int lo, int hi);

    void play(int id);

private:
    std::uint32_t next_rand();
    const assets::rss::Sound* get(int id);

    static constexpr int kStreams = 8;
    SDL_AudioStream* streams_[kStreams]{};
    SDL_AudioStream* music_stream_ = nullptr;
    assets::rss::Sound music_;
    assets::res::SoundList names_;
    std::map<int, assets::rss::Sound> cache_;
    std::filesystem::path sound_dir_;
    std::uint32_t counter_ = 0;
    std::uint32_t lcg_ = 0x1234ABCD;
    bool ok_ = false;
};

}  // namespace bomber::game
