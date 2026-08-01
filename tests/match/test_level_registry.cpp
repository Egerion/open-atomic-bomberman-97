// bomber::match LevelRegistry tests. The registry is the single source of truth
// for the game's playable levels: pick_stage's rotation, AssetStore's per-stage
// asset paths, and the LEVEL & ROUNDS screen all read from it. These tests pin
// the GOLDEN-SENSITIVE contract — with only the 11 built-ins the rotation and
// asset base names must be byte-identical to the old hardcoded sites — and the
// one-edit extensibility: adding a custom level must not disturb the built-ins.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "bomber/match/level_registry.hpp"
#include "bomber/match/match_factory.hpp"  // pick_stage

using namespace bomber;

namespace {

// The exact behaviour of the OLD pick_stage/rotation, reproduced verbatim so the
// registry path can be diffed against it. `reference_allowed` is the raw enabled
// list (ascending built-in index); `reference_pick` applies the empty->{0} fixup
// and the seed pick, matching the pre-registry code byte-for-byte.
std::vector<int> reference_allowed(const sim::Tuning& t) {
    std::vector<int> allowed;
    for (int i = 0; i < 11; ++i)
        if (t.level_enabled[i]) allowed.push_back(i);
    return allowed;
}
int reference_pick(const sim::Tuning& t, std::uint32_t seed) {
    std::vector<int> allowed = reference_allowed(t);
    if (allowed.empty()) allowed.push_back(0);
    return allowed[seed % allowed.size()];
}

// A tuning whose level_enabled mask is exactly `mask` (leaves every other field
// at its default — only the rotation gate matters here).
sim::Tuning tuning_with_mask(const std::array<int, 11>& mask) {
    sim::Tuning t;
    for (int i = 0; i < 11; ++i) t.level_enabled[i] = mask[static_cast<std::size_t>(i)];
    return t;
}

}  // namespace

// (a) with_builtins() yields the 11 stock defs with the exact FIELD/TILES/XBRICK
// base names, ascending index, builtin=true, and the generic fallback names.
TEST_CASE("with_builtins yields the 11 stock level defs") {
    const match::LevelRegistry reg = match::LevelRegistry::with_builtins();
    REQUIRE(reg.all().size() == 11);

    static const char* const kNames[] = {
        "NEW TRADITIONALIST", "CLASSIC GREEN ACRES", "HOCKEY RINK",
        "ANCIENT EGYPT",      "COAL MINE",           "BEACH",
        "ALIENS",             "HAUNTED HOUSE",       "UNDER THE OCEAN",
        "DEEP FOREST GREEN",  "INNER CITY TRASH"};

    for (int i = 0; i < 11; ++i) {
        const match::LevelDef& d = reg.all()[static_cast<std::size_t>(i)];
        CHECK(d.index == i);
        CHECK(d.builtin);
        CHECK(d.field_asset == "FIELD" + std::to_string(i));
        CHECK(d.tiles_asset == "TILES" + std::to_string(i));
        CHECK(d.xbrick_asset == "XBRICK" + std::to_string(i));
        CHECK(d.name_fallback == kNames[i]);
    }

    // find() locates by stage index; unknown indices return null (the signal
    // AssetStore uses to fall back to the legacy path default).
    CHECK(reg.find(0)->field_asset == "FIELD0");
    CHECK(reg.find(10)->xbrick_asset == "XBRICK10");
    CHECK(reg.find(11) == nullptr);
    CHECK(reg.find(-1) == nullptr);

    // The default singleton is the same 11-level set.
    CHECK(match::builtin_levels().all().size() == 11);
    CHECK(match::builtin_levels().find(4)->name_fallback == "COAL MINE");
}

// (b) enabled_stages reproduces the old rotation for several masks, and
// pick_stage over the built-ins is byte-identical to the reference for a spread
// of seeds (including the empty-mask -> {0} degenerate case).
TEST_CASE("enabled_stages + pick_stage reproduce the legacy rotation") {
    const match::LevelRegistry reg = match::LevelRegistry::with_builtins();

    // The stock default mask {1,1,0,1,1,1,0,1,1,1,1}: hockey rink (2) and coal
    // mine (6) disabled, so the rotation is exactly {0,1,3,4,5,7,8,9,10}.
    {
        sim::Tuning t;  // defaults
        CHECK(reg.enabled_stages(t) == std::vector<int>{0, 1, 3, 4, 5, 7, 8, 9, 10});
    }

    const std::array<std::array<int, 11>, 4> masks = {{
        {1, 1, 0, 1, 1, 1, 0, 1, 1, 1, 1},  // stock default
        {1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1},  // all enabled
        {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},  // none -> pick_stage falls back to {0}
        {0, 0, 1, 0, 0, 1, 0, 1, 0, 0, 0},  // a sparse arbitrary mask {2,5,7}
    }};
    for (const auto& mask : masks) {
        const sim::Tuning t = tuning_with_mask(mask);
        CHECK(reg.enabled_stages(t) == reference_allowed(t));
        for (std::uint32_t seed : {0u, 1u, 2u, 7u, 42u, 1000u, 65535u, 0xFFFFFFFFu}) {
            CHECK(match::pick_stage(t, seed) == reference_pick(t, seed));
            CHECK(match::pick_stage(t, seed, reg) == reference_pick(t, seed));
        }
    }
}

// (c) Adding one custom LevelDef grows all() to 12 without disturbing the
// built-in rotation while the custom level is disabled; enabling it appends the
// custom index after the built-ins.
TEST_CASE("adding a custom level leaves the built-in rotation unchanged") {
    const sim::Tuning t;                  // stock default mask
    const std::uint32_t seed = 0xABCDEF01;

    match::LevelRegistry reg = match::LevelRegistry::with_builtins();
    const int before = match::pick_stage(t, seed, reg);
    const std::vector<int> rotation_before = reg.enabled_stages(t);

    match::LevelDef custom;
    custom.index = 11;
    custom.name_fallback = "MY CUSTOM MAP";
    custom.field_asset = "MYFIELD";
    custom.tiles_asset = "MYTILES";
    custom.xbrick_asset = "MYXBRICK";
    custom.builtin = false;
    custom.enabled = false;  // registered but NOT in the rotation yet
    reg.add(custom);

    CHECK(reg.all().size() == 12);
    CHECK(reg.find(11) != nullptr);
    CHECK(reg.find(11)->field_asset == "MYFIELD");
    CHECK_FALSE(reg.find(11)->builtin);

    // The built-in rotation and the fixed-seed pick are untouched.
    CHECK(reg.enabled_stages(t) == rotation_before);
    CHECK(match::pick_stage(t, seed, reg) == before);

    // A custom level flagged enabled DOES join the rotation, appended after the
    // built-ins (proving the per-def flag gates custom eligibility both ways).
    match::LevelRegistry reg_on = match::LevelRegistry::with_builtins();
    match::LevelDef custom_on = custom;
    custom_on.enabled = true;
    reg_on.add(custom_on);
    const std::vector<int> rotation_on = reg_on.enabled_stages(t);
    CHECK(rotation_on.size() == rotation_before.size() + 1);
    CHECK(rotation_on.back() == 11);  // appended after the built-ins
}
