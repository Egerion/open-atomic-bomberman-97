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
    if (sting_stream_) SDL_DestroyAudioStream(sting_stream_);
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

    // Seed the cosmetic generator from the clock and build the slot table —
    // this is the moment the original picks the session's random SUBSET of each
    // large voice group (sub_42814B's cull pass) and zeroes the play counters
    // (0x428480/0x42849F allocate both arrays here). A FIXED seed was the whole
    // reason the title sting never varied between launches: the pick was random
    // but the sequence was identical every boot.
    //
    // DETERMINISM: this is the presentation-side generator (root CLAUDE.md rule
    // 6). The original draws its sound picks from the SAME libc rand() that the
    // gameplay code uses — worth recording, but the port deliberately keeps the
    // two apart: a wall-clock seed anywhere near sim::State::rng would desync
    // every online match and move every golden hash.
    bank_.load(names, static_cast<std::uint32_t>(SDL_GetPerformanceCounter()));

    step(0.80f);
    music_stream_ =
        SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, nullptr, nullptr);
    if (music_stream_) {
        // NO music duck. The original attenuates NOTHING: the master volume is
        // set exactly once, at sound-system bring-up (0x4194F0), to 0x7FFF =
        // maximum, and every sound object — music and SFX alike — is born at
        // 0x7FFF too (the constructor's own store at 0x4197A9). The per-object
        // SetVolume wrapper sub_41A50D has five callers and ALL five live inside
        // the sound library, replaying an object's already-stored level; no game
        // code ever asks for a level. So music and effects share one bus at full
        // scale and their relative loudness is whatever the authored .RSS files
        // carry. (docs/re/sound-engine.md §9. This line used to apply a 0.55
        // gain "to sit under the effects" — an invented constant with no
        // citation, and audibly wrong: it made every music track two thirds the
        // level the 1997 mix intended.)
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
    const int slot = bank_.pick(lo);
    // The contiguous run never spills past the authored block in practice; the
    // clamp keeps the caller's stated bound honest anyway. (It also charges the
    // pick either way, exactly as before — bank_.pick has already counted it.)
    if (hi >= 0 && (slot < lo || slot > hi)) return;
    // sub_427BFB: same group pick, but the binary builds its own sound object
    // outside the counted pool, so the cap can neither refuse it nor be
    // charged for it. One dedicated stream mirrors that (and one sting at a
    // time is all the four call sites can ever produce).
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
    // One-shot clip: flushing lets the resampler drain completely, so the
    // stream's queue really reaches zero when playback ends. Without this
    // every stream stays "busy" forever and sound dies after a few clips.
    SDL_FlushAudioStream(stream);
}

std::uint32_t AudioEngine::next_rand() { return bank_.next_rand(); }

const assets::rss::Sound* AudioEngine::get(int slot) {
    // Names come from the BANK, not raw SOUNDLST: the load-time cull compacts
    // each culled block, so after load a slot holds whichever survivor landed
    // there, not the id SOUNDLST authored. The cache is keyed by clip name for
    // the same reason — several slots can legitimately name the same file.
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
