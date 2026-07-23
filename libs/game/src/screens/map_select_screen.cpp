#include "bomber/game/screens/map_select_screen.hpp"

#include <SDL3/SDL.h>

#include <cstddef>
#include <cstdint>
#include <iterator>
#include <string>
#include <vector>

#include "bomber/game/anim_pace.hpp"      // anim_step_index
#include "bomber/game/bmscreen.hpp"       // HelpBrowser
#include "bomber/game/frontend_util.hpp"  // pick_glue
#include "bomber/game/hud_format.hpp"     // fmt_s / fmt_us
#include "bomber/game/sprites.hpp"        // Sprite, Anim, resolve_sequence
#include "bomber/sim/constants.hpp"       // sim::kTileW / kTileH

namespace bomber::game {

// The 11 built-in level names (VALUELST 450-460 / getvalue(150+n)); RANDOM is
// getstring(149). These GENERIC fallbacks are ours — the real names live in the
// user's MESSAGES.TXT and load at runtime via getstring, never committed.
static const char* level_fallback(int idx) {
    static const char* kNames[] = {"NEW TRADITIONALIST",
                                   "CLASSIC GREEN ACRES",
                                   "HOCKEY RINK",
                                   "ANCIENT EGYPT",
                                   "COAL MINE",
                                   "BEACH",
                                   "ALIENS",
                                   "HAUNTED HOUSE",
                                   "UNDER THE OCEAN",
                                   "DEEP FOREST GREEN",
                                   "INNER CITY TRASH"};
    return (idx >= 0 && idx < static_cast<int>(std::size(kNames))) ? kNames[idx] : "LEVEL";
}

// The LEVEL & ROUNDS screen (sub_406DDE @0x406DDE, the VALUELST "OPTIONS SCREEN"
// getvalue 730/735). Screen 2 of the pre-match flow. A 2-row list on a random
// GLUE<n> backdrop (1020 track inherited): row 0 = LEVEL (-1 RANDOM else 0..10 of
// getvalue(35)=11 built-ins, named getstring(150+n) / getstring(149)); row 1 =
// NUMBER OF WINS (1..100). Left/Right cycle the highlighted row's value (level
// wraps [-1 .. 10]; wins +-1 or +-5 on PgUp/PgDn), Up/Down switch rows. Enter
// commits the level (state_.selected_level -> dword_464998) and win target (state_.win_target
// -> dword_464A7C) and starts; Escape backs to the player screen. Presentation
// only — the committed level drives start_match's stage choice.
AppInput MapSelectScreen::run() {
    const std::string glue = pick_glue(state_.setup_lcg, ctx_.values);
    const int level_count = static_cast<int>(ctx_.values.column_or(35, 0, 11));  // getvalue(35)
    const float lx = static_cast<float>(ctx_.values.column_or(735, 0, 55));
    const float ly = static_cast<float>(ctx_.values.column_or(735, 1, 170));
    const float lys = static_cast<float>(ctx_.values.column_or(735, 2, 24));

    // Sample-block preview geometry (docs/re/setup-screens.md "The sample
    // block preview", sub_406AA3, VALUELST 730-733): X,Y = grid origin,
    // XSize/YSize = grid size IN CELLS (5x5). Cell pitch is the same 40x36
    // the in-match renderer uses (sim::kTileW/kTileH), 1:1, no stretching.
    const int px = static_cast<int>(ctx_.values.column_or(730, 0, 400));
    const int py = static_cast<int>(ctx_.values.column_or(730, 1, 100));
    const int pxsize = static_cast<int>(ctx_.values.column_or(730, 2, 5));
    const int pysize = static_cast<int>(ctx_.values.column_or(730, 3, 5));

    int row = 0;  // 0 = level, 1 = wins (v34 = 2 rows in sub_406DDE)
    // WORKING COPIES (sub_406DDE 8092-8093: dword_45E0B8/45E0B4 seeded from
    // the committed globals on entry): edits touch only these; Enter/Space
    // commits them (LABEL_101, 8261-8271) and Escape DISCARDS them — the old
    // in-place member edits leaked cancelled changes into the next visit.
    int level = state_.selected_level;
    int wins = state_.win_target;
    // Enter/Space debounce (8100/8220-8228): accept is IGNORED until 1 s
    // (sub_4148AC() = 1 locally) after entry or the last value change — the
    // original's guard against a held Enter from the previous screen
    // committing instantly.
    std::uint64_t accept_after_ms = SDL_GetTicks() + 1000;
    // The sample-block pattern (which cells are blank/solid/brick, and which
    // level's tile art each drawn cell uses) is re-rolled only on screen
    // entry and on a LEVEL row change (sub_406AA3's v35 re-arm), NEVER every
    // frame — pinned in the doc above. -2 is a sentinel forcing the first
    // roll below.
    int pattern_level = -2;
    // tile_of[row][col]: the stage index whose "tile <n> solid/brick" art
    // that cell draws, or -1 for a blank cell. Solid/brick-ness itself is
    // re-derived below from the (j&1,i&1) parity rule, which is pure
    // geometry and does not need re-rolling.
    std::vector<std::vector<int>> tile_of(static_cast<std::size_t>(pysize),
                                          std::vector<int>(static_cast<std::size_t>(pxsize), -1));
    int field_stage = -1;  // the field-swatch stage picked alongside tile_of

    // Cursor blink + footer anchors, same VALUELST sources as the sibling
    // screens (sub_413BD6 / sub_413FB9).
    const int blink_base = static_cast<int>(ctx_.values.column_or(690, 0, 2));
    const int blink_spread = static_cast<int>(ctx_.values.column_or(690, 1, 2));
    const float fcx = static_cast<float>(ctx_.values.column_or(790, 0, 320));
    const float ffy = static_cast<float>(ctx_.values.column_or(790, 1, 440));
    const float lw = static_cast<float>(ctx_.values.column_or(735, 3, 300));  // clip width

    // One frame of the screen (sub_406DDE's per-frame body 8107-8153) — a
    // lambda so the F1 help browser composites over the identical frame.
    auto draw_frame = [&]() {
        // Re-roll the sample-block pattern on entry and whenever the LEVEL
        // changes (sub_406AA3's v35 re-arm) — never every frame.
        if (level != pattern_level) {
            pattern_level = level;
            int max_n = level_count > 1 ? level_count : 1;
            for (int i = 0; i < pysize; ++i) {
                for (int j = 0; j < pxsize; ++j) {
                    bool solid_cell = (j & 1) != 0 && (i & 1) != 0;
                    bool brick_cell = false;
                    if (!solid_cell && (j > 1 || i > 1)) {
                        state_.setup_lcg = state_.setup_lcg * 1664525u + 1013904223u;
                        brick_cell = (state_.setup_lcg >> 16) % 5 != 0;  // rand()%5 != 0
                    }
                    if (!solid_cell && !brick_cell) {
                        tile_of[static_cast<std::size_t>(i)][static_cast<std::size_t>(j)] = -1;
                        continue;
                    }
                    int n = level;
                    if (n < 0) {  // RANDOM: re-pick per cell (pinned quirk)
                        state_.setup_lcg = state_.setup_lcg * 1664525u + 1013904223u;
                        n = static_cast<int>((state_.setup_lcg >> 16) % static_cast<unsigned>(max_n));
                    }
                    tile_of[static_cast<std::size_t>(i)][static_cast<std::size_t>(j)] = n;
                }
            }
            field_stage = level;
            if (field_stage < 0) {
                state_.setup_lcg = state_.setup_lcg * 1664525u + 1013904223u;
                field_stage = static_cast<int>((state_.setup_lcg >> 16) % static_cast<unsigned>(max_n));
            }
        }

        SDL_SetRenderDrawColor(ctx_.sdl, 0, 0, 0, 255);
        SDL_RenderClear(ctx_.sdl);
        const Sprite& bg = ctx_.assets.frontend_pcx(glue);
        if (bg.tex) {
            SDL_FRect dst{0.0f, 0.0f, static_cast<float>(bg.w), static_cast<float>(bg.h)};
            SDL_RenderTexture(ctx_.sdl, bg.tex, nullptr, &dst);
        }
        // Sample-block preview panel (sub_406AA3, level&rounds audit
        // 2026-07-12): border fill = the general WHITE byte_49D38F —
        // (240,248,252), NOT the old invented (40,40,60) — at (378,80,
        // 224x202); the field swatch is a 1:1 CROP of FIELDn.PCX starting 48
        // rows down (the `&v17[12*640]` int-indexing = 48 scanlines; NOT a
        // stretch — sub_4428B4 is a plain rect copy), 220x198 at (380,82),
        // which lines the backdrop's own board grid up under the drawn
        // tiles; then the 5x5 solid/brick grid at native 40x36 cells.
        {
            const float bx = static_cast<float>(px - 22);
            const float by = static_cast<float>(py - 20);
            const float bw = static_cast<float>(pxsize * sim::kTileW + 24);
            const float bh = static_cast<float>(pysize * sim::kTileH + 22);
            SDL_SetRenderDrawColor(ctx_.sdl, 240, 248, 252, 255);
            SDL_FRect border{bx, by, bw, bh};
            SDL_RenderFillRect(ctx_.sdl, &border);
            if (field_stage >= 0) {
                const AssetStore::StagePreview& fprev = ctx_.assets.stage_preview(field_stage);
                if (fprev.field) {
                    const float sw = static_cast<float>(pxsize * sim::kTileW + 20);
                    const float sh = static_cast<float>(pysize * sim::kTileH + 18);
                    SDL_FRect src{0.0f, 48.0f, sw, sh};
                    SDL_FRect panel{static_cast<float>(px - 20), static_cast<float>(py - 18), sw,
                                    sh};
                    SDL_RenderTexture(ctx_.sdl, fprev.field, &src, &panel);
                }
            }
            for (int i = 0; i < pysize; ++i) {
                for (int j = 0; j < pxsize; ++j) {
                    int n = tile_of[static_cast<std::size_t>(i)][static_cast<std::size_t>(j)];
                    if (n < 0) continue;
                    const AssetStore::StagePreview& prev = ctx_.assets.stage_preview(n);
                    bool solid_cell = (j & 1) != 0 && (i & 1) != 0;
                    const Anim& a = solid_cell ? prev.solid : prev.brick;
                    if (a.steps.empty()) continue;
                    const Sprite& sp = a.steps[0];
                    if (!sp.tex) continue;
                    SDL_FRect cell{static_cast<float>(px + j * sim::kTileW),
                                   static_cast<float>(py + i * sim::kTileH),
                                   static_cast<float>(sim::kTileW),
                                   static_cast<float>(sim::kTileH)};
                    SDL_RenderTexture(ctx_.sdl, sp.tex, nullptr, &cell);
                }
            }
        }
        // Both text rows in the SAME white ink + black outline (sub_41696C;
        // byte_49D38F decodes to (240,248,252) — LUT 0x7FFF -> idx 72,
        // level&rounds audit), clip getvalue(738)=300. The original marks the
        // active row with the cursor sprite ALONE — the old per-row colour
        // highlight and the grey key legend were invented.
        const std::string level_name =
            level < 0 ? ctx_.assets.getstring(149, "Random Each Game")
                      : ctx_.assets.getstring(150 + level, level_fallback(level));
        const std::string level_line = fmt_s(ctx_.assets.getstring(210, "%s"), level_name);
        ctx_.front_font.draw_outlined(ctx_.sdl, level_line, lx, ly, 240, 248, 252, 0, 0, 0,
                                  lw);
        // Wins line: getstring(211) "%u %s to win match" with the %s picked
        // by the "win by kills" option — getstring(208) "Wins" /
        // getstring(209) "Kills" (pseudo.c 8124-8127; the old single-%u
        // splice left a literal "%s" on screen with the real MESSAGES.TXT).
        const std::string wins_word =
            ctx_.assets.getstring(state_.options.win_by_kills ? 209 : 208,
                              state_.options.win_by_kills ? "Kills" : "Wins");
        const std::string wins_line =
            fmt_us(ctx_.assets.getstring(211, "%u %s to win match"), wins, wins_word);
        ctx_.front_font.draw_outlined(ctx_.sdl, wins_line, lx, ly + lys, 240, 248, 252, 0,
                                  0, 0, lw);
        // Footer (sub_413FB9): centred cyan "Press F1 for help".
        const std::string help = ctx_.assets.getstring(330, "Press F1 for help");
        const float help_w = static_cast<float>(ctx_.front_font.measure(help));
        ctx_.front_font.draw_outlined(ctx_.sdl, help, fcx - (help_w + 2.0f) / 2.0f, ffy, 96,
                                  252, 252, 0, 0, 0);
        // The bomber-dude row cursor (sub_413BD6 at 8140-8141): (getvalue(735)
        // - 20, row_y + 16) — the +16 is the empirically pinned anchor nudge
        // (measured off THIS screen's native capture; see options_screen.cpp).
        Anim cur = resolve_sequence(ctx_.assets.misc(), "cursor1");
        if (!cur.steps.empty()) {
            const std::size_t st = ctx_.cursor_blink.step(SDL_GetTicks() / 1000ull, cur.steps.size(),
                                                      blink_base, blink_spread);
            const Sprite& sp = cur.steps[anim_step_index(st, cur.steps.size())];
            if (sp.tex) {
                SDL_FRect d{lx - 20.0f - static_cast<float>(sp.hx),
                            ly + lys * static_cast<float>(row) + 16.0f -
                                static_cast<float>(sp.hy),
                            static_cast<float>(sp.w), static_cast<float>(sp.h)};
                SDL_RenderTexture(ctx_.sdl, sp.tex, nullptr, &d);
            }
        }
    };

    bool waiting = true;
    while (waiting) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_EVENT_QUIT) return AppInput::Quit;
            if (ev.type != SDL_EVENT_KEY_DOWN) continue;
            const SDL_Keycode k = ev.key.key;
            if (k == SDLK_ESCAPE) {
                // sub_406DDE's own Esc handler (pseudo.c 8186-8191) is called
                // straight from sub_410F81's TAIL (pseudo.c 15516, gated
                // `if (!dword_464A68)`) with NO loop back to the player
                // screen afterwards — so this aborts the WHOLE Play flow to
                // the menu, exactly like the Goldman wheel's own Esc (doc §5),
                // NOT "back one screen" to present_setup. It also forfeits any
                // pending gold player (`dword_46492C = -1`, doc §2's "Cleared
                // to -1 by" list). The WORKING level/wins copies are simply
                // dropped (8092-8093 re-seed on the next entry) — the
                // committed selections stay untouched.
                ctx_.audio.play(20);
                state_.gold_player = -1;
                return AppInput::Back;
            }
            if (k == SDLK_F1) {
                // 0x13B -> sub_41431C (pseudo.c 8208-8216): the same generic
                // *.BM help browser, composited over this screen.
                ctx_.audio.play(20);
                HelpBrowser browser(ctx_.assets, ctx_.front_font);
                browser.enter(ctx_.values.at_or(15, 1) != 0);
                while (!browser.done()) {
                    SDL_Event hev;
                    while (SDL_PollEvent(&hev)) {
                        if (hev.type == SDL_EVENT_QUIT) return AppInput::Quit;
                        if (hev.type == SDL_EVENT_KEY_DOWN) browser.on_key(hev.key.key, ctx_.audio);
                    }
                    if (browser.viewing() && browser.viewer().done()) browser.close_viewer();
                    ctx_.audio.update_music();
                    draw_frame();
                    browser.draw(ctx_.sdl);
                    SDL_RenderPresent(ctx_.sdl);
                    SDL_Delay(2);
                }
                continue;
            }
            if (k == SDLK_RETURN || k == SDLK_KP_ENTER || k == SDLK_SPACE) {
                // Accept (LABEL_101, 8261-8271) — Space accepts too, and the
                // key first rides the any-key blip 20 (8176), then the accept
                // sting 10. Ignored inside the 1 s debounce window
                // (8220-8228): the key still blips, nothing commits.
                ctx_.audio.play(20);
                if (SDL_GetTicks() < accept_after_ms) continue;
                ctx_.audio.play(10);
                state_.selected_level = level;  // commit the working copies
                state_.win_target = wins;
                waiting = false;
                break;
            }
            ctx_.audio.play(20);
            if (k == SDLK_UP || k == SDLK_DOWN)
                row = (row + 1) % 2;  // 2 rows: either arrow toggles
            else if (row == 0 && k == SDLK_LEFT) {  // --level, wrap below -1
                if (--level < -1) level = level_count - 1;
                accept_after_ms = SDL_GetTicks() + 1000;  // debounce re-arm (8325)
            } else if (row == 0 && k == SDLK_RIGHT) {  // ++level, wrap above count-1 to -1
                if (++level >= level_count) level = -1;
                accept_after_ms = SDL_GetTicks() + 1000;
            } else if (row == 1 && k == SDLK_LEFT) {  // wins -1
                if (--wins < 1) wins = 1;
                accept_after_ms = SDL_GetTicks() + 1000;
            } else if (row == 1 && k == SDLK_RIGHT) {  // wins +1
                if (++wins > 100) wins = 100;
                accept_after_ms = SDL_GetTicks() + 1000;
            } else if (row == 1 && k == SDLK_PAGEUP) {
                // wins +5 (batch_0x405B3A.cpp 1097-1128, code 372) — WINS row
                // only, clamp 1..100. Rebound from the old invented Ctrl+Left/
                // Right, which the original never used.
                wins += 5;
                if (wins > 100) wins = 100;
                accept_after_ms = SDL_GetTicks() + 1000;
            } else if (row == 1 && k == SDLK_PAGEDOWN) {  // wins -5 (code 371), WINS row only
                wins -= 5;
                if (wins < 1) wins = 1;
                accept_after_ms = SDL_GetTicks() + 1000;
            }
        }

        ctx_.audio.update_music();
        draw_frame();
        SDL_RenderPresent(ctx_.sdl);
        SDL_Delay(2);
    }
    return AppInput::Advance;
}

}  // namespace bomber::game
