#pragma once

// The scheme-editor's pure grid model — sub_4028D2 @0x4028D2 (docs/re/
// results-and-options.md #5, CONFIRMED): the 15x11 tile grid, the current
// brush, flood fill, and the 10 movable player-start markers with their team
// flags. Deliberately SDL-free (no <SDL3/...> include anywhere in this file
// or editor_grid.cpp) so painting and flood fill are unit-testable
// without a renderer, per the task's "pure logic in an SDL-free helper with
// doctests" requirement. editor_screen.{hpp,cpp} wraps this with mouse/
// keyboard input and drawing.
//
// Grid dimensions mirror bomber::sim's kGridWidth/kGridHeight (15x11) and
// kMaxPlayers (10) exactly — the scheme format's board is the sim's board,
// not a separate editor-only size.

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "bomber/assets/sch.hpp"
#include "bomber/sim/constants.hpp"

namespace bomber::game {

inline constexpr int kEditorGridWidth = sim::kGridWidth;    // 15
inline constexpr int kEditorGridHeight = sim::kGridHeight;  // 11
inline constexpr int kEditorMaxStarts = sim::kMaxPlayers;   // 10
inline constexpr int kEditorPowerupKinds = sim::kPowerupKinds;  // 13

// The three paintable cell kinds (§5: "the status line uses the `tile %d
// blank/solid/brick` strings"), in the SAME '1'/'2'/'3' key order §5 pins:
// '1' blank, '2' solid, '3' brick.
enum class EditorBrush : std::uint8_t {
    Blank = 0,
    Solid = 1,
    Brick = 2,
};

inline char brush_to_cell_char(EditorBrush b) {
    switch (b) {
        case EditorBrush::Solid: return static_cast<char>(assets::sch::Cell::Solid);
        case EditorBrush::Brick: return static_cast<char>(assets::sch::Cell::Brick);
        default: return static_cast<char>(assets::sch::Cell::Blank);
    }
}

inline EditorBrush cell_char_to_brush(char c) {
    if (c == static_cast<char>(assets::sch::Cell::Solid)) return EditorBrush::Solid;
    if (c == static_cast<char>(assets::sch::Cell::Brick)) return EditorBrush::Brick;
    return EditorBrush::Blank;
}

// '0' key — sub_4028D2 case 48 (docs/re/results-and-options.md §5, PINNED
// from the body, pseudo.c 5654-5657): `if (++dword_45B7B8 > 0)
// dword_45B7B8 = -1;`. NOT a plain 0/-1 flip — it is this exact
// increment-then-clamp sequence, which happens to toggle strictly between 0
// and -1 for any starting value in {0, -1} (0 -> 1 -> clamped to -1; -1 -> 0,
// not >0, stays 0). A dead-end feature in the original: `dword_45B7B8`
// formats the "tile %d blank/solid/brick" sequence name (sub_402206), and no
// shipped TILES ANI owns a "tile -1 *" sequence, so the -1 state always
// misses the sequence lookup — the port's canvas falls back to its flat-
// swatch rendering in that state, same as a missing-asset install. A free
// function (not an EditorGrid method) since it's pure int arithmetic with no
// grid state involved — independently testable without SDL.
inline int toggle_editor_tileset(int v) {
    if (++v > 0) v = -1;
    return v;
}

// One of the 10 movable player-start markers (§5: "the 10 player-start
// markers... plus a teamring%u ANI sprite showing each start's team flag").
struct EditorStart {
    int x = 0;
    int y = 0;
    bool team = false;  // 'T'/'t' toggles this (§5)
};

// The pure editable state: a rectangular tile grid + 10 starts + the -B/-N
// metadata fields + the 13 powerup rows. Mouse/keyboard glue
// (editor_screen.cpp) drives this through the methods below; nothing here
// touches SDL, files, or randomness.
class EditorGrid {
public:
    EditorGrid() { reset(kEditorGridWidth, kEditorGridHeight); }

    // Resets to the original's "new scheme" board — sub_4049C0 @0x4049C0
    // (docs/re/results-and-options.md §5, PINNED): density 90; even rows all
    // brick (":::::::::::::::"), odd rows brick/solid alternating
    // (":#:#:#:#:#:#:#:") — i.e. the classic pillar field, fully bricked;
    // start slot j's team flag = j & 1 (alternating); all 13 powerup rows
    // zeroed. Start POSITIONS come from VALUELST ids 600..619 (x =
    // getvalue(600+2j), y = getvalue(601+2j), wrapped into the board with
    // repeated += / -= width/height) — the caller passes them via
    // `start_xy` since this model is VALUELST-free; nullptr keeps the same
    // wrap rule applied to a zeroed table (all starts at (0,0)).
    void reset(int width, int height,
               const std::array<std::array<int, 2>, kEditorMaxStarts>* start_xy = nullptr);

    // Loads from a parsed Scheme (sub_4028D2(0) — "edit an existing scheme",
    // §5), clamping to the editor's fixed 15x11 board if the source scheme
    // is a different size (shouldn't happen for our own schemes, but keeps
    // the model's invariant — every row exactly width() wide — regardless of
    // input).
    void load_from_scheme(const assets::sch::Scheme& scheme);
    // Exports the current model back into a Scheme (paired with load_from_
    // scheme so save = to_scheme() + assets::sch::write()).
    assets::sch::Scheme to_scheme() const;

    int width() const { return width_; }
    int height() const { return height_; }

    EditorBrush cell(int x, int y) const;
    // Paints ONE cell with `brush` (§5 left mouse: "paint the hovered cell
    // with the current brush"). PINNED: sub_4028D2's paint path is exactly
    // `sub_4048EB(cell_x, cell_y, brush)` — one cell per click, cell chars
    // '#' (35) solid / ':' (58) brick / '.' (46) blank; there is NO
    // multi-cell brush in the original (the brush has only a TYPE), so the
    // earlier "brush sizes 1/2/3, anchor rule TODO(RE)" is resolved by
    // removal. Out-of-bounds is a silent no-op (sub_4048EB's own bounds
    // check).
    void paint(int x, int y, EditorBrush brush);
    // Ctrl+F (§5): flood-fills the WHOLE grid with `brush` — confirmed as a
    // whole-grid fill, not a connected-region fill (§5: "flood-fill the
    // whole grid with the brush").
    void flood_fill(EditorBrush brush);

    // Brick density, 0-100 (§5 'D'/'d': "brick-density prompt (text entry,
    // clamped 0-100 into dword_4647A0 — the scheme -B field)").
    int density() const { return density_; }
    void set_density(int d);

    // Scheme name (§5 'N'/'n': the -N field).
    const std::string& name() const { return name_; }
    void set_name(std::string n) { name_ = std::move(n); }

    // The 10 player starts (§5: '+'/'='/'-'/'_' cycle the selected slot,
    // 'T'/'t' toggles its team flag, right mouse moves it).
    const EditorStart& start(int slot) const { return starts_[static_cast<std::size_t>(slot)]; }
    void move_start(int slot, int x, int y);
    void toggle_start_team(int slot);

    // Powerup rules, one row per sim::kPowerupKinds (13) kind, mirroring
    // assets::sch::PowerupRule 1:1 — sub_402595's sub-editor (§5 'P'/'p').
    const std::vector<assets::sch::PowerupRule>& powerups() const { return powerups_; }
    std::vector<assets::sch::PowerupRule>& powerups() { return powerups_; }

private:
    int width_ = kEditorGridWidth;
    int height_ = kEditorGridHeight;
    std::vector<std::string> rows_;  // rows_[y][x], same char alphabet as sch::Cell
    int density_ = 90;               // sub_4049C0's new-scheme default (BASIC.SCH also ships -B,90)
    std::string name_;
    std::array<EditorStart, kEditorMaxStarts> starts_{};
    std::vector<assets::sch::PowerupRule> powerups_;

    bool in_bounds(int x, int y) const { return x >= 0 && x < width_ && y >= 0 && y < height_; }
};

}  // namespace bomber::game
