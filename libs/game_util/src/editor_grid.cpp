#include "bomber/game_util/editor_grid.hpp"

#include <algorithm>

namespace bomber::game {

void EditorGrid::reset(int width, int height,
                       const std::array<std::array<int, 2>, kEditorMaxStarts>* start_xy) {
    // Guard against a non-positive size: every caller today passes the fixed
    // 15x11 board, but this is a public, independently-testable entry point
    // (class doc: "unit-testable without a renderer"), and the start_xy wrap
    // loops below (`while (x < 0) x += width_` etc.) spin forever on
    // width_/height_ <= 0. Same fallback idiom as load_from_scheme's own
    // `scheme.width() > 0 ? ... : kEditorGridWidth` clamp.
    width_ = width > 0 ? width : kEditorGridWidth;
    height_ = height > 0 ? height : kEditorGridHeight;
    // sub_4049C0 (PINNED): even rows are memcpy'd from ":::::::::::::::"
    // (all brick), odd rows from ":#:#:#:#:#:#:#:" (brick/solid alternating)
    // — the classic pillar field, fully bricked. Generalised per-cell for a
    // non-15-wide board (ours is always 15): solid iff both x and y are odd.
    rows_.assign(static_cast<std::size_t>(height_),
                 std::string(static_cast<std::size_t>(width_), ' '));
    for (int y = 0; y < height_; ++y)
        for (int x = 0; x < width_; ++x)
            rows_[static_cast<std::size_t>(y)][static_cast<std::size_t>(x)] =
                ((y & 1) != 0 && (x & 1) != 0) ? brush_to_cell_char(EditorBrush::Solid)
                                               : brush_to_cell_char(EditorBrush::Brick);
    density_ = 90;  // sub_4049C0: dword_4647A0 = 90
    name_.clear();
    for (int j = 0; j < kEditorMaxStarts; ++j) {
        // Start positions: VALUELST getvalue(600+2j)/getvalue(601+2j) via the
        // caller (start_xy), wrapped into the board with the original's
        // repeated +=/-= loops; team flag = j & 1 (sub_4049C0 writes the slot
        // index's low bit into the team field at dword_46481C, stride 12,
        // offset +8).
        int x = start_xy ? (*start_xy)[static_cast<std::size_t>(j)][0] : 0;
        int y = start_xy ? (*start_xy)[static_cast<std::size_t>(j)][1] : 0;
        while (x < 0) x += width_;
        while (x >= width_) x -= width_;
        while (y < 0) y += height_;
        while (y >= height_) y -= height_;
        starts_[static_cast<std::size_t>(j)] = EditorStart{x, y, (j & 1) != 0};
    }
    powerups_.clear();
    for (int i = 0; i < kEditorPowerupKinds; ++i) {
        assets::sch::PowerupRule pr;
        pr.id = i;  // all four rule fields zeroed — sub_4049C0's final loop
        powerups_.push_back(pr);
    }
}

void EditorGrid::load_from_scheme(const assets::sch::Scheme& scheme) {
    width_ = scheme.width() > 0 ? scheme.width() : kEditorGridWidth;
    height_ = scheme.height() > 0 ? scheme.height() : kEditorGridHeight;
    rows_.assign(
        static_cast<std::size_t>(height_),
        std::string(static_cast<std::size_t>(width_), brush_to_cell_char(EditorBrush::Blank)));
    for (int y = 0; y < height_ && y < static_cast<int>(scheme.rows.size()); ++y) {
        const std::string& src = scheme.rows[static_cast<std::size_t>(y)];
        for (int x = 0; x < width_ && x < static_cast<int>(src.size()); ++x)
            rows_[static_cast<std::size_t>(y)][static_cast<std::size_t>(x)] =
                src[static_cast<std::size_t>(x)];
    }
    density_ = std::clamp(scheme.brick_density, 0, 100);
    name_ = scheme.name;

    for (auto& st : starts_) st = EditorStart{};
    for (const auto& sp : scheme.spawns) {
        if (sp.player < 0 || sp.player >= kEditorMaxStarts) continue;
        starts_[static_cast<std::size_t>(sp.player)] = EditorStart{
            std::clamp(sp.x, 0, width_ - 1), std::clamp(sp.y, 0, height_ - 1), sp.team != 0};
    }

    powerups_ = scheme.powerups;
}

assets::sch::Scheme EditorGrid::to_scheme() const {
    assets::sch::Scheme s;
    s.version = 2;  // every shipped scheme is "-V,2" (install DATA/SCHEMES, checked 2026-07-08)
    s.name = name_;
    s.brick_density = density_;
    s.rows = rows_;
    for (int i = 0; i < kEditorMaxStarts; ++i) {
        const EditorStart& st = starts_[static_cast<std::size_t>(i)];
        assets::sch::Spawn sp;
        sp.player = i;
        sp.x = st.x;
        sp.y = st.y;
        sp.team = st.team ? 1 : 0;
        s.spawns.push_back(sp);
    }
    s.powerups = powerups_;
    return s;
}

EditorBrush EditorGrid::cell(int x, int y) const {
    if (!in_bounds(x, y)) return EditorBrush::Blank;
    return cell_char_to_brush(rows_[static_cast<std::size_t>(y)][static_cast<std::size_t>(x)]);
}

void EditorGrid::paint(int x, int y, EditorBrush brush) {
    if (!in_bounds(x, y)) return;
    rows_[static_cast<std::size_t>(y)][static_cast<std::size_t>(x)] = brush_to_cell_char(brush);
}

void EditorGrid::flood_fill(EditorBrush brush) {
    // §5, CONFIRMED: "flood-fill the WHOLE grid with the brush" — not a
    // connected-region flood fill, so this is a plain full-board stamp.
    for (int y = 0; y < height_; ++y)
        for (int x = 0; x < width_; ++x) paint(x, y, brush);
}

void EditorGrid::set_density(int d) {
    density_ = std::clamp(d, 0, 100);
}

void EditorGrid::move_start(int slot, int x, int y) {
    if (slot < 0 || slot >= kEditorMaxStarts) return;
    EditorStart& st = starts_[static_cast<std::size_t>(slot)];
    st.x = std::clamp(x, 0, width_ - 1);
    st.y = std::clamp(y, 0, height_ - 1);
}

void EditorGrid::toggle_start_team(int slot) {
    if (slot < 0 || slot >= kEditorMaxStarts) return;
    starts_[static_cast<std::size_t>(slot)].team = !starts_[static_cast<std::size_t>(slot)].team;
}

}  // namespace bomber::game
