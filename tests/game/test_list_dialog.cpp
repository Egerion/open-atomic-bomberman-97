// The generic list dialog's pinned pixel geometry — sub_42DBCC
// (list_dialog_geometry.hpp, docs/re/results-and-options.md §5c). These
// numbers were read out of BM95.EXE on 2026-07-26; the suite exists so a
// refactor of the chrome cannot quietly move them, the same way
// test_golden.cpp pins the sim.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <utility>  // std::pair (the dead-space sweep)

#include "bomber/game_util/list_dialog_geometry.hpp"

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
        // ...and it would NOT fit at thirteen. Asked against the SHIPPED
        // geometry rather than against two locally computed constants, which
        // would be a statement about arithmetic and could not fail.
        const ListDialogGeometry g = picker(200, 120, 12);
        CHECK(g.item_y0 + 13 * g.item_h > g.win_h);
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
        // Exactly the travel, because the offset climbs 0.65 px per row and can
        // therefore never skip a pixel: 65 of the 100 steps move the thumb and
        // 35 do not. A thumb that never moved satisfies both bounds above, so
        // this is the assertion the case turns on.
        CHECK(moved == 65);
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

// ---------------------------------------------------------------------------
// sub_42DBCC's INPUT MODEL — read out of BM95.EXE 2026-07-29. The geometry
// suite above pins what the widget LOOKS like; this one pins what it DOES.

using bomber::game::kListKeyDown;
using bomber::game::kListKeyEnd;
using bomber::game::kListKeyEnter;
using bomber::game::kListKeyEscape;
using bomber::game::kListKeyHome;
using bomber::game::kListKeyPageDown;
using bomber::game::kListKeyPageUp;
using bomber::game::kListKeyUp;
using bomber::game::list_dialog_hit_test;
using bomber::game::list_dialog_key;
using bomber::game::list_dialog_letter_jump;
using bomber::game::list_dialog_mouse_down;
using bomber::game::list_dialog_mouse_move;
using bomber::game::list_dialog_mouse_up;
using bomber::game::ListDialogAction;
using bomber::game::ListDialogHit;
using bomber::game::ListDialogNav;
using bomber::game::ListDialogWidget;

namespace {

constexpr int kRows = kListDialogRows;  // 10

// The *.SCH picker's own dialog, hit-tested with plausible button boxes: a
// FONT6 arrow glyph is narrow, "Done" is not. Only the widths matter to the
// hit test, and neither box overlaps the item column (which ends at x = 208).
constexpr int kArrowW = 20, kBtnH = 18, kDoneW = 60;

ListDialogHit hit_at(const ListDialogGeometry& g, int x, int y) {
    return list_dialog_hit_test(g, kRows, kArrowW, kBtnH, kDoneW, kBtnH, x, y);
}

// Screen-space centre of visible row `i` in the default picker geometry.
int row_x() { return 100 + 8 + 4; }
int row_y(const ListDialogGeometry& g, int i) { return 100 + g.item_y0 + i * g.item_h + 1; }

// A screen-space x inside the scrollbar track.
int track_x(const ListDialogGeometry& g) {
    return 100 + g.sb_track_x + 2;
}

// One press on the track at screen y `sy`, which the widget needs twice — once
// to resolve the hotspot and once to place the click within the thumb band.
ListDialogAction press_track(ListDialogNav& nav, const ListDialogGeometry& g, int sx, int sy) {
    return list_dialog_mouse_down(nav, g, hit_at(g, sx, sy), kRows, 40, sy);
}

}  // namespace

TEST_SUITE("list dialog input model (sub_42DBCC)") {

    // --- item 2: THERE IS NO WRAP -----------------------------------------

    TEST_CASE("Up at the very top of the list does NOTHING") {
        // @0x42E476: highlight == 0 falls to the edi test @0x42E48C, which is
        // also 0, and the `jle` lands on the repaint dispatch having changed
        // neither register. The port used to do (row + count - 1) % count and
        // jump to the LAST item.
        ListDialogNav nav{0, 0};
        CHECK(list_dialog_key(nav, kListKeyUp, kRows, 40) == ListDialogAction::None);
        CHECK(nav.top_row == 0);
        CHECK(nav.highlight == 0);
    }

    TEST_CASE("Down at the very bottom of the list does NOTHING") {
        // @0x42E4BD: top + visible == total, so the `jge` skips the increment.
        ListDialogNav nav{30, 9};
        CHECK(list_dialog_key(nav, kListKeyDown, kRows, 40) == ListDialogAction::None);
        CHECK(nav.top_row == 30);
        CHECK(nav.highlight == 9);
    }

    TEST_CASE("Up/Down move the HIGHLIGHT first and only then the view") {
        ListDialogNav nav{5, 4};
        list_dialog_key(nav, kListKeyDown, kRows, 40);
        CHECK(nav.top_row == 5);  // the view holds still...
        CHECK(nav.highlight == 5);
        list_dialog_key(nav, kListKeyUp, kRows, 40);
        CHECK(nav.top_row == 5);
        CHECK(nav.highlight == 4);

        // ...until the highlight is already parked on the window's edge.
        ListDialogNav bottom{5, 9};
        list_dialog_key(bottom, kListKeyDown, kRows, 40);
        CHECK(bottom.top_row == 6);
        CHECK(bottom.highlight == 9);  // the BAR stays on the same screen row
        ListDialogNav top{5, 0};
        list_dialog_key(top, kListKeyUp, kRows, 40);
        CHECK(top.top_row == 4);
        CHECK(top.highlight == 0);
    }

    TEST_CASE("Down stops on the last REAL item of a short list") {
        // @0x42E4A5's second compare is against total-1, so a 3-item list
        // cannot park the bar on a blank row 3..9 — and the view cannot move
        // either, so Down at item 2 is a total no-op.
        ListDialogNav nav{0, 2};
        CHECK(list_dialog_key(nav, kListKeyDown, kRows, 3) == ListDialogAction::None);
        CHECK(nav.top_row == 0);
        CHECK(nav.highlight == 2);
    }

    TEST_CASE("walking Down through a long list never wraps back to the start") {
        // 40 items, 10 rows: 39 presses reach the last item and the 40th does
        // nothing. A wrapping handler would be back at item 0 by then.
        ListDialogNav nav{0, 0};
        for (int i = 0; i < 60; ++i) list_dialog_key(nav, kListKeyDown, kRows, 40);
        CHECK(nav.top_row == 30);
        CHECK(nav.highlight == 9);
        CHECK(nav.top_row + nav.highlight == 39);
        for (int i = 0; i < 60; ++i) list_dialog_key(nav, kListKeyUp, kRows, 40);
        CHECK(nav.top_row == 0);
        CHECK(nav.highlight == 0);
    }

    // --- item 3: HOME/END/PAGEUP/PAGEDOWN ARE PURE VIEW SCROLLS -----------

    TEST_CASE("Home and End move ONLY the view, so the selection changes with it") {
        // @0x42E525 / @0x42E539 write edi and nothing else. The highlight bar
        // stays on the same SCREEN ROW, which is what makes the selected item
        // change as a side effect. The port's help browser moved the
        // SELECTION (Home -> item 0, End -> the last item) and let the view
        // follow, which lands the bar on a different screen row entirely.
        ListDialogNav nav{12, 3};
        CHECK(list_dialog_key(nav, kListKeyHome, kRows, 40) == ListDialogAction::None);
        CHECK(nav.top_row == 0);
        CHECK(nav.highlight == 3);              // NOT 0
        CHECK(nav.top_row + nav.highlight == 3);  // selection followed the view

        CHECK(list_dialog_key(nav, kListKeyEnd, kRows, 40) == ListDialogAction::None);
        CHECK(nav.top_row == 30);  // total - visible
        CHECK(nav.highlight == 3);
        CHECK(nav.top_row + nav.highlight == 33);  // NOT the last item, 39
    }

    TEST_CASE("PageUp and PageDown scroll the view by exactly one window, clamped") {
        ListDialogNav nav{0, 7};
        list_dialog_key(nav, kListKeyPageDown, kRows, 40);
        CHECK(nav.top_row == 10);
        CHECK(nav.highlight == 7);
        list_dialog_key(nav, kListKeyPageDown, kRows, 40);
        CHECK(nav.top_row == 20);
        list_dialog_key(nav, kListKeyPageDown, kRows, 40);
        CHECK(nav.top_row == 30);  // clamped to total - visible @0x42E519
        list_dialog_key(nav, kListKeyPageDown, kRows, 40);
        CHECK(nav.top_row == 30);  // and then does nothing at all @0x42E508
        CHECK(nav.highlight == 7);

        list_dialog_key(nav, kListKeyPageUp, kRows, 40);
        CHECK(nav.top_row == 20);
        list_dialog_key(nav, kListKeyPageUp, kRows, 40);
        CHECK(nav.top_row == 10);
        list_dialog_key(nav, kListKeyPageUp, kRows, 40);
        CHECK(nav.top_row == 0);
        list_dialog_key(nav, kListKeyPageUp, kRows, 40);
        CHECK(nav.top_row == 0);  // @0x42E4E2's `jle`
        CHECK(nav.highlight == 7);
    }

    TEST_CASE("a page that would overshoot the top clamps to 0, not below") {
        ListDialogNav nav{4, 2};  // 4 < visible, so top - 10 goes negative
        list_dialog_key(nav, kListKeyPageUp, kRows, 40);
        CHECK(nav.top_row == 0);  // @0x42E4F3's `xor edi, edi`
        CHECK(nav.highlight == 2);
    }

    TEST_CASE("in a list that FITS, all four view keys do nothing whatsoever") {
        // max_top = total - visible <= 0, so End/PageDown fail their `jge`
        // and Home/PageUp fail their `jle`. The selection is untouched — the
        // port's End jumped to the last item here.
        for (const int total : {1, 5, 10}) {
            for (const int key :
                 {kListKeyHome, kListKeyEnd, kListKeyPageUp, kListKeyPageDown}) {
                ListDialogNav nav{0, 2};
                CHECK(list_dialog_key(nav, key, kRows, total) == ListDialogAction::None);
                CHECK(nav.top_row == 0);
                CHECK(nav.highlight == 2);
            }
        }
    }

    TEST_CASE("Enter activates and Escape cancels, and neither moves the list") {
        ListDialogNav nav{7, 3};
        CHECK(list_dialog_key(nav, kListKeyEnter, kRows, 40) == ListDialogAction::Activate);
        CHECK(list_dialog_key(nav, kListKeyEscape, kRows, 40) == ListDialogAction::Cancel);
        CHECK(nav.top_row == 7);
        CHECK(nav.highlight == 3);
    }

    TEST_CASE("the type-ahead jump pulls the match to the TOP of the window") {
        // @0x42E58A-@0x42E5A0: edi = min(match, max_top), ebp = match - edi.
        ListDialogNav nav{0, 0};
        list_dialog_letter_jump(nav, 12, kRows, 40);
        CHECK(nav.top_row == 12);
        CHECK(nav.highlight == 0);
        // ...unless that would scroll past the end, when the window stops and
        // the highlight takes up the slack.
        list_dialog_letter_jump(nav, 37, kRows, 40);
        CHECK(nav.top_row == 30);
        CHECK(nav.highlight == 7);
        // A list that fits is skipped outright @0x42E554.
        ListDialogNav fits{0, 1};
        list_dialog_letter_jump(fits, 4, kRows, 6);
        CHECK(fits.top_row == 0);
        CHECK(fits.highlight == 1);
    }

    // --- item 1: THE MOUSE --------------------------------------------------

    TEST_CASE("the hit test finds one row hotspot per visible row, and stops at the band") {
        const ListDialogGeometry g = scrolled(40, 0);
        REQUIRE(g.win_x == 100);
        REQUIRE(g.item_x == 8);
        REQUIRE(g.item_y0 == 28);

        for (int i = 0; i < kRows; ++i) {
            CAPTURE(i);
            const ListDialogHit h = hit_at(g, row_x(), row_y(g, i));
            CHECK(h.widget == ListDialogWidget::Row);
            CHECK(h.row == i);
        }
        // The band's own edges: item_x .. item_x + item_w, item_y0 .. item_y0 +
        // visible*font_h.
        CHECK(hit_at(g, 100 + g.item_x, 100 + g.item_y0).widget == ListDialogWidget::Row);
        CHECK(hit_at(g, 100 + g.item_x - 1, 100 + g.item_y0).widget == ListDialogWidget::None);
        CHECK(hit_at(g, 100 + g.item_x + g.item_w, 100 + g.item_y0).widget ==
              ListDialogWidget::None);
        CHECK(hit_at(g, row_x(), 100 + g.item_y0 - 1).widget == ListDialogWidget::None);
        CHECK(hit_at(g, row_x(), 100 + g.item_bottom).widget == ListDialogWidget::None);
    }

    TEST_CASE("the hit test finds the scrollbar and Done, and nothing outside the window") {
        const ListDialogGeometry g = scrolled(40, 0);
        CHECK(hit_at(g, 100 + g.sb_button_x + 2, 100 + g.sb_up_y + 2).widget ==
              ListDialogWidget::ScrollUp);
        CHECK(hit_at(g, 100 + g.sb_button_x + 2, 100 + g.sb_down_y + 2).widget ==
              ListDialogWidget::ScrollDown);
        CHECK(hit_at(g, 100 + g.sb_track_x + 2, 100 + g.sb_track_y + 2).widget ==
              ListDialogWidget::Track);
        CHECK(hit_at(g, 100 + g.done_x + 2, 100 + g.done_y + 2).widget == ListDialogWidget::Done);

        CHECK(hit_at(g, 99, 150).widget == ListDialogWidget::None);
        CHECK(hit_at(g, 150, 99).widget == ListDialogWidget::None);
        CHECK(hit_at(g, 100 + g.win_w, 150).widget == ListDialogWidget::None);
        CHECK(hit_at(g, 150, 100 + g.win_h).widget == ListDialogWidget::None);
    }

    TEST_CASE("the highlight FOLLOWS the pointer over the rows — no click needed") {
        // The 0x200+i ids @0x42E212 sit in the widget's MOUSE-ENTER slot
        // (+0x18), not a click slot, so hovering a row moves the bar
        // @0x42E453. The view does not move with it.
        const ListDialogGeometry g = scrolled(40, 12);
        ListDialogNav nav{12, 0};
        list_dialog_mouse_move(nav, hit_at(g, row_x(), row_y(g, 6)), 40);
        CHECK(nav.top_row == 12);
        CHECK(nav.highlight == 6);
        CHECK(nav.top_row + nav.highlight == 18);

        // Off the rows, nothing happens — and the rows pass -1 for the LEAVE
        // slot @0x42E212, so the bar stays where the pointer left it.
        list_dialog_mouse_move(nav, hit_at(g, 100 + g.done_x + 2, 100 + g.done_y + 2), 40);
        CHECK(nav.highlight == 6);
        list_dialog_mouse_move(nav, hit_at(g, 5, 5), 40);
        CHECK(nav.highlight == 6);
    }

    TEST_CASE("hovering a row with no item behind it is ignored") {
        // @0x42E43F's total compare. All ten hotspots exist whatever the list
        // length (@0x42E1ED loops over the VISIBLE count).
        const ListDialogGeometry g = scrolled(3, 0);
        ListDialogNav nav{0, 1};
        CHECK(hit_at(g, row_x(), row_y(g, 7)).widget == ListDialogWidget::Row);
        list_dialog_mouse_move(nav, hit_at(g, row_x(), row_y(g, 7)), 3);
        CHECK(nav.highlight == 1);
    }

    TEST_CASE("a single left press on a row activates it — there is no double-click") {
        // 0x400+i @0x42E387 joins Enter's own path @0x42E395. One press.
        const ListDialogGeometry g = scrolled(40, 12);
        ListDialogNav nav{12, 0};
        const ListDialogHit h = hit_at(g, row_x(), row_y(g, 4));
        CHECK(list_dialog_mouse_down(nav, g, h, kRows, 40, row_y(g, 4)) ==
              ListDialogAction::Activate);
        CHECK(nav.top_row + nav.highlight == 16);
    }

    TEST_CASE("the scrollbar arrows are one Up / one Down keypress each") {
        // Their widget ids @0x42DFE8 / @0x42E00E are LITERALLY 0x148 and
        // 0x150, so they run the same handlers the keys do — including moving
        // the highlight inside the window before scrolling the view.
        const ListDialogGeometry g = scrolled(40, 5);
        ListDialogNav nav{5, 4};
        const ListDialogHit up = hit_at(g, 100 + g.sb_button_x + 2, 100 + g.sb_up_y + 2);
        const ListDialogHit down = hit_at(g, 100 + g.sb_button_x + 2, 100 + g.sb_down_y + 2);
        CHECK(list_dialog_mouse_down(nav, g, down, kRows, 40, 0) == ListDialogAction::None);
        CHECK(nav.top_row == 5);
        CHECK(nav.highlight == 5);
        CHECK(list_dialog_mouse_down(nav, g, up, kRows, 40, 0) == ListDialogAction::None);
        CHECK(nav.highlight == 4);

        ListDialogNav edge{5, 0};
        list_dialog_mouse_down(edge, g, up, kRows, 40, 0);
        CHECK(edge.top_row == 4);
        CHECK(edge.highlight == 0);

        // ...and they clamp at the ends like the keys do.
        ListDialogNav at_top{0, 0};
        list_dialog_mouse_down(at_top, g, up, kRows, 40, 0);
        CHECK(at_top.top_row == 0);
        CHECK(at_top.highlight == 0);
    }

    // @0x42E3D3-@0x42E415: above the thumb -> 0x149 (PageUp), below -> 0x151
    // (PageDown), inside the 15-px thumb band -> the loop, with no handler at
    // all. sub_42DBCC has no drag code anywhere. The two cases below share the
    // 40-item list scrolled to row 15, whose thumb sits at 79 (47 + 15*65/30).
    TEST_CASE("a track click above or below the thumb pages the view") {
        const ListDialogGeometry g = scrolled(40, 15);
        const int tx = track_x(g);
        const int thumb_top = 100 + g.sb_thumb_y0;
        REQUIRE(g.sb_thumb_y0 == 79);

        ListDialogNav above{15, 3};
        CHECK(press_track(above, g, tx, thumb_top - 9) == ListDialogAction::None);
        CHECK(above.top_row == 5);   // one page up
        CHECK(above.highlight == 3);  // the bar did not move

        ListDialogNav below{15, 3};
        press_track(below, g, tx, thumb_top + 21);
        CHECK(below.top_row == 25);
        CHECK(below.highlight == 3);
    }

    TEST_CASE("a click ON THE THUMB does nothing — there is no drag") {
        const ListDialogGeometry g = scrolled(40, 15);
        const int tx = track_x(g);
        const int thumb_top = 100 + g.sb_thumb_y0;

        // The thumb band is [thumb_y, thumb_y + 14] INCLUSIVE.
        for (const int dy : {0, 7, 14}) {
            CAPTURE(dy);
            ListDialogNav on{15, 3};
            CHECK(press_track(on, g, tx, thumb_top + dy) == ListDialogAction::None);
            CHECK(on.top_row == 15);
            CHECK(on.highlight == 3);
        }
        // ...and one pixel past it on either side pages again, which is what
        // makes the three no-ops above a BAND and not a dead scrollbar.
        ListDialogNav just_above{15, 3};
        press_track(just_above, g, tx, thumb_top - 1);
        CHECK(just_above.top_row == 5);
        ListDialogNav just_below{15, 3};
        press_track(just_below, g, tx, thumb_top + 15);
        CHECK(just_below.top_row == 25);
    }

    TEST_CASE("\"Done\" fires on RELEASE, and only when the press was on it too") {
        // Its id 0x1b sits in the RELEASE slot (+0x24) @0x42E040, while every
        // other widget here fills the PRESS slot (+0x20). And the dispatcher
        // only delivers a release to the widget the press captured
        // @0x432F5D, so pressing Done and sliding off cancels the click.
        const ListDialogGeometry g = scrolled(40, 0);
        const ListDialogHit done = hit_at(g, 100 + g.done_x + 2, 100 + g.done_y + 2);
        const ListDialogHit row = hit_at(g, row_x(), row_y(g, 0));
        REQUIRE(done.widget == ListDialogWidget::Done);

        ListDialogNav nav{0, 0};
        CHECK(list_dialog_mouse_down(nav, g, done, kRows, 40, 0) == ListDialogAction::None);
        CHECK(list_dialog_mouse_up(done, ListDialogWidget::Done) == ListDialogAction::Cancel);
        CHECK(list_dialog_mouse_up(row, ListDialogWidget::Done) == ListDialogAction::None);
        CHECK(list_dialog_mouse_up(done, ListDialogWidget::Row) == ListDialogAction::None);
        CHECK(list_dialog_mouse_up(done, ListDialogWidget::None) == ListDialogAction::None);
    }

    TEST_CASE("nothing else responds to a press: the title strip and dead space") {
        const ListDialogGeometry g = scrolled(40, 5);
        ListDialogNav nav{5, 2};
        for (const auto& p : {std::pair<int, int>{100 + g.win_w / 2, 100 + 8},   // title strip
                              std::pair<int, int>{100 + 2, 100 + g.done_y + 2},  // dead space
                              std::pair<int, int>{50, 50}}) {                    // outside
            CHECK(list_dialog_mouse_down(nav, g, hit_at(g, p.first, p.second), kRows, 40,
                                         p.second) == ListDialogAction::None);
            CHECK(nav.top_row == 5);
            CHECK(nav.highlight == 2);
        }
    }

}  // TEST_SUITE
