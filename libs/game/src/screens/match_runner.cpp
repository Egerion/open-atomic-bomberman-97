#include "bomber/game/screens/match_runner.hpp"

#include <SDL3/SDL.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>

#include "bomber/assets/extra.hpp"                   // assets::extra::load_for_board
#include "bomber/game/dialog_chrome.hpp"             // kDialogInkR/G/B (fps overlay)
#include "bomber/game/goldman_wheel.hpp"             // kClogsPrizeId / wheel_prize_to_powerup
#include "bomber/game/input.hpp"                     // SlotInputType
#include "bomber/game/match_outcome.hpp"             // is_team_mode
#include "bomber/game/renderer.hpp"                  // kScreenW
#include "bomber/game/results.hpp"                   // tally_kills
#include "bomber/game/screens/campaign_screens.hpp"  // HelpBrowserModal (the in-round F1)
#include "bomber/game/sprites.hpp"                   // Sprite (player-row "xxx" marker)
#include "bomber/match/match_factory.hpp"   // build_match_config / pick_stage / apply_actors
#include "bomber/net/rollback_session.hpp"  // net::RollbackSession (netplay drive, seam is fwd-only)

namespace bomber::game {

namespace {

// Per-level in-round stage-track fallback (sub_4293E5, docs/re/in-match-shell.md
// §2): SOUNDLST 1100+level, or this id when the level has no entry. A local copy
// of the constant that lived beside start_match in game_app.cpp before this
// extraction (its only user moved here — same pattern as results_screens.cpp's
// local kWinMusicId copy).
constexpr int kStageMusicFallback = 1120;  // 0x460 — GENERIC.RSS

}  // namespace

void MatchRunner::start_match(std::uint32_t seed) {
    // The config build was split out VERBATIM into build_config() so the ONLINE
    // setup stage can produce the SAME config from the SAME screens without
    // seeding the sim (see that method's doc comment). Everything below the call
    // is start_match's unchanged tail; the golden hashes depend on this being a
    // pure extract-method, not a rewrite.
    const sim::MatchConfig cfg = build_config(seed);
    state_.sim = sim::Simulation(cfg);
    const int stage = cfg.tuning.level_index;
    if (ctx_.assets.load_stage(stage)) {
        ctx_.seqs.resolve_stage(ctx_.assets, stage);
        // Disable music during gameplay (options.ini "disable_game_music=" /
        // Options row 13, §3): the original's round init (sub_410B6E
        // LABEL_48) FREES the music outright (sub_427342) when the option is
        // set — the round is SILENT, the setup-screens track (1020) does not
        // bleed into it. Menu/results music is untouched (the option is
        // specifically "during gameplay"; round end starts 1130 regardless).
        //
        // Per-level stage track (docs/re/in-match-shell.md §2, sub_4293E5):
        // SOUNDLST 1100+level, falling back to 1120 ("generic") when the level
        // has no entry — our 11 built-in stages all have one (SOUNDLST.RES
        // 1100..1110), so this only matters for a stripped/modified install.
        if (!state_.options.disable_game_music) {
            int stage_music = 1100 + stage;
            if (!ctx_.audio.has_track(stage_music)) stage_music = kStageMusicFallback;  // 1120
            ctx_.audio.start_music(stage_music);
        } else {
            ctx_.audio.stop_music();  // sub_427342: silent round, not "keep 1020 playing"
        }
    }
    // Untimed round HUD (docs/re/in-match-shell.md §3): the 1001 sentinel is a
    // presentation-only concept (see cfg.tuning.game_seconds's own comment in
    // build_config — the sim gets a very long but finite clock instead), so
    // tell the renderer directly rather than trying to infer "untimed" back
    // out of ticks_left.
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access) — emplaced in init()
    state_.renderer.reset_match(state_.options.playtime_seconds == kPlayTimeUnlimited);
    ctx_.sounds.reset();
}

sim::MatchConfig MatchRunner::build_config(std::uint32_t seed) const {
    // Random Start (options.ini "random_start=" / Options row 1, §3):
    // shuffles which of the scheme's own spawn slots each player index gets
    // — CONFIRMED as the original's 200-pair-swap over the 10 start slots
    // (sub_421793; match_factory.hpp mirrors the loop, docs/re/facts.md
    // "Options toggles").
    // `team_play` makes build_match_config lay down the scheme's own "-S"
    // per-spawn teams (facts.md "The .SCH -S row's 4th field is the per-slot
    // TEAM"). The setup-screen roster below then overwrites cfg.team[]
    // wholesale — which is not a contradiction: setup_team[] was itself seeded
    // from this same scheme on entry to the PLAYER INPUT screen, so the two
    // agree unless the user pressed 'T', in which case the user's choice wins,
    // exactly as it does in sub_410F81 (scheme load first, key loop after).
    // Passing it here keeps the SDL-free path (abtool, a headless host)
    // honest on its own, rather than leaving the field readable only through
    // the GUI.
    sim::MatchConfig cfg =
        match::build_match_config(state_.scheme, sim::kMaxPlayers, seed, &ctx_.values,
                                  state_.options.random_start, state_.team_play);
    // Roster from the PLAYER INPUT screen (present_setup): OFF slots are inactive,
    // COMPUTER slots are AI-driven, KEYBOARD slots are local human(s). The per-slot
    // team feeds MatchConfig::team[] -> the hashed Player::team.
    //
    // Mapping: the original's +84 byte is 0 or 1 and IN TEAM MODE BOTH values
    // are real teams (sub_4141F8 inks team 1 vs "the rest" — two sides), while
    // the sim's convention reserves team 0 for "no team / solo side"
    // (simulation.cpp on_same_side). So shift the setup byte up by one when
    // team play is on: +84==0 -> sim team 1, +84==1 -> sim team 2. Without the
    // shift every un-toggled slot would wrongly fight solo.
    for (int i = 0; i < sim::kMaxPlayers; ++i) {
        cfg.active[i] = state_.setup_type[i] != 0;
        cfg.ai[i] = state_.setup_type[i] == 1;
        cfg.team[i] = static_cast<std::uint8_t>(state_.setup_team[i] + 1);
    }
    // Override the Conveyor Speed index from options.ini if present (this
    // install = 2 high); otherwise Tuning keeps the confirmed default (1
    // medium). conveyor_speed() clamps to [0, count-1], so a raw index is safe.
    if (state_.conveyor_speed_index) cfg.tuning.conveyor_speed_index = *state_.conveyor_speed_index;
    // Stomped Bombs Detonate (options.ini "stomped_bombs_detonate=" / Options
    // row 4, §3 — dword_464940): whether a closing enclosement wall landing
    // on a grounded bomb DETONATES it (queued full explosion) or silently
    // eats it. The enclosure site reads the merged global directly
    // (sub_426818 ~27260), so the Options value overrides the VALUELST id 46
    // seed unconditionally — options_ was itself seeded from getvalue(46)
    // at init, mirroring sub_41095A. Consumer: EnclosureSystem::drop_wall.
    cfg.tuning.wall_detonates = state_.options.stomped_bombs_detonate ? 1 : 0;
    // Diseases Can Be Destroyed (options.ini "diseases_destroyable=" /
    // Options row 11, §3 — dword_464990): OFF makes a destroyed floor skull
    // relocate to a random free tile instead of being lost (sub_4230A5 /
    // sub_42331C flame walk -> sub_4255B2(2)). Same merged-global override
    // as above (seed = getvalue(120), sub_41095A). Consumers:
    // FlameSystem::spread_to and BombSystem::slide.
    cfg.tuning.diseases_destroyable = state_.options.diseases_destroyable;
    // Enclosement Depth (options.ini "enclosement_depth=" / Options row 7,
    // §3): a REAL Tuning consumer (enclosure.cpp/ai.cpp). base_tuning_ already
    // carries the VALUELST default; the Options screen's live edit overrides
    // it per match, same pattern as Conveyor Speed above.
    cfg.tuning.enclosement_depth = state_.options.enclosement_depth;
    // Play Time (options.ini "playtime=" / Options row 9, §3): a REAL Tuning
    // consumer (setup.cpp's ticks_left = game_seconds * kTicksPerSecond). The
    // "unlimited" sentinel (1001) has no sim meaning yet — a very long but
    // finite clock is the closest faithful stand-in without inventing a
    // separate "no clock" sim mode (out of scope: PRESENTATION/CONFIG ONLY).
    cfg.tuning.game_seconds = state_.options.playtime_seconds == kPlayTimeUnlimited
                                  ? kUnlimitedGameSeconds
                                  : state_.options.playtime_seconds;
    // Team Play (options.ini "team_play=" / the interactive Options screen):
    // the game-type-level team-mode GATE (docs/re/setup-screens.md
    // `dword_464964`), separate from each slot's own +84 team byte. OFF means
    // team mode is off regardless of what a slot's 'T' toggle left behind, so
    // zero every slot's team here (sim team 0 = solo side) — MatchConfig::
    // team[] stays the single source of truth for the hashed Player::team.
    if (!state_.team_play) cfg.team.fill(0);
    // Goldman wheel award (docs/re/goldman-roulette.md §4/§9): sub_4214BC
    // grants the last spin's prize to the gold player EVERY round of the
    // following match, not just the round right after the spin —
    // build_match_config runs at every start_match() call (including
    // RoundContinue's re-init), so re-applying gold_prize_/gold_player_ here
    // reproduces that "persists until the next spin" behaviour for free. A
    // no-op (all-false/all-zero overlay) whenever gold_prize_ < 0 (no
    // successful spin yet).
    if (state_.gold_player >= 0 && state_.gold_prize >= 0) {
        // Clogs (prize 13) is NOT a sim::PowerupType (doc §8/§9.2 — never a
        // scheme/spawn kind) — it routes to MatchConfig::born_with_clogs
        // instead of wheel_prize_to_powerup/born_with_extra, alongside (not
        // instead of) the normal-kind branch below.
        bool is_clogs = state_.gold_prize == kClogsPrizeId;
        sim::PowerupType pt =
            is_clogs ? sim::PowerupType::None : wheel_prize_to_powerup(state_.gold_prize);
        if (pt != sim::PowerupType::None || is_clogs) {
            auto kind = static_cast<int>(pt);
            if (state_.team_play) {
                // Team mode: the doc's "team id encoded as 0 or 2" compares
                // against the RAW +84 byte, i.e. our setup_team_[] before the
                // +1 shift above — every member of the gold TEAM gets the
                // bump (doc §4 "every member of the gold team").
                for (int i = 0; i < sim::kMaxPlayers; ++i) {
                    if (!cfg.active[i] || state_.setup_team[i] != state_.gold_player) continue;
                    if (is_clogs)
                        cfg.born_with_clogs[i] =
                            1;  // reset-then-+1 every round, §9.3 — not accumulated
                    else
                        cfg.born_with_extra[i][kind] = true;
                }
            } else if (state_.gold_player < sim::kMaxPlayers && cfg.active[state_.gold_player]) {
                if (is_clogs)
                    cfg.born_with_clogs[state_.gold_player] = 1;  // reset-then-+1 every round, §9.3
                else
                    cfg.born_with_extra[state_.gold_player][kind] = true;
            }
        }
    }
    // Level from the LEVEL screen (present_map_select -> dword_464998): the match
    // init (sub_410B6E) resolves it to a stage index dword_46499C. RANDOM (-1) ->
    // keep pick_stage over the enabled rotation (VALUELST 1150-1160, the same
    // 200-try random loop the original runs); a specific level (0..10) -> use that
    // index directly. Clamp to the valid stage range defensively.
    int stage;
    if (state_.selected_level < 0) {
        // RANDOM: pick from the registry's enabled rotation. ctx_.assets.levels()
        // defaults to the 11 built-ins, so this is byte-identical to the old
        // pick_stage(tuning, seed); a registered custom map joins the rotation.
        stage = match::pick_stage(state_.base_tuning, seed, ctx_.assets.levels());
    } else {
        stage = state_.selected_level;
        // Clamp to a level the registry knows (built-ins 0..10; a registered
        // custom map extends this). Unknown -> last built-in (10), matching the
        // old `if (stage > 10) stage = 10` for every reachable built-in index.
        if (!ctx_.assets.levels().find(stage)) stage = 10;
    }
    // The sim's per-level gates (tile regeneration ids 340-350/695, ice/
    // input-lag ids 450-460 — docs/re/facts.md "Per-level tile regeneration",
    // "Ice / input-lag") are indexed by the SAME stage number as dword_46499C
    // in the original, i.e. exactly this `stage` value.
    cfg.tuning.level_index = stage;
    // Overlay this board's stage actors (conveyors/trampolines/etc) from
    // EXTRA<stage>.RES before constructing the sim — the actor layout is a
    // hashed setup input like the cell grid (docs/re/stage-actors.md). A board
    // with no EXTRA file simply has none. Random '-T,H' trampolines resolve off
    // a setup-only RNG inside apply_actors, never the sim's per-tick stream.
    auto actors =
        assets::extra::load_for_board(state_.game_dir, stage, sim::kGridWidth, sim::kGridHeight);
    match::apply_actors(cfg, actors, seed);
    // Campaign rover/ghost hazards (docs/re/campaign.md "Rover/ghost/AI
    // roster", "sub_40151B — the REAL per-stage starter"): fields 3-6 of the
    // current stage's .CAM record. build_state (setup.cpp) spawns them (ghost
    // first, then rover, matching sub_40151B's own call order) as part of
    // Simulation's constructor. A non-campaign match leaves these at 0
    // (MatchConfig's default), so RoverSystem::spawn/tick are true no-ops.
    if (state_.campaign_active && state_.campaign_stage_index >= 0 &&
        state_.campaign_stage_index < static_cast<int>(state_.campaign_stages.size())) {
        const assets::res::CampaignStage& stage_rec =
            state_.campaign_stages[static_cast<std::size_t>(state_.campaign_stage_index)];
        cfg.campaign_rovers = stage_rec.rovers;
        cfg.campaign_rover_speed = stage_rec.rover_speed;
        cfg.campaign_ghosts = stage_rec.ghosts;
        cfg.campaign_ghost_speed = stage_rec.ghost_speed;
    }
    // --demo / --demo-shots: disarm the round-start input freeze (VALUELST
    // id 30 ≈ 1 s of dead input, facts.md "Round-start input freeze") — the
    // scripted demo match's tick-indexed input script and the visual-golden
    // shot ticks (tests/visual/shots.txt) were all captured acting from tick
    // 0, and shifting the whole choreography by 20 ticks would re-time every
    // pinned frame for no coverage gain. A demo-fixture pin like the
    // LETTERBOX scaler in init(); live play keeps the authentic freeze.
    if (state_.demo) cfg.tuning.input_freeze_ticks = 0;
    return cfg;
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

void MatchRunner::draw_player_row(const sim::State& s) {
    auto splice_next = [](std::string& f, int v) {
        auto p = f.find('%');
        if (p == std::string::npos) return;
        std::size_t q = p + 1;
        while (q < f.size() && f[q] != 'u' && f[q] != 'd' && f[q] != 'i') ++q;
        if (q < f.size()) f = f.substr(0, p) + std::to_string(v) + f.substr(q + 1);
    };
    for (int i = 0; i < sim::kMaxPlayers; ++i) {
        // byte_461BD4 (+0x10, "alive/on-screen" per facts.md's "Player struct"
        // entry) gates the whole row entry -> sim::Player::present, which is
        // set once at match setup and stays true for the rest of the match
        // regardless of round elimination (unlike `alive`, checked below).
        if (!s.players[i].present) continue;
        int col = i / 2;  // getvalue(115 + i/2): 5 columns, VALUELST 10/110/210/310/410
        int row = i & 1;  // getvalue(113 + i&1): 2 rows, VALUELST 6/26
        float x = static_cast<float>(ctx_.values.column_or(115 + col, 0, 10 + 100 * col));
        float y = static_cast<float>(ctx_.values.column_or(113 + row, 0, 6 + 20 * row));

        std::string line = ctx_.assets.getstring(37, "S:%d K:%d");
        splice_next(line, state_.win_count[i]);
        splice_next(line, state_.kill_count[i]);
        std::uint8_t c[3];
        ctx_.assets.slot_color(i, c);
        // In-match "S:x K:y" score overlay: sub_41696C (batch_0x420D4E.cpp's
        // per-frame count-alive loop) — ink sub_41672F(i) == slot_color, outline
        // sub_416867(i): black in team mode, else white for player 1 (the black
        // bomberman) and black for everyone else. (Not in the demo/golden path —
        // run_app draws it, run_demo does not.)
        const std::uint8_t ol = (!::bomber::game::is_team_mode(state_.team_play, state_.sim.state(),
                                                               state_.setup_team) &&
                                 i == 1)
                                    ? 255
                                    : 0;
        ctx_.front_font.draw_outlined(ctx_.sdl, line, x, y, c[0], c[1], c[2], ol, ol, ol);

        // dword_461BC4 (+0x00, "active/moving state") gates the "xxx" overlay
        // -> sim::Player::alive, the per-ROUND flag (reset every round,
        // unlike `present` above) — a player dead THIS round still keeps
        // their score visible underneath the marker.
        if (!s.players[i].alive && !ctx_.seqs.eliminated_marker.steps.empty()) {
            const Sprite& sp = ctx_.seqs.eliminated_marker.steps[0];
            if (sp.tex) {
                SDL_FRect dst{x - static_cast<float>(sp.hx), y - static_cast<float>(sp.hy),
                              static_cast<float>(sp.w), static_cast<float>(sp.h)};
                SDL_RenderTexture(ctx_.sdl, sp.tex, nullptr, &dst);
            }
        }
    }
}

void MatchRunner::draw_fps_overlay(int fps) {
    if (!state_.show_fps || !ctx_.front_font.loaded()) return;
    // Three compact lines hard in the top-right corner, stacked: fps, then the
    // cadence state, then the vsync state. Right-aligned and drawn at a reduced
    // SCALE via FontTextures::draw's `scale` (dst-rect only — NEVER
    // SDL_SetRenderScale, which perturbed the whole render transform). Small
    // enough that all three sit ABOVE the match clock rather than over it. GREEN
    // marks the native-feel state of each lever; a manual 1-px black outline
    // keeps them legible over the field.
    constexpr float kS = 0.7f;
    const float right = static_cast<float>(kScreenW) - 3.0f;
    const float lh = static_cast<float>(ctx_.front_font.line_height()) * kS;
    auto line = [&](const std::string& s, float y, bool hot) {
        const float x = right - static_cast<float>(ctx_.front_font.measure(s)) * kS;
        ctx_.front_font.draw(ctx_.sdl, s, x - 1, y, 0, 0, 0, kS);
        ctx_.front_font.draw(ctx_.sdl, s, x + 1, y, 0, 0, 0, kS);
        ctx_.front_font.draw(ctx_.sdl, s, x, y - 1, 0, 0, 0, kS);
        ctx_.front_font.draw(ctx_.sdl, s, x, y + 1, 0, 0, 0, kS);
        ctx_.front_font.draw(ctx_.sdl, s, x, y, hot ? 120 : kDialogInkR, hot ? 240 : kDialogInkG,
                             hot ? 120 : kDialogInkB, kS);
    };
    char buf[24];
    std::snprintf(buf, sizeof(buf), "%d FPS", fps);
    float y = 2.0f;
    line(buf, y, state_.uncap_fps);
    y += lh;
    line(state_.native_cadence ? "NATIVE" : "20HZ", y, state_.native_cadence);
    y += lh;
    line(state_.uncap_fps ? "UNCAP" : "VSYNC", y, state_.uncap_fps);
}

AppInput MatchRunner::run() {
    // A NETPLAY match (increment 5b) is seeded canonically by
    // GameApp::run_netplay BEFORE run() — both peers build a byte-identical
    // MatchConfig from the shared seed, independent of each machine's
    // options.ini/level pick, so re-running start_match here (which reads the
    // per-machine roster/options/level) would clobber that parity. A normal
    // match still (re)builds + seeds the sim here every round, as before.
    // GOLDEN-SAFE: net_session is null on every non-netplay path, so this
    // guard is transparent to the golden/demo/normal-match callers.
    if (!state_.net_session) start_match(state_.next_seed++);
    const std::uint64_t tick_ns = 1'000'000'000ull / sim::kTicksPerSecond;
    std::uint64_t last = SDL_GetTicksNS();
    std::uint64_t acc = 0;
    // Round-end gating. `over_ticks` is a plain countdown (the campaign
    // hazard-clear grace, and the netplay fallback); `await_death_fx` is the
    // animation-driven wait the deciding kill arms instead — see
    // advance_round_end below for why the two differ.
    int over_ticks = -1;
    bool await_death_fx = false;
    // Frame pacing (docs/re/in-match-shell.md's per-frame tick driver,
    // sub_42A191/sub_41E61E: the original is a DirectDraw flip loop — one
    // input read + at most one tick per DISPLAYED frame, the flip block IS
    // the throttle). GameApp::init() requests vsync (SDL_SetRenderVSync, its
    // comment explains why), but on Windows windowed mode SDL_RenderPresent
    // does NOT reliably block: DWM gives the swapchain a multi-frame flip
    // queue, so presents return instantly in bursts (measured 4-12 ms frame
    // deltas) until the queue fills, then stall (20-25 ms). The sim
    // accumulator crossings then land on that jerky CPU-side train and ticks
    // get assigned to frames in 2/4-frame beats instead of the steady
    // 3-frames-per-tick a 20 Hz sim on a 60 Hz display needs — measured with
    // the same live-run rig as the 2026-07-10 input-latency audit: 4-41% of
    // tick-to-tick gaps were a frame off (visible micro-stutter), whether or
    // not the old blind SDL_Delay(2) throttle ran after present. The fix is
    // explicit pacing: sleep until the next display-refresh boundary after
    // each present (SDL_DelayNS, target advanced by the measured refresh
    // period). When present genuinely blocks on vblank the target is already
    // reached and the sleep is a no-op (the resync branch keeps the target
    // phase-locked to the real vblank train); when it doesn't block, the
    // sleep supplies exactly the cadence vsync failed to. Input latency is
    // unchanged versus a truly-blocking vsync — one SDL_PollEvent + one
    // collect_inputs() sample per displayed frame either way, the original's
    // own acquisition bound — and no fixed extra delay sits on that path.
    // Refresh-rate mismatch (59.94 Hz panel reported as 60, VRR) only drifts
    // the target phase; the resync branch absorbs it. Unknown refresh falls
    // back to 60 Hz, which still bounds the loop (no uncapped free-run on
    // drivers where SDL_SetRenderVSync is a no-op, e.g. dummy video).
    std::uint64_t period_ns = 1'000'000'000ull / 60;
    if (const SDL_DisplayMode* mode =
            SDL_GetCurrentDisplayMode(SDL_GetDisplayForWindow(ctx_.window));
        mode && mode->refresh_rate_numerator > 0 && mode->refresh_rate_denominator > 0) {
        period_ns = 1'000'000'000ull * mode->refresh_rate_denominator /
                    static_cast<std::uint64_t>(mode->refresh_rate_numerator);
    }
    std::uint64_t pace_target_ns = SDL_GetTicksNS() + period_ns;
    // F8 FPS-indicator state: count presented frames and refresh the shown
    // figure ~4x/second (a 250 ms window) so the number is readable, not a
    // blur. Purely for the top-right overlay; nothing gameplay reads it.
    std::uint64_t fps_frames = 0;
    std::uint64_t fps_window_start_ns = SDL_GetTicksNS();
    int shown_fps = 0;
    // Per-player "action key seen down at a frame sample since the last
    // consumed tick" — the frame-cadence tap capture; see the sampling
    // comment inside the loop. Only action1/action2 are ever set.
    struct TapLatch {
        bool action1 = false, action2 = false;
    };
    std::array<TapLatch, sim::kMaxPlayers> tap_latch{};
    // Round-end / linger bookkeeping for ONE advanced 50 ms tick. Called from
    // both the fixed-tick catch-up loop and the F9 native-cadence path (once per
    // 50 ms systems pass). Returns true when the post-round linger has elapsed
    // and run_match should hand back to the Results flow.
    auto advance_round_end = [&]() -> bool {
        const sim::State& s = state_.sim.state();
        // Campaign hazard-clear grace timer (docs/re/campaign.md "Round pacing"
        // clause 3, sub_4016DA's dword_4646C0): fires once when every hazard has
        // been dead kHazardClearTicks ticks — an independent early-out.
        if (over_ticks < 0 && !await_death_fx && state_.campaign_active &&
            s.hazard_clear_timer == sim::kHazardClearTicks) {
            over_ticks = 3 * sim::kTicksPerSecond;
        }
        // Team-aware round-over: "one SIDE left" (docs/re/ai.md TEAM follow-up);
        // sides_remaining() degenerates to alive_count() in a solo match.
        //
        // WHEN the results screen takes over is not a timer in the original, and
        // this used to be a flat 3 s. The round driver sub_42A3F6 ends each pass
        // of its loop with two guards: the player count sub_421947 (call at
        // 0x42A6B2, keep looping while > 1) and the clock-expired predicate
        // sub_41087D (0x42A6BE) — no sleep sits between them and the outcome
        // tier. That count still includes a player who is mid-DEATH-ANIMATION:
        // the kill routine sub_41DCB2 only raises the dying flag, and the
        // per-player pass sub_41F29B clears the slot's in-play flag only once
        // the death sequence has played its last step (its length coming from
        // sub_41DA5C). So the round is over one frame after the LAST corpse
        // finishes animating. The shipped "die green" sequences run 12-93 steps
        // (0.6-4.65 s at our 20 Hz), so a fixed 60-tick linger truncated half of
        // them and sat on an already-finished field after the short ones.
        //
        // The clock-expired exit, by contrast, has no linger at all.
        if (over_ticks < 0 && !await_death_fx &&
            (sim::sides_remaining(s) <= 1 || s.ticks_left == 0)) {
            if (s.ticks_left == 0) {
                std::printf("time up — draw!\n");
                return true;  // straight into the outcome tier, nothing to wait for
            }
            for (int i = 0; i < sim::kMaxPlayers; ++i)
                if (s.players[i].present && s.players[i].alive) std::printf("player %d wins!\n", i);
            // NETPLAY keeps the old fixed linger. The death-sequence pool and
            // each sequence's step count come from the LOCAL install's DATA/ANI
            // files, which build_hash does not cover, so an animation-driven
            // handoff could land on a different tick on each peer and leave one
            // of them ticking a round the other has already walked out of. A
            // fixed count is the same tick on both.
            if (state_.net_session)
                over_ticks = 3 * sim::kTicksPerSecond;
            else
                await_death_fx = true;
        }
        // The round is over when nothing is left playing. death_fx_active()
        // measures each effect against its own sequence length, the same rule
        // draw_world retires it on — sub_41DA5C's step count.
        if (await_death_fx) return !state_.renderer.death_fx_active(s);
        return over_ticks > 0 && --over_ticks == 0;
    };
    while (true) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_EVENT_QUIT) return AppInput::Quit;
            // ATTRACT abort (docs/re/frontend-flow.md "Attract mode" point 3,
            // mirroring sub_42A3F6's round-loop tail, which jumps to LABEL_34
            // as soon as a keypress has set dword_464938): ANY key, mouse
            // button, or gamepad
            // button input during an attract demo returns to the menu
            // IMMEDIATELY — checked first, ahead of the specific-key
            // handling below, and only while attract_ is armed (a real match
            // never takes this branch, so a human round's own key bindings
            // are unaffected). run_app's StartMatch handler calls
            // restore_from_attract() unconditionally once this returns,
            // whether the round ended naturally or was aborted here — an
            // attract match never shows Results either way (point 2).
            if (state_.attract &&
                (ev.type == SDL_EVENT_KEY_DOWN || ev.type == SDL_EVENT_MOUSE_BUTTON_DOWN ||
                 ev.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN))
                return AppInput::MatchOver;
            // Ctrl+Q = CONFIRMED instant, unconfirmed-dialog forfeit
            // (docs/re/in-match-shell.md "Esc negative finding": raw key 0x11
            // = 17 = Ctrl+Q is the ONLY key that aborts a round mid-match in
            // the original — dword_46492C=-1/dword_464A68=2, no confirm
            // prompt, straight to the standard teardown). Wired here as the
            // faithful key.
            if (ev.type == SDL_EVENT_KEY_DOWN && ev.key.key == SDLK_Q &&
                (ev.key.mod & SDL_KMOD_CTRL) != 0)
                return AppInput::MatchOver;
            // Esc also bails to the menu — a PORT CONVENIENCE, not a binary
            // fact: the same doc's finding is that literal Esc (27) is INERT
            // mid-round in the original (falls through the round loop's key
            // chain untouched; only Ctrl+Q aborts). We keep this binding
            // anyway because it gives players a familiar "quit to menu" key,
            // functionally standing in for the original's Ctrl+Q rather than
            // matching its own (inert) Esc — see the doc's "Port status"
            // paragraph for the full rationale.
            if (ev.type == SDL_EVENT_KEY_DOWN && ev.key.key == SDLK_ESCAPE)
                return AppInput::MatchOver;
            // docs/re/in-match-shell.md §1, auxiliary key table row
            // 0x13B=315=F1 (local only, matching `!sub_40C06A()` — no
            // network gate needed here since this port has no network play):
            // opens the SAME generic *.BM help browser row 5 opens
            // (sub_41431C -> sub_414235, §4) without leaving the round,
            // bracketed by the tick-suspend guard sub_42A16F(1)/(0)
            // (pseudo.c 29769-29771) — "the whole game freezes under the
            // help overlay: sim, rendering, HUD, everything" while it is up.
            if (ev.type == SDL_EVENT_KEY_DOWN && ev.key.key == SDLK_F1) {
                // sub_42A16F(1): suspend the tick callback. present_help_
                // browser_modal draws the LAST rendered match frame as its
                // backdrop (never advancing the sim) and returns once the
                // browser is dismissed.
                AppInput help = HelpBrowserModal(ctx_, {state_.renderer, state_.sim.state()}).run();
                if (help == AppInput::Quit) return AppInput::Quit;
                // sub_42A16F(0): resume. Reset the accumulator/clock instead
                // of reproducing the original's documented bug (the round
                // clock silently absorbs the whole modal duration in one
                // lump, §1's "Pause negative finding" point 2) — a
                // DELIBERATE deviation, per this task's brief, so closing
                // the browser does not fire a tick burst or eat round time.
                last = SDL_GetTicksNS();
                acc = 0;
                continue;
            }
            // A pad unplugged/plugged mid-match: rescan so a disconnect drops
            // that slot to neutral input (via collect_inputs' range check)
            // rather than leaving it wedged, and a reconnect resumes control at
            // its old index without needing a trip back to the setup screen.
            if (ev.type == SDL_EVENT_GAMEPAD_ADDED || ev.type == SDL_EVENT_GAMEPAD_REMOVED)
                ctx_.gamepads.refresh();
        }

        // Frame-cadence action-key capture (docs/re/in-match-shell.md "Input
        // acquisition"). The ORIGINAL samples its live key-state array
        // byte_4A2BA0 (maintained by sub_433E14 from DirectInput BUFFERED
        // records — sub_43B5EC/sub_43B700/sub_4449B0's GetDeviceData drain)
        // once per DISPLAYED FRAME: sub_41F29B shuffles the key bytes
        // (+54=+56, then +56=0; pseudo.c 22976-22979) and re-reads them via
        // sub_41E61E (23037) in the same per-frame callback, so its
        // edge-gated bomb drop can only miss a tap shorter than ONE display
        // frame (~14-16 ms). Feeding the sim a state sample taken only once
        // per 50 ms tick widened that loss window ~3x — a normal human tap
        // (~30-40 ms) could fall entirely between two tick samples and the
        // bomb press silently vanished. Restore the original's cadence:
        // sample the mapped inputs here, once per rendered frame (this loop
        // is the port's equivalent of the flip-loop callback), and latch
        // action-key downs until the next tick consumes them. Directions are
        // deliberately NOT latched: they are level-driven (the original
        // integrates held time in ms, so a sub-tick tap moved a few px at
        // most — stretching it to a full 50 ms tick budget would overshoot
        // the original far more than dropping it does), while action1/2 are
        // EDGE-consumed — capture-or-lose — which is exactly what the frame
        // sampling exists to capture.
        const sim::TickInputs frame_in = collect_inputs();
        for (int i = 0; i < sim::kMaxPlayers; ++i) {
            tap_latch[i].action1 = tap_latch[i].action1 || frame_in.players[i].action1;
            tap_latch[i].action2 = tap_latch[i].action2 || frame_in.players[i].action2;
        }

        std::uint64_t now = SDL_GetTicksNS();
        const std::uint64_t delta_ns = now - last;
        acc += delta_ns;
        last = now;
        // One loop iteration == one SDL_RenderPresent below; tally it and
        // recompute the shown rate once the 250 ms window elapses.
        ++fps_frames;
        if (const std::uint64_t span = now - fps_window_start_ns; span >= 250'000'000ull) {
            shown_fps = static_cast<int>(fps_frames * 1'000'000'000ull / span);
            fps_frames = 0;
            fps_window_start_ns = now;
        }
        // Netplay MUST use the deterministic fixed-tick path: Simulation::frame()
        // (the F9 native-cadence driver) advances on the real wall-clock delta,
        // which differs per machine and would instantly desync the peers. So the
        // net_session gate forces the else branch below regardless of the live F9
        // lever (a normal match is unchanged — net_session is null).
        if (state_.native_cadence && !state_.net_session) {
            // F9 native-cadence path: advance the sim ONE displayed frame on the
            // measured wall-clock delta. Simulation::frame runs the movement/AI
            // pass at frame rate and drains the 50 ms systems pass off its own
            // accumulator, so this is the original's per-frame gameplay driver
            // (sub_42A191) — low input latency, fps-scaled granularity — but
            // NON-DETERMINISTIC (real delta). Consume the action-key taps this
            // frame; run the round-end bookkeeping once per 50 ms tick advanced.
            std::int32_t delta_ms = static_cast<std::int32_t>(delta_ns / 1'000'000ull);
            if (delta_ms < 1) delta_ms = 1;
            if (delta_ms > 4 * sim::kMsPerTick) delta_ms = 4 * sim::kMsPerTick;
            sim::TickInputs in = frame_in;
            for (int i = 0; i < sim::kMaxPlayers; ++i) {
                in.players[i].action1 = in.players[i].action1 || tap_latch[i].action1;
                in.players[i].action2 = in.players[i].action2 || tap_latch[i].action2;
                tap_latch[i].action1 = false;
                tap_latch[i].action2 = false;
            }
            const std::uint64_t tick_before = state_.sim.state().tick;
            state_.sim.frame(in, delta_ms);
            ctx_.sounds.on_tick(state_.sim.state());
            // Pose countdowns age once per SIM TICK, not per displayed frame:
            // pass whether this frame actually crossed a tick (else kick/punch/
            // pickup poses play ~9x too fast in native cadence).
            // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
            state_.renderer.on_events(state_.sim.state(), state_.sim.state().tick != tick_before);
            state_.renderer.advance_tick(
                state_.sim.state());  // NOLINT(bugprone-unchecked-optional-access)
            tally_kills(state_.sim.state().events, state_.kill_count);
            for (std::uint64_t t = tick_before; t < state_.sim.state().tick; ++t)
                if (advance_round_end()) return AppInput::MatchOver;
            acc = 0;  // the fixed-tick accumulator is dormant on this path
        } else {
            // Long-stall guard (spiral-of-death / teleport clamp). A window drag,
            // alt-tab, asset stall, or a debugger break can hand us a multi-hundred-
            // ms delta; without a cap the `while` below fires that many catch-up
            // ticks in one frame — the sim lurches (entities snap-teleport across
            // the board, past the 32 px interp snap threshold) and, worse, the loop
            // can wedge trying to out-run real time. Cap the queue at a few ticks'
            // worth: excess wall-time is DROPPED (the match briefly runs in slow
            // motion) rather than fast-forwarded. Does not touch determinism — the
            // sim still advances one deterministic tick per crossing; only how many
            // crossings a single frailty-induced hitch produces is bounded.
            constexpr std::uint64_t kMaxCatchupTicks = 4;
            if (acc > kMaxCatchupTicks * tick_ns) acc = kMaxCatchupTicks * tick_ns;
            while (acc >= tick_ns) {
                // Consume the frame-sampled latch on the FIRST tick of a catch-up
                // burst only (a later tick in the same burst re-reads the live
                // state, matching the original's one-edge-check-per-update under
                // a slow frame — its clamped ms delta produces exactly one
                // sub_41E61E read per displayed frame too).
                //
                // Build this tick's input from the frame sample + the latched taps.
                // The latch-clear + `acc -= tick_ns` move to AFTER the tick; on the
                // LOCAL path this is behaviourally identical to the old consume-then-
                // tick order (nothing reads the latch or `acc` between here and there).
                sim::TickInputs in = frame_in;
                for (int i = 0; i < sim::kMaxPlayers; ++i) {
                    in.players[i].action1 = in.players[i].action1 || tap_latch[i].action1;
                    in.players[i].action2 = in.players[i].action2 || tap_latch[i].action2;
                }
                // Netplay drives the SAME borrowed sim through the ROLLBACK session
                // (deterministic tick() only — never frame()): advance() sends our
                // seats, PREDICTS the peer's still-missing input (repeat-last), ticks
                // the predicted frame, and transparently rolls back + re-simulates when
                // the real input arrives and differs. So the local player sees ZERO
                // input delay and the match runs at real time — no stalling on the
                // network the way input-delay lockstep did. A local match ticks
                // directly, exactly as before (net_session is null everywhere else).
                if (state_.net_session) {
                    state_.net_session->advance(in);
                } else {
                    state_.sim.tick(in);
                }
                // The tick actually happened — NOW consume the taps and one tick's
                // worth of the accumulator, then run the per-tick bookkeeping.
                for (int i = 0; i < sim::kMaxPlayers; ++i) {
                    tap_latch[i].action1 = false;
                    tap_latch[i].action2 = false;
                }
                acc -= tick_ns;
                ctx_.sounds.on_tick(state_.sim.state());
                state_.renderer.on_events(
                    state_.sim.state());  // NOLINT(bugprone-unchecked-optional-access)
                // Roll the renderer's inter-tick snapshots forward for THIS tick,
                // inside the catch-up loop — so a frame that advances the sim two
                // ticks still leaves interp `prev` at the penultimate tick (a clean
                // 1-tick lerp span) instead of two ticks back (the snap/double-speed
                // jitter). Tick-keyed, so draw_frame's own trailing call is a no-op
                // on the live path and still primes the demo/screenshot path.
                state_.renderer.advance_tick(
                    state_.sim.state());  // NOLINT(bugprone-unchecked-optional-access)
                // §1's kill tally (sub_421B0F): a GameApp-side pass over this
                // tick's events, separate from the renderer's own on_events walk
                // (renderer_ never mutates GameApp state — CLAUDE.md's libs/game
                // boundary). Cumulative for the whole match (see kill_count_'s
                // doc comment); reset only in reset_match_scores().
                tally_kills(state_.sim.state().events, state_.kill_count);

                if (advance_round_end()) return AppInput::MatchOver;
            }
        }  // end else: the deterministic fixed-tick accumulator path

        ctx_.audio.update_music();
        // Gold Bomberman twinkle (docs/re/goldman-roulette.md §6): tell the
        // renderer which player/team is the pending gold winner every frame —
        // gold_player_ only changes between rounds, but this is a cheap int
        // pair and keeps the renderer decoupled from GameApp's own state.
        // NOLINTNEXTLINE(bugprone-unchecked-optional-access) — emplaced in init()
        state_.renderer.set_gold_player(
            state_.gold_player,
            ::bomber::game::is_team_mode(state_.team_play, state_.sim.state(), state_.setup_team));
        // Tell the renderer which animation clock to use (F9): per-frame walk/
        // fidget phase advance in native cadence, once-per-tick otherwise.
        // NOLINTNEXTLINE(bugprone-unchecked-optional-access) — emplaced in init()
        state_.renderer.set_native_cadence(state_.native_cadence);
        // F9: glide fraction for the 50 ms-stepped entities (flying/sliding
        // bombs, rovers) = how far into the current 50 ms tick this frame falls.
        // NOLINTNEXTLINE(bugprone-unchecked-optional-access) — emplaced in init()
        state_.renderer.set_entity_interp(state_.native_cadence
                                              ? static_cast<float>(state_.sim.systems_accum_ms()) /
                                                    static_cast<float>(sim::kMsPerTick)
                                              : 1.0f);
        // Inter-tick interpolation fraction (renderer.hpp's draw_frame doc):
        // acc < tick_ns after the catch-up loop, so this is in [0,1) — how far
        // into the current 50 ms tick this displayed frame falls. The original
        // needed no such blend because its gameplay driver itself ran per
        // displayed frame on the ms delta (sub_42A191); our fixed 20 Hz sim
        // recovers that on-screen fluidity here, cosmetically.
        // Native-cadence mode renders the sim's live state directly (alpha=1 =>
        // player_interp/interp_pos return the current position, no lerp): the
        // sim already ran at frame rate this frame, so there is nothing to blend.
        float interp_alpha =
            state_.native_cadence ? 1.0f : static_cast<float>(acc) / static_cast<float>(tick_ns);
        // A NETPLAY stall breaks the catch-up loop early with acc still >=
        // tick_ns (the sim is frozen waiting for the peer), which would push
        // alpha past 1 and EXTRAPOLATE entities forward during the pause; clamp
        // so the last simulated pose is held instead. No-op on every other path
        // (the loop always drains acc below tick_ns there, so alpha < 1 already).
        if (interp_alpha > 1.0f) interp_alpha = 1.0f;
        // NOLINTNEXTLINE(bugprone-unchecked-optional-access) — emplaced in init()
        state_.renderer.draw_frame(state_.sim.state(), interp_alpha);
        // The player-row HUD strip (docs/re/in-match-shell.md "The player
        // row") needs GameApp's own win_count_/kill_count_/front_font_, none
        // of which Renderer owns — drawn as a GameApp-side overlay on top of
        // Renderer's frame, same layering the original has (sub_420F07 draws
        // it every tick, after the field/world but the clock/hurry HUD is
        // logically part of the same pass).
        draw_player_row(state_.sim.state());
        draw_fps_overlay(shown_fps);
        SDL_RenderPresent(ctx_.sdl);
        // Refresh-boundary pacer — see the pacing comment at the top of this
        // function. No-op when present already blocked past the target;
        // supplies the missing block (and re-phases the target) when it
        // didn't.
        // Pace target: the refresh period by default, or the sim's sub-frame
        // period when F8's uncapped mode is armed (see uncap_fps_). At the
        // sub-frame rate every canonical frame player_interp can distinguish
        // reaches the screen — capping any higher would only re-show sub-frames
        // (there are just kSubFrames per tick), so this is the useful ceiling,
        // not a hard free-run. The else-branch resync makes a mid-match toggle
        // self-correct within a frame.
        const std::uint64_t pace_period_ns =
            state_.uncap_fps ? tick_ns / sim::kSubFrames : period_ns;
        std::uint64_t after_present_ns = SDL_GetTicksNS();
        if (after_present_ns < pace_target_ns) {
            SDL_DelayNS(pace_target_ns - after_present_ns);
            pace_target_ns += pace_period_ns;
        } else {
            pace_target_ns = after_present_ns + pace_period_ns;
        }
    }
}

}  // namespace bomber::game
