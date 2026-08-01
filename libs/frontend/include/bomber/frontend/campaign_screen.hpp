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
#include "bomber/render/asset_store.hpp"
#include "bomber/ui/bmscreen.hpp"
#include "bomber/ui/list_picker.hpp"

namespace bomber::game {

// The `*.cam` file picker — the same widget shape as SchemeFilePicker (glob one
// directory for one extension, list, Up/Down + Enter/Esc). Row text is just the
// filename: sub_4015C6's list shows filenames only, and a .CAM has no embedded
// name until parsed.
class CampaignFilePicker {
public:
    CampaignFilePicker(const AssetStore& assets, const FontTextures& font)
        : assets_(&assets), font_(&font), list_(font) {}

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
    // Only valid when done() && !cancelled(); range-checked all the same.
    const std::filesystem::path& selected() const;
    bool empty() const { return list_.empty(); }

    // CORRECTED 2026-07-26: sub_42DBCC shows TEN rows, not thirteen — 13 is only
    // the window's font-height multiplier (list_dialog_geometry.hpp).
    static constexpr int kVisibleRows = ListPicker::kVisibleRows;

private:
    std::string header() const;
    void draw_backdrop(SDL_Renderer* ren) const;

    const AssetStore* assets_ = nullptr;
    const FontTextures* font_ = nullptr;
    std::string backdrop_;
    // The shared (100,100) glob/list widget — sub_41404B + sub_42DBCC.
    ListPicker list_;
    bool done_ = false;
    bool cancelled_ = false;
};

}  // namespace bomber::game
