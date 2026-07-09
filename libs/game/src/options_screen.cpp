#include "bomber/game/options_screen.hpp"

#include <cstdio>
#include <string>

namespace bomber::game {

namespace {

// CONFIRMED layout (docs/re/results-and-options.md §3): VALUELST
// `745,55,40,22,500` -> x=55, y0=40, ystep=22, colour=500.
constexpr int kHeaderX = 55;
constexpr int kHeaderY = 20;
constexpr int kListX = 55;
constexpr int kListY0 = 40;
constexpr int kListYStep = 22;
constexpr int kHintY = 460;

constexpr Uint8 kInkR = 230, kInkG = 230, kInkB = 210;
constexpr Uint8 kSelR = 255, kSelG = 220, kSelB = 80;   // highlighted row
constexpr Uint8 kHintR = 160, kHintG = 160, kHintB = 160;

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
    auto row_count = static_cast<int>(OptionRow::kCount);
    switch (key) {
        case SDLK_UP:
        case SDLK_W:
            row_ = (row_ + row_count - 1) % row_count;
            audio.play(20);  // nav blip, SOUNDLST 20 — matches present_setup's key table
            break;
        case SDLK_DOWN:
        case SDLK_S:
            row_ = (row_ + 1) % row_count;
            audio.play(20);
            break;
        case SDLK_LEFT:
        case SDLK_RIGHT: {
            audio.play(20);
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
                case OptionRow::DiseasesDestroy:
                    snap_.diseases_destroyable = !snap_.diseases_destroyable;
                    changed_ = true;
                    break;
                case OptionRow::DisableMusic:
                    snap_.disable_game_music = !snap_.disable_game_music;
                    changed_ = true;
                    break;
                case OptionRow::KeyRemap:
                    open_keyremap_ = true;
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
                audio.play(10);
                open_keyremap_ = true;
                break;
            }
            audio.play(10);  // accept sting, SOUNDLST 10
            done_ = true;
            break;
        case SDLK_ESCAPE:
            // Esc leaves without discarding an already-made change — the caller
            // (present_options_screen) decides whether to persist based on
            // changed(), same on Enter or Esc. Only the *screen's* dismissal
            // semantics differ (Back vs Advance) for the app-flow graph.
            audio.play(10);
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

    font_->draw(ren, "OPTIONS", static_cast<float>(kHeaderX), static_cast<float>(kHeaderY), kInkR,
                kInkG, kInkB);

    // In the ORIGINAL's row order (§3); the omitted rows (2/8/10/12/14/16/17/
    // 18, see the header doc) simply are not entries in this array, so the
    // cursor walks only the live subset.
    std::string rows[static_cast<int>(OptionRow::kCount)] = {
        std::string("TEAM PLAY: ") + yes_no(snap_.team_play),
        std::string("RANDOM START: ") + yes_no(snap_.random_start),
        std::string("CONVEYOR SPEED: ") + conveyor_label(snap_.conveyor_speed_index),
        std::string("STOMPED BOMBS DETONATE: ") + yes_no(snap_.stomped_bombs_detonate),
        std::string("WIN MATCHES BY KILL TOTAL: ") + yes_no(snap_.win_by_kills),
        std::string("GOLD BOMBERMAN: ") + yes_no(snap_.goldman),
        std::string("ENCLOSEMENT DEPTH: ") + enclosement_label(snap_.enclosement_depth),
        std::string("PLAY TIME: ") + playtime_label(snap_.playtime_seconds),
        std::string("DISEASES CAN BE DESTROYED: ") + yes_no(snap_.diseases_destroyable),
        std::string("DISABLE MUSIC DURING GAMEPLAY: ") + yes_no(snap_.disable_game_music),
        std::string("DEFINE KEYBOARD LAYOUTS..."),
    };

    for (int i = 0; i < static_cast<int>(OptionRow::kCount); ++i) {
        float y = static_cast<float>(kListY0 + i * kListYStep);
        bool sel = (i == row_);
        Uint8 r = sel ? kSelR : kInkR, g = sel ? kSelG : kInkG, b = sel ? kSelB : kInkB;
        std::string line = (sel ? "> " : "  ") + rows[i];
        font_->draw(ren, line, static_cast<float>(kListX), y, r, g, b);
    }

    font_->draw(ren, "UP/DOWN SELECT   LEFT/RIGHT CHANGE   ENTER/ESC DONE   F1 HELP",
                static_cast<float>(kListX), static_cast<float>(kHintY), kHintR, kHintG, kHintB);
}

}  // namespace bomber::game
