#include "bomber/ui/list_picker.hpp"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <system_error>

#include "bomber/ui/dialog_chrome.hpp"

namespace bomber::game {

namespace {

// Every original list-modal row draws in byte_49D38F white — the identical ink
// at the help browser's, the *.SCH picker's and the *.cam picker's (100, 100)
// call sites (results-and-options.md §1/§4/§5).
constexpr Rgb kRowInk{255, 255, 255};

// The pinned window origin: both RE'd sub_42DBCC callers push the literal
// (100, 100) — the window is NOT centred.
constexpr float kListX = 100.0f;
constexpr float kListY = 100.0f;

}  // namespace

std::string upper_ascii(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return s;
}

void ListPicker::reset() {
    entries_.clear();
    nav_ = ListDialogNav{};
    item_w_ = 0.0f;
    pressed_ = ListDialogWidget::None;
}

void ListPicker::glob(const std::filesystem::path& dir, const std::string& upper_ext) {
    std::error_code ec;
    if (!std::filesystem::is_directory(dir, ec)) return;
    for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
        if (entry.is_regular_file() && upper_ascii(entry.path().extension().string()) == upper_ext)
            entries_.push_back(entry.path());
    }
    // The PINNED ordering (see the header): strupr the bare filename, THEN a
    // plain strcmp qsort. Sorting the whole std::filesystem::path instead —
    // case-sensitively — is the drift two of the three clones had.
    std::sort(entries_.begin(), entries_.end(), [](const auto& a, const auto& b) {
        return upper_ascii(a.filename().string()) < upper_ascii(b.filename().string());
    });
}

void ListPicker::measure_rows(const std::function<std::string(int)>& row_text) {
    item_w_ = 0.0f;
    if (font_ == nullptr) return;
    for (int i = 0; i < count(); ++i)
        item_w_ = std::max(item_w_, static_cast<float>(font_->measure(row_text(i))));
}

const std::filesystem::path* ListPicker::selected() const {
    const int sel = nav_.top_row + nav_.highlight;      // @0x42E39A
    if (sel < 0 || sel >= count()) return nullptr;      // @0x42E3A8
    return &entries_[static_cast<std::size_t>(sel)];
}

ListDialogGeometry ListPicker::layout() const {
    return list_dialog_layout_for(
        *font_, ListDialogSpec{header_, kListX, kListY, item_w_, kVisibleRows, count(),
                               nav_.top_row});
}

bool ListPicker::hit_testable() const {
    return !entries_.empty() && font_ != nullptr && font_->loaded();
}

void ListPicker::on_mouse_move(float x, float y, bool buttons_held) {
    if (!hit_testable()) return;
    if (buttons_held) return;  // @0x4330A0: the enter id needs an idle mouse
    list_dialog_mouse_move(nav_, list_dialog_hit_for(*font_, layout(), kVisibleRows, {x, y}),
                           count());
}

ListDialogAction ListPicker::on_mouse_down(float x, float y) {
    if (!hit_testable()) return ListDialogAction::None;
    const ListDialogGeometry g = layout();
    const ListDialogHit hit = list_dialog_hit_for(*font_, g, kVisibleRows, {x, y});
    pressed_ = hit.widget;
    return list_dialog_mouse_down(nav_, g, hit, kVisibleRows, count(), static_cast<int>(y));
}

ListDialogAction ListPicker::on_mouse_up(float x, float y) {
    if (!hit_testable()) return ListDialogAction::None;
    const ListDialogHit hit = list_dialog_hit_for(*font_, layout(), kVisibleRows, {x, y});
    const ListDialogWidget was = pressed_;
    pressed_ = ListDialogWidget::None;
    return list_dialog_mouse_up(hit, was);
}

void ListPicker::draw(SDL_Renderer* ren, const std::function<std::string(int)>& row_text) const {
    const int last = std::min(count(), nav_.top_row + kVisibleRows);
    const ListDialogLayout lay =
        draw_list_dialog(DialogPen{ren, *font_}, ListDialogSpec{header_, kListX, kListY, item_w_,
                                                                kVisibleRows, count(),
                                                                nav_.top_row});
    for (int i = nav_.top_row; i < last; ++i) {
        const int vi = i - nav_.top_row;
        const float ty = lay.item_y0 + static_cast<float>(vi) * lay.item_h;
        // sub_442C28 LIGHTENS the selected row rather than inverting it, so
        // every row keeps the same ink either way.
        if (vi == nav_.highlight) draw_list_selection(ren, lay, vi);
        font_->draw(ren, row_text(i), SDL_FPoint{lay.item_x, ty}, TextStyle{kRowInk});
    }
}

}  // namespace bomber::game
