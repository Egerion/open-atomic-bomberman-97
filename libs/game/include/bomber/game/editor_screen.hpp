#pragma once

// The hidden scheme editor's SDL presentation — sub_403184 (chooser menu),
// sub_4028D2 (the editor proper), and sub_402595 (powerup-rules sub-editor),
// docs/re/results-and-options.md #5, CONFIRMED. Reached ONLY via the raw
// Ctrl+E x6 trigger inside the main menu's input loop (game_app.cpp's
// present_menu, mirroring sub_42B9CE's `++counter > 5` on key code 5) — there
// is no menu row for this screen in the original, so unlike OptionsScreen/
// KeyRemapScreen this is not part of the AppState/AppInput flow graph
// (app_flow.hpp): GameApp::present_editor() owns its own nested event loop
// exactly like present_keyremap_screen() does, and returns to the menu loop
// when the chooser is exited.
//
// Screen-widget details §5 does not pin exactly (e.g. the file-picker's list
// layout, the name/density prompt's text box chrome) are kept minimal and
// marked TODO(RE) in the .cpp — no invented facts, per the task brief.

#include <SDL3/SDL.h>

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "bomber/game/asset_store.hpp"
#include "bomber/game/audio_engine.hpp"
#include "bomber/game/bmscreen.hpp"
#include "bomber/game/editor_grid.hpp"

namespace bomber::game {

// The *.SCH file picker (sub_407582, §5: "a findfirst glob like the help
// browser's") shown before sub_4028D2(0), "edit an existing scheme". §5's
// summary confirms the glob call exists but does not walk sub_407582's own
// body, so the list widget itself (scrolling, exact layout) is our own
// minimal reproduction of the generic list-dialog convention the rest of the
// front end uses (sub_41485A, §4) — TODO(RE) if sub_407582 is later walked
// in full.
class SchemeFilePicker {
public:
    SchemeFilePicker(const AssetStore& assets, const FontTextures& font)
        : assets_(&assets), font_(&font) {}

    // Globs `*.SCH` in `schemes_dir` (case-insensitive extension match, like
    // the real DOS findfirst/findnext would see on a FAT/NTFS volume).
    void enter(const std::filesystem::path& schemes_dir, std::string backdrop);

    // Up/Down move the cursor; Enter selects (done()==true, cancelled()==
    // false); Esc cancels (done()==true, cancelled()==true).
    void on_key(SDL_Keycode key, AudioEngine& audio);
    void draw(SDL_Renderer* ren) const;

    bool done() const { return done_; }
    bool cancelled() const { return cancelled_; }
    // The selected file's full path — only valid when done() && !cancelled().
    const std::filesystem::path& selected() const { return entries_[static_cast<std::size_t>(row_)]; }
    bool empty() const { return entries_.empty(); }

private:
    const AssetStore* assets_ = nullptr;
    const FontTextures* font_ = nullptr;
    std::string backdrop_;
    std::vector<std::filesystem::path> entries_;
    int row_ = 0;
    bool done_ = false;
    bool cancelled_ = false;
};

// sub_403184's 3-item chooser (§5): '1' edit an existing scheme (after a
// file picker), '2' new scheme, Esc/'Q' exit, F1 help.
enum class EditorChooserResult {
    None,       // still open
    EditExisting,
    New,
    Exit,
    Help,
};

class EditorChooserScreen {
public:
    EditorChooserScreen(const AssetStore& assets, const FontTextures& font)
        : assets_(&assets), font_(&font) {}

    void enter(std::string backdrop);
    // Feed one SDL keycode; returns the resolved action for this frame (None
    // most frames).
    EditorChooserResult on_key(SDL_Keycode key, AudioEngine& audio);
    void draw(SDL_Renderer* ren) const;

private:
    const AssetStore* assets_ = nullptr;
    const FontTextures* font_ = nullptr;
    std::string backdrop_;
};

// The powerup-rules sub-editor — sub_402595 (§5 'P'/'p'): 13 rows (one per
// bomber::sim::kPowerupKinds), each with born-with / has-override+value /
// forbidden fields, mirroring assets::sch::PowerupRule 1:1 (our scheme
// parser already models these exactly, per the task brief).
class PowerupRulesScreen {
public:
    PowerupRulesScreen(const AssetStore& assets, const FontTextures& font)
        : assets_(&assets), font_(&font) {}

    // Edits `rows` IN PLACE (a reference into the parent EditorGrid's own
    // powerups() vector) so Esc/Enter both keep whatever was toggled — §5
    // does not document a separate cancel path for this sub-editor.
    void enter(std::vector<assets::sch::PowerupRule>* rows);
    void on_key(SDL_Keycode key, AudioEngine& audio);
    void draw(SDL_Renderer* ren) const;

    bool done() const { return done_; }

private:
    const AssetStore* assets_ = nullptr;
    const FontTextures* font_ = nullptr;
    std::vector<assets::sch::PowerupRule>* rows_ = nullptr;
    int row_ = 0;
    bool done_ = false;
};

// The editor proper — sub_4028D2 (§5): mouse tile painting on the grid,
// brush sizes 1/2/3, Ctrl+F flood fill, the 10 movable start markers (with
// team flags), density/name prompts, and (via 'P') the powerup sub-editor.
class EditorScreen {
public:
    EditorScreen(const AssetStore& assets, const FontTextures& font)
        : assets_(&assets), font_(&font), powerups_screen_(assets, font) {}

    // `initial` is nullopt for sub_4028D2(1) ("new scheme" — an all-blank
    // board, §5); a loaded Scheme for sub_4028D2(0) ("edit an existing
    // scheme").
    void enter(std::optional<assets::sch::Scheme> initial, std::string backdrop);

    // True while the modal "press key" / text-entry prompts (density/name)
    // are capturing input, so the caller routes key/text events here instead
    // of the normal grid shortcuts (mirrors KeyRemapScreen::capturing()'s
    // "modal capture" shape, §2's sub_407AD9 precedent, applied to §5's own
    // 'D'/'N' text prompts).
    bool prompting() const { return prompt_kind_ != PromptKind::None; }
    // True while the powerup sub-editor (sub_402595) is open on top — the
    // caller routes key events to powerups() instead while this is set.
    bool editing_powerups() const { return editing_powerups_; }
    PowerupRulesScreen& powerups_screen() { return powerups_screen_; }

    void on_key(SDL_Keycode key, AudioEngine& audio);
    void on_text_input(const char* text);
    // Mouse: `gx`/`gy` are already-converted grid cell coordinates (the
    // caller does the pixel->cell mapping via the drawn grid's origin/cell
    // size, mirroring sub_42665C/sub_4266A3, §5).
    void on_mouse_down(int button, int gx, int gy);

    void draw(SDL_Renderer* ren) const;

    // True once Esc/'Q' was accepted past the save-changes confirm (§5).
    bool done() const { return done_; }
    // True if the save-changes confirm resolved to "save" (getstring(735),
    // §5) rather than "discard".
    bool save_requested() const { return save_requested_; }
    const EditorGrid& grid() const { return grid_; }

    // Cell geometry (grid-space origin + cell size in screen pixels) so the
    // caller can convert a raw mouse pixel into (gx, gy) before calling
    // on_mouse_down. §5 does not pin exact pixel coordinates for the editor
    // canvas (TODO(RE)) — chosen here to center the 15x11 board in the
    // 640x480 logical screen with a status strip below.
    static constexpr int kCellSize = 32;
    static constexpr int kOriginX = (640 - kEditorGridWidth * kCellSize) / 2;
    static constexpr int kOriginY = 24;

private:
    enum class PromptKind { None, Density, Name, SaveConfirm };

    const AssetStore* assets_ = nullptr;
    const FontTextures* font_ = nullptr;

    std::string backdrop_;
    EditorGrid grid_;
    EditorBrush brush_ = EditorBrush::Blank;
    int brush_size_ = 1;         // 1/2/3, §5
    int selected_start_ = 0;     // '+'/'='/'-'/'_' cycles this, §5

    PromptKind prompt_kind_ = PromptKind::None;
    std::string prompt_text_;    // in-progress text for the Name prompt

    bool editing_powerups_ = false;
    PowerupRulesScreen powerups_screen_;

    bool done_ = false;
    bool save_requested_ = false;

    void cycle_brush();
    void start_density_prompt();
    void start_name_prompt();
    void start_save_confirm();
};

}  // namespace bomber::game
