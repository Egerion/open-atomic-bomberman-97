#include "systems/enclosure.hpp"

#include <algorithm>
#include <utility>
#include <vector>

#include "grid.hpp"
#include "systems/powerups.hpp"

namespace bomber::sim {
namespace {

// The full drop-EVENT sequence for every ring the grid geometrically supports,
// plus the event-count after each ring closes. A literal port of sub_426818's
// advance/accept-or-turn state machine (docs/re/enclosure.md §4), NOT a
// from-scratch ring formula — because the original does not emit exactly one
// event per unique tile. Turning a corner without moving re-drops the SAME tile
// on the next 250 ms slot (a genuine extra pause and a second crush chance), and
// the up-walk's last step lands back on the ring's own start tile. Porting the
// machine literally reproduces both, and gets the degenerate innermost rings
// (one row tall, where every direction can reject in a row) right for free.
struct SpiralData {
    std::vector<std::pair<int, int>> events;
    std::vector<int> ring_end;  // ring_end[r] = event count once ring r has closed
};

const SpiralData& spiral() {
    static const SpiralData data = [] {
        SpiralData d;
        int x = 0, y = 0, dir = 1, depth = 0;
        // Ring cap: the grid's own geometric limit — a ring needs positive width
        // AND height. For the shipped 15x11 board that is 6 rings, which is also
        // what VALUELST 27's "all the way" cashes out to, so the cap never binds
        // in practice; it exists so a malformed custom depth degrades gracefully
        // instead of spinning on an inverted ring box.
        const int max_rings = std::min(kGridWidth, kGridHeight) / 2 + 1;
        for (int guard = 0; guard < 100000; ++guard) {
            d.events.emplace_back(x, y);
            const int nx = x + grid::kDx[dir], ny = y + grid::kDy[dir];
            const bool ok = (kGridWidth - depth > nx) && (kGridHeight - depth > ny) &&
                            (nx >= depth) && (ny >= depth);
            if (ok) {
                x = nx;
                y = ny;
                continue;
            }
            dir = (dir + 1) & 3;
            if (dir != 1) continue;  // (x,y) unchanged: the next slot re-drops it
            d.ring_end.push_back(static_cast<int>(d.events.size()));
            if (depth + 1 >= max_rings) break;
            ++depth;
            ++x;
            ++y;
        }
        return d;
    }();
    return data;
}

// Rings closed for a given enclosement_depth SETTING, per VALUELST 27's authored
// comment: "0 is none, 1 is 2 rows, 2 is 4 rows, 3 is all the way".
//
// REPORTED DEVIATION, deliberately not changed. A literal transcription of
// sub_426818's own stop check (twice dword_464974 compared against the ring
// counter dword_462240, stopping once the former is <= the latter, evaluated as
// a traversal wraps back to dir=1) reads as rings 0..2*depth INCLUSIVE — one
// ring MORE — and an exact re-implementation of that check does close a real
// extra ring for depth 1/2 on a 15x11 board. That reading contradicts both the
// VALUELST-authored comment and the golden-tested behaviour (cells[2][2] stays
// open at depth 1); depth 3 cannot discriminate, since both readings exhaust the
// board's 6 rings. The port keeps the comment-and-golden-corroborated rule; see
// docs/re/audit/enclosure.md for the full evidence.
int rings_for(int depth) {
    int max_rings = static_cast<int>(spiral().ring_end.size());
    return std::min(std::clamp(depth, 0, 3) * 2, max_rings);
}

// sub_405D0C — the actor-registry sweep sub_426818's ARM branch runs exactly
// once, the frame the walls start closing: it walks all 100 registry slots and
// clears the active flag of every one whose type field at +4 is nonzero and
// either <= 1 or exactly 3. The type is unsigned and already known nonzero, so
// "<= 1" is exactly "== 1": the sweep deactivates every WARPHOLE and every
// TRAMPOLINE and leaves dirarrows and conveyors alone.
//
// Clearing the active flag removes the actor from BOTH consumers at once — the
// tile->actor lookup sub_405654 (so the step-on trigger and the sliding-bomb
// warphole block stop firing) and the animator sub_4056CA (so the art
// disappears). It is a GAMEPLAY change, not a render hide, and the reason is the
// crush: a player mid-hop (state 5) or mid-warp (states 6/7) is immune to
// sub_41DE63, the very routine the wall crush calls, so leaving those two types
// live would let a player ride straight through a closing wall.
//
// This corrects docs/re/audit/enclosure.md Finding 0, which read
// batch_0x405B3A.cpp's stale header comment calling dword_45E0A8 a "level-select
// broadcast table" and concluded the call was inert. It is the same global the
// actor pool lives in (globals.h line 678: "the 100-slot x 152-byte 'extra
// object' pool"). Confirmed in-game on COAL MINE: the warpholes vanish the
// moment the walls start closing, the whole ring-2 rectangle at once, long
// before the spiral could reach their tiles.
void clear_hurry_disabled_actors(State& s) {
    // warp_dest_x/y and actor_dir are deliberately left alone: the original only
    // zeroes the ACTIVE dword at the head of the slot record, every other field
    // survives, and no consumer reads them without first matching on the type. An
    // in-flight hop/warp likewise completes — states 5/6/7 never re-consult the
    // registry, and neither do tick_bounce/tick_warp.
    grid::for_each_cell([&](int x, int y) {
        const ActorType t = s.actor_type[y][x];
        if (t == ActorType::Warphole || t == ActorType::Trampoline)
            s.actor_type[y][x] = ActorType::None;
    });
}

// The grounded bomb on a crushed tile detonates or is eaten — the "Stomped Bombs
// Detonate" option (dword_464940; VALUELST id 46 seeds it, options.ini and the
// Options row override it). sub_426818 ~27257 finds the bomb via the
// grounded-bomb scan sub_422E48 — which skips motion states 2/3, so a bomb
// arcing over the tile sails on — then ON queues a proper detonation via
// sub_423209(bomb, -1), OFF zeroes the bomb in place with no explosion.
void crush_bombs_on(State& s, int wx, int wy) {
    for (Bomb& b : s.bombs) {
        if (!b.active || b.flying || b.tile_x() != wx || b.tile_y() != wy) continue;
        if (!s.tuning.wall_detonates) {
            // The OFF branch keeps looping to exhaustion, matching sub_424841's
            // fall-through re-search, which takes no jump out of the loop.
            b.active = false;
            if (s.players[b.owner].bombs_placed > 0) --s.players[b.owner].bombs_placed;
            continue;
        }
        // sub_423209(bomb, -1) does NOT explode synchronously — it appends to a
        // 100-slot pending queue drained by sub_42331C, and that drain runs once
        // per frame behind a frame-stamp gate. In sub_42A191's order the draining
        // call sub_4245B9 runs BEFORE the enclosure, and the only other sub_42331C
        // call that frame runs after it but no longer drains. So a bomb queued by
        // THIS frame's wall drop detonates on the FOLLOWING frame: a confirmed
        // one-tick defer. Forcing our fuse to fire on the next tick_fuses() pass
        // (which precedes enclosure.update() in our tick order) is the equivalent.
        b.fuse = 1;
        // A FIZZLING DUD is crushed exactly like a live bomb: the dud marker lives
        // in the bomb's state dword, not the motion word, so sub_422E48 finds it,
        // and sub_426818's crush loop has no dud branch. On the drain side only
        // the elapsed-fuse INCREMENT is gated on "state != 2" — the detonation
        // test right after it is ungated. Our tick_fuses() gates on dud_left first
        // (correctly: an ordinary fuse must stay frozen while the bomb fizzles),
        // so without this clear the forced fuse would be swallowed for the rest of
        // the fizzle window (up to 120 ticks) and erupt from under an
        // already-solid wall. facts.md "Enclosure wall crushes a fizzling dud".
        b.dud_left = 0;
        // Only the FIRST grounded bomb on the tile is queued per drop event: at
        // pseudo.c 27262 the loop hands its find to sub_423209 and at 27263 jumps
        // unconditionally out, so there is no path back to the top of that while
        // on the ON branch. A second grounded bomb sharing the tile — structurally
        // possible, since sub_422E48 is a linear scan by coordinate rather than a
        // 1:1 per-cell grid — is left untouched. docs/re/audit/enclosure.md #1.
        break;
    }
}

// The crush kill. The finder sub_421D3F excludes player-type 4 (network
// spectator; this port has no such slot), and the shared kill routine it feeds,
// sub_41DE63, early-outs while the victim's movement state is 5 (hop) or 6/7
// (warp) — the SAME immunity the rover landing kill and the ordinary flame death
// funnel through (docs/re/campaign.md clause 4, stage-actors.md §592).
void crush_player(State& s, int i, int wx, int wy) {
    Player& p = s.players[i];
    if (!p.present || !p.alive || p.bounce != 0 || p.warp != 0) return;
    if (p.tile_x() != wx || p.tile_y() != wy) return;
    p.alive = false;
    if (p.carrying) {
        p.carrying = false;
        if (s.players[p.carried_owner].bombs_placed > 0) --s.players[p.carried_owner].bombs_placed;
    }
    // The crush routes through the same kill funnel as a flame death, so it
    // scatters too (facts.md "Death powerup scatter"). PowerupSystem is a thin
    // State& wrapper — construct one locally.
    PowerupSystem{s}.death_scatter(p);
    // No attributable killer for a wall crush: event.hpp's PlayerDied convention
    // reads data == -1 as "no killer", distinct from a self-kill.
    s.events.push_back({Event::Type::PlayerDied, static_cast<std::int8_t>(i),
                        static_cast<std::int8_t>(wx), static_cast<std::int8_t>(wy), -1});
}

}  // namespace

int EnclosureSystem::total(int depth) {
    int rings = rings_for(depth);
    if (rings <= 0) return 0;
    return spiral().ring_end[static_cast<std::size_t>(rings - 1)];
}

bool EnclosureSystem::position(int index, int depth, int* x, int* y) {
    if (index < 0 || index >= total(depth)) return false;
    const auto& ev = spiral().events[static_cast<std::size_t>(index)];
    *x = ev.first;
    *y = ev.second;
    return true;
}

void EnclosureSystem::drop_wall(int wx, int wy) {
    State& s = s_;
    crush_bombs_on(s, wx, wy);
    s.cells[wy][wx] = Cell::Solid;
    s.burning[wy][wx] = 0;
    s.flame[wy][wx] = 0;
    s.hidden[wy][wx] = PowerupType::None;
    s.floor[wy][wx] = PowerupType::None;
    for (int i = 0; i < kMaxPlayers; ++i) crush_player(s, i, wx, wy);
    s.events.push_back({Event::Type::WallClosed, -1, static_cast<std::int8_t>(wx),
                        static_cast<std::int8_t>(wy), 0});
}

// CONFIRMED cadence (sub_426818): one drop EVENT every 250 ms of wall clock,
// gated by timeGetTime() — the deadline global dword_46223C advances by a
// hardcoded 250 each time it fires. It is a HARDCODED constant, NOT a getvalue
// (the only enclosure getvalues are id 27 = depth and id 101 = the hurry
// threshold). At the locked 20 Hz tick that is exactly one event per 5 ticks;
// the original's up-to-5-drops-per-frame catch-up only fires when a frame ran
// long, which deterministic lockstep never does. Applies uniformly to every
// entry in spiral().events, phantom corner repeats included.
static constexpr int kEncloseIntervalTicks = 250 / (1000 / kTicksPerSecond);  // = 5

void EnclosureSystem::update() {
    State& s = s_;
    int depth = s.tuning.enclosement_depth;

    // sub_410578/sub_4105D2's dword_4601A4 is (total_ms - elapsed_ms)/1000,
    // FLOORED and clamped >= 0. ticks_left already IS the remaining ms/50 and is
    // itself clamped >= 0, so an integer divide reproduces that floor exactly.
    // Comparing raw ticks_left against threshold*kTicksPerSecond is NOT
    // equivalent — it rounds the wrong way, firing the banner one tick early and
    // the wall-arm up to 19 ticks late.
    const int seconds_left = s.ticks_left / kTicksPerSecond;

    // TWO distinct moments in the original, and conflating them closed the walls
    // 5 s too early (docs/re/enclosure.md §2):
    //  1. banner + voice callout: remaining < getvalue(101), STRICT (the HUD
    //     block ~29533, latched on dword_464984, sub_427961(2700));
    //  2. the walls actually START closing: remaining <= getvalue(101) - 5, NOT
    //     strict (sub_426818 ~27170). That is 5 s after the banner window OPENS,
    //     and the banner's own window is the other half of the same HUD check, so
    //     the banner turns off exactly as the walls arm — no gap, no overlap.
    //
    // NO `ticks_left > 0` guard: sub_410578's remaining-seconds is clamped >= 0,
    // so once the predicate goes true it stays true forever — the original keeps
    // closing walls through and past TimeUp (sudden death) and never freezes the
    // spiral. Gating on ticks_left > 0 froze enclose_index the instant the match
    // clock hit zero, stranding the spiral mid-ring.
    const bool warn = seconds_left < s.tuning.hurry_seconds;
    const bool closing = seconds_left <= s.tuning.hurry_seconds - 5;

    if (!s.hurry && warn) {
        s.hurry = true;
        s.events.push_back({Event::Type::Hurry, -1, -1, -1, 0});
    }

    // Arm the drop machinery once, on the first tick the walls close.
    // enclose_interval stays 0 until then, so it doubles as the "armed" flag.
    if (closing && s.enclose_interval == 0) {
        s.enclose_interval = kEncloseIntervalTicks;
        s.enclose_timer = s.enclose_interval;
        s.enclose_index = 0;
        // sub_405D0C is NOT depth-gated: the original runs it inside the arm block
        // guarded by dword_45BE9C, which sits ABOVE the "2*depth > ring" drop
        // gate, so even enclosement_depth = 0 still kills warpholes/trampolines.
        clear_hurry_disabled_actors(s);
        // NO drop on the arm tick (the original's deadline == now, gate shut); the
        // first wall lands exactly one interval later.
        return;
    }

    if (closing && s.enclose_interval > 0 && depth > 0 && s.enclose_index < total(depth) &&
        --s.enclose_timer <= 0) {
        s.enclose_timer = s.enclose_interval;
        int wx = 0, wy = 0;
        if (position(s.enclose_index++, depth, &wx, &wy)) drop_wall(wx, wy);
    }
}

}  // namespace bomber::sim
