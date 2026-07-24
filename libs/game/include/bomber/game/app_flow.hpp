#pragma once

#include <cstdint>

// The front-end screen/state machine — the SDL-free, doctestable core of the
// application flow (docs/adr/0004-frontend-screen-flow.md). It mirrors the
// original's top-level boot path (`sub_42B060` logos+title, then the
// `sub_42B9CE` menu loop, docs/re/frontend-flow.md): a small state enum plus a
// PURE transition function over a tiny event alphabet. No SDL, no globals, no
// wall clock — the SDL screen/audio layer sits on top and merely feeds events
// in. Presentation only: nothing here touches the sim or its RNG, so it has no
// bearing on the determinism contract (ADR-0003).

namespace bomber::game {

// The application-level states. Ordered along the nominal boot path
// (Boot -> Logo -> Title -> Menu -> Match -> Results -> Menu|Match). The menu
// is a HUB: selecting an item routes to one leaf state and every leaf returns
// to the menu. `Quit` is the terminal state; `next()` is a fixed point there.
//
// Match/Results form a best-of-N LOOP, not a single pass: a match is a
// sequence of ROUNDS (each a fresh sim, docs/re/frontend-flow.md "results
// flow"). Results is the three-tier `sub_42A3F6` tail — DRAW (no survivor,
// replay), the RESULTS cumulative tally (a survivor but nobody has reached
// win_target_ yet, replay), or VICTORY<n> (a player reached win_target_,
// return to Menu). Which of those three renders is a presentation-side
// decision (round_winner() + the win tally) fed as the input to next(); the
// graph itself only distinguishes "decided" (Advance/Back -> Menu) from
// "not decided" (RoundContinue -> Match).
//
// The four .BM-backed leaves (Options/Controllers/Network/Credits) mirror the
// deep menu items `sub_42B9CE` dispatches into — `sub_42B0CE`/`sub_42B47D`
// (setup screens), `sub_41302D(aCreditsBm)` (a .BM text viewer). Their screens
// now render the real `.BM` help/credits text (BmScreen, sub_41302D); the
// fully-INTERACTIVE settings/controller-remap widgets remain deferred behind the
// same states (docs/re/frontend-flow.md "The .BM text-screen viewer").
enum class AppState : std::uint8_t {
    Boot,         // pre-first-frame; immediately advances into the flow
    Logo,         // IPLOGO then HSLOGO (skippable / timed)
    Title,        // TITLE.PCX + title sting (wait-for-key or 7 s timeout -> menu)
    Menu,         // navigable main menu (MAINMENU.PCX): the hub
    Match,        // one ROUND of the deterministic sim (best-of-N loop)
    Results,      // DRAW / RESULTS tally / VICTORY end-of-round screen
    Options,      // OPTIONS.BM help viewer (interactive settings UI = deferred)
    Controllers,  // INPUT.BM help viewer; unreachable from any menu row by
                  // design — CONFIRMED negative (docs/re/frontend-flow.md):
                  // the original has no dedicated row/screen for this either,
                  // INPUT.BM is just one topic in the generic *.BM help
                  // browser (row 5/F1) both games already glob and list
    Network,      // NETWORK.BM help viewer (no longer routed from a menu row —
                  // rows 1/2 now START/JOIN a netplay game; kept reachable via
                  // the generic *.BM help browser, and as the flow leaf below)
    NetHost,      // START NET GAME: host a 2-player UDP lockstep match
                  // (bind + seed handshake + run_netplay_match; ADR-0010 §3.3)
    NetJoin,      // JOIN NET GAME: connect to a host (address prompt + handshake)
    Credits,      // CREDITS.BM viewer (text + inline images)
    Quit,         // shut down
};

// The event alphabet the flow reacts to. Deliberately tiny: the SDL layer
// collapses every concrete input into one of these.
//   Advance    — a keypress accept OR a screen's dwell/attract timeout elapsed
//                (the original synthesizes Enter on the getvalue(12) timeout,
//                sub_42A088, so accept and timeout are the same event here).
//   Back       — Escape / cancel.
//   MatchOver  — the running round ended (one player left or time up); always
//                lands on Results, whether or not the MATCH is decided.
//   RoundContinue — leaving Results when the match is NOT yet decided: a draw
//                (replay unconditionally) or a survivor whose cumulative win
//                tally is still below win_target_ (docs/re/frontend-flow.md
//                "results flow" middle tier). Routes Results -> Match for the
//                next round (same roster/settings, sim reconstructed fresh).
//   CampaignContinue — leaving Results when a MATCH just clinched AND
//                campaign mode is active with stages remaining (docs/re/
//                campaign.md "Advances through campaign stages
//                automatically", sub_401312/sub_40133F gated
//                `if (dword_46489C)`): routes Results -> Match exactly like
//                RoundContinue, but the SDL shell has already loaded the
//                NEXT campaign stage's scheme/roster before feeding this
//                event, rather than replaying the same match. A separate
//                event (not RoundContinue) because the graph is presentation-
//                only bookkeeping — RoundContinue's doc contract is
//                specifically "same roster/settings", which campaign
//                stage-advance breaks.
//   StartMatch — the menu's Start/Play item was chosen (Menu -> Match).
//   OpenOptions/OpenControllers/OpenNetwork/OpenCredits — the menu opened a
//                deep leaf; each routes Menu -> the matching leaf state.
//   OpenNetHost/OpenNetJoin — the two netplay menu rows (START/JOIN NET GAME):
//                Menu -> NetHost/NetJoin, the connection screens that run the
//                seed handshake and then a 2-player UDP lockstep match.
//   Quit       — hard quit request (window close or the menu's Quit item).
enum class AppInput : std::uint8_t {
    Advance,
    Back,
    MatchOver,
    RoundContinue,
    CampaignContinue,
    StartMatch,
    OpenOptions,
    OpenControllers,
    OpenNetwork,
    OpenNetHost,
    OpenNetJoin,
    OpenCredits,
    Quit,
};

// The pure transition. Given the current state and one event, returns the next
// state. Total and side-effect-free — this is the whole contract the doctest
// pins. The SDL shell decides WHEN to feed each event (keypress, timeout, match
// end, menu selection); this function decides only WHERE each event leads.
constexpr AppState next(AppState state, AppInput input) {
    // A quit request short-circuits from anywhere.
    if (input == AppInput::Quit) return AppState::Quit;

    switch (state) {
        case AppState::Boot:
            // Boot is a spring: any tick advances it into the logo sequence.
            // Back at the very first frame is treated as "skip to menu" so a
            // -nologo-style fast path has somewhere to land.
            return input == AppInput::Back ? AppState::Menu : AppState::Logo;

        case AppState::Logo:
            // Advance (key or timeout) walks past the logos to the title;
            // Back skips the remaining logos straight to the title, matching
            // the original's "any accept leaves the logo" behaviour.
            return AppState::Title;

        case AppState::Title:
            // From the title, Advance enters the menu. This is the SAME event
            // for a real key accept AND the getvalue(12) = 7 s timeout: the boot
            // path (sub_42B060) is linear — on timeout it synthesizes Enter and
            // returns, so the caller drops into the menu; there is NO attract
            // re-run of the logos/title. Back on the title backs out of the app
            // (nothing sits behind the title).
            return input == AppInput::Back ? AppState::Quit : AppState::Menu;

        case AppState::Menu:
            // The menu is a hub. A concrete selection routes to its leaf; Back
            // quits the app (the top-level menu has nothing behind it). A plain
            // Advance (no selection) is inert — the SDL menu resolves the
            // highlighted item into one of the specific events below.
            switch (input) {
                case AppInput::StartMatch: return AppState::Match;
                case AppInput::OpenOptions: return AppState::Options;
                case AppInput::OpenControllers: return AppState::Controllers;
                case AppInput::OpenNetwork: return AppState::Network;
                case AppInput::OpenNetHost: return AppState::NetHost;
                case AppInput::OpenNetJoin: return AppState::NetJoin;
                case AppInput::OpenCredits: return AppState::Credits;
                case AppInput::Back: return AppState::Quit;
                default: return AppState::Menu;  // Advance/MatchOver: stay put
            }

        case AppState::Match:
            // Only the round ending moves us on; stray Advance/Back inside a
            // running round do not change the app state (the match owns input).
            return input == AppInput::MatchOver ? AppState::Results : AppState::Match;

        case AppState::Results:
            // RoundContinue (draw, or a survivor below win_target_) starts the
            // NEXT round with the same roster/settings; CampaignContinue (a
            // clinched match with campaign stages remaining) starts the NEXT
            // STAGE's match the same way; any other event (a decided
            // non-campaign match's Advance, or Back) returns to the menu. The
            // SDL shell computes which applies (round_winner() + the win
            // tally + campaign_active_) BEFORE feeding this event — the graph
            // itself is data-driven.
            return (input == AppInput::RoundContinue || input == AppInput::CampaignContinue)
                       ? AppState::Match
                       : AppState::Menu;

        // The .BM leaves all return to the menu on any accept or Back — a
        // dismissable text/help screen has nowhere else to go (sub_42B9CE
        // re-enters its loop after each sub-screen returns). The two netplay
        // connection leaves likewise fall back to the menu once the match ends
        // (or the connect was cancelled/timed out).
        case AppState::Options:
        case AppState::Controllers:
        case AppState::Network:
        case AppState::NetHost:
        case AppState::NetJoin:
        case AppState::Credits: return AppState::Menu;

        case AppState::Quit: return AppState::Quit;
    }
    return state;  // unreachable; keeps -Wreturn-type / MSVC happy
}

// True once the flow has reached its terminal state.
constexpr bool is_terminal(AppState state) {
    return state == AppState::Quit;
}

}  // namespace bomber::game
