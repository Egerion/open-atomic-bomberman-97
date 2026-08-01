#include "bomber/frontend/campaign_screen.hpp"

#include <algorithm>
#include <cctype>

#include "bomber/ui/dialog_chrome.hpp"  // the shared sub_42DBCC list chrome

namespace bomber::game {

namespace {

// The general white ink every row is drawn in — the selected one included,
// because sub_442C28 LIGHTENS the band under the text rather than inverting it,
// so no per-row recolour and no "> " marker are needed.
constexpr Uint8 kInkR = 255, kInkG = 255, kInkB = 255;
constexpr Uint8 kHintR = 160, kHintG = 160, kHintB = 160;

bool has_cam_extension(const std::filesystem::path& p) {
    std::string ext = p.extension().string();
    for (auto& c : ext) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return ext == ".CAM";
}

}  // namespace

void CampaignFilePicker::enter(const std::filesystem::path& install_root, std::string backdrop) {
    backdrop_ = std::move(backdrop);
    entries_.clear();
    nav_ = ListDialogNav{};
    item_w_ = 0.0f;
    pressed_ = ListDialogWidget::None;
    done_ = false;
    cancelled_ = false;
    std::error_code ec;
    if (!std::filesystem::is_directory(install_root, ec)) return;
    for (const auto& entry : std::filesystem::directory_iterator(install_root, ec)) {
        if (!entry.is_regular_file()) continue;
        if (has_cam_extension(entry.path())) entries_.push_back(entry.path());
    }
    std::sort(entries_.begin(), entries_.end());  // sub_41404B qsorts its glob results
    // sub_42FEF0 @0x42DC16 — the widest ITEM alone, cached so the mouse
    // handlers hit-test exactly the layout draw() paints.
    if (font_)
        for (const auto& e : entries_)
            item_w_ = std::max(item_w_, static_cast<float>(font_->measure(e.filename().string())));
}

std::string CampaignFilePicker::header() const {
    return assets_ ? assets_->getstring(1250, "Select a campaign:")
                   : std::string("Select a campaign:");
}

ListDialogGeometry CampaignFilePicker::layout() const {
    return list_dialog_layout_for(*font_, header(), 100.0f, 100.0f, item_w_, kVisibleRows,
                                  static_cast<int>(entries_.size()), nav_.top_row);
}

void CampaignFilePicker::on_key(SDL_Keycode key, AudioEngine& audio) {
    if (entries_.empty()) {
        // The empty-glob acknowledge box (sub_414340): unconditional nav blip on
        // any real key @0x414532, dismissed by Enter/Space/Escape with no sting.
        audio.play(20);
        if (key == SDLK_ESCAPE || key == SDLK_RETURN) { done_ = true; cancelled_ = true; }
        return;
    }
    // THE LIST DIALOG IS SILENT (docs/re/sound-engine.md §8). sub_4015C6 is the
    // *.cam picker; the only sound-bearing functions it can reach are that same
    // sub_414340 box and — through the LOADER sub_401085, not the list —
    // sub_4074A3's SFX-40 load-failure buzz. Its list navigation and its accept
    // make no sound, because the shared list widget makes none.
    const int count = static_cast<int>(entries_.size());
    // W/S stay as the port's own alias, like the *.SCH picker's.
    const int code = (key == SDLK_W)   ? kListKeyUp
                     : (key == SDLK_S) ? kListKeyDown
                     : (key == SDLK_SPACE)
                         ? kListKeyEnter
                         : list_dialog_key_code(key);
    switch (list_dialog_key(nav_, code, kVisibleRows, count)) {
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
    if (entries_.empty() || !font_ || !font_->loaded() || buttons_held) return;
    list_dialog_mouse_move(nav_, list_dialog_hit_for(*font_, layout(), kVisibleRows, x, y),
                           static_cast<int>(entries_.size()));
}

void CampaignFilePicker::on_mouse_down(float x, float y) {
    if (entries_.empty() || !font_ || !font_->loaded()) return;
    const ListDialogGeometry g = layout();
    const ListDialogHit hit = list_dialog_hit_for(*font_, g, kVisibleRows, x, y);
    pressed_ = hit.widget;
    switch (list_dialog_mouse_down(nav_, g, hit, kVisibleRows,
                                   static_cast<int>(entries_.size()), static_cast<int>(y))) {
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
    if (entries_.empty() || !font_ || !font_->loaded()) return;
    const ListDialogHit hit = list_dialog_hit_for(*font_, layout(), kVisibleRows, x, y);
    const ListDialogWidget was = pressed_;
    pressed_ = ListDialogWidget::None;
    if (list_dialog_mouse_up(hit, was) == ListDialogAction::Cancel) {
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
    if (entries_.empty()) {
        // sub_4015C6's empty-glob path: the getstring(1215)/getstring(97) error
        // dialog (docs/re/campaign.md §3).
        const std::string err =
            assets_ ? assets_->getstring(1215, "No campaign files found!") : std::string();
        font_->draw(ren, err.empty() ? "No campaign files found!" : err, 100.0f, 124.0f, kHintR,
                    kHintG, kHintB);
        return;
    }
    // sub_4015C6 hands its glob to the SAME sub_41485A -> sub_42DB80 ->
    // sub_42DBCC widget the *.SCH picker and the help browser use, so it gets the
    // same chrome at the same literal (100, 100). This screen used to draw bare
    // "> name" text with no panel and no scrollbar — a port stand-in, and the
    // reason a scrolled campaign list showed no position at all.
    //
    // sub_42FEF0 @0x42DC16: the widest ITEM drives the width (measured at
    // enter()); the widget folds the title in itself, so it must NOT be pre-maxed.
    const int count = static_cast<int>(entries_.size());
    const int last = std::min(count, nav_.top_row + kVisibleRows);
    const ListDialogLayout lay = draw_list_dialog(ren, *font_, header(), 100.0f, 100.0f, item_w_,
                                                  kVisibleRows, count, nav_.top_row);
    for (int i = nav_.top_row; i < last; ++i) {
        const int vi = i - nav_.top_row;
        const float ty = lay.item_y0 + static_cast<float>(vi) * lay.item_h;
        // The selection band follows the highlight OFFSET (@0x42E656).
        if (vi == nav_.highlight) draw_list_selection(ren, lay, vi);
        font_->draw(ren, entries_[static_cast<std::size_t>(i)].filename().string(), lay.item_x, ty,
                    kInkR, kInkG, kInkB);
    }
}

}  // namespace bomber::game
