#include "bomber/game/options_screen.hpp"

#include <cstdio>
#include <initializer_list>
#include <string>

#include "bomber/game/anim_pace.hpp"
#include "bomber/game/hud_format.hpp"

namespace bomber::game {

namespace {

// CONFIRMED layout (docs/re/results-and-options.md §3): VALUELST
// `745,55,40,22,500` -> x=55, y0=40, ystep=22, W=500 (a text-clip width, not
// a colour — see options_screen.hpp's file doc).
constexpr int kListX = 55;
constexpr int kListY0 = 40;
constexpr int kListYStep = 22;
// CONFIRMED: sub_413BD6(getvalue(745)-20, row_y) — the cursor sprite sits
// 20px left of the row text's own x.
constexpr int kCursorX = kListX - 20;

// CONFIRMED (docs/re/results-and-options.md §1's LUT decode table):
// byte_49D38F = general draw ink = RGB(255,255,255), used for EVERY row,
// selected or not — the original has no per-row/selected recolour at all,
// only the separate cursor1 sprite (drawn in draw() below) marks selection.
constexpr Uint8 kInkR = 255, kInkG = 255, kInkB = 255;

int playtime_index(int seconds) {
    for (int i = 0; i < kPlayTimeChoiceCount; ++i)
        if (kPlayTimeChoices[i] == seconds) return i;
    return 0;
}

// Crash-proof single-%s splice (the same rule as game_app.cpp's fmt_s: the
// format string is the user's own MESSAGES.TXT, so an unexpected specifier
// stays literal rather than risking a wrong-type sprintf).
std::string fmt_s(const std::string& f, const std::string& v) {
    auto p = f.find('%');
    if (p == std::string::npos) return f;
    std::size_t q = p + 1;
    while (q < f.size() && f[q] != 'u' && f[q] != 'd' && f[q] != 'i' && f[q] != 's' && f[q] != '%')
        ++q;
    if (q < f.size() && f[q] == 's') return f.substr(0, p) + v + f.substr(q + 1);
    return f;
}

// Sequential splice for the four-field modem row (getstring(264) "Modem:
// P:%u  I:%u  B:%u  #:%s"): each %u/%d/%i/%s in order takes the next
// argument; %% and anything unmatched stays literal.
std::string fmt_seq(std::string f, std::initializer_list<std::string> args) {
    std::size_t pos = 0;
    for (const auto& a : args) {
        auto p = f.find('%', pos);
        while (p != std::string::npos && p + 1 < f.size() && f[p + 1] == '%')
            p = f.find('%', p + 2);
        if (p == std::string::npos || p + 1 >= f.size()) break;
        const char c = f[p + 1];
        if (c != 'u' && c != 'd' && c != 'i' && c != 's') break;
        f = f.substr(0, p) + a + f.substr(p + 2);
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

// CONFIRMED 2026-07-09 re-read of sub_4080DC's key-dispatch tail (pseudo.c
// 9297-9406): `v165` is the raw key code; the ONLY branch that sets the
// loop's exit flag (`v167 = 1`) is `v165 == 0x1B` (Escape) — Enter (13) and
// Space (32) both `goto LABEL_29`, the EXACT SAME per-row switch Right
// (`v165 == 0x14D`) dispatches to (toggle / cycle-forward / open sub-screen).
// Left (`v165 == 0x14B`) runs a second, separately-listed switch with the
// SAME 19 cases: toggles do the SAME toggle (direction is ignored for them,
// matching this file's existing per-row comments), cyclers decrement instead
// of increment, and every "opens a sub-screen" row (2/8/14/15/16/17/18)
// dispatches the SAME open action Right/Enter/Space use — e.g. row 15
// ("Define keyboard layouts", sub_407B9D) opens via ALL FOUR of Left, Right,
// Enter, and Space in the original, not Enter-only. The port's previous
// `done_ = true` default on Enter/Space (falling out of the screen on any
// non-KeyRemap row) had no such branch in sub_4080DC at all — it was
// invented, and is the reported "rows drop you back to the main menu" bug.
// Escape is the ONLY key that ends the screen.
void OptionsScreen::activate_row(int dir) {
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
        case OptionRow::NodeName:
        case OptionRow::Modem:
        case OptionRow::NetProtocol:
            // Display-only rows (options_screen.hpp's file doc) — the
            // original's handlers here (sub_4074DC text-entry prompt,
            // sub_40798B/sub_407F4F nested net sub-screens) are real UI this
            // port does not implement; every direction (Left/Right/Enter/
            // Space all reach this same case in the original) stays a no-op
            // here rather than inventing one.
            break;
        case OptionRow::SchemeFile:
            // CONFIRMED (pseudo.c 9342-9343 `goto LABEL_46` in the forward
            // switch, 9443-9445 in the Left switch): BOTH directions open
            // sub_407582 — the *.SCH file-picker list dialog. (The §3
            // table's earlier "sub_4076FE(±1) stepper" label for this row
            // described the PLAY TIME stepper, not this handler.)
            open_scheme_picker_ = true;
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
            // sub_4076FE's off-list fallback (pseudo.c 8557-8558): a
            // playtime outside the fixed chain (e.g. a hand-edited
            // options.ini value the reader's [60,600] clamp let through)
            // snaps to getvalue(100) — 150 in the shipped VALUELST —
            // instead of stepping. (Live ValueList plumbing into this
            // screen is deferred like the footer anchor; the shipped
            // literal stands in.)
            const int cur = playtime_index(snap_.playtime_seconds);
            if (kPlayTimeChoices[cur] != snap_.playtime_seconds) {
                snap_.playtime_seconds = 150;
                changed_ = true;
                break;
            }
            int idx = cur + dir;
            if (idx < 0) idx = kPlayTimeChoiceCount - 1;
            if (idx >= kPlayTimeChoiceCount) idx = 0;
            snap_.playtime_seconds = kPlayTimeChoices[idx];
            changed_ = true;
            break;
        }
        case OptionRow::AssignKeyboard:
            snap_.assign_keyboards = !snap_.assign_keyboards;
            changed_ = true;
            break;
        case OptionRow::DiseasesDestroy:
            snap_.diseases_destroyable = !snap_.diseases_destroyable;
            changed_ = true;
            break;
        case OptionRow::LostNetRevertAI:
            snap_.lost_net_revert_ai = !snap_.lost_net_revert_ai;
            changed_ = true;
            break;
        case OptionRow::DisableMusic:
            snap_.disable_game_music = !snap_.disable_game_music;
            changed_ = true;
            break;
        case OptionRow::KeyRemap:
            // CONFIRMED (pseudo.c case 15, both the LABEL_29 forward switch
            // AND the Left-arrow switch `goto LABEL_53`): all four of
            // Left/Right/Enter/Space open the remap sub-screen — not
            // Enter/Space only.
            open_keyremap_ = true;
            break;
        case OptionRow::SmallMemory:
            snap_.small_memory = !snap_.small_memory;
            changed_ = true;
            break;
        default: break;
    }
}

void OptionsScreen::on_key(SDL_Keycode key, AudioEngine& audio) {
    open_keyremap_ = false;
    open_scheme_picker_ = false;
    // CONFIRMED (pseudo.c 9298-9299): sub_4080DC plays SFX 20 (nav blip) for
    // ANY real keypress, unconditionally — there is no distinct "accept"
    // sound anywhere in this function. One call here covers every branch
    // below, replacing the earlier per-branch audio.play(10)/(20) split.
    audio.play(20);
    switch (key) {
        case SDLK_UP:
        case SDLK_W:
            // CONFIRMED off-by-one (pseudo.c 9086/9391-9392, `v168 = 18`):
            // wraps over kCursorRowCount (18), not kCount (19) — row 18 is
            // never reachable. See options_screen.hpp's file doc.
            row_ = (row_ + kCursorRowCount - 1) % kCursorRowCount;
            break;
        case SDLK_DOWN:
        case SDLK_S: row_ = (row_ + 1) % kCursorRowCount; break;
        case SDLK_LEFT: activate_row(-1); break;
        case SDLK_RIGHT:
        case SDLK_RETURN:
        case SDLK_KP_ENTER:
        case SDLK_SPACE:
            // CONFIRMED: Right/Enter/Space are the SAME "forward" dispatch
            // in the original (see activate_row's file doc above) — none of
            // them end the screen.
            activate_row(1);
            break;
        case SDLK_ESCAPE:
            // Esc leaves without discarding an already-made change — the caller
            // (present_options_screen) decides whether to persist based on
            // changed(), same on Enter or Esc. Only the *screen's* dismissal
            // semantics differ (Back vs Advance) for the app-flow graph.
            // CONFIRMED (pseudo.c 9374-9378, `v165 <= 0x1B` -> `v167 = 1`):
            // Escape is the ONLY key that sets sub_4080DC's own exit flag.
            done_ = true;
            break;
        default: break;
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

    // No title/header text — CONFIRMED absent from sub_4080DC's body (see
    // options_screen.hpp's file doc); the previous "OPTIONS" header at a
    // guessed (55,20) had no RE citation and has been removed.

    // All 18 rows, in the ORIGINAL's exact order, with label AND value text
    // from MESSAGES.TXT exactly as sub_4080DC composes them (chrome audit
    // 2026-07-12, pseudo.c 9098-9281; fallbacks below are the shipped file's
    // own mixed-case strings): labels getstring(250..267); Yes/No via
    // getstring(26/25) WITH their leading/trailing padding spaces; conveyor
    // getstring(295+idx); enclosement getstring(315+idx) (note "All the
    // way!"); play time via getstring(280) "Infinite" or getstring(281)
    // "%u:%02u" M:SS; node name = the runtime buffer (EMPTY by default —
    // sub_40FE34's bss unk_460140, not an options.ini key) quoted by
    // getstring(252); the four-field modem line getstring(264) from the
    // options.ini modem keys; rows 265/266 carry NO value suffix. The old
    // hardcoded ALL-CAPS strings (and their "(N/A)"/"..." suffixes, and a
    // 19th "ADJUST AUDIO" row) were the port's invention — the reported
    // case/contrast mismatch against the real screen.
    auto msg = [&](int id, const char* fb) {
        return assets_ ? assets_->getstring(id, fb) : std::string(fb);
    };
    auto yn = [&](bool b) { return msg(b ? 26 : 25, b ? " Yes " : " No "); };
    static constexpr const char* kConveyorFb[3] = {"Low", "Medium", "High"};
    static constexpr const char* kEncloseFb[4] = {"None", "A Little", "A Lot", "All the way!"};
    const int conv = snap_.conveyor_speed_index;
    const int depth = snap_.enclosement_depth;
    // Row 8 shows byte_4648C4 VERBATIM (sub_4080DC's draw sprintf's the
    // buffer as-is; it is the PICKER that stores it extension-stripped +
    // uppercased). The port's old display-time extension strip was invented.
    const std::string& scheme = snap_.scheme_filename;
    const std::string playtime =
        snap_.playtime_seconds == 1001
            ? msg(280, "Infinite")
            : format_clock(msg(281, "%u:%02u"), snap_.playtime_seconds);
    std::string rows[static_cast<int>(OptionRow::kCount)] = {
        fmt_s(msg(250, "Team Play: %s"), yn(snap_.team_play)),
        fmt_s(msg(251, "Random Start: %s"), yn(snap_.random_start)),
        fmt_s(msg(252, "Node Name: '%s'"), snap_.node_name),
        fmt_s(msg(253, "Conveyor Speed: %s"), msg(295 + conv, kConveyorFb[conv])),
        fmt_s(msg(254, "Stomped Bombs Detonate: %s"), yn(snap_.stomped_bombs_detonate)),
        fmt_s(msg(255, "Win Matches By Kill Total: %s"), yn(snap_.win_by_kills)),
        fmt_s(msg(256, "Gold Bomberman: %s"), yn(snap_.goldman)),
        fmt_s(msg(257, "Enclosement Depth: %s"), msg(315 + depth, kEncloseFb[depth])),
        fmt_s(msg(258, "Scheme File: %s"), scheme),
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
        // CONFIRMED (pseudo.c 9280, `getstring((dword_464824==0)+25)`): the
        // label is the INVERSE of the backing value — small_memory==false
        // (Enhanced Memory Model in effect) shows Yes.
        fmt_s(msg(267, "Use Enhanced Memory Model: %s"), yn(!snap_.small_memory)),
    };

    for (int i = 0; i < static_cast<int>(OptionRow::kCount); ++i) {
        float y = static_cast<float>(kListY0 + i * kListYStep);
        // Every frontend string goes through the 4-pass-outline primitive
        // (sub_41696C; FontTextures::draw_outlined's doc) — white ink over a
        // 1-px black outline, clipped to VALUELST 745's column-3 width (500).
        // The un-outlined draw this replaced was the port's contrast problem
        // over light GLUE backdrops.
        font_->draw_outlined(ren, rows[i], static_cast<float>(kListX), y, kInkR, kInkG, kInkB, 0,
                             0, 0, 500.0f);
    }

    // Footer (sub_413FB9 -> getstring(330), drawn on this screen too —
    // sub_4080DC calls it right after the cursor blit at pseudo.c 9295):
    // centred via sub_4172BA's x = cx - (w+2)/2 at the VALUELST 790 anchor
    // (320, 440), cyan byte_497F8F (96,252,252) over black. The anchor uses
    // the shipped row's literal values; the setup screen reads them live —
    // plumbing the ValueList into OptionsScreen is deferred to the
    // MESSAGES-label pass.
    if (assets_) {
        const std::string help = assets_->getstring(330, "Press F1 for help");
        const float help_w = static_cast<float>(font_->measure(help));
        font_->draw_outlined(ren, help, 320.0f - (help_w + 2.0f) / 2.0f, 440.0f, 96, 252, 252, 0,
                             0, 0);
    }

    // The selection cursor — CONFIRMED sub_413BD6: the "cursor1" MISC.ANI
    // sprite at (x-20, row_y + 16), NOT a text recolour (see
    // options_screen.hpp's file doc). row_ is always in [0, kCursorRowCount),
    // so it never lands on the permanently-unreachable row 18. Pacing: idle
    // on step 0, timed blink one step per rendered frame
    // (cursor_indicator.hpp — the old continuous /6 spin was a placeholder).
    //
    // The +16 y nudge is pinned EMPIRICALLY from a 1:1 native capture of the
    // level screen (2026-07-12; VALUELST 736 = 170, measured sprite rows
    // 155..186 → anchor = row_y + 16, one FONT6 cell below the row's y): the
    // hotspot-anchored dude's feet stand just under the row text's baseline,
    // its head reaching ~18 px above. The decompile loses this +16 term to
    // register mangling at every sub_413BD6 call site, so the capture is the
    // authority; anchoring at row_y drew the dude a full cell too high (the
    // user-reported misalignment).
    if (assets_) {
        Anim cur = resolve_sequence(assets_->misc(), "cursor1");
        if (!cur.steps.empty()) {
            const std::size_t step = anim_step_index(
                cursor_blink_.step(blink_now_s_, cur.steps.size(), blink_base_s_, blink_spread_s_),
                cur.steps.size());
            const Sprite& sp = cur.steps[step];
            if (sp.tex) {
                float cy = static_cast<float>(kListY0 + row_ * kListYStep + 16);
                SDL_FRect d{static_cast<float>(kCursorX - sp.hx), cy - static_cast<float>(sp.hy),
                            static_cast<float>(sp.w), static_cast<float>(sp.h)};
                SDL_RenderTexture(ren, sp.tex, nullptr, &d);
            }
        } else {
            // Missing MISC.ANI: fall back to a plain marker so the selected
            // row stays legible rather than silently losing its cursor.
            float cy = static_cast<float>(kListY0 + row_ * kListYStep);
            font_->draw(ren, ">", static_cast<float>(kCursorX), cy, kInkR, kInkG, kInkB);
        }
    }
}

}  // namespace bomber::game
