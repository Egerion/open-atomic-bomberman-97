// bomber::net LinkProbe tests (link_probe.hpp): the mutual path verification
// that stands between a punch — or a relay allocation — and Phase::Ready.
//
// The punch's own proof is ONE-SIDED: receiving the PONG for your PING proves
// the path for YOU and says nothing about what the peer concluded before its own
// deadline. These cases pin the two properties that fix follows from — neither
// end reports verified() until BOTH directions are proven, and a path that never
// answers expires instead of being reported as usable — plus the punch echo that
// stops a winner from starving a peer that is still punching.
//
// Runs over the deterministic LoopbackLink, so there are no sockets and no wall
// clock: latency and loss are dialled in and the outcome is reproducible.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <cstdint>
#include <vector>

#include "bomber/net/link_probe.hpp"
#include "bomber/net/protocol.hpp"
#include "bomber/net/transport.hpp"

using namespace bomber::net;  // NOLINT(google-build-using-namespace) — test-local

namespace {

constexpr std::uint32_t kNonceA = 0x1111'1111u;
constexpr std::uint32_t kNonceB = 0x2222'2222u;

// One pump of both ends plus the link's delivery clock, on a synthetic ms clock.
struct Pair {
    LoopbackLink link;
    LoopbackTransport ta{link, 0};
    LoopbackTransport tb{link, 1};

    explicit Pair(int latency = 0, int drop_every = 0) : link(latency, drop_every) {}
};

}  // namespace

TEST_CASE("both ends verify only once BOTH directions are proven") {
    Pair p;
    LinkProbe a(p.ta, kNonceA, /*deadline_ms=*/5000);
    LinkProbe b(p.tb, kNonceB, /*deadline_ms=*/5000);

    std::int64_t now = 0;
    // The exchange itself needs a couple of pumps; the linger deliberately holds
    // both ends a little longer so neither one's completion depends on the
    // other's silence (link_probe.hpp LINGER).
    for (; now < 1000 && !(a.verified() && b.verified()); now += 10) {
        a.step(now);
        b.step(now);
        p.link.step();
    }
    CHECK(a.verified());
    CHECK(b.verified());
    CHECK_FALSE(a.expired());
    CHECK_FALSE(b.expired());
    // Verification is NOT instantaneous: the linger is what makes it mutual
    // rather than merely local.
    CHECK(now >= kProbeLingerMs);
}

TEST_CASE("a path nobody answers expires instead of reporting a usable link") {
    // The production failure in one line: seat 0 held a relay allocation and seat
    // 1 never made one, so every datagram was discarded at the forwarder. A probe
    // over that path must say so rather than let the match layer start.
    Pair p;
    LinkProbe a(p.ta, kNonceA, /*deadline_ms=*/1000);

    for (std::int64_t now = 0; now <= 1200 && !a.expired(); now += 10) {
        a.step(now);
        p.link.step();  // the peer never pumps: nothing ever answers
    }
    CHECK(a.expired());
    CHECK_FALSE(a.verified());
    CHECK_FALSE(a.peer_seen());
}

TEST_CASE("one-way traffic is not enough — hearing the peer proves only half") {
    // A probe arriving proves peer->me. It must NOT be mistaken for proof that
    // our own datagrams are getting through, which is exactly the inference the
    // 2-peer punch made and the reason a one-sided match could start at all.
    Pair p;
    LinkProbe a(p.ta, kNonceA, /*deadline_ms=*/800);

    // Hand-feed A a probe from a peer that has never seen us.
    for (std::int64_t now = 0; now <= 900 && !a.expired(); now += 10) {
        const std::vector<std::uint8_t> from_peer = encode_probe(kNonceB, /*seen_peer=*/false);
        p.link.send(/*from=*/1, from_peer.data(), from_peer.size());
        a.step(now);
        p.link.step();
    }
    CHECK(a.peer_seen());     // we can hear them
    CHECK_FALSE(a.verified());  // ... but they never said they could hear us
    CHECK(a.expired());
}

TEST_CASE("a reflected copy of our own probe is not a peer") {
    // The star hub reflects a guest's datagram to the other guests, so our own
    // bytes can come back to us. Verifying off that would be verifying against
    // ourselves.
    Pair p;
    LinkProbe a(p.ta, kNonceA, /*deadline_ms=*/600);

    for (std::int64_t now = 0; now <= 700 && !a.expired(); now += 10) {
        const std::vector<std::uint8_t> mirror = encode_probe(kNonceA, /*seen_peer=*/true);
        p.link.send(/*from=*/1, mirror.data(), mirror.size());
        a.step(now);
        p.link.step();
    }
    CHECK_FALSE(a.peer_seen());
    CHECK_FALSE(a.verified());
    CHECK(a.expired());
}

TEST_CASE("a hole-punch PING is echoed while we verify") {
    // THE STARVATION FIX. LobbyFlow stops pumping the punch the moment it
    // connects and Rendezvous::step() returns early when it is no longer
    // Punching, so the winner used to go silent — and a peer still waiting for
    // its own PONG then timed out and relayed alone. Whoever holds the path must
    // keep answering.
    Pair p;
    LinkProbe a(p.ta, kNonceA, /*deadline_ms=*/1000);

    const std::vector<std::uint8_t> ping = encode_punch(0xDEAD'BEEFu, /*is_pong=*/false);
    p.link.send(/*from=*/1, ping.data(), ping.size());

    bool pong_seen = false;
    std::vector<std::uint8_t> got;
    for (std::int64_t now = 0; now <= 200 && !pong_seen; now += 10) {
        a.step(now);
        p.link.step();
        while (p.tb.poll(&got)) {
            Message m;
            if (!decode(got.data(), got.size(), &m)) continue;
            if (m.type == MsgType::Punch && m.punch.is_pong && m.punch.nonce == 0xDEAD'BEEFu)
                pong_seen = true;
        }
    }
    CHECK(pong_seen);
}

TEST_CASE("verification survives a lossy path") {
    // Every third datagram from each side is dropped. The probe re-sends on an
    // interval for exactly this reason, so a link that carries most of its
    // traffic must still be recognised as carrying rather than pushed onto the
    // relay for no reason.
    Pair p(/*latency=*/2, /*drop_every=*/3);
    LinkProbe a(p.ta, kNonceA, /*deadline_ms=*/3000);
    LinkProbe b(p.tb, kNonceB, /*deadline_ms=*/3000);

    for (std::int64_t now = 0; now <= 3000 && !(a.verified() && b.verified()); now += 10) {
        a.step(now);
        b.step(now);
        p.link.step();
    }
    CHECK(a.verified());
    CHECK(b.verified());
}
