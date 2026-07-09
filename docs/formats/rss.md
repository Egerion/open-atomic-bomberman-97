# Sound format (`DATA/SOUND/*.RSS`)

Headerless raw PCM audio. 2027 files ship with the game — every sound
effect and music track is one `.RSS`. The format is documented by the
game's OWN data, not reverse-engineered from the binary: `SOUNDLST.RES`'s
header comment states the sample format directly (`docs/formats/res.md`
covers `SOUNDLST.RES`'s `id -> base name` grammar; this doc is the audio
payload those base names point at, `<name>.RSS`).

## Layout

```
raw PCM: 22050 Hz, 2 channels (stereo), 16-bit signed, little-endian,
         interleaved L/R samples, no header, no trailer.
```

That's the entire format — the file IS the sample data. Sample count =
`file_size / 4` (4 bytes per stereo frame = 2 channels x 2 bytes); if the
byte count is odd the loader truncates the final incomplete sample rather
than throwing (a length mismatch here is a truncated/corrupt file, not a
different format variant, so silently dropping the last dangling byte is
harmless and matches how a raw-PCM stream is naturally consumed).

## Parser

`bomber::assets::rss::load` (`libs/assets/src/rss.cpp`) reads the whole
file and reinterprets every 2-byte little-endian pair as one `std::int16_t`
sample into `Sound { samples }` (interleaved L/R, `libs/assets/include/
bomber/assets/rss.hpp`). `Sound::kSampleRate` (22050) and `Sound::kChannels`
(2) are compile-time constants — the format has no per-file rate/channel
field to read, so these are baked in rather than parsed. `Sound::seconds()`
derives playback duration as `samples.size() / channels / sample_rate`.

There is no validation to enforce beyond the odd-byte truncation above: any
byte sequence is a legal (if possibly silent/noisy) `.RSS` file, so
`rss::load` never throws for a malformed body — only `read_file` itself can
throw (`std::runtime_error`) if the path can't be opened at all
(`binary_reader.hpp`).

## Playback

Not part of the format itself, but the consumption path: `SOUNDLST.RES`
maps a numeric sound-event id to an `.RSS` base name (`docs/formats/res.md`
§1); `libs/game`'s `AudioEngine`/`SoundDirector` load the named `.RSS` and
mix it at its native 22050 Hz stereo rate. Cosmetic sound-pick randomness
(e.g. which of several `.RSS` variants plays for a given event) is driven
by a presentation-side RNG, never `State::rng` — the determinism contract
in `CLAUDE.md` §"Determinism contract" rule 6.
