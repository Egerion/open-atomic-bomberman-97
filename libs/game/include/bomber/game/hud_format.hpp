#pragma once

#include <cstdio>
#include <string>

// Pure, SDL-free helpers for the in-round clock HUD (docs/re/in-match-shell.md
// §3, sub_4105D2): the MM:SS format and the <=30s warning-colour threshold.
// Kept out of renderer.cpp so both are unit-testable without linking SDL3
// (mirrors results.hpp/app_flow.hpp — see tests/test_frontend.cpp,
// bomber_frontend_tests links no SDL3).

namespace bomber::game {

// sub_4105D2's MM:SS split: v13/60, v13%60 (truncating integer division on
// whole seconds remaining) formatted through MESSAGES.TXT id 281 = "%u:%02u".
// `seconds_left` is already whole seconds (the caller ceil-divides ticks_left
// by the tick rate); this function only does the minutes/seconds split and
// substitutes the two %u specifiers of `fmt` (the getstring(281) result, or
// its "%u:%02u" fallback when MESSAGES.TXT lacks the id) — never risking a
// wrong-type sprintf on the user's own MESSAGES.TXT text, matching game_app.
// cpp's fmt_u/fmt_s convention (single-specifier only; this one substitutes
// exactly two, in order, and leaves anything else in the string untouched).
inline std::string format_clock(const std::string& fmt, int seconds_left) {
    if (seconds_left < 0) seconds_left = 0;
    int minutes = seconds_left / 60;
    int secs = seconds_left % 60;
    std::string out;
    out.reserve(fmt.size() + 4);
    int subs_done = 0;
    for (std::size_t i = 0; i < fmt.size(); ++i) {
        if (fmt[i] == '%' && i + 1 < fmt.size() && subs_done < 2) {
            std::size_t j = i + 1;
            bool zero_pad = false;
            if (j < fmt.size() && fmt[j] == '0') { zero_pad = true; ++j; }
            if (j + 1 < fmt.size() && fmt[j] == '2' && fmt[j + 1] == 'u') {
                int v = subs_done == 0 ? minutes : secs;
                if (zero_pad) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "%02d", v);
                    out += buf;
                } else {
                    out += std::to_string(v);  // "%2u" (no zero flag): no padding applied
                }
                ++subs_done;
                i = j + 1;
                continue;
            }
            if (j < fmt.size() && fmt[j] == 'u') {
                out += std::to_string(subs_done == 0 ? minutes : secs);
                ++subs_done;
                i = j;
                continue;
            }
        }
        out += fmt[i];
    }
    return out;
}

// The doc's "colour changes at <=30s remaining" (byte_49D38F -> byte_49A390);
// the 30 is a hardcoded literal in the original, not a VALUELST id.
inline constexpr int kClockWarningSeconds = 30;
inline bool clock_warning(int seconds_left) { return seconds_left <= kClockWarningSeconds; }

}  // namespace bomber::game
