#pragma once

// The generic list dialog's PIXEL GEOMETRY — sub_42DBCC @ 0x42DBCC, read out
// of the binary 2026-07-26 (docs/re/results-and-options.md §5c). SDL-free and
// integer-only so the numbers below are doctest-pinnable on their own
// (tests/game/test_list_dialog.cpp), the same way editor_grid.hpp and
// goldman_wheel.hpp keep their pure models out of the SDL layer.
//
// THE CALL CHAIN (this resolves the §5c-vs-editor_screen.cpp citation clash):
//   sub_407582  the *.SCH picker  -- calls -->
//   sub_41485A  @0x407657: a mouse show/hide bracket (sub_431178/sub_431360)
//               and NOTHING else -- calls -->
//   sub_42DB80  @0x41488E: an argument trampoline that appends an 8th
//               argument, the INITIAL SELECTION INDEX, hardcoded 0 -- calls -->
//   sub_42DBCC  @0x42DB94: the widget that actually measures, builds and
//               draws the window. `ret 0x10` = 4 stack args; sub_41485A's own
//               `ret 0xc` = 3.
// So neither prior citation was complete: §5c's "sub_41485A -> sub_42DBCC"
// skipped sub_42DB80, and editor_screen.cpp's "the generic list dialog
// (sub_42DBCC) is invoked at (100, 100)" named the widget but not the routine
// sub_407582 actually calls.
//
// THE WINDOW IS NOT CENTRED. sub_42DBCC opens it with
// `sub_43C734(x, y, w, h, colormode=256, flags=0x14)` @0x42DC81, passing its
// OWN a5/a6 straight through as x/y -- and sub_407582 passes the literal pair
// (100, 100) @0x407641. sub_43C734 really is 6-arg (`ret 8` + the four Watcom
// register args); its a1/a2 reach sub_43D398 @0x43C8C2, which bounds-checks the
// window's +0x18 width plus the first of them against the right clip edge, and
// the +0x1c height plus the second against the bottom -- so a1 = x, a2 = y,
// decisively. This
// CONFIRMS the rescued `worktree-dialog-chrome-todo-re` branch (commit
// 0c0b00d) and retires dialog_chrome.hpp's old "X is never an explicit
// parameter anywhere in this family" note.
//
// NO WINZ 9-PATCH: sub_42DBCC's body contains no sub_41726B/sub_416B43 call
// (checked over the whole function), so this dialog keeps sub_43C734's flat
// colormode-256 base coat -- dword_45C46C -> (88, 84, 80). The grey popup, not
// the blue one.

namespace bomber::game {

// sub_42DBCC's own visible-row count. @0x42DC44 it seeds TWO separate
// counters: a stack slot set to 10 (the rows it will actually draw) and a
// register set to 13 (the font-height multiplier for the window it asks
// sub_43C734 for). Both are decremented together when the window allocation
// fails — the retry @0x42DCA2 loops back only while that multiplier is still
// above 8, i.e. heights 13..9 and rows 10..6 — which cannot happen in the port.
// The invariant across that retry is `height_multiplier == rows + 3`, which is
// what list_dialog_geometry() encodes.
//
// Ten, NOT thirteen: 13 is only the height multiplier. The arithmetic proves
// it -- the item area alone would need 13*fh starting at y = fh+16, i.e.
// 14*fh + 16 > 13*fh + 22 for any fh > 6, so a 13-row list could not fit its
// own window.
inline constexpr int kListDialogRows = 10;

// Every field is in pixels. `win_*` are screen-space; everything else is
// WINDOW-RELATIVE, exactly as sub_42DBCC computes them (it draws into the
// window's own pixel buffer at `bitmap + y*pitch + x`).
struct ListDialogGeometry {
    int win_x = 0, win_y = 0, win_w = 0, win_h = 0;

    // Title strip: a base-coat fill at (5,5) then a SUNKEN bevel over it.
    int strip_fill_w = 0, strip_fill_h = 0;  // fill size, origin is (5,5)
    int strip_x1 = 0, strip_y1 = 0;          // bevel far corner (inclusive)
    int title_x = 0, title_y = 0;            // centred title text

    // Item column. `item_y0` is the first row's top; rows step by `item_h`.
    int item_x = 0, item_y0 = 0, item_w = 0, item_h = 0;
    int item_bottom = 0;  // y just past the last visible row
    // Item-area base-coat fill, and the SUNKEN bevel framing it.
    int item_fill_x = 0, item_fill_y = 0, item_fill_w = 0, item_fill_h = 0;
    int item_frame_x1 = 0, item_frame_y1 = 0;

    // Scrollbar — ALWAYS drawn (no conditional guards it in sub_42DBCC).
    int sb_button_x = 0, sb_up_y = 0, sb_down_y = 0;
    int sb_track_x = 0, sb_track_y = 0, sb_track_w = 0, sb_track_h = 0;
    int sb_frame_x0 = 0, sb_frame_y0 = 0, sb_frame_x1 = 0, sb_frame_y1 = 0;
    // The thumb is a FIXED 15x15 raised bevel — its SIZE never tracks the list
    // length. Its Y does: see list_dialog_thumb_y().
    int sb_thumb_x0 = 0, sb_thumb_y0 = 0, sb_thumb_x1 = 0, sb_thumb_y1 = 0;

    int done_x = 0, done_y = 0;  // the "Done" button's top-left
    int footer_y0 = 0;           // PORT-ONLY reserved block (see below)
};

// The window width alone — @0x42DC24-0x42DC49. Split out so a PORT-ONLY
// caller that wants to centre its window (the ADR-0011 lobby screens, which
// have no RE'd position to copy) can work out the x it must pass.
inline int list_dialog_width(int item_text_w, int title_w) {
    const int span = (title_w > item_text_w + 16) ? title_w : item_text_w + 16;
    return span + 20;
}

// The scrollbar thumb's Y — the ONE number in this geometry that is not a
// constant, and the one the port used to pin to the top of the track.
//
// sub_42DBCC keeps the thumb's Y in a single stack slot and writes it from two
// places. When the dialog is BUILT @0x42E077-0x42E090 it is the top-of-track
// value, item_y0 + font_h + 7 (= 2*font_h + 23), and the thumb's extent is
// seeded to 14 @0x42E097 — the 15x15 block, size-minus-one. Then on every
// SCROLL repaint (the `-4` branch, taken only when the TOP VISIBLE ROW
// changed) @0x42E6B5-0x42E6F7 it is recomputed as that same base plus a
// proportional offset. The offset is built up in this order: the item area's
// inner height (item_bottom - item_y0, i.e. visible_rows * font_h, cached
// @0x42E2E4), less twice the font height, less 16 @0x42E6C3, less the thumb's
// own 14 @0x42E6C6, less one more @0x42E6CD — that is the TRAVEL span —
// multiplied by the top-row index @0x42E6D0 and divided, signed and
// truncating, by (total - visible) @0x42E6DF (cached @0x42E2F5).
//
// The whole block is guarded @0x42E67A-0x42E681 by a total-vs-visible compare
// that skips it when the list fits, which is both why a short list keeps the
// build-time thumb and why that divisor is never zero. Its first act
// @0x42E6B0 is to repaint a 15x15 patch of base coat over the OLD thumb: the
// original moves the thumb and nothing else, never redrawing the track.
//
// Two things an approximation gets wrong, both settled by the above:
//
//  * IT IS DRIVEN BY THE TOP VISIBLE ROW, NOT THE HIGHLIGHT. The register the
//    multiply reads is the first-drawn-item index — the one that indexes the
//    item array @0x42DEEE and @0x42E5E0, and that the arrow, page, home and
//    end handlers clamp to [0, total-visible] @0x42E4E0-0x42E549. The
//    highlight is a separate register, offset within the visible window, and
//    the selected absolute index is their sum @0x42E39A; neither reaches the
//    thumb expression. Consistently, the highlight-only repaint (the `-3`
//    branch) does not touch the thumb at all, so moving the cursor inside the
//    visible window leaves it exactly where it was.
//  * IT MOVES CONTINUOUSLY, NOT IN ROW-SIZED STEPS. The offset is one
//    truncating division, so a scrolled row is worth travel/(total-visible)
//    px — for a long list far less than a row height, and for a list one row
//    too long the entire travel at once.
//
// The travel span is the track height less 16, not less 15: the track is
// visible*font_h - 2*font_h - 15 tall (@0x42E0C0-0x42E0E8) and the thumb is
// 15, so a flush-bottom thumb would subtract 30. The original subtracts 31
// @0x42E6CD and the thumb comes to rest one pixel above the track's last row.
// Mirrored, not corrected.
inline int list_dialog_thumb_y(int font_h, int visible_rows, int total_rows, int top_row) {
    const int base = (font_h + 16) + font_h + 7;  // item_y0 + font_h + 7 @0x42E08B
    // @0x42E681's `jle`: a list that fits keeps the build-time top-of-track
    // thumb forever. This also makes the division below unreachable at zero.
    if (total_rows <= visible_rows) return base;
    // Left UNGUARDED against a negative span, exactly as the original leaves
    // it: sub_42DBCC only ever runs 6..10 rows (@0x42DC44's retry loop) and the
    // FON heights are 12/16, and every port caller that can overflow passes ten
    // rows too, so `visible_rows * font_h` is never small enough to invert this.
    // Clamping it would be inventing arithmetic the binary does not contain.
    const int travel = visible_rows * font_h - 2 * font_h - 16 - 14 - 1;
    const int max_top = total_rows - visible_rows;
    // PORT-ONLY clamp. Every original caller reaches the division with the top
    // row already inside [0, max_top] because sub_42DBCC owns the scroll state
    // and clamps it in its own key handlers; the port hands the offset in from
    // the outside, so this stops a caller with a stale one from drawing the
    // thumb off its track. A no-op for input that respects that invariant.
    const int top = top_row < 0 ? 0 : (top_row > max_top ? max_top : top_row);
    return base + top * travel / max_top;
}

// Builds the layout sub_42DBCC would.
//
//   `x`, `y`            the literal window origin the caller passes through
//                       sub_41485A (the *.SCH picker and the help browser both
//                       pass 100, 100).
//   `item_text_w`       the widest ITEM's pixel width -- sub_42FEF0 @0x42DC16,
//                       a plain max over textwidth(items[i]).
//   `title_w`           textwidth(title).
//   `font_h`            the active font's height.
//   `visible_rows`      kListDialogRows in every RE'd caller.
//   `footer_lines`      PORT-ONLY (ADR-0011): extra text lines reserved
//                       between the item area and the "Done" button for the
//                       online lobby's key hints, which have no home in
//                       sub_42DBCC's own chrome. 0 keeps every RE'd caller's
//                       geometry byte-identical.
//   `total_rows`        the WHOLE list's length -- sub_42DBCC's own a4/[esp+
//                       0xa4]. Only the thumb's Y reads it. The default 0 is
//                       `<= visible_rows`, i.e. the fits-on-screen case, which
//                       is exactly the build-time layout every other field
//                       already describes.
//   `top_row`           the first visible item's index -- sub_42DBCC's `edi`.
inline ListDialogGeometry list_dialog_geometry(int x, int y, int item_text_w, int title_w,
                                               int font_h, int visible_rows = kListDialogRows,
                                               int footer_lines = 0, int total_rows = 0,
                                               int top_row = 0) {
    ListDialogGeometry g;

    // @0x42DC24-0x42DC49: a working width is taken as itemw + 16; if the
    // title's text width exceeds it, the title wins and itemw is bumped so
    // that itemw + 16 still equals that working width; the window width is
    // that working width + 20. So item_w is always win_w - 36.
    g.win_x = x;
    g.win_y = y;
    g.win_w = list_dialog_width(item_text_w, title_w);
    g.item_w = g.win_w - 36;

    // @0x42DC5E: height = font_h * (rows + 3) + 22. The PORT-ONLY footer block
    // extends it; because the "Done" button is bottom-anchored it follows.
    const int footer_h = footer_lines > 0 ? 8 + footer_lines * font_h : 0;
    g.win_h = font_h * (visible_rows + 3) + 22 + footer_h;

    // Title strip: fill @0x42DD9E is (w - 11) x (font_h + 3) at (5,5); the
    // sunken bevel @0x42DE0E runs (5,5)..(w-6, font_h+8). Title text @0x42DDCB
    // at (w/2 - title_w/2, 8) -- the offset is `w*8 + w/2 - tw/2` bytes into a
    // pitch-w buffer, i.e. y = 8.
    g.strip_fill_w = g.win_w - 11;
    g.strip_fill_h = font_h + 3;
    g.strip_x1 = g.win_w - 6;
    g.strip_y1 = font_h + 8;
    g.title_x = g.win_w / 2 - title_w / 2;
    g.title_y = 8;

    // Items @0x42DE4A-0x42DE57: base pointer is `bitmap + (font_h+16)*pitch +
    // 8`, so x = 8, y = font_h + 16; the row pitch is one font height.
    g.item_x = 8;
    g.item_y0 = font_h + 16;
    g.item_h = font_h;
    g.item_bottom = g.item_y0 + visible_rows * font_h;
    // Fill @0x42DEA2: the base pointer minus 3 px and minus 2 rows, sized
    // (item_w + 6) x (rows*font_h + 2). Frame @0x42DFD2: (5, font_h+13)
    // .. (item_w+10, item_bottom), sunken.
    g.item_fill_x = g.item_x - 3;
    g.item_fill_y = g.item_y0 - 2;
    g.item_fill_w = g.item_w + 6;
    g.item_fill_h = visible_rows * font_h + 2;
    g.item_frame_x1 = g.item_w + 10;
    g.item_frame_y1 = g.item_bottom;

    // Scrollbar. Arrow buttons @0x42E000 / @0x42E034 share x = w - 25; the up
    // one sits at the item frame's top (font_h + 13), the down one at
    // item_bottom - font_h - 5. Track fill @0x42E0F8 is 15 wide at
    // (w - 21, 2*font_h + 23); its sunken frame @0x42E189 runs
    // (w-22, 2*font_h+22)..(w-6, item_bottom - font_h - 9).
    g.sb_button_x = g.win_w - 25;
    g.sb_up_y = font_h + 13;
    g.sb_down_y = g.item_bottom - font_h - 5;
    g.sb_track_x = g.win_w - 21;
    g.sb_track_y = 2 * font_h + 23;
    g.sb_track_w = 15;
    g.sb_track_h = g.item_bottom - 3 * font_h - 31;
    g.sb_frame_x0 = g.win_w - 22;
    g.sb_frame_y0 = 2 * font_h + 22;
    g.sb_frame_x1 = g.win_w - 6;
    g.sb_frame_y1 = g.item_bottom - font_h - 9;
    // Thumb @0x42E1D4 / @0x42E6F7: raised, drawn as the inclusive corner quad
    // (track_x, thumb_y)..(track_x + 14, thumb_y + 14) -- a hard 15x15 whose
    // SIZE is independent of how long the list is, but whose Y slides with the
    // top visible row (list_dialog_thumb_y). Note the TRACK does not follow it:
    // sb_track_y/sb_track_h are computed once at build time from the thumb's
    // INITIAL y (@0x42E0B2-0x42E0E8) and the original never repaints them --
    // the scroll path erases and redraws only the 15x15 thumb itself.
    g.sb_thumb_x0 = g.sb_track_x;
    g.sb_thumb_y0 = list_dialog_thumb_y(font_h, visible_rows, total_rows, top_row);
    g.sb_thumb_x1 = g.win_w - 7;
    g.sb_thumb_y1 = g.sb_thumb_y0 + 14;

    // "Done" @0x42E072: (w/2 - 32, win_h - font_h - 14). Same x rule as
    // sub_414340's " Ok " button, and its widget id is likewise 27 (Esc).
    g.done_x = g.win_w / 2 - 32;
    g.done_y = g.win_h - font_h - 14;

    g.footer_y0 = g.item_bottom + 8;
    return g;
}

}  // namespace bomber::game
