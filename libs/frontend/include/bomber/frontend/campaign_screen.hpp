#pragma once

// The hidden campaign-mode picker — sub_4015C6 @0x4015C6 (docs/re/
// campaign.md, CONFIRMED). Reached ONLY via the raw 'C'x5 trigger inside
// present_setup's input loop (game_app.cpp, mirroring editor_screen.hpp's
// Ctrl+E x6 pattern for the scheme editor) — there is no menu row for this
// screen, so like the editor it is not part of the AppState/AppInput flow
// graph (app_flow.hpp): GameApp owns a nested event loop around this widget
// and returns to present_setup's own loop when the picker is dismissed.
//
// sub_4015C6 globs "*.cam" (aCam, sub_411D17 + sub_41404B — the SAME
// findfirst/qsort helper the *.SCH picker and the *.BM help browser use) in
// the install ROOT, shows a list dialog (sub_41485A, getstring 1250 header),
// and on selection parses the file (sub_401085) and sets the campaign-active
// flag. This widget only covers the glob + list + selection; parsing is
// bomber::assets::res::load_campaign (libs/assets/campaign.hpp) and the
// campaign-active state machine lives in GameApp (game_app.hpp/.cpp).

#include <SDL3/SDL.h>

#include <filesystem>
#include <string>
#include <vector>

#include "bomber/audio/audio_engine.hpp"
#include "bomber/game_util/list_dialog_geometry.hpp"  // kListDialogRows
#include "bomber/render/asset_store.hpp"
#include "bomber/ui/bmscreen.hpp"

namespace bomber::game {

// The `*.cam` file picker — same widget shape as editor_screen.hpp's
// SchemeFilePicker (glob one directory for one extension, list, Up/Down +
// Enter/Esc). Row text is just the filename (unlike SchemeFilePicker's
// "<filename> <scheme name>" — a .CAM has no single embedded name until
// parsed, and parsing every candidate up front to preview it is not worth
// the RE fidelity here; sub_4015C6's list shows filenames only).
class CampaignFilePicker {
public:
    CampaignFilePicker(const AssetStore& assets, const FontTextures& font)
        : assets_(&assets), font_(&font) {}

    // Globs `*.cam` (case-insensitive extension match) in `install_root` and
    // resets the list cursor.
    void enter(const std::filesystem::path& install_root, std::string backdrop);

    // Up/Down/Home/End/PageUp/PageDown navigate exactly as sub_42DBCC's own
    // handlers do (list_dialog_geometry.hpp's input model); Enter selects
    // (done()==true, cancelled()==false); Esc cancels (done()==true,
    // cancelled()==true).
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

    // CORRECTED 2026-07-26: sub_42DBCC shows TEN rows, not thirteen — 13 is
    // only the window's font-height multiplier (list_dialog_geometry.hpp).
    // The old "still draws bare text rather than routing through
    // draw_list_dialog's chrome" note is retired: it does now, so a scrolled
    // campaign list shows its position on the same scrollbar the *.SCH picker
    // and the help browser draw.
    static constexpr int kVisibleRows = kListDialogRows;

private:
    std::string header() const;
    ListDialogGeometry layout() const;

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
