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
// returns `roll_result` (0 unless a case sets it), so a cue that is behind a
// random gate still shows up here — the point is to catch cues that exist at
// all, not to sample a distribution. The death-anim cases below do drive
// `roll_result`, because for that cue WHICH value came up is the whole question.
struct Recorder final : SoundSink {
    std::vector<int> ids;
    int roll_result = 0;
    void play(int id) override { ids.push_back(id); }
    void play_exact(int id) override { ids.push_back(id); }
    void play_debounced(int id, std::uint64_t) override { ids.push_back(id); }
    bool chance(int) override { return true; }
    int roll(int) override { return roll_result; }
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
    // sub_421F7E, the head-hit handler itself, makes no audio call of any kind,
    // and nothing anywhere loads 360 as a literal. A bomb landing on a head
    // makes no sound OF ITS OWN, which is what this pins.
    //
    // Not to be confused with: SOUNDLST 360-363 (bombhit1..4) ARE reachable —
    // the death-anim overlay computes 340+anim and lands on them for anims
    // 20-23 (docs/re/sound-engine.md §10). Different cue, different event; the
    // silence asserted here is unaffected by that and must stay.
    Recorder rec;
    SoundDirector d(rec);
    sim::State s = one_player_at(3, 3);
    s.events.push_back({sim::Event::Type::HeadHit, 0, 3, 3, 0});
    d.on_tick(s);
    CHECK(rec.ids.empty());
}

TEST_CASE("a death plays the scream group and the anim overlay") {
    // sub_41DCB2: sub_427961(300) then sub_4278F2(340 + actor[+4]) @0x41DDE4,
    // in that order. The overlay's index is the DEATH ANIMATION, rolled as
    // rand() % getvalue(105) + 1 with id 105 authored 24 — so roll()==0 is
    // anim 1, and anim 1 is SOUNDLST 341 `burnedup`, the clip the overlay was
    // written for. The post-death taunt is scheduled for tick+25, not now.
    Recorder rec;
    SoundDirector d(rec);
    sim::State s = one_player_at(3, 3);
    s.events.push_back({sim::Event::Type::PlayerDied, 0, 3, 3, 0});
    d.on_tick(s);
    REQUIRE(rec.ids.size() == 2);
    CHECK(rec.ids[0] == 300);  // the scream group
    CHECK(rec.ids[1] == 341);  // burnedup
}

TEST_CASE("the death overlay covers 341..364, one id per death") {
    // The roll is 1-based over 24 anims, so the overlay id is always in
    // 341..364 and there is always exactly one of it. Which of those ids are
    // actually AUDIBLE is SoundBank's business (15 of the 24 are empty slots and
    // sub_4278F2 returns without a sound) — the director's job is only to ask
    // for the right slot, so this asserts the mapping across the whole domain.
    for (int anim0 = 0; anim0 < 24; ++anim0) {
        CAPTURE(anim0);
        Recorder rec;
        rec.roll_result = anim0;
        SoundDirector d(rec);
        sim::State s = one_player_at(3, 3);
        s.events.push_back({sim::Event::Type::PlayerDied, 0, 3, 3, 0});
        d.on_tick(s);
        REQUIRE(rec.ids.size() == 2);
        CHECK(rec.ids[1] == 341 + anim0);
        CHECK(rec.ids[1] >= 341);
        CHECK(rec.ids[1] <= 364);
    }
}

TEST_CASE("the death overlay reproduces the original's id collision") {
    // Anims 10-13 and 20-23 land on SOUNDLST blocks authored for OTHER cues —
    // trampoline (350-353) and bombhit (360-363) — because they were written
    // inside the index space the 340+N overlay reserved and sub_4278F2 addresses
    // a slot directly instead of walking a group. That is the ORIGINAL's data
    // collision and the port reproduces it deliberately (docs/re/sound-engine.md
    // §10). This case exists so the collision cannot be "tidied up" by accident:
    // if it starts failing, read the doc before changing the code.
    auto overlay_for = [](int anim1) {
        Recorder rec;
        rec.roll_result = anim1 - 1;  // roll() is 0-based, the anim index is 1-based
        SoundDirector d(rec);
        sim::State s = one_player_at(3, 3);
        s.events.push_back({sim::Event::Type::PlayerDied, 0, 3, 3, 0});
        d.on_tick(s);
        REQUIRE(rec.ids.size() == 2);
        return rec.ids[1];
    };
    CHECK(overlay_for(1) == 341);   // burnedup — the intended clip
    CHECK(overlay_for(10) == 350);  // trampoline block
    CHECK(overlay_for(13) == 353);
    CHECK(overlay_for(20) == 360);  // bombhit block
    CHECK(overlay_for(23) == 363);
    CHECK(overlay_for(24) == 364);  // empty slot -> SoundBank drops it
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
