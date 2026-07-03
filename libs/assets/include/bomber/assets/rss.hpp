#pragma once

#include <cstdint>
#include <filesystem>
#include <vector>

namespace bomber::assets::rss {

// .RSS sound: headerless raw PCM, 22050 Hz, stereo, s16le
// (documented in the game's own SOUNDLST.RES header).

struct Sound {
    static constexpr int kSampleRate = 22050;
    static constexpr int kChannels = 2;
    std::vector<std::int16_t> samples;  // interleaved L/R

    double seconds() const {
        return samples.empty() ? 0.0
                               : static_cast<double>(samples.size()) / kChannels / kSampleRate;
    }
};

Sound load(const std::filesystem::path& path);

}  // namespace bomber::assets::rss
