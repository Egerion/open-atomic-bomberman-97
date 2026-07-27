#include "systems/enclosure.hpp"

#include <algorithm>
#include <utility>
#include <vector>

#include "systems/flames.hpp"
#include "systems/powerups.hpp"

namespace bomber::sim {
namespace {

// Direction table (dword_45BECC = cos = DX, dword_45BEDC = sin = DY; both
// confirmed against the .data dump: {0,1,0,-1} / {-1,0,1,0}), GODIR-indexed
// (0=Up,1=Right,2=Down,3=Left).
constexpr int kDX[4] = {0, 1, 0, -1};
constexpr int kDY[4] = {-1, 0, 1, 0};

// The full drop-EVENT sequence (not just the unique tiles) for every ring the
// grid geometrically supports, plus the event-count after each ring closes.
// A literal port of sub_426818's advance/accept-or-turn state machine
// (docs/re/enclosure.md §4), not a from-scratch ring formula — that matters
// because the original does NOT emit exactly one event per unique tile:
//
//   - Every 250 ms cadence slot (LABEL_26) unconditionally re-drops
//     sub_425E9B(x,y) at whatever (x,y) currently is, THEN computes the next
//     position. Turning a corner without moving (three of a ring's four
//     corners) leaves (x,y) unchanged, so the NEXT slot re-drops the SAME
//     tile — replaying the wall-slam sound and re-running the crush/detonate
//     checks on it a second time (a no-op solidify, but a genuine extra
//     250 ms pause and a second crush chance).
//   - The fourth corner (top-left, where the ring closes and the walk steps
//     inward) is reached differently: the up-walk runs all the way back to
//     the ring's OWN start tile as an ordinary accepted step (the bounds
//     check only excludes tiles outside the current ring box, not tiles
//     already visited), so that tile is dropped twice too — once opening the
//     ring, once again as the up-walk's last step — with no phantom repeat
//     needed.
//
// Porting the state machine literally reproduces both for free; a hand-
// derived "N unique tiles per ring" formula would have to special-case them
// (and get the degenerate innermost rings, which are only 1 tile wide/tall,
// right too — every direction can reject in a row there).
struct SpiralData {
    std::vector<std::pair<int, int>> events;
    std::vector<int> ring_end;  // ring_end[r] = event count once ring r has closed
};

const SpiralData& spiral() {
    static const SpiralData data = [] {
        SpiralData d;
        int x = 0, y = 0, dir = 1, depth = 0;
        // Ring cap: the grid's own geometric limit (a ring needs positive
        // width AND height; min(kGridWidth, kGridHeight)/2 rounded up gives
        // the last valid — possibly degenerate, e.g. 1 row tall — ring). For
        // the shipped 15x11 board that is 6 rings (indices 0..5), which is
        // also VALUELST 28's documented depth-setting count (id 28 = 4,
        // "0,1,2,3") times the 2-rings-per-step the id 27 comment gives, so
        // the cap never actually binds in practice; it exists purely so a
        // malformed/custom depth value degrades gracefully instead of
        // spinning on an inverted ring box.
        int max_rings = std::min(kGridWidth, kGridHeight) / 2 + 1;
        for (int guard = 0; guard < 100000; ++guard) {
            d.events.emplace_back(x, y);
            int nx = x + kDX[dir], ny = y + kDY[dir];
            bool ok = (kGridWidth - depth > nx) && (kGridHeight - depth > ny) && (nx >= depth) &&
                      (ny >= depth);
            if (ok) {
                x = nx;
                y = ny;
                continue;
            }
            dir = (dir + 1) & 3;
            if (dir == 1) {
                d.ring_end.push_back(static_cast<int>(d.events.size()));
                if (depth + 1 >= max_rings) break;
                ++depth;
                ++x;
                ++y;
            }
            // else: (x, y) unchanged — the next iteration re-drops it (a
            // phantom corner repeat).
        }
        return d;
    }();
    return data;
}

// Rings closed for a given enclosement_depth SETTING. VALUELST 27's authored
// comment (DATA/RES/VALUELST.RES) is explicit: "0 is none, 1 is 2 rows, 2 is
// 4 rows, 3 is all the way" — i.e. rings = 2 * depth, capped at the grid's
// own ring count (6 for 15x11, which is what "all the way" cashes out to).
// NOTE: a literal transcription of sub_426818's own stop check
// (`2*dword_464974 <= dword_462240`, evaluated once a ring's traversal wraps
// back to dir=1) reads as "stop once the ring just closed is ring number
// 2*depth", i.e. rings 0..2*depth INCLUSIVE — one ring MORE than the
// comment says, and confirmed (via an exact re-implementation of that check)
// to close a real extra ring for depth 1/2 on a 15x11 board. That literal
// reading directly contradicts the VALUELST-authored comment AND the
// existing golden-tested behaviour (cells[2][2] staying open at depth 1;
// depth 3 "all the way" cannot discriminate the two readings, since both
// exhaust a 15x11 board's 6 rings). Given the conflict, this port keeps the
// comment-and-golden-corroborated "2 * depth" rule rather than the literal
// disassembly transcription — see the audit report for the full evidence;
// flagged DEVIATION-reported, not changed.
int rings_for(int depth) {
    int max_rings = static_cast<int>(spiral().ring_end.size());
    return std::min(std::clamp(depth, 0, 3) * 2, max_rings);
}

// sub_405D0C — the actor-registry sweep sub_426818's ARM branch runs exactly
// once, the frame the walls start closing (native/src/game/batch_0x405B3A.cpp
// lines 298-325, called from batch_0x42583B.cpp line 695):
//
//   for (100 slots) if (active) { t = slot[+4]; if (t) if (t <= 1 || t == 3) active = 0; }
//
// `t` is the actor TYPE field (0=DirArrow, 1=Warphole, 2=Conveyor,
// 3=Trampoline, docs/re/stage-actors.md §1) and is unsigned, so with `t != 0`
// already established `t <= 1` is exactly `t == 1`: the sweep DEACTIVATES
// every WARPHOLE and every TRAMPOLINE and leaves dirarrows and conveyors
// alone. Clearing the slot's active flag removes the actor from BOTH consumers
// at once — the tile->actor lookup sub_405654 (so the step-on warp/bounce
// trigger, and the sliding-bomb warphole block in sub_4230A5, stop firing) and
// the per-frame animator sub_4056CA (so the art disappears). It is therefore a
// GAMEPLAY change, not a render hide.
//
// Why those two types: a player in the trampoline-hop (movement state 5) or
// warp (states 6/7) is immune to the shared kill routine sub_41DE63, which is
// the very routine the wall crush calls — leaving them live would let a player
// ride a bounce or a warp straight through a closing wall. Conveyors and
// dirarrows create no such invulnerable state and keep working.
//
// (This corrects docs/re/audit/enclosure.md Finding 0, which read
// batch_0x405B3A.cpp's own stale header comment calling dword_45E0A8 a
// "level-select broadcast table" and concluded the call was inert. It is the
// same single global the actor pool lives in — allocated by sub_404D16,
// scanned by sub_405654/sub_4056CA in batch_0x404852.cpp; globals.h line 678
// labels it "the 100-slot x 152-byte 'extra object' pool". Confirmed in-game
// by Ege on COAL MINE: the warpholes vanish the moment the walls start
// closing, whole ring-2 rectangle at once, long before the spiral could reach
// their tiles.)
void clear_hurry_disabled_actors(State& s) {
    for (int y = 0; y < kGridHeight; ++y)
        for (int x = 0; x < kGridWidth; ++x) {
            const ActorType t = s.actor_type[y][x];
            if (t == ActorType::Warphole || t == ActorType::Trampoline)
                s.actor_type[y][x] = ActorType::None;
        }
    // NOTE: warp_dest_x/y and actor_dir are deliberately left alone. The
    // original only zeroes the slot's ACTIVE dword (`*v3 = 0`); every other
    // field of the record survives untouched, and no consumer reads them
    // without first matching on the type (start_warp gates on
    // actor_type == Warphole), so retaining them is the faithful mapping.
    // An in-flight hop/warp likewise completes: sub_41F29B's states 5/6/7
    // never re-consult the registry, and neither do tick_bounce/tick_warp.
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
    // A grounded bomb on the tile detonates or is eaten — the "Stomped Bombs
    // Detonate" option (dword_464940; VALUELST id 46 seeds it, options.ini
    // stomped_bombs_detonate= / Options row 4 override it). The original's
    // stepper (sub_426818 ~27257): finds the bomb via the grounded-bomb scan
    // (sub_422E48, motion != flying/carried); ON -> sub_423209(bomb, -1)
    // queues a PROPER detonation; OFF -> sub_424841 zeroes the bomb in
    // place, no explosion, no effect. See docs/re/facts.md "Options toggles".
    for (std::size_t bi = 0; bi < s.bombs.size(); ++bi) {
        Bomb& b = s.bombs[bi];
        // Airborne bombs are exempt: sub_422E48 skips motion states 2/3
        // (flying/carried), so a bomb arcing over the tile sails on.
        if (!b.active || b.flying || b.tile_x() != wx || b.tile_y() != wy) continue;
        if (s.tuning.wall_detonates) {
            // sub_423209(bomb, -1) does NOT explode synchronously — it only
            // APPENDS to a 100-slot pending-detonation queue (dword_4621F8/
            // FC/462200). The queue is drained by sub_42331C (force-sets the
            // bomb's elapsed-fuse word +68 to its own threshold +74, so the
            // SAME call's normal fuse check detonates it), and that drain
            // only runs once per frame, gated on a frame-stamp
            // (dword_462210 != dword_464994). Per sub_42A191's per-frame
            // order, sub_4245B9 (which drains) runs BEFORE sub_426818
            // (enclosure); sub_42459A, the ONLY other sub_42331C call this
            // frame, runs AFTER enclosure but does not drain (the frame-
            // stamp gate already fired). So a bomb queued by THIS frame's
            // wall drop is not drained/detonated until the FOLLOWING frame's
            // sub_4245B9 — a confirmed one-tick defer, not an instant
            // explosion. Force our own fuse to fire on the NEXT tick_fuses()
            // pass (which runs before enclosure.update() in our own tick
            // order) instead of exploding here — the faithful equivalent.
            b.fuse = 1;
            // A FIZZLING DUD is crushed exactly like a live bomb. The dud
            // marker lives in the bomb record's state dword (sub_422EDE
            // writes 2 there), not in the motion word, so the grounded-bomb
            // finder sub_422E48 — which rejects only a zero state or motion
            // 2/3 — finds a dud just like an armed bomb, and sub_426818's
            // crush loop has no dud branch at all. On the drain side
            // (sub_42331C) only the elapsed-fuse INCREMENT is gated on
            // "state != 2"; the "elapsed >= duration" detonation test right
            // after it is UNGATED, so the queue's forced elapsed = duration
            // write detonates a dud on the next frame's pass, same as a live
            // bomb. Our tick_fuses() gates on dud_left FIRST (correctly — an
            // ordinary fuse must stay frozen while the bomb fizzles), so the
            // forced fuse would otherwise be swallowed for the rest of the
            // fizzle window (up to dud_frames = 120 ticks = 6 s) and erupt
            // from under an already-solid wall. Clear the fizzle here, on the
            // forced path only. facts.md "Enclosure wall crushes a fizzling
            // dud".
            b.dud_left = 0;
            // FIX (enclosure F1, docs/re/audit/enclosure.md Finding 1):
            // sub_426818's bomb-crush loop (native/src/game/batch_0x42583B.cpp
            // lines 769-820; pseudo.c 27262 `sub_423209((int)v6, -1)` then an
            // unconditional `goto LABEL_47`, pseudo.c 27263) queues only the
            // FIRST grounded bomb it finds on the crushed tile per drop event
            // -- there is no path back to the top of that while(1) loop on
            // the ON branch, so the search runs at most once. A second
            // grounded bomb sharing the exact same tile (structurally
            // possible: sub_422E48 is a linear scan by cell coordinate, not
            // a 1:1 per-cell grid) is left untouched by this event. Break
            // after the first match to match; the OFF branch below
            // deliberately keeps looping to exhaustion (matches
            // sub_424841's fall-through re-search, no goto LABEL_47 on that
            // path).
            break;
        } else {
            b.active = false;
            if (s.players[b.owner].bombs_placed > 0) --s.players[b.owner].bombs_placed;
        }
    }
    s.cells[wy][wx] = Cell::Solid;
    s.burning[wy][wx] = 0;
    s.flame[wy][wx] = 0;
    s.hidden[wy][wx] = PowerupType::None;
    s.floor[wy][wx] = PowerupType::None;
    for (int i = 0; i < kMaxPlayers; ++i) {
        Player& p = s.players[i];
        // The finder (sub_421D3F) excludes player-type 4 (network-spectator;
        // no local equivalent, this port has no spectator slots) from its
        // own search predicate. The bounce/warp exemption is a SEPARATE
        // guard inside the shared kill routine, sub_41DE63, that this same
        // finder feeds into: it early-outs (returns 0, no death) while the
        // victim's movement state is 5 (trampoline hop) or 6/7 (warp
        // out/in) — the SAME guard the rover/ghost landing-tile kill and the
        // ordinary flame-death funnel through (docs/re/campaign.md clause
        // 4, stage-actors.md §592). A player mid-hop or mid-warp when the
        // wall drops is untouched here too.
        if (p.present && p.alive && p.bounce == 0 && p.warp == 0 && p.tile_x() == wx &&
            p.tile_y() == wy) {
            p.alive = false;
            if (p.carrying) {
                p.carrying = false;
                if (s.players[p.carried_owner].bombs_placed > 0)
                    --s.players[p.carried_owner].bombs_placed;
            }
            // A wall-crushed player scatters its powerups too: the crush routes
            // through the SAME shared kill funnel (sub_41DE63 -> anim -> the
            // sub_41DBFE scatter) as a flame death (docs/re/facts.md "Death
            // powerup scatter"). Inert in every golden scenario (none reach the
            // wall-close phase), but faithful. PowerupSystem is a thin State&
            // wrapper — construct one locally.
            PowerupSystem{s}.death_scatter(p);
            // No attributable killer for a wall crush (event.hpp's PlayerDied
            // convention: data == -1 means "no killer", distinct from a
            // self-kill where data == the victim's own index).
            s.events.push_back({Event::Type::PlayerDied, static_cast<std::int8_t>(i),
                                static_cast<std::int8_t>(wx), static_cast<std::int8_t>(wy), -1});
        }
    }
    s.events.push_back({Event::Type::WallClosed, -1, static_cast<std::int8_t>(wx),
                        static_cast<std::int8_t>(wy), 0});
}

// CONFIRMED cadence (sub_426818, the enclosure stepper): one drop EVENT every
// 250 ms of wall clock, gated by timeGetTime() — `dword_46223C += 250`. At
// the locked 20 Hz tick rate (50 ms/tick, dword_46494C = 1000/getvalue(30))
// that is exactly ONE event per 5 ticks. It is a HARDCODED constant, NOT a
// VALUELST getvalue (the only enclosure getvalues are id 27 = depth and
// id 101 = the hurry threshold). The original's up-to-5-drops-per-frame
// catch-up (`v22 = 5`) only fires when a frame ran long; in deterministic
// lockstep every frame is 50 ms, so the cadence is a clean 5 ticks. Every
// entry in `spiral().events` — including the phantom corner repeats — is one
// such event, so this interval applies uniformly to the whole sequence. See
// docs/re/enclosure.md.
static constexpr int kEncloseIntervalTicks = 250 / (1000 / kTicksPerSecond);  // = 5

void EnclosureSystem::update() {
    State& s = s_;
    int depth = s.tuning.enclosement_depth;

    // sub_410578/sub_4105D2's dword_4601A4 is (total_ms - elapsed_ms)/1000,
    // FLOORED and clamped >= 0. Our ticks_left already IS exactly the
    // remaining ms/50 (kTicksPerSecond=20, 50 ms/tick) and is itself clamped
    // >= 0 (simulation.cpp), so a plain integer divide reproduces that floor
    // exactly, with no slop. (Comparing raw ticks_left against
    // threshold*kTicksPerSecond directly — the prior version of this code —
    // is NOT equivalent: it silently rounds the wrong way, firing the banner
    // one tick early and the wall-arm up to 19 ticks late. Flooring first
    // and comparing whole seconds, like the original does, avoids both.)
    const int seconds_left = s.ticks_left / kTicksPerSecond;

    // TWO distinct moments in the original, kept separate here:
    //  1. The "HURRY!" banner + voice callout: remaining < getvalue(101)
    //     STRICT (the HUD block ~29533, `sub_410578() [saved via edx across
    //     the getvalue call, Watcom's register calling convention] < getvalue
    //     (101)`, latched on dword_464984, sub_427961(2700)). `s.hurry` + the
    //     Hurry EVENT model this — the presentation (banner, sound_director)
    //     rides the event, so it fires at this moment.
    //  2. The walls actually START closing: remaining <= getvalue(101) - 5,
    //     NOT strict (the enclosure stepper sub_426818 ~27170 arms on
    //     `sub_410578() <= getvalue(101) - 5`). The tile DROPS are gated on
    //     this — 5 s after the banner window OPENS, not after it closes (the
    //     banner's own window is remaining ∈ (getvalue(101)-5, getvalue(101)),
    //     4 whole seconds wide from the OTHER half of the same HUD check,
    //     `sub_410578() > getvalue(101) - 5` STRICT — so the banner turns off
    //     exactly as the walls arm, no gap, no overlap).
    // Conflating the two (drop at moment 1) closed the walls 5 s too early;
    // this decouples them. See docs/re/enclosure.md §2.
    //
    // NO `ticks_left > 0` guard: sub_410578's remaining-seconds is CLAMPED to
    // >= 0 (never negative), so once the threshold predicate goes true it
    // stays true forever — the original keeps closing walls through and past
    // TimeUp (sudden death), it never freezes the spiral. Our ticks_left
    // similarly floors at 0 (simulation.cpp's `if (ticks_left > 0)
    // --ticks_left`), so dropping the guard here is the direct, monotonic
    // equivalent — gating on ticks_left > 0 instead froze enclose_index the
    // instant the match clock hit zero, stranding the spiral mid-ring on any
    // match that runs out of time before all rings close.
    const bool warn = seconds_left < s.tuning.hurry_seconds;
    const bool closing = seconds_left <= s.tuning.hurry_seconds - 5;

    // Moment 1: fire the banner/sound once (edge on `hurry`).
    if (!s.hurry && warn) {
        s.hurry = true;
        s.events.push_back({Event::Type::Hurry, -1, -1, -1, 0});
    }

    // Moment 2: arm the drop machinery once, on the first tick the walls close.
    // enclose_interval stays 0 until then, so it doubles as the "armed" flag.
    if (closing && s.enclose_interval == 0) {
        s.enclose_interval = kEncloseIntervalTicks;
        s.enclose_timer = s.enclose_interval;
        s.enclose_index = 0;
        // sub_405D0C: warpholes and trampolines are switched off for the rest
        // of the round, globally, on this one edge. NOT depth-gated — the
        // original runs it inside the `if (!dword_45BE9C)` arm block, which
        // sits ABOVE the `2*enclosement_depth > ring` drop gate, so even
        // enclosement_depth = 0 (walls never actually close) still kills them.
        clear_hurry_disabled_actors(s);
        return;  // NO drop on the arm tick (original: dword_46223C==now, gate shut);
                 // the first wall lands exactly one interval (5 ticks) later.
    }

    if (closing && s.enclose_interval > 0 && depth > 0 && s.enclose_index < total(depth) &&
        --s.enclose_timer <= 0) {
        s.enclose_timer = s.enclose_interval;
        int wx = 0, wy = 0;
        if (position(s.enclose_index++, depth, &wx, &wy)) drop_wall(wx, wy);
    }
}

}  // namespace bomber::sim
