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

// No music id lives here: 1020 is the SETUP-SCREENS backdrop track and the
// goldman wheel genuinely INHERITS it from the Play handler rather than starting
// it (docs/re/sound-engine.md §9). kResultsDwellMs and the tier's other shared
// vocabulary live in outcome_tier.hpp.

namespace {

// The scoreboard's VALUELST anchors, read once — a parameter object (§3).
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

// getvalue(783), resolved by results-and-options.md's "screen-ink byte globals"
// pin: byte_49D38F decodes to RGB555 (31,31,31) = white, verified against the
// install's FIELD0/5/10/MAINMENU palettes (dist2=0 on all four).
constexpr Uint8 kHeaderR = 255, kHeaderG = 255, kHeaderB = 255;

// hud_format's fmt_u fills only the FIRST specifier; this splices the next one in
// by hand so the RE'd format string still reads naturally with real fallback text.
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
          // Both predicates are the SHARED match_outcome.hpp ones, so the shell's
          // VICTORY-vs-scoreboard decision and this render cannot disagree.
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
    std::string pre_clinch_line() const;
    std::string clinch_line() const;

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
        // sub_42A3F6's RESULTS tally loop @0x42ADE9, NOT sub_42A088's: any real
        // key blips, the accept codes 13/32 reach the sting, and Escape does NOT —
        // it sets the abort flag and jumps to the loop tail without touching the
        // sound engine.
        ctx_.audio.play(20);
        const bool escape = ev.key.key == SDLK_ESCAPE;
        // ONLINE between-rounds (net_round_gate.hpp): the wait loop is dismissed by
        // the machine DRIVING the game, never by a client — a guest's key gets the
        // SFX-40 buzz, and the HOST's accept commits the next round but leaves the
        // screen up until the peer holds it. Escape is exempt on both, because
        // abandoning the match stays local.
        if (state_.net_gate != nullptr && !escape) {
            const bool readonly = state_.net_gate->readonly();
            if (readonly) ctx_.audio.play(40);
            if (!readonly) state_.net_gate->accept();
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
        // (setup_session.hpp's one-pump-at-a-time rule), and as a side effect
        // drains the socket of the round that just ended.
        state_.net_gate->pump();
        // ready() is tested FIRST and WINS OUTRIGHT: a gate that has both held the
        // peer and then seen it drop still advances, which is what keeps the two
        // peers leaving on the same round.
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
    // The 6 s timeout is NOT a silent exit: @0x42AE4C it forces key = 13, which
    // falls straight into the accept path and plays the sting. It does NOT blip,
    // because the override lands AFTER the blip test and the key it replaced was
    // -1. So: 10 alone. The port used to advance in complete silence here.
    ctx_.audio.play(10);
    waiting_ = false;
}

void ScoreboardLoop::draw_frame() {
    draw_backdrop();
    draw_header();
    if (team_mode_) draw_team_rows();
    if (!team_mode_) draw_player_rows();
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

// getstring(30) "Game Winner was %s !" names this ROUND's winner, not necessarily
// the player who clinched the whole match. sub_42A3F6's "!dword_464AEC" gate is
// about not re-drawing across frames of the SAME round, which this screen's
// per-round call already satisfies.
void ScoreboardLoop::draw_header() {
    const int round_w = round_winner(state_.state);
    const std::string header =
        fmt_s(ctx_.assets.getstring(30, "Game Winner was %s !"),
              round_w >= 0 ? "P" + std::to_string(round_w + 1) : std::string("-"));
    ctx_.front_font.draw_outlined(ctx_.sdl, header, layout_.hx, layout_.hy, kHeaderR, kHeaderG,
                                  kHeaderB, 0, 0, 0);
}

// Team rows use the original's fixed two-ink helper sub_4141F8 (`team ?
// byte_49D0DA : byte_49D38F`): team 0 = the general white, team != 0 = red
// (252,80,80).
void ScoreboardLoop::draw_team_rows() {
    const sim::State& s = state_.state;
    std::array<bool, sim::kMaxPlayers> team_drawn{};
    int row = 0;
    for (int i = 0; i < sim::kMaxPlayers; ++i) {
        if (!s.players[i].present) continue;
        const int t = state_.setup_team[i];
        if (t < 0 || t >= sim::kMaxPlayers || team_drawn[t]) continue;
        team_drawn[t] = true;
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

// Non-team rows keep the slot's own ink (sub_41672F, docs/re/player-colour.md).
// getstring(31)'s two counters are independent: the match score, and CUMULATIVE
// MATCH kills — not round kills, despite the string's "kills" label.
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
        // sub_416867(i): in solo mode player 1 (the BLACK bomberman) gets a WHITE
        // outline so its dark ink stays legible; everyone else gets black, as do
        // the team rows, the header and the outcome line.
        const std::uint8_t ol = i == 1 ? 255 : 0;
        ctx_.front_font.draw_outlined(ctx_.sdl, line, layout_.rx,
                                      layout_.ry0 + layout_.rystep * static_cast<float>(row), c[0],
                                      c[1], c[2], ol, ol, ol);
        ++row;
    }
}

// Pre-clinch line (batch_0x4293E5.cpp:1262-1263): getstring(dword_46497C + 120)
// formatted with the FLAT target read straight from dword_464A7C — NOT the
// remaining count. The id keys off win_by_kills (a non-team feature), not team
// mode, and always shows the total goal.
std::string ScoreboardLoop::pre_clinch_line() const {
    const std::string fmt =
        state_.options.win_by_kills
            ? ctx_.assets.getstring(121, "(Match winner must score %u kills)")
            : ctx_.assets.getstring(120, "(Match winner must score %u victories)");
    return fmt_u(fmt, static_cast<unsigned>(state_.win_target));
}

// Clinch line (batch_0x4293E5.cpp:1300-1310): win_by_kills -> getstring(36)
// "PLAYER %u WINS THE MATCH!" with the winner's index + 1, else getstring(35)
// "%s WINS THE MATCH!" with the name. The native has NO team-specific win string
// here — the former `team_mode ? "TEAM %u WINS"` gate AND its fallback text were
// both invented (id 36 is "PLAYER %u", and the selector is win_by_kills).
std::string ScoreboardLoop::clinch_line() const {
    if (state_.options.win_by_kills)
        return fmt_u(ctx_.assets.getstring(36, "PLAYER %u WINS THE MATCH!"),
                     static_cast<unsigned>(clinched_player_ + 1));
    return fmt_s(ctx_.assets.getstring(35, "%s WINS THE MATCH!"),
                 "P" + std::to_string(clinched_player_ + 1));
}

// Outcome line inks, pinned by results-and-options.md §1's LUT decode:
// byte_49A624 ("still playing") = (168,168,164) mid-grey, byte_497F8F ("match
// over") = (96,252,252) cyan.
void ScoreboardLoop::draw_outcome() {
    const bool clinched = clinched_player_ >= 0;
    const std::string outcome = clinched ? clinch_line() : pre_clinch_line();
    const Uint8 r = clinched ? 96 : 168;
    const Uint8 g = clinched ? 252 : 168;
    const Uint8 b = clinched ? 252 : 164;
    ctx_.front_font.draw_outlined(ctx_.sdl, outcome, layout_.ox, layout_.oy, r, g, b, 0, 0, 0);
}

}  // namespace

// The between-round RESULTS cumulative-tally screen (sub_42A3F6 tail,
// docs/re/results-and-options.md §1). Any key, or the 6 s idle dwell, dismisses
// it; if the outcome line reports a clinch the flow shows VICTORY instead and
// never reaches this screen at all.
AppInput ScoreboardScreen::run() {
    return ScoreboardLoop(ctx_, state_).run();
}

namespace {

WheelGeometry read_wheel_geometry(const assets::res::ValueList& v) {
    WheelGeometry geo;
    geo.cx = static_cast<int>(v.column_or(1000, 0, 320));
    geo.cy = static_cast<int>(v.column_or(1000, 1, 240));
    geo.rx = static_cast<int>(v.column_or(1002, 0, 200));
    geo.ry = static_cast<int>(v.column_or(1002, 1, 150));
    geo.freq_x = static_cast<int>(v.column_or(1006, 0, 1));
    geo.freq_y = static_cast<int>(v.column_or(1006, 1, 1));
    return geo;
}

// Returns nullopt to keep spinning. F1 opens the SAME generic *.BM help browser
// every other F1 site opens (doc §5) — the old fixed OPTIONS.BM cut here was a
// stale stand-in.
std::optional<AppInput> pump_wheel_events(ScreenContext& ctx, GoldmanScreen& wheel) {
    SDL_Event ev;
    while (SDL_PollEvent(&ev)) {
        if (ev.type == SDL_EVENT_QUIT) return AppInput::Quit;
        if (ev.type != SDL_EVENT_KEY_DOWN) continue;
        if (ev.key.key != SDLK_F1) {
            wheel.on_key(ev.key.key, ctx.audio);
            continue;
        }
        if (HelpBrowserScreen(ctx).run() == AppInput::Quit) return AppInput::Quit;
    }
    return std::nullopt;
}

}  // namespace

// The Goldman Roulette wheel (docs/re/goldman-roulette.md), sub_4034BC. The
// caller gates on !attract && goldman && local && gold_player >= 0 before
// calling, matching the gate order at the head of sub_410F81 (doc §2); this runs
// the spin/award and the Esc-abort's gold-player clear.
//
// NO music call here — the wheel INHERITS 1020 from the Play handler
// (sub_4034BC starts no track). Restarting it here made WIN.RSS jump back to the
// top on the hand-off to player select. docs/re/sound-engine.md §9.
AppInput GoldmanWheelScreen::run() {
    const int segment_steps = static_cast<int>(ctx_.values.column_or(1004, 0, kWheelSegmentSteps));
    const WheelGeometry geo = read_wheel_geometry(ctx_.values);
    GoldmanScreen wheel(ctx_.assets, ctx_.seqs, ctx_.front_font);
    // A dedicated presentation LCG per spin, never State::rng — the same shape as
    // setup_lcg/panic_lcg.
    state_.goldman_lcg = state_.goldman_lcg * 1664525u + 1013904223u;
    wheel.enter(state_.goldman_lcg, segment_steps);

    // Refresh-boundary pacing: the wheel advances one spin step per tick(), so a
    // blind SDL_Delay(2) free-running at 300-500 Hz on Windows spun it far too
    // fast.
    platform::FrameClock frame_clock(ctx_.window);
    while (!wheel.done()) {
        if (const std::optional<AppInput> exit = pump_wheel_events(ctx_, wheel)) return *exit;
        wheel.tick(ctx_.audio);
        ctx_.audio.update_music();
        SDL_SetRenderDrawColor(ctx_.sdl, 0, 0, 0, 255);
        SDL_RenderClear(ctx_.sdl);
        wheel.draw(ctx_.sdl, geo);
        SDL_RenderPresent(ctx_.sdl);
        frame_clock.pace();
    }

    if (wheel.aborted()) {
        // doc §2/§5: Esc aborts the WHOLE Play flow and forfeits the gold player,
        // so the caller must skip the setup/level screens on Back.
        state_.gold_player = -1;
        return AppInput::Back;
    }
    // doc §4: the prize persists until the NEXT spin; start_match re-applies it
    // every round of the following match via born_with_extra.
    state_.gold_prize = wheel.prize();
    return AppInput::Advance;
}

}  // namespace bomber::game
