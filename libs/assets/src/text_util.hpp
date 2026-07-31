#pragma once

#include <string>
#include <string_view>

// PRIVATE to libs/assets — not under include/, so it is not part of the
// component's public surface.
//
// The original's text resources (.SCH, EXTRA<N>.RES, MESSAGES.TXT, the .CAM
// stage list, VALUELST.RES) are all DOS-authored and share one trimming
// convention. Four files carried a byte-identical copy of it; a fifth copy is
// how the 0x1a in that character set eventually gets left out of one parser and
// a shipped file starts failing to load on its last line only.

namespace bomber::assets::text {

// Leading/trailing ASCII blanks, the CR of a CRLF pair, and the DOS EOF marker
// (0x1a) that terminates several of the shipped files.
inline constexpr std::string_view kTrimmed = " \t\r\x1a";

inline std::string trim(std::string_view s) {
    const auto b = s.find_first_not_of(kTrimmed);
    if (b == std::string_view::npos) return {};
    const auto e = s.find_last_not_of(kTrimmed);
    return std::string(s.substr(b, e - b + 1));
}

}  // namespace bomber::assets::text
