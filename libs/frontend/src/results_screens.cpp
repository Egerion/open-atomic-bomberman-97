#include "bomber/frontend/results_screens.hpp"

#include <SDL3/SDL.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>

#include "bomber/frontend/goldman_screen.hpp"  // GoldmanScreen (+ kWheelSegmentSteps)
#include "bomber/frontend/outcome_tier.hpp"    // kResultsDwellMs
#include "bomber/game_util/hud_format.hpp"     // fmt_u / fmt_s
#include "bomber/game_util/match_outcome.hpp"  // round_winner / is_team_mode / match_clinch / auto_advance_results
#include "bomber/platform/frame_clock.hpp"  // platform::FrameClock
#include "bomber/render/sprites.hpp"        // Sprite
#include "bomber/ui/help_screens.hpp"       // HelpBrowserScreen (the wheel's F1)

namespace bomber::game {

// kResultsDwellMs (sub_42A3F6's 6 s auto-advance dwell for an all-AI/attract
// RESULTS loop; a human match waits for Enter — auto_advance_results()) comes
// from screens/outcome_tier.hpp, which owns the whole tier's shared vocabulary.
// This file used to keep a second copy alongside game_app.cpp's.
//
// There is no music id here: 1020 is the SETUP-SCREENS backdrop track
// (docs/re/in-match-shell.md §2) and the goldman wheel genuinely inherits it
// from the Play handler sub_42A3F6 rather than starting it, so game_app.cpp is
// its only owner (docs/re/sound-engine.md §9).

namespace {

// The scoreboard's VALUELST anchors, read once — a parameter object (§3) rather
// than the seven consecutive locals that used to open run().
struct ScoreboardLayout {
    float hx, hy;           // header 780
    float rx, ry0, rystep;  // tally rows 785
    float ox, oy;           // outcome line 800
};

ScoreboardLayout read_layout(const assets::res::ValueList& v) {
    return ScoreboardLayout{
        static_cast<float>(v.column_or(780, 0, 150)), static_cast<float>(v.column_or(780, 1, 140)),
        static_cast<float>(v.column_or(785, 0, 150)), static_cast<float>(v.column_or(785, 1, 210)),
        static_cast<float>(v.column_or(785, 2, 20)),  static_cast<float>(v.column_or(800, 0, 150)),
        static_cast<float>(v.column_or(800, 1, 94))};
}

// getvalue(783) is a colour index in the original — resolved:
// docs/re/results-and-options.md's "screen-ink byte globals" pin. byte_49D38F
// (general draw ink) is an offset into the shared RGB555 -> palette-index LUT
// (byte_495390), decoding to RGB555 (31,31,31) = white; verified against the
// install's FIELD0/5/10/MAINMENU.PCX palettes (nearest entry (255,255,255),
// dist2=0 on all four).
constexpr Uint8 kHeaderR = 255, kHeaderG = 255, kHeaderB = 255;

// fmt_u only substitutes the FIRST specifier; this splices the remaining ones in
// by hand so the RE'd format string still reads naturally with real fallback
// text. Used for getstring(31)'s score and kills.
void splice_next(std::string& f, int v) {
    const std::size_t p = f.find('%');
    if (p == std::string::npos) return;
    std::size_t q = p + 1;
    while (q < f.size() && f[q] != 'u' && f[q] != 'd' && f[q] != 'i') ++q;
    if (q < f.size()) f = f.substr(0, p) + std::to_string(v) + f.substr(q + 1);
}

// The between-round RESULTS screen's frame loop, as its own object. nullopt from
// a phase method means "keep looping"; a value means run() returns it now.
class ScoreboardLoop {
public:
    ScoreboardLoop(ScreenContext ctx, ScoreboardState state)
        : ctx_(ctx),
          state_(state),
          layout_(read_layout(ctx.values)),
          // Team mode + the §1 v73 match-clinch check — factored into
          // is_team_mode() / match_clinch() (game_app.hpp) so run_app's Results
          // handler (the VICTORY-vs-scoreboard decision) and this render agree on
          // the exact same predicate, including the win_by_kills branch
          // (docs/re/results-and-options.md §3 row 5, now live).
          team_mode_(is_team_mode(state.team_play, state.state, state.setup_team)),
          clinched_player_(match_clinch(state.state, state.team_play, state.setup_team,
                                        state.options.win_by_kills, state.kill_count,
                                        state.win_count, state.win_target)),
          start_(SDL_GetTicks()) {}

    AppInput run();

private:
    std::optional<AppInput> pump_events();
    void pump_gate_or_dwell();
    void draw_frame();
    void draw_backdrop();
    void draw_header();
    void draw_team_rows();
    void draw_player_rows();
    void draw_outcome();

    ScreenContext ctx_;
    ScoreboardState state_;
    ScoreboardLayout layout_;
    AppInput result_ = AppInput::Advance;
    bool team_mode_;
    int clinched_player_;
    std::uint64_t start_;
    bool waiting_ = true;
};

AppInput ScoreboardLoop::run() {
    while (waiting_) {
        if (const std::optional<AppInput> exit = pump_events()) return *exit;
        pump_gate_or_dwell();
        // The frame still draws on the pass that cleared `waiting_`, exactly as
        // the original's `while (waiting)` did.
        ctx_.audio.update_music();
        draw_frame();
        SDL_RenderPresent(ctx_.sdl);
        SDL_Delay(2);
    }
    return result_;
}

std::optional<AppInput> ScoreboardLoop::pump_events() {
    SDL_Event ev;
    while (SDL_PollEvent(&ev)) {
        if (ev.type == SDL_EVENT_QUIT) return AppInput::Quit;
        if (ev.type != SDL_EVENT_KEY_DOWN) continue;
        // sub_42A3F6's RESULTS tally loop @0x42ADE9 — NOT sub_42A088's. Any real
        // key blips (0x42AE04); the accept codes 13/32 reach the sting at
        // 0x42AEF5; Escape (0x42AE9E) does NOT — it sets the abort flag
        // `dword_464A68 = 2`, raises done and jumps to the loop tail without
        // touching the sound engine.
        ctx_.audio.play(20);
        const bool escape = ev.key.key == SDLK_ESCAPE;
        // ONLINE between-rounds (net_round_gate.hpp): the original's RESULTS wait
        // loop is dismissed by the machine driving the game, never by a client — a
        // `sub_40C06A() == 1` peer that presses a key gets the SFX-40 buzz, and the
        // accept set carries a network-only code (903) the host injects. So a
        // GUEST's key does not leave, and the HOST's accept commits the next round
        // but leaves the screen up until the peer holds it (ready(), below).
        // Escape is exempt on both: abandoning the match stays local.
        if (state_.net_gate != nullptr && !escape) {
            if (state_.net_gate->readonly())
                ctx_.audio.play(40);
            else
                state_.net_gate->accept();
            continue;
        }
        if (!escape) ctx_.audio.play(10);  // Escape leaves silently
        result_ = escape ? AppInput::Back : AppInput::Advance;
        waiting_ = false;
        // Deliberately NOT a break: the original drains the rest of this frame's
        // queue after raising its done flag.
    }
    return std::nullopt;
}

void ScoreboardLoop::pump_gate_or_dwell() {
    if (state_.net_gate != nullptr) {
        // The gate owns the ONLY pump of the transport for this screen's duration
        // (setup_session.hpp's one-pump-at-a-time rule) — and, as a side effect,
        // drains the socket of the round that just ended before the next round's
        // session ever looks at it.
        state_.net_gate->pump();
        // ready() is tested FIRST and wins outright: a gate that has both held the
        // peer and then seen it drop still advances, which is what the original's
        // else-if did and what keeps the two peers leaving on the same round.
        if (state_.net_gate->ready()) {
            waiting_ = false;
            return;
        }
        if (state_.net_gate->failed()) {
            result_ = AppInput::Back;
            waiting_ = false;
        }
        return;
    }
    if (!auto_advance_results(state_.demo, state_.demo_ticks, state_.demo_shots, state_.setup_type))
        return;
    if (SDL_GetTicks() - start_ < kResultsDwellMs) return;
    // sub_42A3F6's RESULTS loop only auto-advances after 6 s for an all-AI/attract
    // roster; a human match waits for Enter (auto_advance_results()). An online
    // match always has a human, and both peers must leave together — so the dwell
    // never applies there.
    //
    // The timeout is not a silent exit: @0x42AE4C it forces key = 13, which falls
    // straight into the accept path and plays the sting. It does NOT blip, because
    // the override lands AFTER the blip test and the key it replaced was -1 (no
    // key). So: 10 alone. The port used to advance in complete silence here.
    ctx_.audio.play(10);
    waiting_ = false;
}

void ScoreboardLoop::draw_frame() {
    draw_backdrop();
    draw_header();
    if (team_mode_)
        draw_team_rows();
    else
        draw_player_rows();
    draw_outcome();
}

void ScoreboardLoop::draw_backdrop() {
    SDL_SetRenderDrawColor(ctx_.sdl, 0, 0, 0, 255);
    SDL_RenderClear(ctx_.sdl);
    const Sprite& bg = ctx_.assets.frontend_pcx("RESULTS");
    if (bg.tex == nullptr) return;
    SDL_FRect dst{0.0f, 0.0f, static_cast<float>(bg.w), static_cast<float>(bg.h)};
    SDL_RenderTexture(ctx_.sdl, bg.tex, nullptr, &dst);
}

// Header text (getstring(30), "Game Winner was %s !"), drawn once per round —
// the winner named is this ROUND's winner (round_winner()), not necessarily the
// player who clinched the whole match. (This screen IS one round's worth of
// display, so we always draw it; sub_42A3F6's "!dword_464AEC" gate is about not
// re-drawing across frames of the SAME round, which our per-round call already
// satisfies.) sub_41696C (batch_0x4293E5.cpp:1171) — outlined, like every
// scoreboard string; the color2 outline is byte_495390[0] = black.
void ScoreboardLoop::draw_header() {
    const int round_w = round_winner(state_.state);
    const std::string header =
        fmt_s(ctx_.assets.getstring(30, "Game Winner was %s !"),
              round_w >= 0 ? "P" + std::to_string(round_w + 1) : std::string("-"));
    ctx_.front_font.draw_outlined(ctx_.sdl, header, layout_.hx, layout_.hy, kHeaderR, kHeaderG,
                                  kHeaderB, 0, 0, 0);
}

// Team rows use the original's fixed two-ink helper sub_4141F8 (@0x4141F8,
// `team ? byte_49D0DA : byte_49D38F`) — both inks are RGB555 offsets into the
// byte_495390 LUT (results-and-options.md §1 "screen-ink byte globals"): team 0 =
// the general white ink (31,31,31), team != 0 = red (31,10,10) -> (252,80,80)
// against the install's shared UI palette entries.
void ScoreboardLoop::draw_team_rows() {
    const sim::State& s = state_.state;
    std::array<bool, sim::kMaxPlayers> team_drawn{};
    int row = 0;
    for (int i = 0; i < sim::kMaxPlayers; ++i) {
        if (!s.players[i].present) continue;
        const int t = state_.setup_team[i];
        if (t < 0 || t >= sim::kMaxPlayers || team_drawn[t]) continue;
        team_drawn[t] = true;
        // getstring(38) carries one %u (team number); splice the score in after it
        // manually since fmt_u only substitutes the first.
        std::string line =
            fmt_u(ctx_.assets.getstring(38, "Team %u score: %u"), static_cast<unsigned>(t + 1));
        line += " " + std::to_string(state_.win_count[i]);
        const bool team1 = t != 0;  // sub_4141F8's `a1 ?` branch
        ctx_.front_font.draw_outlined(
            ctx_.sdl, line, layout_.rx, layout_.ry0 + layout_.rystep * static_cast<float>(row),
            static_cast<Uint8>(team1 ? 252 : 255), static_cast<Uint8>(team1 ? 80 : 255),
            static_cast<Uint8>(team1 ? 80 : 255), 0, 0, 0);
        ++row;
    }
}

// Non-team rows keep the slot's own ink (sub_41672F -> AssetStore::slot_color,
// docs/re/player-colour.md). getstring(31) "Player %u score: %u (kills: %d)" —
// two independent counters (§1): win_count_ (match score) and kill_count_
// (cumulative match kills, NOT round kills despite the string's "kills" label —
// see kill_count_'s declaration comment in game_app.hpp for the §1 citation;
// tallied every tick from PlayerDied events, self-kills excluded per our
// documented semantics).
void ScoreboardLoop::draw_player_rows() {
    const sim::State& s = state_.state;
    int row = 0;
    for (int i = 0; i < sim::kMaxPlayers; ++i) {
        if (!s.players[i].present) continue;
        std::string line =
            fmt_u(ctx_.assets.getstring(31, "Player %u score: %u (kills: %d)"), i + 1);
        splice_next(line, state_.win_count[i]);
        splice_next(line, state_.kill_count[i]);
        std::uint8_t c[3];
        ctx_.assets.slot_color(i, c);
        // Outline colour = sub_416867(i) (batch_0x415C1F.cpp:394): in solo mode
        // player 1 (the BLACK bomberman, index 1) gets a WHITE outline
        // (byte_49D38F) so its dark ink stays legible; everyone else gets black
        // (byte_495390[0]). (Team rows + header + outcome are always black.)
        const std::uint8_t ol = i == 1 ? 255 : 0;
        ctx_.front_font.draw_outlined(ctx_.sdl, line, layout_.rx,
                                      layout_.ry0 + layout_.rystep * static_cast<float>(row), c[0],
                                      c[1], c[2], ol, ol, ol);
        ++row;
    }
}

// Outcome line inks — pinned (results-and-options.md §1 "screen-ink byte
// globals"): byte_49A624 ("still playing") and byte_497F8F ("match over") are
// RGB555 offsets into the byte_495390 LUT, decoding to (20,20,20) mid-grey and
// (10,31,31) cyan; resolved to (168,168,164) and (96,252,252) against the
// install's shared UI palette entries (identical across FIELD0/5/10 +
// MAINMENU.PCX).
void ScoreboardLoop::draw_outcome() {
    std::string outcome;
    std::uint8_t oc[3];
    if (clinched_player_ < 0) {
        // Pre-clinch line (batch_0x4293E5.cpp:1262-1263): getstring(dword_46497C +
        // 120) formatted with the FLAT target read straight from dword_464A7C —
        // NOT the remaining count. The strings are 120 "(Match winner must score
        // %u victories)" / 121 "(... %u kills)", so the id keys off win_by_kills (a
        // non-team feature), not team mode, and always shows the total goal.
        const std::string fmt =
            state_.options.win_by_kills
                ? ctx_.assets.getstring(121, "(Match winner must score %u kills)")
                : ctx_.assets.getstring(120, "(Match winner must score %u victories)");
        outcome = fmt_u(fmt, static_cast<unsigned>(state_.win_target));
        oc[0] = 168;
        oc[1] = 168;
        oc[2] = 164;  // byte_49A624: RGB555 (20,20,20) grey
    } else {
        // Clinch line (batch_0x4293E5.cpp:1300-1310): win_by_kills ->
        // getstring(36) "PLAYER %u WINS THE MATCH!" with the winning player NUMBER
        // (the clinch winner's index + 1); else getstring(35) "%s WINS THE MATCH!"
        // with the winner name. The native has NO team-specific win string here —
        // the former `team_mode ? "TEAM %u WINS"` gate AND that fallback text were
        // both invented (id 36 is "PLAYER %u", and the selector is win_by_kills,
        // not team mode).
        if (state_.options.win_by_kills)
            outcome = fmt_u(ctx_.assets.getstring(36, "PLAYER %u WINS THE MATCH!"),
                            static_cast<unsigned>(clinched_player_ + 1));
        else
            outcome = fmt_s(ctx_.assets.getstring(35, "%s WINS THE MATCH!"),
                            "P" + std::to_string(clinched_player_ + 1));
        oc[0] = 96;
        oc[1] = 252;
        oc[2] = 252;  // byte_497F8F: RGB555 (10,31,31) cyan
    }
    ctx_.front_font.draw_outlined(ctx_.sdl, outcome, layout_.ox, layout_.oy, oc[0], oc[1], oc[2], 0,
                                  0, 0);
}

}  // namespace

// The between-round RESULTS cumulative-tally screen (sub_42A3F6 tail,
// docs/re/results-and-options.md §1): RESULTS.PCX backdrop, a header drawn
// once per round, one row per active player/team with a win-count + kill-
// count tally in per-player ink, and an outcome line reporting either "still
// need N" (match not yet clinched) or "wins the match" (clinched). Any key
// (or the 6 s idle dwell) dismisses it; the caller then starts the next round
// or, if the outcome line reports a clinch, the flow instead shows the
// VICTORY screen and never reaches this scoreboard (run_app's Results case).
AppInput ScoreboardScreen::run() {
    return ScoreboardLoop(ctx_, state_).run();
}

// The Goldman Roulette wheel (docs/re/goldman-roulette.md), sub_4034BC. Run
// from run_app's Menu/StartMatch handler, BEFORE present_setup — the exact
// gate order at the head of sub_410F81 (doc §2): !attract (this port has no
// attract-mode match yet, so that leg is always true) && goldman option on
// && local game (always true, no network play) && a gold player pending
// (gold_player_ >= 0 — doc's re-entry check re-derived from sub_4034BC's own
// internal guard, "with no pending gold player the function is a silent
// no-op"). The caller (run_app) is expected to have already checked
// options_.goldman && gold_player_ >= 0 before calling this, matching the
// doc's gate order; this function itself only runs the spin/award, plus the
// Esc-abort's gold_player_ clear (doc §2 "Cleared to -1 by: Esc on the
// wheel").
AppInput GoldmanWheelScreen::run() {
    // NO music call here — the wheel inherits 1020 from the Play handler, which
    // is what this screen's own comment always claimed while the code did the
    // opposite. sub_4034BC starts no track; the port's run_app StartMatch
    // handler starts 1020 once, before both this and the setup screen, mirroring
    // sub_42A3F6 @0x42A436. Restarting it here made WIN.RSS jump back to the top
    // on the hand-off to player select. docs/re/sound-engine.md §9.
    const int segment_steps = static_cast<int>(ctx_.values.column_or(1004, 0, kWheelSegmentSteps));
    const int cx = static_cast<int>(ctx_.values.column_or(1000, 0, 320));
    const int cy = static_cast<int>(ctx_.values.column_or(1000, 1, 240));
    const int rx = static_cast<int>(ctx_.values.column_or(1002, 0, 200));
    const int ry = static_cast<int>(ctx_.values.column_or(1002, 1, 150));
    const int freq_x = static_cast<int>(ctx_.values.column_or(1006, 0, 1));
    const int freq_y = static_cast<int>(ctx_.values.column_or(1006, 1, 1));

    GoldmanScreen wheel(ctx_.assets, ctx_.seqs, ctx_.front_font);
    // Advance a dedicated presentation LCG seed per spin (never State::rng) —
    // same shape as setup_lcg_/panic_lcg_ elsewhere in this file.
    state_.goldman_lcg = state_.goldman_lcg * 1664525u + 1013904223u;
    wheel.enter(state_.goldman_lcg, segment_steps);

    AppInput result = AppInput::Advance;
    // Refresh-boundary pacing (see refresh_period_ns): the wheel advances one
    // spin step per wheel.tick(), so a blind SDL_Delay(2) free-running at
    // 300-500 Hz on Windows spun it far too fast. Pace to the real refresh.
    platform::FrameClock frame_clock(ctx_.window);
    while (!wheel.done()) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_EVENT_QUIT) return AppInput::Quit;
            if (ev.type != SDL_EVENT_KEY_DOWN) continue;
            const SDL_Keycode k = ev.key.key;
            if (k == SDLK_F1) {
                // doc §5: F1 opens the SAME generic *.BM help browser
                // (sub_41431C) every other F1 site opens — the old fixed
                // OPTIONS.BM cut here was a stale stand-in (chrome audit
                // 2026-07-12, fix list item 9).
                AppInput help = HelpBrowserScreen(ctx_).run();
                if (help == AppInput::Quit) return AppInput::Quit;
                continue;
            }
            wheel.on_key(k, ctx_.audio);
        }
        wheel.tick(ctx_.audio);
        ctx_.audio.update_music();
        SDL_SetRenderDrawColor(ctx_.sdl, 0, 0, 0, 255);
        SDL_RenderClear(ctx_.sdl);
        wheel.draw(ctx_.sdl, cx, cy, rx, ry, freq_x, freq_y);
        SDL_RenderPresent(ctx_.sdl);
        frame_clock.pace();
    }

    if (wheel.aborted()) {
        // doc §2/§5: Esc aborts the WHOLE Play flow and forfeits the gold
        // player — the caller must skip present_setup/present_map_select and
        // return to the menu on AppInput::Back.
        state_.gold_player = -1;
        return AppInput::Back;
    }
    // doc §4: the prize persists (gold_prize_) until the NEXT spin; start_match
    // re-applies it every round of the following match via born_with_extra.
    state_.gold_prize = wheel.prize();
    return result;
}

}  // namespace bomber::game
