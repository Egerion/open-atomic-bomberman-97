#include "bomber/frontend/options_screen.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <initializer_list>
#include <string>

#include "bomber/game_util/anim_pace.hpp"
#include "bomber/game_util/hud_format.hpp"

namespace bomber::game {

namespace {

// CONFIRMED layout (docs/re/results-and-options.md §3): VALUELST
// `745,55,40,22,500` -> x=55, y0=40, ystep=22, W=500 (a text-clip WIDTH, not a
// colour). sub_413BD6(getvalue(745)-20, row_y) puts the cursor sprite 20 px left
// of the row text.
constexpr int kListX = 55;
constexpr int kListY0 = 40;
constexpr int kListYStep = 22;
constexpr int kCursorX = kListX - 20;
constexpr float kRowClipW = 500.0f;

// CONFIRMED (§1's LUT decode table): byte_49D38F = general draw ink =
// RGB(255,255,255), used for EVERY row, selected or not — the original has no
// per-row or selected recolour at all; only the cursor1 sprite marks selection.
constexpr Uint8 kInkR = 255, kInkG = 255, kInkB = 255;

// Pinned EMPIRICALLY from a 1:1 native capture of the level screen (2026-07-12;
// VALUELST 736 = 170, measured sprite rows 155..186 -> anchor = row_y + 16, one
// FONT6 cell below the row's y). The decompile loses this term to register
// mangling at every sub_413BD6 call site, so the capture is the authority;
// anchoring at row_y drew the dude a full cell too high.
constexpr int kCursorYNudge = 16;

int playtime_index(int seconds) {
    for (int i = 0; i < kPlayTimeChoiceCount; ++i)
        if (kPlayTimeChoices[i] == seconds) return i;
    return 0;
}

// Wrapping cycler — the shape every "Left decrements / Right increments" row uses.
int cycle(int value, int dir, int count) {
    const int next = value + dir;
    if (next < 0) return count - 1;
    if (next >= count) return 0;
    return next;
}

// Sequential splice for the four-field modem row (getstring(264) "Modem: P:%u
// I:%u  B:%u  #:%s"): each %u/%d/%i/%s in order takes the next argument; %% and
// anything unmatched stays literal. (hud_format's fmt_u/fmt_s fill only the
// first specifier.)
std::string fmt_seq(std::string f, std::initializer_list<std::string> args) {
    std::size_t pos = 0;
    for (const auto& a : args) {
        auto p = f.find('%', pos);
        while (p != std::string::npos && p + 1 < f.size() && f[p + 1] == '%')
            p = f.find('%', p + 2);
        if (p == std::string::npos || p + 1 >= f.size()) break;
        const char c = f[p + 1];
        if (c != 'u' && c != 'd' && c != 'i' && c != 's') break;
        f.replace(p, 2, a);
        pos = p + a.size();
    }
    return f;
}

}  // namespace

void OptionsScreen::enter(const OptionsSnapshot& current, std::string backdrop) {
    row_ = 0;
    done_ = false;
    changed_ = false;
    open_keyremap_ = false;
    open_scheme_picker_ = false;
    open_node_name_prompt_ = false;
    goldman_touched_ = false;
    team_play_touched_ = false;
    snap_ = current;
    snap_.conveyor_speed_index = std::clamp(snap_.conveyor_speed_index, 0, 2);
    snap_.enclosement_depth = std::clamp(snap_.enclosement_depth, 0, 3);
    // The random GLUE<n> backdrop (CONFIRMED sub_4148E5) is picked by the
    // caller's shared pick_glue(), so there is exactly one presentation LCG for
    // the pick across every pre-match screen.
    backdrop_ = std::move(backdrop);
}

void OptionsScreen::toggle(bool& value) {
    value = !value;
    changed_ = true;
}

// §3 row 0: Team Play forces win_by_kills off (a one-way gate — row 0 -> row 5,
// not the reverse). pseudo.c 9310-9311/9410-9412: toggling it ALSO clears
// dword_46492C inline on every press, which is what team_play_touched_ records.
void OptionsScreen::toggle_team_play() {
    toggle(snap_.team_play);
    if (snap_.team_play) snap_.win_by_kills = false;
    team_play_touched_ = true;
}

// sub_4076FE's off-list fallback (pseudo.c 8557-8558): a playtime outside the
// fixed chain snaps to getvalue(100) = 150 instead of stepping. (Live ValueList
// plumbing into this screen is deferred; the shipped literal stands in.)
void OptionsScreen::step_playtime(int dir) {
    const int cur = playtime_index(snap_.playtime_seconds);
    changed_ = true;
    if (kPlayTimeChoices[cur] != snap_.playtime_seconds) {
        snap_.playtime_seconds = 150;
        return;
    }
    snap_.playtime_seconds = kPlayTimeChoices[cycle(cur, dir, kPlayTimeChoiceCount)];
}

// DELIBERATE EXCEPTION to the 40-line ceiling (docs/coding-standards.md §8,
// "leave the switches the 1997 binary invented alone"): a case-for-case mirror of
// sub_4080DC's per-row dispatch with the case index EQUAL to the original's row
// index. Splitting it, or replacing it with a strategy table, destroys the
// property that makes the port checkable against the binary; the length is the
// row count, not tangle.
//
// Toggles ignore `dir`, cyclers step by it, and the sub-screen rows open
// regardless — the original's "every direction reaches the same per-row case"
// table (docs/frontend-options-rows.md "Key dispatch").
void OptionsScreen::activate_row(int dir) {
    switch (static_cast<OptionRow>(row_)) {
        case OptionRow::TeamPlay: toggle_team_play(); break;
        case OptionRow::RandomStart: toggle(snap_.random_start); break;
        case OptionRow::NodeName: open_node_name_prompt_ = true; break;
        case OptionRow::ConveyorSpeed:
            snap_.conveyor_speed_index = cycle(snap_.conveyor_speed_index, dir, 3);
            changed_ = true;
            break;
        case OptionRow::StompedBombs: toggle(snap_.stomped_bombs_detonate); break;
        // §3 row 5: forced off whenever Team Play is on, so the toggle attempt is
        // a no-op while Team Play holds it down.
        case OptionRow::WinByKills:
            if (!snap_.team_play) toggle(snap_.win_by_kills);
            break;
        case OptionRow::GoldBomberman:
            toggle(snap_.goldman);
            goldman_touched_ = true;  // cleared inline on every press, like row 0
            break;
        case OptionRow::EnclosementDepth:
            snap_.enclosement_depth = cycle(snap_.enclosement_depth, dir, 4);
            changed_ = true;
            break;
        case OptionRow::SchemeFile: open_scheme_picker_ = true; break;
        case OptionRow::PlayTime: step_playtime(dir); break;
        case OptionRow::AssignKeyboard: toggle(snap_.assign_keyboards); break;
        case OptionRow::DiseasesDestroy: toggle(snap_.diseases_destroyable); break;
        case OptionRow::LostNetRevertAI: toggle(snap_.lost_net_revert_ai); break;
        case OptionRow::DisableMusic: toggle(snap_.disable_game_music); break;
        // Display-only rows: the original's handlers here (sub_40798B/sub_407F4F
        // nested modem/protocol sub-screens) are real UI this port does not
        // implement, so every direction stays a no-op rather than inventing one.
        case OptionRow::Modem:
        case OptionRow::NetProtocol: break;
        case OptionRow::KeyRemap: open_keyremap_ = true; break;
        case OptionRow::SmallMemory: toggle(snap_.small_memory); break;
        default: break;
    }
}

void OptionsScreen::on_key(SDL_Keycode key, AudioEngine& audio) {
    open_keyremap_ = false;
    open_scheme_picker_ = false;
    open_node_name_prompt_ = false;
    // CONFIRMED (pseudo.c 9298-9299): sub_4080DC plays SFX 20 for ANY real
    // keypress, unconditionally — there is no distinct "accept" sound anywhere in
    // this function. One call here replaces the earlier per-branch 10/20 split.
    audio.play(20);
    switch (key) {
        // The wrap count is kCursorRowCount (18), not kCount — arrow-only
        // (batch_0x4074DC.cpp:860,871); the old W/S aliases were invented.
        case SDLK_UP: row_ = (row_ + kCursorRowCount - 1) % kCursorRowCount; break;
        case SDLK_DOWN: row_ = (row_ + 1) % kCursorRowCount; break;
        case SDLK_LEFT: activate_row(-1); break;
        case SDLK_RIGHT:
        case SDLK_RETURN:
        case SDLK_KP_ENTER:
        case SDLK_SPACE: activate_row(1); break;
        // CONFIRMED (pseudo.c 9374-9378): Escape is the ONLY key that sets
        // sub_4080DC's exit flag, and it does NOT discard an already-made change —
        // the caller persists from changed() on Enter or Esc alike.
        case SDLK_ESCAPE: done_ = true; break;
        default: break;
    }
}

// All 18 rows in the ORIGINAL's order, with label AND value text composed exactly
// as sub_4080DC composes them (pseudo.c 9098-9281; the fallbacks are the shipped
// MESSAGES.TXT's own mixed-case strings). The old hardcoded ALL-CAPS strings,
// their "(N/A)"/"..." suffixes and a 19th "ADJUST AUDIO" row were the port's
// invention — the reported case mismatch against the real screen.
std::array<std::string, static_cast<std::size_t>(OptionRow::kCount)> OptionsScreen::row_text()
    const {
    auto msg = [&](int id, const char* fb) {
        return assets_ ? assets_->getstring(id, fb) : std::string(fb);
    };
    auto yn = [&](bool b) { return msg(b ? 26 : 25, b ? " Yes " : " No "); };
    static constexpr const char* kConveyorFb[3] = {"Low", "Medium", "High"};
    static constexpr const char* kEncloseFb[4] = {"None", "A Little", "A Lot", "All the way!"};
    const int conv = snap_.conveyor_speed_index;
    const int depth = snap_.enclosement_depth;
    const std::string playtime = snap_.playtime_seconds == kPlayTimeUnlimited
                                     ? msg(280, "Infinite")
                                     : format_clock(msg(281, "%u:%02u"), snap_.playtime_seconds);
    return {
        fmt_s(msg(250, "Team Play: %s"), yn(snap_.team_play)),
        fmt_s(msg(251, "Random Start: %s"), yn(snap_.random_start)),
        fmt_s(msg(252, "Node Name: '%s'"), snap_.node_name),
        fmt_s(msg(253, "Conveyor Speed: %s"), msg(295 + conv, kConveyorFb[conv])),
        fmt_s(msg(254, "Stomped Bombs Detonate: %s"), yn(snap_.stomped_bombs_detonate)),
        fmt_s(msg(255, "Win Matches By Kill Total: %s"), yn(snap_.win_by_kills)),
        fmt_s(msg(256, "Gold Bomberman: %s"), yn(snap_.goldman)),
        fmt_s(msg(257, "Enclosement Depth: %s"), msg(315 + depth, kEncloseFb[depth])),
        // Row 8 shows byte_4648C4 VERBATIM — it is the PICKER that normalises it.
        fmt_s(msg(258, "Scheme File: %s"), snap_.scheme_filename),
        fmt_s(msg(259, "Play Time: %s"), playtime),
        fmt_s(msg(260, "Assign Keyboard Player: %s"), yn(snap_.assign_keyboards)),
        fmt_s(msg(261, "Diseases Can Be Destroyed: %s"), yn(snap_.diseases_destroyable)),
        fmt_s(msg(262, "Lost net players revert to AIs: %s"), yn(snap_.lost_net_revert_ai)),
        fmt_s(msg(263, "Disable music during gameplay: %s"), yn(snap_.disable_game_music)),
        fmt_seq(msg(264, "Modem:  P:%u  I:%u  B:%u  #:%s"),
                {std::to_string(snap_.modemport), std::to_string(snap_.modemirq),
                 std::to_string(snap_.modembaud), snap_.modemdial}),
        msg(265, "Define keyboard layouts"),
        msg(266, "Set Default Network Protocol"),
        // CONFIRMED (pseudo.c 9280, `getstring((dword_464824==0)+25)`): the label
        // is the INVERSE of the backing value.
        fmt_s(msg(267, "Use Enhanced Memory Model: %s"), yn(!snap_.small_memory)),
    };
}

void OptionsScreen::draw(SDL_Renderer* ren) const {
    if (!ren) return;
    draw_backdrop(ren);
    if (!font_ || !font_->loaded()) return;
    // No title/header text — CONFIRMED absent from sub_4080DC's body
    // (docs/frontend-options-rows.md "Layout and chrome").
    draw_rows(ren);
    draw_footer(ren);
    draw_cursor(ren);
}

void OptionsScreen::draw_backdrop(SDL_Renderer* ren) const {
    if (!assets_) return;
    const Sprite& bg = assets_->frontend_pcx(backdrop_);
    if (bg.tex == nullptr) {
        // Missing backdrop art: clear to a dark panel so text stays legible
        // rather than drawing over stale contents.
        SDL_SetRenderDrawColor(ren, 20, 20, 30, 255);
        SDL_RenderClear(ren);
        return;
    }
    SDL_FRect d{0, 0, static_cast<float>(bg.w), static_cast<float>(bg.h)};
    SDL_RenderTexture(ren, bg.tex, nullptr, &d);
}

// Every frontend string goes through the 4-pass-outline primitive (sub_41696C):
// white ink over a 1-px black outline, clipped to VALUELST 745's column-3 width.
// The un-outlined draw this replaced was the port's contrast problem over light
// GLUE backdrops.
void OptionsScreen::draw_rows(SDL_Renderer* ren) const {
    const auto rows = row_text();
    for (std::size_t i = 0; i < rows.size(); ++i) {
        const float y = static_cast<float>(kListY0 + static_cast<int>(i) * kListYStep);
        font_->draw_outlined(ren, rows[i], static_cast<float>(kListX), y, kInkR, kInkG, kInkB, 0, 0,
                             0, kRowClipW);
    }
}

// Footer (sub_413FB9 -> getstring(330), pseudo.c 9295): centred via sub_4172BA's
// x = cx - (w+2)/2 at the VALUELST 790 anchor, cyan byte_497F8F over black. The
// anchor uses the shipped row's literal values because plumbing the live
// ValueList into this screen is deferred.
void OptionsScreen::draw_footer(SDL_Renderer* ren) const {
    if (!assets_) return;
    const std::string help = assets_->getstring(330, "Press F1 for help");
    const float help_w = static_cast<float>(font_->measure(help));
    font_->draw_outlined(ren, help, 320.0f - (help_w + 2.0f) / 2.0f, 440.0f, 96, 252, 252, 0, 0, 0);
}

// The selection cursor — CONFIRMED sub_413BD6: the "cursor1" MISC.ANI sprite at
// (x-20, row_y + kCursorYNudge), NOT a text recolour. row_ is always in
// [0, kCursorRowCount). Pacing: idle on step 0, timed blink one step per rendered
// frame (cursor_indicator.hpp).
void OptionsScreen::draw_cursor(SDL_Renderer* ren) const {
    if (!assets_) return;
    const Anim cur = resolve_sequence(assets_->misc(), "cursor1");
    const float row_y = static_cast<float>(kListY0 + row_ * kListYStep);
    if (cur.steps.empty()) {
        // Missing MISC.ANI: a plain marker keeps the selected row legible rather
        // than silently losing its cursor.
        font_->draw(ren, ">", static_cast<float>(kCursorX), row_y, kInkR, kInkG, kInkB);
        return;
    }
    const std::size_t step = anim_step_index(
        cursor_blink_.step(blink_now_s_, cur.steps.size(), blink_base_s_, blink_spread_s_),
        cur.steps.size());
    const Sprite& sp = cur.steps[step];
    if (sp.tex == nullptr) return;
    SDL_FRect d{static_cast<float>(kCursorX - sp.hx),
                row_y + static_cast<float>(kCursorYNudge - sp.hy), static_cast<float>(sp.w),
                static_cast<float>(sp.h)};
    SDL_RenderTexture(ren, sp.tex, nullptr, &d);
}

}  // namespace bomber::game
