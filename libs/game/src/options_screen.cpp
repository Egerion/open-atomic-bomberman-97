#include "bomber/game/options_screen.hpp"

#include <cstdio>
#include <string>

#include "bomber/game/anim_pace.hpp"

namespace bomber::game {

namespace {

// CONFIRMED layout (docs/re/results-and-options.md §3): VALUELST
// `745,55,40,22,500` -> x=55, y0=40, ystep=22, W=500 (a text-clip width, not
// a colour — see options_screen.hpp's file doc).
constexpr int kListX = 55;
constexpr int kListY0 = 40;
constexpr int kListYStep = 22;
// CONFIRMED: sub_413BD6(getvalue(745)-20, row_y) — the cursor sprite sits
// 20px left of the row text's own x.
constexpr int kCursorX = kListX - 20;

// CONFIRMED (docs/re/results-and-options.md §1's LUT decode table):
// byte_49D38F = general draw ink = RGB(255,255,255), used for EVERY row,
// selected or not — the original has no per-row/selected recolour at all,
// only the separate cursor1 sprite (drawn in draw() below) marks selection.
constexpr Uint8 kInkR = 255, kInkG = 255, kInkB = 255;

const char* conveyor_label(int idx) {
    switch (idx) {
        case 0: return "LOW";
        case 1: return "MEDIUM";
        case 2: return "HIGH";
        default: return "?";
    }
}

const char* enclosement_label(int idx) {
    // §3 row 7: msg 315-318, "None/A Little/A Lot/All the way".
    switch (idx) {
        case 0: return "NONE";
        case 1: return "A LITTLE";
        case 2: return "A LOT";
        case 3: return "ALL THE WAY";
        default: return "?";
    }
}

std::string playtime_label(int seconds) {
    if (seconds == 1001) return "UNLIMITED";
    return std::to_string(seconds) + "s";
}

int playtime_index(int seconds) {
    for (int i = 0; i < kPlayTimeChoiceCount; ++i)
        if (kPlayTimeChoices[i] == seconds) return i;
    return 0;
}

const char* yes_no(bool v) { return v ? "YES" : "NO"; }  // getstring(<global>+25): 25=" No ", 26=" Yes "

}  // namespace

void OptionsScreen::enter(const OptionsSnapshot& current, std::string backdrop) {
    row_ = 0;
    cursor_frame_ = 0;
    done_ = false;
    changed_ = false;
    open_keyremap_ = false;
    goldman_touched_ = false;
    team_play_touched_ = false;
    snap_ = current;
    if (snap_.conveyor_speed_index < 0) snap_.conveyor_speed_index = 0;
    if (snap_.conveyor_speed_index > 2) snap_.conveyor_speed_index = 2;
    if (snap_.enclosement_depth < 0) snap_.enclosement_depth = 0;
    if (snap_.enclosement_depth > 3) snap_.enclosement_depth = 3;
    // Random GLUE<n> backdrop (docs/re/setup-screens.md "Backdrop — a RANDOM
    // glue picture", CONFIRMED sub_4148E5) — picked by the caller's shared
    // pick_glue() (GameApp), the same helper present_setup/present_map_select
    // use, so there is exactly one presentation LCG for this pick.
    backdrop_ = std::move(backdrop);
}

void OptionsScreen::on_key(SDL_Keycode key, AudioEngine& audio) {
    open_keyremap_ = false;
    // CONFIRMED (pseudo.c 9298-9299): sub_4080DC plays SFX 20 (nav blip) for
    // ANY real keypress, unconditionally — there is no distinct "accept"
    // sound anywhere in this function. One call here covers every branch
    // below, replacing the earlier per-branch audio.play(10)/(20) split.
    audio.play(20);
    switch (key) {
        case SDLK_UP:
        case SDLK_W:
            // CONFIRMED off-by-one (pseudo.c 9086/9391-9392, `v168 = 18`):
            // wraps over kCursorRowCount (18), not kCount (19) — row 18 is
            // never reachable. See options_screen.hpp's file doc.
            row_ = (row_ + kCursorRowCount - 1) % kCursorRowCount;
            break;
        case SDLK_DOWN:
        case SDLK_S:
            row_ = (row_ + 1) % kCursorRowCount;
            break;
        case SDLK_LEFT:
        case SDLK_RIGHT: {
            int dir = (key == SDLK_LEFT) ? -1 : 1;
            switch (static_cast<OptionRow>(row_)) {
                case OptionRow::TeamPlay:
                    snap_.team_play = !snap_.team_play;
                    // §3 row 0: "forces win_by_kills off" when Team Play is on.
                    if (snap_.team_play) snap_.win_by_kills = false;
                    // pseudo.c 9310-9311/9410-9412: toggling Team Play ALSO
                    // clears `dword_46492C` inline, every press — not just
                    // Gold Bomberman's own row (doc §2's "Cleared to -1 by"
                    // list previously missed this).
                    team_play_touched_ = true;
                    changed_ = true;
                    break;
                case OptionRow::RandomStart:
                    snap_.random_start = !snap_.random_start;
                    changed_ = true;
                    break;
                case OptionRow::NodeName:
                case OptionRow::SchemeFile:
                case OptionRow::Modem:
                case OptionRow::NetProtocol:
                    // Display-only rows (options_screen.hpp's file doc) — the
                    // original's handlers here are a text-entry prompt / a
                    // `.SCH` file-list stepper / nested sub-screens this port
                    // does not implement; Left/Right/Enter are no-ops, same
                    // as the original leaves them for an unbuilt feature.
                    break;
                case OptionRow::ConveyorSpeed: {
                    int v = snap_.conveyor_speed_index + dir;
                    if (v < 0) v = 2;
                    if (v > 2) v = 0;
                    snap_.conveyor_speed_index = v;
                    changed_ = true;
                    break;
                }
                case OptionRow::StompedBombs:
                    snap_.stomped_bombs_detonate = !snap_.stomped_bombs_detonate;
                    changed_ = true;
                    break;
                case OptionRow::WinByKills:
                    // §3 row 5: "forced off whenever Team Play is on" — a
                    // no-op toggle attempt while Team Play holds it down,
                    // matching the original's one-way gate (row 0 -> row 5,
                    // not the reverse).
                    if (!snap_.team_play) {
                        snap_.win_by_kills = !snap_.win_by_kills;
                        changed_ = true;
                    }
                    break;
                case OptionRow::GoldBomberman:
                    snap_.goldman = !snap_.goldman;
                    // pseudo.c 9334-9335/9436-9437: cleared inline on every
                    // press, matching team_play_touched_ above.
                    goldman_touched_ = true;
                    changed_ = true;
                    break;
                case OptionRow::EnclosementDepth: {
                    int v = snap_.enclosement_depth + dir;
                    if (v < 0) v = 3;
                    if (v > 3) v = 0;
                    snap_.enclosement_depth = v;
                    changed_ = true;
                    break;
                }
                case OptionRow::PlayTime: {
                    int idx = playtime_index(snap_.playtime_seconds) + dir;
                    if (idx < 0) idx = kPlayTimeChoiceCount - 1;
                    if (idx >= kPlayTimeChoiceCount) idx = 0;
                    snap_.playtime_seconds = kPlayTimeChoices[idx];
                    changed_ = true;
                    break;
                }
                case OptionRow::AssignKeyboard:
                    snap_.assign_keyboards = !snap_.assign_keyboards;
                    changed_ = true;
                    break;
                case OptionRow::DiseasesDestroy:
                    snap_.diseases_destroyable = !snap_.diseases_destroyable;
                    changed_ = true;
                    break;
                case OptionRow::LostNetRevertAI:
                    snap_.lost_net_revert_ai = !snap_.lost_net_revert_ai;
                    changed_ = true;
                    break;
                case OptionRow::DisableMusic:
                    snap_.disable_game_music = !snap_.disable_game_music;
                    changed_ = true;
                    break;
                case OptionRow::KeyRemap:
                    open_keyremap_ = true;
                    break;
                case OptionRow::SmallMemory:
                    snap_.small_memory = !snap_.small_memory;
                    changed_ = true;
                    break;
                default:
                    break;
            }
            break;
        }
        case SDLK_RETURN:
        case SDLK_KP_ENTER:
        case SDLK_SPACE:
            // Enter/Space on the "Define keyboard layouts" row opens the
            // remap sub-screen (§3 row 15, sub_407B9D) exactly like
            // Left/Right does for a cycler row elsewhere in the list — every
            // OTHER row's Enter/Space is a no-op (they are toggles/cyclers,
            // not text-entry, per §3's "Input model": only Left/Right/Enter
            // dispatch the per-row handler, and for a toggle "direction is
            // ignored").
            if (static_cast<OptionRow>(row_) == OptionRow::KeyRemap) {
                open_keyremap_ = true;
                break;
            }
            done_ = true;
            break;
        case SDLK_ESCAPE:
            // Esc leaves without discarding an already-made change — the caller
            // (present_options_screen) decides whether to persist based on
            // changed(), same on Enter or Esc. Only the *screen's* dismissal
            // semantics differ (Back vs Advance) for the app-flow graph.
            done_ = true;
            break;
        default:
            break;
    }
}

void OptionsScreen::draw(SDL_Renderer* ren) const {
    if (!ren) return;

    // Backdrop: the random GLUE<n> picture picked in enter(), full-screen —
    // same convention as the .BM viewer's MAINMENU backdrop (bmscreen.cpp).
    if (assets_) {
        const Sprite& bg = assets_->frontend_pcx(backdrop_);
        if (bg.tex) {
            SDL_FRect d{0, 0, static_cast<float>(bg.w), static_cast<float>(bg.h)};
            SDL_RenderTexture(ren, bg.tex, nullptr, &d);
        } else {
            // Missing backdrop art: clear to a dark panel so text stays legible
            // rather than drawing over stale contents.
            SDL_SetRenderDrawColor(ren, 20, 20, 30, 255);
            SDL_RenderClear(ren);
        }
    }

    if (!font_ || !font_->loaded()) return;

    // No title/header text — CONFIRMED absent from sub_4080DC's body (see
    // options_screen.hpp's file doc); the previous "OPTIONS" header at a
    // guessed (55,20) had no RE citation and has been removed.

    // All 19 rows, in the ORIGINAL's exact order (§3) — including the 8 the
    // port previously hid. Rows the port can't act on (Node Name/Scheme
    // File/Modem/Net Protocol/Adjust Audio) are still drawn, matching the
    // original showing them unconditionally in the same general ink.
    std::string rows[static_cast<int>(OptionRow::kCount)] = {
        std::string("TEAM PLAY: ") + yes_no(snap_.team_play),
        std::string("RANDOM START: ") + yes_no(snap_.random_start),
        std::string("NODE NAME: ") + "(N/A)",
        std::string("CONVEYOR SPEED: ") + conveyor_label(snap_.conveyor_speed_index),
        std::string("STOMPED BOMBS DETONATE: ") + yes_no(snap_.stomped_bombs_detonate),
        std::string("WIN MATCHES BY KILL TOTAL: ") + yes_no(snap_.win_by_kills),
        std::string("GOLD BOMBERMAN: ") + yes_no(snap_.goldman),
        std::string("ENCLOSEMENT DEPTH: ") + enclosement_label(snap_.enclosement_depth),
        std::string("SCHEME FILE: ") +
            (snap_.scheme_filename.empty() ? std::string("(N/A)") : snap_.scheme_filename),
        std::string("PLAY TIME: ") + playtime_label(snap_.playtime_seconds),
        std::string("ASSIGN KEYBOARD PLAYER: ") + yes_no(snap_.assign_keyboards),
        std::string("DISEASES CAN BE DESTROYED: ") + yes_no(snap_.diseases_destroyable),
        std::string("LOST NET PLAYERS REVERT TO AI: ") + yes_no(snap_.lost_net_revert_ai),
        std::string("DISABLE MUSIC DURING GAMEPLAY: ") + yes_no(snap_.disable_game_music),
        std::string("MODEM: P/I/B/#: ") + "(N/A)",
        std::string("DEFINE KEYBOARD LAYOUTS..."),
        std::string("SET DEFAULT NETWORK PROTOCOL: ") + "(N/A)",
        // CONFIRMED (pseudo.c 9280, `getstring((dword_464824==0)+25)`): the
        // label is the INVERSE of the backing value — small_memory==false
        // (Enhanced Memory Model in effect) shows YES.
        std::string("USE ENHANCED MEMORY MODEL: ") + yes_no(!snap_.small_memory),
        std::string("ADJUST AUDIO: ") + "(N/A)",
    };

    for (int i = 0; i < static_cast<int>(OptionRow::kCount); ++i) {
        float y = static_cast<float>(kListY0 + i * kListYStep);
        font_->draw(ren, rows[i], static_cast<float>(kListX), y, kInkR, kInkG, kInkB);
    }

    // The selection cursor — CONFIRMED sub_413BD6: the "cursor1" MISC.ANI
    // sprite at (x-20, row_y), NOT a text recolour (see options_screen.hpp's
    // file doc). row_ is always in [0, kCursorRowCount), so it never lands
    // on the permanently-unreachable row 18.
    if (assets_) {
        Anim cur = resolve_sequence(assets_->misc(), "cursor1");
        if (!cur.steps.empty()) {
            const std::size_t step = anim_step_index(cursor_frame_ / 6, cur.steps.size());
            const Sprite& sp = cur.steps[step];
            if (sp.tex) {
                float cy = static_cast<float>(kListY0 + row_ * kListYStep);
                SDL_FRect d{static_cast<float>(kCursorX - sp.hx), cy - static_cast<float>(sp.hy),
                            static_cast<float>(sp.w), static_cast<float>(sp.h)};
                SDL_RenderTexture(ren, sp.tex, nullptr, &d);
            }
        } else {
            // Missing MISC.ANI: fall back to a plain marker so the selected
            // row stays legible rather than silently losing its cursor.
            float cy = static_cast<float>(kListY0 + row_ * kListYStep);
            font_->draw(ren, ">", static_cast<float>(kCursorX), cy, kInkR, kInkG, kInkB);
        }
    }
}

}  // namespace bomber::game
