#pragma once

// Small output helpers for abtool (verification/debugging only, not engine code).

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <vector>

#include "bomber/assets/image.hpp"
#include "bomber/assets/rss.hpp"

namespace bomber::tools {

namespace detail {
inline void put16(std::vector<std::uint8_t>& v, std::uint16_t x) {
    v.push_back(x & 0xFF);
    v.push_back(x >> 8);
}
inline void put32(std::vector<std::uint8_t>& v, std::uint32_t x) {
    for (int i = 0; i < 4; ++i) v.push_back((x >> (i * 8)) & 0xFF);
}
inline void write_all(const std::filesystem::path& path, const std::vector<std::uint8_t>& v) {
    std::ofstream f(path, std::ios::binary);
    if (!f) throw std::runtime_error("cannot write: " + path.string());
    f.write(reinterpret_cast<const char*>(v.data()), static_cast<std::streamsize>(v.size()));
}
}  // namespace detail

// 32-bit BGRA bottom-up BMP (BITMAPINFOHEADER).
inline void write_bmp(const std::filesystem::path& path, const assets::Image& img) {
    using namespace detail;
    const std::uint32_t data_size = static_cast<std::uint32_t>(img.width) * img.height * 4;
    std::vector<std::uint8_t> v;
    v.reserve(54 + data_size);
    v.push_back('B'); v.push_back('M');
    put32(v, 54 + data_size);
    put32(v, 0);
    put32(v, 54);
    put32(v, 40);
    put32(v, static_cast<std::uint32_t>(img.width));
    put32(v, static_cast<std::uint32_t>(img.height));
    put16(v, 1);
    put16(v, 32);
    put32(v, 0);  // BI_RGB
    put32(v, data_size);
    put32(v, 2835); put32(v, 2835);
    put32(v, 0); put32(v, 0);
    for (int y = img.height - 1; y >= 0; --y) {
        for (int x = 0; x < img.width; ++x) {
            std::size_t o = (static_cast<std::size_t>(y) * img.width + x) * 4;
            v.push_back(img.rgba[o + 2]);  // B
            v.push_back(img.rgba[o + 1]);  // G
            v.push_back(img.rgba[o + 0]);  // R
            v.push_back(img.rgba[o + 3]);  // A
        }
    }
    write_all(path, v);
}

// Standard PCM16 WAV.
inline void write_wav(const std::filesystem::path& path, const assets::rss::Sound& snd) {
    using namespace detail;
    const std::uint32_t data_bytes = static_cast<std::uint32_t>(snd.samples.size() * 2);
    const std::uint32_t byte_rate =
        assets::rss::Sound::kSampleRate * assets::rss::Sound::kChannels * 2;
    std::vector<std::uint8_t> v;
    v.reserve(44 + data_bytes);
    auto tag = [&v](const char* s) { v.insert(v.end(), s, s + 4); };
    tag("RIFF"); put32(v, 36 + data_bytes); tag("WAVE");
    tag("fmt "); put32(v, 16);
    put16(v, 1);  // PCM
    put16(v, assets::rss::Sound::kChannels);
    put32(v, assets::rss::Sound::kSampleRate);
    put32(v, byte_rate);
    put16(v, assets::rss::Sound::kChannels * 2);
    put16(v, 16);
    tag("data"); put32(v, data_bytes);
    for (std::int16_t s : snd.samples) put16(v, static_cast<std::uint16_t>(s));
    write_all(path, v);
}

}  // namespace bomber::tools
