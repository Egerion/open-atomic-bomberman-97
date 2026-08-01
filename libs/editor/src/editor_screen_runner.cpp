#include "bomber/editor/editor_screen_runner.hpp"

#include <SDL3/SDL.h>

#include <array>
#include <cctype>   // std::isalnum (the filename sanitizer)
#include <cstddef>  // std::size_t
#include <exception>
#include <filesystem>
#include <optional>
#include <string>
#include <system_error>  // std::error_code

#include "bomber/assets/sch.hpp"               // assets::sch::Scheme/load/write
#include "bomber/editor/editor_screen.hpp"     // EditorChooserScreen/EditorScreen/SchemeFilePicker
#include "bomber/game_util/frontend_util.hpp"  // pick_glue
#include "bomber/game_util/log.hpp"            // log_warn
#include "bomber/ui/dialog_chrome.hpp"         // dispatch_list_mouse
#include "bomber/ui/help_screens.hpp"          // HelpBrowserScreen
#include "bomber/ui/scheme_filename_prompt.hpp"  // SchemeFilenamePrompt

namespace bomber::game {

namespace {

// One presented frame of a nested editor loop: the music pump, the clear, the
// screen's own draw, the present, and the 2 ms yield every front-end loop uses.
template <class Screen>
void present_frame(const ScreenContext& ctx, const Screen& screen) {
    ctx.audio.update_music();
    SDL_SetRenderDrawColor(ctx.sdl, 0, 0, 0, 255);
    SDL_RenderClear(ctx.sdl);
    screen.draw(ctx.sdl);
    SDL_RenderPresent(ctx.sdl);
    SDL_Delay(2);
}

// A window/backbuffer pixel as a logical (640x480) point, per the
// SDL_LOGICAL_PRESENTATION_LETTERBOX mode init() sets — the modern stand-in for
// sub_42665C/sub_4266A3's pixel->cell mappers (§5).
SDL_FPoint logical_point(SDL_Renderer* ren, float wx, float wy) {
    SDL_FPoint p{0.0f, 0.0f};
    SDL_RenderCoordinatesFromWindow(ren, wx, wy, &p.x, &p.y);
    return p;
}

// The chooser's own pump: the resolved action for this frame, or nothing when
// SDL_EVENT_QUIT ended it.
std::optional<EditorChooserResult> pump_chooser(const ScreenContext& ctx,
                                                EditorChooserScreen& chooser) {
    EditorChooserResult action = EditorChooserResult::None;
    SDL_Event ev;
    while (SDL_PollEvent(&ev)) {
        if (ev.type == SDL_EVENT_QUIT) return std::nullopt;
        if (ev.type != SDL_EVENT_KEY_DOWN) continue;
        const EditorChooserResult r = chooser.on_key(ev.key.key, ctx.audio);
        if (r != EditorChooserResult::None) action = r;
    }
    present_frame(ctx, chooser);
    return action;
}

struct PickResult {
    bool quit = false;                          // SDL_EVENT_QUIT ended the loop
    std::optional<std::filesystem::path> path;  // nothing when cancelled or empty
};

// sub_407582's *.SCH picker over DATA/SCHEMES.
PickResult run_scheme_picker(const ScreenContext& ctx, const std::filesystem::path& dir,
                             std::string backdrop) {
    SchemeFilePicker picker(ctx.assets, ctx.front_font);
    picker.enter(dir, std::move(backdrop));
    while (!picker.done()) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_EVENT_QUIT) return PickResult{true, std::nullopt};
            // sub_42DBCC is mouse-first; the list's arrows, track and rows are
            // live widgets, not decoration.
            if (dispatch_list_mouse(ctx.sdl, ev, picker)) continue;
            if (ev.type != SDL_EVENT_KEY_DOWN) continue;
            picker.on_key(ev.key.key, ctx.audio);
        }
        present_frame(ctx, picker);
    }
    if (picker.cancelled() || picker.empty()) return PickResult{};
    return PickResult{false, picker.selected()};
}

// What the chooser's answer resolves to: either a board to open, or a reason to
// go back round to the chooser.
struct BoardChoice {
    bool quit = false;                           // SDL_EVENT_QUIT ended the picker
    bool opened = false;                         // false: back to the chooser
    std::optional<assets::sch::Scheme> initial;  // nullopt for a New scheme
    std::string source_stem;                     // byte_4648C4's seed; empty for New
};

BoardChoice choose_board(const ScreenContext& ctx, EditorEditState& state,
                         EditorChooserResult action) {
    BoardChoice choice;
    // sub_4028D2(1), "new scheme": no picker, no source file, blank board.
    if (action == EditorChooserResult::New) {
        choice.opened = true;
        return choice;
    }
    if (action != EditorChooserResult::EditExisting) return choice;
    const PickResult pick = run_scheme_picker(ctx, state.game_dir / "DATA" / "SCHEMES",
                                              pick_glue(state.setup_lcg, ctx.values));
    choice.quit = pick.quit;
    if (pick.quit || !pick.path) return choice;  // cancelled, or an empty glob
    try {
        choice.initial = assets::sch::load(*pick.path);
        // The SOURCE filename stem, so a save writes BACK to the source rather
        // than to a name-derived sibling.
        choice.source_stem = pick.path->stem().string();
        choice.opened = true;
    } catch (const std::exception&) {
        choice.opened = false;  // corrupt or unreadable: back to the chooser
    }
    return choice;
}

// One SDL event into the editor. False when it was SDL_EVENT_QUIT.
bool feed_editor_event(const ScreenContext& ctx, EditorScreen& editor, const SDL_Event& ev) {
    if (ev.type == SDL_EVENT_QUIT) return false;
    if (editor.editing_powerups()) {
        if (ev.type != SDL_EVENT_KEY_DOWN) return true;
        editor.powerups_screen().on_key(ev.key.key);
        // sub_402595 returns into sub_4028D2's loop on its exit keys; mirror that
        // by closing the sub-editor here, since the screen sets done() but cannot
        // clear the parent's routing flag itself.
        if (editor.powerups_screen().done()) editor.close_powerups();
        return true;
    }
    if (ev.type == SDL_EVENT_TEXT_INPUT) {
        editor.on_text_input(ev.text.text);
        return true;
    }
    if (ev.type == SDL_EVENT_MOUSE_BUTTON_DOWN) {
        const SDL_FPoint p = logical_point(ctx.sdl, ev.button.x, ev.button.y);
        const int gx = (static_cast<int>(p.x) - EditorScreen::kOriginX) / EditorScreen::kCellW;
        const int gy = (static_cast<int>(p.y) - EditorScreen::kOriginY) / EditorScreen::kCellH;
        editor.on_mouse_down(ev.button.button, gx, gy);
        return true;
    }
    if (ev.type == SDL_EVENT_MOUSE_MOTION) {
        // §5d, PINNED: sub_431804 reads the live cursor position every loop
        // iteration to draw the brush preview AT it (pseudo.c 5520-5524) — the
        // same mapping as the button case, but RAW pixels, since EditorScreen
        // does its own hotspot-anchored draw.
        const SDL_FPoint p = logical_point(ctx.sdl, ev.motion.x, ev.motion.y);
        editor.on_mouse_move(p.x, p.y);
        return true;
    }
    if (ev.type != SDL_EVENT_KEY_DOWN) return true;
    // Ctrl+F (flood fill) and Ctrl+B (reset, §5) both need the modifier; every
    // other editor key, '0's tileset toggle included, is unmodified. So plain
    // 'F'/'B' — e.g. the powerup sub-editor's own 'F', a SEPARATE handler —
    // never reach EditorScreen ungated.
    const bool needs_ctrl = editor_key_needs_ctrl(ev.key.key);
    const bool ctrl_down = (ev.key.mod & SDL_KMOD_CTRL) != 0;
    if (!needs_ctrl || ctrl_down) editor.on_key(ev.key.key);
    return true;
}

// Text input for the whole editor session: the 'N'/'n' scheme-name prompt (§5)
// needs characters beyond the raw keycode switch, and leaving it on while the
// prompt is closed is harmless, since on_text_input only accepts them then. RAII
// so the SDL_EVENT_QUIT path cannot leak it.
class TextInputScope {
public:
    explicit TextInputScope(SDL_Window* window) : window_(window) { SDL_StartTextInput(window_); }
    ~TextInputScope() { SDL_StopTextInput(window_); }
    TextInputScope(const TextInputScope&) = delete;
    TextInputScope& operator=(const TextInputScope&) = delete;

private:
    SDL_Window* window_;
};

// False when SDL_EVENT_QUIT ended the session.
bool run_editor_loop(const ScreenContext& ctx, EditorScreen& editor) {
    const TextInputScope text_input{ctx.window};
    while (!editor.done()) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev))
            if (!feed_editor_event(ctx, editor, ev)) return false;
        present_frame(ctx, editor);
    }
    return true;
}

// sub_4049C0's default start positions for a NEW scheme: VALUELST
// x = getvalue(600+2j), y = getvalue(601+2j), wrapped into the board by
// EditorGrid::reset. Zeroes when VALUELST is absent.
std::array<std::array<int, 2>, kEditorMaxStarts> default_starts(
    const assets::res::ValueList& values) {
    std::array<std::array<int, 2>, kEditorMaxStarts> starts{};
    for (std::size_t j = 0; j < starts.size(); ++j) {
        const int id = 600 + 2 * static_cast<int>(j);
        starts[j][0] = static_cast<int>(values.column_or(id, 0, 0));
        starts[j][1] = static_cast<int>(values.column_or(id + 1, 0, 0));
    }
    return starts;
}

// §5: exit writes through sub_403C16 — our assets::sch::write() — on a confirmed
// save, into the install's DATA/SCHEMES dir (the SAME place the game loads them),
// NEVER the repo.
//
// Faithful save-as (sub_4028D2 exit, batch_0x402150.cpp:645-649): the original
// pops a getstring(736) filename entry SEEDED with the source filename
// (byte_4648C4) and writes to whatever it holds, so editing an existing scheme
// and accepting the prompt overwrites the SOURCE. The port used to derive the
// name from the -N field, which turned "edit BASIC.SCH -> save" into a stray
// sibling instead of an update. Seed with the source stem when editing an
// existing scheme, else the -N name, else "EDITED".
void save_edited_scheme(const ScreenContext& ctx, EditorEditState& state,
                        const EditorScreen& editor, const std::string& source_stem) {
    const assets::sch::Scheme out = editor.grid().to_scheme();
    const std::string seed =
        !source_stem.empty() ? source_stem : (out.name.empty() ? std::string("EDITED") : out.name);
    std::string file_stem = SchemeFilenamePrompt(ctx).run(seed);
    for (auto& c : file_stem)
        if (!std::isalnum(static_cast<unsigned char>(c))) c = '_';
    if (file_stem.empty()) file_stem = "EDITED";  // never write a bare ".SCH"
    const std::filesystem::path schemes_dir = state.game_dir / "DATA" / "SCHEMES";
    std::error_code ec;
    std::filesystem::create_directories(schemes_dir, ec);
    const std::filesystem::path out_path = schemes_dir / (file_stem + ".SCH");
    try {
        assets::sch::write(out, out_path);
        // Make the freshly-saved scheme immediately selectable through the
        // existing rotation/pick path: point this session's live scheme at it,
        // exactly as passing --scheme would.
        state.scheme = out;
        state.scheme_path = out_path;
    } catch (const std::exception& e) {
        log_warn("scheme editor: save failed: %s", e.what());
    }
}

}  // namespace

void EditorRunner::run() {
    // The hidden scheme editor (docs/re/results-and-options.md §5): the chooser
    // (sub_403184) -> optionally the *.SCH picker (sub_407582) -> the editor
    // proper (sub_4028D2) -> optionally the powerup sub-editor (sub_402595).
    // Runs its own nested loop exactly like present_keyremap_screen(): this
    // screen has no AppState/AppInput slot, so it simply returns to
    // present_menu's loop when the chooser is dismissed.
    EditorChooserScreen chooser(ctx_.assets, ctx_.front_font);
    chooser.enter(pick_glue(state_.setup_lcg, ctx_.values));

    while (true) {
        const std::optional<EditorChooserResult> action = pump_chooser(ctx_, chooser);
        if (!action || *action == EditorChooserResult::Exit) return;
        if (*action == EditorChooserResult::Help) {
            // §5: F1 opens the same generic help browser (sub_41431C, §4) every
            // other screen reaches on F1 — CORRECTED 2026-07-08: this used to
            // call present_bm_screen("EDITOR") directly, a fixed-topic cut that
            // contradicted its own comment. Confirmed against sub_403184's own
            // F1 branch (key code 315 calls sub_41431C): it is the generic
            // browser, listing EDITOR.BM as one glob entry among the rest.
            if (HelpBrowserScreen(ctx_).run() == AppInput::Quit) return;
            continue;
        }
        const BoardChoice board = choose_board(ctx_, state_, *action);
        if (board.quit) return;
        if (!board.opened) continue;

        // Canvas art: the editor draws the "tile 0 blank/solid/brick" sequences
        // (sub_402206's dword_45B7B8 is only ever 0 — §5), so make sure tileset 0
        // is the one loaded; a later start_match reloads whatever stage the match
        // picks.
        ctx_.assets.load_stage(0);

        const std::array<std::array<int, 2>, kEditorMaxStarts> starts = default_starts(ctx_.values);
        EditorScreen editor(ctx_.assets, ctx_.front_font);
        editor.enter(board.initial, pick_glue(state_.setup_lcg, ctx_.values), &starts);
        if (!run_editor_loop(ctx_, editor)) return;
        if (editor.save_requested()) save_edited_scheme(ctx_, state_, editor, board.source_stem);
        // Either way, saved or discarded, fall back to the chooser so Ctrl+E's
        // single trigger can serve multiple edits without re-pressing the 6-key
        // sequence — §5 does not document the chooser as single-shot, and
        // sub_403184's own loop implies it re-shows after each sub_4028D2 return.
    }
}

}  // namespace bomber::game
