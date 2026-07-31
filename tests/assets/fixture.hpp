#pragma once

// Shared scaffolding for the bomber::assets suites. Every loader in this
// component reads a FILE, so most cases here have the same two needs: build a
// byte buffer to the format spec, and put it on disk under a name that cleans
// itself up. Both were copied into each suite as it was written.
//
// Nothing format-specific belongs here — a CIMG header or a PCX palette is the
// business of the one suite that knows the format.

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace bomber::test {

using Bytes = std::vector<std::uint8_t>;

// The original's files are little-endian throughout (Watcom on x86).
inline void put_u16(Bytes& b, unsigned v) {
    b.push_back(static_cast<std::uint8_t>(v & 0xFF));
    b.push_back(static_cast<std::uint8_t>((v >> 8) & 0xFF));
}

inline void put_u32(Bytes& b, std::uint32_t v) {
    for (int i = 0; i < 4; ++i) b.push_back(static_cast<std::uint8_t>((v >> (8 * i)) & 0xFF));
}

inline void append(Bytes& into, const Bytes& more) {
    into.insert(into.end(), more.begin(), more.end());
}

inline void append_chars(Bytes& b, const char* text, std::size_t n) {
    for (std::size_t i = 0; i < n; ++i) b.push_back(static_cast<std::uint8_t>(text[i]));
}

// A scratch file that removes itself. RAII rather than a trailing
// `fs::remove()` because a failed REQUIRE unwinds straight past that call, and
// a leaked temp file makes the NEXT run of a suite start from stale bytes.
class TempFile {
public:
    TempFile(const char* name, const Bytes& data)
        : path_(std::filesystem::temp_directory_path() / name) {
        std::ofstream f(path_, std::ios::binary | std::ios::trunc);
        f.write(reinterpret_cast<const char*>(data.data()),
                static_cast<std::streamsize>(data.size()));
    }
    TempFile(const char* name, const std::string& text)
        : path_(std::filesystem::temp_directory_path() / name) {
        std::ofstream f(path_, std::ios::binary | std::ios::trunc);
        f << text;
    }
    ~TempFile() {
        std::error_code ec;
        std::filesystem::remove(path_, ec);
    }
    TempFile(const TempFile&) = delete;
    TempFile& operator=(const TempFile&) = delete;

    const std::filesystem::path& path() const { return path_; }
    operator const std::filesystem::path&() const { return path_; }  // NOLINT: reads as the path

    // The file's current bytes, for the writer round-trips that inspect layout.
    std::string text() const {
        std::ifstream in(path_, std::ios::binary);
        return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    }

private:
    std::filesystem::path path_;
};

// A path in the temp directory that is guaranteed NOT to exist — the "absent
// file" contract every loader in this component has.
inline std::filesystem::path absent_path(const char* name) {
    std::filesystem::path p = std::filesystem::temp_directory_path() / name;
    std::error_code ec;
    std::filesystem::remove(p, ec);
    return p;
}

}  // namespace bomber::test
