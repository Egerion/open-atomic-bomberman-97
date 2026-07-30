#pragma once

#include <array>
#include <vector>

#include "bomber/game/results.hpp"  // tally_kills
#include "bomber/net/rollback_session.hpp"
#include "bomber/sim/constants.hpp"
#include "bomber/sim/event.hpp"

// THE NETPLAY KILL TALLY — the online counterpart of results.hpp's tally_kills,
// and the reason it needs one.
//
// tally_kills sums `PlayerDied` events across ticks into a per-match counter that
// the RESULTS row shows and, under Team Play + "win by kills", the MATCH-CLINCH
// predicate reads (match_outcome.hpp's win_by_kills branch). Summing per-tick
// events is fine on the local path, where every tick is simulated exactly once.
// It is NOT fine under rollback: a mispredicted tick is simulated, then simulated
// AGAIN with corrected input, and `State::events` is rebuilt each time. Fed
// straight from `sim.state().events` once per pump, the counter therefore
// accumulated whichever pass each peer happened to see — so two peers with
// different rollback histories reached DIFFERENT kill totals from an identical,
// agreed simulation, and could clinch the match for different players.
//
// Nothing in the project could catch that: the counter lives outside the hashed
// sim state (determinism rule 4 says events are per-tick outputs, never hashed),
// so the goldens, the per-tick desync check and `build_hash` were all blind to it.
//
// So the online path takes its events from RollbackSession's CONFIRMED stream
// instead (rollback_session.hpp's note): every tick handed over exactly once,
// with the events the final agreed history produced, read off the same snapshot
// the confirmed hash both peers compare is taken from. The counter stops being an
// independent accumulation and becomes a function of the agreed history.
//
// These two live in their own header rather than in results.hpp because
// results.hpp is deliberately dependency-light — several SDL-free suites include
// it with only bomber::sim and bomber::assets linked, and pulling libs/net in
// there would break them. Header-only and SDL-free all the same, which is what
// lets tests/net drive the REAL wiring over a two-peer link.

namespace bomber::game {

// One pump's worth. `scratch` is a caller-owned buffer only so the per-tick path
// allocates nothing; its contents on entry are ignored.
inline void tally_netplay_kills(net::RollbackSession& session, std::vector<sim::Event>& scratch,
                                std::array<int, sim::kMaxPlayers>& kill_count) {
    scratch.clear();
    session.drain_confirmed_events(scratch);
    tally_kills(scratch, kill_count);
}

// The round's LAST tally, called once as the round stops: closes the range over
// the speculative tail so both peers end having covered the same ticks (see
// drain_remaining_events). The session must not be advanced afterwards.
inline void tally_netplay_kills_final(net::RollbackSession& session,
                                      std::vector<sim::Event>& scratch,
                                      std::array<int, sim::kMaxPlayers>& kill_count) {
    scratch.clear();
    session.drain_remaining_events(scratch);
    tally_kills(scratch, kill_count);
}

}  // namespace bomber::game
