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
// The screen-widget details are now pinned from full body reads (docs/re/
// results-and-options.md §5): the file picker is the generic list dialog
// (sub_41485A -> sub_42DBCC) at (100,100) with header getstring(721) and up
// to 13 visible rows; the powerup rows edit through sub_4023A2's chain of
// four modal prompts; the canvas draws the match field's own "tile %d
// blank/solid/brick" ANI sequences and MISC.ANI's "teamring%u" markers at
// the match field origin/cell size (sub_426524/sub_42655F). Only the text
// box chrome (sub_42E938's Done/Cancel dialog frame) stays a minimal
// reproduction.

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

// The *.SCH file picker — sub_407582 @0x407582 (§5, PINNED from the body):
// globs "*.SCH" (sub_41404B, the same findfirst/qsort helper as the help
// browser), reads each file's embedded -N scheme name (sub_404BE9) and lists
// "<filename> <scheme name>" rows through the generic list dialog
// (sub_41485A -> sub_42DBCC) at (100, 100) with header getstring(721), the
// general white ink, and up to 13 visible rows (the dialog shrinks to 12..9
// rows if the window can't fit; more entries scroll). Selecting a row strips
// the name suffix at the first space and stores the filename as the live
// schemefilename (byte_4648C4); an empty glob shows the getstring(720)/95
// error dialog instead.
class SchemeFilePicker {
public:
    SchemeFilePicker(const AssetStore& assets, const FontTextures& font)
        : assets_(&assets), font_(&font) {}

    // Globs `*.SCH` in `schemes_dir` (case-insensitive extension match, like
    // the real DOS findfirst/findnext would see on a FAT/NTFS volume) and
    // pre-reads each scheme's -N name for the two-column row text.
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

    // sub_42DBCC sizes the dialog for 13 rows first (falling back 12..9 only
    // when the window allocation fails, which ours never does).
    static constexpr int kVisibleRows = 13;

private:
    const AssetStore* assets_ = nullptr;
    const FontTextures* font_ = nullptr;
    std::string backdrop_;
    std::vector<std::filesystem::path> entries_;
    std::vector<std::string> names_;  // each file's -N scheme name ("" if unreadable)
    int row_ = 0;
    int top_ = 0;  // first visible row (scroll window of kVisibleRows)
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

// The powerup-rules sub-editor — sub_402595 @0x402595 (§5 'P'/'p', PINNED
// from the body): 13 rows at y = 24*i + 60, each a clickable button (id
// 5000+i) whose label is the powerup name getstring(850+i) in the
// byte_49D37A yellow ink, a born-with column (getstring(757), the
// dword_4647A4 count) and an override column (getstring(759) with the value
// when dword_464764 is set, else getstring(758)) in the general white ink.
// Header getstring(754) at (300, 30) in the byte_497F8F cyan ink; exit hint
// getstring(737) at the bottom. Activating a row runs sub_4023A2 @0x4023A2 —
// a CHAIN of four modal prompts, each independently cancellable (a cancel
// keeps that one field and still continues the chain):
//   1. born-with count — text entry (sub_42E938, seeded "%u", atoi, NO
//      clamp; only the .SCH reader clamps < 0 to 0 at load),
//   2. forbidden      — yes/no dialog (sub_42EDE0) into dword_4647E0,
//   3. has-override   — yes/no dialog into dword_464764,
//   4. override value — text entry (seeded "%d", atoi, NO clamp), asked only
//      when has-override is set; when it is NOT set the value is forced to 0
//      (even if prompt 3 was cancelled with it already clear).
// Exit keys: Enter(13)/Esc(27)/Space(32)/'Q'/'q' close the sub-editor; F1
// opens the help browser. The original activates rows by MOUSE ONLY (the
// 5000+i buttons); our keyboard 'E'/Right on the highlighted row is the
// port's substitute for that click (documented deviation, not an RE fact).
class PowerupRulesScreen {
public:
    PowerupRulesScreen(const AssetStore& assets, const FontTextures& font)
        : assets_(&assets), font_(&font) {}

    // Edits `rows` IN PLACE (a reference into the parent EditorGrid's own
    // powerups() vector) — the original edits the live globals with no
    // cancel path for the sub-editor as a whole.
    void enter(std::vector<assets::sch::PowerupRule>* rows);
    void on_key(SDL_Keycode key, AudioEngine& audio);
    // Digits typed while a text-entry prompt of the sub_4023A2 chain is
    // open arrive here (the chain's numeric prompts are generic text fields
    // + atoi in the original; we accept digits only since both fields are
    // numeric).
    bool prompting() const { return step_ != ChainStep::None; }
    void draw(SDL_Renderer* ren) const;

    bool done() const { return done_; }

private:
    // sub_4023A2's four-prompt chain, in its exact order.
    enum class ChainStep { None, BornWith, Forbidden, HasOverride, OverrideValue };

    void begin_chain();
    void advance_chain(AudioEngine& audio);

    const AssetStore* assets_ = nullptr;
    const FontTextures* font_ = nullptr;
    std::vector<assets::sch::PowerupRule>* rows_ = nullptr;
    int row_ = 0;
    ChainStep step_ = ChainStep::None;
    std::string entry_;  // in-progress text for the two numeric prompts
    bool done_ = false;
};

// The editor proper — sub_4028D2 (§5): single-cell mouse tile painting,
// Ctrl+F flood fill, the 10 movable start markers (with team flags),
// density/name prompts, and (via 'P') the powerup sub-editor. The canvas
// draws the real match art: "tile %d blank/solid/brick" sequences (the same
// TILES ANI the match field uses; the original formats the tileset number
// from dword_45B7B8, which only ever holds 0 — its '0'-key toggle flips it
// to -1, a dead state with no matching sequences) and MISC.ANI's
// "teamring%u" (%u = the start's team flag) under each start's slot-number
// label in the slot's own ink (sub_41672F; the editor runs with team play
// zeroed by sub_40330E, so it is always the slot-colour branch).
class EditorScreen {
public:
    EditorScreen(const AssetStore& assets, const FontTextures& font)
        : assets_(&assets), font_(&font), powerups_screen_(assets, font) {}

    // `initial` is nullopt for sub_4028D2(1) ("new scheme" — sub_4049C0's
    // fully-bricked pillar board, default name getstring(729), density 90);
    // a loaded Scheme for sub_4028D2(0) ("edit an existing scheme").
    // `default_starts` feeds sub_4049C0's VALUELST 600..619 start positions
    // through the caller (who owns the ValueList); nullptr => (0,0)s.
    void enter(std::optional<assets::sch::Scheme> initial, std::string backdrop,
               const std::array<std::array<int, 2>, kEditorMaxStarts>* default_starts = nullptr);

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
    // Called by the driver once powerups_screen().done() — returns input
    // routing to the grid (sub_402595 returning into sub_4028D2's loop).
    void close_powerups() { editing_powerups_ = false; }

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

    // Cell geometry — PINNED: the editor draws through the SAME cell->pixel
    // mappers as the match field (sub_426524/sub_42655F, which read the
    // field origin/cell tunables dword_464898/4648A8/4648A4/4648A0), so the
    // canvas is exactly the in-game field layout: origin (20, 68), cell
    // 40x36 (renderer.hpp's kFieldOriginX/Y + sim::kTileW/H). The caller
    // converts a raw mouse pixel into (gx, gy) with these before calling
    // on_mouse_down (our draw is top-left anchored per cell, so the inverse
    // map is the plain divide — the original's extra -(cellH/2-1) y bias in
    // sub_4266A3 compensates its bottom-anchored blit, not a different
    // layout).
    static constexpr int kCellW = 40;   // sim::kTileW
    static constexpr int kCellH = 36;   // sim::kTileH
    static constexpr int kOriginX = 20; // kFieldOriginX
    static constexpr int kOriginY = 68; // kFieldOriginY

private:
    // FillConfirm: sub_4028D2's Ctrl+F case asks the getstring(760)/97
    // yes/no confirm BEFORE flood-filling (the fill itself is the k/j loop
    // over sub_4048EB).
    enum class PromptKind { None, Density, Name, SaveConfirm, FillConfirm };

    const AssetStore* assets_ = nullptr;
    const FontTextures* font_ = nullptr;

    std::string backdrop_;
    EditorGrid grid_;
    EditorBrush brush_ = EditorBrush::Blank;
    int selected_start_ = 0;     // '+'/'='/'-'/'_' cycles this, §5

    // Canvas art, resolved in enter(): the match tile sequences ("tile 0
    // blank/solid/brick" — dword_45B7B8 is only ever 0, see the class doc)
    // and MISC.ANI's teamring0/teamring1 start markers. Empty Anims (missing
    // assets) fall back to the flat-colour swatches in draw().
    Anim tile_blank_, tile_solid_, tile_brick_;
    Anim teamring_[2];

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
