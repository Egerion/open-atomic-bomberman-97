#pragma once

// The scheme-editor's pure grid model — sub_4028D2 @0x4028D2 (docs/re/
// results-and-options.md #5, CONFIRMED): the 15x11 tile grid, the current
// brush, flood fill, and the 10 movable player-start markers with their team
// flags. Deliberately SDL-free (no <SDL3/...> include anywhere in this file
// or editor_grid.cpp) so brush stamping and flood fill are unit-testable
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
enum class EditorBrush {
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

    // Resets to an all-blank grid of the given size with starts spread along
    // row 0 (an arbitrary, documented placeholder layout — §5 does not pin a
    // "new scheme" default board, only that sub_4028D2(1) opens a NEW scheme;
    // TODO(RE): the original's actual blank-scheme defaults are unpinned).
    void reset(int width, int height);

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
    // with the current brush"). Out-of-bounds is a silent no-op — mirrors
    // the original's pixel->cell mappers only ever producing in-range
    // coordinates; the model stays defensive for callers driven by raw mouse
    // pixels.
    void paint(int x, int y, EditorBrush brush);
    // Stamps an `size`x`size` square brush centered at (x,y) — brush sizes
    // 1/2/3 per the task brief; §5's body read did not pin the exact
    // multi-cell anchor (top-left vs. centered), so this is a documented
    // TODO(RE): kept centered (clamped to size//2 so a 2-wide brush leans
    // top-left, matching the common "hover = top-left corner of the stamp"
    // convention) rather than an invented anchor rule.
    void stamp(int cx, int cy, int size, EditorBrush brush);
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
    int density_ = 90;               // BASIC.SCH ships -B,90 (match_factory.hpp's citation)
    std::string name_;
    std::array<EditorStart, kEditorMaxStarts> starts_{};
    std::vector<assets::sch::PowerupRule> powerups_;

    bool in_bounds(int x, int y) const { return x >= 0 && x < width_ && y >= 0 && y < height_; }
};

}  // namespace bomber::game
