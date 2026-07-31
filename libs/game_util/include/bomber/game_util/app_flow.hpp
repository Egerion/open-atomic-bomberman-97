#pragma once

#include <cstdint>

// The front-end screen/state machine (docs/adr/0004-frontend-screen-flow.md): a
// small state enum plus a PURE transition function over a tiny event alphabet,
// mirroring the original's top-level boot path (`sub_42B060` logos+title, then
// the `sub_42B9CE` menu loop, docs/re/frontend-flow.md). The SDL layer sits on
// top and only feeds events in. Presentation only — nothing here touches the sim
// or its RNG (ADR-0003).

namespace bomber::game {

// Ordered along the nominal boot path. The menu is a HUB: selecting an item
// routes to one leaf and every leaf returns to it. Match/Results form a
// best-of-N LOOP — Results is the three-tier `sub_42A3F6` tail (DRAW, the
// cumulative RESULTS tally, or VICTORY<n>). WHICH of the three renders is a
// presentation-side decision fed as the input to next(); the graph itself only
// distinguishes "decided" from "not decided".
enum class AppState : std::uint8_t {
    Boot,
    Logo,     // IPLOGO then HSLOGO (skippable / timed)
    Title,    // TITLE.PCX + title sting (wait-for-key or 7 s timeout)
    Menu,     // MAINMENU.PCX: the hub
    Match,    // one ROUND of the deterministic sim
    Results,  // DRAW / RESULTS tally / VICTORY
    Options,  // OPTIONS.BM help viewer (interactive settings UI = deferred)
    // INPUT.BM, unreachable from any menu row BY DESIGN — CONFIRMED negative
    // (docs/re/frontend-flow.md): the original has no dedicated row either,
    // INPUT.BM is one topic in the generic *.BM help browser (row 5/F1).
    Controllers,
    // NETWORK.BM. No longer routed from a menu row — rows 1/2 now START/JOIN a
    // netplay game — but still reachable through that same help browser.
    Network,
    NetHost,  // START NET GAME: bind + seed handshake + match (ADR-0010 §3.3)
    NetJoin,  // JOIN NET GAME: address prompt + handshake
    Credits,  // CREDITS.BM viewer (text + inline images)
    Quit,
};

// Deliberately tiny: the SDL layer collapses every concrete input into one of
// these. Three do not mean the obvious thing:
//   Advance   — a keypress accept OR a dwell/attract timeout. The original
//               synthesizes Enter on the getvalue(12) timeout (sub_42A088), so
//               accept and timeout really are the same event.
//   RoundContinue    — leaving Results with the MATCH undecided: same roster and
//               settings, sim reconstructed fresh.
//   CampaignContinue — leaving Results when a match just clinched AND campaign
//               mode has stages remaining (docs/re/campaign.md, sub_401312/
//               sub_40133F gated on dword_46489C). Routes like RoundContinue,
//               but the shell has already loaded the NEXT stage — a separate
//               event because RoundContinue's contract is specifically "same
//               roster/settings", which a stage advance breaks.
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

namespace detail {

// The menu hub. A plain Advance (no selection) is inert: the SDL menu resolves
// the highlighted item into one of the specific events first.
constexpr AppState menu_next(AppInput input) {
    switch (input) {
        case AppInput::StartMatch: return AppState::Match;
        case AppInput::OpenOptions: return AppState::Options;
        case AppInput::OpenControllers: return AppState::Controllers;
        case AppInput::OpenNetwork: return AppState::Network;
        case AppInput::OpenNetHost: return AppState::NetHost;
        case AppInput::OpenNetJoin: return AppState::NetJoin;
        case AppInput::OpenCredits: return AppState::Credits;
        case AppInput::Back: return AppState::Quit;  // nothing sits behind it
        default: return AppState::Menu;
    }
}

}  // namespace detail

// The pure transition: total and side-effect-free. The SDL shell decides WHEN to
// feed each event; this decides only WHERE each event leads.
constexpr AppState next(AppState state, AppInput input) {
    if (input == AppInput::Quit) return AppState::Quit;

    switch (state) {
        // Back at the very first frame is "skip to menu", so a -nologo-style
        // fast path has somewhere to land.
        case AppState::Boot: return input == AppInput::Back ? AppState::Menu : AppState::Logo;

        // Any accept leaves the logos, matching the original.
        case AppState::Logo: return AppState::Title;

        // The boot path (sub_42B060) is LINEAR: on timeout it synthesizes Enter
        // and returns, so there is no attract re-run of the logos/title.
        case AppState::Title: return input == AppInput::Back ? AppState::Quit : AppState::Menu;

        case AppState::Menu: return detail::menu_next(input);

        // The match owns its own input; only the round ending moves us on.
        case AppState::Match:
            return input == AppInput::MatchOver ? AppState::Results : AppState::Match;

        case AppState::Results:
            return (input == AppInput::RoundContinue || input == AppInput::CampaignContinue)
                       ? AppState::Match
                       : AppState::Menu;

        // A dismissable text/help screen has nowhere else to go (sub_42B9CE
        // re-enters its loop after each sub-screen returns), and the two netplay
        // leaves likewise fall back once the match ends or the connect failed.
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

constexpr bool is_terminal(AppState state) {
    return state == AppState::Quit;
}

}  // namespace bomber::game
