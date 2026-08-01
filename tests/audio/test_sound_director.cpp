#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <cstddef>
#include <cstdint>
#include <vector>

#include "bomber/audio/sound_director.hpp"
#include "bomber/game_util/death_anim.hpp"
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

using bomber::game::death_anim_index;
using bomber::game::death_anim_slot;
using bomber::game::death_overlay_sound;
using bomber::game::SoundDirector;
using bomber::game::SoundSink;
namespace sim = bomber::sim;

namespace {

// Records every id the director asks for. `chance` always says yes and `roll`
// returns `roll_result` (0 unless a case sets it), so a cue that is behind a
// random gate still shows up here — the point is to catch cues that exist at
// all, not to sample a distribution. `roll_result` is deliberately POISONED in
// the death cases below: the overlay must not depend on it any more.
struct Recorder final : SoundSink {
    std::vector<int> ids;
    std::vector<std::uint64_t> exact_frames;  // what play_exact was told to stamp
    int roll_result = 0;
    void play(int id) override { ids.push_back(id); }
    void play_exact(int id, std::uint64_t frame) override {
        ids.push_back(id);
        exact_frames.push_back(frame);
    }
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

// One death by `victim` on tick `tick`, recorded into `rec`. Takes the recorder
// by reference rather than returning one: SoundSink has a virtual destructor, so
// copying a Recorder would lean on a deprecated implicit copy constructor.
void death_at(Recorder& rec, std::uint64_t tick, int victim) {
    SoundDirector d(rec);
    sim::State s = one_player_at(3, 3);
    s.tick = tick;
    s.events.push_back({sim::Event::Type::PlayerDied, static_cast<std::int8_t>(victim), 3, 3, 0});
    d.on_tick(s);
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
    // in that order. The overlay's index is the DEATH ANIMATION — 1-based over
    // the 24 the shipped VALUELST id 105 declares — and anim 1 is SOUNDLST 341
    // `burnedup`, the clip the overlay was written for. Tick 0, slot 0 is anim 1
    // by death_anim_index's arithmetic. The post-death taunt is scheduled for
    // tick+25, not now.
    Recorder rec;
    death_at(rec, 0, 0);
    REQUIRE(rec.ids.size() == 2);
    CHECK(rec.ids[0] == 300);  // the scream group
    CHECK(rec.ids[1] == 341);  // burnedup
}

TEST_CASE("the overlay names the animation the renderer will draw") {
    // THE POINT OF THE WHOLE FEATURE. The original has ONE actor[+4] that both
    // the `die green %d` sprite and the 340+N overlay read, so the sound always
    // describes the corpse on screen. The port has no such field, and the audio
    // side used to roll its own index — right 1 time in 24. Both sides now call
    // death_anim_index on (tick, victim slot), so this pins the two halves of
    // that contract together: the id the director asks for is exactly
    // 340 + the index, and the renderer's 0-based pool slot is exactly index-1
    // for the 24 sequences the install ships.
    //
    // `roll()` is poisoned with a value that would produce a DIFFERENT overlay
    // under the old code — if an independent draw ever comes back, this fails.
    for (std::uint64_t tick : {std::uint64_t{0}, std::uint64_t{7}, std::uint64_t{23},
                               std::uint64_t{24}, std::uint64_t{101}, std::uint64_t{4001}}) {
        for (int victim = 0; victim < 4; ++victim) {
            CAPTURE(tick);
            CAPTURE(victim);
            Recorder rec;
            rec.roll_result = 5;  // poison: would be a DIFFERENT overlay if read
            death_at(rec, tick, victim);
            const int anim = death_anim_index(tick, victim);
            REQUIRE(rec.ids.size() == 2);
            CHECK(rec.ids[1] == death_overlay_sound(anim));
            CHECK(death_anim_slot(tick, victim, 24) == static_cast<std::size_t>(anim - 1));
        }
    }
}

TEST_CASE("the death overlay covers 341..364, one id per death") {
    // The index is 1-based over 24 anims, so the overlay id is always in
    // 341..364 and there is always exactly one of it. Which of those ids are
    // actually AUDIBLE is SoundBank's business (15 of the 24 are empty slots and
    // sub_4278F2 returns without a sound) — the director's job is only to ask
    // for the right slot, so this asserts the mapping across the whole domain.
    // Sweeping the tick sweeps the domain: 24 consecutive ticks visit all 24,
    // and the per-tick EXACT id is the whole claim — 24 ticks, 24 distinct ids,
    // each the right one. (This once collected the ids into a container and
    // asserted its size/bounds too; every such derived figure is implied by the
    // exact pin, so the summary could only fail after a per-tick CHECK already
    // had.)
    for (std::uint64_t tick = 0; tick < 24; ++tick) {
        CAPTURE(tick);
        Recorder rec;
        death_at(rec, tick, 0);
        REQUIRE(rec.ids.size() == 2);
        CHECK(rec.ids[1] == static_cast<int>(341 + tick));
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
        // Slot 0 makes the index the tick, 1-based: anim N is tick N-1.
        Recorder rec;
        death_at(rec, static_cast<std::uint64_t>(anim1 - 1), 0);
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

TEST_CASE("the overlay hands sub_4278F2 the frame it stamps") {
    // sub_4278F2 ends `counts[id] = dword_464994`, so the caller's frame is not
    // decoration — it is what retires the slot from its group's least-played
    // rotation (SoundBank::pick_exact). The director passes the sim tick, the
    // same clock it already gives play_debounced.
    Recorder rec;
    death_at(rec, 4321, 2);
    REQUIRE(rec.exact_frames.size() == 1);
    CHECK(rec.exact_frames[0] == 4321u);
}

TEST_CASE("the wall slam also stamps its frame") {
    // The enclosure's is the other sub_4278F2 call site (sub_426818). It latches
    // `140 + rand%3` on the first dropped tile and replays that ONE id for the
    // rest of the sequence — so every tile in the run stamps, each with its own
    // tick, and the id never changes.
    Recorder rec;
    rec.roll_result = 1;  // -> 141
    SoundDirector d(rec);
    sim::State s = one_player_at(3, 3);
    for (std::uint64_t tick = 60; tick < 63; ++tick) {
        s.tick = tick;
        s.events.clear();
        s.events.push_back({sim::Event::Type::WallClosed, -1, 0, 0, 0});
        d.on_tick(s);
    }
    CHECK(rec.ids == std::vector<int>{141, 141, 141});
    CHECK(rec.exact_frames == std::vector<std::uint64_t>{60u, 61u, 62u});
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
