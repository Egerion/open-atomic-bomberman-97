#pragma once

#include <SDL3/SDL.h>

#include <cstdint>
#include <filesystem>
#include <functional>  // boot "Loading sound..." progress callback
#include <map>

#include "bomber/assets/reslist.hpp"
#include "bomber/assets/rss.hpp"
#include "bomber/audio/sound_bank.hpp"
#include "bomber/audio/sound_sink.hpp"

// The MIXING half of the sound engine: a pool of SDL3 streams fed with the
// original headerless PCM (.RSS) clips, degrading to silence when no audio
// device is available (headless CI). The which-clip half is SoundBank.

namespace bomber::game {

// The vtable buys nothing at runtime here; it exists so the event -> id half can
// be tested without SDL.
class AudioEngine : public SoundSink {
public:
    AudioEngine() = default;

    // Raw-owning (an SDL audio-subsystem refcount plus kStreams + 2 streams), so
    // a copy would double-free. Nothing moves it either — it is a GameApp value
    // member — so the move operations stay implicitly suppressed.
    ~AudioEngine() override;
    AudioEngine(const AudioEngine&) = delete;
    AudioEngine& operator=(const AudioEngine&) = delete;

    // Opens the device streams and indexes SOUNDLST. False = stay silent.
    // `progress` reports a coarse 0 -> 1 fraction so the boot dialog can animate
    // and pump the window (sub_4287B9's 5/20/40/60/80/100 steps).
    bool init(const std::filesystem::path& game_dir,
              const std::function<void(float)>& progress = {});

    bool enabled() const { return ok_; }

    // Starts (or switches) looping background music by SOUNDLST id.
    void start_music(int id);

    // sub_427342's "free the music handle". The round init calls this when music
    // is disabled: the original SILENCES the round rather than merely skipping
    // the stage track (docs/re/in-match-shell.md §2).
    void stop_music();

    // True when SOUNDLST NAMES this id, whether or not its .RSS loads — lets a
    // caller fall back the way sub_4293E5 does instead of start_music() going
    // quiet on an unnamed id.
    bool has_track(int id) const { return bank_.name(id) != nullptr; }

    // Call regularly: re-queues the track shortly before it runs out (loop).
    void update_music();

    // Cosmetic 1-in-n chance (presentation-layer RNG, never the sim's).
    bool chance(int n) override;

    // Cosmetic uniform draw in [0, n), for a caller that must LATCH a choice
    // across several calls rather than re-pick — e.g. the wall-slam SFX, drawn
    // once per enclosure arm (docs/re/facts.md "Wall-slam SFX").
    int roll(int n) override;

    // sub_427961 — the ONE call every gameplay and front-end site uses. `id` is
    // a GROUP BASE, not a clip. A group of one behaves like a plain play.
    void play(int id) override;

    // sub_4278F2 — the named slot with NO group pick; only the wall-slam and the
    // death-anim overlay do this. `frame` is not a timing hint: it is STAMPED
    // into the slot's play counter, retiring the slot from its group's rotation
    // (SoundBank::pick_exact carries the reading).
    void play_exact(int id, std::uint64_t frame) override;

    // sub_427BFB — the group pick onto a voice NOT counted against the
    // concurrency cap and unable to be refused by it. The four screen stings use
    // it: title (2800), menu-quit (2600), draw (1700), winner (2000). Two once
    // went through the counted pool, where a busy results transition could drop
    // them; the original cannot swallow a victory sting.
    //
    // `hi` bounds the block, because SOUNDLST's blocks sit back to back and a
    // group is "the contiguous run from `lo`" — without it a block with no hole
    // before the next base lets the pick walk into it, playing a "we have a
    // winner" take on a DRAW. A pick past `hi` is dropped. -1 = no bound.
    //
    // ONE sting voice exists and a new sting clears it, which is safe only
    // because no two of the four call sites can fire together.
    void play_sting(int lo, int hi = -1);

    // sub_427ABB — `play` with the 3-frame same-group debounce. Jelly bounce
    // only.
    void play_debounced(int id, std::uint64_t frame) override;

private:
    std::uint32_t next_rand();
    const assets::rss::Sound* get(int id);
    void start_voice(int slot, bool counted);

    // Physical stream pool — NOT the concurrency limit. That one is POLICY
    // (voice_cap_), applied on top, so the pool only has to be larger than it.
    static constexpr int kStreams = 32;
    // The original's concurrent-voice cap, VALUELST id 8 (authored 5), read from
    // the install at init; this is the fallback when VALUELST is unreadable.
    // `sub_427859` opens with `if (getvalue(8) < active_voices) return;` and
    // there is NO eviction: an over-cap sound is DROPPED, never stolen from a
    // voice already playing. (The port once stole the closest-to-finishing
    // stream instead, which cut clips off mid-word.)
    static constexpr int kDefaultVoiceCap = 5;
    int voice_cap_ = kDefaultVoiceCap;
    SDL_AudioStream* streams_[kStreams]{};
    SDL_AudioStream* music_stream_ = nullptr;
    // The uncapped sting voice (sub_427BFB's own sound object), separate so a
    // busy match can never swallow a screen sting.
    SDL_AudioStream* sting_stream_ = nullptr;
    assets::rss::Sound music_;
    SoundBank bank_;
    std::map<std::string, assets::rss::Sound> cache_;  // clip name -> decoded PCM
    std::filesystem::path sound_dir_;
    // Set only once THIS instance's SDL_InitSubSystem(AUDIO) succeeded, so the
    // destructor undoes only a bring-up we own.
    bool audio_inited_ = false;
    bool ok_ = false;
};

}  // namespace bomber::game
