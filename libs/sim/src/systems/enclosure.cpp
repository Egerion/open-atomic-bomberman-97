#include "systems/enclosure.hpp"

#include <algorithm>
#include <utility>
#include <vector>

#include "systems/flames.hpp"

namespace bomber::sim {
namespace {

// Clockwise ring order starting top-left, outermost ring first.
const std::vector<std::pair<int, int>>& enclose_order() {
    static const std::vector<std::pair<int, int>> order = [] {
        std::vector<std::pair<int, int>> o;
        int rings = std::min(kGridWidth, kGridHeight) / 2 + 1;
        for (int r = 0; r < rings; ++r) {
            int x0 = r, x1 = kGridWidth - 1 - r, y0 = r, y1 = kGridHeight - 1 - r;
            if (x0 > x1 || y0 > y1) break;
            for (int x = x0; x <= x1; ++x) o.emplace_back(x, y0);
            for (int y = y0 + 1; y <= y1; ++y) o.emplace_back(x1, y);
            if (y1 > y0)
                for (int x = x1 - 1; x >= x0; --x) o.emplace_back(x, y1);
            if (x1 > x0)
                for (int y = y1 - 1; y > y0; --y) o.emplace_back(x0, y);
        }
        return o;
    }();
    return order;
}

// Depth setting -> number of rings closed (id 27: 1 = 2 rings, 3 = all).
int enclose_rings(int depth) {
    if (depth <= 0) return 0;
    if (depth >= 3) return 6;
    return depth * 2;
}

}  // namespace

int EnclosureSystem::total(int depth) {
    int rings = enclose_rings(depth);
    int n = 0;
    for (int r = 0; r < rings; ++r) {
        int w = kGridWidth - 2 * r, h = kGridHeight - 2 * r;
        if (w <= 0 || h <= 0) break;
        n += (h <= 1) ? w : (w <= 1 ? h : 2 * w + 2 * h - 4);
    }
    return std::min<int>(n, static_cast<int>(enclose_order().size()));
}

bool EnclosureSystem::position(int index, int depth, int* x, int* y) {
    if (index < 0 || index >= total(depth)) return false;
    *x = enclose_order()[static_cast<std::size_t>(index)].first;
    *y = enclose_order()[static_cast<std::size_t>(index)].second;
    return true;
}

void EnclosureSystem::drop_wall(int wx, int wy) {
    State& s = s_;
    // A bomb on the tile detonates or is eaten, per VALUELST id 46.
    for (std::size_t bi = 0; bi < s.bombs.size(); ++bi) {
        Bomb& b = s.bombs[bi];
        if (!b.active || b.tile_x() != wx || b.tile_y() != wy) continue;
        if (s.tuning.wall_detonates) {
            flames_.explode(bi);
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
        if (p.present && p.alive && p.tile_x() == wx && p.tile_y() == wy) {
            p.alive = false;
            if (p.carrying) {
                p.carrying = false;
                if (s.players[p.carried_owner].bombs_placed > 0)
                    --s.players[p.carried_owner].bombs_placed;
            }
            s.events.push_back({Event::Type::PlayerDied, static_cast<std::int8_t>(i),
                                static_cast<std::int8_t>(wx), static_cast<std::int8_t>(wy), 0});
        }
    }
    s.events.push_back({Event::Type::WallClosed, -1, static_cast<std::int8_t>(wx),
                        static_cast<std::int8_t>(wy), 0});
}

// CONFIRMED cadence (sub_426818, the enclosure stepper): one wall tile drops
// every 250 ms of wall clock, gated by timeGetTime() — `dword_46223C += 250`.
// At the locked 20 Hz tick rate (50 ms/tick, dword_46494C = 1000/getvalue(30))
// that is exactly ONE wall per 5 ticks. It is a HARDCODED constant, NOT a
// VALUELST getvalue (the only enclosure getvalues are id 27 = depth and
// id 101 = the hurry threshold). The original's up-to-5-drops-per-frame
// catch-up (`v22 = 5`) only fires when a frame ran long; in deterministic
// lockstep every frame is 50 ms, so the cadence is a clean 5 ticks. See
// docs/re/enclosure.md.
static constexpr int kEncloseIntervalTicks = 250 / (1000 / kTicksPerSecond);  // = 5

void EnclosureSystem::update() {
    State& s = s_;
    int depth = s.tuning.enclosement_depth;

    // TWO distinct moments in the original, kept separate here:
    //  1. The "HURRY!" banner + voice callout: remaining <= getvalue(101)
    //     (the HUD block ~29533, latched on dword_464984, sub_427961(2700)).
    //     `s.hurry` + the Hurry EVENT model this — the presentation (banner,
    //     sound_director) rides the event, so it fires at this moment.
    //  2. The walls actually START closing: remaining <= getvalue(101) - 5, i.e.
    //     5 s LATER (the enclosure stepper sub_426818 ~27170 arms on
    //     `sub_410578() <= getvalue(101) - 5`). The tile DROPS are gated on this.
    // Conflating the two (drop at moment 1) closed the walls 5 s too early; this
    // decouples them. ticks_left/20 is our seconds-remaining; the -5/-0 are
    // whole-second offsets in tick units. See docs/re/enclosure.md §2.
    //
    // NO `ticks_left > 0` guard: sub_410578's remaining-seconds is CLAMPED to >=
    // 0 (never negative), so once the threshold predicate goes true it stays
    // true forever — the original keeps closing walls through and past TimeUp
    // (sudden death), it never freezes the spiral. Our ticks_left similarly
    // floors at 0 (simulation.cpp's `if (ticks_left > 0) --ticks_left`), so
    // dropping the guard here is the direct, monotonic equivalent — gating on
    // ticks_left > 0 instead froze enclose_index the instant the match clock
    // hit zero, stranding the spiral mid-ring on any match that runs out of
    // time before all rings close.
    const bool warn = s.ticks_left <= s.tuning.hurry_seconds * kTicksPerSecond;
    const bool closing = s.ticks_left <= (s.tuning.hurry_seconds - 5) * kTicksPerSecond;

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
