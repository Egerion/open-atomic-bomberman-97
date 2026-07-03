#include "bomber/assets/rss.hpp"

#include "bomber/assets/binary_reader.hpp"

namespace bomber::assets::rss {

Sound load(const std::filesystem::path& path) {
    auto buf = read_file(path);
    Sound snd;
    std::size_t n = buf.size() / 2;  // truncate a trailing odd byte if any
    snd.samples.resize(n);
    for (std::size_t i = 0; i < n; ++i) {
        snd.samples[i] = static_cast<std::int16_t>(
            buf[i * 2] | (static_cast<std::uint16_t>(buf[i * 2 + 1]) << 8));
    }
    return snd;
}

}  // namespace bomber::assets::rss
