#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

// The little-endian primitives every wire codec in this component is built on.
//
// PRIVATE to libs/net (it lives under src/, not include/): nothing outside this
// component has any business laying out our datagrams. It exists because
// input_codec.cpp and protocol.cpp each carried a byte-for-byte copy of the same
// four functions in their own anonymous namespaces, so a fix to one silently
// left the other alone — and the two files encode HALVES OF THE SAME DATAGRAM
// (protocol.cpp writes the tag, input_codec.cpp the payload behind it).
//
// LITTLE-ENDIAN IS THE WIRE CONTRACT, not a host detail: these read and write
// byte by byte and never memcpy an integer, so a big-endian peer produces and
// accepts the identical bytes. kWireProtocolVersion is 9 and a deployed
// matchmaker is written against these layouts — changing what they emit is a
// wire break, not a refactor.

namespace bomber::net {

inline void put_u16_le(std::vector<std::uint8_t>& b, std::uint16_t v) {
    b.push_back(static_cast<std::uint8_t>(v & 0xFFU));
    b.push_back(static_cast<std::uint8_t>((v >> 8) & 0xFFU));
}

inline void put_u32_le(std::vector<std::uint8_t>& b, std::uint32_t v) {
    for (int i = 0; i < 4; ++i) b.push_back(static_cast<std::uint8_t>((v >> (8 * i)) & 0xFFU));
}

inline void put_u64_le(std::vector<std::uint8_t>& b, std::uint64_t v) {
    for (int i = 0; i < 8; ++i) b.push_back(static_cast<std::uint8_t>((v >> (8 * i)) & 0xFFU));
}

// The readers take a bare pointer because every caller has already bounds-checked
// the framing (a decoder rejects a short datagram before it reads a field) — that
// check is the codec's job and cannot be delegated here without knowing the frame.
inline std::uint16_t get_u16_le(const std::uint8_t* d) {
    return static_cast<std::uint16_t>(static_cast<unsigned>(d[0]) |
                                      (static_cast<unsigned>(d[1]) << 8));
}

inline std::uint32_t get_u32_le(const std::uint8_t* d) {
    std::uint32_t v = 0;
    for (int i = 0; i < 4; ++i) v |= static_cast<std::uint32_t>(d[i]) << (8 * i);
    return v;
}

inline std::uint64_t get_u64_le(const std::uint8_t* d) {
    std::uint64_t v = 0;
    for (int i = 0; i < 8; ++i) v |= static_cast<std::uint64_t>(d[i]) << (8 * i);
    return v;
}

}  // namespace bomber::net
