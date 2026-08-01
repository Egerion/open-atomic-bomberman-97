#pragma once

#include <array>
#include <cstdint>
#include <string>

#include "bomber/game_util/app_flow.hpp"      // AppInput
#include "bomber/netui/net_setup_roster.hpp"  // LocalRoster / LevelPreview / LevelChoice
#include "bomber/sim/constants.hpp"           // sim::kMaxPlayers
#include "bomber/ui/screen_context.hpp"

// ONLINE MATCH SETUP — the "this screen is driven by someone else" seam.
//
// SHAPE, FROM THE ORIGINAL (docs/re/network-screens.md §7). Neither 1997 network
// screen picks a map or assigns an AI: both commit into `sub_42A3F6`, the SAME
// handler main-menu row 0 (PLAY) uses, so a net game's roster and level come
// from the ORDINARY shared screens — `sub_410F81` (PLAYER INPUT TYPE SELECTION)
// and `sub_406DDE` (LEVEL & ROUNDS). There the HOST makes every change and
// broadcasts it (roster kind 40, team kind 58, level kind 43, rounds kind 44)
// while a GUEST is strictly read-only: every edit path is guarded by
// `sub_40C06A() != 1`, and an edit key falls through to a shared
// `sub_427961(40)` — the "you can't do that here" buzz. So this is NOT a
// net-only setup screen; it is a parameter the two REAL screens take, and a
// `nullptr` session leaves them byte-identical to local play.
//
// SEAT LOCKING (a port constraint, not an RE'd behaviour — flagged as such).
// The original lets each machine upload its own slots (kind 40 from
// `sub_410F81`'s tail); our layer is host-drives-everything (setup_session.hpp's
// "NOT IMPLEMENTED, DELIBERATELY") and seat ownership is already fixed before
// the setup stage runs. A human slot outside those seats would be simulated from
// local input on one peer and from nothing on the other — a guaranteed desync —
// so EVERY wire seat is LOCKED here (ours to KEYBOARD, the rest to type 4 =
// OTHER, exactly what `sub_40D372` writes for "someone else's player") and every
// other slot cycles OFF <-> COMPUTER only. Both fields are MASKS, so a 3-, 6- or
// 10-seat lobby needs nothing new. AI slots are simulated identically on every
// peer, so they are how a match gets more PLAYERS than it has machines.
//
// THE `rounds == 0` SENTINEL. `SetupPreviewFrame` is display-only and carries no
// "which screen is the host on" field, so the host publishes `rounds = 0` on the
// ROSTER screen and the real win target (1..100) once it reaches LEVEL & ROUNDS
// — the guest's cue to switch screens with it, standing in for the original's
// `sub_40F064(901/902)` (kind 32) advance commands. 0 is not a legal win target
// anywhere, so it cannot collide with a real value.

namespace bomber::net {
class SetupSession;
}  // namespace bomber::net

namespace bomber::game {

// Members are ordered pointer-first so the struct carries no avoidable padding
// (.clang-tidy's clang-analyzer-optin.performance.Padding).
struct NetSetupLink {
    // BORROWED and owned by the caller (NetplayRunner::present_setup), which also
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

bool net_setup_final(const NetSetupLink& l);   // the agreed config is on EVERY peer
bool net_setup_failed(const NetSetupLink& l);  // timed out / the peer vanished
bool net_setup_has_preview(const NetSetupLink& l);
// The `rounds != 0` sentinel above: the host has moved on to LEVEL & ROUNDS.
bool net_setup_on_level_screen(const NetSetupLink& l);

// HOST, once before the roster screen opens: force every wire seat into the only
// roster shape the seat masks can simulate, and clear every other slot holding
// an untransmittable human type (see SEAT LOCKING).
void net_setup_seed_host_roster(const NetSetupLink& l, std::array<int, sim::kMaxPlayers>& type,
                                std::array<int, sim::kMaxPlayers>& sub);

// HOST: broadcast the current roster + level choice as a live preview.
// `level.level` is -1 for RANDOM and `level.rounds` 0 while still on the roster
// screen (the sentinel above). The roster's `sub` half never travels — the wire
// carries seat KINDS, and each machine re-derives its own key sets.
void net_setup_publish(const NetSetupLink& l, const LocalRoster& roster, const LevelPreview& level);

// HOST, from LEVEL & ROUNDS: re-publish the CURRENT preview with only the
// level/rounds fields replaced. The roster half comes from the session's own last
// frame, so a screen that holds no roster state cannot blank the guest's roster.
void net_setup_publish_level(const NetSetupLink& l, const LevelPreview& level);

// GUEST: mirror the newest preview into the display roster, re-pointed to THIS
// machine's view (see SEAT LOCKING). A no-op until the first preview arrives.
void net_setup_apply_roster(const NetSetupLink& l, const LocalRoster& out);

// GUEST: mirror the newest preview's level/rounds. `out.level` is clamped to a
// stage this install's registry knows (an unknown index falls back to RANDOM for
// the sample-block swatch only); `out.name` is the HOST's own label, so the
// guest reads the map the host picked even for a custom map it does not have.
void net_setup_apply_level(const NetSetupLink& l, int level_count, const LevelChoice& out);

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
