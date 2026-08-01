#pragma once

#include <cstdint>

#include "bomber/game_util/app_flow.hpp"  // AppInput
#include "bomber/netplay/netplay_state.hpp"
#include "bomber/sim/match_config.hpp"  // sim::MatchConfig

// ONE NETPLAY MATCH over an ALREADY-CONNECTED transport — a best-of-N sequence
// of ROUNDS, exactly like the local Play flow (docs/re/in-match-shell.md "The
// round-end shell": `sub_42A3F6` loops back into `sub_410B6E` until somebody
// clinches) — driven through the SAME MatchRunner a local match uses.
//
// The loop is implementation detail of netplay_match.cpp: the round gates, the
// netdiag.log recorder and the loop itself live in that file's anonymous
// namespace, so nothing net-shaped crosses this header.

namespace bomber::net {
class Transport;  // the abstract seam: a bare socket, the star hub, or the relay
}  // namespace bomber::net

namespace bomber::game {

// WHO PLAYS WHAT. Seat bitmask, bit s == seat s, matching
// LockstepSession::fill_seats; `all` is EVERY network seat the caller was given
// — the server's seat_assign online, 0b11 for the CLI/LAN pairs.
//
// AI slots are NOT in these masks: they are simulated identically on every peer
// from the shared config and their input is never exchanged
// (rollback_session.hpp), so the roster can hold ten PLAYERS over fewer seats.
struct NetSeats {
    std::uint16_t local = 0;  // the seats THIS peer owns
    std::uint16_t all = 0;    // EVERY network seat in the match
    // Gates the peer-drop handoff (only the hub may schedule a silent seat's
    // move to the AI — net::DropPolicy — since a guest must never mutate the
    // hashed State on its own authority) and, on the shared outcome screens,
    // who is allowed to dismiss.
    bool host = false;

    // The seats SOMEBODY ELSE plays, which is what SetupSession wants.
    std::uint16_t remote() const { return static_cast<std::uint16_t>(all & ~local); }
};

// The round counter feeding net::round_tick_base is walked across every match
// played on one transport, so it has to stay inside the range that function
// documents as wrap-free. Both peers count the same rounds, so both wrap on the
// same one; by the time 1024 have been played a straggler from round 0 is many
// hours dead.
inline constexpr int kNetRoundBaseWrap = 1024;

// What a SESSION carries across the matches played on one transport. nullptr
// means "a caller that plays exactly one match and offers no rematch".
struct NetSessionCarry {
    // IN/OUT. Where this match's rounds sit in the shared tick space: every
    // round's base is net::round_tick_base(round_base + round), so match 2's
    // round 0 cannot land in the tick space match 1 was still sending into.
    int round_base = 0;
    // OUT. Set when the match was DECIDED and both peers agreed
    // (net::RematchSession) to walk back to the setup screens over the same
    // transport instead of tearing it down.
    bool rematch = false;
};

// Everything ONE match is run over. A parameter object (§3) rather than six
// arguments; call-scoped, like ScreenContext.
//
// `config` is THE agreed MatchConfig and is used verbatim — the whole board, the
// roster, the stage index, the tuning and the seed. On the host it is what the
// setup stage confirmed; on the guest it is SetupSession::final_config(), i.e.
// the host's exact bytes (match_config_codec.hpp). It replaces the hard-coded
// canonical config this used to build, which was a determinism shortcut that
// cost online play its map choice, its AI slots and its roster.
struct NetMatchRun {
    const NetplaySeams& seams;
    NetplayState& state;
    net::Transport& transport;
    NetSeats seats;
    const sim::MatchConfig& config;
    NetSessionCarry* carry = nullptr;
};

// Build a byte-identical arena and play rounds until somebody clinches, somebody
// leaves, or the link dies. `config` seeds round 0; a round that ends without a
// clinch runs the outcome screen with a between-rounds gate over it
// (net_round_gate.hpp) and starts the next round on the config the HOST confirms
// through that gate. Every round's seed and tick base come from
// net::round_rotation.hpp, so both peers agree on which round they are in with
// no extra traffic.
//
// Returns Advance once the match is decided/abandoned, Quit on a window close.
AppInput run_netplay_match(const NetMatchRun& run);

}  // namespace bomber::game
