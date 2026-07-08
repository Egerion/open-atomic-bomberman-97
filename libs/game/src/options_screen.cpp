#include "bomber/game/options_screen.hpp"

#include <cstdio>
#include <string>

namespace bomber::game {

namespace {

// TODO(RE): none of the coordinates/colours below are pinned to a getvalue()
// id — the real Options/game-type screen's layout is not in docs/re/. These
// are a clean-room minimal layout only, sized to sit comfortably over a
// GLUE<n> backdrop the way present_setup's documented player-list column
// does (docs/re/setup-screens.md: list at x=70, y0=170, ystep=24).
constexpr int kHeaderX = 70;
constexpr int kHeaderY = 120;
constexpr int kListX = 70;
constexpr int kListY0 = 190;
constexpr int kListYStep = 40;
constexpr int kHintY = 420;

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

}  // namespace

void OptionsScreen::enter(bool team_play, int conveyor_speed_index, std::string backdrop) {
    row_ = 0;
    done_ = false;
    changed_ = false;
    team_play_ = team_play;
    conveyor_speed_index_ = conveyor_speed_index;
    if (conveyor_speed_index_ < 0) conveyor_speed_index_ = 0;
    if (conveyor_speed_index_ > 2) conveyor_speed_index_ = 2;
    // Random GLUE<n> backdrop (docs/re/setup-screens.md "Backdrop — a RANDOM
    // glue picture", CONFIRMED sub_4148E5) — picked by the caller's shared
    // pick_glue() (GameApp), the same helper present_setup/present_map_select
    // use, so there is exactly one presentation LCG for this pick.
    backdrop_ = std::move(backdrop);
}

void OptionsScreen::on_key(SDL_Keycode key, AudioEngine& audio) {
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
                    team_play_ = !team_play_;
                    changed_ = true;
                    break;
                case OptionRow::ConveyorSpeed: {
                    int v = conveyor_speed_index_ + dir;
                    if (v < 0) v = 2;
                    if (v > 2) v = 0;
                    conveyor_speed_index_ = v;
                    changed_ = true;
                    break;
                }
                default:
                    break;
            }
            break;
        }
        case SDLK_RETURN:
        case SDLK_KP_ENTER:
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

    struct Row {
        std::string label;
    };
    Row rows[static_cast<int>(OptionRow::kCount)] = {
        {std::string("TEAM PLAY: ") + (team_play_ ? "ON" : "OFF")},
        {std::string("CONVEYOR SPEED: ") + conveyor_label(conveyor_speed_index_)},
    };

    for (int i = 0; i < static_cast<int>(OptionRow::kCount); ++i) {
        float y = static_cast<float>(kListY0 + i * kListYStep);
        bool sel = (i == row_);
        Uint8 r = sel ? kSelR : kInkR, g = sel ? kSelG : kInkG, b = sel ? kSelB : kInkB;
        std::string line = (sel ? "> " : "  ") + rows[i].label;
        font_->draw(ren, line, static_cast<float>(kListX), y, r, g, b);
    }

    font_->draw(ren, "UP/DOWN SELECT   LEFT/RIGHT CHANGE   ENTER/ESC DONE   F1 HELP",
                static_cast<float>(kListX), static_cast<float>(kHintY), kHintR, kHintG, kHintB);
}

}  // namespace bomber::game
