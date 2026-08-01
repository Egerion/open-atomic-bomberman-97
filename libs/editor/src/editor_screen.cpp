#include "bomber/editor/editor_screen.hpp"

#include <array>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <string>

#include "bomber/ui/dialog_chrome.hpp"

namespace bomber::game {

namespace {

// Screen inks, pinned via the byte_495390 RGB555-LUT decode (docs/re/
// results-and-options.md §1 "screen-ink byte globals"). kSel/kHint are our own
// cursor/hint tints — the original has no keyboard cursor, its rows are mouse
// buttons.
constexpr Rgb kInk{255, 255, 255};  // byte_49D38F white
constexpr Rgb kName{252, 248, 88};  // byte_49D37A yellow
constexpr Rgb kHead{96, 252, 252};  // byte_497F8F cyan
constexpr Rgb kSel{255, 220, 80};
constexpr Rgb kHint{160, 160, 160};
// byte_49A390 — sub_407582's empty-glob error ink (LUT offset 0x5000 -> idx
// 248), the same dark red the main-menu quit prompt uses.
constexpr Rgb kErr{164, 0, 0};

// §5 CONFIRMED: title getstring(730) at getvalue(810/811/813); rows
// getstring(731..733) at getvalue(815-818). MESSAGES.TXT is install data and is
// never committed (CLAUDE.md), so every LABEL below is our own paraphrase — the
// ids are cited so a real install's strings substitute in; only the ids and
// positions are RE facts.
constexpr int kChooserHeaderX = 50, kChooserHeaderY = 100;  // getvalue(810/811)
constexpr int kChooserItemX = 80, kChooserItemY0 = 140,
              kChooserItemYStep = 20;  // getvalue(815-818)

// getstring with a compiled-in fallback, for the null-AssetStore case.
std::string text_of(const AssetStore* assets, int id, const char* fallback) {
    return assets ? assets->getstring(id, fallback) : std::string(fallback);
}

// The chooser, the picker and the editor all sit on a pick_glue backdrop; a
// missing PCX leaves the flat clear the original's palette load would show.
void draw_backdrop(SDL_Renderer* ren, const AssetStore* assets, const std::string& name) {
    if (!assets) return;
    const Sprite& bg = assets->frontend_pcx(name);
    if (!bg.tex) {
        SDL_SetRenderDrawColor(ren, 20, 20, 30, 255);
        SDL_RenderClear(ren);
        return;
    }
    SDL_FRect d{0, 0, static_cast<float>(bg.w), static_cast<float>(bg.h)};
    SDL_RenderTexture(ren, bg.tex, nullptr, &d);
}

// One ANI frame, hotspot-anchored at (cx, cy) — the sub_415920 convention the
// match renderer uses. False when the sequence has no usable frame, so a caller
// can fall back to its own marker.
bool draw_anim_step(SDL_Renderer* ren, const Anim& a, float cx, float cy) {
    if (a.steps.empty()) return false;
    const Sprite& sp = a.steps[0];
    if (!sp.tex) return false;
    SDL_FRect dst{cx - sp.hx, cy - sp.hy, static_cast<float>(sp.w), static_cast<float>(sp.h)};
    SDL_RenderTexture(ren, sp.tex, nullptr, &dst);
    return true;
}

// The three Y/N confirms all take sub_41456C's own accept/decline keys.
enum class ConfirmAnswer : std::uint8_t { Pending, Yes, No };

ConfirmAnswer confirm_answer(SDL_Keycode key) {
    if (key == SDLK_Y || key == SDLK_RETURN || key == SDLK_KP_ENTER) return ConfirmAnswer::Yes;
    if (key == SDLK_N || key == SDLK_ESCAPE) return ConfirmAnswer::No;
    return ConfirmAnswer::Pending;
}

}  // namespace

// ---------------------------------------------------------------------------
// SchemeFilePicker — sub_407582 (§5)

void SchemeFilePicker::enter(const std::filesystem::path& schemes_dir, std::string backdrop) {
    backdrop_ = std::move(backdrop);
    names_.clear();
    list_.reset();
    done_ = false;
    cancelled_ = false;
    // The glob and the PINNED uppercased-filename ordering (sub_41404B's strupr
    // pass @0x414146, then the strcmp qsort @0x41415D) live in ListPicker; the
    // ": <scheme name>" suffix is appended afterwards, by sub_407582's own
    // reformat loop, and never participates in the sort.
    list_.set_header(header());
    list_.glob(schemes_dir, ".SCH");
    // sub_407582 pre-reads each file's embedded -N name (sub_404BE9) for the
    // second column. An unreadable file keeps the getstring(727) default,
    // exactly as a file with no -N line does — sub_404BE9 seeds that default
    // into its buffer before it even opens the file.
    names_.reserve(static_cast<std::size_t>(list_.count()));
    for (int i = 0; i < list_.count(); ++i) {
        std::string n;
        try {
            n = assets::sch::load(list_.path(i)).name;
        } catch (const std::exception&) {  // NOLINT(bugprone-empty-catch)
            // Deliberate: see above — the default is already in place.
        }
        names_.push_back(std::move(n));
    }
    list_.measure_rows([this](int i) { return row_text(i); });
}

std::string SchemeFilePicker::header() const {
    return text_of(assets_, 721, "Available Scheme Files:");
}

const std::filesystem::path& SchemeFilePicker::selected() const {
    // @0x42E39A under the @0x42E3A8 range check its HelpBrowser clone always
    // carried and this copy lacked. The empty fallback covers only API misuse
    // (a call without done() && !cancelled()); no runner takes that path.
    static const std::filesystem::path kNone;
    const std::filesystem::path* sel = list_.selected();
    return sel != nullptr ? *sel : kNone;
}

void SchemeFilePicker::apply(ListDialogAction action) {
    if (action == ListDialogAction::None) return;
    done_ = true;
    cancelled_ = action == ListDialogAction::Cancel;
}

void SchemeFilePicker::on_key(SDL_Keycode key, AudioEngine& audio) {
    if (list_.empty()) {
        // The empty-glob acknowledge box is sub_414340's own key loop: a nav blip
        // on ANY real key, closing only on Enter(13)/Space(32)/Esc(27).
        audio.play(20);
        if (key == SDLK_ESCAPE || key == SDLK_RETURN || key == SDLK_KP_ENTER || key == SDLK_SPACE)
            apply(ListDialogAction::Cancel);
        return;
    }
    // THE LIST DIALOG IS SILENT (docs/re/sound-engine.md §8). sub_407582's only
    // reachable sound is the empty-glob box above; the widget it hands the glob
    // to — sub_41485A -> sub_42DB80 -> sub_42DBCC — has no play call anywhere in
    // its 344-function closure.
    //
    // W/S and Space are the port's own aliases: the original would route a
    // letter to sub_42FEB0's type-ahead, which this picker does not implement.
    const int code = (key == SDLK_W)       ? kListKeyUp
                     : (key == SDLK_S)     ? kListKeyDown
                     : (key == SDLK_SPACE) ? kListKeyEnter
                                           : list_dialog_key_code(key);
    apply(list_dialog_key(list_.nav(), code, kVisibleRows, list_.count()));
}

void SchemeFilePicker::on_mouse_move(float x, float y, bool buttons_held) {
    list_.on_mouse_move(x, y, buttons_held);
}

void SchemeFilePicker::on_mouse_down(float x, float y) {
    apply(list_.on_mouse_down(x, y));
}

void SchemeFilePicker::on_mouse_up(float x, float y) {
    apply(list_.on_mouse_up(x, y));
}

void SchemeFilePicker::draw(SDL_Renderer* ren) const {
    if (!ren) return;
    draw_backdrop(ren, assets_, backdrop_);
    if (!font_ || !font_->loaded()) return;
    if (list_.empty()) {
        // sub_407582's empty-glob branch @0x4076CA: instead of the list it raises
        // sub_414340 with getstring(95) "NOTE!" over getstring(720), ink
        // byte_49A390. This lives in the picker rather than in each caller
        // because in the original it is the same ONE routine both entry points
        // call.
        draw_acknowledge_dialog(DialogPen{ren, *font_},
                                assets_ ? &assets_->frontend_pcx("WINZ") : nullptr,
                                AcknowledgeLabels{text_of(assets_, 95, "NOTE!"),
                                                  text_of(assets_, 720, "No Scheme files found!"),
                                                  text_of(assets_, 27, " Ok ")},
                                AcknowledgeStyle{kErr});
        return;
    }
    // sub_407582 @0x407641 pushes the LITERAL pair (100, 100) and getstring(721)
    // into sub_41485A -> sub_42DB80 -> sub_42DBCC, in the general white ink —
    // the pinned chrome ListPicker carries.
    list_.draw(ren, [this](int i) { return row_text(i); });
}

std::string SchemeFilePicker::row_text(int i) const {
    // sub_407582 @0x4075EE formats every row through aSS = "%s: %s" (0x458B11):
    // the glob filename WITH its extension, then the file's own -N scheme name.
    // sub_41404B strupr's every globbed name @0x414146, so the filename half is
    // uppercase; the -N name keeps the case the file spells it with.
    const std::string& nm = names_[static_cast<std::size_t>(i)];
    const std::string name = nm.empty() ? text_of(assets_, 727, "No Scheme Name") : nm;
    return upper_ascii(list_.path(i).filename().string()) + ": " + name;
}

// ---------------------------------------------------------------------------
// EditorChooserScreen — sub_403184 (§5)

void EditorChooserScreen::enter(std::string backdrop) {
    backdrop_ = std::move(backdrop);
}

EditorChooserResult EditorChooserScreen::on_key(SDL_Keycode key, AudioEngine& audio) {
    // This chooser is the ONLY part of the editor that makes a sound, and the
    // blip is UNCONDITIONAL: sub_403184 @0x403288 fires SFX 20 for every real key
    // (only the -1/-2 no-key codes skip it) BEFORE its dispatch switch at
    // 0x403292, so an unmapped key still clicks. No branch of that switch plays
    // anything else — there is no accept sting on '1'/'2'/Esc/F1. Blip first,
    // then dispatch, so the two cannot drift apart.
    audio.play(20);
    switch (key) {
        case SDLK_1: return EditorChooserResult::EditExisting;  // sub_4028D2(0), after the picker
        case SDLK_2: return EditorChooserResult::New;           // sub_4028D2(1)
        case SDLK_ESCAPE:
        case SDLK_Q: return EditorChooserResult::Exit;
        case SDLK_F1: return EditorChooserResult::Help;  // key code 315
        default: return EditorChooserResult::None;
    }
}

void EditorChooserScreen::draw(SDL_Renderer* ren) const {
    if (!ren) return;
    draw_backdrop(ren, assets_, backdrop_);
    if (!font_ || !font_->loaded()) return;

    // getstring(730), then getstring(731..733); our paraphrases (see the file
    // header's note on MESSAGES.TXT).
    font_->draw(
        ren, "SCHEME EDITOR",
        SDL_FPoint{static_cast<float>(kChooserHeaderX), static_cast<float>(kChooserHeaderY)},
        TextStyle{kInk});
    static constexpr std::array<const char*, 3> kItems{"1) EDIT AN EXISTING SCHEME",
                                                       "2) NEW SCHEME", "ESC) EXIT"};
    for (int i = 0; i < static_cast<int>(kItems.size()); ++i) {
        const float y = static_cast<float>(kChooserItemY0 + i * kChooserItemYStep);
        font_->draw(ren, kItems[static_cast<std::size_t>(i)],
                    SDL_FPoint{static_cast<float>(kChooserItemX), y}, TextStyle{kInk});
    }
    font_->draw(ren, "F1 HELP",
                SDL_FPoint{static_cast<float>(kChooserItemX),
                           static_cast<float>(kChooserItemY0 + 4 * kChooserItemYStep)},
                TextStyle{kHint});
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
    // sub_4023A2's chain, started by activating a row (the original's click on
    // button 5000+row): prompt 1 is the born-with text entry, seeded "%u".
    step_ = ChainStep::BornWith;
    entry_ = std::to_string((*rows_)[static_cast<std::size_t>(row_)].born_with);
}

void PowerupRulesScreen::advance_chain() {
    // The next prompt of sub_4023A2's fixed order. Prompt 4 is asked only when
    // has-override is set; otherwise the value is FORCED to 0 (the original's
    // else-branch unconditionally zeroes that row's dword_4646C4 entry).
    //
    // No cue: the four prompts are the generic sub_42E938 / sub_42EDE0 widgets,
    // and NEITHER makes a sound — 155 and 143 functions of closure, no play call.
    auto& pr = (*rows_)[static_cast<std::size_t>(row_)];
    switch (step_) {
        case ChainStep::BornWith: step_ = ChainStep::Forbidden; return;
        case ChainStep::Forbidden: step_ = ChainStep::HasOverride; return;
        case ChainStep::HasOverride: break;
        default: step_ = ChainStep::None; return;
    }
    if (!pr.has_override) {
        pr.override_value = 0;
        step_ = ChainStep::None;
        return;
    }
    step_ = ChainStep::OverrideValue;
    entry_ = std::to_string(pr.override_value);
}

void PowerupRulesScreen::on_text_prompt_key(SDL_Keycode key) {
    // Text entry (sub_42E938): a generic line edit committed with Enter and
    // atoi'd — NO clamp for either numeric field (only the .SCH reader clamps
    // born-with < 0 at load). Digits only, since both fields are numeric.
    if (key >= SDLK_0 && key <= SDLK_9) {
        if (entry_ == "0") entry_.clear();
        entry_ += static_cast<char>('0' + (key - SDLK_0));
        return;
    }
    if (key == SDLK_BACKSPACE) {
        if (!entry_.empty()) entry_.pop_back();
        return;
    }
    if (key == SDLK_ESCAPE) {
        advance_chain();  // cancel: keeps the old value, the chain continues
        return;
    }
    if (key != SDLK_RETURN && key != SDLK_KP_ENTER) return;
    auto& pr = (*rows_)[static_cast<std::size_t>(row_)];
    const int v = entry_.empty() ? 0 : std::atoi(entry_.c_str());
    if (step_ == ChainStep::BornWith) pr.born_with = v;
    if (step_ == ChainStep::OverrideValue) pr.override_value = v;
    advance_chain();
}

void PowerupRulesScreen::on_yesno_prompt_key(SDL_Keycode key) {
    // Yes/no (sub_42EDE0): Y/N commit, Esc cancels and keeps the old value —
    // either way the chain continues.
    if (key == SDLK_ESCAPE) {
        advance_chain();
        return;
    }
    if (key != SDLK_Y && key != SDLK_N) return;
    auto& pr = (*rows_)[static_cast<std::size_t>(row_)];
    const int v = (key == SDLK_Y) ? 1 : 0;
    if (step_ == ChainStep::Forbidden) pr.forbidden = v;
    if (step_ == ChainStep::HasOverride) pr.has_override = v;
    advance_chain();
}

void PowerupRulesScreen::on_row_key(SDL_Keycode key) {
    // SILENT, like the rest of the editor: sub_402595's body contains no play
    // call, and the only sound-bearing function it can reach at all is the F1
    // help browser's error box.
    const int count = static_cast<int>(rows_->size());
    switch (key) {
        case SDLK_UP:
        case SDLK_W: row_ = (row_ + count - 1) % count; break;
        case SDLK_DOWN:
        case SDLK_S: row_ = (row_ + 1) % count; break;
        case SDLK_E:
        case SDLK_RIGHT:
            // The keyboard substitute for the original's click on the row button
            // (id 5000+row) — starts sub_4023A2's prompt chain.
            begin_chain();
            break;
        case SDLK_ESCAPE:
        case SDLK_RETURN:
        case SDLK_KP_ENTER:
        case SDLK_SPACE:
        case SDLK_Q:
        case SDLK_P:
            // sub_402595's exit keys ('P' kept as our symmetric close of the §5
            // open gesture).
            done_ = true;
            break;
        default: break;
    }
}

void PowerupRulesScreen::on_key(SDL_Keycode key) {
    if (!rows_ || rows_->empty()) {
        if (key == SDLK_ESCAPE || key == SDLK_RETURN) done_ = true;
        return;
    }
    // An open prompt of the sub_4023A2 chain owns all input. Each prompt cancels
    // INDEPENDENTLY: sub_42E938/sub_42EDE0 returning -1 keeps that one field and
    // the chain still continues to the next prompt.
    if (step_ == ChainStep::BornWith || step_ == ChainStep::OverrideValue) {
        on_text_prompt_key(key);
        return;
    }
    if (step_ == ChainStep::Forbidden || step_ == ChainStep::HasOverride) {
        on_yesno_prompt_key(key);
        return;
    }
    on_row_key(key);
}

std::string PowerupRulesScreen::powerup_name(int row) const {
    const auto& pr = (*rows_)[static_cast<std::size_t>(row)];
    std::string name = text_of(assets_, 850 + pr.id, "");
    if (name.empty()) name = "POWERUP " + std::to_string(pr.id);
    return name;
}

void PowerupRulesScreen::draw_row(SDL_Renderer* ren, int row) const {
    // Row layout (sub_402595): y = 24*i + 60; the powerup NAME column
    // (getstring(756) with getstring(850+i)) at x=90 in the byte_49D37A yellow;
    // born-with (getstring(757)) at x=210 and the override column
    // (getstring(759)/getstring(758)) at x=450, both white.
    const auto& pr = (*rows_)[static_cast<std::size_t>(row)];
    const float y = 60.0f + 24.0f * static_cast<float>(row);
    const bool sel = row == row_;
    font_->draw(ren, (sel ? "> " : "  ") + powerup_name(row), SDL_FPoint{10.0f, y},
                TextStyle{sel ? kSel : kName});
    std::array<char, 64> mid{};
    std::snprintf(mid.data(), mid.size(), "BORN-WITH:%d%s", pr.born_with,
                  pr.forbidden ? "  FORBIDDEN" : "");
    font_->draw(ren, mid.data(), SDL_FPoint{210.0f, y}, TextStyle{kInk});
    const std::string ov = pr.has_override ? ("OVERRIDE " + std::to_string(pr.override_value))
                                           : std::string("(default)");
    font_->draw(ren, ov, SDL_FPoint{450.0f, y}, TextStyle{kInk});
}

void PowerupRulesScreen::draw_chain_prompt(SDL_Renderer* ren) const {
    // sub_4023A2's own sub_42E938/sub_42EDE0 calls, ALL at the CONFIRMED literal
    // y=400 (pseudo.c 5225 assigns that y and every one of the four calls at
    // 5230/5239/5245/5254 reuses it), each labelled getstring(<id>) formatted
    // with the row's own powerup name (sub_4518D0's "%s%s"-style pack, pseudo.c
    // 5226-5229 — the exact format string is not RE'd, the ids are). Routes
    // through the SAME pinned dialog_chrome primitives as the parent editor.
    if (step_ == ChainStep::None || !rows_ || rows_->empty()) return;
    const std::string name = " " + powerup_name(row_);
    const DialogPen pen{ren, *font_};
    switch (step_) {
        case ChainStep::BornWith:
            draw_text_entry_dialog(pen, 400.0f,
                                   TextEntryLabels{text_of(assets_, 762, "Born with:") + name,
                                                   entry_, "Done", "Cancel"});
            return;
        case ChainStep::OverrideValue:
            draw_text_entry_dialog(pen, 400.0f,
                                   TextEntryLabels{text_of(assets_, 768, "Override value:") + name,
                                                   entry_, "Done", "Cancel"});
            return;
        case ChainStep::Forbidden:
            // sub_42EDE0's HARDCODED literal "Yes"/"No" labels, not a
            // message-table lookup (dialog_chrome.hpp).
            draw_compact_confirm_dialog(pen, text_of(assets_, 764, "Forbidden?") + name, "Yes",
                                        "No");
            return;
        case ChainStep::HasOverride:
            draw_compact_confirm_dialog(pen, text_of(assets_, 766, "Override amount?") + name,
                                        "Yes", "No");
            return;
        default: return;
    }
}

void PowerupRulesScreen::draw(SDL_Renderer* ren) const {
    if (!ren) return;
    SDL_SetRenderDrawColor(ren, 15, 15, 25, 255);
    SDL_RenderClear(ren);
    if (!font_ || !font_->loaded()) return;

    // Header getstring(754) at (300, 30) in the byte_497F8F cyan.
    font_->draw(ren, text_of(assets_, 754, "POWERUP RULES"), SDL_FPoint{300.0f, 30.0f},
                TextStyle{kHead});
    if (!rows_) return;
    for (int i = 0; i < static_cast<int>(rows_->size()); ++i) draw_row(ren, i);
    draw_chain_prompt(ren);
    font_->draw(ren, "UP/DOWN ROW   E/RIGHT EDIT ROW   ENTER/ESC/SPACE/Q DONE",
                SDL_FPoint{40.0f, 460.0f}, TextStyle{kHint});
}

// ---------------------------------------------------------------------------
// EditorScreen — sub_4028D2 (§5)

void EditorScreen::enter(const std::optional<assets::sch::Scheme>& initial, std::string backdrop,
                         const std::array<std::array<int, 2>, kEditorMaxStarts>* default_starts) {
    backdrop_ = std::move(backdrop);
    default_starts_ = default_starts;  // Ctrl+B's own reset target, §5 case 2
    load_board(initial);
    brush_ = EditorBrush::Blank;
    selected_start_ = 0;
    tileset_ = 0;    // dword_45B7B8 starts at 0 every session, §5 case 48
    dirty_ = false;  // sub_4028D2's own touched flag, pseudo.c 5514
    prompt_kind_ = PromptKind::None;
    prompt_text_.clear();
    editing_powerups_ = false;
    done_ = false;
    save_requested_ = false;

    refresh_tile_sequences();
    teamring_ = {};
    if (!assets_) return;
    // Each start's team flag draws MISC.ANI's "teamring%u".
    teamring_[0] = resolve_sequence(assets_->misc(), "teamring0");
    teamring_[1] = resolve_sequence(assets_->misc(), "teamring1");
}

void EditorScreen::load_board(const std::optional<assets::sch::Scheme>& initial) {
    if (initial) {
        grid_.load_from_scheme(*initial);
        return;
    }
    // sub_4028D2(1), "new scheme": sub_4049C0's board (editor_grid.cpp) plus the
    // default name getstring(729) the prologue applies right after it.
    grid_.reset(kEditorGridWidth, kEditorGridHeight, default_starts_);
    if (assets_) grid_.set_name(assets_->getstring(729, "UNNAMED"));
}

void EditorScreen::refresh_tile_sequences() {
    // sub_402206 (§5d, pseudo.c 5120-5145) formats "tile %d blank/solid/brick"
    // with %d = dword_45B7B8 (our tileset_). CORRECTED 2026-07-09 (docs/re/
    // facts.md "ANI sequence-name audit"): -1 is NOT a dead state — the original
    // resolves names in one GLOBAL pool merged from every MASTER.ALI file
    // (sub_41D957), and EDIT.ANI (listed there) owns "tile -1 blank/brick/solid",
    // the editor's schematic tiles. Our per-file model probes TILES then EDIT to
    // reproduce that; a miss in both leaves the Anim empty and draw() falls back
    // to flat swatches.
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

bool EditorScreen::have_tile_art() const {
    return !tile_blank_.steps.empty() && !tile_solid_.steps.empty() && !tile_brick_.steps.empty();
}

const Anim& EditorScreen::brush_anim() const {
    if (brush_ == EditorBrush::Solid) return tile_solid_;
    if (brush_ == EditorBrush::Brick) return tile_brick_;
    return tile_blank_;
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

void EditorScreen::start_board_reset() {
    // Ctrl+B (raw code 2, §5), gated on KMOD_CTRL by the caller. PINNED
    // (pseudo.c 5584-5599): while the board is untouched sub_4049C0 runs
    // immediately with NO confirm; once touched, the getstring(740)/97 confirm
    // gates it instead. Either way the touched-flag bump executes
    // UNCONDITIONALLY after the if/else — even a CANCELLED confirm marks the
    // board dirty — so Ctrl+B always sets dirty_.
    const bool needs_confirm = dirty_;
    dirty_ = true;
    if (needs_confirm) {
        prompt_kind_ = PromptKind::ResetConfirm;
        return;
    }
    grid_.reset(kEditorGridWidth, kEditorGridHeight, default_starts_);
}

void EditorScreen::open_powerup_editor() {
    editing_powerups_ = true;
    powerups_screen_.enter(&grid_.powerups());
    dirty_ = true;  // case 80/112 bumps touched unconditionally, pseudo.c 5694
}

void EditorScreen::request_exit() {
    // sub_4028D2's exit case (27/81/113, pseudo.c 5621-5643) wraps its WHOLE
    // save-confirm+write body in a test of the touched flag: an untouched board
    // exits immediately with NO prompt and NO write at all.
    if (dirty_) {
        prompt_kind_ = PromptKind::SaveConfirm;
        return;
    }
    save_requested_ = false;
    done_ = true;
}

void EditorScreen::on_mouse_down(int button, int gx, int gy) {
    if (prompting() || editing_powerups_) return;  // modal sub-screens own input
    if (button == SDL_BUTTON_LEFT) {
        // §5, PINNED: exactly one cell per click — sub_4028D2's paint path is
        // sub_4048EB(cell_x, cell_y, brush); no multi-cell brush exists.
        grid_.paint(gx, gy, brush_);
        dirty_ = true;  // left-button (mask bit 0) branch bumps touched, pseudo.c 5561
        return;
    }
    if (button != SDL_BUTTON_RIGHT) return;
    // §5: move the currently-selected player-start marker to the hovered cell.
    grid_.move_start(selected_start_, gx, gy);
    dirty_ = true;  // right-button (mask bit 1) branch bumps touched, pseudo.c 5577
}

void EditorScreen::on_text_input(const char* text) {
    if (prompt_kind_ != PromptKind::Name || !text) return;
    prompt_text_ += text;
}

void EditorScreen::on_density_key(SDL_Keycode key) {
    // §5 'D'/'d': the brick-density prompt (text entry, 0-100).
    if (key >= SDLK_0 && key <= SDLK_9) {
        if (prompt_text_ == "0") prompt_text_.clear();
        prompt_text_ += static_cast<char>('0' + (key - SDLK_0));
        return;
    }
    if (key == SDLK_BACKSPACE) {
        if (!prompt_text_.empty()) prompt_text_.pop_back();
        return;
    }
    if (key == SDLK_ESCAPE) {
        prompt_kind_ = PromptKind::None;  // discard the in-progress edit
        return;
    }
    if (key != SDLK_RETURN && key != SDLK_KP_ENTER) return;
    grid_.set_density(prompt_text_.empty() ? 0 : std::atoi(prompt_text_.c_str()));
    prompt_kind_ = PromptKind::None;
    dirty_ = true;  // case 68/100 bumps touched only when ACCEPTED, pseudo.c 5678
}

void EditorScreen::on_name_key(SDL_Keycode key) {
    // §5 'N'/'n': the -N scheme-name prompt. Printable characters arrive through
    // on_text_input (SDL_EVENT_TEXT_INPUT); this handles the control keys.
    if (key == SDLK_BACKSPACE) {
        if (!prompt_text_.empty()) prompt_text_.pop_back();
        return;
    }
    if (key == SDLK_ESCAPE) {
        prompt_kind_ = PromptKind::None;
        return;
    }
    if (key != SDLK_RETURN && key != SDLK_KP_ENTER) return;
    grid_.set_name(prompt_text_);
    prompt_kind_ = PromptKind::None;
    dirty_ = true;  // case 78/110 bumps touched only when ACCEPTED, pseudo.c 5688
}

void EditorScreen::on_confirm_key(SDL_Keycode key) {
    const ConfirmAnswer answer = confirm_answer(key);
    if (answer == ConfirmAnswer::Pending) return;
    const bool yes = answer == ConfirmAnswer::Yes;
    if (prompt_kind_ == PromptKind::SaveConfirm) {
        // §5: the exit confirm (getstring(735)) decides whether sch::write() runs.
        save_requested_ = yes;
        done_ = true;
        return;
    }
    // Ctrl+F, sub_4028D2 case 6: the getstring(760)/97 confirm gates the fill,
    // and the touched-flag bump is INSIDE the accepted branch (pseudo.c
    // 5605-5613) — unlike Ctrl+B, a cancelled fill leaves the flag alone.
    if (yes && prompt_kind_ == PromptKind::FillConfirm) {
        grid_.flood_fill(brush_);
        dirty_ = true;
    }
    // Ctrl+B: the confirm only gates whether sub_4049C0 RUNS — dirty_ was
    // already set by start_board_reset(), whatever the answer here.
    if (yes && prompt_kind_ == PromptKind::ResetConfirm)
        grid_.reset(kEditorGridWidth, kEditorGridHeight, default_starts_);
    prompt_kind_ = PromptKind::None;
}

void EditorScreen::on_prompt_key(SDL_Keycode key) {
    switch (prompt_kind_) {
        case PromptKind::Density: on_density_key(key); return;
        case PromptKind::Name: on_name_key(key); return;
        case PromptKind::SaveConfirm:
        case PromptKind::FillConfirm:
        case PromptKind::ResetConfirm: on_confirm_key(key); return;
        case PromptKind::None: return;
    }
}

void EditorScreen::on_grid_key(SDL_Keycode key) {
    // §5's documented key switch. Ctrl+F/Ctrl+B arrive as the plain letter with
    // KMOD_CTRL and the caller gates them (editor_key_needs_ctrl).
    switch (key) {
        case SDLK_1: brush_ = EditorBrush::Blank; break;
        case SDLK_2: brush_ = EditorBrush::Solid; break;
        case SDLK_3: brush_ = EditorBrush::Brick; break;
        case SDLK_TAB:
        case SDLK_RETURN:
        case SDLK_KP_ENTER:
        case SDLK_SPACE: cycle_brush(); break;
        // PINNED (case 6): the original asks the getstring(760)/97 confirm first,
        // so this opens the prompt rather than filling immediately.
        case SDLK_F: prompt_kind_ = PromptKind::FillConfirm; break;
        case SDLK_B: start_board_reset(); break;
        // '0' (case 48): sub_402206's dword_45B7B8 tileset toggle, between the
        // stage-0 match art and EDIT.ANI's schematic "tile -1" tiles.
        case SDLK_0:
            tileset_ = toggle_editor_tileset(tileset_);
            refresh_tile_sequences();
            break;
        case SDLK_EQUALS:
        case SDLK_KP_PLUS: selected_start_ = (selected_start_ + 1) % kEditorMaxStarts; break;
        case SDLK_MINUS:
        case SDLK_KP_MINUS:
            selected_start_ = (selected_start_ + kEditorMaxStarts - 1) % kEditorMaxStarts;
            break;
        case SDLK_T:
            grid_.toggle_start_team(selected_start_);
            dirty_ = true;  // case 84/116 bumps touched unconditionally, pseudo.c 5699
            break;
        case SDLK_D: start_density_prompt(); break;
        case SDLK_N: start_name_prompt(); break;
        case SDLK_P: open_powerup_editor(); break;
        case SDLK_ESCAPE:
        case SDLK_Q: request_exit(); break;
        default: break;
    }
}

// THE EDITOR SCREEN IS SILENT. sub_4028D2's own body has no play call, and
// neither does any direct callee: at call depth <= 3 the only sound-bearing
// functions reachable at all are the two generic modals (sub_414340 via the
// *.SCH picker's empty-glob error, sub_41456C via sub_402942 -> sub_402AE6), and
// those blip because they are modals, not because the editor asked. The confirms
// and prompts it actually uses — sub_42EDE0 and sub_42E938 — make no sound at
// all: 143 and 155 functions of closure, zero play calls. So painting, brush
// changes, Ctrl+F/Ctrl+B, the density/name prompts, the save confirm and the exit
// are ALL noiseless. The port had ~35 invented cues in here. Do not add any.
void EditorScreen::on_key(SDL_Keycode key) {
    if (editing_powerups_) return;  // caller routes to powerups_screen() instead
    if (prompt_kind_ != PromptKind::None) {
        on_prompt_key(key);
        return;
    }
    on_grid_key(key);
}

void EditorScreen::draw_cell(SDL_Renderer* ren, int x, int y) const {
    const float cx = static_cast<float>(kOriginX + x * kCellW) + kCellW / 2.0f;
    const float cy = static_cast<float>(kOriginY + y * kCellH) + kCellH - 1.0f;
    const EditorBrush cell = grid_.cell(x, y);
    if (have_tile_art()) {
        draw_anim_step(ren, tile_blank_, cx, cy);
        if (cell == EditorBrush::Solid) draw_anim_step(ren, tile_solid_, cx, cy);
        if (cell == EditorBrush::Brick) draw_anim_step(ren, tile_brick_, cx, cy);
        return;
    }
    // Fallback swatches when the ANI art is missing.
    switch (cell) {
        case EditorBrush::Solid: SDL_SetRenderDrawColor(ren, 120, 120, 130, 255); break;
        case EditorBrush::Brick: SDL_SetRenderDrawColor(ren, 150, 90, 40, 255); break;
        default: SDL_SetRenderDrawColor(ren, 40, 90, 40, 255); break;
    }
    SDL_FRect rect{static_cast<float>(kOriginX + x * kCellW),
                   static_cast<float>(kOriginY + y * kCellH), static_cast<float>(kCellW - 1),
                   static_cast<float>(kCellH - 1)};
    SDL_RenderFillRect(ren, &rect);
}

void EditorScreen::draw_grid(SDL_Renderer* ren) const {
    // PINNED (sub_4022A1): every cell first draws the "tile N blank" frame, then
    // a non-blank cell overdraws its solid/brick frame on top, all anchored like
    // the match renderer (hotspot at the cell's bottom-centre, the
    // sub_426524/sub_42655F convention).
    for (int y = 0; y < grid_.height(); ++y)
        for (int x = 0; x < grid_.width(); ++x) draw_cell(ren, x, y);
}

void EditorScreen::draw_brush_preview(SDL_Renderer* ren) const {
    // PINNED (pseudo.c 5518-5524): every frame, AFTER the grid but BEFORE the
    // start markers and status text, the original draws the CURRENT brush's own
    // "tile %d blank/solid/brick" frame (resolved by sub_402206 from the SAME
    // brush-selection variable the 1/2/3/Tab keys write) at the raw mouse pixel
    // read by sub_431804, through the SAME sub_415920 primitive the cells use —
    // the same hotspot anchor, centred on the cursor instead of a cell.
    if (!have_tile_art()) return;
    draw_anim_step(ren, brush_anim(), mouse_px_, mouse_py_);
}

void EditorScreen::draw_start_marker(SDL_Renderer* ren, int slot) const {
    const EditorStart& st = grid_.start(slot);
    const float ccx = static_cast<float>(kOriginX + st.x * kCellW) + kCellW / 2.0f;
    const float cby = static_cast<float>(kOriginY + st.y * kCellH) + kCellH - 1.0f;
    const bool drew_ring = draw_anim_step(ren, teamring_[st.team ? 1 : 0], ccx, cby);
    if (font_ && font_->loaded()) {
        std::array<std::uint8_t, 3> c{255, 255, 255};
        if (assets_) assets_->slot_color(slot, c.data());
        font_->draw(ren, std::to_string(slot + 1), SDL_FPoint{ccx - 20.0f, cby - kCellH},
                    TextStyle{{c[0], c[1], c[2]}});
    }
    if (!drew_ring) {
        // Fallback marker when MISC.ANI is missing: an outline box, team flag as
        // the outline colour (yellow = flagged).
        SDL_FRect marker{static_cast<float>(kOriginX + st.x * kCellW + 4),
                         static_cast<float>(kOriginY + st.y * kCellH + 4),
                         static_cast<float>(kCellW - 8), static_cast<float>(kCellH - 8)};
        SDL_SetRenderDrawColor(ren, 255, 255, st.team ? 0 : 255, 255);
        SDL_RenderRect(ren, &marker);
    }
    if (slot != selected_start_) return;
    SDL_FRect inner{static_cast<float>(kOriginX + st.x * kCellW + 6),
                    static_cast<float>(kOriginY + st.y * kCellH + 6),
                    static_cast<float>(kCellW - 12), static_cast<float>(kCellH - 12)};
    SDL_SetRenderDrawColor(ren, 255, 220, 80, 255);
    SDL_RenderRect(ren, &inner);
}

void EditorScreen::draw_starts(SDL_Renderer* ren) const {
    // PINNED (sub_4028D2's draw loop): each slot draws its number ("%u", i+1) at
    // (cell_centre_x - 20, cell_bottom - 36) in the slot's own ink (sub_41672F ->
    // AssetStore::slot_color; the editor runs with team play zeroed by
    // sub_40330E, so it is always the slot-colour branch), then the "teamring%u"
    // MISC.ANI sprite at (cell_centre_x - 20, cell_bottom). The selected-slot
    // inner box is our own cursor — the original tracks the selection only in the
    // status line.
    for (int i = 0; i < kEditorMaxStarts; ++i) draw_start_marker(ren, i);
}

void EditorScreen::draw_status(SDL_Renderer* ren) const {
    // Status labels (sub_4028D2, all in the general white ink): getstring(742)
    // scheme file at (20,5), getstring(743) density at (20,23), getstring(738)
    // selected start at (20,41), and the getstring(737) exit hint at the bottom
    // (y = 476 - text height). Our single-line summary keeps those ids' CONTENT
    // in one strip.
    const char* brush_name = brush_ == EditorBrush::Solid   ? "SOLID"
                             : brush_ == EditorBrush::Brick ? "BRICK"
                                                            : "BLANK";
    std::array<char, 160> status{};
    std::snprintf(status.data(), status.size(), "BRUSH:%s  START:%d%s  DENSITY:%d  NAME:%s",
                  brush_name, selected_start_ + 1, grid_.start(selected_start_).team ? "[T]" : "",
                  grid_.density(), grid_.name().empty() ? "(none)" : grid_.name().c_str());
    font_->draw(ren, status.data(), SDL_FPoint{20.0f, 5.0f}, TextStyle{kInk});
    font_->draw(ren,
                "1/2/3 BRUSH  TAB CYCLE  CTRL+F FILL  CTRL+B RESET  0 TILESET  +/- START  T TEAM  "
                "D DENSITY  N NAME  P POWERUPS  ESC SAVE/EXIT",
                SDL_FPoint{20.0f, 460.0f}, TextStyle{kHint});
}

void EditorScreen::draw_confirm(SDL_Renderer* ren, int line_id, const char* line_default,
                                int note_id) const {
    draw_confirm_dialog(
        DialogPen{ren, *font_}, assets_ ? &assets_->frontend_pcx("WINZ") : nullptr,
        ConfirmLabels{text_of(assets_, line_id, line_default), text_of(assets_, note_id, ""),
                      text_of(assets_, 26, " Yes "), text_of(assets_, 25, " No ")},
        DialogInk{kDialogInk});
}

void EditorScreen::draw_prompt(SDL_Renderer* ren) const {
    // Every editor prompt routes through the SAME pinned sub_41456C/sub_42E938
    // chrome (dialog_chrome.hpp) the boot LOADING dialog and the main-menu quit
    // confirm use; the earlier per-screen ad hoc black boxes had no basis in the
    // decompile.
    switch (prompt_kind_) {
        case PromptKind::Density:
        case PromptKind::Name: {
            // sub_42E938 text entry (§5 'D'/'N', y=180 CONFIRMED literal, cases
            // 68/78) — label getstring(739)/(728), Done/Cancel hardcoded.
            const bool density = prompt_kind_ == PromptKind::Density;
            const std::string label =
                text_of(assets_, density ? 739 : 728, density ? "DENSITY (0-100):" : "NAME:");
            draw_text_entry_dialog(DialogPen{ren, *font_}, 180.0f,
                                   TextEntryLabels{label, prompt_text_, "Done", "Cancel"});
            return;
        }
        // getstring(735) + getstring(95) — §5's exit case, pseudo.c 5626-5629.
        case PromptKind::SaveConfirm:
            draw_confirm(ren, 735, "Save changes to this scheme?", 95);
            return;
        // getstring(760) + getstring(97) — §5 Ctrl+F, pseudo.c 5601-5604.
        case PromptKind::FillConfirm:
            draw_confirm(ren, 760, "Fill the whole board with the brush?", 97);
            return;
        // getstring(740) + getstring(97) — §5 Ctrl+B, pseudo.c 5587-5590.
        case PromptKind::ResetConfirm:
            draw_confirm(ren, 740, "Reset the board to a blank scheme?", 97);
            return;
        case PromptKind::None: return;
    }
}

void EditorScreen::draw(SDL_Renderer* ren) const {
    if (!ren) return;
    if (editing_powerups_) {
        powerups_screen_.draw(ren);
        return;
    }
    draw_backdrop(ren, assets_, backdrop_);
    draw_grid(ren);
    draw_brush_preview(ren);
    draw_starts(ren);
    if (!font_ || !font_->loaded()) return;
    draw_status(ren);
    draw_prompt(ren);
}

}  // namespace bomber::game
