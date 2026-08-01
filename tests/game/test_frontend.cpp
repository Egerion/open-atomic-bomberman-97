// Locks the SDL-free front-end screen/state-machine core (app_flow.hpp): the
// pure next(state, input) transition that mirrors the original's boot path
// (sub_42B060 logos+title -> sub_42B9CE menu loop, docs/re/frontend-flow.md).
// This is presentation-only glue — it never touches the sim, so there is no
// determinism/golden impact; the doctest just pins the flow graph and its
// skip/timeout/quit/menu-hub edges. See docs/adr/0004-frontend-screen-flow.md.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <array>
#include <cstdint>
#include <optional>
#include <vector>

#include "bomber/game_util/app_flow.hpp"
#include "bomber/game_util/hud_format.hpp"
#include "bomber/game_util/match_outcome.hpp"
#include "bomber/game_util/results.hpp"
#include "bomber/input/dos_scancode.hpp"
#include "bomber/input/input.hpp"
#include "bomber/sim/constants.hpp"
#include "bomber/sim/event.hpp"

using bomber::game::AppInput;
using bomber::game::AppState;
using bomber::game::assign_gold_player;
using bomber::game::attract_computer_count;
using bomber::game::attract_stage_pick;
using bomber::game::campaign_round_needs_replay;
using bomber::game::clock_warning;
using bomber::game::cycle_slot_input_type;
using bomber::game::default_setup_team;
using bomber::game::fill_attract_roster;
using bomber::game::format_clock;
using bomber::game::gold_twinkle_matches;
using bomber::game::is_terminal;
using bomber::game::kClockWarningSeconds;
using bomber::game::KeyAction;
using bomber::game::KeySet;
using bomber::game::kKeyActionCount;
using bomber::game::kKeyboardSets;
using bomber::game::next;
using bomber::game::reset_setup_teams;
using bomber::game::seed_campaign_ai_slots;
using bomber::game::SlotInputType;
using bomber::game::tally_kills;
using bomber::game::victory_background_name;
using bomber::game::win_by_kills_clinch;
using bomber::sim::Event;
using bomber::sim::kMaxPlayers;

TEST_CASE("the nominal boot path walks Boot->Logo->Title->Menu->Match->Results->Menu") {
    // Boot/Logo/Title accept on the same event (Advance) — a keypress OR the
    // attract timeout, exactly as sub_42A088 synthesizes Enter on getvalue(12).
    AppState s = AppState::Boot;
    s = next(s, AppInput::Advance);
    CHECK(s == AppState::Logo);
    s = next(s, AppInput::Advance);
    CHECK(s == AppState::Title);
    s = next(s, AppInput::Advance);
    CHECK(s == AppState::Menu);
    // The menu is a hub: the Start/Play item resolves to StartMatch.
    s = next(s, AppInput::StartMatch);
    CHECK(s == AppState::Match);

    // A running match ignores stray accepts; only MatchOver advances it.
    CHECK(next(AppState::Match, AppInput::Advance) == AppState::Match);
    CHECK(next(AppState::Match, AppInput::Back) == AppState::Match);
    CHECK(next(AppState::Match, AppInput::StartMatch) == AppState::Match);
    s = next(s, AppInput::MatchOver);
    CHECK(s == AppState::Results);

    // Results returns to the menu, closing the loop.
    s = next(s, AppInput::Advance);
    CHECK(s == AppState::Menu);
}

TEST_CASE("the menu is a hub: each item routes to its leaf, every leaf returns") {
    // Selecting a working leaf.
    CHECK(next(AppState::Menu, AppInput::StartMatch) == AppState::Match);
    // Selecting each .BM-backed leaf (STUBs this round).
    CHECK(next(AppState::Menu, AppInput::OpenOptions) == AppState::Options);
    CHECK(next(AppState::Menu, AppInput::OpenControllers) == AppState::Controllers);
    CHECK(next(AppState::Menu, AppInput::OpenNetwork) == AppState::Network);
    // The two netplay rows (START/JOIN NET GAME) route to their connect leaves.
    CHECK(next(AppState::Menu, AppInput::OpenNetHost) == AppState::NetHost);
    CHECK(next(AppState::Menu, AppInput::OpenNetJoin) == AppState::NetJoin);
    CHECK(next(AppState::Menu, AppInput::OpenCredits) == AppState::Credits);

    // Every leaf returns to the menu on accept AND on Back — a dismissable
    // screen has nowhere else to go (sub_42B9CE re-enters its loop after each);
    // the netplay connect leaves fall back the same way once the match ends.
    for (AppState leaf : {AppState::Options, AppState::Controllers, AppState::Network,
                          AppState::NetHost, AppState::NetJoin, AppState::Credits,
                          AppState::Results}) {
        CHECK(next(leaf, AppInput::Advance) == AppState::Menu);
        CHECK(next(leaf, AppInput::Back) == AppState::Menu);
    }

    // A bare Advance in the menu (no item resolved) is inert — the SDL menu
    // turns the highlighted row into a specific Open*/StartMatch event.
    CHECK(next(AppState::Menu, AppInput::Advance) == AppState::Menu);
    CHECK(next(AppState::Menu, AppInput::MatchOver) == AppState::Menu);
}

TEST_CASE("the getvalue(12)=7s timeout walks the boot chain to the menu, linearly") {
    // The boot path is LINEAR (sub_42B060): with no player input the SDL shell
    // feeds Advance on each screen's 7 s dwell timeout, and the title's timeout
    // falls straight through to the menu (Enter is synthesized, sub_42B060
    // returns) — there is NO attract re-run of the logos/title. So an unattended
    // machine reaches the menu on its own and stays there.
    AppState s = AppState::Boot;
    for (int i = 0; i < 3; ++i) s = next(s, AppInput::Advance);  // Boot->Logo->Title->Menu
    CHECK(s == AppState::Menu);
    // Once at the menu the timeout does not bounce back into the intro.
    CHECK(next(AppState::Title, AppInput::Advance) == AppState::Menu);
}

TEST_CASE("Back skips and backs out along the flow") {
    // Back at the very first frame is the -nologo fast path: straight to menu.
    CHECK(next(AppState::Boot, AppInput::Back) == AppState::Menu);
    // Back leaves the logo for the title (any accept leaves a logo).
    CHECK(next(AppState::Logo, AppInput::Back) == AppState::Title);
    // Nothing sits behind the title or the menu, so Back there quits.
    CHECK(next(AppState::Title, AppInput::Back) == AppState::Quit);
    CHECK(next(AppState::Menu, AppInput::Back) == AppState::Quit);
    // Back on the results screen or any leaf just returns to the menu.
    CHECK(next(AppState::Results, AppInput::Back) == AppState::Menu);
    CHECK(next(AppState::Options, AppInput::Back) == AppState::Menu);
}

TEST_CASE("Quit short-circuits from every state and is terminal") {
    const AppState all[] = {
        AppState::Boot,    AppState::Logo,    AppState::Title,   AppState::Menu,
        AppState::Match,   AppState::Results, AppState::Options, AppState::Controllers,
        AppState::Network, AppState::NetHost, AppState::NetJoin, AppState::Credits,
        AppState::Quit,
    };
    for (AppState s : all) {
        CHECK(next(s, AppInput::Quit) == AppState::Quit);
    }
    CHECK(is_terminal(AppState::Quit));
    CHECK_FALSE(is_terminal(AppState::Menu));
    // Quit is a fixed point: no event escapes it.
    CHECK(next(AppState::Quit, AppInput::Advance) == AppState::Quit);
    CHECK(next(AppState::Quit, AppInput::Back) == AppState::Quit);
    CHECK(next(AppState::Quit, AppInput::MatchOver) == AppState::Quit);
    CHECK(next(AppState::Quit, AppInput::StartMatch) == AppState::Quit);
    CHECK(next(AppState::Quit, AppInput::RoundContinue) == AppState::Quit);
    CHECK(next(AppState::Quit, AppInput::CampaignContinue) == AppState::Quit);
}

TEST_CASE("best-of-N: a not-yet-decided round loops Results -> Match again") {
    // docs/re/frontend-flow.md "results flow": a survivor exists but nobody has
    // reached win_target_ yet (the RESULTS tally tier) -- and a draw (no
    // survivor) -- both replay the next round with the same roster/settings.
    // The SDL shell resolves round_winner() + the win tally into RoundContinue
    // BEFORE calling next(); the pure graph just loops on that event.
    AppState s = AppState::Match;
    s = next(s, AppInput::MatchOver);
    CHECK(s == AppState::Results);

    // Not decided (RESULTS tally tier, or a DRAW): loop straight back to Match
    // for the next round -- no detour through the menu.
    s = next(s, AppInput::RoundContinue);
    CHECK(s == AppState::Match);

    // The loop can repeat for as many rounds as the match needs.
    s = next(s, AppInput::MatchOver);
    CHECK(s == AppState::Results);
    s = next(s, AppInput::RoundContinue);
    CHECK(s == AppState::Match);
}

TEST_CASE(
    "campaign: CampaignContinue loops Results -> Match like RoundContinue, for a clinched match") {
    // docs/re/campaign.md "Advances through campaign stages automatically":
    // a clinched match (VICTORY) with campaign stages remaining routes back
    // to Match for the NEXT stage instead of the menu -- the SDL shell has
    // already loaded that stage's scheme/roster before feeding this event
    // (game_app.cpp's Results handler). The pure graph treats it exactly
    // like RoundContinue (Results -> Match); only the SDL-side state it
    // carries differs.
    AppState s = AppState::Match;
    s = next(s, AppInput::MatchOver);
    CHECK(s == AppState::Results);
    s = next(s, AppInput::CampaignContinue);
    CHECK(s == AppState::Match);
}

TEST_CASE("best-of-N: a decided match (VICTORY) or an explicit Back leaves Results for the menu") {
    // A player reaching win_target_ (VICTORY<n>) or a draw are both round-
    // ending outcomes shown on Results; when the match IS decided the shell
    // feeds a plain Advance (or the player hits Back/Escape), which returns to
    // the menu and ends the match, exactly like the pre-existing single-round
    // flow this loop extends.
    CHECK(next(AppState::Results, AppInput::Advance) == AppState::Menu);
    CHECK(next(AppState::Results, AppInput::Back) == AppState::Menu);

    // Reaching the menu from Results resets nothing in the pure graph itself
    // (tallies are GameApp state, not part of AppState) -- but the app is back
    // at the hub, ready for a fresh StartMatch.
    AppState s = next(AppState::Results, AppInput::Advance);
    CHECK(next(s, AppInput::StartMatch) == AppState::Match);
}

TEST_CASE("Logo always yields the title regardless of which accept arrives") {
    // Whether the key or the timeout fires, a logo leads only to the title.
    CHECK(next(AppState::Logo, AppInput::Advance) == AppState::Title);
    CHECK(next(AppState::Logo, AppInput::Back) == AppState::Title);
}

// Locks the PLAYER INPUT TYPE SELECTION slot-type cycle (sub_421E80 @0x421E80,
// docs/re/setup-screens.md "Input-type cycle helpers"): off -> computer ->
// keyboard 0 -> keyboard 1 -> joystick 0..(n-1) -> off, where n is the number
// of CONNECTED gamepads at cycle time (GamepadMapper::count(), passed in — the
// helper itself is SDL-free so this doctest exercises it without a window or a
// physical pad).
TEST_CASE("cycle_slot_input_type walks OFF -> COMPUTER -> KBD0 -> KBD1 -> OFF with no pads") {
    int type = 0, sub = 0;
    const int joysticks = 0;
    cycle_slot_input_type(type, sub, joysticks);
    CHECK(type == static_cast<int>(SlotInputType::Computer));
    cycle_slot_input_type(type, sub, joysticks);
    CHECK(type == static_cast<int>(SlotInputType::Keyboard));
    CHECK(sub == 0);
    cycle_slot_input_type(type, sub, joysticks);
    CHECK(type == static_cast<int>(SlotInputType::Keyboard));
    CHECK(sub == 1);
    // No sticks connected: keyboard 1 wraps straight back to OFF, skipping the
    // joystick leg entirely (sub_429628 finds none present).
    cycle_slot_input_type(type, sub, joysticks);
    CHECK(type == static_cast<int>(SlotInputType::Off));
    CHECK(sub == 0);
}

TEST_CASE("cycle_slot_input_type walks through every present joystick before wrapping") {
    int type = static_cast<int>(SlotInputType::Keyboard), sub = 1;
    const int joysticks = 2;  // two pads connected
    cycle_slot_input_type(type, sub, joysticks);
    CHECK(type == static_cast<int>(SlotInputType::Joystick));
    CHECK(sub == 0);
    cycle_slot_input_type(type, sub, joysticks);  // joystick 0 -> joystick 1
    CHECK(type == static_cast<int>(SlotInputType::Joystick));
    CHECK(sub == 1);
    cycle_slot_input_type(type, sub, joysticks);  // last present stick -> off
    CHECK(type == static_cast<int>(SlotInputType::Off));
    CHECK(sub == 0);
}

TEST_CASE("cycle_slot_input_type reacts to the live joystick count, not a stale one") {
    // If a pad is unplugged between visits to the cycle (joystick count drops
    // to 0 after the slot already landed on JOYSTICK 0), the next Right press
    // must not get stuck cycling a stick that no longer exists — it wraps off.
    int type = static_cast<int>(SlotInputType::Joystick), sub = 0;
    cycle_slot_input_type(type, sub, /*joystick_count=*/0);
    CHECK(type == static_cast<int>(SlotInputType::Off));
    CHECK(sub == 0);
}

TEST_CASE("a full lap of the cycle returns to OFF, for any joystick count") {
    for (int joysticks : {0, 1, 3}) {
        int type = 0, sub = 0;
        int steps = 4 + joysticks;  // off->cpu->kbd0->kbd1->(joy0..joy(n-1))->off
        for (int i = 0; i < steps; ++i) cycle_slot_input_type(type, sub, joysticks);
        CHECK(type == static_cast<int>(SlotInputType::Off));
        CHECK(sub == 0);
    }
}

// Locks the setup-screen's TEAM default (sub_4049C0, pseudo.c line 6716 writes
// each slot's team field — dword_46481C, stride 12, offset +8 — with the slot
// index's low bit, i.e. slot parity; re-applied on EVERY entry to the setup
// screen via sub_410F81 -> sub_4046CC -> sub_403EEE -> sub_4049C0 —
// docs/re/setup-screens.md "TEAM default — CORRECTED 2026-07-09"). Before
// this fix `present_setup()` left every slot's team at its all-0 default, so
// Team Play ON without anyone pressing 'T' put every player on the SAME sim
// side: everyone got the WHITE colour override and sides_remaining() read
// <=1 from tick 0 (round "ends instantly").
TEST_CASE("default_setup_team alternates 0/1 by slot parity") {
    CHECK(default_setup_team(0) == 0);
    CHECK(default_setup_team(1) == 1);
    CHECK(default_setup_team(2) == 0);
    CHECK(default_setup_team(3) == 1);
    CHECK(default_setup_team(9) == 1);
}

// MatchRunner shifts this byte up by one on the way to the sim (`cfg.team[i] =
// setup_team[i] + 1`), because sim team 0 means "no team" — so the alternating
// default is what makes two players two DISTINCT non-zero sides. That shift, and
// the setup screen's own `team ? 0 : 1` toggle, are statements in SDL-linked
// screens: a headless case can only re-type them and then check its own
// arithmetic, so what is pinned here is the one half that lives in game_util.
TEST_CASE(
    "reset_setup_teams fills every slot with the alternating default, "
    "clobbering any earlier value") {
    std::array<int, kMaxPlayers> team{};
    team.fill(1);  // simulate a stale all-1 roster from a previous visit
    reset_setup_teams(team);
    for (int i = 0; i < kMaxPlayers; ++i) CHECK(team[i] == (i & 1));
    // In particular: two active players in the default two-player layout
    // (slots 0 and 1) land on DIFFERENT sides, not the same one. With the OLD
    // all-0 default they collapsed onto one sim side and the round ended on
    // tick 0.
    CHECK(team[0] != team[1]);
}

// Locks the key-remap UI's data shape (docs/re/results-and-options.md §2,
// sub_407B9D's "2x6 button grid"): 2 keyboard sets, 6 bindable actions each,
// in the CONFIRMED action-name id order (1120 Move Up .. 1125 Action 2).
// input.hpp keeps KeySet/KeyAction SDL-free (plain `int` scancodes) exactly
// so this shape is testable here without linking SDL3 (see input.hpp's file
// doc) — the SDL_Scancode interpretation itself (default_key_set(),
// KeyboardMapper::read()) lives in input.cpp, part of the SDL-linked
// bomber_game_core target, and is exercised only via the live app/manual QA.
TEST_CASE("KeySet/KeyAction shape: 2 keyboard sets, 6 actions each") {
    CHECK(kKeyboardSets == 2);
    CHECK(kKeyActionCount == 6);
    CHECK(static_cast<int>(KeyAction::Up) == 0);
    CHECK(static_cast<int>(KeyAction::Right) == 1);
    CHECK(static_cast<int>(KeyAction::Down) == 2);
    CHECK(static_cast<int>(KeyAction::Left) == 3);
    CHECK(static_cast<int>(KeyAction::Action1) == 4);
    CHECK(static_cast<int>(KeyAction::Action2) == 5);

    // A KeySet is exactly 6 plain-int scancode slots, one per KeyAction —
    // round-trips through assignment like any POD.
    KeySet ks{};
    ks.scancode[static_cast<int>(KeyAction::Up)] = 200;
    ks.scancode[static_cast<int>(KeyAction::Action1)] = 57;
    CHECK(ks.scancode[static_cast<int>(KeyAction::Up)] == 200);
    CHECK(ks.scancode[static_cast<int>(KeyAction::Action1)] == 57);
}

// The §1 kill tally (results.hpp's tally_kills): counts PlayerDied.data
// (event.hpp's killer-index convention) into a per-player counter, excluding
// both "no killer" (-1) and self-kills (data == player) — see results.hpp's
// doc comment for why self-kills are excluded ("our semantics", §1 does not
// pin this).
TEST_CASE("tally_kills counts an owner kill, skips a self-kill and a no-killer death") {
    std::array<int, kMaxPlayers> kills{};
    std::vector<Event> events;
    events.push_back({Event::Type::PlayerDied, /*player=*/1, 0, 0, /*data=killer*/ 0});
    events.push_back({Event::Type::PlayerDied, /*player=*/2, 0, 0, /*data=killer*/ 2});   // self
    events.push_back({Event::Type::PlayerDied, /*player=*/3, 0, 0, /*data=killer*/ -1});  // crush
    events.push_back({Event::Type::Explosion, 0, 0, 0, 0});  // unrelated event type, ignored
    tally_kills(events, kills);
    CHECK(kills[0] == 1);
    for (int i = 1; i < kMaxPlayers; ++i) CHECK(kills[i] == 0);

    // Tallying is additive across ticks/calls — a second bomb-owner kill by
    // the same player accumulates (cumulative for the whole match, §1's
    // "carried across rounds within one match").
    std::vector<Event> more{{Event::Type::PlayerDied, 4, 0, 0, 0}};
    tally_kills(more, kills);
    CHECK(kills[0] == 2);
}

// §1's unique-leader tie-break (leader count exactly 1): "the clinch instead
// compares the highest round-kill
// total against the target, breaking ties by requiring a single unique
// leader". win_by_kills_clinch (results.hpp) mirrors that predicate exactly.
TEST_CASE("win_by_kills_clinch requires reaching the target AND a unique leader") {
    std::array<bool, kMaxPlayers> present{};
    present[0] = present[1] = true;

    std::array<int, kMaxPlayers> kills{};
    kills[0] = 5;
    kills[1] = 2;
    CHECK(win_by_kills_clinch(kills, present, /*target=*/5) == 0);  // unique leader, at target

    kills[0] = 4;  // below target: no clinch yet even with a unique leader
    CHECK(win_by_kills_clinch(kills, present, /*target=*/5) == -1);

    kills[0] = 5;
    kills[1] = 5;  // tied at the target: leader count != 1, no clinch
    CHECK(win_by_kills_clinch(kills, present, /*target=*/5) == -1);

    // An absent slot's always-0 kill count must not fake a tie against a
    // real player who also happens to have 0 kills.
    std::array<int, kMaxPlayers> zero_kills{};
    present[1] = false;  // only player 0 is active now
    CHECK(win_by_kills_clinch(zero_kills, present, /*target=*/0) == 0);
}

// docs/re/goldman-roulette.md §2 (pseudo.c 30004-30022): dword_46492C ==
// v73, the MATCH-CLINCH winner, gated on goldman being on; -1 otherwise. In
// team mode the stored value is the clinching player's raw team id.
TEST_CASE("assign_gold_player mirrors dword_46492C's RESULTS-tier write") {
    std::array<int, kMaxPlayers> team_of{};
    team_of[0] = 0;
    team_of[1] = 1;
    team_of[2] = 1;

    // goldman off: always -1, regardless of who clinched or team mode.
    CHECK(assign_gold_player(/*goldman_on=*/false, /*team_mode=*/false, /*clinched=*/2, team_of) ==
          -1);
    CHECK(assign_gold_player(false, true, 2, team_of) == -1);

    // goldman on, solo: the clinching player's own index passes through
    // untouched (v73 IS a player index outside team mode).
    CHECK(assign_gold_player(true, false, 2, team_of) == 2);
    // No clinch yet this RESULTS pass (clinch index -1): no pending gold player.
    CHECK(assign_gold_player(true, false, -1, team_of) == -1);

    // goldman on, team mode: dword_46492C stores the clinching player's team
    // (sub_4223E7's raw team byte; our port keeps it 0/1, not the original's
    // 0/2 internal encoding — doc §2).
    CHECK(assign_gold_player(true, true, 2, team_of) == 1);  // player 2 -> team 1
    CHECK(assign_gold_player(true, true, 0, team_of) == 0);  // player 0 -> team 0
    // No clinch yet: -1 passes straight through, never indexed into team_of.
    CHECK(assign_gold_player(true, true, -1, team_of) == -1);
}

// The gold TWINKLE seeding gate, end to end through the same chain the app
// wires: a team-mode clinch -> assign_gold_player stores the RAW 0/1 team id ->
// the renderer's per-slot gate compares against the hashed Player::team, which
// apply_roster (match_runner.cpp) builds as `setup_team[i] + 1` (0 is reserved
// for "no team / solo side", match_factory.hpp). sub_420F07's own team branch
// (pseudo.c 23662-23669) re-encodes the slot's +84 byte into dword_46492C's
// doubled representation before comparing — SAME encoding on both sides — so
// the port must apply its own +1 shift here. Unshifted, a raw-team-1 clinch
// twinkled the LOSING team and a raw-team-0 clinch twinkled nobody (the
// 2026-08-01 team-play sparkle bug).
TEST_CASE("the gold twinkle lands on the clinching team's members, never the beaten team's") {
    std::array<int, kMaxPlayers> setup_team{};  // raw ids: slots 0/1 -> 0, 2/3 -> 1
    setup_team[2] = setup_team[3] = 1;
    std::array<int, kMaxPlayers> sim_team{};  // the +1 shift apply_roster performs
    for (int i = 0; i < 4; ++i) sim_team[i] = setup_team[i] + 1;

    // Team raw 1 clinches through slot 2 (winning_side resolves to the lowest
    // alive member): the pending gold id is the raw team, 1.
    const int gold = assign_gold_player(/*goldman_on=*/true, /*team_mode=*/true,
                                        /*clinched=*/2, setup_team);
    REQUIRE(gold == 1);
    // Every member of the CLINCHING team twinkles; no member of the beaten team
    // does (sub_420F07 seeds sub_420D4E for each matching alive slot).
    for (int i = 0; i < 4; ++i)
        CHECK(gold_twinkle_matches(/*team_mode=*/true, gold, i, sim_team[i]) ==
              (setup_team[i] == 1));

    // The mirror clinch: team raw 0 wins -> ITS members twinkle. Under the
    // unshifted comparison this selected NOBODY (no Player::team is 0 in team
    // mode) while the raw-1 case above selected exactly the losers.
    const int gold0 = assign_gold_player(true, true, /*clinched=*/0, setup_team);
    REQUIRE(gold0 == 0);
    for (int i = 0; i < 4; ++i)
        CHECK(gold_twinkle_matches(true, gold0, i, sim_team[i]) == (setup_team[i] == 0));
}

TEST_CASE("gold_twinkle_matches: solo compares the slot index; no pending gold seeds nobody") {
    // Solo (sub_420F07's else branch): dword_46492C == i, the slot index; the
    // slot's team value is irrelevant.
    CHECK(gold_twinkle_matches(/*team_mode=*/false, /*gold=*/2, /*slot=*/2, /*sim_team=*/0));
    CHECK_FALSE(gold_twinkle_matches(false, 2, 1, 0));
    // -1 = no pending gold player: nobody matches in either mode. (The original
    // never reaches the compare with -1 — the seeding is gated earlier — but the
    // port pushes the pair to the renderer every frame, so the predicate itself
    // must refuse.)
    for (int i = 0; i < 4; ++i) {
        CHECK_FALSE(gold_twinkle_matches(false, -1, i, i + 1));
        CHECK_FALSE(gold_twinkle_matches(true, -1, i, i + 1));
    }
}

// docs/re/frontend-flow.md "VICTORY" §3 (aTeamU vs aVictoryU): the
// match-clinch outcome screen is TEAM<0/1>.PCX under Team Play, else
// VICTORY<player>.PCX — a real gap fixed 2026-07-09 (the port previously
// always resolved VICTORY<player>, even under Team Play, because this branch
// predates Player::team landing).
TEST_CASE("victory_background_name: solo names the winning player") {
    CHECK(victory_background_name(/*team_mode=*/false, /*player=*/0, /*team=*/0) == "VICTORY0");
    CHECK(victory_background_name(false, 7, 1) == "VICTORY7");
    // The team id is ignored entirely outside team mode.
    CHECK(victory_background_name(false, 3, 99) == "VICTORY3");
}

TEST_CASE("victory_background_name: team mode names the clinching TEAM, not the player") {
    CHECK(victory_background_name(/*team_mode=*/true, /*player=*/2, /*team=*/1) == "TEAM1");
    CHECK(victory_background_name(true, 5, 0) == "TEAM0");
    // The player index is ignored entirely once team mode picks the branch —
    // only the clinching player's raw setup-screen team id matters.
    CHECK(victory_background_name(true, 9, 0) == "TEAM0");
}

// docs/re/campaign.md "Rover/ghost/AI roster — CORRECTED": sub_40151B's
// per-stage starter seeds the AI count via sub_422928 (rand()%10 + retry-
// on-occupied), NOT a sequential fill from slot 0.
TEST_CASE("seed_campaign_ai_slots claims exactly ai_count DISTINCT slots") {
    std::uint32_t lcg = 0x1234u;
    auto slots = seed_campaign_ai_slots(lcg, 4);
    CHECK(slots.size() == 4);
    // Distinct: no duplicate slot claimed twice (sub_422928 re-rolls on hit).
    std::array<bool, kMaxPlayers> seen{};
    for (int s : slots) {
        REQUIRE(s >= 0);
        REQUIRE(s < kMaxPlayers);
        CHECK_FALSE(seen[static_cast<std::size_t>(s)]);
        seen[static_cast<std::size_t>(s)] = true;
    }
}

TEST_CASE("seed_campaign_ai_slots clamps an out-of-range count to [0, kMaxPlayers]") {
    std::uint32_t lcg = 1;
    CHECK(seed_campaign_ai_slots(lcg, 0).empty());
    CHECK(seed_campaign_ai_slots(lcg, -3).empty());
    // A count above the roster size still only claims all 10 slots, not more.
    CHECK(seed_campaign_ai_slots(lcg, 999).size() == static_cast<std::size_t>(kMaxPlayers));
}

TEST_CASE(
    "seed_campaign_ai_slots is deterministic for a fixed lcg seed/count (presentation RNG, not "
    "State::rng)") {
    // Two INDEPENDENT lcg objects, deliberately (the test_match.cpp:174 shape):
    // equality across fresh state is what an implementation reaching for global
    // entropy (::rand, a time seed) fails. On its own, though, this whole suite
    // was satisfied by a sequential fill of the free slots — the exact shape
    // docs/re/campaign.md's CORRECTION refutes — so the two assertions after it
    // pin that the lcg is really the source: the call consumes it, and a
    // different seed produces a different claim set.
    std::uint32_t lcg_a = 42;
    std::uint32_t lcg_b = 42;
    const auto slots_a = seed_campaign_ai_slots(lcg_a, 5);
    CHECK(slots_a == seed_campaign_ai_slots(lcg_b, 5));
    CHECK(lcg_a != 42u);  // the roll stream was consumed, not ignored
    bool varies = false;
    for (std::uint32_t seed : {7u, 99u, 0xBEEFu, 0x5EED5u}) {
        std::uint32_t lcg = seed;
        if (seed_campaign_ai_slots(lcg, 5) != slots_a) varies = true;
    }
    CHECK(varies);  // rand()%10 placement: the seed picks the slots
}

// sub_422928 @0x422977 only claims a slot whose type byte currently reads 0, and
// its caller sub_40151B never clears one — so a campaign STAGE ADVANCE adds AI to
// the standing roster instead of replacing it. This is what keeps the human alive
// across a stage transition: the port used to blank all ten slots first, which
// deleted the player on every advance and left an AI-only roster the round pacing
// then read as "no human survivor" and replayed forever.
TEST_CASE("seed_campaign_ai_slots never claims an occupied slot") {
    std::uint32_t lcg = 0x5EEDu;
    std::array<bool, kMaxPlayers> occupied{};
    occupied[3] = true;  // the human's slot, set on the PLAYER INPUT screen
    occupied[7] = true;  // an AI a previous stage already seeded
    auto slots = seed_campaign_ai_slots(lcg, 4, occupied);
    CHECK(slots.size() == 4);
    for (int s : slots) {
        CHECK(s != 3);
        CHECK(s != 7);
    }
}

TEST_CASE("seed_campaign_ai_slots clamps to the FREE slots, and a full roster seeds nothing") {
    std::uint32_t lcg = 0x5EEDu;
    std::array<bool, kMaxPlayers> occupied{};
    for (int i = 0; i < kMaxPlayers - 2; ++i) occupied[static_cast<std::size_t>(i)] = true;
    CHECK(seed_campaign_ai_slots(lcg, 9, occupied).size() == 2);  // only two seats left
    occupied.fill(true);
    CHECK(seed_campaign_ai_slots(lcg, 3, occupied).empty());
}

// docs/re/campaign.md "Round pacing" clauses 4-5 (sub_4016DA, pseudo.c
// 4634-4648): the scan over slots 0..9 returns early once sub_421DD2 reports a
// slot type that is neither 0 nor 1 and sub_4228C4 reports that slot alive;
// falling through the whole loop instead -> replay the stage.
TEST_CASE("campaign_round_needs_replay: an all-COMPUTER roster replays even with a live side") {
    std::array<bool, kMaxPlayers> present{};
    std::array<bool, kMaxPlayers> alive{};
    std::array<int, kMaxPlayers> slot_type{};
    present[0] = alive[0] = true;
    slot_type[0] = 1;  // COMPUTER — the sole survivor, but doesn't save the round
    present[1] = alive[1] = true;
    slot_type[1] = 1;  // COMPUTER, also alive
    CHECK(campaign_round_needs_replay(present, alive, slot_type) == true);
}

TEST_CASE("campaign_round_needs_replay: a single live human/joystick slot blocks the replay") {
    std::array<bool, kMaxPlayers> present{};
    std::array<bool, kMaxPlayers> alive{};
    std::array<int, kMaxPlayers> slot_type{};
    present[0] = alive[0] = true;
    slot_type[0] = 1;  // COMPUTER, alive
    present[3] = alive[3] = true;
    slot_type[3] = 2;  // human, alive — bails the early-out loop
    CHECK(campaign_round_needs_replay(present, alive, slot_type) == false);
}

TEST_CASE("campaign_round_needs_replay: a DEAD human slot does not block the replay") {
    std::array<bool, kMaxPlayers> present{};
    std::array<bool, kMaxPlayers> alive{};
    std::array<int, kMaxPlayers> slot_type{};
    present[0] = true;
    alive[0] = false;  // human present but dead -> sub_4228C4's liveness check fails
    slot_type[0] = 2;
    CHECK(campaign_round_needs_replay(present, alive, slot_type) == true);
}

TEST_CASE("campaign_round_needs_replay: an absent (not present) slot does not block the replay") {
    std::array<bool, kMaxPlayers> present{};
    std::array<bool, kMaxPlayers> alive{};
    std::array<int, kMaxPlayers> slot_type{};
    alive[0] = true;  // alive but never present — an empty/never-seeded slot
    slot_type[0] = 2;
    CHECK(campaign_round_needs_replay(present, alive, slot_type) == true);
}

TEST_CASE("campaign_round_needs_replay: a fully empty roster replays (vacuous fall-through)") {
    std::array<bool, kMaxPlayers> present{};
    std::array<bool, kMaxPlayers> alive{};
    std::array<int, kMaxPlayers> slot_type{};
    CHECK(campaign_round_needs_replay(present, alive, slot_type) == true);
}

// docs/re/in-match-shell.md §3 (sub_4105D2): MM:SS via MESSAGES.TXT id 281 =
// "%u:%02u", fed the whole seconds remaining divided by 60 and modulo 60.
TEST_CASE("format_clock splits whole seconds into MM:SS via the 281 format") {
    CHECK(format_clock("%u:%02u", 0) == "0:00");
    CHECK(format_clock("%u:%02u", 5) == "0:05");
    CHECK(format_clock("%u:%02u", 59) == "0:59");
    CHECK(format_clock("%u:%02u", 60) == "1:00");
    CHECK(format_clock("%u:%02u", 150) == "2:30");
    // A getstring(281) fallback still substitutes correctly through leading/
    // trailing text a modified MESSAGES.TXT entry might carry.
    CHECK(format_clock("Time: %u:%02u left", 65) == "Time: 1:05 left");
    // Negative input (should never happen — ticks_left floors at 0) clamps
    // rather than producing a negative/garbage string.
    CHECK(format_clock("%u:%02u", -5) == "0:00");
}

// docs/re/in-match-shell.md §3 point 4: the ink colour changes at <=30s
// remaining — a separate, permanent swap from the ~60s "hurry" flash.
TEST_CASE("clock_warning fires at the confirmed <=30s threshold") {
    CHECK(kClockWarningSeconds == 30);
    CHECK_FALSE(clock_warning(31));
    CHECK(clock_warning(30));
    CHECK(clock_warning(1));
    CHECK(clock_warning(0));
}

// docs/re/frontend-flow.md "Attract mode" (sub_410F81's attract branch,
// pseudo.c 15125-15143): `rand()%10 + 1`, clamped to a MINIMUM of 3 —
// i.e. the roll only ever widens the floor, never narrows the ceiling.
TEST_CASE("attract_computer_count rolls 1..10 clamped to a floor of 3") {
    // roll % 10 == 0 -> n = 0 + 1 = 1, clamped up to 3.
    CHECK(attract_computer_count(0) == 3);
    CHECK(attract_computer_count(10) == 3);  // 10 % 10 == 0 -> same as roll 0
    // roll % 10 == 1 -> n = 2, still clamped to 3.
    CHECK(attract_computer_count(1) == 3);
    // roll % 10 == 2 -> n = 3, right at the floor already (no clamp needed).
    CHECK(attract_computer_count(2) == 3);
    // roll % 10 == 3 -> n = 4, above the floor, passes through untouched.
    CHECK(attract_computer_count(3) == 4);
    // roll % 10 == 9 -> n = 10, the maximum roster size.
    CHECK(attract_computer_count(9) == 10);
    CHECK(attract_computer_count(19) == 10);  // 19 % 10 == 9

    // Every possible roll lands in the documented [3, 10] range.
    for (unsigned r = 0; r < 200; ++r) {
        int n = attract_computer_count(r);
        CHECK(n >= 3);
        CHECK(n <= 10);
    }
}

// fill_attract_roster (doc "Attract mode" point 1): every slot OFF first,
// then exactly `computer_count` of them (the FIRST N, slot order) flipped to
// COMPUTER; team is zeroed everywhere ("forces team play off").
TEST_CASE("fill_attract_roster sets the first N slots COMPUTER, the rest OFF, no team") {
    std::array<int, kMaxPlayers> type{}, sub{}, team{};
    // Poison the arrays first so the helper's own reset is what's tested,
    // not a lucky zero-initialized default.
    type.fill(static_cast<int>(SlotInputType::Keyboard));
    sub.fill(1);
    team.fill(1);

    fill_attract_roster(4, type, sub, team);
    for (int i = 0; i < kMaxPlayers; ++i) {
        if (i < 4) {
            CHECK(type[i] == static_cast<int>(SlotInputType::Computer));
        } else {
            CHECK(type[i] == static_cast<int>(SlotInputType::Off));
        }
        CHECK(sub[i] == 0);
        CHECK(team[i] == 0);  // team play forced off for every slot
    }
}

TEST_CASE("fill_attract_roster handles the documented extremes: 3 and 10") {
    std::array<int, kMaxPlayers> type{}, sub{}, team{};

    fill_attract_roster(3, type, sub, team);
    int on = 0;
    for (int i = 0; i < kMaxPlayers; ++i)
        if (type[i] == static_cast<int>(SlotInputType::Computer)) ++on;
    CHECK(on == 3);

    fill_attract_roster(10, type, sub, team);
    for (int i = 0; i < kMaxPlayers; ++i)
        CHECK(type[i] == static_cast<int>(SlotInputType::Computer));
}

// attract_stage_pick (doc "Attract mode": `rand() % getvalue(35)` DIRECTLY,
// bypassing the VALUELST 1150-1160 random-level enable-flag rotation a
// normal RANDOM-level pick honours).
TEST_CASE("attract_stage_pick wraps into [0, level_count) regardless of enable flags") {
    CHECK(attract_stage_pick(0, 11) == 0);
    CHECK(attract_stage_pick(10, 11) == 10);
    CHECK(attract_stage_pick(11, 11) == 0);  // wraps
    CHECK(attract_stage_pick(24, 11) == 2);  // 24 % 11 == 2

    // Every roll stays in range for a variety of level counts, including the
    // degenerate "stripped VALUELST" case of a count < 1 (clamped to 1).
    for (int count : {0, 1, 5, 11}) {
        int effective_count = count < 1 ? 1 : count;
        for (unsigned r = 0; r < 50; ++r) {
            int stage = attract_stage_pick(r, count);
            CHECK(stage >= 0);
            CHECK(stage < effective_count);
        }
    }
}

// NOT COVERED HERE: the attract save/restore roundtrip (sub_4224E2/sub_422552,
// docs/re/frontend-flow.md "Attract mode" point 3). Its snapshot type,
// AttractSaved, lives in libs/frontend's menu_state.hpp, which the headless
// preset does not build — so a case here can only declare a local stand-in,
// assign to it and assign back, which asserts std::array's copy semantics and
// nothing about the port. Pinning it needs AttractSaved (and the save/restore
// pair over it) in bomber::game_util, where a headless suite can reach them.

// The DOS scancode display rule (docs/re/results-and-options.md §2,
// sub_407B9D pseudo.c 8743-8744 + the off_45B914 table read straight from
// BM95.EXE's data): name = kDosKeyNames[code & 0x7F], and NO name at all
// (nullptr) once the masked code falls outside the 0x59-entry table. The
// extended arrows alias their numpad names through the mask.
TEST_CASE("dos_scancode_name follows the original's &0x7F / <0x59 rule") {
    using bomber::game::dos_scancode_name;
    using bomber::game::kDosKeyNameCount;

    // sub_40614A's default set 0: arrows (E0-extended, 0x80|code) + Space +
    // Enter — displayed with the numpad-aliased names.
    CHECK(std::string(dos_scancode_name(200)) == "(8)Up");
    CHECK(std::string(dos_scancode_name(205)) == "(6)Right");
    CHECK(std::string(dos_scancode_name(208)) == "(2)Down");
    CHECK(std::string(dos_scancode_name(203)) == "(4)Left");
    CHECK(std::string(dos_scancode_name(57)) == "Space");
    CHECK(std::string(dos_scancode_name(28)) == "Enter");
    // Default set 1: R/G/F/D + S + A.
    CHECK(std::string(dos_scancode_name(19)) == "R");
    CHECK(std::string(dos_scancode_name(34)) == "G");
    CHECK(std::string(dos_scancode_name(33)) == "F");
    CHECK(std::string(dos_scancode_name(32)) == "D");
    CHECK(std::string(dos_scancode_name(31)) == "S");
    CHECK(std::string(dos_scancode_name(30)) == "A");
    // Unbound (0) still has a table entry — the empty string, drawn as
    // "Key: ''" — while a masked code past the table draws nothing.
    CHECK(std::string(dos_scancode_name(0)).empty());
    CHECK(dos_scancode_name(kDosKeyNameCount) == nullptr);      // 0x59: first gap
    CHECK(dos_scancode_name(0x80 | 0x59) == nullptr);           // extended fold of the gap
    CHECK(std::string(dos_scancode_name(0x80 | 28)) == "Enter");  // KP Enter aliases Enter
}

// ---------------------------------------------------------------------------
// The best-of-N MATCH loop (sub_42A3F6's round-end shell, docs/re/
// in-match-shell.md): a round that ends without a clinch must start ANOTHER
// round, not return to the menu. These cases pin the exact decision run_app's
// Results handler makes — tally the round win, then ask match_clinch() whether
// the MATCH is over — because a "2-round match quits after round 1" report is
// precisely this predicate answering wrong.

namespace {

// The round-end bookkeeping the RESULTS tier keeps, bundled so a case reads as
// the sequence of rounds it is describing instead of as its own setup.
struct MatchTally {
    bomber::sim::State state;
    std::array<int, kMaxPlayers> win_count{};
    std::array<int, kMaxPlayers> kill_count{};
    std::array<int, kMaxPlayers> setup_team{};
    int win_target = 2;
    bool team_play = false;

    // A free-for-all slot: sim team 0 is "no team", so nothing else is set.
    void seat(int slot) { state.players[slot].present = true; }

    // MatchRunner's `cfg.team[i] = setup_team[i] + 1` — the sim reserves team 0
    // for "no team", so the frontend's raw +84 byte shifts up by one.
    void seat_on_team(int slot, int team) {
        seat(slot);
        setup_team[slot] = team;
        state.players[slot].team = static_cast<std::uint8_t>(team + 1);
    }

    // One round decided by its sole survivor, tallied the way the handler does.
    // Returns round_winner()'s verdict so a case can assert it.
    int survive(int survivor) {
        for (auto& p : state.players) p.alive = false;
        state.players[survivor].alive = true;
        const int winner = bomber::game::round_winner(state);
        bomber::game::award_round_win(win_count, winner, team_play, state, setup_team);
        return winner;
    }

    int clinch() const {
        return bomber::game::match_clinch(state, team_play, setup_team, /*win_by_kills=*/false,
                                          kill_count, win_count, win_target);
    }
};

}  // namespace

TEST_CASE("best-of-N: an undecided round continues the match; the target ends it") {
    MatchTally m;
    m.seat(0);
    m.seat(1);

    // Round 1: player 0 survives. Tally the win — the match is NOT decided, so
    // the flow feeds RoundContinue and next() routes Results -> Match.
    CHECK(m.survive(0) == 0);
    CHECK(m.clinch() == -1);
    CHECK(next(AppState::Results, AppInput::RoundContinue) == AppState::Match);

    // Round 2: player 0 survives again and reaches the target — NOW the match is
    // over, VICTORY shows, and a plain Advance routes Results -> Menu.
    CHECK(m.survive(0) == 0);
    CHECK(m.clinch() == 0);
    CHECK(next(AppState::Results, AppInput::Advance) == AppState::Menu);

    // A DRAW scores nobody, so an untallied round still continues the match. It
    // is the ABSENCE OF A SURVIVOR that draws, not the clock: match_outcome.hpp
    // records that testing the clock first is what used to steal a win earned in
    // the round's last second.
    m.win_count.fill(0);
    for (auto& p : m.state.players) p.alive = false;  // mutual wipe-out
    m.state.ticks_left = 0;
    CHECK(bomber::game::round_winner(m.state) == -1);
    CHECK(m.clinch() == -1);
}

TEST_CASE("best-of-N: the win target comes from VALUELST 310, then options.ini, then 2") {
    using bomber::game::reset_match_scores;
    MatchTally m;
    m.win_count[3] = 7;  // stale counters from the previous match, to be cleared
    int win_target = 0;

    // getvalue(310) "how many wins to win a match?", which this install ships as
    // 2 — so the shipped default match is best-of-2 and CANNOT end after one
    // round. It wins over options.ini's num_to_win_match= when both are present.
    bomber::assets::res::ValueList values;
    values.values[310] = 2;
    reset_match_scores(m.win_count, m.kill_count, win_target, values, 5);
    CHECK(win_target == 2);
    CHECK(m.win_count[3] == 0);

    // With no VALUELST entry the target falls back to num_to_win_match=, and to
    // 2 when that is absent too; a hand-edited 0 is clamped up to 1 rather than
    // ending the match before it starts.
    bomber::assets::res::ValueList empty;
    reset_match_scores(m.win_count, m.kill_count, win_target, empty, 5);
    CHECK(win_target == 5);
    reset_match_scores(m.win_count, m.kill_count, win_target, empty, std::nullopt);
    CHECK(win_target == 2);
    reset_match_scores(m.win_count, m.kill_count, win_target, empty, 0);
    CHECK(win_target == 1);
}

// The TEAM half of the same round-end tail: sub_421B56 @ 0x421B56 credits the
// surviving slot and then, under Team Play, COPIES that counter into every other
// PRESENT slot on the winner's team. Without the copy the tally splits across a
// team's members — the RESULTS "Team N score" row (which reads ONE member, like
// the original's clinch) shows a team that keeps winning stuck near zero, and
// the clinch lands rounds late.
TEST_CASE("team play: a round win is mirrored to the whole team, so the clinch is on time") {
    MatchTally m;  // 2v2: slots 0+1 are team 0, slots 2+3 are team 1
    m.team_play = true;
    for (int i = 0; i < 4; ++i) m.seat_on_team(i, i / 2);

    // Round 1: team 0 wins with slot 1 — slot 0 died, so an unmirrored tally
    // would leave it on 0 forever if it keeps dying.
    CHECK(m.survive(1) == 1);
    CHECK(m.win_count[0] == 1);  // the DEAD teammate is credited too (+0x10 is
    CHECK(m.win_count[1] == 1);  // "present", not "alive")
    CHECK(m.win_count[2] == 0);
    CHECK(m.win_count[3] == 0);
    CHECK(m.clinch() == -1);  // one win of two: not decided

    // Round 2: team 0 wins again, this time with the OTHER member. The mirror is
    // a COPY, not a second increment, so the team reads 2 — not 1 and 1 — and
    // the match clinches on round 2, exactly at win_target. Pre-fix this said
    // 1/1 and the match dragged on to a third round.
    CHECK(m.survive(0) == 0);
    CHECK(m.win_count[0] == 2);
    CHECK(m.win_count[1] == 2);
    CHECK(m.clinch() == 0);      // the first present member of the clinching team
    CHECK(m.win_count[2] == 0);  // the losing team picks up nothing from the mirror
    CHECK(m.win_count[3] == 0);
}

TEST_CASE("team play OFF: the same 2v2 round credits only the survivor") {
    // The original gates the copy loop on dword_464964, so with Team Play off a
    // teammate's win is not shared however the roster is arranged.
    MatchTally m;
    for (int i = 0; i < 4; ++i) m.seat_on_team(i, i / 2);
    CHECK(m.survive(1) == 1);
    CHECK(m.win_count[0] == 0);
    CHECK(m.win_count[1] == 1);
}
