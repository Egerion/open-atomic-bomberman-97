#pragma once

// Exhaustive MatchConfig scaffolding shared by the bomber::net suites that care
// about the wire codec (test_match_config_codec.cpp, test_setup_session.cpp).
//
// Two halves, and BOTH must name every gameplay field of sim::MatchConfig:
//   * distinctive_match_config() fills each field with a UNIQUE, non-default
//     value drawn from one increasing counter, so a codec that skips a field,
//     or that swaps two fields of the same type, cannot round-trip cleanly.
//   * compare_match_config() reports, per named field, whether two configs
//     agree — the field-by-field comparator the round-trip test asserts on
//     (sim::MatchConfig has no operator==, and adding one just to satisfy a
//     test would weaken the struct).
//
// KEEP IN SYNC with libs/sim/include/bomber/sim/match_config.hpp. A field that
// is missing from BOTH this file and libs/net's codec is the single hole these
// tests cannot see; the codec's own encoded-size assertion is the tripwire for
// the other direction.

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "bomber/sim/match_config.hpp"

namespace bomber::sim::test {

// One strictly increasing source for every filled value. Starts well above the
// largest VALUELST default (1300), so no field can accidentally keep its
// default, and never repeats, so no two int fields can be confused.
class ConfigFiller {
public:
    std::int32_t next() { return n_++; }
    std::uint8_t next_u8() { return static_cast<std::uint8_t>(next() & 0xFF); }

private:
    std::int32_t n_ = 5000;
};

inline Tuning distinctive_tuning(ConfigFiller& f) {
    Tuning t;
    t.input_freeze_ticks = f.next();
    t.fuse_frames = f.next();
    t.start_speed = f.next();
    t.skate_speed_bonus = f.next();
    t.clogs_speed_penalty = f.next();
    t.kicked_bomb_speed = f.next();
    t.punched_bomb_speed = f.next();
    t.game_seconds = f.next();
    t.taunt_chance = f.next();
    t.hurry_seconds = f.next();
    t.overpowered_relocate_seconds = f.next();
    t.enclosement_depth = f.next();
    t.wall_detonates = f.next();
    for (auto& v : t.start_with) v = f.next();
    for (auto& v : t.limits) v = f.next();
    for (auto& v : t.spawn_counts) v = f.next();
    for (auto& row : t.color_rgb)
        for (auto& v : row) v = f.next();
    for (auto& v : t.level_enabled) v = f.next();
    t.level_index = f.next();
    for (auto& v : t.regen_seconds) v = f.next();
    t.regen_clear_radius = f.next();
    for (auto& v : t.ice_delay_ms) v = f.next();
    t.pickup_pause = f.next();
    t.powers_lost_min = f.next();
    t.powers_lost_rand = f.next();
    t.head_stun_frames = f.next();
    t.punch_arc_first = f.next();
    t.punch_arc_hop = f.next();
    t.taunt_many_bombs = f.next();
    t.taunt_many_chance = f.next();
    t.jelly_turn_chance = f.next();
    t.dud_gate_base = f.next();
    t.dud_gate_rand = f.next();
    t.dud_chance = f.next();
    t.dud_frames = f.next();
    t.flame_frames = f.next();
    t.brick_burn_frames = f.next();
    t.conveyor_speed_count = f.next();
    for (auto& v : t.conveyor_speeds) v = f.next();
    t.conveyor_speed_index = f.next();
    t.trampoline_bounce_frames = f.next();
    t.ai_personalities = f.next();
    t.fire_god_lookahead = f.next();
    t.ai_blast_chance = f.next();
    t.ai_powerup_range = f.next();
    for (auto& v : t.disease_frames) v = f.next();
    t.disease_freshness = f.next();
    t.disease_cure_chance = f.next();
    // Every one of these defaults to true, so false is the distinctive value.
    t.diseases_time_limited = false;
    t.diseases_multiply = false;
    t.diseases_curable = false;
    t.diseases_destroyable = false;
    t.rover_turn_chance = f.next();
    t.campaign_ai_kill_score = f.next();
    t.rover_kill_score = f.next();
    t.ghost_kill_score = f.next();
    return t;
}

// A config in which EVERY field differs from a default-constructed one. Values
// are deliberately not all sim-legal (level_index far past the 11 stages, say):
// the codec must move bytes faithfully, not clamp them.
inline MatchConfig distinctive_match_config() {
    ConfigFiller f;
    MatchConfig c;

    for (auto& row : c.cells)
        for (auto& cell : row) cell = static_cast<Cell>(f.next() % 3);
    // Include the None sentinel (255) in the mix so it round-trips too.
    const ActorType kinds[5] = {ActorType::DirArrow, ActorType::Warphole, ActorType::Conveyor,
                                ActorType::Trampoline, ActorType::None};
    for (auto& row : c.actor_type)
        for (auto& a : row) a = kinds[f.next() % 5];
    for (auto& row : c.actor_dir)
        for (auto& d : row) d = f.next_u8();
    for (auto& row : c.warp_dest_x)
        for (auto& d : row) d = f.next_u8();
    for (auto& row : c.warp_dest_y)
        for (auto& d : row) d = f.next_u8();

    c.spawns.resize(kMaxPlayers);
    for (auto& sp : c.spawns) {
        sp.x = f.next();
        sp.y = f.next();
    }
    c.player_count = f.next();
    for (int i = 0; i < kMaxPlayers; ++i) {
        c.ai[static_cast<std::size_t>(i)] = (i % 2) == 0;      // default: all false
        c.active[static_cast<std::size_t>(i)] = (i % 3) != 0;  // default: all true
        c.team[static_cast<std::size_t>(i)] = f.next_u8();     // default: all 0
    }
    c.seed = 0xDEADBEEFu;
    c.tuning = distinctive_tuning(f);
    for (auto& v : c.spawn_override) v = f.next();
    for (int i = 0; i < kPowerupKinds; ++i) {
        c.forbidden[static_cast<std::size_t>(i)] = (i % 2) == 0;
        c.born_with[static_cast<std::size_t>(i)] = (i % 3) == 0;
    }
    for (int s = 0; s < kMaxPlayers; ++s)
        for (int i = 0; i < kPowerupKinds; ++i)
            c.born_with_extra[static_cast<std::size_t>(s)][static_cast<std::size_t>(i)] =
                ((s + i) % 2) == 0;
    for (auto& v : c.born_with_clogs) v = f.next();
    c.campaign_rovers = f.next();
    c.campaign_rover_speed = f.next();
    c.campaign_ghosts = f.next();
    c.campaign_ghost_speed = f.next();
    return c;
}

// --- the field-by-field comparator -------------------------------------------

namespace detail {

template <class T>
bool field_eq(const T& x, const T& y) {
    return x == y;
}
template <class T, std::size_t N>
bool field_eq(const T (&x)[N], const T (&y)[N]) {
    for (std::size_t i = 0; i < N; ++i)
        if (!field_eq(x[i], y[i])) return false;
    return true;
}
inline bool field_eq(const std::vector<SpawnPoint>& x, const std::vector<SpawnPoint>& y) {
    if (x.size() != y.size()) return false;
    for (std::size_t i = 0; i < x.size(); ++i)
        if (x[i].x != y[i].x || x[i].y != y[i].y) return false;
    return true;
}

}  // namespace detail

using FieldReport = std::vector<std::pair<std::string, bool>>;

// Every gameplay field of MatchConfig, named, with whether a and b agree on it.
inline FieldReport compare_match_config(const MatchConfig& a, const MatchConfig& b) {
    FieldReport out;
#define BOMBER_CMP(path) out.emplace_back(#path, detail::field_eq(a.path, b.path))
    BOMBER_CMP(cells);
    BOMBER_CMP(actor_type);
    BOMBER_CMP(actor_dir);
    BOMBER_CMP(warp_dest_x);
    BOMBER_CMP(warp_dest_y);
    BOMBER_CMP(spawns);
    BOMBER_CMP(player_count);
    BOMBER_CMP(ai);
    BOMBER_CMP(active);
    BOMBER_CMP(team);
    BOMBER_CMP(seed);
    BOMBER_CMP(spawn_override);
    BOMBER_CMP(forbidden);
    BOMBER_CMP(born_with);
    BOMBER_CMP(born_with_extra);
    BOMBER_CMP(born_with_clogs);
    BOMBER_CMP(campaign_rovers);
    BOMBER_CMP(campaign_rover_speed);
    BOMBER_CMP(campaign_ghosts);
    BOMBER_CMP(campaign_ghost_speed);
    BOMBER_CMP(tuning.input_freeze_ticks);
    BOMBER_CMP(tuning.fuse_frames);
    BOMBER_CMP(tuning.start_speed);
    BOMBER_CMP(tuning.skate_speed_bonus);
    BOMBER_CMP(tuning.clogs_speed_penalty);
    BOMBER_CMP(tuning.kicked_bomb_speed);
    BOMBER_CMP(tuning.punched_bomb_speed);
    BOMBER_CMP(tuning.game_seconds);
    BOMBER_CMP(tuning.taunt_chance);
    BOMBER_CMP(tuning.hurry_seconds);
    BOMBER_CMP(tuning.overpowered_relocate_seconds);
    BOMBER_CMP(tuning.enclosement_depth);
    BOMBER_CMP(tuning.wall_detonates);
    BOMBER_CMP(tuning.start_with);
    BOMBER_CMP(tuning.limits);
    BOMBER_CMP(tuning.spawn_counts);
    BOMBER_CMP(tuning.color_rgb);
    BOMBER_CMP(tuning.level_enabled);
    BOMBER_CMP(tuning.level_index);
    BOMBER_CMP(tuning.regen_seconds);
    BOMBER_CMP(tuning.regen_clear_radius);
    BOMBER_CMP(tuning.ice_delay_ms);
    BOMBER_CMP(tuning.pickup_pause);
    BOMBER_CMP(tuning.powers_lost_min);
    BOMBER_CMP(tuning.powers_lost_rand);
    BOMBER_CMP(tuning.head_stun_frames);
    BOMBER_CMP(tuning.punch_arc_first);
    BOMBER_CMP(tuning.punch_arc_hop);
    BOMBER_CMP(tuning.taunt_many_bombs);
    BOMBER_CMP(tuning.taunt_many_chance);
    BOMBER_CMP(tuning.jelly_turn_chance);
    BOMBER_CMP(tuning.dud_gate_base);
    BOMBER_CMP(tuning.dud_gate_rand);
    BOMBER_CMP(tuning.dud_chance);
    BOMBER_CMP(tuning.dud_frames);
    BOMBER_CMP(tuning.flame_frames);
    BOMBER_CMP(tuning.brick_burn_frames);
    BOMBER_CMP(tuning.conveyor_speed_count);
    BOMBER_CMP(tuning.conveyor_speeds);
    BOMBER_CMP(tuning.conveyor_speed_index);
    BOMBER_CMP(tuning.trampoline_bounce_frames);
    BOMBER_CMP(tuning.ai_personalities);
    BOMBER_CMP(tuning.fire_god_lookahead);
    BOMBER_CMP(tuning.ai_blast_chance);
    BOMBER_CMP(tuning.ai_powerup_range);
    BOMBER_CMP(tuning.disease_frames);
    BOMBER_CMP(tuning.disease_freshness);
    BOMBER_CMP(tuning.disease_cure_chance);
    BOMBER_CMP(tuning.diseases_time_limited);
    BOMBER_CMP(tuning.diseases_multiply);
    BOMBER_CMP(tuning.diseases_curable);
    BOMBER_CMP(tuning.diseases_destroyable);
    BOMBER_CMP(tuning.rover_turn_chance);
    BOMBER_CMP(tuning.campaign_ai_kill_score);
    BOMBER_CMP(tuning.rover_kill_score);
    BOMBER_CMP(tuning.ghost_kill_score);
#undef BOMBER_CMP
    return out;
}

// The fields on which a and b DISAGREE, by name (empty == fully equal).
inline std::vector<std::string> match_config_diffs(const MatchConfig& a, const MatchConfig& b) {
    std::vector<std::string> out;
    for (const auto& [name, equal] : compare_match_config(a, b))
        if (!equal) out.push_back(name);
    return out;
}

// The fields on which a and b AGREE, by name. Used to prove the filler above
// really touched everything the comparator knows about.
inline std::vector<std::string> match_config_matches(const MatchConfig& a, const MatchConfig& b) {
    std::vector<std::string> out;
    for (const auto& [name, equal] : compare_match_config(a, b))
        if (equal) out.push_back(name);
    return out;
}

}  // namespace bomber::sim::test
