#include "bomber/frontend/campaign_screen.hpp"

#include "bomber/ui/dialog_chrome.hpp"  // the shared sub_42DBCC list chrome

namespace bomber::game {

namespace {

// sub_4015C6's empty-glob hint tint (a port stand-in; the pinned error dialog
// ids are cited at the draw site).
constexpr Rgb kHint{160, 160, 160};

}  // namespace

void CampaignFilePicker::enter(const std::filesystem::path& install_root, std::string backdrop) {
    backdrop_ = std::move(backdrop);
    list_.reset();
    done_ = false;
    cancelled_ = false;
    // sub_4015C6 globs through the SAME sub_41404B helper as the *.SCH picker
    // and the help browser, so the glob, the case-insensitive extension match
    // and the PINNED uppercased-filename sort live in ListPicker — this picker
    // used to sort the full paths case-sensitively against the same citation.
    list_.set_header(header());
    list_.glob(install_root, ".CAM");
    list_.measure_rows([this](int i) { return list_.path(i).filename().string(); });
}

std::string CampaignFilePicker::header() const {
    return assets_ ? assets_->getstring(1250, "Select a campaign:")
                   : std::string("Select a campaign:");
}

const std::filesystem::path& CampaignFilePicker::selected() const {
    // @0x42E39A under the @0x42E3A8 range check (see ListPicker::selected). The
    // empty fallback covers only API misuse — a call without done() &&
    // !cancelled() — which no runner performs.
    static const std::filesystem::path kNone;
    const std::filesystem::path* sel = list_.selected();
    return sel != nullptr ? *sel : kNone;
}

void CampaignFilePicker::on_key(SDL_Keycode key, AudioEngine& audio) {
    if (list_.empty()) {
        // The empty-glob acknowledge box (sub_414340): unconditional nav blip on
        // any real key @0x414532, dismissed by Enter/Space/Escape with no sting.
        audio.play(20);
        if (key == SDLK_ESCAPE || key == SDLK_RETURN) {
            done_ = true;
            cancelled_ = true;
        }
        return;
    }
    // THE LIST DIALOG IS SILENT (docs/re/sound-engine.md §8). sub_4015C6 is the
    // *.cam picker; the only sound-bearing functions it can reach are that same
    // sub_414340 box and — through the LOADER sub_401085, not the list —
    // sub_4074A3's SFX-40 load-failure buzz. Its list navigation and its accept
    // make no sound, because the shared list widget makes none.
    //
    // W/S stay as the port's own alias, like the *.SCH picker's.
    const int code = (key == SDLK_W)   ? kListKeyUp
                     : (key == SDLK_S) ? kListKeyDown
                     : (key == SDLK_SPACE)
                         ? kListKeyEnter
                         : list_dialog_key_code(key);
    switch (list_dialog_key(list_.nav(), code, kVisibleRows, list_.count())) {
        case ListDialogAction::Activate:
            done_ = true;
            cancelled_ = false;
            break;
        case ListDialogAction::Cancel:
            done_ = true;
            cancelled_ = true;
            break;
        case ListDialogAction::None:
            break;
    }
}

void CampaignFilePicker::on_mouse_move(float x, float y, bool buttons_held) {
    list_.on_mouse_move(x, y, buttons_held);
}

void CampaignFilePicker::on_mouse_down(float x, float y) {
    switch (list_.on_mouse_down(x, y)) {
        case ListDialogAction::Activate:
            done_ = true;
            cancelled_ = false;
            break;
        case ListDialogAction::Cancel:
            done_ = true;
            cancelled_ = true;
            break;
        case ListDialogAction::None:
            break;
    }
}

void CampaignFilePicker::on_mouse_up(float x, float y) {
    if (list_.on_mouse_up(x, y) == ListDialogAction::Cancel) {
        done_ = true;
        cancelled_ = true;
    }
}

void CampaignFilePicker::draw_backdrop(SDL_Renderer* ren) const {
    if (!assets_) return;
    const Sprite& bg = assets_->frontend_pcx(backdrop_);
    if (bg.tex == nullptr) {
        SDL_SetRenderDrawColor(ren, 20, 20, 30, 255);
        SDL_RenderClear(ren);
        return;
    }
    SDL_FRect d{0, 0, static_cast<float>(bg.w), static_cast<float>(bg.h)};
    SDL_RenderTexture(ren, bg.tex, nullptr, &d);
}

void CampaignFilePicker::draw(SDL_Renderer* ren) const {
    if (!ren) return;
    draw_backdrop(ren);
    if (!font_ || !font_->loaded()) return;
    if (list_.empty()) {
        // sub_4015C6's empty-glob path: the getstring(1215)/getstring(97) error
        // dialog (docs/re/campaign.md §3).
        const std::string err =
            assets_ ? assets_->getstring(1215, "No campaign files found!") : std::string();
        font_->draw(ren, err.empty() ? "No campaign files found!" : err, SDL_FPoint{100.0f, 124.0f},
                    TextStyle{kHint});
        return;
    }
    // sub_4015C6 hands its glob to the SAME sub_41485A -> sub_42DB80 ->
    // sub_42DBCC widget the *.SCH picker and the help browser use, so it gets the
    // same chrome at the same literal (100, 100). This screen used to draw bare
    // "> name" text with no panel and no scrollbar — a port stand-in, and the
    // reason a scrolled campaign list showed no position at all.
    list_.draw(ren, [this](int i) { return list_.path(i).filename().string(); });
}

}  // namespace bomber::game
