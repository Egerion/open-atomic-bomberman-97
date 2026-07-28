#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <cstdint>
#include <vector>

#include "bomber/audio/sound_director.hpp"
#include "bomber/sim/state.hpp"

// Pins the event -> SOUNDLST-group mapping, and in particular the cues the
// original does NOT have. Every "no sound" assertion below is backed by an
// exhaustive call-site scan of BM95.EXE (docs/re/sound-engine.md §8): the ids
// simply have no caller, so the port must not invent one. If one of these
// starts failing because a cue came back, the fix is to delete the cue again,
// not to relax the test.
//
// SDL-free by construction: SoundDirector talks to SoundSink, so this suite
// compiles sound_director.cpp against the recorder below and runs headless.

using bomber::game::SoundDirector;
using bomber::game::SoundSink;
namespace sim = bomber::sim;

namespace {

// Records every id the director asks for. `chance` always says yes and `roll`
// always returns 0, so a cue that is behind a random gate still shows up here —
// the point is to catch cues that exist at all, not to sample a distribution.
struct Recorder final : SoundSink {
    std::vector<int> ids;
    void play(int id) override { ids.push_back(id); }
    void play_exact(int id) override { ids.push_back(id); }
    void play_debounced(int id, std::uint64_t) override { ids.push_back(id); }
    bool chance(int) override { return true; }
    int roll(int) override { return 0; }
};

// A minimal state with one live player parked on a known tile.
sim::State one_player_at(int tx, int ty) {
    sim::State s;
    sim::Player& p = s.players[0];
    p.present = true;
    p.alive = true;
    p.x = tx * sim::kTileWF + sim::kTileWF / 2;
    p.y = ty * sim::kTileHF + sim::kTileHF / 2;
    p.max_bombs = 1;
    return s;
}

}  // namespace

TEST_CASE("head hit is silent") {
    // SOUNDLST 360-363 (bombhit1..4) load, but no call site in the binary names
    // 360: not among the 70 sub_427961 sites, and the primitives' addresses are
    // never taken, so no indirect path exists either. The bomb landing on a head
    // makes no sound of its own.
    Recorder rec;
    SoundDirector d(rec);
    sim::State s = one_player_at(3, 3);
    s.events.push_back({sim::Event::Type::HeadHit, 0, 3, 3, 0});
    d.on_tick(s);
    CHECK(rec.ids.empty());
}

TEST_CASE("a plain drop is audible") {
    // The control for the spooger case below: sub_41F29B's drop branch places at
    // the player's OWN tile and reaches sub_427961(100).
    Recorder rec;
    SoundDirector d(rec);
    sim::State s = one_player_at(3, 3);
    s.events.push_back({sim::Event::Type::BombPlaced, 0, 3, 3, 0});
    d.on_tick(s);
    REQUIRE(rec.ids.size() == 1);
    CHECK(rec.ids[0] == 100);
}

TEST_CASE("a diarrhea drop swaps the splat in, still one cue") {
    Recorder rec;
    SoundDirector d(rec);
    sim::State s = one_player_at(3, 3);
    s.events.push_back({sim::Event::Type::BombPlaced, 0, 3, 3, 1});
    d.on_tick(s);
    REQUIRE(rec.ids.size() == 1);
    CHECK(rec.ids[0] == 550);
}

TEST_CASE("the spooger string is silent") {
    // The spooge loop lays its whole run ahead of the player and every exit
    // jumps PAST the sound block, so none of 40/1200/550/100 plays — no matter
    // how long the string is. The port used to fire one drop SFX per bomb.
    Recorder rec;
    SoundDirector d(rec);
    sim::State s = one_player_at(3, 3);
    for (int i = 1; i <= 5; ++i)
        s.events.push_back({sim::Event::Type::BombPlaced, 0, static_cast<std::int8_t>(3 + i), 3, 0});
    d.on_tick(s);
    CHECK(rec.ids.empty());
}

TEST_CASE("the spooger taunt gate cannot fire either") {
    // The "huge string of bombs" taunt (1200) sits INSIDE the block the spooge
    // run jumps over. With chance() forced true and the bomb allotment exactly
    // met, a plain drop would taunt; a spooged bomb must still say nothing.
    Recorder rec;
    SoundDirector d(rec);
    sim::State s = one_player_at(3, 3);
    s.tuning.taunt_many_bombs = 1;
    sim::Player& p = s.players[0];
    p.max_bombs = 4;
    p.bombs_placed = 4;
    s.events.push_back({sim::Event::Type::BombPlaced, 0, 4, 3, 0});  // one tile AHEAD
    d.on_tick(s);
    CHECK(rec.ids.empty());
}

TEST_CASE("a throw makes no sound of its own") {
    Recorder rec;
    SoundDirector d(rec);
    sim::State s = one_player_at(3, 3);
    s.events.push_back({sim::Event::Type::BombThrown, 0, 3, 3, 0});
    d.on_tick(s);
    CHECK(rec.ids.empty());
}

TEST_CASE("a punch that launches nothing makes no sound") {
    // sub_427961(150) sits inside sub_424A50's `if (bomb ahead)`; ev.data is
    // that hit flag. The glove still swings, silently.
    Recorder rec;
    SoundDirector d(rec);
    sim::State s = one_player_at(3, 3);
    s.events.push_back({sim::Event::Type::BombPunched, 0, 3, 3, 0});
    d.on_tick(s);
    CHECK(rec.ids.empty());

    s.events.clear();
    s.events.push_back({sim::Event::Type::BombPunched, 0, 3, 3, 1});
    d.on_tick(s);
    REQUIRE(rec.ids.size() == 1);
    CHECK(rec.ids[0] == 150);
}
