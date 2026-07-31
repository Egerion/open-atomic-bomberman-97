#pragma once

#include <cstdint>
#include <string_view>

// The front-end's diagnostic seam (docs/coding-standards.md §11: "No std::cout /
// printf inside library code. Console output belongs behind a logging seam so it
// can be routed, silenced, or captured").
//
// libs/game held 69 of the repo's 76 raw console-output sites, and they were not
// one population. Nearly all of them are the SAME useful diagnostic: an optional
// 1997 asset failed to load and the game is carrying on without it. Those are
// worth keeping — an install missing EDIT.ANI still runs, and the line that says
// so is the only way a user finds out why the editor has no icons. What they
// lacked was a seam: written straight to stderr they cannot be silenced by a
// capture run, captured by a test, or routed into the same file netdiag.log
// already uses.
//
// So the fix is a seam, not a deletion. The printf-style signature is deliberate
// — every call site it replaces already was one, so the change is a rename
// rather than a rewrite of 69 format strings, and the varargs stay inside the
// two functions below instead of being spread across the package. Format
// checking is preserved on GCC/Clang via the format attribute, which is where
// the pre-push lint runs.
//
// Two sites in libs/game are NOT this and stay as they are, both matching §11's
// own listed exceptions:
//   - the --demo-shots / --bm-shot / --menu-shot progress lines in game_app.cpp.
//     Those are a capture CLI whose stdout IS its product, exactly like
//     apps/abtool; tests/visual/run_visual_golden.cmake collects that stream.
//   - netdiag.log (net_overlay.cpp), a diagnostic file written from a destructor
//     so it survives any exit path.

namespace bomber::game {

enum class LogLevel : std::uint8_t {
    Info,  // progress worth seeing: the renderer in use, a settings toggle
    Warn,  // a degraded outcome: an optional asset missing, a save that failed
};

// A sink receives the already-formatted line WITHOUT a trailing newline; the
// default sink writes it to stderr with one, which is byte-for-byte what every
// call site did before this header existed.
using LogSink = void (*)(LogLevel level, std::string_view message);

// Install a sink, or pass nullptr to restore the stderr default. Returns the
// previous sink so a caller can nest (a capture run installs a silencer, a test
// installs a recorder). Not thread-safe by construction: libs/game is the
// single-threaded presentation layer, and a mutex here would buy nothing.
LogSink set_log_sink(LogSink sink);

// Declared twice rather than through a BOMBER_LOG_FORMAT macro: §1 says no C
// macros, and the duplication is two lines against a macro that has to launder
// its own argument through parentheses to be correct. The attribute is what keeps
// -Wformat checking these call sites on GCC/Clang, which is where the pre-push
// lint runs; MSVC has no equivalent and simply takes the plain declarations.
#if defined(__GNUC__) || defined(__clang__)
void log_warn(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
void log_info(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
#else
void log_warn(const char* fmt, ...);
void log_info(const char* fmt, ...);
#endif

}  // namespace bomber::game
