// Determinism rule 4, mechanically: EVERY gameplay field of State / Player /
// Bomb / Brain / Rover must change state_hash(). One `covered` line per field,
// each perturbing exactly that field on an otherwise identical populated state.
//
// WHY THIS EXISTS RATHER THAN A READING OF hash.cpp. The gap that prompted it
// was not subtle — nine fields were missing, four of them on Bomb — and it
// survived because the only check was a human comparing two files, while
// hash.cpp's packed words documented their UNUSED BITS to the bit. Reading
// proves nothing repeatably; this fails the build the next time a field is added
// to a struct without being mixed, which is the only form of the check that
// keeps working after everyone who remembers the sweep has moved on.
//
// A FAILURE HERE IS NOT A GOLDEN FAILURE. Golden hashes moving means behaviour
// changed; a line here going red means a field exists that two peers can
// disagree about while hashing identically — the desync detector is blind to it
// and the lobby door lets the mismatch through.
//
// The exclusions at the bottom are pinned the same way, asserting the digest
// does NOT move: they are the register in hash.cpp's header, and a field
// wrongly added to the hash would fail there instead.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <string>

#include "bomber/sim/simulation.hpp"

using namespace bomber::sim;

namespace {

// A state with every field at a distinctive NON-DEFAULT value, so a perturbation
// is always a real change, and with the two conditional blocks in hash.cpp
// actually entered: players[0] is carrying (the carried-bomb payload) and
// bombs[0] is flying (the airborne leg).
State populated() {
    State s;
    s.tick = 100;
    s.rng = 0xABCDEF01u;
    s.ticks_left = 500;
    s.input_freeze = 3;
    s.hurry = true;
    s.enclose_index = 4;
    s.enclose_timer = 2;
    s.enclose_interval = 5;
    s.regen_timer = 7;
    s.dud_gate = 33;
    s.next_bomb_id = 9;
    s.campaign_hazards_active = true;
    s.hazard_clear_timer = 6;

    s.cells[1][2] = Cell::Brick;
    s.actor_type[1][2] = ActorType::Conveyor;
    s.actor_dir[1][2] = 1;
    s.warp_dest_x[1][2] = 3;
    s.warp_dest_y[1][2] = 4;
    s.hidden[1][2] = PowerupType::Kick;
    s.floor[1][3] = PowerupType::Flame;
    s.flame[2][2] = 5;
    s.flame_owner[2][2] = 1;
    s.flame_colour[2][2] = 2;
    s.flame_kind[2][2] = FlameKind::MidEast;
    s.burning[2][3] = 4;

    s.pending_chain.push_back({7u, static_cast<std::int8_t>(2)});

    Player& p = s.players[0];
    p.present = true;
    p.alive = true;
    p.ai = true;
    p.team = 2;
    p.x = 1234;
    p.y = 5678;
    p.facing = Direction::Down;
    p.speed = 40;
    p.move_budget = 25;
    p.max_bombs = 3;
    p.bombs_placed = 2;
    p.flame = 4;
    p.goldflame = true;
    p.skates = 2;
    p.clogs = 1;
    p.kick = true;
    p.punch = true;
    p.grab = true;
    p.spooge = true;
    p.trigger = true;
    p.trigger_placed = 1;
    p.stun = 8;
    p.pickup_pause = 3;
    p.bounce = 15;
    p.warp = 9;
    p.warp_to_x = 6;
    p.warp_to_y = 7;
    p.carrying = true;
    p.carried_fuse = 120;
    p.carried_flame = 3;
    p.carried_jelly = true;
    p.carried_trigger = true;
    p.carried_owner = 1;
    p.carried_colour = 2;
    p.jelly = true;
    p.prev_action1 = true;
    p.prev_action2 = true;
    p.disease[0] = true;
    p.disease_timer = 60;
    p.disease_fresh = 20;
    p.ice_history[0] = 2;
    p.ice_history[17] = 3;

    Brain& br = s.brains[0];
    br.personality = 1;
    br.state_flag = 9;
    br.has_path_target = true;
    br.path_target_x = 5;
    br.path_target_y = 6;
    br.path_target_cost = 400;
    br.wander_dir = 2;
    br.pow_seek.active = true;
    br.pow_seek.timer = 100;
    br.pow_seek.tile_x = 3;
    br.pow_seek.tile_y = 4;
    br.pow_seek.step_dir = 1;
    br.enemy_seek.active = true;
    br.enemy_seek.timer = 150;
    br.enemy_seek.target_slot = 1;
    br.enemy_seek.step_dir = 3;

    Bomb b;
    b.active = true;
    b.id = 11;
    b.owner = 1;
    b.colour = 2;
    b.x = 4000;
    b.y = 3600;
    b.fuse = 50;
    b.fuse_init = 60;
    b.created_tick = 90;
    b.dud_left = 5;
    b.flame = 3;
    b.jelly = true;
    b.trigger = true;
    b.moving = true;
    b.stop_pending = true;
    b.dir = Direction::Up;
    b.flying = true;
    b.fly_ticks = 4;
    b.fly_total = 8;
    b.fly_arc = 65;
    b.from_x = 4000;
    b.from_y = 3600;
    b.to_x = 8000;
    b.to_y = 7200;
    s.bombs.push_back(b);

    Rover r;
    r.alive = true;
    r.kind = RoverKind::Rover;
    r.x = 2000;
    r.y = 1800;
    r.dir = 1;
    r.speed = 30;
    r.move_budget = 12;
    r.anim_step = 7;
    s.rovers.push_back(r);

    return s;
}

// Perturb one field on a fresh copy; the digest MUST move.
template <class F>
void covered(const char* field, F&& perturb) {
    State s = populated();
    const std::uint64_t before = state_hash(s);
    perturb(s);
    const std::string msg = std::string("state_hash ignores ") + field;
    CHECK_MESSAGE(state_hash(s) != before, msg);
}

// The exclusion register in hash.cpp's header; the digest must NOT move.
template <class F>
void excluded(const char* field, F&& perturb) {
    State s = populated();
    const std::uint64_t before = state_hash(s);
    perturb(s);
    const std::string msg = std::string("state_hash now mixes ") + field;
    CHECK_MESSAGE(state_hash(s) == before, msg);
}

}  // namespace

TEST_CASE("state_hash covers every State scalar") {
    covered("tick", [](State& s) { ++s.tick; });
    covered("rng", [](State& s) { s.rng = 0x11112222u; });
    covered("ticks_left", [](State& s) { s.ticks_left = 501; });
    covered("input_freeze", [](State& s) { s.input_freeze = 4; });
    covered("hurry", [](State& s) { s.hurry = false; });
    covered("enclose_index", [](State& s) { s.enclose_index = 5; });
    covered("enclose_timer", [](State& s) { s.enclose_timer = 3; });
    // The field this suite was written for: EnclosureSystem::update's armed flag.
    covered("enclose_interval", [](State& s) { s.enclose_interval = 6; });
    covered("regen_timer", [](State& s) { s.regen_timer = 8; });
    covered("dud_gate", [](State& s) { s.dud_gate = 34; });
    covered("next_bomb_id", [](State& s) { s.next_bomb_id = 10; });
    covered("campaign_hazards_active", [](State& s) { s.campaign_hazards_active = false; });
    covered("hazard_clear_timer", [](State& s) { s.hazard_clear_timer = 7; });
}

TEST_CASE("state_hash covers every State grid and container") {
    covered("cells", [](State& s) { s.cells[1][2] = Cell::Solid; });
    covered("actor_type", [](State& s) { s.actor_type[1][2] = ActorType::DirArrow; });
    covered("actor_dir", [](State& s) { s.actor_dir[1][2] = 2; });
    covered("warp_dest_x", [](State& s) { s.warp_dest_x[1][2] = 4; });
    covered("warp_dest_y", [](State& s) { s.warp_dest_y[1][2] = 5; });
    covered("hidden", [](State& s) { s.hidden[1][2] = PowerupType::Punch; });
    covered("floor", [](State& s) { s.floor[1][3] = PowerupType::Skate; });
    covered("flame", [](State& s) { s.flame[2][2] = 6; });
    covered("flame_owner", [](State& s) { s.flame_owner[2][2] = 2; });
    covered("flame_colour", [](State& s) { s.flame_colour[2][2] = 3; });
    covered("flame_kind", [](State& s) { s.flame_kind[2][2] = FlameKind::Center; });
    covered("burning", [](State& s) { s.burning[2][3] = 5; });
    covered("pending_chain length", [](State& s) { s.pending_chain.push_back({8u, 0}); });
    covered("pending_chain.bomb_id", [](State& s) { s.pending_chain[0].bomb_id = 8; });
    covered("pending_chain.skip_dir", [](State& s) { s.pending_chain[0].skip_dir = 3; });
    covered("bombs length", [](State& s) { s.bombs.push_back(Bomb{}); });
    covered("rovers length", [](State& s) { s.rovers.push_back(Rover{}); });
}

TEST_CASE("state_hash covers every Player field") {
    covered("present", [](State& s) { s.players[0].present = false; });
    covered("alive", [](State& s) { s.players[0].alive = false; });
    covered("ai", [](State& s) { s.players[0].ai = false; });
    covered("team", [](State& s) { s.players[0].team = 3; });
    covered("x", [](State& s) { s.players[0].x = 1235; });
    covered("y", [](State& s) { s.players[0].y = 5679; });
    covered("facing", [](State& s) { s.players[0].facing = Direction::Right; });
    covered("speed", [](State& s) { s.players[0].speed = 41; });
    covered("move_budget", [](State& s) { s.players[0].move_budget = 26; });
    covered("max_bombs", [](State& s) { s.players[0].max_bombs = 4; });
    covered("bombs_placed", [](State& s) { s.players[0].bombs_placed = 1; });
    covered("flame", [](State& s) { s.players[0].flame = 5; });
    covered("goldflame", [](State& s) { s.players[0].goldflame = false; });
    covered("skates", [](State& s) { s.players[0].skates = 3; });
    covered("clogs", [](State& s) { s.players[0].clogs = 2; });
    covered("kick", [](State& s) { s.players[0].kick = false; });
    covered("punch", [](State& s) { s.players[0].punch = false; });
    covered("grab", [](State& s) { s.players[0].grab = false; });
    covered("spooge", [](State& s) { s.players[0].spooge = false; });
    covered("trigger", [](State& s) { s.players[0].trigger = false; });
    covered("trigger_placed", [](State& s) { s.players[0].trigger_placed = 2; });
    covered("stun", [](State& s) { s.players[0].stun = 9; });
    covered("pickup_pause", [](State& s) { s.players[0].pickup_pause = 4; });
    covered("bounce", [](State& s) { s.players[0].bounce = 16; });
    covered("warp", [](State& s) { s.players[0].warp = 10; });
    covered("warp_to_x", [](State& s) { s.players[0].warp_to_x = 7; });
    covered("warp_to_y", [](State& s) { s.players[0].warp_to_y = 8; });
    covered("carrying", [](State& s) { s.players[0].carrying = false; });
    covered("jelly", [](State& s) { s.players[0].jelly = false; });
    covered("prev_action1", [](State& s) { s.players[0].prev_action1 = false; });
    covered("prev_action2", [](State& s) { s.players[0].prev_action2 = false; });
    covered("disease[]", [](State& s) { s.players[0].disease[1] = true; });
    covered("disease_timer", [](State& s) { s.players[0].disease_timer = 61; });
    covered("disease_fresh", [](State& s) { s.players[0].disease_fresh = 21; });
    covered("ice_history[0]", [](State& s) { s.players[0].ice_history[0] = 1; });
    // The buffer is packed 8 slots to a word; index 17 lands in the third one,
    // so a whole-array walk is not needed to prove every word is reached.
    covered("ice_history[17]", [](State& s) { s.players[0].ice_history[17] = 1; });
}

TEST_CASE("state_hash covers the carried-bomb payload") {
    // Gated on `carrying` in hash.cpp, so populated() sets it; a grounded player
    // holds a stale payload no reader consults.
    covered("carried_fuse", [](State& s) { s.players[0].carried_fuse = 121; });
    covered("carried_flame", [](State& s) { s.players[0].carried_flame = 4; });
    covered("carried_jelly", [](State& s) { s.players[0].carried_jelly = false; });
    covered("carried_trigger", [](State& s) { s.players[0].carried_trigger = false; });
    covered("carried_owner", [](State& s) { s.players[0].carried_owner = 2; });
    covered("carried_colour", [](State& s) { s.players[0].carried_colour = 3; });
}

TEST_CASE("state_hash covers every Bomb field") {
    covered("active", [](State& s) { s.bombs[0].active = false; });
    covered("id", [](State& s) { s.bombs[0].id = 12; });
    covered("owner", [](State& s) { s.bombs[0].owner = 2; });
    covered("colour", [](State& s) { s.bombs[0].colour = 3; });
    covered("x", [](State& s) { s.bombs[0].x = 4001; });
    covered("y", [](State& s) { s.bombs[0].y = 3601; });
    covered("fuse", [](State& s) { s.bombs[0].fuse = 51; });
    covered("fuse_init", [](State& s) { s.bombs[0].fuse_init = 61; });
    covered("created_tick", [](State& s) { s.bombs[0].created_tick = 91; });
    covered("dud_left", [](State& s) { s.bombs[0].dud_left = 6; });
    covered("flame", [](State& s) { s.bombs[0].flame = 4; });
    covered("jelly", [](State& s) { s.bombs[0].jelly = false; });
    covered("trigger", [](State& s) { s.bombs[0].trigger = false; });
    covered("moving", [](State& s) { s.bombs[0].moving = false; });
    covered("stop_pending", [](State& s) { s.bombs[0].stop_pending = false; });
    covered("dir", [](State& s) { s.bombs[0].dir = Direction::Left; });
    covered("flying", [](State& s) { s.bombs[0].flying = false; });
}

TEST_CASE("state_hash covers the airborne leg") {
    // Gated on `flying`, as the carried payload is gated on `carrying`.
    covered("fly_ticks", [](State& s) { s.bombs[0].fly_ticks = 5; });
    covered("fly_total", [](State& s) { s.bombs[0].fly_total = 9; });
    covered("fly_arc", [](State& s) { s.bombs[0].fly_arc = 66; });
    covered("from_x", [](State& s) { s.bombs[0].from_x = 4001; });
    covered("from_y", [](State& s) { s.bombs[0].from_y = 3601; });
    covered("to_x", [](State& s) { s.bombs[0].to_x = 8001; });
    covered("to_y", [](State& s) { s.bombs[0].to_y = 7201; });
    // fly_ticks and fly_total used to share one word with a 6-bit mask on
    // fly_ticks; a leg longer than 63 ticks (VALUELST id 301 tuned down) aliased
    // silently. Both now get 32 bits, so the high bits have to discriminate.
    covered("fly_ticks past the old 6-bit mask", [](State& s) {
        s.bombs[0].fly_ticks = 4 + 64;
        s.bombs[0].fly_total = 200;
    });
}

TEST_CASE("state_hash covers every Brain field") {
    covered("personality", [](State& s) { s.brains[0].personality = 2; });
    covered("state_flag", [](State& s) { s.brains[0].state_flag = 8; });
    covered("has_path_target", [](State& s) { s.brains[0].has_path_target = false; });
    covered("path_target_x", [](State& s) { s.brains[0].path_target_x = 6; });
    covered("path_target_y", [](State& s) { s.brains[0].path_target_y = 7; });
    covered("path_target_cost", [](State& s) { s.brains[0].path_target_cost = 401; });
    covered("wander_dir", [](State& s) { s.brains[0].wander_dir = 3; });
    covered("pow_seek.active", [](State& s) { s.brains[0].pow_seek.active = false; });
    covered("pow_seek.timer", [](State& s) { s.brains[0].pow_seek.timer = 101; });
    covered("pow_seek.tile_x", [](State& s) { s.brains[0].pow_seek.tile_x = 4; });
    covered("pow_seek.tile_y", [](State& s) { s.brains[0].pow_seek.tile_y = 5; });
    covered("pow_seek.step_dir", [](State& s) { s.brains[0].pow_seek.step_dir = 2; });
    covered("enemy_seek.active", [](State& s) { s.brains[0].enemy_seek.active = false; });
    covered("enemy_seek.timer", [](State& s) { s.brains[0].enemy_seek.timer = 151; });
    covered("enemy_seek.target_slot", [](State& s) { s.brains[0].enemy_seek.target_slot = 2; });
    covered("enemy_seek.step_dir", [](State& s) { s.brains[0].enemy_seek.step_dir = 2; });
}

TEST_CASE("state_hash covers every Rover field") {
    covered("alive", [](State& s) { s.rovers[0].alive = false; });
    covered("kind", [](State& s) { s.rovers[0].kind = RoverKind::Ghost; });
    covered("x", [](State& s) { s.rovers[0].x = 2001; });
    covered("y", [](State& s) { s.rovers[0].y = 1801; });
    covered("dir", [](State& s) { s.rovers[0].dir = 2; });
    covered("speed", [](State& s) { s.rovers[0].speed = 31; });
    covered("move_budget", [](State& s) { s.rovers[0].move_budget = 13; });
    covered("anim_step", [](State& s) { s.rovers[0].anim_step = 8; });
    // speed used to sit at `<< 24` in the flag word, overlaying anim_step's
    // `<< 48`: this pair ORs to the SAME word under the old packing, so it is
    // the case that discriminates the fix rather than merely re-checking that
    // both fields are mixed at all.
    covered("speed vs anim_step aliasing", [](State& s) {
        s.rovers[0].speed = 30 + (1 << 24);
        s.rovers[0].anim_step = 7 ^ 1;
    });
}

TEST_CASE("state_hash excludes exactly the register in hash.cpp") {
    excluded("events", [](State& s) { s.events.push_back({Event::Type::Hurry, -1, -1, -1, 0}); });
    excluded("sub_trace", [](State& s) {
        s.sub_trace[0][0].x = 999;
        s.sub_trace[0][0].facing = Direction::Left;
    });
    excluded("tuning", [](State& s) { s.tuning.fuse_frames = 12345; });
    excluded("forbidden", [](State& s) { s.forbidden[0] = true; });
}
