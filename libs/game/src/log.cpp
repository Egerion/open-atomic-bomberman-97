#include "bomber/game/log.hpp"

#include <cstdarg>
#include <cstdio>
#include <string>
#include <vector>

namespace bomber::game {
namespace {

// The default sink reproduces exactly what the 69 call sites did before the seam
// existed: the line, a newline, stderr. Keeping that byte-identical is what makes
// the seam a refactor rather than a behaviour change.
void write_to_stderr(LogLevel, std::string_view message) {
    std::fprintf(stderr, "%.*s\n", static_cast<int>(message.size()), message.data());
}

// The one piece of process-wide mutable state in libs/game, and it is the
// irreducible core of what a routable log seam IS — the alternative is threading
// a logger reference through every screen constructor to reach the asset loader.
// Confined to this translation unit and reachable only through set_log_sink.
LogSink g_sink = &write_to_stderr;

// Format into a std::string. vsnprintf tells us the length it WANTED, so the
// common short line costs one pass over a stack buffer and a long one costs a
// second pass rather than being truncated.
std::string format_line(const char* fmt, std::va_list args) {
    std::va_list probe;
    va_copy(probe, args);
    char stack[256];
    const int needed = std::vsnprintf(stack, sizeof(stack), fmt, probe);
    va_end(probe);
    if (needed < 0) return std::string(fmt);  // a malformed format string: say so verbatim
    if (static_cast<std::size_t>(needed) < sizeof(stack)) return std::string(stack);
    std::vector<char> heap(static_cast<std::size_t>(needed) + 1);
    std::vsnprintf(heap.data(), heap.size(), fmt, args);
    return std::string(heap.data(), static_cast<std::size_t>(needed));
}

// g_sink is never null — set_log_sink maps nullptr back to the default — so a
// caller silences the package by installing a sink that discards, not by
// clearing it. That keeps "no sink" from being a second, untestable state.
void emit(LogLevel level, const char* fmt, std::va_list args) {
    g_sink(level, format_line(fmt, args));
}

}  // namespace

LogSink set_log_sink(LogSink sink) {
    LogSink previous = g_sink;
    g_sink = sink != nullptr ? sink : &write_to_stderr;
    return previous;
}

void log_warn(const char* fmt, ...) {
    std::va_list args;
    va_start(args, fmt);
    emit(LogLevel::Warn, fmt, args);
    va_end(args);
}

void log_info(const char* fmt, ...) {
    std::va_list args;
    va_start(args, fmt);
    emit(LogLevel::Info, fmt, args);
    va_end(args);
}

}  // namespace bomber::game
