#pragma once

#include <cstdint>

#include "bomber/game/app_flow.hpp"  // AppInput
#include "bomber/game/screens/netplay_state.hpp"
#include "bomber/sim/match_config.hpp"  // sim::MatchConfig

// ONE NETPLAY MATCH over an ALREADY-CONNECTED transport — a best-of-N sequence
// of ROUNDS, exactly like the local Play flow (docs/re/in-match-shell.md "The
// round-end shell": `sub_42A3F6` loops back into `sub_410B6E` until somebody
// clinches) — driven through the SAME MatchRunner a local match uses.
//
// Extracted out of GameApp (this was `run_netplay_match_seats`, the app shell's
// largest method by a factor of three). The whole loop is implementation detail
// of netplay_match.cpp: the round gates, the netdiag.log recorder and the loop
// itself are all in that file's anonymous namespace, so nothing net-shaped
// crosses this header and it names `net::Transport` by reference alone.

namespace bomber::net {
class Transport;  // the abstract seam: a bare socket, the star hub, or the relay
}  // namespace bomber::net

namespace bomber::game {

// WHO PLAYS WHAT, over whichever transport carries them. One value instead of
// the three parallel arguments (local mask, all mask, is-host flag) that every
// netplay entry point used to thread through by hand — they are never
// independent and getting two of them out of step is a desync.
//
// Seat bitmask, bit s == seat s, matching LockstepSession::fill_seats. `all` is
// EVERY network seat the caller was given — the server's seat_assign online,
// 0b11 for the CLI/LAN pairs — and the transport is whatever carries them: a
// bare socket for a pair, a StarHubTransport on the hub of a >2-seat match
// (ADR-0011 decisions 2+4). Neither the session nor anything below it needs to
// know which: RollbackSession already accepts arbitrary masks and the star is a
// Transport like any other.
//
// AI slots are NOT in these masks — they are simulated identically on every
// peer from the shared config and their input is never exchanged
// (rollback_session.hpp), so the roster can still hold ten PLAYERS over fewer
// seats.
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
// played on one transport, so it has to be kept inside the range that function
// documents as wrap-free (100 << 22 is "an order of magnitude inside uint32").
// Both peers count the same rounds — the rotation is agreed — so both wrap on
// the same round and stay in the same tick space. By the time 1024 rounds have
// been played, a straggler from round 0 is many hours dead.
inline constexpr int kNetRoundBaseWrap = 1024;

// What a SESSION carries across the matches played on one transport, and the
// only thing a match reports back beyond its AppInput.
//
// It replaces the two independent out-parameters (`int* round_base`, `bool*
// rematch`) the old signature carried. They were never independently null —
// the CLI passed neither and the session loop passed both — so one nullable
// bundle says the same thing with half the pointers: nullptr means "a caller
// that plays exactly one match and offers no rematch", which is what every
// `!= nullptr` test in the loop was really asking.
struct NetSessionCarry {
    // IN/OUT. Where this match's rounds sit in the shared tick space: every
    // round's tick space is net::round_tick_base(round_base + round), so match
    // 2's round 0 cannot land in the tick space match 1 was still sending into.
    int round_base = 0;
    // OUT. Set when the match was DECIDED and both peers agreed
    // (net::RematchSession) to walk back to the setup screens over the same
    // transport instead of tearing it down.
    bool rematch = false;
};

// Run ONE match: given an already-connected `transport`, the seats this peer
// owns, and the AGREED config, build a byte-identical arena and play rounds
// until somebody clinches, somebody leaves, or the link dies.
//
// `cfg` is THE agreed MatchConfig and is used verbatim — the whole board, the
// roster, the stage index, the tuning and the seed. On the host it is what the
// setup stage confirmed; on the guest it is SetupSession::final_config(), i.e.
// the host's exact bytes (match_config_codec.hpp). It replaces the hard-coded
// canonical config this used to build, which was a determinism shortcut that
// cost online play its map choice, its AI slots and its roster.
//
// A MATCH, NOT A ROUND. `cfg` seeds round 0; a round that ends without a clinch
// runs the outcome screen with a between-rounds gate over it
// (screens/net_round_gate.hpp) and starts the next round on the config the HOST
// confirms through that gate — the same `sub_42A3F6` best-of-N loop the local
// Play flow runs, with the host driving the advance exactly as the original's
// network client does (docs/re/in-match-shell.md "The round-end shell"). Every
// round's seed and tick base come from net::round_rotation.hpp, so both peers
// agree on which round they are in with no extra traffic.
//
// Returns Advance once the match is decided/abandoned, Quit on a window close.
AppInput run_netplay_match(const NetplaySeams& seams, NetplayState& state,
                           net::Transport& transport, NetSeats seats, const sim::MatchConfig& cfg,
                           NetSessionCarry* carry = nullptr);

}  // namespace bomber::game
