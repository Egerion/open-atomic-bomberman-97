#pragma once

#include <cstdint>
#include <string_view>

// The presentation stack's diagnostic seam (docs/coding-standards.md §11).
//
// Nearly every one of the 69 raw console-output sites this replaced is the SAME
// diagnostic: an optional 1997 asset failed to load and the game carries on
// without it. Those are worth keeping — an install missing EDIT.ANI still runs,
// and the line saying so is the only way a user learns why the editor has no
// icons. What they lacked was a seam: written straight to stderr they cannot be
// silenced by a capture run, recorded by a test, or routed into netdiag.log.
//
// Two sites stay raw, matching §11's own listed exceptions: game_app.cpp's
// --demo-shots/--bm-shot/--menu-shot progress lines (a capture CLI whose stdout
// IS its product, collected by tests/visual/run_visual_golden.cmake), and
// netdiag.log, written from a destructor so it survives any exit path.

namespace bomber::game {

enum class LogLevel : std::uint8_t {
    Info,  // progress worth seeing: the renderer in use, a settings toggle
    Warn,  // a degraded outcome: an optional asset missing, a save that failed
};

// A sink receives the already-formatted line WITHOUT a trailing newline; the
// default sink writes it to stderr with one, byte-for-byte what every call site
// did before this header existed.
using LogSink = void (*)(LogLevel level, std::string_view message);

// Install a sink, or pass nullptr to restore the stderr default. Returns the
// previous sink so a caller can nest (a capture run installs a silencer, a test
// installs a recorder). Not thread-safe by construction: this is the
// single-threaded presentation layer, and a mutex would buy nothing.
LogSink set_log_sink(LogSink sink);

// Declared twice rather than through a macro: §1 says no C macros, and the
// duplication is two lines against a macro that must launder its own argument
// through parentheses to be correct. The attribute keeps -Wformat checking these
// call sites on GCC/Clang, where the pre-push lint runs; MSVC has no equivalent.
#if defined(__GNUC__) || defined(__clang__)
void log_warn(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
void log_info(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
#else
void log_warn(const char* fmt, ...);
void log_info(const char* fmt, ...);
#endif

}  // namespace bomber::game
