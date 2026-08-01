#pragma once

#include <SDL3/SDL.h>

#include <filesystem>
#include <functional>
#include <string>
#include <vector>

#include "bomber/game_util/list_dialog_geometry.hpp"

// The ONE file-list modal behind sub_41404B (the findfirst/findnext glob helper)
// + sub_41485A -> sub_42DB80 -> sub_42DBCC (the generic bevel list dialog at the
// literal (100, 100)). The original has ONE of these; the port had grown three
// ~70-line near-clones — HelpBrowser (*.BM), SchemeFilePicker (*.SCH),
// CampaignFilePicker (*.cam) — and two of the three had drifted off the pinned
// sort below. This is the shared SDL adapter; the SDL-free DECISIONS stay in
// list_dialog_geometry.hpp where the headless suite pins them, and each owner
// keeps its own key handling, row text and empty-glob error box.

namespace bomber::game {

class FontTextures;

// sub_412A3B — the strupr the glob helper runs over every matched filename
// before sorting (and that sub_407582 runs again over the picked value).
std::string upper_ascii(std::string s);

class ListPicker {
public:
    explicit ListPicker(const FontTextures& font) : font_(&font) {}

    // The pickers' shared enter() prologue: drop the entries and the widget's
    // two nav registers.
    void reset();

    // sub_41404B: a DOS findfirst/findnext glob of "*.<ext>" over `dir` —
    // case-insensitive on the extension, as the original glob is on the FAT
    // install media (`upper_ext` is e.g. ".BM"). ORDERING, PINNED: sub_41404B
    // uppercases EVERY globbed name first (@0x414146, a sub_412A3B/strupr pass
    // over the whole array) and only then qsorts it (@0x41415D) with the
    // comparator at 0x41400F, a plain sub_451F10/strcmp. So the sort key is the
    // UPPERCASED BARE FILENAME — anything an owner appends to a row (the *.SCH
    // picker's ": <scheme name>") never participates.
    void glob(const std::filesystem::path& dir, const std::string& upper_ext);

    // The (100, 100) list's title strip, folded into the width by the WIDGET
    // (win_w = max(item_text_w + 16, measure(title)) + 20), so it must not be
    // pre-maxed into the rows. Stored once so draw and the hit tests cannot see
    // different titles.
    void set_header(std::string header) { header_ = std::move(header); }

    // sub_42FEF0 @0x42DC16 — the widest ITEM row alone drives the width,
    // measured ONCE here so the mouse handlers can rebuild the same layout
    // cheaply on every motion event.
    void measure_rows(const std::function<std::string(int)>& row_text);

    int count() const { return static_cast<int>(entries_.size()); }
    bool empty() const { return entries_.empty(); }
    const std::filesystem::path& path(int i) const {
        return entries_[static_cast<std::size_t>(i)];
    }

    // The selection sum @0x42E39A under the @0x42E3A8 range check; nullptr for
    // an empty list or a stale highlight, so no caller can index out of range.
    const std::filesystem::path* selected() const;

    // sub_42DBCC's own two registers — `top_row + highlight` is the selection.
    // Exposed because every owner's KEY handling differs (letter jump, W/S
    // aliases) and drives them through list_dialog_geometry.hpp's decisions.
    ListDialogNav& nav() { return nav_; }
    const ListDialogNav& nav() const { return nav_; }

    ListDialogGeometry layout() const;

    // The widget's MOUSE half, in logical (640x480) coordinates — the trio
    // dispatch_list_mouse expects. No-ops (None) while the list is not
    // hit-testable (empty glob, or no loaded font). The original only re-homes
    // the highlight while no button is down (@0x4330A0).
    void on_mouse_move(float x, float y, bool buttons_held);
    ListDialogAction on_mouse_down(float x, float y);
    ListDialogAction on_mouse_up(float x, float y);

    // sub_42DBCC's chrome at the pinned literal (100, 100), the sub_442C28
    // selection band at the highlight OFFSET (@0x42E656, not an absolute
    // index), and the visible rows in byte_49D38F white — the SAME ink at all
    // three original call sites (results-and-options.md §4/§5).
    void draw(SDL_Renderer* ren, const std::function<std::string(int)>& row_text) const;

    // CLARIFIED 2026-07-26 (list_dialog_geometry.hpp): sub_42DBCC keeps TWO
    // counters @0x42DC44 — 10 rows drawn, and a separate 13 as the window's
    // font-height multiplier. 10 is the row count outright.
    static constexpr int kVisibleRows = kListDialogRows;

private:
    bool hit_testable() const;

    const FontTextures* font_ = nullptr;
    std::string header_;
    std::vector<std::filesystem::path> entries_;
    ListDialogNav nav_;
    float item_w_ = 0.0f;
    ListDialogWidget pressed_ = ListDialogWidget::None;
};

}  // namespace bomber::game
