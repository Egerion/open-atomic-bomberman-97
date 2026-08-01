#include "bomber/frontend/match_runner.hpp"

#include <SDL3/SDL.h>

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>

#include "bomber/assets/extra.hpp"                  // assets::extra::load_for_board
#include "bomber/audio/round_music.hpp"             // round_music_id (the sub_410B6E guard)
#include "bomber/frontend/campaign_screens.hpp"     // HelpBrowserModal (the in-round F1)
#include "bomber/game_util/campaign_round_end.hpp"  // CampaignVerdict / campaign_verdict
#include "bomber/game_util/goldman_wheel.hpp"       // kClogsPrizeId / wheel_prize_to_powerup
#include "bomber/game_util/match_outcome.hpp"       // is_team_mode
#include "bomber/game_util/net_tally.hpp"           // tally_netplay_kills (the rollback-safe tally)
#include "bomber/game_util/results.hpp"             // tally_kills
#include "bomber/input/input.hpp"                   // SlotInputType
#include "bomber/match/match_factory.hpp"   // build_match_config / pick_stage / apply_actors
#include "bomber/net/path_failover.hpp"     // net::PathFailover (pumped beside the session)
#include "bomber/net/rollback_session.hpp"  // net::RollbackSession (netplay drive, seam is fwd-only)
#include "bomber/netui/net_overlay.hpp"     // draw_net_overlay (the F3 panel)
#include "bomber/render/renderer.hpp"       // kScreenW
#include "bomber/render/sprites.hpp"        // Sprite (player-row "xxx" marker)
#include "bomber/ui/dialog_chrome.hpp"      // kDialogInk (fps overlay)

namespace bomber::game {

namespace {

// Substitute the NEXT %u/%d/%i in `f`, leaving anything else literal — a modified
// MESSAGES.TXT must not be able to crash a draw. (hud_format's fmt_u fills only
// the first; results_screens.cpp carries the same helper.)
void splice_int(std::string& f, int v) {
    const std::size_t p = f.find('%');
    if (p == std::string::npos) return;
    std::size_t q = p + 1;
    while (q < f.size() && f[q] != 'u' && f[q] != 'd' && f[q] != 'i') ++q;
    if (q < f.size()) f = f.substr(0, p) + std::to_string(v) + f.substr(q + 1);
}

// ANY key, mouse button, or gamepad button — the attract abort's input set.
bool is_any_input(const SDL_Event& ev) {
    return ev.type == SDL_EVENT_KEY_DOWN || ev.type == SDL_EVENT_MOUSE_BUTTON_DOWN ||
           ev.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN;
}

// The panel's refresh period, or 60 Hz when the driver won't say — which still
// bounds the loop on drivers where SDL_SetRenderVSync is a no-op (dummy video).
std::uint64_t display_period_ns(SDL_Window* window) {
    const SDL_DisplayMode* mode = SDL_GetCurrentDisplayMode(SDL_GetDisplayForWindow(window));
    if (mode == nullptr || mode->refresh_rate_numerator <= 0 || mode->refresh_rate_denominator <= 0)
        return 1'000'000'000ull / 60;
    return 1'000'000'000ull * mode->refresh_rate_denominator /
           static_cast<std::uint64_t>(mode->refresh_rate_numerator);
}

// `kind` < 0 means Clogs, which is NOT a sim::PowerupType (§8/§9.2 — never a
// scheme/spawn kind), so it routes to born_with_clogs instead.
void grant_gold_prize(sim::MatchConfig& cfg, int slot, int kind) {
    if (kind < 0) {
        cfg.born_with_clogs[slot] = 1;  // reset-then-+1 every round, §9.3 — not accumulated
        return;
    }
    cfg.born_with_extra[slot][kind] = true;
}

// Goldman wheel award (docs/re/goldman-roulette.md §4/§9): sub_4214BC grants the
// last spin's prize EVERY round of the following match, so re-applying it at
// every start_match() reproduces "persists until the next spin" for free.
void apply_gold_award(sim::MatchConfig& cfg, const MatchRunnerState& s) {
    if (s.gold_player < 0 || s.gold_prize < 0) return;
    const bool is_clogs = s.gold_prize == kClogsPrizeId;
    const sim::PowerupType pt =
        is_clogs ? sim::PowerupType::None : wheel_prize_to_powerup(s.gold_prize);
    if (pt == sim::PowerupType::None && !is_clogs) return;
    const int kind = is_clogs ? -1 : static_cast<int>(pt);
    if (!s.team_play) {
        if (s.gold_player < sim::kMaxPlayers && cfg.active[s.gold_player])
            grant_gold_prize(cfg, s.gold_player, kind);
        return;
    }
    // Team mode compares against the RAW +84 byte — setup_team[] BEFORE
    // build_config's +1 shift — and every member of the gold TEAM gets it (§4).
    for (int i = 0; i < sim::kMaxPlayers; ++i)
        if (cfg.active[i] && s.setup_team[i] == s.gold_player) grant_gold_prize(cfg, i, kind);
}

// The original's +84 byte is 0 or 1 and IN TEAM MODE BOTH values are real teams,
// while the sim reserves team 0 for "solo side" — hence the +1 shift. Without it
// every un-toggled slot would fight solo.
void apply_roster(sim::MatchConfig& cfg, const MatchRunnerState& s) {
    for (int i = 0; i < sim::kMaxPlayers; ++i) {
        cfg.active[i] = s.setup_type[i] != 0;
        cfg.ai[i] = s.setup_type[i] == 1;
        cfg.team[i] = static_cast<std::uint8_t>(s.setup_team[i] + 1);
    }
}

// The Options rows that override a Tuning field per match. Each has a VALUELST
// seed the Options value overrides UNCONDITIONALLY, because the original's
// consumer sites read the merged global directly and options_ was itself seeded
// from getvalue at init. Stomped Bombs (id 46) is consumed by EnclosureSystem,
// Diseases (id 120) by FlameSystem and BombSystem.
void apply_option_overrides(sim::MatchConfig& cfg, const MatchRunnerState& s) {
    if (s.conveyor_speed_index) cfg.tuning.conveyor_speed_index = *s.conveyor_speed_index;
    cfg.tuning.wall_detonates = s.options.stomped_bombs_detonate ? 1 : 0;
    cfg.tuning.diseases_destroyable = s.options.diseases_destroyable;
    cfg.tuning.enclosement_depth = s.options.enclosement_depth;
    cfg.tuning.game_seconds = s.options.playtime_seconds == kPlayTimeUnlimited
                                  ? kUnlimitedGameSeconds
                                  : s.options.playtime_seconds;
}

// Campaign rover/ghost hazards — fields 3-6 of the stage's .CAM record. A
// non-campaign match leaves these at 0, so RoverSystem is a true no-op.
void apply_campaign_hazards(sim::MatchConfig& cfg, const MatchRunnerState& s) {
    if (!s.campaign_active || s.campaign_stage_index < 0 ||
        s.campaign_stage_index >= static_cast<int>(s.campaign_stages.size()))
        return;
    const assets::res::CampaignStage& rec =
        s.campaign_stages[static_cast<std::size_t>(s.campaign_stage_index)];
    cfg.campaign_rovers = rec.rovers;
    cfg.campaign_rover_speed = rec.rover_speed;
    cfg.campaign_ghosts = rec.ghosts;
    cfg.campaign_ghost_speed = rec.ghost_speed;
}

}  // namespace

void MatchRunner::start_match(std::uint32_t seed) {
    const sim::MatchConfig cfg = build_config(seed);
    state_.sim = sim::Simulation(cfg);
    const int stage = cfg.tuning.level_index;
    if (ctx_.assets.load_stage(stage)) ctx_.seqs.resolve_stage(ctx_.assets, stage);
    // The round's music must NOT be nested inside the load_stage() branch above: a
    // stage whose FIELD<n>.PCX failed to load would run in silence, a coupling the
    // original does not have — the two are not even in the same function there
    // (docs/re/sound-engine.md §9).
    const int track = round_music_id(stage, state_.options.disable_game_music,
                                     [this](int id) { return ctx_.audio.has_track(id); });
    if (track == kRoundMusicSilent) ctx_.audio.stop_music();
    if (track != kRoundMusicSilent) ctx_.audio.start_music(track);
    // The 1001 sentinel is presentation-only (the sim gets a long finite clock —
    // options_model.hpp), so tell the renderer directly rather than trying to
    // infer "untimed" back out of ticks_left.
    state_.renderer.reset_match(state_.options.playtime_seconds == kPlayTimeUnlimited);
    ctx_.sounds.reset();
}

sim::MatchConfig MatchRunner::build_config(std::uint32_t seed) const {
    // Random Start (Options row 1) shuffles which spawn slot each player index
    // gets — CONFIRMED as the original's 200-pair-swap (sub_421793).
    //
    // `team_play` makes build_match_config lay down the scheme's own "-S"
    // per-spawn teams, which apply_roster then overwrites wholesale. Not a
    // contradiction: setup_team[] was itself seeded from this same scheme on entry
    // to the PLAYER INPUT screen, so the two agree unless the user pressed 'T' —
    // and then the user wins, exactly as in sub_410F81. Passing it here also keeps
    // the SDL-free path (abtool, a headless host) honest on its own.
    sim::MatchConfig cfg =
        match::build_match_config(state_.scheme, sim::kMaxPlayers, seed, &ctx_.values,
                                  state_.options.random_start, state_.team_play);
    apply_roster(cfg, state_);
    apply_option_overrides(cfg, state_);
    // Team Play OFF means team mode is off regardless of what a slot's 'T' toggle
    // left behind (dword_464964), so zero every slot's team — MatchConfig::team[]
    // stays the single source of truth for the hashed Player::team.
    if (!state_.team_play) cfg.team.fill(0);
    apply_gold_award(cfg, state_);
    // Level from the LEVEL screen. RANDOM (-1) keeps pick_stage over the enabled
    // rotation (VALUELST 1150-1160, the same 200-try loop the original runs); a
    // specific level is used directly, clamped to one the registry knows.
    const bool random_stage = state_.selected_level < 0;
    int stage = random_stage ? match::pick_stage(state_.base_tuning, seed, ctx_.assets.levels())
                             : state_.selected_level;
    if (!random_stage && !ctx_.assets.levels().find(stage)) stage = 10;
    // The sim's per-level gates (tile regeneration ids 340-350/695, ice/input-lag
    // ids 450-460) are indexed by the SAME stage number as dword_46499C.
    cfg.tuning.level_index = stage;
    // Overlay this board's stage actors BEFORE constructing the sim — the actor
    // layout is a hashed setup input like the cell grid (docs/re/stage-actors.md).
    // Random '-T,H' trampolines resolve off a setup-only RNG inside apply_actors,
    // never the sim's per-tick stream.
    const auto actors =
        assets::extra::load_for_board(state_.game_dir, stage, sim::kGridWidth, sim::kGridHeight);
    match::apply_actors(cfg, actors, seed);
    apply_campaign_hazards(cfg, state_);
    // --demo / --demo-shots disarm the round-start input freeze (VALUELST id 30):
    // both were captured acting from tick 0. Live play keeps the real freeze.
    if (state_.demo) cfg.tuning.input_freeze_ticks = 0;
    return cfg;
}

void MatchRunner::leave(NetLeave how) {
    if (state_.net_leave != nullptr) *state_.net_leave = how;
}

bool MatchRunner::stop_outstanding() const {
    return state_.net_session != nullptr && state_.net_session->end_round_scheduled() &&
           !state_.net_session->round_ended();
}

void MatchRunner::draw_net_esc_prompt() {
    if (state_.net_session == nullptr || !ctx_.front_font.loaded()) return;
    // Bottom-centre is the one region nothing else occupies in-match.
    constexpr float kS = 0.7f;
    // The two sides must NOT read the same — see docs/frontend-match-loop.md.
    const char* first =
        state_.net_is_host ? "ENDING ROUND - DRAW" : "ONLY THE HOST CAN STOP THE MATCH";
    const char* second = "PRESS ESC AGAIN TO LEAVE THE MATCH";
    const float lh = static_cast<float>(ctx_.front_font.line_height()) * kS;
    float y = static_cast<float>(kScreenH) - 6.0f - 2.0f * lh;
    for (const char* s : {first, second}) {
        const std::string line = s;
        const float x =
            (static_cast<float>(kScreenW) - static_cast<float>(ctx_.front_font.measure(line)) * kS) /
            2.0f;
        ctx_.front_font.draw(ctx_.sdl, line, SDL_FPoint{x - 1, y}, TextStyle{{}, kS});
        ctx_.front_font.draw(ctx_.sdl, line, SDL_FPoint{x + 1, y}, TextStyle{{}, kS});
        ctx_.front_font.draw(ctx_.sdl, line, SDL_FPoint{x, y - 1}, TextStyle{{}, kS});
        ctx_.front_font.draw(ctx_.sdl, line, SDL_FPoint{x, y + 1}, TextStyle{{}, kS});
        ctx_.front_font.draw(ctx_.sdl, line, SDL_FPoint{x, y}, TextStyle{{255, 220, 90}, kS});
        y += lh;
    }
}

MatchCadence MatchRunner::cadence(std::uint64_t acc, std::uint64_t tick_ns) const {
    return match_cadence(state_.native_cadence, state_.net_session != nullptr,
                         static_cast<int>(state_.sim.systems_accum_ms()), sim::kMsPerTick, acc,
                         tick_ns);
}

sim::TickInputs MatchRunner::collect_inputs() const {
    sim::TickInputs in;
    const sim::TickInputs kb = ctx_.keyboard.read();
    for (int i = 0; i < sim::kMaxPlayers; ++i) {
        switch (static_cast<SlotInputType>(state_.setup_type[i])) {
            case SlotInputType::Keyboard:
                in.players[i] = kb.players[state_.setup_sub[i] == 0 ? 0 : 1];
                break;
            case SlotInputType::Joystick:
                in.players[i] = ctx_.gamepads.read(state_.setup_sub[i]);
                break;
            default: break;  // Off/Computer/Other: neutral — AI or absence owns the slot
        }
    }
    return in;
}

sim::TickInputs MatchRunner::latched_inputs(const RunLoop& loop) const {
    sim::TickInputs in = loop.frame_in;
    for (int i = 0; i < sim::kMaxPlayers; ++i) {
        in.players[i].action1 = in.players[i].action1 || loop.tap_latch[i].action1;
        in.players[i].action2 = in.players[i].action2 || loop.tap_latch[i].action2;
    }
    return in;
}

void MatchRunner::clear_latch(RunLoop& loop) const {
    for (TapLatch& latch : loop.tap_latch) latch = TapLatch{};
}

void MatchRunner::draw_player_row(const sim::State& s) {
    for (int i = 0; i < sim::kMaxPlayers; ++i) {
        // byte_461BD4 gates the whole row -> Player::present, set once at match
        // setup and true for the rest of it regardless of round elimination.
        if (!s.players[i].present) continue;
        const int col = i / 2;  // getvalue(115 + i/2): 5 columns, VALUELST 10/110/210/310/410
        const int row = i & 1;  // getvalue(113 + i&1): 2 rows, VALUELST 6/26
        const SDL_FPoint at{
            static_cast<float>(ctx_.values.column_or(115 + col, 0, 10 + 100 * col)),
            static_cast<float>(ctx_.values.column_or(113 + row, 0, 6 + 20 * row)),
        };
        draw_player_score(i, at);
        // dword_461BC4 gates the "xxx" overlay -> Player::alive, the per-ROUND
        // flag: a player dead THIS round keeps their score visible under it.
        if (!s.players[i].alive) draw_eliminated_marker(at);
    }
}

// Ink sub_41672F(i) == slot_color; outline sub_416867(i) — black in team mode,
// else white for player 1 (the black bomberman). NOT in the demo/golden path.
void MatchRunner::draw_player_score(int i, SDL_FPoint at) {
    std::string line = ctx_.assets.getstring(37, "S:%d K:%d");
    splice_int(line, state_.win_count[i]);
    splice_int(line, state_.kill_count[i]);
    std::uint8_t c[3];
    ctx_.assets.slot_color(i, c);
    const bool solo =
        !::bomber::game::is_team_mode(state_.team_play, state_.sim.state(), state_.setup_team);
    const std::uint8_t ol = (solo && i == 1) ? 255 : 0;
    ctx_.front_font.draw_outlined(ctx_.sdl, line, at,
                                  OutlinedTextStyle{{c[0], c[1], c[2]}, {ol, ol, ol}});
}

void MatchRunner::draw_eliminated_marker(SDL_FPoint at) {
    if (ctx_.seqs.eliminated_marker.steps.empty()) return;
    const Sprite& sp = ctx_.seqs.eliminated_marker.steps[0];
    if (sp.tex == nullptr) return;
    SDL_FRect dst{at.x - static_cast<float>(sp.hx), at.y - static_cast<float>(sp.hy),
                  static_cast<float>(sp.w), static_cast<float>(sp.h)};
    SDL_RenderTexture(ctx_.sdl, sp.tex, nullptr, &dst);
}

void MatchRunner::draw_fps_overlay(int fps) {
    if (!state_.show_fps || !ctx_.front_font.loaded()) return;
    // The reduced size comes from FontTextures::draw's `scale` (dst-rect only) and
    // NEVER from SDL_SetRenderScale, which perturbed the whole render transform.
    constexpr float kS = 0.7f;
    const float right = static_cast<float>(kScreenW) - 3.0f;
    const float lh = static_cast<float>(ctx_.front_font.line_height()) * kS;
    auto line = [&](const std::string& s, float y, bool hot) {
        const float x = right - static_cast<float>(ctx_.front_font.measure(s)) * kS;
        ctx_.front_font.draw(ctx_.sdl, s, SDL_FPoint{x - 1, y}, TextStyle{{}, kS});
        ctx_.front_font.draw(ctx_.sdl, s, SDL_FPoint{x + 1, y}, TextStyle{{}, kS});
        ctx_.front_font.draw(ctx_.sdl, s, SDL_FPoint{x, y - 1}, TextStyle{{}, kS});
        ctx_.front_font.draw(ctx_.sdl, s, SDL_FPoint{x, y + 1}, TextStyle{{}, kS});
        ctx_.front_font.draw(ctx_.sdl, s, SDL_FPoint{x, y},
                             TextStyle{hot ? Rgb{120, 240, 120} : kDialogInk, kS});
    };
    char buf[24];
    std::snprintf(buf, sizeof(buf), "%d FPS", fps);
    float y = 2.0f;
    line(buf, y, state_.uncap_fps);
    y += lh;
    // WHAT IS IN EFFECT, not what the key says — "(NET)" says why F9 is ignored
    // online (docs/frontend-match-loop.md).
    const bool native = cadence(0, 1).native;
    const char* label = native                  ? "NATIVE"
                        : state_.native_cadence ? "20HZ (NET)"
                                                : "20HZ";
    line(label, y, native);
    y += lh;
    line(state_.uncap_fps ? "UNCAP" : "VSYNC", y, state_.uncap_fps);
}

AppInput MatchRunner::run() {
    // A NETPLAY match is seeded canonically BEFORE run(), so re-seeding here from
    // the per-machine roster/options/level would clobber the peers' parity.
    // GOLDEN-SAFE: net_session is null on every non-netplay path.
    if (!state_.net_session) start_match(state_.next_seed++);
    // sub_40151B @0x401548 clears the pacing verdict before every campaign round,
    // including a replayed one, so a stale verdict can never decide the next.
    state_.campaign_pacing = CampaignPacing{};

    RunLoop loop;
    loop.tick_ns = 1'000'000'000ull / sim::kTicksPerSecond;
    loop.sub_frame_ns = loop.tick_ns / sim::kSubFrames;
    loop.display_period_ns = display_period_ns(ctx_.window);
    loop.last = SDL_GetTicksNS();
    loop.fps_window_start_ns = loop.last;
    loop.pacer = platform::FramePacer(loop.display_period_ns, loop.last);

    while (true) {
        if (const std::optional<AppInput> exit = pump_events(loop)) return *exit;
        sample_frame(loop);
        if (const std::optional<AppInput> exit = advance_sim(loop)) return *exit;
        const bool native = present_frame(loop);
        pace_next_present(loop, native);
    }
}

std::optional<AppInput> MatchRunner::pump_events(RunLoop& loop) {
    SDL_Event ev;
    while (SDL_PollEvent(&ev)) {
        if (ev.type == SDL_EVENT_QUIT) return AppInput::Quit;
        // ATTRACT abort (sub_42A3F6's round-loop tail): ANY input returns to the
        // menu IMMEDIATELY, checked ahead of the specific keys below and only
        // while attract is armed, so a human round's bindings are unaffected.
        if (state_.attract && is_any_input(ev)) return AppInput::MatchOver;
        // A pad unplugged/plugged mid-match: rescan so a disconnect drops that
        // slot to neutral rather than leaving it wedged.
        if (ev.type == SDL_EVENT_GAMEPAD_ADDED || ev.type == SDL_EVENT_GAMEPAD_REMOVED) {
            ctx_.gamepads.refresh();
            continue;
        }
        if (ev.type != SDL_EVENT_KEY_DOWN) continue;
        if (const std::optional<AppInput> exit = handle_key(ev, loop)) return *exit;
    }
    return std::nullopt;
}

std::optional<AppInput> MatchRunner::handle_key(const SDL_Event& ev, RunLoop& loop) {
    // Ctrl+Q — CONFIRMED as the ONLY key that aborts a round mid-match in the
    // original, with no confirm prompt (docs/re/in-match-shell.md "Esc negative
    // finding"). ONLINE it is a LEAVE, and it SAYS so rather than leaving the
    // shell to deduce it from the frozen sim state.
    if (ev.key.key == SDLK_Q && (ev.key.mod & SDL_KMOD_CTRL) != 0) {
        leave(NetLeave::Forfeit);
        return AppInput::MatchOver;
    }
    if (ev.key.key == SDLK_ESCAPE) return on_escape_key(loop);
    if (ev.key.key == SDLK_F1) return on_help_key(loop);
    return std::nullopt;
}

// Esc bails to the menu — a PORT CONVENIENCE, not a binary fact (literal Esc is
// INERT mid-round in the original; only Ctrl+Q aborts). ONLINE it is two
// different things depending on how many times it is pressed: see
// docs/frontend-match-loop.md "Esc online is two different things".
std::optional<AppInput> MatchRunner::on_escape_key(RunLoop& loop) {
    if (state_.net_session == nullptr) return AppInput::MatchOver;
    if (loop.esc.press(SDL_GetTicks(), stop_outstanding()) == EscPress::Leave) {
        leave(NetLeave::Stalled);
        return AppInput::MatchOver;
    }
    state_.net_session->request_end_round();  // host only; a no-op on a guest
    return std::nullopt;
}

// F1 (0x13B) opens the SAME generic *.BM help browser row 5 opens, without
// leaving the round, bracketed by sub_42A16F(1)/(0) — the whole game freezes.
// Resetting the clock on resume is a DELIBERATE deviation from the original's
// documented bug (docs/frontend-match-loop.md "The F1 help modal").
std::optional<AppInput> MatchRunner::on_help_key(RunLoop& loop) {
    const AppInput help = HelpBrowserModal(ctx_, {state_.renderer, state_.sim.state()}).run();
    if (help == AppInput::Quit) return AppInput::Quit;
    loop.last = SDL_GetTicksNS();
    loop.acc = 0;
    return std::nullopt;
}

// One frame's input sample plus the wall-clock bookkeeping. The action-key latch
// restores the original's per-DISPLAYED-FRAME acquisition cadence; directions are
// deliberately NOT latched (docs/frontend-match-loop.md).
void MatchRunner::sample_frame(RunLoop& loop) {
    loop.frame_in = collect_inputs();
    for (int i = 0; i < sim::kMaxPlayers; ++i) {
        loop.tap_latch[i].action1 = loop.tap_latch[i].action1 || loop.frame_in.players[i].action1;
        loop.tap_latch[i].action2 = loop.tap_latch[i].action2 || loop.frame_in.players[i].action2;
    }
    const std::uint64_t now = SDL_GetTicksNS();
    loop.delta_ns = now - loop.last;
    loop.acc += loop.delta_ns;
    loop.last = now;
    // One loop iteration == one SDL_RenderPresent; refresh the shown figure once
    // the 250 ms window elapses so the number is readable, not a blur.
    ++loop.fps_frames;
    const std::uint64_t span = now - loop.fps_window_start_ns;
    if (span < 250'000'000ull) return;
    loop.shown_fps = static_cast<int>(loop.fps_frames * 1'000'000'000ull / span);
    loop.fps_frames = 0;
    loop.fps_window_start_ns = now;
}

// Netplay MUST use the deterministic fixed-tick path: Simulation::frame()
// advances on the real wall-clock delta, which differs per machine and would
// instantly desync the peers. That rule lives in match_cadence.hpp and EVERY
// consumer of the lever reads it through there, so they cannot disagree.
std::optional<AppInput> MatchRunner::advance_sim(RunLoop& loop) {
    if (cadence(loop.acc, loop.tick_ns).native) return advance_native(loop);
    return advance_fixed(loop);
}

// F9: advance the sim ONE displayed frame on the measured wall-clock delta —
// the original's per-frame gameplay driver (sub_42A191). Low input latency,
// fps-scaled granularity, and NON-DETERMINISTIC, because the delta is real.
std::optional<AppInput> MatchRunner::advance_native(RunLoop& loop) {
    std::int32_t delta_ms = static_cast<std::int32_t>(loop.delta_ns / 1'000'000ull);
    if (delta_ms < 1) delta_ms = 1;
    if (delta_ms > 4 * sim::kMsPerTick) delta_ms = 4 * sim::kMsPerTick;
    const sim::TickInputs in = latched_inputs(loop);
    clear_latch(loop);
    const std::uint64_t tick_before = state_.sim.state().tick;
    state_.sim.frame(in, delta_ms);
    ctx_.sounds.on_tick(state_.sim.state());
    // Pose countdowns age once per SIM TICK, not per displayed frame: pass
    // whether this frame actually crossed a tick, else kick/punch/pickup poses
    // play ~9x too fast in native cadence.
    state_.renderer.on_events(state_.sim.state(), state_.sim.state().tick != tick_before);
    state_.renderer.advance_tick(state_.sim.state());
    // Local-only path: F9 is force-disabled online, so there is no replaying
    // event stream to guard against — the live events are each tick's one pass.
    tally_kills(state_.sim.state().events, state_.kill_count);
    for (std::uint64_t t = tick_before; t < state_.sim.state().tick; ++t)
        if (advance_round_end(loop)) return finish_round(loop);
    loop.acc = 0;  // the fixed-tick accumulator is dormant on this path
    return std::nullopt;
}

std::optional<AppInput> MatchRunner::advance_fixed(RunLoop& loop) {
    // Long-stall guard (spiral-of-death / teleport clamp): cap the catch-up queue
    // and DROP the excess wall-time rather than fast-forwarding it. Determinism
    // is untouched — only how many crossings one hitch produces is bounded. See
    // docs/frontend-match-loop.md "The long-stall guard".
    constexpr std::uint64_t kMaxCatchupTicks = 4;
    if (loop.acc > kMaxCatchupTicks * loop.tick_ns) loop.acc = kMaxCatchupTicks * loop.tick_ns;
    while (loop.acc >= loop.tick_ns)
        if (const std::optional<AppInput> exit = tick_once(loop)) return *exit;
    return std::nullopt;
}

// Netplay drives the SAME borrowed sim through the ROLLBACK session
// (deterministic tick() only — never frame()); a local match ticks directly. The
// wall clock is DIAGNOSTICS ONLY: it stops at the session's instrumentation
// bracket and never reaches Simulation::tick, so no hashed state can depend on it
// (determinism rule 1).
void MatchRunner::drive_tick(const sim::TickInputs& in) {
    if (state_.net_session == nullptr) {
        state_.sim.tick(in);
        return;
    }
    state_.net_session->advance(in, static_cast<std::int64_t>(SDL_GetTicks()));
    // The mid-match path failover rides the same 20 Hz pump as the session it
    // watches (path_failover.hpp): detection reads the frontier this advance
    // just moved (or failed to move), and the control-plane keep-alive rides
    // along. Null on every local path, so the golden tick path is untouched.
    if (state_.net_failover != nullptr)
        state_.net_failover->pump(static_cast<std::int64_t>(SDL_GetTicks()), state_.net_session);
}

// §1's kill tally (sub_421B0F), cumulative for the whole match. ONLINE the
// events come from the session's CONFIRMED stream rather than from the live
// (speculative, re-simulatable) state — net_tally.hpp carries the full rationale.
void MatchRunner::tally_round_kills(RunLoop& loop) {
    if (state_.net_session == nullptr) {
        tally_kills(state_.sim.state().events, state_.kill_count);
        return;
    }
    tally_netplay_kills(*state_.net_session, loop.net_events, state_.kill_count);
}

std::optional<AppInput> MatchRunner::tick_once(RunLoop& loop) {
    const sim::TickInputs in = latched_inputs(loop);
    drive_tick(in);
    // The tick actually happened — NOW consume the taps and one tick's worth of
    // the accumulator, so the latch is consumed on the FIRST tick of a catch-up
    // burst only.
    clear_latch(loop);
    loop.acc -= loop.tick_ns;
    ctx_.sounds.on_tick(state_.sim.state());
    state_.renderer.on_events(state_.sim.state());
    // Roll the renderer's inter-tick snapshots forward INSIDE the catch-up loop,
    // so a frame that advances the sim two ticks still leaves interp `prev` at the
    // penultimate tick (a clean 1-tick lerp) instead of two ticks back.
    state_.renderer.advance_tick(state_.sim.state());
    tally_round_kills(loop);
    // ONCE AN ABANDON IS AGREED (Esc online), the agreed tick is the ONLY exit:
    // advance_round_end() reads a SPECULATIVE state and would let one peer leave a
    // tick or two before the other, on a different slice of the round's events.
    if (state_.net_session != nullptr && state_.net_session->end_round_scheduled()) {
        if (state_.net_session->round_ended()) return finish_round(loop);
        return std::nullopt;
    }
    if (advance_round_end(loop)) return finish_round(loop);
    return std::nullopt;
}

AppInput MatchRunner::finish_round(RunLoop& loop) {
    if (state_.net_session)
        tally_netplay_kills_final(*state_.net_session, loop.net_events, state_.kill_count);
    return AppInput::MatchOver;
}

bool MatchRunner::advance_round_end(RunLoop& loop) {
    if (state_.campaign_active) return campaign_round_end(loop);
    return normal_round_end(loop);
}

// CAMPAIGN rounds end by sub_4016DA's pacing verdict and NOTHING else; the
// survivor-count rule is genuinely unreachable there, and applying it used to end
// eight of the seventeen shipped stages on their first tick
// (docs/frontend-match-loop.md "When a round ends").
bool MatchRunner::campaign_round_end(RunLoop& loop) {
    const sim::State& s = state_.sim.state();
    if (loop.over_ticks < 0 && !loop.await_death_fx) latch_campaign_verdict(loop, s);
    return loop.await_death_fx && !state_.renderer.death_fx_active(s);
}

void MatchRunner::latch_campaign_verdict(RunLoop& loop, const sim::State& s) {
    const CampaignPacing pacing = campaign_pacing(
        // clause 2 — sub_410578's REMAINING WHOLE SECONDS, floored the same way
        // EnclosureSystem::update reads the same global.
        s.ticks_left / sim::kTicksPerSecond,
        // clause 3 — the grace window, answerable only by the sim and only for a
        // stage that actually spawned hazards.
        s.campaign_hazards_active && s.hazard_clear_timer >= sim::kHazardClearTicks,
        // clause 5 — no human/joystick slot left alive.
        campaign_no_human_survivor(true, s, state_.setup_type));
    if (pacing.verdict == CampaignVerdict::Running) return;
    state_.campaign_pacing = pacing;
    loop.await_death_fx = true;
}

// Team-aware round-over: "one SIDE left" (docs/re/ai.md TEAM follow-up), which
// degenerates to alive_count() in a solo match. WHEN the results screen takes
// over is not a timer in the original — the round is over one frame after the
// LAST corpse finishes animating (docs/frontend-match-loop.md).
bool MatchRunner::normal_round_end(RunLoop& loop) {
    const sim::State& s = state_.sim.state();
    if (loop.over_ticks < 0 && !loop.await_death_fx) {
        // The clock-expired exit has no linger at all.
        if (s.ticks_left == 0) return true;
        if (sim::sides_remaining(s) <= 1) arm_round_linger(loop);
    }
    // death_fx_active() measures each effect against its own sequence length, the
    // same rule draw_world retires it on (sub_41DA5C's step count).
    if (loop.await_death_fx) return !state_.renderer.death_fx_active(s);
    return loop.over_ticks > 0 && --loop.over_ticks == 0;
}

// NETPLAY keeps a fixed linger because the death-sequence step counts come from
// the LOCAL install's DATA/ANI files, which build_hash does not cover — an
// animation-driven handoff could land on a different tick on each peer.
void MatchRunner::arm_round_linger(RunLoop& loop) {
    if (state_.net_session == nullptr) {
        loop.await_death_fx = true;
        return;
    }
    loop.over_ticks = 3 * sim::kTicksPerSecond;
}

// Draws one frame and returns whether the NATIVE cadence applied to it (which
// the pacer needs). The three presentation values are re-derived HERE rather than
// reused from advance_sim, because both inputs have moved since: the catch-up
// loop has drained `acc`, and a native frame has advanced systems_accum_ms.
bool MatchRunner::present_frame(RunLoop& loop) {
    ctx_.audio.update_music();
    // Gold Bomberman twinkle (docs/re/goldman-roulette.md §6), pushed every frame
    // rather than on change: a cheap int pair, and it keeps the renderer decoupled
    // from the shell's own state.
    state_.renderer.set_gold_player(
        state_.gold_player,
        ::bomber::game::is_team_mode(state_.team_play, state_.sim.state(), state_.setup_team));
    const MatchCadence draw = cadence(loop.acc, loop.tick_ns);
    state_.renderer.set_native_cadence(draw.native);
    state_.renderer.set_entity_interp(draw.entity_interp);
    state_.renderer.draw_frame(state_.sim.state(), draw.interp_alpha);
    // The player-row HUD strip needs win_count/kill_count/front_font, none of
    // which Renderer owns, so it is an overlay on top of Renderer's frame — the
    // same layering the original has (sub_420F07).
    draw_player_row(state_.sim.state());
    draw_fps_overlay(loop.shown_fps);
    if (loop.esc.armed(SDL_GetTicks(), stop_outstanding())) draw_net_esc_prompt();
    // F3 is double-gated on purpose — it needs BOTH the toggle (session-only,
    // never persisted) and a live session (a capture never has one), which is
    // what keeps it out of tests/visual's frames.
    if (state_.show_netstats && state_.net_session != nullptr)
        draw_net_overlay(ctx_.sdl, ctx_.front_font, state_.net_session->stats(),
                         state_.net_local_seats);
    SDL_RenderPresent(ctx_.sdl);
    return draw.native;
}

void MatchRunner::pace_next_present(RunLoop& loop, bool native) {
    const std::uint64_t after_present_ns = SDL_GetTicksNS();
    const platform::FramePacer::Wait wait = plan_wait(loop, native, after_present_ns);
    if (wait.sleep_ns) SDL_DelayNS(wait.sleep_ns);
    // Busy-wait the last stretch: the coarse sleep overshoots by ~0.5 ms on Win11
    // and has a floor of about the same, which at a 5.556 ms period is the
    // difference between hitting the boundary and missing it. Uncapped path only.
    while (wait.spin_until_ns && SDL_GetTicksNS() < wait.spin_until_ns) SDL_CPUPauseInstruction();
}

// DEFAULT (vsync) is the refresh-boundary resync; F8 (uncapped) is the sub-frame
// LATTICE instead. Both are measured in docs/frontend-match-loop.md, and the
// DECISION half is SDL-free in platform::FramePacer, pinned by tests/platform.
platform::FramePacer::Wait MatchRunner::plan_wait(RunLoop& loop, bool native,
                                                  std::uint64_t now_ns) {
    if (!state_.uncap_fps) {
        loop.pacer.set_period(loop.display_period_ns);
        return loop.pacer.plan_resync(now_ns);
    }
    loop.pacer.set_period(loop.sub_frame_ns);
    // Lattice origin = the wall instant the CURRENT tick began; `acc` only moves
    // by measured deltas and whole ticks, so `last - acc` is an exact point on the
    // 50 ms tick clock. The F9 native path zeroes `acc` every frame, so leaving
    // the lattice free-running there is DELIBERATE — re-anchoring onto `last`
    // would silently turn the absolute target back into a relative one.
    if (!native) loop.pacer.set_anchor(loop.last - loop.acc);
    return loop.pacer.plan_subframe(now_ns);
}

}  // namespace bomber::game
