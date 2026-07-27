#pragma once

#include <SDL3/SDL.h>

#include <cstdint>
#include <filesystem>
#include <functional>  // boot "Loading sound..." progress callback
#include <map>

#include "bomber/assets/reslist.hpp"
#include "bomber/assets/rss.hpp"
#include "bomber/audio/sound_bank.hpp"

// Sound engine: a pool of SDL3 audio streams fed with the original headerless
// PCM (.RSS) clips. Degrades to silence when no audio device is available
// (headless CI). Sound selection is cosmetic and never touches sim state.
//
// The WHICH-clip half lives in SoundBank (SDL-free, unit-tested); this class is
// the mixing half plus the three play primitives the binary exposes:
//   play()        <- sub_427961  group pick, counted against the voice cap
//   play_exact()  <- sub_4278F2  one named slot, no group, counted
//   play_sting()  <- sub_427BFB  group pick on an uncapped, uncounted voice

namespace bomber::game {

class AudioEngine {
public:
    AudioEngine() = default;

    // Raw-owning: an SDL audio-subsystem refcount plus kStreams + 2
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
    bool has_track(int id) const { return bank_.name(id) != nullptr; }

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

    // sub_427961 — the ONE call every gameplay and front-end site uses. `id` is
    // a GROUP BASE, not a clip: one member of the contiguous run starting there
    // is chosen least-played-first. Groups of one (SOUNDLST 10 "menuexit", 160
    // "bmdrop3", …) therefore behave exactly like a plain play.
    void play(int id);

    // sub_4278F2 — plays the named slot with NO group pick. Only two call sites
    // in the binary do this: the enclosure wall-slam (which draws its own
    // `rand() % 3` once per arm and then replays the same id, docs/re/facts.md
    // "Wall-slam SFX") and the death handler's cause-specific overlay.
    void play_exact(int id);

    // sub_427BFB — the group pick again, but onto a voice that is NOT counted
    // against the concurrency cap and cannot be refused by it. The four screen
    // stings use it: title intro (2800), menu-quit (2600), draw (1700) and
    // winner (2000). NOT blocking: the binary follows the quit sting with an
    // explicit Sleep(4000) to keep the process alive long enough to hear it,
    // which would be pointless if the call waited.
    void play_sting(int id);

    // sub_427ABB — `play` with the original's 3-frame re-trigger debounce on the
    // same group. `frame` is the caller's game-frame counter (dword_464994; the
    // port passes the sim tick, the same ~20 Hz logic step). Only the jelly
    // bounce uses this.
    void play_debounced(int id, std::uint64_t frame);

    // Legacy spelling kept for the call sites that name the authored block
    // explicitly. The original's group is the contiguous run from `lo`, so this
    // is `play(lo)` with the run clamped to the block end.
    void play_random_in_range(int lo, int hi);

private:
    std::uint32_t next_rand();
    const assets::rss::Sound* get(int id);
    void start_voice(int slot, bool counted);

    // Physical stream pool. This is NOT the concurrency limit — the original's
    // limit is a POLICY one (voice_cap_ below) applied on top, so the pool only
    // has to be comfortably larger than it.
    static constexpr int kStreams = 32;
    // The original's concurrent-voice cap, VALUELST id 8 ("how many concurrent
    // sounds do we want to allow?", authored 5). `sub_427859` opens with
    // `if (getvalue(8) < active_voices) return;` and there is NO eviction: an
    // over-cap sound is simply DROPPED, never stolen from a voice already
    // playing. Read from the install at init; this is the fallback when
    // VALUELST is unreadable. (The pre-2026-07-27 port had no cap and stole the
    // closest-to-finishing stream instead, which cut clips off mid-word.)
    static constexpr int kDefaultVoiceCap = 5;
    int voice_cap_ = kDefaultVoiceCap;
    SDL_AudioStream* streams_[kStreams]{};
    SDL_AudioStream* music_stream_ = nullptr;
    // The uncapped sting voice (sub_427BFB's own sound object, outside the
    // counted pool) — kept separate so a busy match can never swallow a screen
    // sting, exactly as the binary's separate code path cannot be refused.
    SDL_AudioStream* sting_stream_ = nullptr;
    assets::rss::Sound music_;
    SoundBank bank_;
    std::map<std::string, assets::rss::Sound> cache_;  // clip name -> decoded PCM
    std::filesystem::path sound_dir_;
    // True once this instance's init() succeeded at SDL_InitSubSystem(AUDIO), so
    // the destructor undoes only a subsystem bring-up we actually own.
    bool audio_inited_ = false;
    bool ok_ = false;
};

}  // namespace bomber::game
