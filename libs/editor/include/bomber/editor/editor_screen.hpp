#pragma once

// The hidden scheme editor's SDL presentation — sub_403184 (chooser),
// sub_4028D2 (the editor proper) and sub_402595 (powerup-rules sub-editor),
// docs/re/results-and-options.md §5, CONFIRMED. Reached ONLY by the raw
// Ctrl+E x6 trigger inside the main menu's input loop (mirroring sub_42B9CE's
// `++counter > 5` on key code 5): the original has no menu row for it, so unlike
// OptionsScreen/KeyRemapScreen it is not part of the AppState/AppInput flow
// graph and EditorRunner owns its own nested event loop instead.

#include <SDL3/SDL.h>

#include <array>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "bomber/audio/audio_engine.hpp"
#include "bomber/game_util/editor_grid.hpp"
#include "bomber/game_util/list_dialog_geometry.hpp"
#include "bomber/render/asset_store.hpp"
#include "bomber/ui/bmscreen.hpp"
#include "bomber/ui/list_picker.hpp"

namespace bomber::game {

// The *.SCH file picker — sub_407582 @0x407582 (§5c, PINNED from the body,
// re-read 2026-07-26). Globs "*.SCH" (sub_41404B, the same helper as the help
// browser), reads each file's embedded -N scheme name (sub_404BE9; getstring(727)
// "No Scheme Name" when absent) and lists rows formatted through aSS = "%s: %s"
// @0x458B11 in the generic list dialog at the LITERAL (100, 100) with header
// getstring(721) and 10 visible rows.
//
// Selecting a row cuts the line at its FIRST ':' — the separator loaded @0x40767A
// is 0x3A, fed to sub_45167A/strchr, CORRECTED 2026-07-26 from the earlier
// "first '.'" reading — so the stored value KEEPS the extension, uppercased into
// byte_4648C4. It still loads, because the '.'-strip lives in the READER
// (sub_403EEE @0x403FE8). The visible consequence is Options row 8, which prints
// the buffer verbatim ("Scheme File: BASIC.SCH", not "...: BASIC").
//
// An empty glob shows the getstring(95)/getstring(720) sub_414340 error instead,
// drawn by this class because in the original it is the same ONE routine serving
// both the editor's "edit existing" path and Options row 8 (pseudo.c 5501/9445).
class SchemeFilePicker {
public:
    SchemeFilePicker(const AssetStore& assets, const FontTextures& font)
        : assets_(&assets), font_(&font), list_(font) {}

    // Case-insensitive extension match, as the real DOS findfirst would see on a
    // FAT volume; pre-reads each scheme's -N name for the second column.
    void enter(const std::filesystem::path& schemes_dir, std::string backdrop);

    // Navigation is sub_42DBCC's own (list_dialog_geometry.hpp's input model —
    // no wrap, and the four view keys move only the scroll window).
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

    static constexpr int kVisibleRows = ListPicker::kVisibleRows;

private:
    // One row as the original formats it: "<FILENAME.SCH>: <scheme name>". Used
    // both to draw and to measure the list's width (sub_42FEF0).
    std::string row_text(int i) const;
    std::string header() const;
    void apply(ListDialogAction action);

    const AssetStore* assets_ = nullptr;
    const FontTextures* font_ = nullptr;
    std::string backdrop_;
    // The shared (100,100) glob/list widget — sub_41404B + sub_42DBCC.
    ListPicker list_;
    std::vector<std::string> names_;  // each file's -N scheme name ("" if unreadable)
    bool done_ = false;
    bool cancelled_ = false;
};

// sub_403184's 3-item chooser (§5): '1' edit an existing scheme (after the file
// picker), '2' new scheme, Esc/'Q' exit, F1 help.
enum class EditorChooserResult : std::uint8_t {
    None,  // still open
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
    // Returns the resolved action for this frame (None most frames).
    EditorChooserResult on_key(SDL_Keycode key, AudioEngine& audio);
    void draw(SDL_Renderer* ren) const;

private:
    const AssetStore* assets_ = nullptr;
    const FontTextures* font_ = nullptr;
    std::string backdrop_;
};

// The powerup-rules sub-editor — sub_402595 @0x402595 (§5 'P'/'p', PINNED from
// the body): 13 rows at y = 24*i + 60, each a clickable button (id 5000+i)
// labelled getstring(850+i) in the byte_49D37A yellow, plus a born-with column
// (getstring(757), dword_4647A4) and an override column (getstring(759) with the
// value when dword_464764 is set, else getstring(758)) in white. Header
// getstring(754) at (300, 30) in byte_497F8F cyan; exit hint getstring(737).
//
// Activating a row runs sub_4023A2 @0x4023A2, a CHAIN of four modal prompts,
// each independently cancellable (a cancel keeps that one field and the chain
// still continues):
//   1. born-with count — text entry (sub_42E938, seeded "%u", atoi, NO clamp;
//      only the .SCH reader clamps < 0 to 0 at load),
//   2. forbidden      — yes/no (sub_42EDE0) into dword_4647E0,
//   3. has-override   — yes/no into dword_464764,
//   4. override value — text entry (seeded "%d", atoi, NO clamp), asked only
//      when has-override is set; when it is NOT set the value is forced to 0,
//      even if prompt 3 was cancelled with it already clear.
// Exit keys: Enter(13)/Esc(27)/Space(32)/'Q'/'q'; F1 opens the help browser. The
// original activates rows by MOUSE ONLY, so the keyboard 'E'/Right here is the
// port's substitute for that click — a documented deviation, not an RE fact.
class PowerupRulesScreen {
public:
    PowerupRulesScreen(const AssetStore& assets, const FontTextures& font)
        : assets_(&assets), font_(&font) {}

    // Edits `rows` IN PLACE (a reference into the parent EditorGrid's own
    // powerups() vector) — the original edits the live globals, with no cancel
    // path for the sub-editor as a whole.
    void enter(std::vector<assets::sch::PowerupRule>* rows);
    // No AudioEngine: sub_402595 / sub_4023A2 / sub_42E938 / sub_42EDE0 contain
    // no play call, so there is nothing for this screen to talk to.
    void on_key(SDL_Keycode key);
    bool prompting() const { return step_ != ChainStep::None; }
    void draw(SDL_Renderer* ren) const;

    bool done() const { return done_; }

private:
    // sub_4023A2's four-prompt chain, in its exact order.
    enum class ChainStep : std::uint8_t { None, BornWith, Forbidden, HasOverride, OverrideValue };

    void begin_chain();
    void advance_chain();
    void on_text_prompt_key(SDL_Keycode key);
    void on_yesno_prompt_key(SDL_Keycode key);
    void on_row_key(SDL_Keycode key);
    std::string powerup_name(int row) const;
    void draw_row(SDL_Renderer* ren, int row) const;
    void draw_chain_prompt(SDL_Renderer* ren) const;

    const AssetStore* assets_ = nullptr;
    const FontTextures* font_ = nullptr;
    std::vector<assets::sch::PowerupRule>* rows_ = nullptr;
    int row_ = 0;
    ChainStep step_ = ChainStep::None;
    std::string entry_;  // in-progress text for the two numeric prompts
    bool done_ = false;
};

// The editor proper — sub_4028D2 (§5): single-cell mouse tile painting, Ctrl+F
// flood fill, the 10 movable start markers (with team flags), density/name
// prompts, and (via 'P') the powerup sub-editor. The canvas draws the real match
// art: the "tile %d blank/solid/brick" sequences and MISC.ANI's "teamring%u".
class EditorScreen {
public:
    EditorScreen(const AssetStore& assets, const FontTextures& font)
        : assets_(&assets), font_(&font), powerups_screen_(assets, font) {}

    // `initial` is nullopt for sub_4028D2(1) ("new scheme" — sub_4049C0's fully
    // bricked pillar board, default name getstring(729), density 90); a loaded
    // Scheme for sub_4028D2(0). `default_starts` feeds sub_4049C0's VALUELST
    // 600..619 positions through the caller, who owns the ValueList.
    void enter(const std::optional<assets::sch::Scheme>& initial, std::string backdrop,
               const std::array<std::array<int, 2>, kEditorMaxStarts>* default_starts = nullptr);

    // True while a modal density/name/confirm prompt is capturing input, so the
    // caller routes key/text events here instead of the grid shortcuts.
    bool prompting() const { return prompt_kind_ != PromptKind::None; }
    // True while the powerup sub-editor (sub_402595) is open on top.
    bool editing_powerups() const { return editing_powerups_; }
    PowerupRulesScreen& powerups_screen() { return powerups_screen_; }
    // Called by the driver once powerups_screen().done() — returns input routing
    // to the grid (sub_402595 returning into sub_4028D2's loop).
    void close_powerups() { editing_powerups_ = false; }

    // No AudioEngine: the editor screen is SILENT in the original — see the
    // definition's header comment for the call-site evidence.
    void on_key(SDL_Keycode key);
    void on_text_input(const char* text);
    // `gx`/`gy` are already-converted grid cells; the caller does the pixel->cell
    // mapping, mirroring sub_42665C/sub_4266A3 (§5).
    void on_mouse_down(int button, int gx, int gy);
    // Raw mouse PIXEL position (NOT grid-snapped) — feeds the brush preview
    // (§5d), which the original redraws at the live cursor every loop iteration.
    void on_mouse_move(float px, float py) {
        mouse_px_ = px;
        mouse_py_ = py;
    }

    void draw(SDL_Renderer* ren) const;

    // True once Esc/'Q' was accepted past the save-changes confirm (§5).
    bool done() const { return done_; }
    // True if that confirm resolved to "save" (getstring(735)) not "discard".
    bool save_requested() const { return save_requested_; }
    const EditorGrid& grid() const { return grid_; }

    // Cell geometry — PINNED: the editor draws through the SAME cell->pixel
    // mappers as the match field (sub_426524/sub_42655F, reading the field
    // origin/cell tunables dword_464898/4648A8/4648A4/4648A0), so the canvas IS
    // the in-game field layout. The caller inverts these with a plain divide;
    // the original's extra -(cellH/2-1) y bias in sub_4266A3 compensates its
    // bottom-anchored blit, not a different layout.
    static constexpr int kCellW = 40;    // sim::kTileW
    static constexpr int kCellH = 36;    // sim::kTileH
    static constexpr int kOriginX = 20;  // kFieldOriginX
    static constexpr int kOriginY = 68;  // kFieldOriginY

private:
    enum class PromptKind : std::uint8_t {
        None,
        Density,
        Name,
        SaveConfirm,
        FillConfirm,
        ResetConfirm
    };

    void load_board(const std::optional<assets::sch::Scheme>& initial);
    void cycle_brush();
    void start_density_prompt();
    void start_name_prompt();
    void start_board_reset();
    void open_powerup_editor();
    void request_exit();
    // Re-resolves tile_blank_/solid_/brick_ from `tileset_` — once in enter(),
    // and again whenever the '0' key changes it (§5 case 48).
    void refresh_tile_sequences();
    bool have_tile_art() const;
    const Anim& brush_anim() const;

    void on_prompt_key(SDL_Keycode key);
    void on_density_key(SDL_Keycode key);
    void on_name_key(SDL_Keycode key);
    void on_confirm_key(SDL_Keycode key);
    void on_grid_key(SDL_Keycode key);

    void draw_grid(SDL_Renderer* ren) const;
    void draw_cell(SDL_Renderer* ren, int x, int y) const;
    void draw_brush_preview(SDL_Renderer* ren) const;
    void draw_starts(SDL_Renderer* ren) const;
    void draw_start_marker(SDL_Renderer* ren, int slot) const;
    void draw_status(SDL_Renderer* ren) const;
    void draw_prompt(SDL_Renderer* ren) const;
    void draw_confirm(SDL_Renderer* ren, int line_id, const char* line_default, int note_id) const;

    const AssetStore* assets_ = nullptr;
    const FontTextures* font_ = nullptr;

    std::string backdrop_;
    EditorGrid grid_;
    EditorBrush brush_ = EditorBrush::Blank;
    int selected_start_ = 0;  // '+'/'='/'-'/'_' cycles this, §5

    // Ctrl+B's own reset target — sub_4049C0's VALUELST 600..619 start
    // positions, the SAME table enter()'s "new scheme" path uses. Stored rather
    // than passed through once, because Ctrl+B can re-run the reset any number
    // of times in one session. Never owned; the caller's array outlives it.
    const std::array<std::array<int, 2>, kEditorMaxStarts>* default_starts_ = nullptr;

    // §5 case 48's tileset toggle (editor_grid.hpp's toggle_editor_tileset) —
    // only ever 0 or -1. Canvas-art state, not board data.
    int tileset_ = 0;

    // Raw mouse pixel for the brush preview (§5d). Defaults to the canvas origin
    // so the first frame, before any SDL_EVENT_MOUSE_MOTION, previews on-canvas
    // rather than off-screen at (0,0).
    float mouse_px_ = kOriginX;
    float mouse_py_ = kOriginY;

    // Canvas art, resolved in enter(). Empty Anims fall back to flat swatches.
    Anim tile_blank_, tile_solid_, tile_brick_;
    std::array<Anim, 2> teamring_{};

    PromptKind prompt_kind_ = PromptKind::None;
    std::string prompt_text_;  // in-progress text for the Density/Name prompts

    bool editing_powerups_ = false;
    PowerupRulesScreen powerups_screen_;

    // sub_4028D2's own "touched" flag (PINNED, pseudo.c 5513/5555-5710): false
    // each session; set by paint, start-move, an ACCEPTED Ctrl+F fill, an
    // ACCEPTED density/name edit, ANY team-flag toggle, and ANY powerup
    // sub-editor open (whether or not it changes anything) — each site carries
    // its own case-label citation. Gates BOTH Ctrl+B (silent reset while false,
    // confirm-gated once true) AND the Esc/'Q' exit (no prompt and no write).
    bool dirty_ = false;

    bool done_ = false;
    bool save_requested_ = false;
};

// The Ctrl gate the driver applies at the SDL event level: Ctrl+F (raw code 6)
// and Ctrl+B (raw code 2) are ASCII control codes the original's key stream
// encodes directly, which SDL instead reports as the plain letter keycode plus
// KMOD_CTRL. Every other editor key is unmodified and passes straight through.
inline bool editor_key_needs_ctrl(SDL_Keycode key) {
    return key == SDLK_F || key == SDLK_B;
}

}  // namespace bomber::game
