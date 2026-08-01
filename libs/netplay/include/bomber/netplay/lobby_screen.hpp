#pragma once

#include <cstdint>
#include <string>

#include "bomber/netui/chat_overlay.hpp"
#include "bomber/ui/screen_context.hpp"

// The ONLINE LOBBY front-end (ADR-0011 Phase 1d + the Phase 3 browser) — the
// SDL3 screens over bomber::net's finished LobbyFlow state machine. Every pixel
// is composed from the ALREADY-REVERSE-ENGINEERED chrome primitives in
// dialog_chrome.hpp; no new UI chrome is invented here (the project's standing
// rule). The one exception is flagged where it lives: the F2 lobby-chat overlay
// (chat_overlay.hpp), which the maintainer asked for.
//
// run_menu, run_seat_count, run_public_browser and run_online are all the
// generic bevel list dialog sub_42DBCC over the MAINMENU backdrop, exactly like
// the *.BM help browser's picker; run_code_entry is the sub_42E938 text-entry
// family at the same y=180 anchor as the editor's save-as prompt (sub_4028D2).
// Transient and failed states reuse the sub_414340 acknowledge modal.
//
// Structured like netplay_connect_screen.hpp: a ScreenContext by value, a
// platform::FrameClock-paced SDL loop per screen, Esc cancels back to the
// caller, and an SDL_QUIT is surfaced (never swallowed) so GameApp can propagate
// a hard quit. NetplayRunner owns the transport and runs the match.

namespace bomber::net {
class UdpTransport;  // borrowed by reference; the .cpp includes the real header
class LobbyFlow;     // ditto — run_online drives a flow the CALLER owns
}  // namespace bomber::net

namespace bomber::game {

// The NETWORK GAME menu's outcome. Sized like AppState (app_flow.hpp).
enum class LobbyMenuChoice : std::uint8_t {
    Cancel,        // Esc / Done -> back to the main menu
    WindowClosed,  // SDL_QUIT -> GameApp propagates AppInput::Quit
    HostOnline,    // HOST PRIVATE GAME: create a lobby, show its code
    HostPublic,    // HOST PUBLIC GAME: same, but listed for anyone to browse
    JoinOnline,    // JOIN BY CODE: the 6-char code modal, then the waiting room
    HostDirect,    // HOST LAN GAME: the ADR-0010 direct-UDP host (no server)
    JoinDirect,    // JOIN BY IP ADDRESS: the ADR-0010 direct-UDP join
    BrowsePublic,  // BROWSE PUBLIC GAMES: the Phase 3 list, then the same join
};

// What the waiting room needs to DISPLAY, beyond the flow that drives it: which
// transient modal to show while the connection comes up, and how many seats the
// HOST asked for (run_seat_count) so the room can draw an OPEN row for every one
// still unfilled. 0 = a guest — JoinAccepted carries no lobby size (PROTOCOL.md
// §4), so a guest simply lists the roster it was given.
struct LobbyRoomView {
    std::string code;
    int max_seats = 0;
    bool host = false;
};

// The waiting room's outcome. `ready` means LobbyFlow reached Phase::Ready, so
// the transport is punched-and-connected and the fields below are the SERVER's
// authoritative match parameters (design §1.6) — NetplayRunner feeds them
// straight into a NetSeats instead of deriving seats from a role.
struct LobbyRoomResult {
    bool ready = false;
    bool window_closed = false;
    // This peer is the hub. Only the hub schedules a dropped seat's handoff to
    // the AI (net::DropPolicy) — a guest must never mutate the hashed State on
    // its own authority.
    bool is_host = false;
    std::uint32_t seed = 0;
    std::uint16_t local_seats_mask = 0;
    // Every NETWORK seat in the match (LobbyFlow::MatchStart::all_seats_mask,
    // derived from the server's seat_assign). Passed straight to the
    // RollbackSession and the SetupSession instead of assuming 0b11, which is
    // what caps a match at two peers.
    std::uint16_t all_seats_mask = 0;
};

class LobbyScreen {
public:
    explicit LobbyScreen(ScreenContext ctx) : ctx_(ctx) {}

    // `online_available` is false on a lobby-off build (no BOMBER_HAS_LOBBY),
    // which lists only the two direct/LAN rows.
    LobbyMenuChoice run_menu(bool online_available);

    bool run_code_entry(std::string& code, bool& window_closed);

    // The host's lobby size, asked BEFORE the room is created because
    // `max_seats` is a CreateLobby field the server mints seats from
    // (PROTOCOL.md §3, clamped to 2..10) and cannot be changed afterwards.
    //
    // These are NETWORK seats — one per machine — not the match's player count:
    // AI slots are added on the roster screen afterwards and are simulated on
    // every peer rather than exchanged, so a 2-seat lobby can still be a 10-way
    // game (net_setup_roster.hpp's "SEAT LOCKING").
    bool run_seat_count(int& seats, bool& window_closed);

    // How to reach the matchmaker. NetplayRunner resolves these (CLI flag -> env
    // var -> compile-time default) and hands the result down.
    //
    // This header needs NO BOMBER_HAS_LOBBY guard: it names no net type except
    // the forward-declared UdpTransport/LobbyFlow. Only run_online's DEFINITION
    // is guarded — guarding the declaration would give LobbyScreen two different
    // definitions across translation units, since BOMBER_HAS_LOBBY is visible
    // inside the netplay target but not to its consumers (bomber::net is a
    // PRIVATE dependency).
    struct OnlineConfig {
        std::string server_url;  // "ws://host:8080/ws"
        std::string stun_host;   // the same host's UDP STUN echo (PROTOCOL.md §2)
        std::uint16_t stun_port = 8081;
        std::string player_name = "PLAYER";
    };

    // Sit in the waiting room until the match starts, the player leaves, or the
    // flow fails. The CALLER owns `flow` (and the bound socket behind it) and has
    // already told it to host or join: the control connection has to OUTLIVE the
    // waiting room, so the same lobby — and the same conversation — carries on
    // through the online setup screens.
    LobbyRoomResult run_online(net::LobbyFlow& flow, ChatOverlay& chat, const LobbyRoomView& view);

    // Returns true with `code` set to the picked row's code — the SAME contract
    // run_code_entry has, so the caller falls into the identical run_online()
    // waiting room instead of a second copy of it. `transport` is a BROWSING
    // socket: browsing never punches, so the caller opens a fresh one for the
    // match it goes on to join.
    bool run_public_browser(const OnlineConfig& cfg, net::UdpTransport& transport,
                            std::string& code, bool& window_closed);

private:
    ScreenContext ctx_;
};

}  // namespace bomber::game
