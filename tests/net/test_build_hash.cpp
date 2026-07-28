#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "bomber/net/build_hash.hpp"

// The cross-build compatibility digest (ADR-0011): peers compare it before a
// match to reject incompatible builds. We can't pin its exact value here without
// duplicating test_golden's job (a deliberate sim change SHOULD shift it), so we
// assert its contract: deterministic, stable across calls, and non-degenerate.
TEST_CASE("build_hash is stable and non-degenerate") {
    const std::uint32_t a = bomber::net::build_hash();
    const std::uint32_t b = bomber::net::build_hash();
    // Printed, not pinned: build_hash.cpp's own rule is that a sim behaviour
    // change must be MEASURED to move the digest (before vs after). Running this
    // suite is how you take that measurement, so it has to say the number.
    MESSAGE("build_hash = " << a);
    CHECK(a == b);          // deterministic + cached
    CHECK(a != 0u);         // the reference scenario actually ran
    CHECK(a != 0xFFFFFFFFu);
    // Not merely the wire-protocol mix with a zero scenario hash (would mean the
    // sim contributed nothing).
    CHECK(a != (bomber::net::kWireProtocolVersion * 0x9E3779B1u));
}
