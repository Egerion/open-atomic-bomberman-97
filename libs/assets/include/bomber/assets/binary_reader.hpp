#pragma once

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

// Bounds-checked little-endian reader used by all binary asset parsers.
// Every read throws std::out_of_range rather than running past the buffer —
// original 1997 files are treated as untrusted input.

namespace bomber::assets {

class BinaryReader {
public:
    explicit BinaryReader(std::span<const std::uint8_t> data) : data_(data) {}

    std::size_t pos() const { return pos_; }
    std::size_t size() const { return data_.size(); }
    std::size_t remaining() const { return data_.size() - pos_; }

    void seek(std::size_t p) {
        if (p > data_.size()) throw std::out_of_range("BinaryReader: seek past end");
        pos_ = p;
    }
    void skip(std::size_t n) { need(n); pos_ += n; }

    std::uint8_t u8() { need(1); return data_[pos_++]; }
    std::uint16_t u16() {
        need(2);
        auto v = static_cast<std::uint16_t>(data_[pos_] | (data_[pos_ + 1] << 8));
        pos_ += 2;
        return v;
    }
    std::int16_t i16() { return static_cast<std::int16_t>(u16()); }
    std::uint32_t u32() {
        need(4);
        std::uint32_t v = data_[pos_] | (data_[pos_ + 1] << 8) | (data_[pos_ + 2] << 16) |
                          (static_cast<std::uint32_t>(data_[pos_ + 3]) << 24);
        pos_ += 4;
        return v;
    }
    std::int32_t i32() { return static_cast<std::int32_t>(u32()); }

    std::span<const std::uint8_t> bytes(std::size_t n) {
        need(n);
        auto s = data_.subspan(pos_, n);
        pos_ += n;
        return s;
    }

    // Reads a fixed-size field, returns the string up to the first NUL.
    std::string cstr_field(std::size_t field_len) {
        auto s = bytes(field_len);
        std::size_t len = 0;
        while (len < s.size() && s[len] != 0) ++len;
        return std::string(reinterpret_cast<const char*>(s.data()), len);
    }

private:
    void need(std::size_t n) const {
        // `pos_ + n` can wrap for a pathological n; compare against remaining()
        // instead (== size - pos_, with the pos_ <= size_ invariant always
        // held). Identical to `pos_ + n > size` for every in-range read.
        if (n > remaining()) throw std::out_of_range("BinaryReader: read past end");
    }

    std::span<const std::uint8_t> data_;
    std::size_t pos_ = 0;
};

inline std::vector<std::uint8_t> read_file(const std::filesystem::path& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) throw std::runtime_error("cannot open file: " + path.string());
    f.seekg(0, std::ios::end);
    // tellg() answers -1 for a stream it cannot size — a directory opened as a
    // file does exactly that on libstdc++. Casting that straight to size_t
    // asked the vector for SIZE_MAX bytes, which surfaces as std::length_error:
    // a std::logic_error, outside the runtime_error/out_of_range contract this
    // module promises its callers (docs/coding-standards.md §6), so a caller's
    // catch would let it through.
    const std::streamoff len = f.tellg();
    if (len < 0) throw std::runtime_error("cannot determine size of file: " + path.string());
    std::vector<std::uint8_t> buf(static_cast<std::size_t>(len));
    f.seekg(0);
    f.read(reinterpret_cast<char*>(buf.data()), static_cast<std::streamsize>(buf.size()));
    if (!f) throw std::runtime_error("short read: " + path.string());
    return buf;
}

}  // namespace bomber::assets
