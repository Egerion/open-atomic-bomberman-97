#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <filesystem>
#include <set>
#include <string>

#include "bomber/assets/install.hpp"
#include "bomber/audio/sound_bank.hpp"

using bomber::game::SoundBank;

namespace {

// A SOUNDLST stand-in: `n` consecutive ids from `base`, named base_i.
void add_run(bomber::assets::res::SoundList& list, int base, int n, const std::string& prefix) {
    for (int i = 0; i < n; ++i) list.names[base + i] = prefix + std::to_string(i);
}

SoundBank loaded(const bomber::assets::res::SoundList& list, std::uint32_t seed = 7) {
    SoundBank bank;
    bank.load(list, seed);
    return bank;
}

}  // namespace

TEST_SUITE("sound_bank") {

TEST_CASE("a group is the contiguous run of occupied slots from the base") {
    bomber::assets::res::SoundList list;
    add_run(list, 20, 2, "blip");   // 20,21 then a gap
    add_run(list, 40, 2, "buzz");   // 40,41
    add_run(list, 160, 1, "drop");  // a lone slot
    SoundBank bank = loaded(list);

    CHECK(bank.group(20).size() == 2);
    CHECK(bank.group(40).size() == 2);
    CHECK(bank.group(160).size() == 1);
    // Mid-group entry is legal (the run just starts later); an empty slot is not
    // a group at all.
    CHECK(bank.group(21).size() == 1);
    CHECK(bank.group(22).empty());
    CHECK(bank.pick(22) == -1);
}

TEST_CASE("the pick is least-played-first, not uniform") {
    bomber::assets::res::SoundList list;
    add_run(list, 100, 4, "clip");
    SoundBank bank = loaded(list);

    // Four draws must cover the whole group exactly once — an equal-use
    // shuffle, which a uniform draw would not guarantee.
    std::set<int> cycle;
    for (int i = 0; i < 4; ++i) cycle.insert(bank.pick(100));
    CHECK(cycle == std::set<int>{100, 101, 102, 103});

    // ...and it keeps doing that, cycle after cycle.
    for (int c = 0; c < 5; ++c) {
        std::set<int> next;
        for (int i = 0; i < 4; ++i) next.insert(bank.pick(100));
        CHECK(next.size() == 4);
    }
}

TEST_CASE("a two-member group alternates but is not a fixed round-robin") {
    // The nav blip (20 letter1/letter2) and the drop-refused buzz (40
    // enrt1/enrt2) are both two-member groups. Least-played-first means the two
    // never repeat back to back ACROSS a pair boundary is not guaranteed, but
    // each pair of draws always covers both members.
    bomber::assets::res::SoundList list;
    add_run(list, 20, 2, "blip");
    SoundBank bank = loaded(list);
    for (int pair = 0; pair < 20; ++pair) {
        const int a = bank.pick(20);
        const int b = bank.pick(20);
        CHECK(a != b);
    }
}

TEST_CASE("the load-time cull trims each block to its keep count") {
    bomber::assets::res::SoundList list;
    // Every culled block, at full authored size (the real file's counts).
    add_run(list, 200, 20, "expl");
    add_run(list, 400, 84, "get");
    add_run(list, 701, 282, "taunt");  // note: 700 itself is NOT authored
    add_run(list, 1200, 80, "many");
    add_run(list, 1400, 144, "awesome");
    add_run(list, 2300, 88, "disease");
    add_run(list, 2700, 39, "hurry");
    add_run(list, 2800, 11, "intro");  // NOT culled
    SoundBank bank = loaded(list);

    CHECK(bank.group(200).size() == 3);
    CHECK(bank.group(400).size() == 7);
    CHECK(bank.group(1200).size() == 2);
    CHECK(bank.group(1400).size() == 7);
    CHECK(bank.group(2300).size() == 8);
    CHECK(bank.group(2700).size() == 5);
    // The title intro block is absent from the cull table: all eleven takes
    // survive every launch, which is why the boot sting has the most variety.
    CHECK(bank.group(2800).size() == 11);
}

TEST_CASE("the cull COMPACTS, so a base the author never wrote still resolves") {
    // SOUNDLST has no id 700 — the death-taunt block starts at 701 — yet the
    // binary's call site is sub_427961(700). The compaction pass inside the
    // cull is what makes that work.
    bomber::assets::res::SoundList list;
    add_run(list, 701, 282, "taunt");
    SoundBank bank = loaded(list);
    CHECK(bank.name(700) != nullptr);
    CHECK(bank.group(700).size() == 7);
    CHECK(bank.name(707) == nullptr);  // the run is terminated right after
}

TEST_CASE("a fresh session culls to a DIFFERENT random subset") {
    bomber::assets::res::SoundList list;
    add_run(list, 701, 282, "taunt");

    auto survivors = [&](std::uint32_t seed) {
        SoundBank bank = loaded(list, seed);
        std::set<std::string> names;
        for (int id : bank.group(700)) names.insert(*bank.name(id));
        return names;
    };
    // Same data, different seed -> a different seven. (Two 7-of-282 subsets
    // colliding is astronomically unlikely, so an exact-equality check is safe.)
    CHECK(survivors(1) != survivors(2));
    CHECK(survivors(1) == survivors(1));  // and the seed fully determines it
}

TEST_CASE("play counts are charged even when the group is smaller than the block") {
    // A group of one is still a group: the counter advances and the same slot
    // comes back every time.
    bomber::assets::res::SoundList list;
    add_run(list, 160, 1, "bmdrop3");
    SoundBank bank = loaded(list);
    for (int i = 0; i < 5; ++i) CHECK(bank.pick(160) == 160);
}

TEST_CASE("an exact play takes the named slot and never walks the group") {
    // sub_4278F2 is sub_427961 with the group walk removed: the caller gets the
    // slot it named, or nothing when that slot is empty.
    bomber::assets::res::SoundList list;
    add_run(list, 350, 4, "trampo");
    SoundBank bank = loaded(list);

    CHECK(bank.pick_exact(352, 10) == 352);
    CHECK(bank.pick_exact(350, 11) == 350);
    CHECK(bank.pick_exact(354, 12) == -1);  // past the run
    CHECK(bank.pick_exact(-1, 13) == -1);
    CHECK(bank.pick_exact(99999, 14) == -1);
}

TEST_CASE("an exact play STAMPS the frame where an ordinary pick increments") {
    // The one-line difference with the audible consequence. sub_427961 ends
    // `counts[pick] += 1` (0x427AB0); sub_4278F2 ends `counts[id] =
    // dword_464994` (0x427950) — the same array, ASSIGNED the game-frame
    // counter. docs/re/sound-engine.md §4.
    bomber::assets::res::SoundList list;
    add_run(list, 350, 4, "trampo");
    SoundBank bank = loaded(list);

    CHECK(bank.play_count(350) == 0);
    bank.pick_exact(350, 9000);
    CHECK(bank.play_count(350) == 9000);
    // It is an ASSIGNMENT, not a max() and not an increment: a later stamp with
    // a smaller frame lowers the counter again. The port does not "improve" on
    // that — a re-loaded SOUNDLST is exactly how the original recovers.
    bank.pick_exact(350, 5);
    CHECK(bank.play_count(350) == 5);
    // An empty slot is refused and charges nothing.
    const int before = bank.play_count(353);
    CHECK(bank.pick_exact(354, 7777) == -1);
    CHECK(bank.play_count(353) == before);
}

TEST_CASE("a stamped slot is retired from its group's rotation") {
    // THE SESSION-LONG NARROWING. Because the pick is least-played-first, a slot
    // holding a frame number in the thousands can never again equal its group's
    // minimum, so ordinary picks stop choosing it. In the shipped data this is
    // what a death whose anim index lands in 10-13 does to the trampoline group:
    // one exact play and that clip is gone from the trampoline TILE's four-way
    // pick for the rest of the session (docs/re/sound-engine.md §4, §10).
    bomber::assets::res::SoundList list;
    add_run(list, 350, 4, "trampo");
    SoundBank bank = loaded(list);

    bank.pick_exact(352, 9000);  // the death overlay's exact play

    std::set<int> heard;
    for (int i = 0; i < 60; ++i) heard.insert(bank.pick(350));
    // The other three still cycle equal-use; 352 is never drawn.
    CHECK(heard == std::set<int>{350, 351, 353});

    // Contrast: an ORDINARY play of the same slot only costs it one cycle.
    SoundBank plain = loaded(list);
    plain.pick_exact(352, 0);  // stamp with frame 0 == "unplayed"
    std::set<int> all;
    for (int i = 0; i < 60; ++i) all.insert(plain.pick(350));
    CHECK(all == std::set<int>{350, 351, 352, 353});
}

TEST_CASE("a fully retired group degrades to uniform rather than going silent") {
    // The 200-draw ceiling is a fallback, not a policy: when NO member sits at
    // the minimum the last draw is used anyway, so a group every one of whose
    // members has been stamped still plays something.
    bomber::assets::res::SoundList list;
    add_run(list, 350, 4, "trampo");
    SoundBank bank = loaded(list);
    for (int i = 0; i < 4; ++i) bank.pick_exact(350 + i, 9000 + i);

    for (int i = 0; i < 20; ++i) {
        const int got = bank.pick(350);
        CHECK(got >= 350);
        CHECK(got <= 353);
    }
}

TEST_CASE("the jelly debounce swallows re-triggers within three frames") {
    bomber::assets::res::SoundList list;
    add_run(list, 135, 3, "boun");
    SoundBank bank = loaded(list);

    CHECK(bank.pick_debounced(135, 100) >= 0);
    CHECK(bank.pick_debounced(135, 100) == -1);  // same frame
    CHECK(bank.pick_debounced(135, 101) == -1);
    CHECK(bank.pick_debounced(135, 102) == -1);
    CHECK(bank.pick_debounced(135, 103) >= 0);  // last + 3
    CHECK(bank.pick_debounced(135, 104) == -1);
    // A backwards jump (new match, counter reset) is not swallowed.
    CHECK(bank.pick_debounced(135, 5) >= 0);
}

TEST_CASE("against the real SOUNDLST.RES") {
    // Pins the group sizes the shipped data actually produces. SKIPs without an
    // install, the same way tests/visual does.
    const std::filesystem::path dir = bomber::assets::default_game_dir();
    if (dir.empty() || !std::filesystem::exists(dir / "DATA" / "RES" / "SOUNDLST.RES")) {
        MESSAGE("no original install found - skipping");
        return;
    }
    const auto list = bomber::assets::res::load_sounds(dir / "DATA" / "RES" / "SOUNDLST.RES");
    SoundBank bank = loaded(list, 12345);

    // Uncalled blocks keep every take (docs/re/sound-engine.md §3).
    CHECK(bank.group(2800).size() == 11);  // the title intro sting
    CHECK(bank.group(20).size() == 2);     // nav blip: letter1 / letter2
    CHECK(bank.group(40).size() == 2);     // drop refused: enrt1 / enrt2
    CHECK(bank.group(10).size() == 1);     // accept sting: menuexit alone
    CHECK(bank.group(170).size() == 6);    // grab + the four "dead" bmbthrw clips
    CHECK(bank.group(360).size() == 4);    // bombhit1..4 - the old range missed one
    CHECK(bank.group(130).size() == 3);
    CHECK(bank.group(135).size() == 3);
    CHECK(bank.group(350).size() == 4);
    CHECK(bank.group(1330).size() == 3);
    CHECK(bank.group(160).size() == 1);  // bmdrop3 really is alone

    // Culled blocks land exactly on their keep counts.
    CHECK(bank.group(200).size() == 3);
    CHECK(bank.group(400).size() == 7);
    CHECK(bank.group(700).size() == 7);  // and 700 exists only thanks to the compaction
    CHECK(bank.group(1200).size() == 2);
    CHECK(bank.group(1400).size() == 7);
    CHECK(bank.group(2300).size() == 8);
    CHECK(bank.group(2700).size() == 5);

    // Music ids must survive untouched - the cull may not move a track.
    CHECK(bank.name(1000) != nullptr);  // TITLE.RSS
    CHECK(bank.name(1010) != nullptr);  // MENU.RSS
    CHECK(bank.name(1120) != nullptr);  // the generic stage-track fallback
}

TEST_CASE("a group with no hole before the next block walks into it") {
    // WHY AudioEngine::play_sting takes a `hi`. A group is "the contiguous run
    // of occupied slots from the base" and knows nothing about where SOUNDLST's
    // authored BLOCK ends, so two blocks laid end to end with no empty slot
    // between them are one group as far as pick() is concerned. On the real
    // file that would mean a "we have a winner" take (2000) played on a DRAW
    // (1700) — which is why the draw/winner sting sites name their block's last
    // id and a pick past it is dropped rather than played.
    bomber::assets::res::SoundList list;
    add_run(list, 1700, 3, "tie");     // the draw block...
    add_run(list, 1703, 3, "winner");  // ...immediately followed by the next one
    SoundBank bank = loaded(list);

    CHECK(bank.group(1700).size() == 6);  // one group, both blocks
    bool crossed = false;
    for (int i = 0; i < 12; ++i)
        if (bank.pick(1700) > 1702) crossed = true;
    CHECK(crossed);  // and the pick really does land outside the draw block

    // A hole is the only thing that separates them, and the shipped file is not
    // guaranteed to have one — hence the bound, not a hope.
    bomber::assets::res::SoundList spaced;
    add_run(spaced, 1700, 3, "tie");
    add_run(spaced, 1704, 3, "winner");
    SoundBank sbank = loaded(spaced);
    CHECK(sbank.group(1700).size() == 3);
}

TEST_CASE("an empty sound list is inert rather than fatal") {
    SoundBank bank;
    bomber::assets::res::SoundList list;
    bank.load(list, 1);
    CHECK(bank.size() == 0);
    CHECK(bank.pick(20) == -1);
    CHECK(bank.name(20) == nullptr);
    CHECK(bank.pick_debounced(135, 0) == -1);
}

}  // TEST_SUITE
