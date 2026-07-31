#pragma once

#include <array>
#include <vector>

#include "bomber/game_util/results.hpp"  // tally_kills
#include "bomber/net/rollback_session.hpp"
#include "bomber/sim/constants.hpp"
#include "bomber/sim/event.hpp"

// THE NETPLAY KILL TALLY — the online counterpart of results.hpp's tally_kills,
// and the reason it needs one.
//
// Summing per-tick `PlayerDied` events is fine on the local path, where every
// tick is simulated exactly once. Under ROLLBACK it is not: a mispredicted tick
// is simulated, then simulated AGAIN with corrected input, and `State::events`
// is rebuilt each time. Fed straight from `sim.state().events` once per pump,
// the counter accumulated whichever pass each peer happened to see — so two
// peers with different rollback histories reached DIFFERENT kill totals from an
// identical, agreed simulation, and could clinch for different players. Nothing
// could catch it: the counter lives outside the hashed sim state (determinism
// rule 4), so the goldens, the desync check and `build_hash` were all blind.
//
// The online path therefore takes its events from RollbackSession's CONFIRMED
// stream: every tick handed over exactly once, with the events the final agreed
// history produced, off the same snapshot the confirmed hash is taken from.
//
// This is OUTSIDE results.hpp, which is deliberately dependency-light (several
// SDL-free suites include it with only bomber::sim linked). It is the ONE header
// in this package reaching for libs/net, which is why that link is PUBLIC.

namespace bomber::game {

// One pump's worth. `scratch` is caller-owned only so the per-tick path
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
