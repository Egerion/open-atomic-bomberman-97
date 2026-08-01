#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <string>
#include <string_view>
#include <vector>

#include "bomber/game_util/log.hpp"

// The front-end's diagnostic seam (docs/coding-standards.md §11). libs/game held
// 69 of the repo's 76 raw console-output sites; 53 of them now route through
// log_warn/log_info. What makes that a fix rather than a rename is that the
// output became REDIRECTABLE — so that is what this suite pins.
//
// The recorder is a file-scope object because a LogSink is a plain function
// pointer (no std::function, so no captures): that is deliberate in the seam
// itself, and it costs a test one static.

namespace {

struct Record {
    bomber::game::LogLevel level;
    std::string message;
};

std::vector<Record> g_recorded;

void recording_sink(bomber::game::LogLevel level, std::string_view message) {
    g_recorded.push_back(Record{level, std::string(message)});
}

void discarding_sink(bomber::game::LogLevel, std::string_view) {}

// RAII so a failing CHECK cannot leave the recorder installed for the next case.
struct SinkGuard {
    explicit SinkGuard(bomber::game::LogSink sink) : previous_(bomber::game::set_log_sink(sink)) {
        g_recorded.clear();
    }
    ~SinkGuard() { bomber::game::set_log_sink(previous_); }
    SinkGuard(const SinkGuard&) = delete;
    SinkGuard& operator=(const SinkGuard&) = delete;

private:
    bomber::game::LogSink previous_;
};

}  // namespace

TEST_CASE("a sink receives the formatted line, without a trailing newline") {
    const SinkGuard guard(&recording_sink);

    bomber::game::log_warn("%s load failed: %s", "EDIT.ANI", "no such file");

    REQUIRE(g_recorded.size() == 1);
    // The call sites dropped their "\n" when they moved to the seam — the sink
    // adds it (or does not, if it is writing somewhere a newline would be wrong).
    // If the format string kept its newline this would read "...no such file\n".
    CHECK(g_recorded[0].message == "EDIT.ANI load failed: no such file");
    CHECK(g_recorded[0].level == bomber::game::LogLevel::Warn);
}

TEST_CASE("level distinguishes a degraded outcome from progress") {
    const SinkGuard guard(&recording_sink);

    bomber::game::log_info("renderer: %s", "direct3d11");
    bomber::game::log_warn("audio unavailable, continuing silent");

    REQUIRE(g_recorded.size() == 2);
    CHECK(g_recorded[0].level == bomber::game::LogLevel::Info);
    CHECK(g_recorded[1].level == bomber::game::LogLevel::Warn);
}

// The long-line path is a SECOND code path in format_line: vsnprintf reports the
// length it wanted, and anything past the 256-byte stack buffer is re-formatted
// into a heap vector. An install with a deep path reaches it (several call sites
// splice a std::filesystem::path), and a truncating implementation would still
// look correct in every short case above.
TEST_CASE("a line longer than the internal stack buffer is not truncated") {
    const SinkGuard guard(&recording_sink);
    const std::string long_path(400, 'p');

    bomber::game::log_warn("front-end PCX '%s' load failed", long_path.c_str());

    const std::string expected = "front-end PCX '" + long_path + "' load failed";
    REQUIRE(g_recorded.size() == 1);
    CHECK(g_recorded[0].message == expected);  // full equality: nothing was cut at 256
}

TEST_CASE("set_log_sink returns the previous sink, so callers can nest and restore") {
    const bomber::game::LogSink original = bomber::game::set_log_sink(&recording_sink);
    g_recorded.clear();

    const bomber::game::LogSink outer = bomber::game::set_log_sink(&discarding_sink);
    bomber::game::log_warn("swallowed");
    CHECK(g_recorded.empty());  // a capture run silences the package this way

    bomber::game::set_log_sink(outer);  // restore what the outer scope installed
    bomber::game::log_warn("heard");
    CHECK(g_recorded.size() == 1);

    bomber::game::set_log_sink(original);
}

// nullptr means "restore the default", NOT "silence" — the header says so, and a
// sink that could be null would make "no output" two different states, only one
// of which any test could observe. Silencing is discarding_sink, above.
TEST_CASE("passing nullptr restores the default sink rather than clearing it") {
    const bomber::game::LogSink original = bomber::game::set_log_sink(&recording_sink);
    g_recorded.clear();

    bomber::game::set_log_sink(nullptr);
    bomber::game::log_warn("goes to the default sink, not to the recorder");
    CHECK(g_recorded.empty());

    // And the recorder really was displaced rather than merely bypassed: putting
    // it back makes output visible again.
    bomber::game::set_log_sink(&recording_sink);
    bomber::game::log_info("recorded again");
    CHECK(g_recorded.size() == 1);

    bomber::game::set_log_sink(original);
}
