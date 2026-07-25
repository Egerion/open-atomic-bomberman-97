// bomber::net MatchConfig wire codec (match_config_codec.hpp) — the
// determinism boundary of the host-authoritative setup layer.
//
// The point of these tests: online peers must feed Simulation BYTE-IDENTICAL
// input, so the confirmed config travels as bytes instead of as a level index.
// A field the codec forgets is not a cosmetic bug, it is a desync on someone
// else's machine, so the round-trip below fills EVERY field of MatchConfig with
// a unique non-default value (tests/common/match_config_compare.hpp) and
// asserts field-by-field equality. The rest is the untrusted-input contract:
// truncated, over-long, mis-tagged and out-of-range payloads are all rejected
// without a crash and without a throw.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <cstdint>
#include <string>
#include <vector>

#include "bomber/net/match_config_codec.hpp"
#include "match_config_compare.hpp"

using namespace bomber;  // NOLINT(google-build-using-namespace) — test-local

namespace {

// Offsets into the blob, derived from the layout documented in
// match_config_codec.hpp. Pinning them here means a silent layout shift shows
// up as a failing bounds test, not as a mystery desync.
constexpr std::size_t kGridBytes = 15 * 11;
constexpr std::size_t kCellsAt = 2;  // after the u16 layout tag
constexpr std::size_t kActorTypeAt = kCellsAt + kGridBytes;
constexpr std::size_t kSpawnCountAt = kCellsAt + 5 * kGridBytes;

// The size of a full 10-spawn config, pinned. This is the tripwire for "a field
// was added to the codec": the number moves, this fails, and whoever bumped it
// is forced past the comparator in match_config_compare.hpp too.
constexpr std::size_t kFullConfigBytes = 1842;

std::string join(const std::vector<std::string>& names) {
    std::string s;
    for (const auto& n : names) {
        if (!s.empty()) s += ", ";
        s += n;
    }
    return s;
}

}  // namespace

TEST_CASE("the distinctive config really differs from a default one in every field") {
    // Guards the round-trip below: if the filler left a field at its default,
    // a codec that skipped that field would still round-trip "equal".
    const sim::MatchConfig full = sim::test::distinctive_match_config();
    const std::vector<std::string> same = sim::test::match_config_matches(full, sim::MatchConfig{});
    INFO("fields the filler left at their default: " << join(same));
    CHECK(same.empty());
}

TEST_CASE("a fully-populated MatchConfig round-trips field for field") {
    const sim::MatchConfig full = sim::test::distinctive_match_config();
    const std::vector<std::uint8_t> blob = net::encode_match_config(full);

    CHECK(blob.size() == kFullConfigBytes);
    // Documented in the header as the reason the setup layer chunks: a single
    // datagram this big would depend on IP fragmentation surviving the path.
    CHECK(blob.size() > 1200);

    sim::MatchConfig back;
    REQUIRE(net::decode_match_config(blob.data(), blob.size(), &back));

    const std::vector<std::string> diffs = sim::test::match_config_diffs(full, back);
    INFO("fields the codec did not preserve: " << join(diffs));
    CHECK(diffs.empty());
    // Spot-check the two that carry the most desync risk if silently dropped.
    CHECK(back.seed == full.seed);
    CHECK(back.spawns.size() == full.spawns.size());
}

TEST_CASE("a default MatchConfig round-trips too (the empty-spawns edge)") {
    const sim::MatchConfig def;
    const std::vector<std::uint8_t> blob = net::encode_match_config(def);
    sim::MatchConfig back;
    REQUIRE(net::decode_match_config(blob.data(), blob.size(), &back));
    CHECK(sim::test::match_config_diffs(def, back).empty());
    CHECK(back.spawns.empty());
    // 1 spawn-count byte and nothing else: the fixed part of the layout.
    CHECK(blob.size() == kFullConfigBytes - 10 * 8);
}

TEST_CASE("decode rejects malformed payloads without crashing or throwing") {
    const sim::MatchConfig full = sim::test::distinctive_match_config();
    const std::vector<std::uint8_t> blob = net::encode_match_config(full);
    // A sentinel the decoder must never touch on a rejection.
    sim::MatchConfig out;
    out.seed = 0x5A5A5A5Au;

    SUBCASE("null and empty") {
        CHECK_FALSE(net::decode_match_config(nullptr, 0, &out));
        CHECK_FALSE(net::decode_match_config(nullptr, 16, &out));
        CHECK_FALSE(net::decode_match_config(blob.data(), 0, nullptr));
        const std::uint8_t nothing = 0;
        CHECK_FALSE(net::decode_match_config(&nothing, 0, &out));
    }

    SUBCASE("every truncation is rejected") {
        // Stride keeps the suite fast while still hitting every field boundary
        // class (grid bytes, i32s, the spawn table).
        for (std::size_t n = 0; n < blob.size(); n += 7) {
            CHECK_NOTHROW(net::decode_match_config(blob.data(), n, &out));
            CHECK_FALSE(net::decode_match_config(blob.data(), n, &out));
        }
        CHECK_FALSE(net::decode_match_config(blob.data(), blob.size() - 1, &out));
    }

    SUBCASE("trailing bytes are a malformation, not padding") {
        std::vector<std::uint8_t> longer = blob;
        longer.push_back(0);
        CHECK_FALSE(net::decode_match_config(longer.data(), longer.size(), &out));
    }

    SUBCASE("an oversized buffer is refused before any work") {
        const std::vector<std::uint8_t> huge(net::kMaxMatchConfigBytes + 1, 0);
        CHECK_FALSE(net::decode_match_config(huge.data(), huge.size(), &out));
    }

    SUBCASE("a foreign layout tag is refused") {
        std::vector<std::uint8_t> bad = blob;
        bad[0] = static_cast<std::uint8_t>(net::kMatchConfigLayout + 1);
        CHECK_FALSE(net::decode_match_config(bad.data(), bad.size(), &out));
    }

    SUBCASE("an out-of-range Cell is refused") {
        std::vector<std::uint8_t> bad = blob;
        bad[kCellsAt] = 3;  // only Blank/Brick/Solid exist
        CHECK_FALSE(net::decode_match_config(bad.data(), bad.size(), &out));
    }

    SUBCASE("an out-of-range ActorType is refused") {
        std::vector<std::uint8_t> bad = blob;
        bad[kActorTypeAt] = 4;  // 0..3 are real kinds, 255 is None, 4 is nothing
        CHECK_FALSE(net::decode_match_config(bad.data(), bad.size(), &out));
        bad[kActorTypeAt] = 254;
        CHECK_FALSE(net::decode_match_config(bad.data(), bad.size(), &out));
        bad[kActorTypeAt] = 255;  // None is legal — and the length still matches
        CHECK(net::decode_match_config(bad.data(), bad.size(), &out));
    }

    SUBCASE("a spawn count past kMaxPlayers is refused") {
        std::vector<std::uint8_t> bad = blob;
        bad[kSpawnCountAt] = static_cast<std::uint8_t>(sim::kMaxPlayers + 1);
        CHECK_FALSE(net::decode_match_config(bad.data(), bad.size(), &out));
        bad[kSpawnCountAt] = 255;
        CHECK_FALSE(net::decode_match_config(bad.data(), bad.size(), &out));
    }

    SUBCASE("single-byte corruption anywhere never crashes or throws") {
        // A cheap deterministic fuzz over the whole blob: every 13th byte gets
        // three hostile values. Correctness is not the claim here — survival is.
        std::vector<std::uint8_t> bad = blob;
        for (std::size_t i = 0; i < bad.size(); i += 13) {
            const std::uint8_t keep = bad[i];
            for (const std::uint8_t v : {std::uint8_t{0}, std::uint8_t{0x7F}, std::uint8_t{0xFF}}) {
                bad[i] = v;
                CHECK_NOTHROW(net::decode_match_config(bad.data(), bad.size(), &out));
            }
            bad[i] = keep;
        }
    }
}

TEST_CASE("a rejected decode leaves the caller's config untouched") {
    sim::MatchConfig out = sim::test::distinctive_match_config();
    const sim::MatchConfig before = out;
    const std::vector<std::uint8_t> garbage(64, 0xAB);
    CHECK_FALSE(net::decode_match_config(garbage.data(), garbage.size(), &out));
    CHECK(sim::test::match_config_diffs(before, out).empty());
}

TEST_CASE("spawns beyond kMaxPlayers are dropped rather than inflating the datagram") {
    // A hand-edited .SCH can carry `-S,50,...`, which resizes MatchConfig::spawns
    // past the slot space. The sim indexes that vector by player number, so
    // nothing past slot 9 is reachable; the codec must not carry it either.
    sim::MatchConfig wide = sim::test::distinctive_match_config();
    wide.spawns.resize(64);
    const std::vector<std::uint8_t> blob = net::encode_match_config(wide);
    CHECK(blob.size() == kFullConfigBytes);

    sim::MatchConfig back;
    REQUIRE(net::decode_match_config(blob.data(), blob.size(), &back));
    CHECK(back.spawns.size() == static_cast<std::size_t>(sim::kMaxPlayers));
    for (std::size_t i = 0; i < back.spawns.size(); ++i) {
        CHECK(back.spawns[i].x == wide.spawns[i].x);
        CHECK(back.spawns[i].y == wide.spawns[i].y);
    }
}
