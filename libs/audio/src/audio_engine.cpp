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
    // Destroying a stream from SDL_OpenAudioDeviceStream also closes the logical
    // device opened alongside it, so no separate SDL_CloseAudioDevice. Skipping
    // nulls is what makes a PARTIAL init safe: the slots init() never reached are
    // still value-initialized, so each stream is destroyed exactly once.
    for (SDL_AudioStream* stream : streams_)
        if (stream) SDL_DestroyAudioStream(stream);
    if (music_stream_) SDL_DestroyAudioStream(music_stream_);
    if (sting_stream_) SDL_DestroyAudioStream(sting_stream_);
    // SDL_InitSubSystem is refcounted, so only undo a bring-up this instance did.
    if (audio_inited_) SDL_QuitSubSystem(SDL_INIT_AUDIO);
}

bool AudioEngine::init(const std::filesystem::path& game_dir,
                       const std::function<void(float)>& progress) {
    // sub_4287B9's fixed 5/20/40/60/80/100 steps. The port loads .RSS lazily, so
    // the real work is the device bring-up plus the SOUNDLST read — reported in
    // the same shape so the boot bar animates instead of snapping to full.
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
    assets::res::SoundList names;
    try {
        names = assets::res::load_sounds(game_dir / "DATA" / "RES" / "SOUNDLST.RES");
    } catch (const std::exception&) {
        return false;
    }

    // The concurrent-voice cap the original reads with getvalue(8) inside
    // sub_427859. VALUELST is optional here — a missing or unparsable file just
    // leaves the authored default in place rather than failing sound bring-up.
    try {
        const assets::res::ValueList values =
            assets::res::load_values(game_dir / "DATA" / "RES" / "VALUELST.RES");
        const std::int64_t cap = values.at_or(8, kDefaultVoiceCap);
        if (cap > 0 && cap < kStreams) voice_cap_ = static_cast<int>(cap);
    } catch (const std::exception&) {
        voice_cap_ = kDefaultVoiceCap;  // unreadable VALUELST: keep the authored 5
    }

    // The moment the original picks this session's random SUBSET of each large
    // voice group (sub_42814B's cull). The clock seed is load-bearing: a FIXED
    // one made the title sting identical every launch.
    //
    // DETERMINISM: presentation-side generator only (CLAUDE.md rule 6). The
    // original draws sound picks from the SAME libc rand() as gameplay; the port
    // deliberately does not, because a wall-clock seed anywhere near
    // sim::State::rng would desync every online match and move every golden.
    bank_.load(names, static_cast<std::uint32_t>(SDL_GetPerformanceCounter()));

    step(0.80f);
    music_stream_ =
        SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, nullptr, nullptr);
    if (music_stream_) {
        // NO music duck, deliberately. The original attenuates nothing: master
        // volume is set once at bring-up to maximum and every sound object is
        // born there, so music and effects share one bus at full scale
        // (docs/re/sound-engine.md §9). This line once applied a 0.55 gain "to
        // sit under the effects" — an invented constant, and audibly wrong.
        SDL_ResumeAudioStreamDevice(music_stream_);
    }
    sting_stream_ =
        SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, nullptr, nullptr);
    if (sting_stream_) SDL_ResumeAudioStreamDevice(sting_stream_);
    sound_dir_ = game_dir / "DATA" / "SOUND";
    ok_ = true;
    step(1.0f);
    return true;
}

void AudioEngine::start_music(int id) {
    if (!ok_ || !music_stream_) return;
    // sub_42741E indexes the same id table the SFX picker uses, but WITHOUT the
    // group walk — music is always the exact slot. (Every music id sits outside
    // the culled blocks, so the cull cannot move a track out from under it.)
    const std::string* name = bank_.name(id);
    if (!name) return;
    try {
        music_ = assets::rss::load(sound_dir_ / (to_upper(*name) + ".RSS"));
    } catch (const std::exception&) {
        music_.samples.clear();
        return;
    }
    SDL_ClearAudioStream(music_stream_);
    SDL_PutAudioStreamData(music_stream_, music_.samples.data(),
                           static_cast<int>(music_.samples.size() * 2));
}

void AudioEngine::stop_music() {
    // sub_427342's "free the music handle". Clearing music_ is what makes
    // update_music() a no-op, so nothing loops until the next start_music().
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

void AudioEngine::play(int id) {
    if (!ok_) return;
    start_voice(bank_.pick(id), true);
}

void AudioEngine::play_exact(int id, std::uint64_t frame) {
    if (!ok_) return;
    start_voice(bank_.pick_exact(id, frame), true);
}

void AudioEngine::play_debounced(int id, std::uint64_t frame) {
    if (!ok_) return;
    start_voice(bank_.pick_debounced(id, frame), true);
}

void AudioEngine::play_sting(int lo, int hi) {
    if (!ok_) return;
    // sub_427BFB: play()'s group pick onto a sound object outside the counted
    // pool, so the cap can neither refuse it nor be charged for it.
    const int slot = bank_.pick(lo);
    // A group is the contiguous run from `lo` and knows nothing about where the
    // authored block stops, so without `hi` this could land among the NEXT
    // block's takes. Dropped rather than played wrong; the pick is charged
    // either way.
    if (hi >= 0 && (slot < lo || slot > hi)) return;
    start_voice(slot, false);
}

void AudioEngine::start_voice(int slot, bool counted) {
    if (slot < 0) return;
    const assets::rss::Sound* snd = get(slot);
    if (!snd || snd->samples.empty()) return;

    SDL_AudioStream* stream = nullptr;
    if (counted) {
        // sub_427859's opening line: `if (getvalue(8) < active_voices) return;`
        // — count what is still playing and DROP the new sound when the cap is
        // already met. No eviction: a voice in progress is never cut short.
        int active = 0;
        int use = -1;
        for (int i = 0; i < kStreams; ++i) {
            if (SDL_GetAudioStreamQueued(streams_[i]) > 0)
                ++active;
            else if (use < 0)
                use = i;
        }
        if (use < 0 || voice_cap_ < active) return;
        stream = streams_[use];
    } else {
        stream = sting_stream_;
        if (!stream) return;
        SDL_ClearAudioStream(stream);
    }

    SDL_PutAudioStreamData(stream, snd->samples.data(),
                           static_cast<int>(snd->samples.size() * 2));
    // REQUIRED: without the flush the resampler never drains, every stream stays
    // "busy" forever, and sound dies after a few clips.
    SDL_FlushAudioStream(stream);
}

std::uint32_t AudioEngine::next_rand() { return bank_.next_rand(); }

const assets::rss::Sound* AudioEngine::get(int slot) {
    // Names come from the BANK, not raw SOUNDLST: the cull compacts each block,
    // so a slot holds whichever survivor landed there. The cache is keyed by
    // NAME for the same reason — several slots can name the same file.
    const std::string* name = bank_.name(slot);
    if (!name) return nullptr;
    if (auto it = cache_.find(*name); it != cache_.end()) return &it->second;
    if (cache_.size() > 128) cache_.clear();  // crude cap; clips reload lazily
    try {
        cache_[*name] = assets::rss::load(sound_dir_ / (to_upper(*name) + ".RSS"));
    } catch (const std::exception&) {
        cache_[*name] = assets::rss::Sound{};
    }
    return &cache_[*name];
}

}  // namespace bomber::game
