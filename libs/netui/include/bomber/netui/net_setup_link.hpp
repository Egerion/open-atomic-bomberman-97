#pragma once

#include <array>
#include <cstdint>
#include <string>

#include "bomber/game_util/app_flow.hpp"  // AppInput
#include "bomber/sim/constants.hpp"       // sim::kMaxPlayers
#include "bomber/ui/screen_context.hpp"

// ONLINE MATCH SETUP — the "this screen is driven by someone else" seam.
//
// SHAPE, FROM THE ORIGINAL (docs/re/network-screens.md §7). Neither 1997 network
// screen picks a map or assigns an AI: both commit into `sub_42A3F6`, the SAME
// handler main-menu row 0 (PLAY) uses, so a net game's roster and level come
// from the ORDINARY shared screens — `sub_410F81` (PLAYER INPUT TYPE SELECTION)
// and `sub_406DDE` (LEVEL & ROUNDS). There, the HOST makes every change and
// broadcasts it (roster kind 40, team kind 58, level kind 43, rounds kind 44)
// while a GUEST is strictly read-only: every edit path is guarded by
// `sub_40C06A() != 1`, and a guest that presses an edit key falls through to a
// shared `sub_427961(40)` label — the "you can't do that here" buzz.
//
// So this is deliberately NOT a net-only setup screen. It is a parameter the two
// REAL screens take: `nullptr` session = ordinary local play (byte-identical to
// before), a session = the same screen wired to a net::SetupSession — the host
// publishing a preview after every edit, the guest rendering that preview and
// buzzing SFX 40 at any edit key.
//
// SEAT LOCKING (a port constraint, not an RE'd behaviour — flagged as such).
// The original lets each machine upload its own slots (kind 40 from
// `sub_410F81`'s tail); our layer is host-drives-everything (setup_session.hpp's
// "NOT IMPLEMENTED, DELIBERATELY"), and the seat ownership is already fixed by
// the lobby/handshake before the setup stage runs. A human slot outside those
// seats would be simulated from local input on one peer and from nothing on the
// other — a guaranteed desync — so EVERY wire seat is LOCKED here (the ones this
// machine owns to KEYBOARD, the rest to type 4 = OTHER, which is exactly what
// `sub_40D372` writes for "someone else's player") and every other slot cycles
// OFF <-> COMPUTER only. Both fields are MASKS and always were, so a 3-, 6- or
// 10-seat lobby needs nothing new here — it just arrives with more bits set.
// AI slots are simulated identically on every peer from the shared config, so
// they are how a match gets more PLAYERS than it has machines.
//
// THE `rounds == 0` SENTINEL. `SetupPreviewFrame` is display-only and lossy by
// contract (setup_session.hpp), and it carries no "which screen is the host on"
// field. The host publishes `rounds = 0` while it is on the ROSTER screen and
// the real win target (1..100) once it reaches the LEVEL & ROUNDS screen, which
// is the guest's cue to switch screens with it — the port's stand-in for the
// original's `sub_40F064(901/902)` (kind 32) screen-advance commands. 0 is not a
// legal win target anywhere (the level screen clamps to 1..100), so it cannot
// collide with a real value.

namespace bomber::net {
class SetupSession;
}  // namespace bomber::net

namespace bomber::game {

// Members are ordered pointer-first so the struct carries no avoidable padding
// (.clang-tidy's clang-analyzer-optin.performance.Padding).
struct NetSetupLink {
    // BORROWED and owned by the caller (GameApp::present_net_setup), which also
    // owns the transport both this and the match session drain. nullptr = an
    // ordinary local match: every helper below is then a no-op and both screens
    // behave exactly as they did before online setup existed.
    net::SetupSession* session = nullptr;
    std::uint16_t local_seats = 0;   // the wire seats THIS machine plays
    std::uint16_t remote_seats = 0;  // the wire seats the PEER plays
    bool host = false;               // false = read-only (the sub_40C06A guest)
};

inline bool net_setup_active(const NetSetupLink& l) {
    return l.session != nullptr;
}
// The original's `sub_40C06A() == 1` guard: a guest may look, never touch.
inline bool net_setup_readonly(const NetSetupLink& l) {
    return l.session != nullptr && !l.host;
}
// A slot owned by the wire seat assignment (see "SEAT LOCKING" above).
inline bool net_setup_slot_locked(const NetSetupLink& l, int slot) {
    if (l.session == nullptr) return false;
    const auto bit = static_cast<std::uint16_t>(1u << slot);
    return (l.local_seats & bit) != 0 || (l.remote_seats & bit) != 0;
}

// One pump of the setup session on the caller's SDL clock. MUST run every frame
// of every loop a setup screen can sit in (including the F1 help browser and the
// start-guard modal): the host re-broadcasts on an interval and the guest fails
// after `timeout_ms` of silence, so a screen that stops pumping kills the link.
void net_setup_pump(const NetSetupLink& l);

bool net_setup_final(const NetSetupLink& l);   // the agreed config is on BOTH peers
bool net_setup_failed(const NetSetupLink& l);  // timed out / the peer vanished
bool net_setup_has_preview(const NetSetupLink& l);
// The `rounds != 0` sentinel above: the host has moved on to LEVEL & ROUNDS.
bool net_setup_on_level_screen(const NetSetupLink& l);

// HOST: force every wire seat into the only roster shape the seat masks can
// simulate (local -> KEYBOARD with the next key-set, remote -> OTHER), and clear
// every other slot that holds a human type (which would be untransmittable).
// Called once before the roster screen opens.
void net_setup_seed_host_roster(const NetSetupLink& l, std::array<int, sim::kMaxPlayers>& type,
                                std::array<int, sim::kMaxPlayers>& sub);

// HOST: broadcast the current roster + level choice as a live preview. `level` is
// the LEVEL & ROUNDS working value (-1 = RANDOM), `level_name` the string the
// host has on screen (sanitised to printable ASCII and clamped by the codec),
// `rounds` the win target or 0 while still on the roster screen (the sentinel).
void net_setup_publish(const NetSetupLink& l, const std::array<int, sim::kMaxPlayers>& type,
                       const std::array<int, sim::kMaxPlayers>& team, bool team_play, int level,
                       const std::string& level_name, int rounds);

// HOST, from the LEVEL & ROUNDS screen: re-publish the CURRENT preview with only
// the level/rounds fields replaced. The roster half comes from the session's own
// last frame, so the level screen — which holds no roster state of its own — can
// broadcast a map change without blanking the roster the guest is showing.
void net_setup_publish_level(const NetSetupLink& l, int level, const std::string& level_name,
                             int rounds);

// GUEST: mirror the newest preview into the display roster. The kinds are
// re-pointed to THIS machine's view — the seat we own renders as KEYBOARD, every
// other human as OTHER (type 4), matching `sub_40D372`'s "someone else's player".
// A no-op until the first preview arrives.
void net_setup_apply_roster(const NetSetupLink& l, std::array<int, sim::kMaxPlayers>& type,
                            std::array<int, sim::kMaxPlayers>& sub,
                            std::array<int, sim::kMaxPlayers>& team, bool& team_play);

// GUEST: mirror the newest preview's level/rounds. `level` is clamped to a stage
// this install's registry actually knows (an unknown index falls back to RANDOM
// for the sample-block swatch only); `level_name` is the HOST's own label, so the
// guest reads the map the host picked even for a custom map it does not have.
void net_setup_apply_level(const NetSetupLink& l, int level_count, int& level, int& rounds,
                           std::string& level_name);

// `sub_42B47D`'s [WAIT] prompt (docs/re/network-screens.md §6/§9): getstring(80)'s
// single `%c` takes the 4-phase spinner `dword_45BFB4 = {'/','-','\\','|'}`,
// advanced ONCE PER RENDERED FRAME (`dword_464AFC`, frame-paced not time-paced),
// drawn at the pinned x=150, y=200, clip w=400 in `byte_49D38F` (240,248,252)
// over a black outline. Composited over whatever the caller already drew.
void draw_net_wait_prompt(ScreenContext ctx, unsigned& spinner);

// The acknowledge modal (sub_414340) over the MAINMENU backdrop, driven by its
// own key loop exactly as LobbyScreen drives the lobby's failure modal: the nav
// blip on any real key, closing on Enter/Space/Esc. Returns Quit if the window
// closed under it, else Advance.
AppInput run_net_notice(ScreenContext ctx, const std::string& top, const std::string& body);

}  // namespace bomber::game
