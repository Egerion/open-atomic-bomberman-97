#pragma once

// The hidden campaign-mode picker — sub_4015C6 @0x4015C6 (docs/re/campaign.md,
// CONFIRMED). It globs "*.cam" with the SAME findfirst/qsort helper the *.SCH
// picker and the *.BM help browser use, and shows the same sub_41485A list
// dialog. This widget covers only the glob + list + selection.

#include <SDL3/SDL.h>

#include <filesystem>
#include <string>
#include <vector>

#include "bomber/audio/audio_engine.hpp"
#include "bomber/game_util/list_dialog_geometry.hpp"  // kListDialogRows
#include "bomber/render/asset_store.hpp"
#include "bomber/ui/bmscreen.hpp"

namespace bomber::game {

// The `*.cam` file picker — the same widget shape as SchemeFilePicker (glob one
// directory for one extension, list, Up/Down + Enter/Esc). Row text is just the
// filename: sub_4015C6's list shows filenames only, and a .CAM has no embedded
// name until parsed.
class CampaignFilePicker {
public:
    CampaignFilePicker(const AssetStore& assets, const FontTextures& font)
        : assets_(&assets), font_(&font) {}

    // Globs `*.cam` (case-insensitive extension match) in `install_root` and
    // resets the list cursor.
    void enter(const std::filesystem::path& install_root, std::string backdrop);

    // Navigation matches sub_42DBCC's own handlers (list_dialog_geometry.hpp);
    // Enter selects, Esc cancels.
    void on_key(SDL_Keycode key, AudioEngine& audio);

    // The widget's MOUSE half, in logical (640x480) coordinates.
    void on_mouse_move(float x, float y, bool buttons_held);
    void on_mouse_down(float x, float y);
    void on_mouse_up(float x, float y);

    void draw(SDL_Renderer* ren) const;

    bool done() const { return done_; }
    bool cancelled() const { return cancelled_; }
    // The selected file's full path — only valid when done() && !cancelled().
    const std::filesystem::path& selected() const {
        const int sel = nav_.top_row + nav_.highlight;  // @0x42E39A
        return entries_[static_cast<std::size_t>(sel)];
    }
    bool empty() const { return entries_.empty(); }

    // CORRECTED 2026-07-26: sub_42DBCC shows TEN rows, not thirteen — 13 is only
    // the window's font-height multiplier (list_dialog_geometry.hpp).
    static constexpr int kVisibleRows = kListDialogRows;

private:
    std::string header() const;
    ListDialogGeometry layout() const;
    void draw_backdrop(SDL_Renderer* ren) const;

    const AssetStore* assets_ = nullptr;
    const FontTextures* font_ = nullptr;
    std::string backdrop_;
    std::vector<std::filesystem::path> entries_;
    // sub_42DBCC's own two registers; their sum is the selection.
    ListDialogNav nav_;
    float item_w_ = 0.0f;  // sub_42FEF0's max over the item text, cached at enter()
    ListDialogWidget pressed_ = ListDialogWidget::None;
    bool done_ = false;
    bool cancelled_ = false;
};

}  // namespace bomber::game
