#include "bomber/game/editor_grid.hpp"

#include <algorithm>

namespace bomber::game {

void EditorGrid::reset(int width, int height) {
    width_ = width;
    height_ = height;
    rows_.assign(static_cast<std::size_t>(height_), std::string(static_cast<std::size_t>(width_),
                                                                  brush_to_cell_char(EditorBrush::Blank)));
    density_ = 90;
    name_.clear();
    for (int i = 0; i < kEditorMaxStarts; ++i) {
        // Spread starts along row 0, one per column, clamped to the board —
        // see the header's TODO(RE) on the unpinned "new scheme" default.
        starts_[static_cast<std::size_t>(i)] = EditorStart{std::min(i, width_ - 1), 0, false};
    }
    powerups_.clear();
    for (int i = 0; i < kEditorPowerupKinds; ++i) {
        assets::sch::PowerupRule pr;
        pr.id = i;
        powerups_.push_back(pr);
    }
}

void EditorGrid::load_from_scheme(const assets::sch::Scheme& scheme) {
    width_ = scheme.width() > 0 ? scheme.width() : kEditorGridWidth;
    height_ = scheme.height() > 0 ? scheme.height() : kEditorGridHeight;
    rows_.assign(static_cast<std::size_t>(height_), std::string(static_cast<std::size_t>(width_),
                                                                  brush_to_cell_char(EditorBrush::Blank)));
    for (int y = 0; y < height_ && y < static_cast<int>(scheme.rows.size()); ++y) {
        const std::string& src = scheme.rows[static_cast<std::size_t>(y)];
        for (int x = 0; x < width_ && x < static_cast<int>(src.size()); ++x)
            rows_[static_cast<std::size_t>(y)][static_cast<std::size_t>(x)] = src[static_cast<std::size_t>(x)];
    }
    density_ = std::clamp(scheme.brick_density, 0, 100);
    name_ = scheme.name;

    for (auto& st : starts_) st = EditorStart{};
    for (const auto& sp : scheme.spawns) {
        if (sp.player < 0 || sp.player >= kEditorMaxStarts) continue;
        starts_[static_cast<std::size_t>(sp.player)] =
            EditorStart{std::clamp(sp.x, 0, width_ - 1), std::clamp(sp.y, 0, height_ - 1),
                        sp.extra != 0};
    }

    powerups_ = scheme.powerups;
}

assets::sch::Scheme EditorGrid::to_scheme() const {
    assets::sch::Scheme s;
    s.version = 1;  // TODO(RE): §5 pins a -V field exists but not the shipped version number
    s.name = name_;
    s.brick_density = density_;
    s.rows = rows_;
    for (int i = 0; i < kEditorMaxStarts; ++i) {
        const EditorStart& st = starts_[static_cast<std::size_t>(i)];
        assets::sch::Spawn sp;
        sp.player = i;
        sp.x = st.x;
        sp.y = st.y;
        sp.extra = st.team ? 1 : 0;
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

void EditorGrid::stamp(int cx, int cy, int size, EditorBrush brush) {
    if (size < 1) size = 1;
    // Centered, leaning top-left for even sizes (header's documented TODO(RE)
    // on the unpinned anchor rule) — offset runs [-(size/2), (size-1)/2], so
    // a 2-wide brush covers {cursor-1, cursor} on each axis (the EXTRA cell
    // lands above/left of the hovered cell, not below/right).
    int lo = -(size / 2);
    int hi = (size - 1) / 2;
    for (int dy = lo; dy <= hi; ++dy)
        for (int dx = lo; dx <= hi; ++dx) paint(cx + dx, cy + dy, brush);
}

void EditorGrid::flood_fill(EditorBrush brush) {
    // §5, CONFIRMED: "flood-fill the WHOLE grid with the brush" — not a
    // connected-region flood fill, so this is a plain full-board stamp.
    for (int y = 0; y < height_; ++y)
        for (int x = 0; x < width_; ++x) paint(x, y, brush);
}

void EditorGrid::set_density(int d) { density_ = std::clamp(d, 0, 100); }

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
