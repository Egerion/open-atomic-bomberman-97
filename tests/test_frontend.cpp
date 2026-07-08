// Locks the SDL-free front-end screen/state-machine core (app_flow.hpp): the
// pure next(state, input) transition that mirrors the original's boot path
// (sub_42B060 logos+title -> sub_42B9CE menu loop, docs/re/frontend-flow.md).
// This is presentation-only glue — it never touches the sim, so there is no
// determinism/golden impact; the doctest just pins the flow graph and its
// skip/timeout/quit/menu-hub edges. See docs/adr/0004-frontend-screen-flow.md.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "bomber/game/app_flow.hpp"
#include "bomber/game/input.hpp"

using bomber::game::AppInput;
using bomber::game::AppState;
using bomber::game::cycle_slot_input_type;
using bomber::game::is_terminal;
using bomber::game::KeyAction;
using bomber::game::kKeyActionCount;
using bomber::game::kKeyboardSets;
using bomber::game::KeySet;
using bomber::game::next;
using bomber::game::SlotInputType;

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
    CHECK(next(AppState::Menu, AppInput::OpenCredits) == AppState::Credits);

    // Every leaf returns to the menu on accept AND on Back — a dismissable
    // screen has nowhere else to go (sub_42B9CE re-enters its loop after each).
    for (AppState leaf : {AppState::Options, AppState::Controllers, AppState::Network,
                          AppState::Credits, AppState::Results}) {
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
        AppState::Boot,    AppState::Logo,        AppState::Title,   AppState::Menu,
        AppState::Match,   AppState::Results,     AppState::Options, AppState::Controllers,
        AppState::Network, AppState::Credits,     AppState::Quit,
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
