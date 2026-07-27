#include "bomber/game/editor_screen.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>

#include "bomber/game/dialog_chrome.hpp"

namespace bomber::game {

namespace {

// Screen inks, pinned via the byte_495390 RGB555-LUT decode (docs/re/
// results-and-options.md §1 "screen-ink byte globals"): byte_49D38F =
// white (the general draw ink every editor label uses), byte_49D37A =
// yellow (the powerup-name column), byte_497F8F = cyan (the powerup
// sub-editor's header). kSel/kHint are our own cursor/hint tints (the
// original has no keyboard cursor — rows are mouse buttons).
constexpr Uint8 kInkR = 255, kInkG = 255, kInkB = 255;    // byte_49D38F
constexpr Uint8 kNameR = 252, kNameG = 248, kNameB = 88;  // byte_49D37A
constexpr Uint8 kHeadR = 96, kHeadG = 252, kHeadB = 252;  // byte_497F8F
constexpr Uint8 kSelR = 255, kSelG = 220, kSelB = 80;
constexpr Uint8 kHintR = 160, kHintG = 160, kHintB = 160;
// byte_49A390 — sub_407582's empty-glob error ink (LUT offset 0x5000 -> idx
// 248), the same dark red the main-menu quit prompt uses.
constexpr Uint8 kErrR = 164, kErrG = 0, kErrB = 0;

// §5 CONFIRMED: title getstring(730) at getvalue(810/811/813); rows
// getstring(731..733) at getvalue(815-818) ("; Editor - mainmenu header" /
// "; Editor - mainmenu items"). We do not have MESSAGES.TXT text committed
// (never committed, per CLAUDE.md), so the LABEL TEXT below is our own
// paraphrase — the message ids are cited so a real install's strings can be
// substituted later; only the ids/positions are RE facts.
constexpr int kChooserHeaderX = 50, kChooserHeaderY = 100;  // getvalue(810/811)
constexpr int kChooserItemX = 80, kChooserItemY0 = 140,
              kChooserItemYStep = 20;  // getvalue(815-818)

// sub_412A3B — the strupr the glob helper runs over every matched filename
// before sorting (and that sub_407582 runs again over the picked value).
std::string upper(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return s;
}

}  // namespace

// ---------------------------------------------------------------------------
// SchemeFilePicker — sub_407582 (§5)

void SchemeFilePicker::enter(const std::filesystem::path& schemes_dir, std::string backdrop) {
    backdrop_ = std::move(backdrop);
    entries_.clear();
    names_.clear();
    row_ = 0;
    top_ = 0;
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
    // ORDERING, PINNED: sub_41404B uppercases EVERY globbed name first
    // (@0x414146, a sub_412A3B/strupr pass over the whole array) and only then
    // qsorts it (@0x41415D) with the comparator at 0x41400F, which is a plain
    // sub_451F10/strcmp on the two char*. So the sort key is the UPPERCASED
    // BARE FILENAME — the ": <scheme name>" suffix is appended afterwards, by
    // sub_407582's own reformat loop, and never participates.
    std::sort(entries_.begin(), entries_.end(), [](const auto& a, const auto& b) {
        return upper(a.filename().string()) < upper(b.filename().string());
    });
    // sub_407582 pre-reads each file's embedded -N name (sub_404BE9) for the
    // second column. An unreadable/corrupt file falls back to getstring(727),
    // exactly as a file with no -N line does (sub_404BE9 seeds that default
    // into its buffer before it even opens the file).
    names_.reserve(entries_.size());
    for (const auto& p : entries_) {
        std::string n;
        try {
            n = assets::sch::load(p).name;
        } catch (const std::exception&) {  // NOLINT(bugprone-empty-catch)
            // Deliberate: an unreadable/corrupt file keeps the getstring(727)
            // default (sub_407582's behavior — see the function doc).
        }
        names_.push_back(std::move(n));
    }
}

void SchemeFilePicker::on_key(SDL_Keycode key, AudioEngine& audio) {
    if (entries_.empty()) {
        // The empty-glob acknowledge box is sub_414340's own key loop: a nav
        // blip on ANY real key, closing only on Enter(13)/Space(32)/Esc(27).
        audio.play(20);
        if (key == SDLK_ESCAPE || key == SDLK_RETURN || key == SDLK_KP_ENTER || key == SDLK_SPACE) {
            done_ = true;
            cancelled_ = true;
        }
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
        default: break;
    }
    // Keep the cursor inside the kVisibleRows scroll window (sub_42DBCC's
    // list scrolls; our window follows the cursor).
    if (row_ < top_) top_ = row_;
    if (row_ >= top_ + kVisibleRows) top_ = row_ - kVisibleRows + 1;
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
    if (entries_.empty()) {
        // sub_407582's empty-glob branch @0x4076CA: instead of the list it
        // raises sub_414340 with getstring(95) "NOTE!" over getstring(720)
        // "No Scheme files found!", ink byte_49A390 = (164,0,0) dark red.
        // This lives in the picker rather than in each caller because in the
        // original it is the same ONE routine that both entry points call.
        const std::string top = assets_ ? assets_->getstring(95, "NOTE!") : std::string("NOTE!");
        const std::string bottom = assets_ ? assets_->getstring(720, "No Scheme files found!")
                                           : std::string("No Scheme files found!");
        const std::string ok = assets_ ? assets_->getstring(27, " Ok ") : std::string(" Ok ");
        draw_acknowledge_dialog(ren, *font_, assets_ ? &assets_->frontend_pcx("WINZ") : nullptr,
                                top, bottom, ok, kErrR, kErrG, kErrB);
        return;
    }
    // The list dialog itself: sub_407582 @0x407641 pushes the LITERAL pair
    // (100, 100) and getstring(721) into sub_41485A -> sub_42DB80 ->
    // sub_42DBCC, in the general white ink (byte_49D38F | 0x10000). Not
    // centred, and not bare text on the backdrop: draw_list_dialog carries the
    // whole pinned chrome (grey panel, bevels, title strip, scrollbar, "Done"
    // button) — see list_dialog_geometry.hpp.
    const std::string header = assets_ ? assets_->getstring(721, "Available Scheme Files:")
                                       : std::string("Available Scheme Files:");
    const int count = static_cast<int>(entries_.size());
    const int last = std::min(count, top_ + kVisibleRows);

    // sub_42FEF0 @0x42DC16: the widest ITEM row drives the width. The title is
    // folded in by the widget itself, so it must NOT be pre-maxed here.
    float item_w = 0.0f;
    for (int i = 0; i < count; ++i)
        item_w = std::max(item_w, static_cast<float>(font_->measure(row_text(i))));

    const ListDialogLayout lay =
        draw_list_dialog(ren, *font_, header, 100.0f, 100.0f, item_w, kVisibleRows, top_);
    for (int i = top_; i < last; ++i) {
        const int vi = i - top_;
        const float ty = lay.item_y0 + static_cast<float>(vi) * lay.item_h;
        // The selected row is LIGHTENED under unchanged text (sub_442C28), not
        // inverted — so the ink is the same for every row.
        if (i == row_) draw_list_selection(ren, lay, vi);
        font_->draw(ren, row_text(i), lay.item_x, ty, kInkR, kInkG, kInkB);
    }
}

std::string SchemeFilePicker::row_text(int i) const {
    // sub_407582 @0x4075EE formats every row through `aSS` = "%s: %s"
    // (0x458B11): the glob filename WITH its extension, then the file's own -N
    // scheme name. sub_404BE9 seeds getstring(727) "No Scheme Name" into its
    // return buffer before parsing, so a file with no -N line shows that.
    // sub_41404B strupr's every globbed name @0x414146, so the filename half
    // is uppercase; the -N name keeps the case the file spells it with.
    const std::string& nm = names_[static_cast<std::size_t>(i)];
    const std::string fallback =
        assets_ ? assets_->getstring(727, "No Scheme Name") : std::string("No Scheme Name");
    return upper(entries_[static_cast<std::size_t>(i)].filename().string()) + ": " +
           (nm.empty() ? fallback : nm);
}

// ---------------------------------------------------------------------------
// EditorChooserScreen — sub_403184 (§5)

void EditorChooserScreen::enter(std::string backdrop) {
    backdrop_ = std::move(backdrop);
}

EditorChooserResult EditorChooserScreen::on_key(SDL_Keycode key, AudioEngine& audio) {
    // §5: '1' -> edit existing (sub_4028D2(0), via the *.SCH file picker
    // sub_407582 first); '2' -> new (sub_4028D2(1)); Esc/'Q'/'q' exit; F1
    // (315) help. SFX 20 blip on any key (§5: "SFX 20 blip on any key").
    switch (key) {
        case SDLK_1: audio.play(20); return EditorChooserResult::EditExisting;
        case SDLK_2: audio.play(20); return EditorChooserResult::New;
        case SDLK_ESCAPE:
        case SDLK_Q: audio.play(20); return EditorChooserResult::Exit;
        case SDLK_F1: audio.play(20); return EditorChooserResult::Help;
        default: return EditorChooserResult::None;
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
    step_ = ChainStep::None;
    entry_.clear();
    done_ = false;
}

void PowerupRulesScreen::begin_chain() {
    // sub_4023A2's chain, started by activating a row (the original's mouse
    // click on button 5000+row): prompt 1 is the born-with count text
    // entry, seeded with the current value ("%u").
    step_ = ChainStep::BornWith;
    entry_ = std::to_string((*rows_)[static_cast<std::size_t>(row_)].born_with);
}

void PowerupRulesScreen::advance_chain(AudioEngine& audio) {
    // Move to the next prompt of sub_4023A2's fixed order; prompt 4 is asked
    // only when has-override is set, else the value is FORCED to 0 (the
    // original's else-branch unconditionally zeroes that row's entry in
    // dword_4646C4).
    auto& pr = (*rows_)[static_cast<std::size_t>(row_)];
    switch (step_) {
        case ChainStep::BornWith: step_ = ChainStep::Forbidden; break;
        case ChainStep::Forbidden: step_ = ChainStep::HasOverride; break;
        case ChainStep::HasOverride:
            if (pr.has_override) {
                step_ = ChainStep::OverrideValue;
                entry_ = std::to_string(pr.override_value);
            } else {
                pr.override_value = 0;
                step_ = ChainStep::None;
            }
            break;
        case ChainStep::OverrideValue:
        default: step_ = ChainStep::None; break;
    }
    audio.play(20);
}

void PowerupRulesScreen::on_key(SDL_Keycode key, AudioEngine& audio) {
    if (!rows_ || rows_->empty()) {
        if (key == SDLK_ESCAPE || key == SDLK_RETURN) done_ = true;
        return;
    }
    auto& pr = (*rows_)[static_cast<std::size_t>(row_)];

    // A prompt of the sub_4023A2 chain is open — it owns all input. Each
    // prompt cancels INDEPENDENTLY (sub_42E938/sub_42EDE0 returning -1 keeps
    // that one field and the chain still continues to the next prompt).
    if (step_ == ChainStep::BornWith || step_ == ChainStep::OverrideValue) {
        // Text entry (sub_42E938): a generic line edit committed with Enter
        // and atoi'd — NO clamp for either numeric field (only the .SCH
        // reader clamps born-with < 0 at load). Digits only here since both
        // fields are numeric.
        if (key >= SDLK_0 && key <= SDLK_9) {
            if (entry_ == "0") entry_.clear();
            entry_ += static_cast<char>('0' + (key - SDLK_0));
        } else if (key == SDLK_BACKSPACE && !entry_.empty()) {
            entry_.pop_back();
        } else if (key == SDLK_RETURN || key == SDLK_KP_ENTER) {
            int v = entry_.empty() ? 0 : std::atoi(entry_.c_str());
            if (step_ == ChainStep::BornWith)
                pr.born_with = v;
            else
                pr.override_value = v;
            advance_chain(audio);
        } else if (key == SDLK_ESCAPE) {
            advance_chain(audio);  // cancel: keep the old value, continue the chain
        }
        return;
    }
    if (step_ == ChainStep::Forbidden || step_ == ChainStep::HasOverride) {
        // Yes/no dialog (sub_42EDE0): Y/N commit, Esc cancels (keeps the old
        // value) — either way the chain continues.
        if (key == SDLK_Y || key == SDLK_N) {
            int v = (key == SDLK_Y) ? 1 : 0;
            if (step_ == ChainStep::Forbidden)
                pr.forbidden = v;
            else
                pr.has_override = v;
            advance_chain(audio);
        } else if (key == SDLK_ESCAPE) {
            advance_chain(audio);
        }
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
        case SDLK_E:
        case SDLK_RIGHT:
            // Our keyboard substitute for the original's mouse click on the
            // row button (id 5000+row) — starts sub_4023A2's prompt chain.
            audio.play(20);
            begin_chain();
            break;
        case SDLK_ESCAPE:
        case SDLK_RETURN:
        case SDLK_KP_ENTER:
        case SDLK_SPACE:
        case SDLK_Q:
        case SDLK_P:
            // sub_402595's exit keys: Enter(13)/Esc(27)/Space(32)/'Q'/'q'
            // ('P' kept as our symmetric close of the §5 open gesture).
            audio.play(10);
            done_ = true;
            break;
        default: break;
    }
}

void PowerupRulesScreen::draw(SDL_Renderer* ren) const {
    if (!ren) return;
    SDL_SetRenderDrawColor(ren, 15, 15, 25, 255);
    SDL_RenderClear(ren);
    if (!font_ || !font_->loaded()) return;

    // Header getstring(754) at (300, 30) in the byte_497F8F cyan ink.
    const std::string header =
        assets_ ? assets_->getstring(754, "POWERUP RULES") : std::string("POWERUP RULES");
    font_->draw(ren, header, 300.0f, 30.0f, kHeadR, kHeadG, kHeadB);
    if (!rows_) return;
    for (std::size_t i = 0; i < rows_->size(); ++i) {
        const auto& pr = (*rows_)[i];
        // Row layout (sub_402595): y = 24*i + 60; the powerup NAME column
        // (getstring(756) with getstring(850+i)) at x=90 in the byte_49D37A
        // yellow; born-with (getstring(757)) at x=210 and the override
        // column (getstring(759)/getstring(758)) at x=450, both white.
        float y = 60.0f + 24.0f * static_cast<float>(i);
        bool sel = (static_cast<int>(i) == row_);
        std::string name = assets_ ? assets_->getstring(850 + pr.id, "") : std::string();
        if (name.empty()) name = "POWERUP " + std::to_string(pr.id);
        font_->draw(ren, (sel ? "> " : "  ") + name, 10.0f, y, sel ? kSelR : kNameR,
                    sel ? kSelG : kNameG, sel ? kSelB : kNameB);
        char mid[64];
        std::snprintf(mid, sizeof mid, "BORN-WITH:%d%s", pr.born_with,
                      pr.forbidden ? "  FORBIDDEN" : "");
        font_->draw(ren, mid, 210.0f, y, kInkR, kInkG, kInkB);
        std::string ov = pr.has_override ? ("OVERRIDE " + std::to_string(pr.override_value))
                                         : std::string("(default)");
        font_->draw(ren, ov, 450.0f, y, kInkR, kInkG, kInkB);
    }
    // The open chain prompt — sub_4023A2's own sub_42E938/sub_42EDE0 calls,
    // ALL at the CONFIRMED literal y=400 (pseudo.c 5225 assigns that y, reused
    // unchanged by every one of the 4 calls at 5230/5239/5245/5254), each
    // labelled `getstring(<id>)` formatted with the row's own powerup name
    // (`sub_4518D0`'s "%s%s"-style pack, pseudo.c 5226-5229 etc — exact
    // format string not RE'd, id cited). Routes through the SAME pinned
    // dialog_chrome primitives as the parent editor's own confirms/prompts.
    if (step_ != ChainStep::None && rows_ && !rows_->empty()) {
        const auto& pr = (*rows_)[static_cast<std::size_t>(row_)];
        std::string name = assets_ ? assets_->getstring(850 + pr.id, "") : std::string();
        if (name.empty()) name = "POWERUP " + std::to_string(pr.id);
        switch (step_) {
            case ChainStep::BornWith: {
                std::string label =
                    (assets_ ? assets_->getstring(762, "Born with:") : std::string("Born with:")) +
                    " " + name;
                draw_text_entry_dialog(ren, *font_, 400.0f, label, entry_, "Done", "Cancel");
                break;
            }
            case ChainStep::Forbidden: {
                std::string label =
                    (assets_ ? assets_->getstring(764, "Forbidden?") : std::string("Forbidden?")) +
                    " " + name;
                // sub_42EDE0's HARDCODED literal "Yes"/"No" labels (not a
                // message-table lookup — dialog_chrome.hpp).
                draw_compact_confirm_dialog(ren, *font_, label, "Yes", "No");
                break;
            }
            case ChainStep::HasOverride: {
                std::string label = (assets_ ? assets_->getstring(766, "Override amount?")
                                             : std::string("Override amount?")) +
                                    " " + name;
                draw_compact_confirm_dialog(ren, *font_, label, "Yes", "No");
                break;
            }
            case ChainStep::OverrideValue: {
                std::string label = (assets_ ? assets_->getstring(768, "Override value:")
                                             : std::string("Override value:")) +
                                    " " + name;
                draw_text_entry_dialog(ren, *font_, 400.0f, label, entry_, "Done", "Cancel");
                break;
            }
            default: break;
        }
    }
    font_->draw(ren, "UP/DOWN ROW   E/RIGHT EDIT ROW   ENTER/ESC/SPACE/Q DONE", 40.0f, 460.0f,
                kHintR, kHintG, kHintB);
}

// ---------------------------------------------------------------------------
// EditorScreen — sub_4028D2 (§5)

void EditorScreen::enter(std::optional<assets::sch::Scheme> initial, std::string backdrop,
                         const std::array<std::array<int, 2>, kEditorMaxStarts>* default_starts) {
    backdrop_ = std::move(backdrop);
    default_starts_ = default_starts;  // Ctrl+B's own reset target, §5 case 2
    if (initial) {
        grid_.load_from_scheme(*initial);
    } else {
        // sub_4028D2(1), "new scheme": sub_4049C0's board (editor_grid.cpp)
        // + the default name getstring(729) applied by the caller right
        // after it in sub_4028D2's own prologue.
        grid_.reset(kEditorGridWidth, kEditorGridHeight, default_starts_);
        if (assets_) grid_.set_name(assets_->getstring(729, "UNNAMED"));
    }
    brush_ = EditorBrush::Blank;
    selected_start_ = 0;
    tileset_ = 0;    // dword_45B7B8 starts at 0 every session, §5 case 48
    dirty_ = false;  // sub_4028D2's own v49, pseudo.c 5514
    prompt_kind_ = PromptKind::None;
    prompt_text_.clear();
    editing_powerups_ = false;
    done_ = false;
    save_requested_ = false;

    // Canvas art (§5, PINNED): the tile brush/grid draws the "tile %d
    // blank/solid/brick" sequences with %d = dword_45B7B8 — 0 (stage-0 match
    // art) or, after the '0'-key toggle, -1 (EDIT.ANI's schematic tiles; see
    // refresh_tile_sequences) — and each start's team flag draws MISC.ANI's
    // "teamring%u". Missing art leaves the Anim empty and draw() falls back
    // to flat swatches.
    refresh_tile_sequences();
    teamring_[0] = teamring_[1] = Anim{};
    if (assets_) {
        teamring_[0] = resolve_sequence(assets_->misc(), "teamring0");
        teamring_[1] = resolve_sequence(assets_->misc(), "teamring1");
    }
}

void EditorScreen::refresh_tile_sequences() {
    // sub_402206 (§5d, pseudo.c 5120-5145) formats "tile %d blank/solid/
    // brick" with %d = dword_45B7B8 (our tileset_) — called once at entry
    // and again whenever the '0' key (case 48) changes tileset_. tileset_
    // is only ever 0 or -1 in practice. CORRECTED 2026-07-09 (docs/re/
    // facts.md "ANI sequence-name audit"): -1 is NOT a dead state — the
    // original resolves names in one GLOBAL pool merged from every
    // MASTER.ALI file (sub_41D957), and EDIT.ANI (listed there) owns
    // "tile -1 blank/brick/solid", the editor's schematic tiles. Our
    // per-file model probes TILES then EDIT to reproduce that. A miss in
    // both leaves the Anim empty and draw() falls back to flat swatches.
    tile_blank_ = tile_solid_ = tile_brick_ = Anim{};
    if (!assets_) return;
    const std::string n = std::to_string(tileset_);
    tile_blank_ = resolve_sequence(assets_->tiles(), "tile " + n + " blank");
    tile_solid_ = resolve_sequence(assets_->tiles(), "tile " + n + " solid");
    tile_brick_ = resolve_sequence(assets_->tiles(), "tile " + n + " brick");
    if (tile_blank_.steps.empty())
        tile_blank_ = resolve_sequence(assets_->edit(), "tile " + n + " blank");
    if (tile_solid_.steps.empty())
        tile_solid_ = resolve_sequence(assets_->edit(), "tile " + n + " solid");
    if (tile_brick_.steps.empty())
        tile_brick_ = resolve_sequence(assets_->edit(), "tile " + n + " brick");
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

void EditorScreen::start_save_confirm() {
    prompt_kind_ = PromptKind::SaveConfirm;
}

void EditorScreen::start_reset_confirm() {
    prompt_kind_ = PromptKind::ResetConfirm;
}

void EditorScreen::on_mouse_down(int button, int gx, int gy) {
    if (prompting() || editing_powerups_) return;  // modal sub-screens own input
    if (button == SDL_BUTTON_LEFT) {
        // §5, PINNED: exactly one cell per click — sub_4028D2's paint path
        // is sub_4048EB(cell_x, cell_y, brush); no multi-cell brush exists.
        grid_.paint(gx, gy, brush_);
        dirty_ = true;  // left-button (mask bit 0) branch bumps touched, pseudo.c 5561
    } else if (button == SDL_BUTTON_RIGHT) {
        // §5: "MOVE the currently-selected player-start marker to the
        // hovered cell".
        grid_.move_start(selected_start_, gx, gy);
        dirty_ = true;  // right-button (mask bit 1) branch bumps touched, pseudo.c 5577
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
            dirty_ = true;  // case 68/100 bumps touched only when ACCEPTED, pseudo.c 5678
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
            dirty_ = true;  // case 78/110 bumps touched only when ACCEPTED, pseudo.c 5688
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
    if (prompt_kind_ == PromptKind::FillConfirm) {
        // sub_4028D2 case 6 (Ctrl+F): the getstring(760)/97 yes/no confirm
        // gates the fill; only "yes" runs the sub_4048EB loop AND marks the
        // board dirty (the touched-flag bump is INSIDE the accepted branch,
        // pseudo.c 5605-5613 — unlike Ctrl+B below, a cancelled fill leaves
        // that flag alone).
        if (key == SDLK_Y || key == SDLK_RETURN || key == SDLK_KP_ENTER) {
            grid_.flood_fill(brush_);
            dirty_ = true;
            prompt_kind_ = PromptKind::None;
            audio.play(10);
        } else if (key == SDLK_N || key == SDLK_ESCAPE) {
            prompt_kind_ = PromptKind::None;
            audio.play(10);
        }
        return;
    }
    if (prompt_kind_ == PromptKind::ResetConfirm) {
        // sub_4028D2 case 2 (Ctrl+B), pseudo.c 5584-5599: the confirm only
        // gates whether sub_4049C0 actually RUNS — the touched-flag bump happens
        // unconditionally after the if/else, so dirty_ is already true from
        // the SDLK_B case below regardless of the answer here.
        if (key == SDLK_Y || key == SDLK_RETURN || key == SDLK_KP_ENTER) {
            grid_.reset(kEditorGridWidth, kEditorGridHeight, default_starts_);
            prompt_kind_ = PromptKind::None;
            audio.play(10);
        } else if (key == SDLK_N || key == SDLK_ESCAPE) {
            prompt_kind_ = PromptKind::None;
            audio.play(10);
        }
        return;
    }

    // Normal grid-editing input (§5's documented key switch).
    switch (key) {
        case SDLK_1:
            brush_ = EditorBrush::Blank;
            audio.play(20);
            break;
        case SDLK_2:
            brush_ = EditorBrush::Solid;
            audio.play(20);
            break;
        case SDLK_3:
            brush_ = EditorBrush::Brick;
            audio.play(20);
            break;
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
            // gates this case on the Ctrl modifier. PINNED: the original
            // asks the getstring(760)/97 confirm first (sub_4028D2 case 6),
            // so this opens the FillConfirm prompt rather than filling
            // immediately.
            prompt_kind_ = PromptKind::FillConfirm;
            audio.play(20);
            break;
        case SDLK_B:
            // Ctrl+B (raw code 2, §5) — gated on KMOD_CTRL by the caller
            // (game_app.cpp's editor_key_needs_ctrl), same as Ctrl+F above.
            // PINNED (pseudo.c 5584-5599): while the board is untouched
            // (!dirty_), sub_4049C0 runs immediately with NO confirm; once
            // dirty_, the getstring(740)/97 confirm gates it instead. Either
            // way the touched-flag bump executes UNCONDITIONALLY after the if/else
            // — even a CANCELLED confirm still marks the board dirty — so
            // Ctrl+B always sets dirty_ = true.
            if (dirty_) {
                start_reset_confirm();
            } else {
                grid_.reset(kEditorGridWidth, kEditorGridHeight, default_starts_);
            }
            dirty_ = true;
            audio.play(20);
            break;
        case SDLK_0:
            // '0' (48, §5/§5d): sub_402206's dword_45B7B8 tileset toggle —
            // editor_grid.hpp's toggle_editor_tileset. Toggles between the
            // stage-0 match art and EDIT.ANI's schematic "tile -1" tiles
            // (see refresh_tile_sequences' correction note).
            tileset_ = toggle_editor_tileset(tileset_);
            refresh_tile_sequences();
            audio.play(20);
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
            dirty_ = true;  // case 84/116 bumps touched unconditionally, pseudo.c 5699
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
            dirty_ = true;  // case 80/112 bumps touched unconditionally, pseudo.c 5694
            audio.play(20);
            break;
        case SDLK_ESCAPE:
        case SDLK_Q:
            // sub_4028D2's exit case (27/81/113, pseudo.c 5621-5643) wraps
            // its WHOLE save-confirm+write body in a test of the touched
            // flag — an
            // untouched board (!dirty_) exits immediately with NO prompt
            // and NO write at all.
            if (dirty_) {
                start_save_confirm();
            } else {
                save_requested_ = false;
                done_ = true;
            }
            audio.play(20);
            break;
        default: break;
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

    // The tile grid — PINNED (sub_4022A1): every cell first draws the
    // "tile 0 blank" frame, then a non-blank cell overdraws its solid/brick
    // frame on top, all anchored like the match renderer (hotspot draw at
    // the cell's bottom-centre, the sub_426524/sub_42655F convention).
    // Missing art (empty Anims) falls back to the flat swatches.
    auto draw_step = [&](const Anim& a, float cx, float cy) {
        if (a.steps.empty()) return false;
        const Sprite& sp = a.steps[0];
        if (!sp.tex) return false;
        SDL_FRect dst{cx - sp.hx, cy - sp.hy, static_cast<float>(sp.w), static_cast<float>(sp.h)};
        SDL_RenderTexture(ren, sp.tex, nullptr, &dst);
        return true;
    };
    const bool have_tiles =
        !tile_blank_.steps.empty() && !tile_solid_.steps.empty() && !tile_brick_.steps.empty();
    for (int y = 0; y < grid_.height(); ++y) {
        for (int x = 0; x < grid_.width(); ++x) {
            const float cx = static_cast<float>(kOriginX + x * kCellW) + kCellW / 2.0f;
            const float cy = static_cast<float>(kOriginY + y * kCellH) + kCellH - 1.0f;
            if (have_tiles) {
                draw_step(tile_blank_, cx, cy);
                if (grid_.cell(x, y) == EditorBrush::Solid) draw_step(tile_solid_, cx, cy);
                if (grid_.cell(x, y) == EditorBrush::Brick) draw_step(tile_brick_, cx, cy);
                continue;
            }
            SDL_FRect cell{static_cast<float>(kOriginX + x * kCellW),
                           static_cast<float>(kOriginY + y * kCellH),
                           static_cast<float>(kCellW - 1), static_cast<float>(kCellH - 1)};
            switch (grid_.cell(x, y)) {
                case EditorBrush::Solid: SDL_SetRenderDrawColor(ren, 120, 120, 130, 255); break;
                case EditorBrush::Brick: SDL_SetRenderDrawColor(ren, 150, 90, 40, 255); break;
                default: SDL_SetRenderDrawColor(ren, 40, 90, 40, 255); break;
            }
            SDL_RenderFillRect(ren, &cell);
        }
    }

    // Brush preview at cursor — PINNED (pseudo.c 5518-5524): every frame,
    // BEFORE the start markers/status text but AFTER the grid, the original
    // draws the CURRENT brush's own "tile %d blank/solid/brick" frame
    // (resolved by sub_402206 from the SAME brush-selection variable the
    // '1'/'2'/'3'/Tab keys write) at the raw mouse pixel position (read by
    // sub_431804) via
    // the SAME sub_415920 primitive the grid cells above use — i.e. the
    // same hotspot anchor, just centred on the cursor instead of a cell.
    if (have_tiles) {
        const Anim& preview = brush_ == EditorBrush::Solid   ? tile_solid_
                              : brush_ == EditorBrush::Brick ? tile_brick_
                                                             : tile_blank_;
        draw_step(preview, mouse_px_, mouse_py_);
    }

    // Player-start markers — PINNED (sub_4028D2's draw loop): each slot
    // draws its number ("%u", i+1) at (cell_centre_x - 20, cell_bottom - 36)
    // in the slot's own ink (sub_41672F -> AssetStore::slot_color; the
    // editor runs with team play zeroed, so it is always the slot-colour
    // branch), then the "teamring%u" MISC.ANI sprite (%u = the start's team
    // flag) at (cell_centre_x - 20, cell_bottom). The selected-slot inner
    // box is our own cursor (the original tracks the selection only in the
    // status line).
    for (int i = 0; i < kEditorMaxStarts; ++i) {
        const EditorStart& st = grid_.start(i);
        const float ccx = static_cast<float>(kOriginX + st.x * kCellW) + kCellW / 2.0f;
        const float cby = static_cast<float>(kOriginY + st.y * kCellH) + kCellH - 1.0f;
        const Anim& ring = teamring_[st.team ? 1 : 0];
        bool drew_ring = draw_step(ring, ccx, cby);
        if (font_ && font_->loaded()) {
            std::uint8_t c[3] = {255, 255, 255};
            if (assets_) assets_->slot_color(i, c);
            font_->draw(ren, std::to_string(i + 1), ccx - 20.0f, cby - kCellH, c[0], c[1], c[2]);
        }
        if (!drew_ring) {
            // Fallback marker when MISC.ANI is missing: outline box, team
            // flag as the outline colour (yellow = flagged).
            SDL_FRect marker{static_cast<float>(kOriginX + st.x * kCellW + 4),
                             static_cast<float>(kOriginY + st.y * kCellH + 4),
                             static_cast<float>(kCellW - 8), static_cast<float>(kCellH - 8)};
            if (st.team)
                SDL_SetRenderDrawColor(ren, 255, 255, 0, 255);
            else
                SDL_SetRenderDrawColor(ren, 255, 255, 255, 255);
            SDL_RenderRect(ren, &marker);
        }
        if (i == selected_start_) {
            SDL_FRect inner{static_cast<float>(kOriginX + st.x * kCellW + 6),
                            static_cast<float>(kOriginY + st.y * kCellH + 6),
                            static_cast<float>(kCellW - 12), static_cast<float>(kCellH - 12)};
            SDL_SetRenderDrawColor(ren, 255, 220, 80, 255);
            SDL_RenderRect(ren, &inner);
        }
    }

    if (!font_ || !font_->loaded()) return;

    // Status labels (sub_4028D2, all in the general white ink over the
    // background ink): getstring(742) scheme file at (20,5), getstring(743)
    // density at (20,23), getstring(738) selected start at (20,41), and the
    // getstring(737) exit hint at the bottom (y = 476 - text height). Our
    // single-line summary keeps those ids' CONTENT in one strip.
    const char* brush_name = brush_ == EditorBrush::Solid   ? "SOLID"
                             : brush_ == EditorBrush::Brick ? "BRICK"
                                                            : "BLANK";
    char status[160];
    std::snprintf(status, sizeof status, "BRUSH:%s  START:%d%s  DENSITY:%d  NAME:%s", brush_name,
                  selected_start_ + 1, grid_.start(selected_start_).team ? "[T]" : "",
                  grid_.density(), grid_.name().empty() ? "(none)" : grid_.name().c_str());
    font_->draw(ren, status, 20.0f, 5.0f, kInkR, kInkG, kInkB);
    font_->draw(ren,
                "1/2/3 BRUSH  TAB CYCLE  CTRL+F FILL  CTRL+B RESET  0 TILESET  +/- START  T TEAM  "
                "D DENSITY  N NAME  P POWERUPS  ESC SAVE/EXIT",
                20.0f, 460.0f, kHintR, kHintG, kHintB);

    // Every editor prompt now routes through the SAME pinned sub_41456C/
    // sub_42E938 chrome (dialog_chrome.hpp) the boot LOADING dialog and
    // main-menu quit confirm already use — the earlier per-screen ad hoc
    // black boxes had no basis in the decompile (this doc's own #32
    // "Still NOT reproduced" item).
    if (prompt_kind_ == PromptKind::Density || prompt_kind_ == PromptKind::Name) {
        // sub_42E938 text-entry chrome (§5 'D'/'N', y=180 CONFIRMED literal,
        // pseudo.c cases 68/78) — label getstring(739)/(728), Done/Cancel
        // HARDCODED literals (not message-table lookups, dialog_chrome.hpp).
        bool is_density = prompt_kind_ == PromptKind::Density;
        std::string label = assets_ ? assets_->getstring(is_density ? 739 : 728,
                                                         is_density ? "DENSITY (0-100):" : "NAME:")
                                    : std::string(is_density ? "DENSITY (0-100):" : "NAME:");
        draw_text_entry_dialog(ren, *font_, 180.0f, label, prompt_text_, "Done", "Cancel");
    } else if (prompt_kind_ == PromptKind::SaveConfirm) {
        // sub_41456C two-line chrome — getstring(735) + the shared yes/no
        // line getstring(95) (§5 exit case, pseudo.c 5626-5629).
        std::string line1 = assets_ ? assets_->getstring(735, "Save changes to this scheme?")
                                    : std::string("Save changes to this scheme?");
        std::string line2 = assets_ ? assets_->getstring(95, "") : std::string();
        std::string yes_label = assets_ ? assets_->getstring(26, " Yes ") : std::string(" Yes ");
        std::string no_label = assets_ ? assets_->getstring(25, " No ") : std::string(" No ");
        draw_confirm_dialog(ren, *font_, assets_ ? &assets_->frontend_pcx("WINZ") : nullptr, line1,
                            line2, yes_label, no_label, kDialogInkR, kDialogInkG, kDialogInkB);
    } else if (prompt_kind_ == PromptKind::FillConfirm) {
        // sub_41456C two-line chrome — getstring(760) + getstring(97) (§5
        // Ctrl+F, pseudo.c 5601-5604).
        std::string line1 = assets_
                                ? assets_->getstring(760, "Fill the whole board with the brush?")
                                : std::string("Fill the whole board with the brush?");
        std::string line2 = assets_ ? assets_->getstring(97, "") : std::string();
        std::string yes_label = assets_ ? assets_->getstring(26, " Yes ") : std::string(" Yes ");
        std::string no_label = assets_ ? assets_->getstring(25, " No ") : std::string(" No ");
        draw_confirm_dialog(ren, *font_, assets_ ? &assets_->frontend_pcx("WINZ") : nullptr, line1,
                            line2, yes_label, no_label, kDialogInkR, kDialogInkG, kDialogInkB);
    } else if (prompt_kind_ == PromptKind::ResetConfirm) {
        // sub_41456C two-line chrome — getstring(740) + getstring(97) (§5
        // Ctrl+B, pseudo.c 5587-5590).
        std::string line1 = assets_ ? assets_->getstring(740, "Reset the board to a blank scheme?")
                                    : std::string("Reset the board to a blank scheme?");
        std::string line2 = assets_ ? assets_->getstring(97, "") : std::string();
        std::string yes_label = assets_ ? assets_->getstring(26, " Yes ") : std::string(" Yes ");
        std::string no_label = assets_ ? assets_->getstring(25, " No ") : std::string(" No ");
        draw_confirm_dialog(ren, *font_, assets_ ? &assets_->frontend_pcx("WINZ") : nullptr, line1,
                            line2, yes_label, no_label, kDialogInkR, kDialogInkG, kDialogInkB);
    }
}

}  // namespace bomber::game
