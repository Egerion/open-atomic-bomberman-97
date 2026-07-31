#pragma once

#include <cstdio>
#include <string>

// Pure, SDL-free helpers for the in-round clock HUD (docs/re/in-match-shell.md
// §3, sub_4105D2) and the MESSAGES.TXT splices every screen shares.
//
// Every format string here is the PLAYER'S OWN FILE. A hand-edited row reaches
// these functions, so "leave a specifier of the wrong kind literal rather than
// guess" is a contract, not an implementation detail: a wrong-type sprintf on
// user data is a crash.

namespace bomber::game {
namespace detail {

// One clock conversion — "%u", "%0u", "%2u" or "%02u" — starting at `i`.
// Zero padding applies ONLY to the two-digit form: "%0u" is width-less and
// prints bare.
struct ClockConv {
    bool found = false;
    bool pad2 = false;
    std::size_t end = 0;  // index just past the conversion
};

inline ClockConv clock_conv_at(const std::string& fmt, std::size_t i) {
    if (fmt[i] != '%' || i + 1 >= fmt.size()) return ClockConv{};
    std::size_t j = i + 1;
    const bool zero = fmt[j] == '0';
    if (zero) ++j;
    if (j + 1 < fmt.size() && fmt[j] == '2' && fmt[j + 1] == 'u')
        return ClockConv{true, zero, j + 2};
    if (j < fmt.size() && fmt[j] == 'u') return ClockConv{true, false, j + 1};
    return ClockConv{};
}

inline std::string clock_field(int v, bool pad2) {
    if (!pad2) return std::to_string(v);
    char buf[8];
    std::snprintf(buf, sizeof(buf), "%02d", v);
    return buf;
}

// The index just past the first %u/%d/%i/%s/%% in `f`, starting at `p`+1.
inline std::size_t specifier_end(const std::string& f, std::size_t p) {
    std::size_t q = p + 1;
    while (q < f.size() && f[q] != 'u' && f[q] != 'd' && f[q] != 'i' && f[q] != 's' && f[q] != '%')
        ++q;
    return q;
}

}  // namespace detail

// sub_4105D2's MM:SS split, substituted into getstring(281) = "%u:%02u" (or
// that literal when MESSAGES.TXT lacks the id). `seconds_left` is already whole
// seconds; exactly two conversions are substituted, in order, and anything else
// in the string is left untouched.
inline std::string format_clock(const std::string& fmt, int seconds_left) {
    if (seconds_left < 0) seconds_left = 0;
    const int field[2] = {seconds_left / 60, seconds_left % 60};
    std::string out;
    out.reserve(fmt.size() + 4);
    int done = 0;
    for (std::size_t i = 0; i < fmt.size();) {
        const detail::ClockConv c = done < 2 ? detail::clock_conv_at(fmt, i) : detail::ClockConv{};
        if (!c.found) {
            out += fmt[i++];
            continue;
        }
        out += detail::clock_field(field[done], c.pad2);
        ++done;
        i = c.end;
    }
    return out;
}

// The doc's "colour changes at <=30s remaining" (byte_49D38F -> byte_49A390);
// the 30 is a hardcoded literal in the original, not a VALUELST id.
inline constexpr int kClockWarningSeconds = 30;
inline bool clock_warning(int seconds_left) { return seconds_left <= kClockWarningSeconds; }

// Substitute the first %u/%d/%i with `v`, leaving any other specifier literal.
inline std::string fmt_u(const std::string& f, int v) {
    const auto p = f.find('%');
    if (p == std::string::npos) return f;
    const std::size_t q = detail::specifier_end(f, p);
    if (q < f.size() && (f[q] == 'u' || f[q] == 'd' || f[q] == 'i'))
        return f.substr(0, p) + std::to_string(v) + f.substr(q + 1);
    return f;
}

// Substitute the first %s with `v`, leaving any other specifier literal.
inline std::string fmt_s(const std::string& f, const std::string& v) {
    const auto p = f.find('%');
    if (p == std::string::npos) return f;
    const std::size_t q = detail::specifier_end(f, p);
    if (q < f.size() && f[q] == 's') return f.substr(0, p) + v + f.substr(q + 1);
    return f;
}

// Both-args splice for two-specifier rows ("Player %u: %s"): the leading numeric
// first, then the %s; a reordered MESSAGES.TXT degrades gracefully.
inline std::string fmt_us(const std::string& f, int v, const std::string& s) {
    return fmt_s(fmt_u(f, v), s);
}

}  // namespace bomber::game
