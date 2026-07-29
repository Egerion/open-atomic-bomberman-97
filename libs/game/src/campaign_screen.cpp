#include "bomber/game/campaign_screen.hpp"

#include <algorithm>
#include <cctype>

#include "bomber/game/dialog_chrome.hpp"  // the shared sub_42DBCC list chrome

namespace bomber::game {

namespace {
// The general white ink every row of this widget is drawn in — the selected
// one included, because sub_442C28 lightens the band UNDER the text rather
// than inverting it (dialog_chrome.hpp). The old kSel*/"> " marker pair went
// with the bare-text stand-in this screen used to draw.
constexpr Uint8 kInkR = 255, kInkG = 255, kInkB = 255;
constexpr Uint8 kHintR = 160, kHintG = 160, kHintB = 160;
}  // namespace

void CampaignFilePicker::enter(const std::filesystem::path& install_root, std::string backdrop) {
    backdrop_ = std::move(backdrop);
    entries_.clear();
    row_ = 0;
    top_ = 0;
    done_ = false;
    cancelled_ = false;
    std::error_code ec;
    if (!std::filesystem::is_directory(install_root, ec)) return;
    for (const auto& entry : std::filesystem::directory_iterator(install_root, ec)) {
        if (!entry.is_regular_file()) continue;
        std::string ext = entry.path().extension().string();
        for (auto& c : ext) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        if (ext == ".CAM") entries_.push_back(entry.path());
    }
    std::sort(entries_.begin(), entries_.end());  // sub_41404B qsorts its glob results
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
    int count = static_cast<int>(entries_.size());
    switch (key) {
        case SDLK_UP:
        case SDLK_W:
            row_ = (row_ + count - 1) % count;
            break;
        case SDLK_DOWN:
        case SDLK_S:
            row_ = (row_ + 1) % count;
            break;
        case SDLK_RETURN:
        case SDLK_KP_ENTER:
        case SDLK_SPACE:
            done_ = true;
            cancelled_ = false;
            break;
        case SDLK_ESCAPE:
            done_ = true;
            cancelled_ = true;
            break;
        default:
            break;
    }
    if (row_ < top_) top_ = row_;
    if (row_ >= top_ + kVisibleRows) top_ = row_ - kVisibleRows + 1;
}

void CampaignFilePicker::draw(SDL_Renderer* ren) const {
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
    // sub_41485A's list dialog, header getstring(1250) ("Select a campaign:",
    // docs/re/campaign.md §3) in the general white ink.
    const std::string header =
        assets_ ? assets_->getstring(1250, "Select a campaign:") : std::string("Select a campaign:");
    if (entries_.empty()) {
        // sub_4015C6's empty-glob path: getstring(1215)/getstring(97) error
        // dialog (docs/re/campaign.md §3's "shows an error dialog instead").
        const std::string err =
            assets_ ? assets_->getstring(1215, "No campaign files found!") : std::string();
        font_->draw(ren, err.empty() ? "No campaign files found!" : err, 100.0f, 124.0f, kHintR,
                    kHintG, kHintB);
        return;
    }
    // sub_4015C6 hands its glob to the SAME sub_41485A -> sub_42DB80 ->
    // sub_42DBCC widget the *.SCH picker and the help browser use, so it gets
    // the same chrome at the same literal (100, 100): grey panel, title strip,
    // bevelled item frame, scrollbar and "Done" button. This screen used to
    // draw bare "> name" text on the backdrop with no panel and no scrollbar —
    // a port stand-in, and the reason a scrolled campaign list showed no
    // position at all. Laid out exactly like SchemeFilePicker::draw.
    const int count = static_cast<int>(entries_.size());
    const int last = std::min(count, top_ + kVisibleRows);

    // sub_42FEF0 @0x42DC16: the widest ITEM drives the width; the widget folds
    // the title in itself, so it must NOT be pre-maxed here.
    float item_w = 0.0f;
    for (const auto& e : entries_)
        item_w = std::max(item_w, static_cast<float>(font_->measure(e.filename().string())));

    const ListDialogLayout lay =
        draw_list_dialog(ren, *font_, header, 100.0f, 100.0f, item_w, kVisibleRows, count, top_);
    for (int i = top_; i < last; ++i) {
        const int vi = i - top_;
        const float ty = lay.item_y0 + static_cast<float>(vi) * lay.item_h;
        // sub_442C28 LIGHTENS the selected row instead of inverting it, so
        // every row keeps the same ink and the "> " marker is not needed.
        if (i == row_) draw_list_selection(ren, lay, vi);
        font_->draw(ren, entries_[static_cast<std::size_t>(i)].filename().string(), lay.item_x, ty,
                    kInkR, kInkG, kInkB);
    }
}

}  // namespace bomber::game
