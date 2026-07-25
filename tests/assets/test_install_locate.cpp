// Checks for bomber::assets::default_game_dir — the install locator every
// entry point funnels through.
//
// This is the code path that decides whether a freshly-unzipped copy of the
// game starts at all, and it had a hole worth pinning: gamedir.txt was only
// ever opened relative to the WORKING directory, so the exe found an install
// on a machine that happened to have one at a hardcoded absolute path and
// silently found nothing anywhere else. The probes below therefore assert the
// EXE-DIRECTORY lookups specifically, and assert them by ORDER — an exe_dir
// hit must beat the hardcoded absolute fallbacks, otherwise a developer
// machine (which has one of those directories) would pass the test while a
// clean machine still failed.
//
// BOMBER_GAME_DIR is cleared for the duration: it is probed first by design,
// so a value in the tester's environment would mask every case here.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

#include "bomber/assets/install.hpp"

namespace fs = std::filesystem;
using bomber::assets::default_game_dir;

namespace {

void clear_game_dir_env() {
#ifdef _WIN32
    _putenv_s("BOMBER_GAME_DIR", "");  // empty value removes it on Windows
#else
    unsetenv("BOMBER_GAME_DIR");
#endif
}

// A scratch tree that cleans itself up, named per-case so parallel ctest runs
// cannot collide.
struct TempTree {
    fs::path root;
    explicit TempTree(const char* tag)
        : root(fs::temp_directory_path() / ("bomber_install_" + std::string(tag))) {
        fs::remove_all(root);
        fs::create_directories(root);
    }
    ~TempTree() {
        std::error_code ec;
        fs::remove_all(root, ec);
    }
    TempTree(const TempTree&) = delete;
    TempTree& operator=(const TempTree&) = delete;
};

void write_line(const fs::path& file, const std::string& text) {
    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    out << text;
}

}  // namespace

TEST_CASE("gamedir.txt beside the exe is honoured") {
    clear_game_dir_env();
    TempTree t("beside");
    const fs::path exe_dir = t.root / "bin";
    const fs::path install = t.root / "install";
    fs::create_directories(exe_dir);
    fs::create_directories(install / "DATA");
    write_line(exe_dir / "gamedir.txt", install.string());

    // Must win over the hardcoded absolute probes, which on a dev machine do
    // exist — that ordering is the whole point.
    CHECK(default_game_dir(exe_dir) == install);
}

TEST_CASE("a UTF-8 BOM in gamedir.txt does not become part of the path") {
    // PowerShell's `-Encoding utf8` writes a BOM. Left in, it silently turns
    // the path into a different, non-existent one — which reads to the user
    // exactly like the file being ignored.
    clear_game_dir_env();
    TempTree t("bom");
    const fs::path exe_dir = t.root / "bin";
    const fs::path install = t.root / "install";
    fs::create_directories(exe_dir);
    fs::create_directories(install / "DATA");
    write_line(exe_dir / "gamedir.txt", "\xEF\xBB\xBF" + install.string());

    CHECK(default_game_dir(exe_dir) == install);
}

TEST_CASE("the exe dropped INTO the install folder needs no configuration") {
    // The thing a player actually does with one self-contained binary: copy it
    // in among the game files and double-click. DATA/ identifies an install,
    // so the folder's name is irrelevant — this one is deliberately not
    // called BOMBRMAN.
    clear_game_dir_env();
    TempTree t("inside");
    const fs::path exe_dir = t.root / "Atomic Bomberman kopya";
    fs::create_directories(exe_dir / "DATA");

    CHECK(default_game_dir(exe_dir) == exe_dir);
}

TEST_CASE("a BOMBRMAN folder beside the exe needs no configuration at all") {
    clear_game_dir_env();
    TempTree t("adjacent");
    const fs::path exe_dir = t.root / "bin";
    fs::create_directories(exe_dir / "BOMBRMAN" / "DATA");

    CHECK(default_game_dir(exe_dir) == exe_dir / "BOMBRMAN");
}

TEST_CASE("a gamedir.txt naming a missing directory is rejected, not returned") {
    // A stale path must fall through to the remaining probes rather than
    // being handed back as a game_dir the caller then fails to open.
    clear_game_dir_env();
    TempTree t("stale");
    const fs::path exe_dir = t.root / "bin";
    fs::create_directories(exe_dir);
    write_line(exe_dir / "gamedir.txt", (t.root / "no-such-install").string());

    const fs::path got = default_game_dir(exe_dir);
    CHECK(got != t.root / "no-such-install");
}

TEST_CASE("an empty exe_dir keeps the old behaviour (no crash, no exe probes)") {
    // apps/viewer still calls the no-argument form; it must stay valid.
    clear_game_dir_env();
    CHECK_NOTHROW((void)default_game_dir());
}
