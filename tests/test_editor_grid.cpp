// Checks for the scheme editor's pure grid model (editor_grid.hpp,
// docs/re/results-and-options.md #5, sub_4028D2): single-cell painting
// (PINNED — the original has no multi-cell brush), the Ctrl+F whole-grid
// flood fill, sub_4049C0's new-scheme defaults, player-start movement/team
// toggle, and Scheme<->EditorGrid round-tripping. SDL-free.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "bomber/assets/sch.hpp"
#include "bomber/game/editor_grid.hpp"

using namespace bomber::game;

TEST_CASE("reset gives sub_4049C0's classic new-scheme board, 15x11") {
    EditorGrid g;
    CHECK(g.width() == 15);
    CHECK(g.height() == 11);
    // sub_4049C0 (PINNED): even rows all brick ":::::::::::::::", odd rows
    // ":#:#:#:#:#:#:#:" — solid exactly where both x and y are odd.
    for (int y = 0; y < g.height(); ++y)
        for (int x = 0; x < g.width(); ++x) {
            EditorBrush want = ((y & 1) != 0 && (x & 1) != 0) ? EditorBrush::Solid
                                                              : EditorBrush::Brick;
            CHECK(g.cell(x, y) == want);
        }
    CHECK(g.density() == 90);  // dword_4647A0 = 90
}

TEST_CASE("reset alternates the start team flags (j & 1) and wraps positions") {
    // sub_4049C0: team = j & 1; positions wrap into the board with repeated
    // +=/-= width/height (the VALUELST 600..619 values arrive via start_xy).
    std::array<std::array<int, 2>, kEditorMaxStarts> pos{};
    pos[0] = {-1, -1};   // wraps to (14, 10)
    pos[1] = {15, 11};   // wraps to (0, 0)
    pos[2] = {31, 23};   // wraps twice to (1, 1)
    pos[3] = {7, 5};     // in range, unchanged
    EditorGrid g;
    g.reset(kEditorGridWidth, kEditorGridHeight, &pos);
    CHECK(g.start(0).x == 14);
    CHECK(g.start(0).y == 10);
    CHECK(g.start(1).x == 0);
    CHECK(g.start(1).y == 0);
    CHECK(g.start(2).x == 1);
    CHECK(g.start(2).y == 1);
    CHECK(g.start(3).x == 7);
    CHECK(g.start(3).y == 5);
    for (int j = 0; j < kEditorMaxStarts; ++j) CHECK(g.start(j).team == ((j & 1) != 0));
}

TEST_CASE("reset falls back to the 15x11 default for a non-positive size") {
    // reset() is a public, independently-testable entry point; a 0/negative
    // width or height must NOT be allowed to reach the start_xy wrap loops
    // (`while (x < 0) x += width_`), which would spin forever rather than
    // crash. Regression guard for that hang, not a documented original
    // behaviour (no known caller passes anything but 15x11 today).
    EditorGrid g;
    g.reset(0, 0);
    CHECK(g.width() == kEditorGridWidth);
    CHECK(g.height() == kEditorGridHeight);
    g.reset(-5, -1);
    CHECK(g.width() == kEditorGridWidth);
    CHECK(g.height() == kEditorGridHeight);
}

TEST_CASE("to_scheme emits the shipped -V,2 version") {
    EditorGrid g;
    CHECK(g.to_scheme().version == 2);  // every install scheme is "-V,2"
}

TEST_CASE("paint sets exactly the hovered cell (single-cell brush, PINNED)") {
    EditorGrid g;
    g.flood_fill(EditorBrush::Blank);  // clear sub_4049C0's bricked default board
    g.paint(3, 4, EditorBrush::Solid);
    CHECK(g.cell(3, 4) == EditorBrush::Solid);
    CHECK(g.cell(2, 4) == EditorBrush::Blank);
    CHECK(g.cell(4, 4) == EditorBrush::Blank);
    CHECK(g.cell(3, 3) == EditorBrush::Blank);
    CHECK(g.cell(3, 5) == EditorBrush::Blank);
}

TEST_CASE("paint out of bounds is a no-op, not a crash") {
    EditorGrid g;
    g.paint(-1, 0, EditorBrush::Solid);
    g.paint(0, -1, EditorBrush::Solid);
    g.paint(1000, 0, EditorBrush::Solid);
    g.paint(0, 1000, EditorBrush::Solid);
    CHECK(g.cell(-1, 0) == EditorBrush::Blank);  // out-of-bounds reads default to Blank too
}

TEST_CASE("flood fill paints the WHOLE grid, not a connected region") {
    EditorGrid g;
    g.paint(0, 0, EditorBrush::Solid);  // an isolated seed cell elsewhere
    g.flood_fill(EditorBrush::Brick);
    for (int y = 0; y < g.height(); ++y)
        for (int x = 0; x < g.width(); ++x) CHECK(g.cell(x, y) == EditorBrush::Brick);
}

TEST_CASE("density clamps to 0..100") {
    EditorGrid g;
    g.set_density(150);
    CHECK(g.density() == 100);
    g.set_density(-5);
    CHECK(g.density() == 0);
    g.set_density(42);
    CHECK(g.density() == 42);
}

TEST_CASE("move_start clamps to the board and toggle_start_team flips the flag") {
    EditorGrid g;
    g.move_start(0, 3, 4);
    CHECK(g.start(0).x == 3);
    CHECK(g.start(0).y == 4);
    CHECK(g.start(0).team == false);
    g.toggle_start_team(0);
    CHECK(g.start(0).team == true);
    g.toggle_start_team(0);
    CHECK(g.start(0).team == false);

    g.move_start(1, -5, 1000);
    CHECK(g.start(1).x == 0);
    CHECK(g.start(1).y == g.height() - 1);
}

TEST_CASE("move_start / toggle_start_team ignore out-of-range slots") {
    EditorGrid g;
    g.move_start(-1, 1, 1);
    g.move_start(kEditorMaxStarts, 1, 1);
    g.toggle_start_team(-1);
    g.toggle_start_team(kEditorMaxStarts);
    // No crash is the assertion; nothing else observable changed.
    CHECK(g.start(0).x != 1);
}

TEST_CASE("new grid seeds exactly kEditorPowerupKinds default rows") {
    EditorGrid g;
    REQUIRE(g.powerups().size() == static_cast<std::size_t>(kEditorPowerupKinds));
    for (int i = 0; i < kEditorPowerupKinds; ++i)
        CHECK(g.powerups()[static_cast<std::size_t>(i)].id == i);
}

TEST_CASE("Scheme -> EditorGrid -> Scheme round-trips every field") {
    bomber::assets::sch::Scheme s;
    s.version = 2;
    s.name = "ROUND TRIP";
    s.brick_density = 77;
    s.rows = {
        "###############",
        "#.............#",
        "#.###.#.#.###.#",
        "#.............#",
        "#.#.#.#.#.#.#.#",
        "#.............#",
        "#.#.#.#.#.#.#.#",
        "#.............#",
        "#.###.#.#.###.#",
        "#.............#",
        "###############",
    };
    for (int i = 0; i < kEditorMaxStarts; ++i) {
        bomber::assets::sch::Spawn sp;
        sp.player = i;
        sp.x = i;
        sp.y = 2;
        sp.extra = i % 2;
        s.spawns.push_back(sp);
    }
    for (int i = 0; i < kEditorPowerupKinds; ++i) {
        bomber::assets::sch::PowerupRule pr;
        pr.id = i;
        pr.born_with = i % 2;
        pr.forbidden = (i == 5);
        s.powerups.push_back(pr);
    }

    EditorGrid g;
    g.load_from_scheme(s);
    CHECK(g.width() == s.width());
    CHECK(g.height() == s.height());
    CHECK(g.density() == s.brick_density);
    CHECK(g.name() == s.name);
    for (int i = 0; i < kEditorMaxStarts; ++i) {
        CHECK(g.start(i).x == i);
        CHECK(g.start(i).y == 2);
        CHECK(g.start(i).team == (i % 2 != 0));
    }
    for (int y = 0; y < g.height(); ++y)
        for (int x = 0; x < g.width(); ++x)
            CHECK(g.cell(x, y) == cell_char_to_brush(static_cast<char>(s.at(x, y))));

    bomber::assets::sch::Scheme back = g.to_scheme();
    CHECK(back.name == s.name);
    CHECK(back.brick_density == s.brick_density);
    CHECK(back.rows == s.rows);
    REQUIRE(back.spawns.size() == s.spawns.size());
    for (std::size_t i = 0; i < s.spawns.size(); ++i) {
        CHECK(back.spawns[i].player == s.spawns[i].player);
        CHECK(back.spawns[i].x == s.spawns[i].x);
        CHECK(back.spawns[i].y == s.spawns[i].y);
        CHECK(back.spawns[i].extra == s.spawns[i].extra);
    }
    REQUIRE(back.powerups.size() == s.powerups.size());
    for (std::size_t i = 0; i < s.powerups.size(); ++i) {
        CHECK(back.powerups[i].id == s.powerups[i].id);
        CHECK(back.powerups[i].born_with == s.powerups[i].born_with);
        CHECK(back.powerups[i].forbidden == s.powerups[i].forbidden);
    }
}
