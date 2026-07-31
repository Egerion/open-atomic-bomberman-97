#pragma once

// The scheme-editor's pure grid model — sub_4028D2 @0x4028D2 (docs/re/
// results-and-options.md §5, CONFIRMED): the 15x11 tile grid, the current brush,
// flood fill, and the 10 movable player-start markers with their team flags.
// libs/editor wraps this with mouse/keyboard input and drawing.
//
// Grid dimensions mirror bomber::sim's kGridWidth/kGridHeight and kMaxPlayers
// exactly — the scheme format's board IS the sim's board, not a separate
// editor-only size.

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "bomber/assets/sch.hpp"
#include "bomber/sim/constants.hpp"

namespace bomber::game {

inline constexpr int kEditorGridWidth = sim::kGridWidth;        // 15
inline constexpr int kEditorGridHeight = sim::kGridHeight;      // 11
inline constexpr int kEditorMaxStarts = sim::kMaxPlayers;       // 10
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

// '0' key — sub_4028D2 case 48 (§5, PINNED from the body, pseudo.c 5654-5657):
// increment dword_45B7B8, then force it to -1 if the result is above 0. NOT a
// plain 0/-1 flip; it only toggles between 0 and -1 because those are the only
// starting values (0 -> 1 -> clamped to -1; -1 -> 0, not >0, stays 0).
//
// A dead-end feature in the original: dword_45B7B8 formats the "tile %d
// blank/solid/brick" sequence name (sub_402206) and no shipped TILES ANI owns a
// "tile -1 *" sequence, so the -1 state always misses the lookup and the canvas
// falls back to flat swatches, same as a missing-asset install.
inline int toggle_editor_tileset(int v) {
    if (++v > 0) v = -1;
    return v;
}

// One of the 10 movable player-start markers; 'T'/'t' toggles the team flag.
struct EditorStart {
    int x = 0;
    int y = 0;
    bool team = false;
};

// The pure editable state: a rectangular tile grid + 10 starts + the -B/-N
// metadata fields + the 13 powerup rows. Nothing here touches SDL, files, or
// randomness.
class EditorGrid {
public:
    EditorGrid() { reset(kEditorGridWidth, kEditorGridHeight); }

    // The original's "new scheme" board — sub_4049C0 @0x4049C0 (§5, PINNED):
    // density 90; even rows all brick (":::::::::::::::"), odd rows
    // brick/solid alternating (":#:#:#:#:#:#:#:"); start slot j's team flag
    // = j & 1; all 13 powerup rows zeroed.
    //
    // Start POSITIONS come from VALUELST 600..619 (x = getvalue(600+2j),
    // y = getvalue(601+2j), wrapped into the board by repeated +=/-=). The
    // caller passes them via `start_xy` because this model is VALUELST-free;
    // nullptr applies the same wrap to a zeroed table.
    void reset(int width, int height,
               const std::array<std::array<int, 2>, kEditorMaxStarts>* start_xy = nullptr);

    // sub_4028D2(0), "edit an existing scheme". Clamps to the fixed 15x11 board
    // if the source scheme is a different size, so the invariant "every row is
    // exactly width() wide" holds whatever the input.
    void load_from_scheme(const assets::sch::Scheme& scheme);
    assets::sch::Scheme to_scheme() const;

    int width() const { return width_; }
    int height() const { return height_; }

    EditorBrush cell(int x, int y) const;

    // ONE cell per click. PINNED: sub_4028D2's paint path is exactly
    // `sub_4048EB(cell_x, cell_y, brush)` and the brush carries only a TYPE, so
    // there is NO multi-cell brush in the original — the earlier "brush sizes
    // 1/2/3, anchor rule TODO(RE)" is resolved by removal. Out of bounds is a
    // silent no-op, matching sub_4048EB's own check.
    void paint(int x, int y, EditorBrush brush);
    // Ctrl+F: fills the WHOLE grid, CONFIRMED — not a connected-region fill.
    void flood_fill(EditorBrush brush);

    // Brick density, clamped 0-100 into dword_4647A0 — the scheme's -B field.
    int density() const { return density_; }
    void set_density(int d);

    // The -N field.
    const std::string& name() const { return name_; }
    void set_name(std::string n) { name_ = std::move(n); }

    const EditorStart& start(int slot) const { return starts_[static_cast<std::size_t>(slot)]; }
    void move_start(int slot, int x, int y);
    void toggle_start_team(int slot);

    // One row per sim::kPowerupKinds, mirroring assets::sch::PowerupRule 1:1 —
    // sub_402595's sub-editor (§5 'P'/'p').
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

    void reset_starts(const std::array<std::array<int, 2>, kEditorMaxStarts>* start_xy);
    void reset_powerups();

    bool in_bounds(int x, int y) const { return x >= 0 && x < width_ && y >= 0 && y < height_; }
};

}  // namespace bomber::game
