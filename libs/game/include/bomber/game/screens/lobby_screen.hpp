#pragma once

#include <cstdint>
#include <string>

#include "bomber/game/screen_context.hpp"

// The ONLINE LOBBY front-end (ADR-0011 Phase 1d) — the SDL3 screens over
// bomber::net's finished LobbyFlow state machine. Three screens, all composed
// from the ALREADY-REVERSE-ENGINEERED chrome primitives in dialog_chrome.hpp
// (no new UI chrome is invented here — the project's standing rule):
//
//   run_menu()       the NETWORK GAME entry list — the generic bevel list
//                    dialog sub_42DBCC (draw_list_dialog) over the MAINMENU
//                    backdrop, exactly like the *.BM help browser's picker.
//   run_code_entry() the JOIN BY CODE modal — the sub_42E938 text-entry family
//                    (draw_text_entry_dialog) at the SAME y=180 anchor the
//                    editor's save-as prompt and NetplayConnectScreen::run_join
//                    use. Filters to Crockford base-32, uppercases as typed.
//   run_online()     the WAITING ROOM — the same list dialog, its pinned centred
//                    title strip carrying the lobby CODE (the string the host
//                    reads out) and one item row per roster seat; the transient
//                    phases (connecting / punching / failed) reuse the
//                    sub_414340 acknowledge modal the connect screens already
//                    show. Pumps LobbyFlow::step() once per frame.
//
// Structured like netplay_connect_screen.hpp — a ScreenContext by value, a
// platform::FrameClock-paced SDL loop per screen, Esc cancels back to the
// caller, and an SDL_QUIT is surfaced (never swallowed) so GameApp can
// propagate a hard quit. GameApp owns the transport and runs the match.
//
// NOT built here: the PUBLIC LOBBY BROWSER (ADR-0011 Phase 3). The server and
// LobbyClient already speak it (encode_list_public / LobbyMsgType::PublicList);
// the GUI seam is LobbyMenuChoice::BrowsePublic below plus one kRows entry.

namespace bomber::net {
class UdpTransport;  // borrowed by reference; the .cpp includes the real header
}  // namespace bomber::net

namespace bomber::game {

// The NETWORK GAME menu's outcome. Sized like AppState (app_flow.hpp).
enum class LobbyMenuChoice : std::uint8_t {
    Cancel,        // Esc / Done -> back to the main menu
    WindowClosed,  // SDL_QUIT -> GameApp propagates AppInput::Quit
    HostOnline,    // HOST PRIVATE GAME: create a lobby, show its code
    JoinOnline,    // JOIN BY CODE: the 6-char code modal, then the waiting room
    HostDirect,    // HOST LAN GAME: the ADR-0010 direct-UDP host (no server)
    JoinDirect,    // JOIN BY IP ADDRESS: the ADR-0010 direct-UDP join
    // BrowsePublic — Phase 3. See the file header.
};

// The waiting room's outcome. `ready` means LobbyFlow reached Phase::Ready, so
// the transport is punched-and-connected and the two fields below are the
// SERVER's authoritative match parameters (design §1.6) — GameApp feeds them
// straight to run_netplay_match_seats() instead of deriving seats from a role.
struct LobbyRoomResult {
    bool ready = false;
    bool window_closed = false;
    std::uint32_t seed = 0;
    std::uint16_t local_seats_mask = 0;
};

class LobbyScreen {
public:
    explicit LobbyScreen(ScreenContext ctx) : ctx_(ctx) {}

    // The NETWORK GAME list. `online_available` is false on a lobby-off build
    // (no BOMBER_HAS_LOBBY), which lists only the two direct/LAN rows.
    LobbyMenuChoice run_menu(bool online_available);

    // The JOIN BY CODE modal. Returns true with `code` set to the 6 accepted
    // characters; false on Esc (or on a window close, which also sets
    // `window_closed` so the caller can propagate a hard quit).
    bool run_code_entry(std::string& code, bool& window_closed);

    // How to reach the matchmaker. GameApp resolves these (CLI flag -> env var
    // -> compile-time placeholder) and hands the result down.
    //
    // This header needs NO BOMBER_HAS_LOBBY guard: it names no net type except
    // the forward-declared UdpTransport, so it parses identically with and
    // without the lobby. Only run_online's DEFINITION is guarded (lobby_screen
    // .cpp) — guarding the declaration would give LobbyScreen two different
    // definitions across translation units, since BOMBER_HAS_LOBBY is visible
    // inside bomber_game_core but not to its consumers (bomber::net is a PRIVATE
    // dependency). On a lobby-off build run_menu simply never offers the online
    // rows, so nothing references the missing symbol.
    struct OnlineConfig {
        std::string server_url;  // "ws://host:8080/ws"
        std::string stun_host;   // the same host's UDP STUN echo (PROTOCOL.md §2)
        std::uint16_t stun_port = 8081;
        std::string player_name = "PLAYER";
    };

    // Host (or join `code`) and sit in the waiting room until the match starts,
    // the player leaves, or the flow fails. `transport` MUST already be bound:
    // LobbyFlow reuses that one socket for STUN, the punch and the match, so the
    // NAT binding the peers punched is the one gameplay flows through.
    LobbyRoomResult run_online(const OnlineConfig& cfg, net::UdpTransport& transport, bool host,
                               const std::string& code);

private:
    // The shared MAINMENU backdrop every front-end modal sits over (identical to
    // NetplayConnectScreen::draw_backdrop).
    void draw_backdrop();

    ScreenContext ctx_;
};

}  // namespace bomber::game
