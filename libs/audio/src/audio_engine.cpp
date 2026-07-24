#include "bomber/audio/audio_engine.hpp"

#include <cctype>
#include <exception>
#include <string>
#include <vector>

namespace bomber::game {
namespace {

std::string to_upper(std::string s) {
    for (char& ch : s) ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
    return s;
}

}  // namespace

AudioEngine::~AudioEngine() {
    // RAII teardown for the raw SDL handles init() opened. Destroying a stream
    // created by SDL_OpenAudioDeviceStream also closes the logical device opened
    // alongside it (SDL_audio.h), so no separate SDL_CloseAudioDevice is needed.
    // A partial init leaves the not-yet-opened slots null (streams_ is value-
    // initialized, music_stream_ starts null), so skipping nulls destroys
    // exactly what was created before any mid-init failure — each stream once.
    for (SDL_AudioStream* stream : streams_)
        if (stream) SDL_DestroyAudioStream(stream);
    if (music_stream_) SDL_DestroyAudioStream(music_stream_);
    // Balance init()'s SDL_InitSubSystem. It is refcounted, so audio only truly
    // shuts down once every owner has quit; guard on audio_inited_ so we undo
    // solely a bring-up this instance performed.
    if (audio_inited_) SDL_QuitSubSystem(SDL_INIT_AUDIO);
}

bool AudioEngine::init(const std::filesystem::path& game_dir,
                       const std::function<void(float)>& progress) {
    // Coarse boot "Loading sound..." percents (mirrors sub_4287B9's fixed
    // 5/20/40/60/80/100 steps). The port loads .RSS clips lazily rather than
    // preloading SOUNDLST groups, so the real work here is the device/stream
    // bring-up + the SOUNDLST index read — still reported in the same shape so
    // the second boot flash animates instead of snapping to a full bar.
    auto step = [&](float f) {
        if (progress) progress(f);
    };
    step(0.05f);
    if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) return false;
    audio_inited_ = true;
    SDL_AudioSpec spec{SDL_AUDIO_S16LE, assets::rss::Sound::kChannels,
                       assets::rss::Sound::kSampleRate};
    for (int i = 0; i < kStreams; ++i) {
        streams_[i] =
            SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, nullptr, nullptr);
        if (!streams_[i]) return false;
        SDL_ResumeAudioStreamDevice(streams_[i]);
        if (i == kStreams / 2) step(0.40f);  // halfway through the SFX voice pool
    }
    step(0.60f);
    try {
        names_ = assets::res::load_sounds(game_dir / "DATA" / "RES" / "SOUNDLST.RES");
    } catch (const std::exception&) {
        return false;
    }
    step(0.80f);
    music_stream_ =
        SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, nullptr, nullptr);
    if (music_stream_) {
        SDL_SetAudioStreamGain(music_stream_, 0.55f);  // sit under the effects
        SDL_ResumeAudioStreamDevice(music_stream_);
    }
    sound_dir_ = game_dir / "DATA" / "SOUND";
    ok_ = true;
    step(1.0f);
    return true;
}

void AudioEngine::start_music(int id) {
    if (!ok_ || !music_stream_) return;
    auto it = names_.names.find(id);
    if (it == names_.names.end()) return;
    try {
        music_ = assets::rss::load(sound_dir_ / (to_upper(it->second) + ".RSS"));
    } catch (const std::exception&) {
        music_.samples.clear();
        return;
    }
    SDL_ClearAudioStream(music_stream_);
    SDL_PutAudioStreamData(music_stream_, music_.samples.data(),
                           static_cast<int>(music_.samples.size() * 2));
}

void AudioEngine::stop_music() {
    // The analogue of the original's sub_427342 "free the music handle": the
    // current track stops and nothing loops until the next start_music().
    // Clearing music_ makes update_music() a no-op (its empty() guard).
    music_.samples.clear();
    if (ok_ && music_stream_) SDL_ClearAudioStream(music_stream_);
}

void AudioEngine::update_music() {
    if (!ok_ || !music_stream_ || music_.samples.empty()) return;
    const int refill_below =
        assets::rss::Sound::kSampleRate * assets::rss::Sound::kChannels * 2 * 2;  // ~2 s
    if (SDL_GetAudioStreamQueued(music_stream_) < refill_below)
        SDL_PutAudioStreamData(music_stream_, music_.samples.data(),
                               static_cast<int>(music_.samples.size() * 2));
}

bool AudioEngine::chance(int n) {
    return n > 0 && next_rand() % static_cast<std::uint32_t>(n) == 0;
}

int AudioEngine::roll(int n) {
    if (n <= 0) return 0;
    return static_cast<int>(next_rand() % static_cast<std::uint32_t>(n));
}

void AudioEngine::play_one_of(std::initializer_list<int> ids) {
    if (!ok_ || ids.size() == 0) return;
    play(ids.begin()[static_cast<std::size_t>(counter_++) % ids.size()]);
}

void AudioEngine::play_random_in_range(int lo, int hi) {
    if (!ok_) return;
    std::vector<int> c;
    for (auto it = names_.names.lower_bound(lo); it != names_.names.end() && it->first <= hi; ++it)
        c.push_back(it->first);
    if (!c.empty()) play(c[next_rand() % c.size()]);
}

void AudioEngine::play(int id) {
    if (!ok_) return;
    const assets::rss::Sound* snd = get(id);
    if (!snd || snd->samples.empty()) return;
    int use = -1;
    for (int i = 0; i < kStreams; ++i) {
        if (SDL_GetAudioStreamQueued(streams_[i]) == 0) {
            use = i;
            break;
        }
    }
    if (use < 0) {
        // Pool exhausted: steal the stream closest to finishing.
        int best = 0;
        for (int i = 1; i < kStreams; ++i)
            if (SDL_GetAudioStreamQueued(streams_[i]) < SDL_GetAudioStreamQueued(streams_[best]))
                best = i;
        SDL_ClearAudioStream(streams_[best]);
        use = best;
    }
    SDL_PutAudioStreamData(streams_[use], snd->samples.data(),
                           static_cast<int>(snd->samples.size() * 2));
    // One-shot clip: flushing lets the resampler drain completely, so the
    // stream's queue really reaches zero when playback ends. Without this
    // every stream stays "busy" forever and sound dies after a few clips.
    SDL_FlushAudioStream(streams_[use]);
}

std::uint32_t AudioEngine::next_rand() {
    lcg_ = lcg_ * 1664525u + 1013904223u;
    return lcg_ >> 16;
}

const assets::rss::Sound* AudioEngine::get(int id) {
    if (auto it = cache_.find(id); it != cache_.end()) return &it->second;
    auto name_it = names_.names.find(id);
    if (name_it == names_.names.end()) return nullptr;
    if (cache_.size() > 128) cache_.clear();  // crude cap; clips reload lazily
    try {
        cache_[id] = assets::rss::load(sound_dir_ / (to_upper(name_it->second) + ".RSS"));
    } catch (const std::exception&) {
        cache_[id] = assets::rss::Sound{};
    }
    return &cache_[id];
}

}  // namespace bomber::game
