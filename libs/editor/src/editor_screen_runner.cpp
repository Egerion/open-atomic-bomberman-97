#include "bomber/editor/editor_screen_runner.hpp"

#include <SDL3/SDL.h>

#include <array>
#include <cctype>    // std::isalnum (the filename sanitizer)
#include <cstddef>   // std::size_t
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

void EditorRunner::run() {
    // The hidden scheme editor (docs/re/results-and-options.md §5): the
    // chooser (sub_403184) -> optionally the *.SCH file picker (sub_407582)
    // -> the editor proper (sub_4028D2) -> optionally the powerup rules
    // sub-editor (sub_402595). Runs its own nested loop exactly like
    // present_keyremap_screen() — this screen has no AppState/AppInput slot
    // (there is no menu row for it), so it simply returns to present_menu's
    // own loop when the chooser is dismissed.
    EditorChooserScreen chooser(ctx_.assets, ctx_.front_font);
    chooser.enter(pick_glue(state_.setup_lcg, ctx_.values));

    while (true) {
        EditorChooserResult action = EditorChooserResult::None;
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_EVENT_QUIT) return;
            if (ev.type != SDL_EVENT_KEY_DOWN) continue;
            EditorChooserResult r = chooser.on_key(ev.key.key, ctx_.audio);
            if (r != EditorChooserResult::None) action = r;
        }
        ctx_.audio.update_music();
        SDL_SetRenderDrawColor(ctx_.sdl, 0, 0, 0, 255);
        SDL_RenderClear(ctx_.sdl);
        chooser.draw(ctx_.sdl);
        SDL_RenderPresent(ctx_.sdl);
        SDL_Delay(2);

        if (action == EditorChooserResult::Exit) return;
        if (action == EditorChooserResult::Help) {
            // §5: F1 opens the same generic help browser (sub_41431C, §4)
            // every other screen reaches on F1 — CORRECTED 2026-07-08: this
            // was calling present_bm_screen("EDITOR") directly (a fixed-topic
            // cut), contradicting this very comment. Confirmed against
            // sub_403184's own F1 branch (pseudo.c — key code 315 calls
            // sub_41431C): it is the generic browser, listing EDITOR.BM
            // as one glob entry among the rest (§3's correction: "reachable
            // only as a directory-listing entry of the help browser's *.BM
            // glob") — not a direct open of it.
            if (HelpBrowserScreen(ctx_).run() == AppInput::Quit) return;
            continue;
        }

        std::optional<assets::sch::Scheme> initial;
        bool opened = false;
        // The SOURCE filename stem (byte_4648C4's seed) when editing an existing
        // scheme — the faithful save-as prompt defaults to it so a save writes
        // BACK to the source, not a name-derived sibling. Empty for a New scheme.
        std::string source_stem;
        if (action == EditorChooserResult::New) {
            opened = true;  // sub_4028D2(1): blank board, EditorScreen::enter(nullopt, ...)
        } else if (action == EditorChooserResult::EditExisting) {
            // sub_407582: the *.SCH file picker over DATA/SCHEMES.
            SchemeFilePicker picker(ctx_.assets, ctx_.front_font);
            picker.enter(state_.game_dir / "DATA" / "SCHEMES", pick_glue(state_.setup_lcg, ctx_.values));
            while (!picker.done()) {
                SDL_Event pev;
                while (SDL_PollEvent(&pev)) {
                    if (pev.type == SDL_EVENT_QUIT) return;
                    if (dispatch_list_mouse(ctx_.sdl, pev, picker)) continue;
                    if (pev.type != SDL_EVENT_KEY_DOWN) continue;
                    picker.on_key(pev.key.key, ctx_.audio);
                }
                ctx_.audio.update_music();
                SDL_SetRenderDrawColor(ctx_.sdl, 0, 0, 0, 255);
                SDL_RenderClear(ctx_.sdl);
                picker.draw(ctx_.sdl);
                SDL_RenderPresent(ctx_.sdl);
                SDL_Delay(2);
            }
            if (!picker.cancelled() && !picker.empty()) {
                try {
                    initial = assets::sch::load(picker.selected());
                    source_stem = picker.selected().stem().string();  // byte_4648C4 seed
                    opened = true;
                } catch (const std::exception&) {
                    opened = false;  // corrupt/unreadable file: fall back to the chooser
                }
            }
        }

        if (!opened) continue;  // back to the chooser menu

        // Canvas art: the editor draws the "tile 0 blank/solid/brick"
        // sequences (sub_402206's dword_45B7B8 is only ever 0 — §5), so make
        // sure tileset 0 is the one loaded in AssetStore; a later
        // start_match reloads whatever stage the match picks.
        ctx_.assets.load_stage(0);

        // sub_4049C0's default start positions for a NEW scheme: VALUELST
        // x = getvalue(600+2j), y = getvalue(601+2j) (wrapped into the board
        // by EditorGrid::reset). Fallback 0s if VALUELST is absent.
        std::array<std::array<int, 2>, kEditorMaxStarts> default_starts{};
        for (int j = 0; j < kEditorMaxStarts; ++j) {
            default_starts[static_cast<std::size_t>(j)][0] =
                static_cast<int>(ctx_.values.column_or(600 + 2 * j, 0, 0));
            default_starts[static_cast<std::size_t>(j)][1] =
                static_cast<int>(ctx_.values.column_or(601 + 2 * j, 0, 0));
        }

        EditorScreen editor(ctx_.assets, ctx_.front_font);
        editor.enter(initial, pick_glue(state_.setup_lcg, ctx_.values), &default_starts);
        // The 'N'/'n' scheme-name prompt (§5) needs real text input (letters
        // beyond the raw keycode switch below); start it for the whole
        // editor session — harmless while the prompt is closed since
        // on_text_input() only accepts characters when prompt_kind_==Name.
        SDL_StartTextInput(ctx_.window);
        while (!editor.done()) {
            SDL_Event eev;
            while (SDL_PollEvent(&eev)) {
                if (eev.type == SDL_EVENT_QUIT) return;
                if (editor.editing_powerups()) {
                    if (eev.type != SDL_EVENT_KEY_DOWN) continue;
                    editor.powerups_screen().on_key(eev.key.key);
                    // sub_402595 returns into sub_4028D2's loop on its exit
                    // keys — mirror that by closing the sub-editor here (the
                    // screen sets done() but cannot clear the parent's
                    // routing flag itself).
                    if (editor.powerups_screen().done()) editor.close_powerups();
                    continue;
                }
                if (eev.type == SDL_EVENT_TEXT_INPUT) {
                    editor.on_text_input(eev.text.text);
                    continue;
                }
                if (eev.type == SDL_EVENT_MOUSE_BUTTON_DOWN) {
                    // Window/backbuffer pixel -> logical (640x480) coordinate,
                    // per the SDL_LOGICAL_PRESENTATION_LETTERBOX mode set in
                    // init() — mirrors sub_42665C/sub_4266A3's pixel->cell
                    // mappers (§5), then the logical pixel -> grid cell via
                    // EditorScreen's own fixed cell geometry.
                    float lx = 0, ly = 0;
                    SDL_RenderCoordinatesFromWindow(ctx_.sdl, eev.button.x, eev.button.y,
                                                    &lx, &ly);
                    int gx = (static_cast<int>(lx) - EditorScreen::kOriginX) / EditorScreen::kCellW;
                    int gy = (static_cast<int>(ly) - EditorScreen::kOriginY) / EditorScreen::kCellH;
                    editor.on_mouse_down(eev.button.button, gx, gy);
                    continue;
                }
                if (eev.type == SDL_EVENT_MOUSE_MOTION) {
                    // §5d, PINNED: sub_431804 reads the live cursor position
                    // every loop iteration to draw the brush preview AT it
                    // (pseudo.c 5520-5524) — same logical-coordinate mapping
                    // as the button-down case above, but RAW pixels (no
                    // grid-cell snapping; EditorScreen::on_mouse_move does
                    // its own hotspot-anchored draw).
                    float lx = 0, ly = 0;
                    SDL_RenderCoordinatesFromWindow(ctx_.sdl, eev.motion.x, eev.motion.y,
                                                    &lx, &ly);
                    editor.on_mouse_move(lx, ly);
                    continue;
                }
                if (eev.type != SDL_EVENT_KEY_DOWN) continue;
                // Ctrl+F (flood fill) and Ctrl+B (reset, §5) both need the
                // modifier; every other editor key (including '0's tileset
                // toggle) is unmodified — editor_key_needs_ctrl
                // (editor_screen.hpp) lists exactly those two, so plain
                // 'F'/'B' (e.g. the powerup sub-editor's own 'F' forbidden
                // toggle — a SEPARATE screen/handler) never reach here
                // ungated.
                bool needs_ctrl = editor_key_needs_ctrl(eev.key.key);
                bool ctrl_down = (eev.key.mod & SDL_KMOD_CTRL) != 0;
                if (!needs_ctrl || ctrl_down) editor.on_key(eev.key.key);
            }
            ctx_.audio.update_music();
            SDL_SetRenderDrawColor(ctx_.sdl, 0, 0, 0, 255);
            SDL_RenderClear(ctx_.sdl);
            editor.draw(ctx_.sdl);
            SDL_RenderPresent(ctx_.sdl);
            SDL_Delay(2);
        }
        SDL_StopTextInput(ctx_.window);

        // §5: exit writes through sub_403C16 — our assets::sch::write() — on a
        // confirmed save. Written schemes go to the install's DATA/SCHEMES dir
        // (the SAME place the game loads them), NEVER the repo. Faithful save-as
        // (sub_4028D2 exit, batch_0x402150.cpp:645-649): the original pops a
        // getstring(736) filename text-entry SEEDED with the source filename
        // (byte_4648C4) and writes to whatever it holds — so editing an existing
        // scheme and accepting the prompt overwrites the SOURCE. The port used
        // to derive the name from the -N field, which turned "edit BASIC.SCH ->
        // save" into a stray sibling instead of an update. Seed with the source
        // stem when editing existing, else the -N name, else "EDITED".
        if (editor.save_requested()) {
            assets::sch::Scheme out = editor.grid().to_scheme();
            const std::string seed =
                !source_stem.empty() ? source_stem
                                     : (out.name.empty() ? std::string("EDITED") : out.name);
            std::string file_stem = SchemeFilenamePrompt(ctx_).run(seed);
            for (auto& c : file_stem)
                if (!std::isalnum(static_cast<unsigned char>(c))) c = '_';
            if (file_stem.empty()) file_stem = "EDITED";  // never write a bare ".SCH"
            std::filesystem::path schemes_dir = state_.game_dir / "DATA" / "SCHEMES";
            std::error_code ec;
            std::filesystem::create_directories(schemes_dir, ec);
            std::filesystem::path out_path = schemes_dir / (file_stem + ".SCH");
            try {
                assets::sch::write(out, out_path);
                // Make the freshly-saved scheme immediately selectable
                // through the existing scheme rotation/pick path (task
                // requirement 3): point this session's live scheme at it,
                // exactly like passing --scheme would.
                state_.scheme = out;
                state_.scheme_path = out_path;
            } catch (const std::exception& e) {
                log_warn("scheme editor: save failed: %s", e.what());
            }
        }
        // Either way (saved or discarded), fall back to the chooser so
        // Ctrl+E's single trigger can serve multiple edits without
        // re-pressing the 6-key sequence — §5 does not document the
        // chooser as single-shot, and sub_403184's own loop (its Esc/'Q'
        // exit case) implies it re-shows after each sub_4028D2 return.
    }
}

}  // namespace bomber::game
