// The two .SCH fields the port used to parse and then get wrong, pinned end to
// end (scheme text -> MatchConfig -> the sim's first tick):
//
//   1. the "-S" row's 4th field is the per-slot TEAM (docs/re/facts.md "The
//      .SCH -S row's 4th field is the per-slot TEAM"). It used to be parsed
//      into a field literally named `extra` and thrown away, so the 19 shipped
//      team-designed maps played with teams unrelated to the map.
//   2. the "-P" row's 2nd field is a starting-inventory COUNT that REPLACES
//      the VALUELST baseline (facts.md "The .SCH -P row's 2nd field is a COUNT
//      that REPLACES the starting inventory"). It used to collapse to a bool
//      and be applied ONCE, ADDITIVELY, on top of the baseline.
//
// Both go through the real text parser rather than a hand-built Scheme, so a
// regression in the reader fails here too.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <array>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

#include "bomber/assets/reslist.hpp"
#include "bomber/assets/sch.hpp"
#include "bomber/match/match_factory.hpp"
#include "bomber/sim/simulation.hpp"

using namespace bomber;

namespace {

// Writes `text` to a temp .SCH and parses it back through assets::sch::load,
// so every test below exercises the shipped-format reader.
assets::sch::Scheme parse(const std::string& text, const char* stem) {
    const std::filesystem::path p =
        std::filesystem::temp_directory_path() / (std::string(stem) + ".sch");
    {
        std::ofstream f(p, std::ios::binary);
        f << text;
    }
    assets::sch::Scheme s = assets::sch::load(p);
    std::error_code ec;
    std::filesystem::remove(p, ec);
    return s;
}

// A minimal but valid board: the -R rows must all be kGridWidth wide.
std::string board_rows() {
    std::string out;
    for (int y = 0; y < sim::kGridHeight; ++y) {
        out += "-R," + std::to_string(y) + ",";
        out.append(static_cast<std::size_t>(sim::kGridWidth), '.');
        out += "\n";
    }
    return out;
}

// Ten "-S" rows. `teams` selects the 4th field per slot; a negative entry
// writes a THREE-field row instead (the field omitted entirely).
std::string spawn_rows(const std::array<int, 10>& teams) {
    std::string out;
    for (int i = 0; i < 10; ++i) {
        out += "-S," + std::to_string(i) + "," + std::to_string(i % sim::kGridWidth) + ",0";
        if (teams[static_cast<std::size_t>(i)] >= 0)
            out += "," + std::to_string(teams[static_cast<std::size_t>(i)]);
        out += "\n";
    }
    return out;
}

std::string scheme_text(const std::array<int, 10>& teams, const std::string& powerup_rows = {}) {
    return "-V,2\n-N,fixture\n-B,0\n" + board_rows() + spawn_rows(teams) + powerup_rows;
}

constexpr std::array<int, 10> kNoField = {-1, -1, -1, -1, -1, -1, -1, -1, -1, -1};
// E_VS_W.SCH / N_VS_S.SCH / TENNIS.SCH / VOLLEY.SCH all ship exactly this: the
// low five slots on one side, the high five on the other. It is deliberately
// NOT the parity default, so a port that drops the field cannot produce it —
// and that property belongs to the FIXTURE, so it is pinned at compile time: if
// this array ever drifted onto the parity layout, the layout case below would
// silently stop discriminating the drop-the-field regression it exists for. (A
// runtime count of the disagreements, which this replaces, could only fail
// after the elementwise CHECKs already had.)
constexpr std::array<int, 10> kFiveVsFive = {0, 0, 0, 0, 0, 1, 1, 1, 1, 1};
static_assert(
    [] {
        int off_parity = 0;
        for (int i = 0; i < 10; ++i)
            if (kFiveVsFive[static_cast<std::size_t>(i)] != (i & 1)) ++off_parity;
        return off_parity == 4;
    }(),
    "kFiveVsFive must disagree with the parity default on four slots");

}  // namespace

// --- finding 1: the "-S" 4th field is the TEAM -------------------------------

TEST_CASE("a scheme with no 4th field keeps the alternating parity default") {
    // sub_4049C0 seeds every start record with `slot & 1` before the reader
    // opens the file, and a three-field row never overwrites it.
    const assets::sch::Scheme s = parse(scheme_text(kNoField), "obm_teams_none");
    for (const auto& sp : s.spawns) CHECK(sp.team == 0);  // parsed field absent -> 0

    std::array<int, 10> team{};
    match::scheme_setup_teams(s, team);
    for (int i = 0; i < 10; ++i) CHECK(team[static_cast<std::size_t>(i)] == (i & 1));
}

TEST_CASE("a team-designed scheme produces the layout its author drew") {
    const assets::sch::Scheme s = parse(scheme_text(kFiveVsFive), "obm_teams_5v5");

    std::array<int, 10> team{};
    match::scheme_setup_teams(s, team);
    // A port that ignores the 4th field again produces the parity default,
    // which kFiveVsFive's static_assert guarantees these ten CHECKs refuse.
    for (int i = 0; i < 10; ++i)
        CHECK(team[static_cast<std::size_t>(i)] == kFiveVsFive[static_cast<std::size_t>(i)]);
}

TEST_CASE("a partial scheme overrides only the slots it names") {
    // Slots 0 and 1 carry a 4th field that INVERTS the parity default; the
    // other eight omit it and must keep parity.
    std::array<int, 10> teams = kNoField;
    teams[0] = 1;
    teams[1] = 0;
    const assets::sch::Scheme s = parse(scheme_text(teams), "obm_teams_partial");

    std::array<int, 10> team{};
    match::scheme_setup_teams(s, team);
    CHECK(team[0] == 1);
    CHECK(team[1] == 0);
    for (int i = 2; i < 10; ++i) CHECK(team[static_cast<std::size_t>(i)] == (i & 1));
}

TEST_CASE("the 4th field is stored as a boolean, like sub_403EEE's `!= 0`") {
    std::array<int, 10> teams = kNoField;
    teams[0] = 7;  // any nonzero value is team 1
    const assets::sch::Scheme s = parse(scheme_text(teams), "obm_teams_bool");
    CHECK(s.spawns[0].team == 1);
}

TEST_CASE("build_match_config lays the scheme's teams into MatchConfig::team") {
    const assets::sch::Scheme s = parse(scheme_text(kFiveVsFive), "obm_teams_cfg");
    const sim::MatchConfig cfg =
        match::build_match_config(s, sim::kMaxPlayers, 0x1234u, nullptr, false, /*team_play=*/true);

    // sim::Player::team reserves 0 for "solo side", so the +84 byte's 0/1 maps
    // to 1/2 -- both halves are REAL teams, which is the whole point of a
    // two-sided map.
    for (int i = 0; i < 5; ++i) CHECK(cfg.team[static_cast<std::size_t>(i)] == 1);
    for (int i = 5; i < 10; ++i) CHECK(cfg.team[static_cast<std::size_t>(i)] == 2);
}

TEST_CASE("the Options team gate (dword_464964) suppresses the scheme's teams") {
    const assets::sch::Scheme s = parse(scheme_text(kFiveVsFive), "obm_teams_gate");
    const sim::MatchConfig cfg = match::build_match_config(s, sim::kMaxPlayers, 0x1234u, nullptr,
                                                           false, /*team_play=*/false);
    for (const auto v : cfg.team) CHECK(v == 0);  // every player its own side
}

TEST_CASE("the authored teams survive into the hashed Player::team") {
    const assets::sch::Scheme s = parse(scheme_text(kFiveVsFive), "obm_teams_sim");
    sim::MatchConfig cfg =
        match::build_match_config(s, sim::kMaxPlayers, 0x1234u, nullptr, false, /*team_play=*/true);
    cfg.player_count = sim::kMaxPlayers;
    const sim::Simulation sim_(cfg);
    const sim::State& st = sim_.state();

    // Slots the map author put on the same side really are teammates...
    CHECK(st.players[0].team == st.players[4].team);
    CHECK(st.players[5].team == st.players[9].team);
    // ...and the two sides really are different.
    CHECK(st.players[0].team != st.players[5].team);
    // Under the old port slot 0 and slot 1 were opposite (parity); the map says
    // they are teammates.
    CHECK(st.players[0].team == st.players[1].team);
}

// --- finding 2: the "-P" 2nd field is a COUNT that REPLACES -------------------

namespace {

// One "-P" row per kind, all zero except `kind`, which gets `count`.
std::string powerup_rows(int kind, int count) {
    std::string out;
    for (int i = 0; i < sim::kPowerupKinds; ++i) {
        out += "-P," + std::to_string(i) + "," + std::to_string(i == kind ? count : 0) + ",0,0,0\n";
    }
    return out;
}

}  // namespace

TEST_CASE("a born-with COUNT sets the total, it does not add one") {
    // The stock VALUELST starts a player on ONE bomb. A scheme asking for 3
    // must produce exactly 3.
    //
    // This is the assertion the old code could not satisfy from either side:
    // it collapsed the count to a bool and applied it once on top of the
    // baseline, so the only reachable answer was 1 + 1 == 2 -- for a "-P,0,3"
    // row AND for a "-P,0,99" one.
    const assets::sch::Scheme s =
        parse(scheme_text(kNoField, powerup_rows(static_cast<int>(sim::PowerupType::ExtraBomb), 3)),
              "obm_born_count");
    sim::MatchConfig cfg = match::build_match_config(s, 2, 0x1234u);
    CHECK(cfg.tuning.start_with[static_cast<int>(sim::PowerupType::ExtraBomb)] == 3);

    const sim::Simulation sim_(cfg);
    CHECK(sim_.state().players[0].max_bombs == 3);
    CHECK(sim_.state().players[1].max_bombs == 3);
}

TEST_CASE("a born-with count of 0 leaves the VALUELST baseline alone") {
    // sub_403EEE's tail loop only calls the setter when the count is > 0, so a
    // zero row cannot zero the default -- it is "no opinion", not "none".
    assets::res::ValueList values;
    values.values[50] = 4;  // VALUELST id 50 = starting bombs
    const assets::sch::Scheme s =
        parse(scheme_text(kNoField, powerup_rows(static_cast<int>(sim::PowerupType::ExtraBomb), 0)),
              "obm_born_zero");
    const sim::MatchConfig cfg = match::build_match_config(s, 2, 0x1234u, &values);
    CHECK(cfg.tuning.start_with[static_cast<int>(sim::PowerupType::ExtraBomb)] == 4);
    CHECK(sim::Simulation(cfg).state().players[0].max_bombs == 4);
}

TEST_CASE("a nonzero born-with count overrides the VALUELST baseline") {
    assets::res::ValueList values;
    values.values[50] = 4;
    const assets::sch::Scheme s =
        parse(scheme_text(kNoField, powerup_rows(static_cast<int>(sim::PowerupType::ExtraBomb), 2)),
              "obm_born_over");
    const sim::MatchConfig cfg = match::build_match_config(s, 2, 0x1234u, &values);
    // The scheme is read AFTER the value list, exactly as sub_403EEE overwrites
    // an already-loaded value table.
    CHECK(cfg.tuning.start_with[static_cast<int>(sim::PowerupType::ExtraBomb)] == 2);
    CHECK(sim::Simulation(cfg).state().players[0].max_bombs == 2);
}

TEST_CASE("a flag kind's born-with count grants the ability") {
    const assets::sch::Scheme s =
        parse(scheme_text(kNoField, powerup_rows(static_cast<int>(sim::PowerupType::Kick), 1)),
              "obm_born_flag");
    const sim::MatchConfig cfg = match::build_match_config(s, 2, 0x1234u);
    const sim::Simulation sim_(cfg);
    CHECK(sim_.state().players[0].kick == true);
    CHECK(sim_.state().players[1].kick == true);
}

TEST_CASE("mutually exclusive kinds granted together are BOTH kept") {
    // sub_4214BC's baseline write is a raw byte store: it does not run the
    // pickup dispatcher's grab<->spooger eviction, and it draws no RNG.
    //
    // The old path did. It replayed each born-with kind through
    // PowerupSystem::apply, and because the granted Grab was not part of the
    // start_with baseline, applying Spooger one iteration later saw it as
    // SURPLUS: it scattered the glove onto the floor and took it away again.
    // This is the mechanism behind golden B's recapture.
    std::string rows;
    for (int i = 0; i < sim::kPowerupKinds; ++i) {
        const bool on = i == static_cast<int>(sim::PowerupType::Grab) ||
                        i == static_cast<int>(sim::PowerupType::Spooger);
        rows += "-P," + std::to_string(i) + "," + std::to_string(on ? 1 : 0) + ",0,0,0\n";
    }
    const assets::sch::Scheme s = parse(scheme_text(kNoField, rows), "obm_born_excl");
    sim::MatchConfig cfg = match::build_match_config(s, 2, 0x1234u);
    for (auto& c : cfg.tuning.spawn_counts) c = 0;  // isolate: no scatter from the hide pass

    const sim::Simulation sim_(cfg);
    const sim::State& st = sim_.state();
    CHECK(st.players[0].grab == true);
    CHECK(st.players[0].spooge == true);
    // ...and setup scattered nothing to the floor and drew no sim RNG.
    int floor_tokens = 0;
    for (int y = 0; y < sim::kGridHeight; ++y)
        for (int x = 0; x < sim::kGridWidth; ++x)
            if (st.floor[y][x] != sim::PowerupType::None) ++floor_tokens;
    CHECK(floor_tokens == 0);
}
