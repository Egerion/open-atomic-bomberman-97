#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <cstdint>
#include <string>

#include "bomber/net/build_hash.hpp"
#include "golden_scenarios.hpp"

namespace golden = bomber::testing::golden;

namespace {

// Hand-rolled because doctest's MESSAGE stringifies a std::hex manipulator as
// "{?}" rather than applying it, and a fingerprint printed in decimal is not
// paste-able into the constant below.
std::string hex64(std::uint64_t v) {
    std::string s = "0x0000000000000000ull";
    for (int i = 0; i < 16; ++i) s[2 + 15 - i] = "0123456789abcdef"[(v >> (i * 4)) & 0xF];
    return s;
}

}  // namespace

// The two pins below were captured by ONE run at ONE commit. Update both, or
// neither, and read the rule-7 case before you touch either.
//
//   kGoldenFingerprint  what tests/sim/test_golden.cpp's six scenarios say sim
//                       behaviour is (their final hashes, folded).
//   kBuildHash          what libs/net/src/build_hash.cpp's eight scenarios say
//                       it is — the number two peers compare at the lobby door.
constexpr std::uint64_t kGoldenFingerprint = 0xd78286148efb2a98ull;
constexpr std::uint32_t kBuildHash = 2776396481u;

// The cross-build compatibility digest (ADR-0011): peers compare it before a
// match to reject incompatible builds.
TEST_CASE("build_hash is stable and non-degenerate") {
    // No self-equality CHECK: build_hash() returns a function-local static, so
    // two calls are equal by the language, not by the digest. Stability across
    // PROCESSES is what matters and the pinned kBuildHash below carries it.
    const std::uint32_t a = bomber::net::build_hash();
    MESSAGE("build_hash = " << a);
    CHECK(a != 0u);
    CHECK(a != 0xFFFFFFFFu);
    // Not merely the wire-protocol mix with a zero scenario hash (would mean the
    // sim contributed nothing).
    CHECK(a != (bomber::net::kWireProtocolVersion * 0x9E3779B1u));
}

// DETERMINISM RULE 7'S DETECTOR, and until this landed the rule had none: the
// digest's value was pinned nowhere, so "a sim behaviour change must MOVE
// build_hash" was enforced by a paragraph and by someone remembering. It failed
// silently three times.
//
// Pinning the digest alone does not detect the rule's failure. A pin catches
// "the digest moved and you did not notice"; rule 7 breaks the other way round —
// behaviour moved and the digest did NOT, because build_hash's scenarios never
// execute the mechanic that changed. A pin is green through exactly that.
//
// So the detector needs a SECOND, INDEPENDENT answer to "did sim behaviour
// change?", and the goldens are it: six scenarios chosen for different coverage,
// pinned by their own suite, and — the point — not the scenarios in
// build_hash.cpp. When the goldens see a change the digest does not, the two
// disagree, and that disagreement IS the violation.
//
// It is a DELTA detector: it fires on the commit that introduces the divergence,
// and re-pinning both constants silences it. That is intended — the resolution
// is a judgement call, not something a test can make — but it means the review
// artifact is the diff. kGoldenFingerprint moving while kBuildHash stands still
// is the shape to refuse.
//
// What it still cannot see: a change NEITHER set of scenarios executes. Coverage
// is not transitive, and the answer to that is a scenario, not a bigger fold.
TEST_CASE("determinism rule 7: a sim change the goldens can see must move build_hash") {
    const std::uint64_t fingerprint = golden::fingerprint();
    const std::uint32_t digest = bomber::net::build_hash();
    MESSAGE("golden fingerprint = " << hex64(fingerprint) << "   build_hash = " << digest << "u");

    const bool goldens_moved = fingerprint != kGoldenFingerprint;
    const bool digest_moved = digest != kBuildHash;

    if (goldens_moved && !digest_moved) {
        FAIL_CHECK(
            "DETERMINISM RULE 7: sim behaviour moved (a golden scenario's hash changed) but "
            "build_hash did NOT. Two builds of this repo would now simulate differently and "
            "still shake hands at the lobby door. Either the change is a bug in libs/sim, or "
            "build_hash.cpp's scenarios do not EXECUTE the mechanic you changed — in which "
            "case add or extend one (placement is not coverage), confirm the digest moves, and "
            "re-pin both constants below.");
    }
    if (digest_moved && !goldens_moved) {
        MESSAGE(
            "build_hash moved and the goldens did not: a change only build_hash's scenarios "
            "reach. Legal, and the digest doing its job. Re-pin kBuildHash.");
    }

    // The pins themselves, so a move cannot pass unremarked even when both
    // agreed. Recapture is a two-line diff and belongs in the same commit as the
    // behaviour change (rule 5's discipline, applied to the digest).
    CHECK(fingerprint == kGoldenFingerprint);
    CHECK(digest == kBuildHash);
}

// PLACEMENT IS NOT COVERAGE, asserted rather than intended. Scenarios 7 and 8
// exist because the digest was blind to jelly bombs, trigger bombs and rovers,
// and the way that blindness comes BACK is not someone deleting a scenario — it
// is an edit that leaves the scenario running while its mechanic stops being
// reached. The hash is identical either way; these counts are not.
TEST_CASE("build_hash's driven scenarios really execute their mechanics") {
    const bomber::net::BuildHashCoverage cov = bomber::net::build_hash_coverage();
    MESSAGE("jelly_bounces=" << cov.jelly_bounces << " trigger_bombs=" << cov.trigger_bombs
                             << " trigger_detonations=" << cov.trigger_detonations
                             << " rovers_spawned=" << cov.rovers_spawned << " rover_deaths="
                             << cov.rover_deaths << " rover_kills=" << cov.rover_kills);
    CHECK(cov.jelly_bounces > 0);
    CHECK(cov.trigger_bombs > 0);
    CHECK(cov.trigger_detonations > 0);
    CHECK(cov.rovers_spawned > 0);
    CHECK(cov.rover_deaths > 0);
    CHECK(cov.rover_kills > 0);
}
