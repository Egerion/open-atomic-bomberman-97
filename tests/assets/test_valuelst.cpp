// Checks for the VALUELST multi-column support (bomber::assets::res). The
// original getvalue() (sub_412135) reads a FLAT array the loader (sub_4121FF)
// fills by splitting each "id,a,b,c" row into consecutive slots, so
// getvalue(700/701/702) are columns 0/1/2 of row 700. Our loader keeps the
// single-value `values` map (first column, what the sim reads) byte-identical
// and ALSO records every column in `columns` for the presentation layer (the
// main-menu cursor anchor). See docs/re/frontend-flow.md.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>
#include <string>

#include "bomber/assets/reslist.hpp"

namespace fs = std::filesystem;
using bomber::assets::res::load_values;

namespace {

fs::path write_temp(const std::string& body) {
    static int counter = 0;
    fs::path p = fs::temp_directory_path() / ("bomber_vl_" + std::to_string(counter++) + ".res");
    std::ofstream f(p, std::ios::binary);
    f << body;
    return p;
}

}  // namespace

TEST_CASE("single-value rows keep their first-column semantics (sim path unchanged)") {
    // The exact main-menu cursor row from the shipped VALUELST, plus a plain row.
    auto p = write_temp(
        "; MAINMENU cursor: X, Y, YS, W\n"
        "700,332,140, 38,  0\n"
        "12,7\n");
    auto vl = load_values(p);
    // `values` (what match_factory feeds Tuning::apply) is the FIRST column only.
    CHECK(vl.values.at(700) == 332);
    CHECK(vl.values.at(12) == 7);
    CHECK(vl.at_or(700, -1) == 332);
    CHECK(vl.at_or(999, -1) == -1);
    fs::remove(p);
}

TEST_CASE("columns exposes every value of a multi-value row (getvalue 700/701/702)") {
    auto p = write_temp("700,332,140, 38,  0\n");
    auto vl = load_values(p);
    REQUIRE(vl.columns.count(700) == 1);
    const auto& cols = vl.columns.at(700);
    REQUIRE(cols.size() == 4);
    CHECK(cols[0] == 332);  // getvalue(700) = cursor X
    CHECK(cols[1] == 140);  // getvalue(701) = cursor Y
    CHECK(cols[2] == 38);   // getvalue(702) = cursor Y-step
    CHECK(cols[3] == 0);    // getvalue(703) = field width

    // column_or mirrors getvalue(700 + n): present columns, else the fallback.
    CHECK(vl.column_or(700, 0, -1) == 332);
    CHECK(vl.column_or(700, 1, -1) == 140);
    CHECK(vl.column_or(700, 2, -1) == 38);
    CHECK(vl.column_or(700, 9, -7) == -7);   // past the row -> fallback
    CHECK(vl.column_or(701, 0, -7) == -7);   // no such row (it's a column) -> fallback
    fs::remove(p);
}

TEST_CASE("a non-numeric column ends the numeric run for that row") {
    // Whitespace and a trailing comment must not corrupt the column list.
    auto p = write_temp("710, 70,170, 24,150   ; input listing\n");
    auto vl = load_values(p);
    REQUIRE(vl.columns.count(710) == 1);
    const auto& cols = vl.columns.at(710);
    REQUIRE(cols.size() == 4);
    CHECK(cols[0] == 70);
    CHECK(cols[3] == 150);
    fs::remove(p);
}
