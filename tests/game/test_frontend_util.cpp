// THE CROSS-SCREEN FRONT-END HELPERS (frontend_util.hpp), which had no suite at
// all despite being the two functions ADR-0008's screen decomposition was most
// likely to break.
//
// pick_glue is the shared presentation LCG. Seven pre-match screens call it, and
// the header's warning is that a per-screen COPY would diverge the backdrop
// sequence — so what has to be pinned is not "it returns a GLUE name" but that
// the function is a pure step of (lcg, values): the same state in gives the same
// name and the same state out, wherever it is called from. A copy that drifted
// would still return plausible names, which is exactly why eyeballing the
// backdrop never catches it.
//
// reload_scheme is the case-insensitive DATA/SCHEMES resolve shared by the
// Options picker, the campaign stage loader and options.ini's schemefilename=.
// Its load-bearing property is the NEGATIVE one: on any failure the caller's
// Scheme must be left exactly as it was, because the front-end calls it on a
// live scheme the player is already editing.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <set>
#include <string>

#include "bomber/game_util/frontend_util.hpp"

namespace fs = std::filesystem;
using bomber::game::pick_glue;
using bomber::game::reload_scheme;

namespace {

bomber::assets::res::ValueList values_with_glue_count(std::int64_t n) {
    bomber::assets::res::ValueList v;
    v.columns[16] = {n};
    v.values[16] = n;
    return v;
}

// A synthetic scheme written through the shipped writer, so the fixture cannot
// drift from the format the loader accepts.
bomber::assets::sch::Scheme sample_scheme(const std::string& name) {
    bomber::assets::sch::Scheme s;
    s.version = 2;
    s.name = name;
    s.brick_density = 55;
    s.rows = {
        "###############", "#.............#", "#.###.#.#.###.#", "#.............#",
        "#.#.#.#.#.#.#.#", "#.............#", "#.#.#.#.#.#.#.#", "#.............#",
        "#.###.#.#.###.#", "#.............#", "###############",
    };
    return s;
}

// A DATA/SCHEMES tree under a unique temp root, torn down by the destructor so a
// failed assertion cannot leave the next run a stale fixture.
class SchemeDir {
public:
    explicit SchemeDir(const char* tag)
        : root_(fs::temp_directory_path() / ("bomber_fu_" + std::string(tag))) {
        fs::remove_all(root_);
        fs::create_directories(root_ / "DATA" / "SCHEMES");
    }
    ~SchemeDir() {
        std::error_code ec;
        fs::remove_all(root_, ec);
    }
    SchemeDir(const SchemeDir&) = delete;
    SchemeDir& operator=(const SchemeDir&) = delete;

    const fs::path& game_dir() const { return root_; }
    fs::path schemes() const { return root_ / "DATA" / "SCHEMES"; }

    void write_scheme(const std::string& file_name, const std::string& scheme_name) {
        bomber::assets::sch::write(sample_scheme(scheme_name), schemes() / file_name);
    }
    void write_raw(const std::string& file_name, const std::string& text) {
        std::ofstream(schemes() / file_name, std::ios::binary) << text;
    }

private:
    fs::path root_;
};

}  // namespace

// --- pick_glue -------------------------------------------------------------

TEST_CASE("pick_glue advances the shared LCG exactly once and pins its constants") {
    // Hand-computed from the header's literals rather than read back off the
    // function, so this case fails if either constant is edited:
    //   1 * 1664525 + 1013904223 = 1015568748
    //   1015568748 >> 16          = 15496
    //   15496 % 7 (the id-16 fallback count) = 5
    std::uint32_t lcg = 1;
    const std::string name = pick_glue(lcg, bomber::assets::res::ValueList{});
    CHECK(lcg == 1015568748u);  // the state MUST have moved, and moved to this
    CHECK(name == "GLUE5");
}

TEST_CASE("pick_glue is a pure step: same state in, same name and same state out") {
    // The property the seven call sites depend on. Two "screens" that share one
    // LCG must produce the same sequence as one screen called twice — a
    // per-screen copy of the helper is exactly what would break this.
    const auto values = values_with_glue_count(7);

    std::uint32_t one_screen = 12345;
    std::string a1 = pick_glue(one_screen, values);
    std::string a2 = pick_glue(one_screen, values);
    std::string a3 = pick_glue(one_screen, values);

    std::uint32_t shared = 12345;
    std::string b1 = pick_glue(shared, values);  // screen A
    std::string b2 = pick_glue(shared, values);  // screen B, same LCG
    std::string b3 = pick_glue(shared, values);  // screen C

    CHECK(a1 == b1);
    CHECK(a2 == b2);
    CHECK(a3 == b3);
    CHECK(one_screen == shared);
    // ...and the sequence is not a constant, or the check above proves nothing.
    CHECK((a1 != a2 || a2 != a3));
}

TEST_CASE("pick_glue stays inside the count getvalue(16) reports") {
    for (std::int64_t n : {1, 2, 3, 7, 13}) {
        CAPTURE(n);
        const auto values = values_with_glue_count(n);
        std::set<std::string> seen;
        std::uint32_t lcg = 99;
        for (int i = 0; i < 400; ++i) {
            const std::string g = pick_glue(lcg, values);
            REQUIRE(g.rfind("GLUE", 0) == 0);
            const int index = std::stoi(g.substr(4));
            CHECK(index >= 0);
            CHECK(index < static_cast<int>(n));
            seen.insert(g);
        }
        // Every index in range is actually reachable — a modulo that collapsed
        // to one value would still satisfy the bounds above.
        CHECK(seen.size() == static_cast<std::size_t>(n));
    }
}

TEST_CASE("pick_glue falls back to 7 when VALUELST has no id 16") {
    // The shipped fallback in the header (column_or(16, 0, 7)). An install with a
    // trimmed VALUELST must still pick a backdrop rather than divide by nothing.
    std::set<std::string> seen;
    std::uint32_t lcg = 4;
    for (int i = 0; i < 500; ++i) seen.insert(pick_glue(lcg, bomber::assets::res::ValueList{}));
    CHECK(seen.size() == 7);
    CHECK(seen.count("GLUE0") == 1);
    CHECK(seen.count("GLUE6") == 1);
    CHECK(seen.count("GLUE7") == 0);
}

TEST_CASE("pick_glue clamps a zero or negative count instead of dividing by it") {
    // The guard is one line (`if (glue_n < 1) glue_n = 1`) and it is the only
    // thing between a hand-edited VALUELST row and a modulo by zero.
    for (std::int64_t n : {0, -1, -1000}) {
        CAPTURE(n);
        const auto values = values_with_glue_count(n);
        std::uint32_t lcg = 77;
        for (int i = 0; i < 20; ++i) CHECK(pick_glue(lcg, values) == "GLUE0");
    }
}

// --- reload_scheme ---------------------------------------------------------

TEST_CASE("reload_scheme resolves a name with or without its extension") {
    SchemeDir dir("ext");
    dir.write_scheme("BASIC.SCH", "BASIC ARENA");

    bomber::assets::sch::Scheme s;
    CHECK(reload_scheme(s, dir.game_dir(), "BASIC"));
    CHECK(s.name == "BASIC ARENA");

    bomber::assets::sch::Scheme s2;
    CHECK(reload_scheme(s2, dir.game_dir(), "BASIC.SCH"));  // what the picker passes
    CHECK(s2.name == "BASIC ARENA");
}

TEST_CASE("reload_scheme matches the stem case-insensitively, both ways") {
    // A hand-edited options.ini and a MAKECFG-written one disagree about case,
    // and so do the shipped files across installs.
    SchemeDir dir("case");
    dir.write_scheme("MixedCase.sch", "MIXED");

    for (const char* asked : {"mixedcase", "MIXEDCASE", "MixedCase", "mIxEdCaSe.ScH"}) {
        CAPTURE(asked);
        bomber::assets::sch::Scheme s;
        CHECK(reload_scheme(s, dir.game_dir(), asked));
        CHECK(s.name == "MIXED");
    }
}

TEST_CASE("reload_scheme cuts at the LAST dot, so a dotted stem survives") {
    // sub_403EEE @0x403FE8 uses strrchr, not strchr: "MY.MAP.SCH" cuts to the
    // stem "MY.MAP". A strchr paraphrase would cut to "MY" and find nothing —
    // which is exactly what the second half of this case pins as the cost of the
    // faithful rule: an EXTENSIONLESS dotted name is ambiguous, and the last-dot
    // rule resolves it as "the tail is the extension". So "MY.MAP" cuts to "MY"
    // and does NOT resolve. That is a real edge a hand-written options.ini can
    // hit, and it is the original's behaviour rather than a port bug — pinned
    // here so nobody "fixes" it into a first-dot cut and silently breaks the
    // ordinary "BASIC.SCH" path in the other direction.
    SchemeDir dir("dots");
    dir.write_scheme("MY.MAP.SCH", "DOTTED");

    bomber::assets::sch::Scheme s;
    CHECK(reload_scheme(s, dir.game_dir(), "MY.MAP.SCH"));
    CHECK(s.name == "DOTTED");

    bomber::assets::sch::Scheme s2;
    CHECK_FALSE(reload_scheme(s2, dir.game_dir(), "MY.MAP"));  // cut to "MY": no such stem
}

TEST_CASE("a scheme that does not resolve leaves the caller's copy UNTOUCHED") {
    // The load-bearing negative. The front-end calls this on the scheme the
    // player is editing, so a failed resolve must not half-load or clear it.
    SchemeDir dir("missing");
    dir.write_scheme("BASIC.SCH", "BASIC ARENA");

    bomber::assets::sch::Scheme live;
    REQUIRE(reload_scheme(live, dir.game_dir(), "BASIC"));
    const bomber::assets::sch::Scheme before = live;

    CHECK_FALSE(reload_scheme(live, dir.game_dir(), "NOSUCHSCHEME"));
    CHECK(live.name == before.name);
    CHECK(live.rows == before.rows);
    CHECK(live.brick_density == before.brick_density);
    CHECK(live.version == before.version);
}

TEST_CASE("a CORRUPT scheme is refused, and also leaves the copy untouched") {
    // load() throws on a bad numeric field (tests/assets/test_sch_malformed.cpp);
    // reload_scheme catches it. Without the catch the Options picker would take
    // an exception out through a screen's draw loop.
    SchemeDir dir("corrupt");
    dir.write_scheme("GOOD.SCH", "GOOD ONE");
    // The same non-numeric -V fixture tests/assets/test_sch_malformed.cpp proves
    // throws, so this case cannot silently stop testing the catch.
    dir.write_raw("BROKEN.SCH", "-V,x\n-R,0,###\n-R,1,#.#\n-R,2,###\n");

    bomber::assets::sch::Scheme live;
    REQUIRE(reload_scheme(live, dir.game_dir(), "GOOD"));

    CHECK_FALSE(reload_scheme(live, dir.game_dir(), "BROKEN"));
    CHECK(live.name == "GOOD ONE");  // still the good one, not half of the bad one
}

TEST_CASE("reload_scheme refuses names that cut down to nothing") {
    SchemeDir dir("empty");
    dir.write_scheme("BASIC.SCH", "BASIC ARENA");

    bomber::assets::sch::Scheme s;
    CHECK_FALSE(reload_scheme(s, dir.game_dir(), ""));
    CHECK_FALSE(reload_scheme(s, dir.game_dir(), ".SCH"));  // stem cuts to empty
    CHECK_FALSE(reload_scheme(s, dir.game_dir(), "."));
}

TEST_CASE("reload_scheme ignores non-.SCH files and a missing SCHEMES directory") {
    SchemeDir dir("stray");
    dir.write_raw("BASIC.TXT", "not a scheme at all\n");

    bomber::assets::sch::Scheme s;
    CHECK_FALSE(reload_scheme(s, dir.game_dir(), "BASIC"));  // right stem, wrong extension

    // No DATA/SCHEMES at all: the directory_iterator takes an error_code, so this
    // is a clean false rather than a filesystem_error escaping into a screen.
    const fs::path absent = fs::temp_directory_path() / "bomber_fu_no_such_install";
    CHECK_FALSE(reload_scheme(s, absent, "BASIC"));
}
