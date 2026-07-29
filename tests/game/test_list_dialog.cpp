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
using bomber::game::list_dialog_thumb_y;
using bomber::game::list_dialog_width;
using bomber::game::ListDialogGeometry;

// The *.SCH picker's own call: sub_407582 pushes the literal (100, 100).
static ListDialogGeometry picker(int item_w, int title_w, int font_h = 12) {
    return list_dialog_geometry(100, 100, item_w, title_w, font_h);
}

// The same picker, scrolled: `total` items with `top` as the first visible one.
static ListDialogGeometry scrolled(int total, int top, int font_h = 12) {
    return list_dialog_geometry(100, 100, 200, 120, font_h, kListDialogRows,
                                /*footer_lines=*/0, total, top);
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
        // Unscrolled (and, by the default arguments, a list that fits): the
        // build-time position @0x42E08B, the top of the track.
        CHECK(g.sb_thumb_y0 == g.sb_track_y);
        // The scrollbar clears the item column rather than overlapping it.
        CHECK(g.item_frame_x1 < g.sb_frame_x0);
        CHECK(g.sb_frame_x1 == g.win_w - 6);
    }

    // ----------------------------------------------------------------------
    // The thumb's Y — @0x42E077 (build) and @0x42E6B5-0x42E6F7 (scroll).
    //
    // Every case below is a REGRESSION PIN for a real defect: the port used to
    // set sb_thumb_y0 = sb_track_y unconditionally, so scrolling never moved
    // the thumb. Each case with a non-zero top_row over an overflowing list
    // fails against that old code — deliberately, so the suite discriminates
    // rather than passing either way.
    // ----------------------------------------------------------------------

    TEST_CASE("a list that FITS parks the thumb at the top of the track") {
        // The total-vs-visible guard @0x42E681 skips the whole move block, so
        // the build-time position stands however the caller has scrolled.
        for (int total = 0; total <= kListDialogRows; ++total) {
            for (int top = 0; top <= 4; ++top) {
                const ListDialogGeometry g = scrolled(total, top);
                CHECK(g.sb_thumb_y0 == g.sb_track_y);
                CHECK(g.sb_thumb_y0 == 2 * 12 + 23);
            }
        }
    }

    TEST_CASE("the thumb slides with the top visible row") {
        const int fh = 12;
        // 20 items over 10 visible rows: max_top = 10, travel = 8*fh - 31 = 65.
        const ListDialogGeometry track = scrolled(20, 0, fh);
        const int y0 = track.sb_track_y;
        CHECK(y0 == 47);

        SUBCASE("top of the list is the top of the track") {
            CHECK(scrolled(20, 0, fh).sb_thumb_y0 == 47);
        }
        SUBCASE("one row down moves it by travel/max_top, truncated") {
            // 1 * 65 / 10 = 6 (the division truncates), NOT a whole 12-px row.
            CHECK(scrolled(20, 1, fh).sb_thumb_y0 == 47 + 6);
        }
        SUBCASE("halfway down") {
            CHECK(scrolled(20, 5, fh).sb_thumb_y0 == 47 + 32);  // 5*65/10 = 32
        }
        SUBCASE("the bottom of the list is the bottom of the travel") {
            CHECK(scrolled(20, 10, fh).sb_thumb_y0 == 47 + 65);
        }
    }

    TEST_CASE("the thumb moves CONTINUOUSLY, not in row-sized steps") {
        // The offset is one truncating division of top_row*travel by (total -
        // visible), so over a long list a scrolled row moves the thumb by far
        // less than a row height. A "one row of list = one row of pixels"
        // approximation would fail this.
        const int fh = 12;
        const int total = 110;  // max_top = 100, travel = 65 -> 0.65 px/row
        int moved = 0;
        for (int top = 1; top <= 100; ++top) {
            const int prev = scrolled(total, top - 1, fh).sb_thumb_y0;
            const int here = scrolled(total, top, fh).sb_thumb_y0;
            CHECK(here >= prev);          // monotonic
            CHECK(here - prev < fh);      // strictly finer than a row
            if (here != prev) ++moved;
        }
        CHECK(moved > 0);  // it really does move
        // ...and the extremes are still exact.
        CHECK(scrolled(total, 0, fh).sb_thumb_y0 == 47);
        CHECK(scrolled(total, 100, fh).sb_thumb_y0 == 47 + 65);
    }

    TEST_CASE("a list ONE row too long spends the whole travel in one step") {
        // max_top = 1, so the single available scroll step is the full span —
        // the degenerate end of the same formula, and the case that proves the
        // offset is not scaled by a row height.
        const int fh = 12;
        CHECK(scrolled(11, 0, fh).sb_thumb_y0 == 47);
        CHECK(scrolled(11, 1, fh).sb_thumb_y0 == 47 + 65);
    }

    TEST_CASE("the thumb stays inside its track, and stops 1px short at the end") {
        // The original's travel is the track height less 16, not less 15 (the
        // extra decrement @0x42E6CD), so a fully scrolled thumb leaves exactly
        // one pixel of track visible below it. Pinned as the faithful reading,
        // NOT rounded up to flush.
        for (int fh = 8; fh <= 24; ++fh) {
            const ListDialogGeometry g0 = scrolled(50, 0, fh);
            const int track_last = g0.sb_track_y + g0.sb_track_h - 1;
            for (int top = 0; top <= 40; ++top) {
                const ListDialogGeometry g = scrolled(50, top, fh);
                CHECK(g.sb_thumb_y0 >= g.sb_track_y);
                CHECK(g.sb_thumb_y1 <= track_last);
                // The SIZE never changes — this is not a proportional thumb.
                CHECK(g.sb_thumb_y1 - g.sb_thumb_y0 + 1 == 15);
                CHECK(g.sb_thumb_x0 == g.sb_track_x);
                CHECK(g.sb_thumb_x1 == g.win_w - 7);
            }
            CHECK(scrolled(50, 40, fh).sb_thumb_y1 == track_last - 1);
        }
    }

    TEST_CASE("scrolling moves ONLY the thumb — the track and window hold still") {
        // The original repaints a 15x15 block and nothing else (@0x42E6B0
        // erase, @0x42E6FE redraw); the track fill and its frame were computed
        // once at build time from the thumb's INITIAL y and are never touched.
        const ListDialogGeometry a = scrolled(40, 0);
        const ListDialogGeometry b = scrolled(40, 30);
        CHECK(a.sb_thumb_y0 != b.sb_thumb_y0);  // the point of the whole suite
        CHECK(b.sb_track_y == a.sb_track_y);
        CHECK(b.sb_track_h == a.sb_track_h);
        CHECK(b.sb_frame_y0 == a.sb_frame_y0);
        CHECK(b.sb_frame_y1 == a.sb_frame_y1);
        CHECK(b.sb_up_y == a.sb_up_y);
        CHECK(b.sb_down_y == a.sb_down_y);
        CHECK(b.win_h == a.win_h);
        CHECK(b.item_y0 == a.item_y0);
        CHECK(b.item_bottom == a.item_bottom);
        CHECK(b.done_y == a.done_y);
    }

    TEST_CASE("the helper is a pure function of the four list counters") {
        // list_dialog_thumb_y takes no highlight/selection argument at all —
        // that is the API expressing the RE fact. In sub_42DBCC the thumb is
        // recomputed only on the `-4` (top-row-changed) repaint; the `-3`
        // (highlight-moved) repaint leaves it alone, and the selected index
        // `edi + ebp` never reaches the expression at 0x42E6D0.
        CHECK(list_dialog_thumb_y(12, 10, 20, 0) == 47);
        CHECK(list_dialog_thumb_y(12, 10, 20, 10) == 112);
        CHECK(list_dialog_thumb_y(12, 10, 10, 3) == 47);  // fits -> parked
        // The PORT-ONLY clamp: an out-of-range offset cannot push the thumb
        // off its track (sub_42DBCC clamps `edi` itself, so this is only ever
        // reached by a port caller holding a stale value).
        CHECK(list_dialog_thumb_y(12, 10, 20, 999) == list_dialog_thumb_y(12, 10, 20, 10));
        CHECK(list_dialog_thumb_y(12, 10, 20, -5) == list_dialog_thumb_y(12, 10, 20, 0));
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
