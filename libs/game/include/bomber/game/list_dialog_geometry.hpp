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
// register args); its a1/a2 reach sub_43D398 @0x43C8C2, which bounds-checks
// `[win+0x18] (width) + edx` against the right clip edge and `[win+0x1c]
// (height) + ebx` against the bottom -- so a1 = x, a2 = y, decisively. This
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
// counters: `[esp+0xA8] = 10` (the rows it will actually draw) and `ebp = 13`
// (the font-height multiplier for the window it asks sub_43C734 for). Both are
// decremented together when the window allocation fails (`cmp ebp, 8; jg`
// @0x42DCA2 -> heights 13..9, rows 10..6), which cannot happen in the port.
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
    // The thumb is a FIXED 15x15 raised bevel, not a proportional one.
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
inline ListDialogGeometry list_dialog_geometry(int x, int y, int item_text_w, int title_w,
                                               int font_h, int visible_rows = kListDialogRows,
                                               int footer_lines = 0) {
    ListDialogGeometry g;

    // @0x42DC24-0x42DC49: esi = itemw + 16; if textwidth(title) > esi then the
    // title wins and itemw is bumped to keep `itemw + 16 == esi`; window width
    // is esi + 20. So item_w is always win_w - 36.
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
    // Thumb @0x42E1D4: raised, (w-21, 2*font_h+23)..(w-7, 2*font_h+37) -- a
    // hard 15x15, independent of how long the list is.
    g.sb_thumb_x0 = g.sb_track_x;
    g.sb_thumb_y0 = g.sb_track_y;
    g.sb_thumb_x1 = g.win_w - 7;
    g.sb_thumb_y1 = 2 * font_h + 37;

    // "Done" @0x42E072: (w/2 - 32, win_h - font_h - 14). Same x rule as
    // sub_414340's " Ok " button, and its widget id is likewise 27 (Esc).
    g.done_x = g.win_w / 2 - 32;
    g.done_y = g.win_h - font_h - 14;

    g.footer_y0 = g.item_bottom + 8;
    return g;
}

}  // namespace bomber::game
