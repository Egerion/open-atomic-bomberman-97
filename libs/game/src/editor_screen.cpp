#include "bomber/game/editor_screen.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>

namespace bomber::game {

namespace {

constexpr Uint8 kInkR = 230, kInkG = 230, kInkB = 210;
constexpr Uint8 kSelR = 255, kSelG = 220, kSelB = 80;
constexpr Uint8 kHintR = 160, kHintG = 160, kHintB = 160;

// §5 CONFIRMED: title getstring(730) at getvalue(810/811/813); rows
// getstring(731..733) at getvalue(815-818) ("; Editor - mainmenu header" /
// "; Editor - mainmenu items"). We do not have MESSAGES.TXT text committed
// (never committed, per CLAUDE.md), so the LABEL TEXT below is our own
// paraphrase — the message ids are cited so a real install's strings can be
// substituted later; only the ids/positions are RE facts.
constexpr int kChooserHeaderX = 50, kChooserHeaderY = 100;   // getvalue(810/811)
constexpr int kChooserItemX = 80, kChooserItemY0 = 140, kChooserItemYStep = 20;  // getvalue(815-818)

}  // namespace

// ---------------------------------------------------------------------------
// SchemeFilePicker — sub_407582 (§5)

void SchemeFilePicker::enter(const std::filesystem::path& schemes_dir, std::string backdrop) {
    backdrop_ = std::move(backdrop);
    entries_.clear();
    row_ = 0;
    done_ = false;
    cancelled_ = false;
    std::error_code ec;
    if (!std::filesystem::is_directory(schemes_dir, ec)) return;
    for (const auto& entry : std::filesystem::directory_iterator(schemes_dir, ec)) {
        if (!entry.is_regular_file()) continue;
        std::string ext = entry.path().extension().string();
        for (auto& c : ext) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        if (ext == ".SCH") entries_.push_back(entry.path());
    }
    std::sort(entries_.begin(), entries_.end());  // qsort'd results, §4's help-browser precedent
}

void SchemeFilePicker::on_key(SDL_Keycode key, AudioEngine& audio) {
    if (entries_.empty()) {
        if (key == SDLK_ESCAPE || key == SDLK_RETURN) { done_ = true; cancelled_ = true; }
        return;
    }
    int count = static_cast<int>(entries_.size());
    switch (key) {
        case SDLK_UP:
        case SDLK_W:
            row_ = (row_ + count - 1) % count;
            audio.play(20);
            break;
        case SDLK_DOWN:
        case SDLK_S:
            row_ = (row_ + 1) % count;
            audio.play(20);
            break;
        case SDLK_RETURN:
        case SDLK_KP_ENTER:
        case SDLK_SPACE:
            audio.play(10);
            done_ = true;
            cancelled_ = false;
            break;
        case SDLK_ESCAPE:
            audio.play(10);
            done_ = true;
            cancelled_ = true;
            break;
        default:
            break;
    }
}

void SchemeFilePicker::draw(SDL_Renderer* ren) const {
    if (!ren) return;
    if (assets_) {
        const Sprite& bg = assets_->frontend_pcx(backdrop_);
        if (bg.tex) {
            SDL_FRect d{0, 0, static_cast<float>(bg.w), static_cast<float>(bg.h)};
            SDL_RenderTexture(ren, bg.tex, nullptr, &d);
        } else {
            SDL_SetRenderDrawColor(ren, 20, 20, 30, 255);
            SDL_RenderClear(ren);
        }
    }
    if (!font_ || !font_->loaded()) return;
    font_->draw(ren, "SELECT A SCHEME", 60.0f, 60.0f, kInkR, kInkG, kInkB);
    if (entries_.empty()) {
        font_->draw(ren, "(no .SCH files found)", 80.0f, 100.0f, kHintR, kHintG, kHintB);
        return;
    }
    for (std::size_t i = 0; i < entries_.size(); ++i) {
        bool sel = (static_cast<int>(i) == row_);
        Uint8 r = sel ? kSelR : kInkR, g = sel ? kSelG : kInkG, b = sel ? kSelB : kInkB;
        std::string line = (sel ? "> " : "  ") + entries_[i].filename().string();
        font_->draw(ren, line, 80.0f, 100.0f + static_cast<float>(i) * 20.0f, r, g, b);
    }
    font_->draw(ren, "UP/DOWN SELECT   ENTER OPEN   ESC CANCEL", 60.0f, 440.0f, kHintR, kHintG, kHintB);
}

// ---------------------------------------------------------------------------
// EditorChooserScreen — sub_403184 (§5)

void EditorChooserScreen::enter(std::string backdrop) { backdrop_ = std::move(backdrop); }

EditorChooserResult EditorChooserScreen::on_key(SDL_Keycode key, AudioEngine& audio) {
    // §5: '1' -> edit existing (sub_4028D2(0), via the *.SCH file picker
    // sub_407582 first); '2' -> new (sub_4028D2(1)); Esc/'Q'/'q' exit; F1
    // (315) help. SFX 20 blip on any key (§5: "SFX 20 blip on any key").
    switch (key) {
        case SDLK_1:
            audio.play(20);
            return EditorChooserResult::EditExisting;
        case SDLK_2:
            audio.play(20);
            return EditorChooserResult::New;
        case SDLK_ESCAPE:
        case SDLK_Q:
            audio.play(20);
            return EditorChooserResult::Exit;
        case SDLK_F1:
            audio.play(20);
            return EditorChooserResult::Help;
        default:
            return EditorChooserResult::None;
    }
}

void EditorChooserScreen::draw(SDL_Renderer* ren) const {
    if (!ren) return;
    if (assets_) {
        const Sprite& bg = assets_->frontend_pcx(backdrop_);
        if (bg.tex) {
            SDL_FRect d{0, 0, static_cast<float>(bg.w), static_cast<float>(bg.h)};
            SDL_RenderTexture(ren, bg.tex, nullptr, &d);
        } else {
            SDL_SetRenderDrawColor(ren, 20, 20, 30, 255);
            SDL_RenderClear(ren);
        }
    }
    if (!font_ || !font_->loaded()) return;

    // getstring(730), our paraphrase (id cited, text not RE'd — see file doc).
    font_->draw(ren, "SCHEME EDITOR", static_cast<float>(kChooserHeaderX),
                static_cast<float>(kChooserHeaderY), kInkR, kInkG, kInkB);

    // getstring(731..733): edit existing / new / exit.
    const char* items[] = {"1) EDIT AN EXISTING SCHEME", "2) NEW SCHEME", "ESC) EXIT"};
    for (int i = 0; i < 3; ++i) {
        float y = static_cast<float>(kChooserItemY0 + i * kChooserItemYStep);
        font_->draw(ren, items[i], static_cast<float>(kChooserItemX), y, kInkR, kInkG, kInkB);
    }
    font_->draw(ren, "F1 HELP", static_cast<float>(kChooserItemX),
                static_cast<float>(kChooserItemY0 + 4 * kChooserItemYStep), kHintR, kHintG, kHintB);
}

// ---------------------------------------------------------------------------
// PowerupRulesScreen — sub_402595 (§5 'P'/'p')

void PowerupRulesScreen::enter(std::vector<assets::sch::PowerupRule>* rows) {
    rows_ = rows;
    row_ = 0;
    done_ = false;
}

void PowerupRulesScreen::on_key(SDL_Keycode key, AudioEngine& audio) {
    if (!rows_ || rows_->empty()) {
        if (key == SDLK_ESCAPE || key == SDLK_RETURN) done_ = true;
        return;
    }
    int count = static_cast<int>(rows_->size());
    switch (key) {
        case SDLK_UP:
        case SDLK_W:
            row_ = (row_ + count - 1) % count;
            audio.play(20);
            break;
        case SDLK_DOWN:
        case SDLK_S:
            row_ = (row_ + 1) % count;
            audio.play(20);
            break;
        case SDLK_B: {
            // Toggle "born with" (bornwith field, §5).
            auto& pr = (*rows_)[static_cast<std::size_t>(row_)];
            pr.born_with = pr.born_with ? 0 : 1;
            audio.play(20);
            break;
        }
        case SDLK_F: {
            // Toggle "forbidden" (§5).
            auto& pr = (*rows_)[static_cast<std::size_t>(row_)];
            pr.forbidden = pr.forbidden ? 0 : 1;
            audio.play(20);
            break;
        }
        case SDLK_O: {
            // Toggle "has override" — a widget detail (the override VALUE's
            // own entry method) is not pinned by §5's body-read summary
            // (only that override rows exist); TODO(RE): keep it minimal —
            // Left/Right nudge the value while override is on.
            auto& pr = (*rows_)[static_cast<std::size_t>(row_)];
            pr.has_override = pr.has_override ? 0 : 1;
            audio.play(20);
            break;
        }
        case SDLK_LEFT:
        case SDLK_RIGHT: {
            auto& pr = (*rows_)[static_cast<std::size_t>(row_)];
            if (pr.has_override) {
                pr.override_value += (key == SDLK_RIGHT) ? 1 : -1;
                if (pr.override_value < 0) pr.override_value = 0;
                audio.play(20);
            }
            break;
        }
        case SDLK_ESCAPE:
        case SDLK_RETURN:
        case SDLK_KP_ENTER:
        case SDLK_P:
            // 'P' re-toggles the sub-editor closed too (§5's 'P'/'p' opens
            // it from the grid screen; symmetric close is our own minimal
            // completion of that gesture, not itself an RE fact).
            audio.play(10);
            done_ = true;
            break;
        default:
            break;
    }
}

void PowerupRulesScreen::draw(SDL_Renderer* ren) const {
    if (!ren) return;
    SDL_SetRenderDrawColor(ren, 15, 15, 25, 255);
    SDL_RenderClear(ren);
    if (!font_ || !font_->loaded()) return;

    font_->draw(ren, "POWERUP RULES", 40.0f, 20.0f, kInkR, kInkG, kInkB);
    if (!rows_) return;
    for (std::size_t i = 0; i < rows_->size(); ++i) {
        const auto& pr = (*rows_)[i];
        char line[128];
        std::snprintf(line, sizeof line, "POWERUP %2d   BORN-WITH:%s  FORBIDDEN:%s  OVERRIDE:%s%d",
                      pr.id, pr.born_with ? "Y" : "N", pr.forbidden ? "Y" : "N",
                      pr.has_override ? "Y=" : "N ", pr.override_value);
        bool sel = (static_cast<int>(i) == row_);
        Uint8 r = sel ? kSelR : kInkR, g = sel ? kSelG : kInkG, b = sel ? kSelB : kInkB;
        std::string prefix = sel ? "> " : "  ";
        font_->draw(ren, prefix + line, 40.0f, 50.0f + static_cast<float>(i) * 20.0f, r, g, b);
    }
    font_->draw(ren, "UP/DOWN ROW   B BORN-WITH   F FORBIDDEN   O OVERRIDE   LEFT/RIGHT VALUE   ENTER/ESC/P DONE",
                40.0f, 460.0f, kHintR, kHintG, kHintB);
}

// ---------------------------------------------------------------------------
// EditorScreen — sub_4028D2 (§5)

void EditorScreen::enter(std::optional<assets::sch::Scheme> initial, std::string backdrop) {
    backdrop_ = std::move(backdrop);
    if (initial)
        grid_.load_from_scheme(*initial);
    else
        grid_.reset(kEditorGridWidth, kEditorGridHeight);  // sub_4028D2(1), "new scheme"
    brush_ = EditorBrush::Blank;
    brush_size_ = 1;
    selected_start_ = 0;
    prompt_kind_ = PromptKind::None;
    prompt_text_.clear();
    editing_powerups_ = false;
    done_ = false;
    save_requested_ = false;
}

void EditorScreen::cycle_brush() {
    // §5: "Tab/Enter/Space cycle it" — blank -> solid -> brick -> blank.
    switch (brush_) {
        case EditorBrush::Blank: brush_ = EditorBrush::Solid; break;
        case EditorBrush::Solid: brush_ = EditorBrush::Brick; break;
        case EditorBrush::Brick: brush_ = EditorBrush::Blank; break;
    }
}

void EditorScreen::start_density_prompt() {
    prompt_kind_ = PromptKind::Density;
    prompt_text_ = std::to_string(grid_.density());
}

void EditorScreen::start_name_prompt() {
    prompt_kind_ = PromptKind::Name;
    prompt_text_ = grid_.name();
}

void EditorScreen::start_save_confirm() { prompt_kind_ = PromptKind::SaveConfirm; }

void EditorScreen::on_mouse_down(int button, int gx, int gy) {
    if (prompting() || editing_powerups_) return;  // modal sub-screens own input
    if (button == SDL_BUTTON_LEFT) {
        // §5: "paint the hovered cell with the current brush" — stamp()
        // subsumes plain paint() at brush_size_==1.
        grid_.stamp(gx, gy, brush_size_, brush_);
    } else if (button == SDL_BUTTON_RIGHT) {
        // §5: "MOVE the currently-selected player-start marker to the
        // hovered cell".
        grid_.move_start(selected_start_, gx, gy);
    }
}

void EditorScreen::on_text_input(const char* text) {
    if (prompt_kind_ != PromptKind::Name || !text) return;
    prompt_text_ += text;
}

void EditorScreen::on_key(SDL_Keycode key, AudioEngine& audio) {
    if (editing_powerups_) return;  // caller routes to powerups_screen() instead

    if (prompt_kind_ == PromptKind::Density) {
        // §5 'D'/'d': "brick-density prompt (text entry, clamped 0-100...)".
        if (key >= SDLK_0 && key <= SDLK_9) {
            if (prompt_text_ == "0") prompt_text_.clear();
            prompt_text_ += static_cast<char>('0' + (key - SDLK_0));
        } else if (key == SDLK_BACKSPACE && !prompt_text_.empty()) {
            prompt_text_.pop_back();
        } else if (key == SDLK_RETURN || key == SDLK_KP_ENTER) {
            grid_.set_density(prompt_text_.empty() ? 0 : std::atoi(prompt_text_.c_str()));
            prompt_kind_ = PromptKind::None;
            audio.play(10);
        } else if (key == SDLK_ESCAPE) {
            prompt_kind_ = PromptKind::None;  // discard the in-progress edit
            audio.play(10);
        }
        return;
    }
    if (prompt_kind_ == PromptKind::Name) {
        // §5 'N'/'n': the -N scheme-name prompt. Text characters arrive via
        // on_text_input (SDL_EVENT_TEXT_INPUT); this handles control keys.
        if (key == SDLK_BACKSPACE && !prompt_text_.empty()) {
            prompt_text_.pop_back();
        } else if (key == SDLK_RETURN || key == SDLK_KP_ENTER) {
            grid_.set_name(prompt_text_);
            prompt_kind_ = PromptKind::None;
            audio.play(10);
        } else if (key == SDLK_ESCAPE) {
            prompt_kind_ = PromptKind::None;
            audio.play(10);
        }
        return;
    }
    if (prompt_kind_ == PromptKind::SaveConfirm) {
        // §5: exit plays a save-changes confirm (getstring(735)) before
        // writing through sch::write(). 'Y'/Enter save, 'N'/Esc discard.
        if (key == SDLK_Y || key == SDLK_RETURN || key == SDLK_KP_ENTER) {
            save_requested_ = true;
            done_ = true;
            audio.play(10);
        } else if (key == SDLK_N || key == SDLK_ESCAPE) {
            save_requested_ = false;
            done_ = true;
            audio.play(10);
        }
        return;
    }

    // Normal grid-editing input (§5's documented key switch).
    switch (key) {
        case SDLK_1: brush_ = EditorBrush::Blank; audio.play(20); break;
        case SDLK_2: brush_ = EditorBrush::Solid; audio.play(20); break;
        case SDLK_3: brush_ = EditorBrush::Brick; audio.play(20); break;
        case SDLK_TAB:
        case SDLK_RETURN:
        case SDLK_KP_ENTER:
        case SDLK_SPACE:
            cycle_brush();
            audio.play(20);
            break;
        case SDLK_F:
            // Ctrl+F (raw code 6, §5) — SDL reports Ctrl+letter as the plain
            // letter keycode with KMOD_CTRL set; the caller (game_app.cpp)
            // is expected to gate this case on the Ctrl modifier before
            // calling on_key for 'F', mirroring how Ctrl+E is gated in
            // present_menu. Kept unconditional here since EditorScreen has
            // no access to the modifier state in this signature — see the
            // caller's own Ctrl check.
            grid_.flood_fill(brush_);
            audio.play(10);
            break;
        case SDLK_EQUALS:
        case SDLK_KP_PLUS:
            selected_start_ = (selected_start_ + 1) % kEditorMaxStarts;
            audio.play(20);
            break;
        case SDLK_MINUS:
        case SDLK_KP_MINUS:
            selected_start_ = (selected_start_ + kEditorMaxStarts - 1) % kEditorMaxStarts;
            audio.play(20);
            break;
        case SDLK_T:
            grid_.toggle_start_team(selected_start_);
            audio.play(20);
            break;
        case SDLK_D:
            start_density_prompt();
            audio.play(20);
            break;
        case SDLK_N:
            start_name_prompt();
            audio.play(20);
            break;
        case SDLK_P:
            editing_powerups_ = true;
            powerups_screen_.enter(&grid_.powerups());
            audio.play(20);
            break;
        case SDLK_ESCAPE:
        case SDLK_Q:
            start_save_confirm();
            audio.play(20);
            break;
        default:
            break;
    }
}

void EditorScreen::draw(SDL_Renderer* ren) const {
    if (!ren) return;

    if (editing_powerups_) {
        powerups_screen_.draw(ren);
        return;
    }

    if (assets_) {
        const Sprite& bg = assets_->frontend_pcx(backdrop_);
        if (bg.tex) {
            SDL_FRect d{0, 0, static_cast<float>(bg.w), static_cast<float>(bg.h)};
            SDL_RenderTexture(ren, bg.tex, nullptr, &d);
        } else {
            SDL_SetRenderDrawColor(ren, 20, 20, 30, 255);
            SDL_RenderClear(ren);
        }
    }

    // The tile grid: §5 draws "tile %u solid"/"tile %u brick" sequences; we
    // do not have those ANI sequences wired into AssetStore for this screen
    // yet (TODO(RE): editor-canvas art), so cells are flat-colour swatches —
    // faithful to the CELL DATA (paint state) if not the original bitmap art.
    for (int y = 0; y < grid_.height(); ++y) {
        for (int x = 0; x < grid_.width(); ++x) {
            SDL_FRect cell{static_cast<float>(kOriginX + x * kCellSize),
                           static_cast<float>(kOriginY + y * kCellSize),
                           static_cast<float>(kCellSize - 1), static_cast<float>(kCellSize - 1)};
            switch (grid_.cell(x, y)) {
                case EditorBrush::Solid: SDL_SetRenderDrawColor(ren, 120, 120, 130, 255); break;
                case EditorBrush::Brick: SDL_SetRenderDrawColor(ren, 150, 90, 40, 255); break;
                default: SDL_SetRenderDrawColor(ren, 40, 90, 40, 255); break;
            }
            SDL_RenderFillRect(ren, &cell);
        }
    }

    // Player-start markers (§5: slot number in the slot colour, plus a
    // teamring ANI for the team flag — we don't have per-slot colour/ANI
    // wired here yet, TODO(RE): use sub_41672F's colour + the teamring%u
    // sprite once AssetStore exposes them for this screen). A numbered
    // marker with a distinct outline for team members stands in for now.
    for (int i = 0; i < kEditorMaxStarts; ++i) {
        const EditorStart& st = grid_.start(i);
        SDL_FRect marker{static_cast<float>(kOriginX + st.x * kCellSize + 4),
                         static_cast<float>(kOriginY + st.y * kCellSize + 4),
                         static_cast<float>(kCellSize - 8), static_cast<float>(kCellSize - 8)};
        bool sel = (i == selected_start_);
        if (st.team)
            SDL_SetRenderDrawColor(ren, 255, 255, 0, 255);  // team flag outline stand-in
        else
            SDL_SetRenderDrawColor(ren, 255, 255, 255, 255);
        SDL_RenderRect(ren, &marker);
        if (sel) {
            SDL_FRect inner{marker.x + 2, marker.y + 2, marker.w - 4, marker.h - 4};
            SDL_SetRenderDrawColor(ren, 255, 220, 80, 255);
            SDL_RenderRect(ren, &inner);
        }
    }

    if (!font_ || !font_->loaded()) return;

    const char* brush_name = brush_ == EditorBrush::Solid ? "SOLID"
                             : brush_ == EditorBrush::Brick ? "BRICK"
                                                             : "BLANK";
    char status[160];
    std::snprintf(status, sizeof status,
                  "BRUSH:%s(%d)  START:%d%s  DENSITY:%d  NAME:%s", brush_name, brush_size_,
                  selected_start_, grid_.start(selected_start_).team ? "[T]" : "",
                  grid_.density(), grid_.name().empty() ? "(none)" : grid_.name().c_str());
    font_->draw(ren, status, 20.0f, static_cast<float>(kOriginY + grid_.height() * kCellSize + 8),
                kInkR, kInkG, kInkB);
    font_->draw(ren,
                "1/2/3 BRUSH  TAB CYCLE  CTRL+F FILL  +/- START  T TEAM  D DENSITY  N NAME  P POWERUPS  ESC SAVE/EXIT",
                20.0f, static_cast<float>(kOriginY + grid_.height() * kCellSize + 28), kHintR, kHintG,
                kHintB);

    if (prompt_kind_ == PromptKind::Density || prompt_kind_ == PromptKind::Name) {
        SDL_SetRenderDrawBlendMode(ren, SDL_BLENDMODE_BLEND);
        SDL_SetRenderDrawColor(ren, 0, 0, 0, 200);
        SDL_FRect box{160, 200, 320, 60};
        SDL_RenderFillRect(ren, &box);
        std::string label = (prompt_kind_ == PromptKind::Density ? "DENSITY (0-100): " : "NAME: ") +
                             prompt_text_;
        font_->draw(ren, label, 176.0f, 222.0f, kSelR, kSelG, kSelB);
    } else if (prompt_kind_ == PromptKind::SaveConfirm) {
        // getstring(735), §5 — text paraphrased (id cited, not RE'd verbatim).
        SDL_SetRenderDrawBlendMode(ren, SDL_BLENDMODE_BLEND);
        SDL_SetRenderDrawColor(ren, 0, 0, 0, 200);
        SDL_FRect box{140, 200, 360, 60};
        SDL_RenderFillRect(ren, &box);
        font_->draw(ren, "SAVE CHANGES TO THIS SCHEME?  Y/ENTER = YES   N/ESC = NO", 156.0f, 222.0f,
                    kSelR, kSelG, kSelB);
    }
}

}  // namespace bomber::game
