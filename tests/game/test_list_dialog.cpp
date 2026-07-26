// The generic list dialog's pinned pixel geometry — sub_42DBCC
// (list_dialog_geometry.hpp, docs/re/results-and-options.md §5c). These
// numbers were read out of BM95.EXE on 2026-07-26; the suite exists so a
// refactor of the chrome cannot quietly move them, the same way
// test_golden.cpp pins the sim.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "bomber/game/list_dialog_geometry.hpp"

using bomber::game::kListDialogRows;
using bomber::game::list_dialog_geometry;
using bomber::game::list_dialog_width;
using bomber::game::ListDialogGeometry;

// The *.SCH picker's own call: sub_407582 pushes the literal (100, 100).
static ListDialogGeometry picker(int item_w, int title_w, int font_h = 12) {
    return list_dialog_geometry(100, 100, item_w, title_w, font_h);
}

TEST_SUITE("list dialog geometry (sub_42DBCC)") {
    TEST_CASE("ten visible rows, not thirteen") {
        // @0x42DC44 seeds rows = 10 and the height multiplier = 13 as SEPARATE
        // counters. Thirteen is the multiplier.
        CHECK(kListDialogRows == 10);
    }

    TEST_CASE("window is placed at the caller's literal x/y, never centred") {
        // sub_43C734 is 6-arg (x, y, w, h, colormode, flags) — its a1/a2 reach
        // sub_43D398's clip checks as x and y. The picker passes (100, 100).
        const ListDialogGeometry g = picker(200, 120);
        CHECK(g.win_x == 100);
        CHECK(g.win_y == 100);
    }

    TEST_CASE("width = max(item_w + 16, title_w) + 20, and item_w = win_w - 36") {
        SUBCASE("items win") {
            const ListDialogGeometry g = picker(200, 120);
            CHECK(g.win_w == 200 + 16 + 20);
            CHECK(g.item_w == g.win_w - 36);
            CHECK(g.item_w == 200);
        }
        SUBCASE("a wider title takes over and bumps item_w with it") {
            const ListDialogGeometry g = picker(100, 300);
            CHECK(g.win_w == 300 + 20);
            CHECK(g.item_w == g.win_w - 36);
            // The original keeps the invariant `item_w + 16 == span`.
            CHECK(g.item_w + 16 == 300);
        }
        SUBCASE("the +16/+20 split is what list_dialog_width reports") {
            CHECK(list_dialog_width(200, 120) == 236);
            CHECK(list_dialog_width(100, 300) == 320);
        }
    }

    TEST_CASE("height = (rows + 3) * fontheight + 22") {
        // @0x42DC5E: fontheight * 13 + 22 at the pinned 10 rows. The retry loop
        // decrements rows and the multiplier together, so the +3 is the invariant.
        CHECK(picker(200, 120, 12).win_h == 13 * 12 + 22);
        CHECK(picker(200, 120, 16).win_h == 13 * 16 + 22);
        for (int rows = 6; rows <= 10; ++rows) {
            const ListDialogGeometry g = list_dialog_geometry(100, 100, 200, 120, 12, rows);
            CHECK(g.win_h == (rows + 3) * 12 + 22);
        }
    }

    TEST_CASE("the item area fits inside the window it asks for") {
        // The arithmetic proof that 13 rows is impossible: the item area alone
        // would need 14*fh + 16, past a 13*fh + 22 window for any fh > 6.
        for (int font_h = 8; font_h <= 24; ++font_h) {
            const ListDialogGeometry g = picker(200, 120, font_h);
            CHECK(g.item_bottom < g.win_h);
            CHECK(g.done_y + font_h + 6 <= g.win_h);  // button height is fh + 6
            CHECK(g.item_bottom <= g.done_y);
        }
        // ...and it would NOT fit at thirteen.
        const int font_h = 12;
        const int thirteen_row_bottom = font_h + 16 + 13 * font_h;
        CHECK(thirteen_row_bottom > 13 * font_h + 22);
    }

    TEST_CASE("title strip and title placement") {
        const ListDialogGeometry g = picker(200, 120, 12);
        // Fill (5,5) sized (w-11) x (fh+3) @0x42DD9E; sunken bevel to
        // (w-6, fh+8) @0x42DE0E.
        CHECK(g.strip_fill_w == g.win_w - 11);
        CHECK(g.strip_fill_h == 12 + 3);
        CHECK(g.strip_x1 == g.win_w - 6);
        CHECK(g.strip_y1 == 12 + 8);
        // Centred at y = 8 @0x42DDCB (the byte offset is w*8 + w/2 - tw/2).
        CHECK(g.title_y == 8);
        CHECK(g.title_x == g.win_w / 2 - 120 / 2);
    }

    TEST_CASE("item column origin and pitch") {
        const ListDialogGeometry g = picker(200, 120, 12);
        // Base pointer `bitmap + (fh+16)*pitch + 8` @0x42DE4A.
        CHECK(g.item_x == 8);
        CHECK(g.item_y0 == 12 + 16);
        CHECK(g.item_h == 12);
        CHECK(g.item_bottom == g.item_y0 + kListDialogRows * 12);
        // Fill is the base pointer minus 3 px and minus 2 rows @0x42DEA2.
        CHECK(g.item_fill_x == 5);
        CHECK(g.item_fill_y == g.item_y0 - 2);
        CHECK(g.item_fill_w == g.item_w + 6);
        CHECK(g.item_fill_h == kListDialogRows * 12 + 2);
        CHECK(g.item_frame_x1 == g.item_w + 10);
        CHECK(g.item_frame_y1 == g.item_bottom);
    }

    TEST_CASE("scrollbar geometry, including the fixed-size thumb") {
        const int fh = 12;
        const ListDialogGeometry g = picker(200, 120, fh);
        CHECK(g.sb_button_x == g.win_w - 25);
        CHECK(g.sb_up_y == fh + 13);
        CHECK(g.sb_down_y == g.item_bottom - fh - 5);
        CHECK(g.sb_track_x == g.win_w - 21);
        CHECK(g.sb_track_y == 2 * fh + 23);
        CHECK(g.sb_track_w == 15);
        // The thumb is a HARD 15x15 @0x42E1D4 — not proportional to the list.
        CHECK(g.sb_thumb_x1 - g.sb_thumb_x0 + 1 == 15);
        CHECK(g.sb_thumb_y1 - g.sb_thumb_y0 + 1 == 15);
        CHECK(g.sb_thumb_x0 == g.sb_track_x);
        CHECK(g.sb_thumb_y0 == g.sb_track_y);
        // The scrollbar clears the item column rather than overlapping it.
        CHECK(g.item_frame_x1 < g.sb_frame_x0);
        CHECK(g.sb_frame_x1 == g.win_w - 6);
    }

    TEST_CASE("Done button placement matches sub_414340's Ok rule") {
        const ListDialogGeometry g = picker(200, 120, 12);
        CHECK(g.done_x == g.win_w / 2 - 32);  // @0x42E072
        CHECK(g.done_y == g.win_h - 12 - 14);
    }

    TEST_CASE("the PORT-ONLY footer block only grows the window downward") {
        const ListDialogGeometry base = picker(200, 120, 12);
        const ListDialogGeometry with =
            list_dialog_geometry(100, 100, 200, 120, 12, kListDialogRows,
                                 /*footer_lines=*/2);
        // Everything down to the item area is byte-identical...
        CHECK(with.win_w == base.win_w);
        CHECK(with.item_x == base.item_x);
        CHECK(with.item_y0 == base.item_y0);
        CHECK(with.item_bottom == base.item_bottom);
        CHECK(with.sb_track_y == base.sb_track_y);
        // ...only the height and the bottom-anchored button move.
        CHECK(with.win_h == base.win_h + 8 + 2 * 12);
        CHECK(with.done_y == with.win_h - 12 - 14);
        CHECK(with.footer_y0 == with.item_bottom + 8);
        CHECK(with.footer_y0 + 2 * 12 <= with.done_y);
    }

}  // TEST_SUITE
