// Checks for the scheme editor's pure grid model (editor_grid.hpp,
// docs/re/results-and-options.md #5, sub_4028D2): brush stamping, the
// Ctrl+F whole-grid flood fill, player-start movement/team toggle, and
// Scheme<->EditorGrid round-tripping. SDL-free — no renderer needed.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "bomber/assets/sch.hpp"
#include "bomber/game/editor_grid.hpp"

using namespace bomber::game;

TEST_CASE("reset gives an all-blank board of the confirmed 15x11 size") {
    EditorGrid g;
    CHECK(g.width() == 15);
    CHECK(g.height() == 11);
    for (int y = 0; y < g.height(); ++y)
        for (int x = 0; x < g.width(); ++x) CHECK(g.cell(x, y) == EditorBrush::Blank);
}

TEST_CASE("paint sets exactly the hovered cell") {
    EditorGrid g;
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

TEST_CASE("stamp size 1 behaves exactly like paint") {
    EditorGrid g;
    g.stamp(5, 5, 1, EditorBrush::Brick);
    CHECK(g.cell(5, 5) == EditorBrush::Brick);
    CHECK(g.cell(4, 5) == EditorBrush::Blank);
    CHECK(g.cell(6, 5) == EditorBrush::Blank);
}

TEST_CASE("stamp size 2 covers a 2x2 block leaning top-left") {
    EditorGrid g;
    g.stamp(5, 5, 2, EditorBrush::Solid);
    CHECK(g.cell(5, 5) == EditorBrush::Solid);
    CHECK(g.cell(4, 5) == EditorBrush::Solid);
    CHECK(g.cell(5, 4) == EditorBrush::Solid);
    CHECK(g.cell(4, 4) == EditorBrush::Solid);
    // Not painted outside the 2x2 block.
    CHECK(g.cell(6, 5) == EditorBrush::Blank);
    CHECK(g.cell(5, 6) == EditorBrush::Blank);
}

TEST_CASE("stamp size 3 covers a full 3x3 block centered on the cursor") {
    EditorGrid g;
    g.stamp(5, 5, 3, EditorBrush::Solid);
    for (int dy = -1; dy <= 1; ++dy)
        for (int dx = -1; dx <= 1; ++dx) CHECK(g.cell(5 + dx, 5 + dy) == EditorBrush::Solid);
    CHECK(g.cell(3, 5) == EditorBrush::Blank);
    CHECK(g.cell(7, 5) == EditorBrush::Blank);
}

TEST_CASE("stamp clamps to the board edge without crashing") {
    EditorGrid g;
    g.stamp(0, 0, 3, EditorBrush::Solid);  // top-left corner
    CHECK(g.cell(0, 0) == EditorBrush::Solid);
    g.stamp(g.width() - 1, g.height() - 1, 3, EditorBrush::Brick);  // bottom-right corner
    CHECK(g.cell(g.width() - 1, g.height() - 1) == EditorBrush::Brick);
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
