// bomber::net SetupSession (setup_session.hpp) — the host-authoritative match
// setup that replaces netplay's old hard-coded canonical config.
//
// Shape under test, from docs/re/network-screens.md §7: the HOST drives the
// roster and level screens and broadcasts every change; guests are read-only.
// Two things must hold or online play is broken in ways that only show up on
// someone else's network:
//   * the guest ends up with a config EQUAL FIELD FOR FIELD to the host's, so
//     both Simulations are built from identical bytes;
//   * nothing partial is ever applied — a config that lost a chunk must leave
//     the guest with no config at all, not with half of one.
// Everything runs over LoopbackLink, including its deterministic `drop_every`
// packet loss, so the whole flow is reproducible without a socket.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "bomber/net/match_config_codec.hpp"
#include "bomber/net/protocol.hpp"
#include "bomber/net/setup_session.hpp"
#include "bomber/net/transport.hpp"
#include "fanout_bus.hpp"  // bomber::test::StarBus / StarTransport (the N-peer cases)
#include "match_config_compare.hpp"

using namespace bomber;  // NOLINT(google-build-using-namespace) — test-local

namespace {

// The 2-peer pair most cases below run, spelled as the SEAT MASKS the session
// takes since it went N-peer: the host owns seat 0 and waits on seat 1's ack,
// the guest owns seat 1 (and, being a guest, waits on nobody).
constexpr std::uint16_t kHostSeat = 0b01;
constexpr std::uint16_t kGuestSeat = 0b10;

// A Transport shim that can swallow selected outbound datagrams (a chunk index,
// an ack) and counts what went past. Lets a test model "this one packet never
// arrives" precisely, rather than hoping a loss ratio hits the right one.
class FilterTransport : public net::Transport {
public:
    explicit FilterTransport(net::Transport& inner) : inner_(&inner) {}

    void send(const std::uint8_t* data, std::size_t size) override {
        if (drop && drop(data, size)) {
            ++dropped;
            return;
        }
        ++sent;
        inner_->send(data, size);
    }
    bool poll(std::vector<std::uint8_t>* out) override { return inner_->poll(out); }

    std::function<bool(const std::uint8_t*, std::size_t)> drop;
    int sent = 0;
    int dropped = 0;

private:
    net::Transport* inner_;
};

bool is_type(const std::uint8_t* d, std::size_t n, net::MsgType t) {
    return n >= 1 && d[0] == static_cast<std::uint8_t>(t);
}

// One pump round for both peers plus the link's delivery clock. 60 ms per round
// means the 200/250 ms re-send intervals fire every few rounds, so loss
// recovery is genuinely exercised instead of being hidden by a fast retry.
constexpr int kStepMs = 60;

void pump(net::LoopbackLink& link, net::SetupSession& host, net::SetupSession& guest,
          std::int64_t& now, int rounds) {
    for (int i = 0; i < rounds; ++i) {
        host.step(now);
        guest.step(now);
        link.step();
        now += kStepMs;
    }
}

net::SetupPreviewFrame sample_preview() {
    net::SetupPreviewFrame p;
    p.level_name = "HAUNTED HOUSE";
    p.level_index = 7;
    p.rounds = 5;
    p.slots = {net::SetupSlotKind::Human, net::SetupSlotKind::Remote, net::SetupSlotKind::Ai,
               net::SetupSlotKind::Ai,    net::SetupSlotKind::Off,    net::SetupSlotKind::Off,
               net::SetupSlotKind::Off,   net::SetupSlotKind::Off,    net::SetupSlotKind::Off,
               net::SetupSlotKind::Off};
    p.team = {1, 1, 2, 2, 0, 0, 0, 0, 0, 0};
    return p;
}

bool previews_match(const net::SetupPreviewFrame& a, const net::SetupPreviewFrame& b) {
    return a.level_name == b.level_name && a.level_index == b.level_index && a.rounds == b.rounds &&
           a.slots == b.slots && a.team == b.team && a.revision == b.revision;
}

std::string join(const std::vector<std::string>& names) {
    std::string s;
    for (const auto& n : names) {
        if (!s.empty()) s += ", ";
        s += n;
    }
    return s;
}

}  // namespace

TEST_CASE("the three setup frames round-trip and reject malformed payloads") {
    net::Message m;

    SUBCASE("preview") {
        const net::SetupPreviewFrame p = sample_preview();
        const std::vector<std::uint8_t> packet = net::encode_setup_preview(p);
        CHECK(packet.size() == 1 + 4 + 1 + 1 + 1 + p.level_name.size() + 20);
        REQUIRE(net::decode(packet.data(), packet.size(), &m));
        CHECK(m.type == net::MsgType::SetupPreview);
        CHECK(previews_match(m.setup_preview, p));

        for (std::size_t n = 0; n < packet.size(); ++n)
            CHECK_FALSE(net::decode(packet.data(), n, &m));
        std::vector<std::uint8_t> longer = packet;
        longer.push_back(0);
        CHECK_FALSE(net::decode(longer.data(), longer.size(), &m));

        std::vector<std::uint8_t> bad = packet;
        bad[7] = static_cast<std::uint8_t>(net::kSetupLevelNameMax + 1);  // name_len
        CHECK_FALSE(net::decode(bad.data(), bad.size(), &m));
        bad = packet;
        bad[8] = 0;  // a NUL inside the level name
        CHECK_FALSE(net::decode(bad.data(), bad.size(), &m));
        bad = packet;
        bad[8 + p.level_name.size()] = 4;  // a slot kind past Remote
        CHECK_FALSE(net::decode(bad.data(), bad.size(), &m));
    }

    SUBCASE("chunk") {
        net::SetupChunkFrame c;
        c.revision = 9;
        c.total_len = 1500;
        c.checksum = 0xABCD1234u;
        c.chunk_count = 2;
        c.chunk_index = 1;
        c.payload.assign(1500 - net::kSetupChunkPayloadBytes, 0x5Au);
        const std::vector<std::uint8_t> packet = net::encode_setup_chunk(c);
        REQUIRE(net::decode(packet.data(), packet.size(), &m));
        CHECK(m.type == net::MsgType::SetupChunk);
        CHECK(m.setup_chunk.revision == 9);
        CHECK(m.setup_chunk.total_len == 1500);
        CHECK(m.setup_chunk.checksum == 0xABCD1234u);
        CHECK(m.setup_chunk.chunk_count == 2);
        CHECK(m.setup_chunk.chunk_index == 1);
        CHECK(m.setup_chunk.payload == c.payload);

        for (std::size_t n = 0; n < packet.size(); ++n)
            CHECK_FALSE(net::decode(packet.data(), n, &m));

        std::vector<std::uint8_t> bad = packet;
        bad[13] = 3;  // a chunk_count that ceil(1500/1024) does not imply
        CHECK_FALSE(net::decode(bad.data(), bad.size(), &m));
        bad = packet;
        bad[14] = 2;  // an index outside the count
        CHECK_FALSE(net::decode(bad.data(), bad.size(), &m));
        bad = packet;
        bad[5] = bad[6] = bad[7] = bad[8] = 0;  // total_len 0
        CHECK_FALSE(net::decode(bad.data(), bad.size(), &m));
        bad = packet;
        bad[8] = 0xFF;  // total_len far past kMaxMatchConfigBytes
        CHECK_FALSE(net::decode(bad.data(), bad.size(), &m));

        // A last chunk claiming a FULL payload lies about its own slice length.
        net::SetupChunkFrame liar = c;
        liar.payload.assign(net::kSetupChunkPayloadBytes, 0);
        const std::vector<std::uint8_t> lie = net::encode_setup_chunk(liar);
        CHECK_FALSE(net::decode(lie.data(), lie.size(), &m));
    }

    SUBCASE("ack") {
        const std::vector<std::uint8_t> packet = net::encode_setup_ack(7, 0xFEEDFACEu, 3);
        CHECK(packet.size() == 10);  // wire v5: the seat byte is what makes N peers work
        REQUIRE(net::decode(packet.data(), packet.size(), &m));
        CHECK(m.type == net::MsgType::SetupAck);
        CHECK(m.setup_ack.revision == 7);
        CHECK(m.setup_ack.checksum == 0xFEEDFACEu);
        CHECK(m.setup_ack.seat == 3);
        for (std::size_t n = 0; n < packet.size(); ++n)
            CHECK_FALSE(net::decode(packet.data(), n, &m));
        std::vector<std::uint8_t> longer = packet;
        longer.push_back(0);
        CHECK_FALSE(net::decode(longer.data(), longer.size(), &m));

        // The seat indexes a per-seat mask, so it is bounds-checked at the wire
        // exactly as MsgType::Drop's is.
        for (int seat = sim::kMaxPlayers; seat < 256; ++seat) {
            std::vector<std::uint8_t> bad = packet;
            bad[9] = static_cast<std::uint8_t>(seat);
            CHECK_FALSE(net::decode(bad.data(), bad.size(), &m));
        }
    }
}

TEST_CASE("the host's live preview reaches the guest; the guest cannot drive setup") {
    net::LoopbackLink link;
    net::LoopbackTransport ta(link, 0);
    net::LoopbackTransport tb(link, 1);
    net::SetupSession host(ta, /*is_host=*/true, kHostSeat, kGuestSeat);
    net::SetupSession guest(tb, /*is_host=*/false, kGuestSeat, 0);
    std::int64_t now = 0;

    CHECK(host.phase() == net::SetupSession::Phase::Waiting);
    CHECK(guest.phase() == net::SetupSession::Phase::Waiting);

    host.publish(sample_preview());
    CHECK(host.has_preview());
    CHECK(host.revision() == 1);
    pump(link, host, guest, now, 6);

    REQUIRE(guest.has_preview());
    CHECK(guest.phase() == net::SetupSession::Phase::Live);
    CHECK(previews_match(host.preview(), guest.preview()));
    CHECK(guest.preview().level_name == "HAUNTED HOUSE");
    CHECK(guest.preview().slots[1] == net::SetupSlotKind::Remote);
    CHECK(guest.preview().team[2] == 2);

    SUBCASE("a second publish supersedes the first") {
        net::SetupPreviewFrame p2 = sample_preview();
        p2.level_name = "GIGGLING GHOSTS";
        p2.level_index = 3;
        host.publish(p2);
        CHECK(host.revision() == 2);
        pump(link, host, guest, now, 6);
        CHECK(guest.preview().level_name == "GIGGLING GHOSTS");
        CHECK(guest.revision() == 2);
    }

    SUBCASE("a stale preview arriving late does not roll the display back") {
        net::SetupPreviewFrame p2 = sample_preview();
        p2.level_name = "GIGGLING GHOSTS";
        host.publish(p2);
        pump(link, host, guest, now, 6);
        REQUIRE(guest.revision() == 2);

        // Replay revision 1 straight onto the wire, as a reordering network
        // would. UDP gives no ordering guarantee, so this must be discarded.
        net::SetupPreviewFrame stale = sample_preview();
        stale.revision = 1;
        const std::vector<std::uint8_t> packet = net::encode_setup_preview(stale);
        ta.send(packet.data(), packet.size());
        pump(link, host, guest, now, 4);
        CHECK(guest.revision() == 2);
        CHECK(guest.preview().level_name == "GIGGLING GHOSTS");
    }

    SUBCASE("guest publish/confirm are ignored — guests are read-only") {
        net::SetupPreviewFrame theirs = sample_preview();
        theirs.level_name = "GUEST WUZ HERE";
        guest.publish(theirs);
        guest.confirm(sim::test::distinctive_match_config());
        pump(link, host, guest, now, 8);

        CHECK(guest.preview().level_name == "HAUNTED HOUSE");  // unchanged
        CHECK_FALSE(guest.has_final_config());
        CHECK(host.has_preview());  // the host still owns the only setup there is
        CHECK(host.preview().level_name == "HAUNTED HOUSE");
        CHECK_FALSE(host.has_final_config());
        CHECK(host.phase() == net::SetupSession::Phase::Live);
    }
}

TEST_CASE("the confirmed config reaches the guest byte-for-byte and is acknowledged") {
    net::LoopbackLink link;
    net::LoopbackTransport ta(link, 0);
    net::LoopbackTransport tb(link, 1);
    net::SetupSession host(ta, /*is_host=*/true, kHostSeat, kGuestSeat);
    net::SetupSession guest(tb, /*is_host=*/false, kGuestSeat, 0);
    std::int64_t now = 0;

    const sim::MatchConfig cfg = sim::test::distinctive_match_config();
    // The whole reason this payload is chunked at all.
    REQUIRE(net::encode_match_config(cfg).size() > net::kSetupChunkPayloadBytes);

    host.publish(sample_preview());
    pump(link, host, guest, now, 4);
    host.confirm(cfg);
    CHECK(host.phase() == net::SetupSession::Phase::Confirming);
    CHECK(host.has_final_config());  // the host holds it immediately...
    CHECK_FALSE(host.peer_acked());  // ...but must not start until the guest does

    pump(link, host, guest, now, 12);

    REQUIRE(guest.has_final_config());
    CHECK(guest.phase() == net::SetupSession::Phase::Final);
    const std::vector<std::string> diffs =
        sim::test::match_config_diffs(host.final_config(), guest.final_config());
    INFO("fields that did not survive the wire: " << join(diffs));
    CHECK(diffs.empty());

    CHECK(host.peer_acked());
    CHECK(host.phase() == net::SetupSession::Phase::Final);
    CHECK(guest.revision() == host.revision());
}

TEST_CASE("a chunked config converges over a lossy link") {
    // Every 3rd datagram from each side is dropped, deterministically — both
    // the host's chunks and the guest's acks.
    net::LoopbackLink link(/*latency=*/1, /*drop_every=*/3);
    net::LoopbackTransport ta(link, 0);
    net::LoopbackTransport tb(link, 1);
    net::SetupSession host(ta, /*is_host=*/true, kHostSeat, kGuestSeat);
    net::SetupSession guest(tb, /*is_host=*/false, kGuestSeat, 0);
    std::int64_t now = 0;

    host.publish(sample_preview());
    pump(link, host, guest, now, 10);
    CHECK(guest.has_preview());  // the preview re-send rides out the loss too

    host.confirm(sim::test::distinctive_match_config());
    pump(link, host, guest, now, 80);

    REQUIRE(guest.has_final_config());
    CHECK(guest.phase() == net::SetupSession::Phase::Final);
    CHECK(sim::test::match_config_diffs(host.final_config(), guest.final_config()).empty());
    CHECK(host.peer_acked());
}

TEST_CASE("a missing chunk never yields a partially applied config") {
    net::LoopbackLink link;
    net::LoopbackTransport raw_a(link, 0);
    FilterTransport ta(raw_a);
    net::LoopbackTransport tb(link, 1);
    net::SetupSession host(ta, /*is_host=*/true, kHostSeat, kGuestSeat);
    net::SetupSession guest(tb, /*is_host=*/false, kGuestSeat, 0);
    std::int64_t now = 0;

    // Chunk 1 is never allowed onto the wire; chunk 0 keeps arriving.
    int blocked_index = 1;
    ta.drop = [&blocked_index](const std::uint8_t* d, std::size_t n) {
        return is_type(d, n, net::MsgType::SetupChunk) && n > 14 &&
               d[14] == static_cast<std::uint8_t>(blocked_index);
    };

    host.confirm(sim::test::distinctive_match_config());
    pump(link, host, guest, now, 40);

    CHECK(ta.dropped > 0);
    CHECK_FALSE(guest.has_final_config());
    CHECK(guest.phase() != net::SetupSession::Phase::Final);
    CHECK_FALSE(host.peer_acked());
    CHECK(host.phase() == net::SetupSession::Phase::Confirming);

    // Let the missing slice through: reassembly completes from the next burst.
    blocked_index = -1;
    pump(link, host, guest, now, 12);
    REQUIRE(guest.has_final_config());
    CHECK(sim::test::match_config_diffs(host.final_config(), guest.final_config()).empty());
    CHECK(host.peer_acked());
}

TEST_CASE("a lost ack is recovered by the guest's re-ack on the next chunk") {
    net::LoopbackLink link;
    net::LoopbackTransport ta(link, 0);
    net::LoopbackTransport raw_b(link, 1);
    FilterTransport tb(raw_b);
    net::SetupSession host(ta, /*is_host=*/true, kHostSeat, kGuestSeat);
    net::SetupSession guest(tb, /*is_host=*/false, kGuestSeat, 0);
    std::int64_t now = 0;

    bool swallow_acks = true;
    tb.drop = [&swallow_acks](const std::uint8_t* d, std::size_t n) {
        return swallow_acks && is_type(d, n, net::MsgType::SetupAck);
    };

    host.confirm(sim::test::distinctive_match_config());
    pump(link, host, guest, now, 16);
    REQUIRE(guest.has_final_config());  // the guest is done...
    CHECK_FALSE(host.peer_acked());     // ...but the host never heard so
    CHECK(tb.dropped > 0);

    swallow_acks = false;
    pump(link, host, guest, now, 12);
    CHECK(host.peer_acked());
    CHECK(host.phase() == net::SetupSession::Phase::Final);
}

TEST_CASE("garbage and foreign datagrams on the shared socket are ignored") {
    net::LoopbackLink link;
    net::LoopbackTransport ta(link, 0);
    net::LoopbackTransport tb(link, 1);
    net::SetupSession host(ta, /*is_host=*/true, kHostSeat, kGuestSeat);
    net::SetupSession guest(tb, /*is_host=*/false, kGuestSeat, 0);
    std::int64_t now = 0;

    host.publish(sample_preview());
    pump(link, host, guest, now, 6);
    REQUIRE(guest.has_preview());

    // A trailing hole-punch PONG (a real possibility on this socket), a
    // truncated chunk header, an unknown tag, and pure noise.
    const std::vector<std::uint8_t> punch = net::encode_punch(0x1234u, true);
    ta.send(punch.data(), punch.size());
    const std::vector<std::uint8_t> stub = {static_cast<std::uint8_t>(net::MsgType::SetupChunk), 1,
                                            2};
    ta.send(stub.data(), stub.size());
    const std::vector<std::uint8_t> unknown = {200, 1, 2, 3};
    ta.send(unknown.data(), unknown.size());
    std::vector<std::uint8_t> noise(1400);
    for (std::size_t i = 0; i < noise.size(); ++i) noise[i] = static_cast<std::uint8_t>(i * 7 + 3);
    ta.send(noise.data(), noise.size());

    CHECK_NOTHROW(pump(link, host, guest, now, 8));
    CHECK(guest.phase() == net::SetupSession::Phase::Live);
    CHECK_FALSE(guest.has_final_config());
    CHECK(guest.preview().level_name == "HAUNTED HOUSE");
}

TEST_CASE("a guest whose host goes silent times out") {
    net::LoopbackLink link;
    net::LoopbackTransport ta(link, 0);
    net::LoopbackTransport tb(link, 1);
    net::SetupSession host(ta, /*is_host=*/true, kHostSeat, kGuestSeat);  // never publishes
    net::SetupSession guest(tb, /*is_host=*/false, kGuestSeat, 0, /*timeout_ms=*/500);
    std::int64_t now = 0;

    pump(link, host, guest, now, 4);
    CHECK(guest.phase() == net::SetupSession::Phase::Waiting);
    pump(link, host, guest, now, 10);
    CHECK(guest.phase() == net::SetupSession::Phase::Failed);
    CHECK(guest.failed());
    // The host, meanwhile, is idle by design: editing time never expires.
    CHECK(host.phase() == net::SetupSession::Phase::Waiting);
}

TEST_CASE("a host whose guest never acknowledges times out after confirm") {
    net::LoopbackLink link;
    net::LoopbackTransport raw_a(link, 0);
    FilterTransport ta(raw_a);
    net::LoopbackTransport tb(link, 1);
    net::SetupSession host(ta, /*is_host=*/true, kHostSeat, kGuestSeat, /*timeout_ms=*/500);
    net::SetupSession guest(tb, /*is_host=*/false, kGuestSeat, 0);
    std::int64_t now = 0;

    ta.drop = [](const std::uint8_t* d, std::size_t n) {
        return is_type(d, n, net::MsgType::SetupChunk);  // the guest hears nothing
    };
    host.confirm(sim::test::distinctive_match_config());
    pump(link, host, guest, now, 4);
    CHECK(host.phase() == net::SetupSession::Phase::Confirming);
    pump(link, host, guest, now, 10);
    CHECK(host.phase() == net::SetupSession::Phase::Failed);
    CHECK_FALSE(guest.has_final_config());
}

TEST_CASE("re-confirming supersedes the previous config") {
    net::LoopbackLink link;
    net::LoopbackTransport ta(link, 0);
    net::LoopbackTransport tb(link, 1);
    net::SetupSession host(ta, /*is_host=*/true, kHostSeat, kGuestSeat);
    net::SetupSession guest(tb, /*is_host=*/false, kGuestSeat, 0);
    std::int64_t now = 0;

    sim::MatchConfig first = sim::test::distinctive_match_config();
    first.seed = 0x11111111u;
    host.confirm(first);
    pump(link, host, guest, now, 12);
    REQUIRE(guest.has_final_config());
    CHECK(guest.final_config().seed == 0x11111111u);

    sim::MatchConfig second = sim::test::distinctive_match_config();
    second.seed = 0x22222222u;
    host.confirm(second);
    CHECK(host.phase() == net::SetupSession::Phase::Confirming);
    pump(link, host, guest, now, 12);
    CHECK(guest.final_config().seed == 0x22222222u);
    CHECK(host.peer_acked());
    CHECK(sim::test::match_config_diffs(host.final_config(), guest.final_config()).empty());
}

// --- MORE THAN TWO PEERS -----------------------------------------------------
//
// The whole reason SetupAck grew a seat byte (wire v5). Everything below runs
// over the StarBus in tests/common/fanout_bus.hpp, which is the PRODUCTION
// topology, not a broadcast: a guest's datagram reaches only the hub, and the
// other guests hear it only because the hub reflects it inside its own poll().
// So these cases also prove the hub keeps the star alive by being pumped —
// which is exactly what setup_session.hpp's one-pump-at-a-time rule protects.

namespace {

// One SetupSession per endpoint of an N-way star, seat i on endpoint i.
struct StarPeers {
    explicit StarPeers(std::size_t n, int timeout_ms = 30000) : bus(n) {
        transports.reserve(n);
        for (std::size_t i = 0; i < n; ++i)
            transports.push_back(std::make_unique<test::StarTransport>(bus, i));
        const auto all = static_cast<std::uint16_t>((1U << n) - 1U);
        sessions.reserve(n);
        for (std::size_t i = 0; i < n; ++i) {
            const auto mine = static_cast<std::uint16_t>(1U << i);
            sessions.push_back(std::make_unique<net::SetupSession>(
                *transports[i], /*is_host=*/i == 0, mine,
                static_cast<std::uint16_t>(all & ~mine), timeout_ms));
        }
    }

    net::SetupSession& host() { return *sessions[0]; }
    net::SetupSession& peer(std::size_t i) { return *sessions[i]; }

    // Pump every peer, then advance the bus clock. `skip` is the endpoint that is
    // NOT pumped this round (kNone = pump them all) — a peer that never steps
    // sends nothing, which is how a guest goes silent for real.
    void pump(int rounds, std::size_t skip = kNone) {
        for (int i = 0; i < rounds; ++i) {
            for (std::size_t p = 0; p < sessions.size(); ++p)
                if (p != skip) sessions[p]->step(now);
            bus.step();
            now += 60;  // the 200/250 ms re-send intervals fire every few rounds
        }
    }

    static constexpr std::size_t kNone = static_cast<std::size_t>(-1);

    test::StarBus bus;
    std::vector<std::unique_ptr<test::StarTransport>> transports;
    std::vector<std::unique_ptr<net::SetupSession>> sessions;
    std::int64_t now = 0;
};

}  // namespace

TEST_CASE("a 4-peer star: the host reaches Final only after EVERY guest has acked") {
    StarPeers s(4);  // hub + 3 guests, seats 0..3
    const sim::MatchConfig cfg = sim::test::distinctive_match_config();

    s.host().publish(sample_preview());
    s.pump(6);
    for (std::size_t i = 1; i < 4; ++i) {
        INFO("guest " << i);
        REQUIRE(s.peer(i).has_preview());  // the hub's fan-out reached all of them
        CHECK(s.peer(i).preview().level_name == "HAUNTED HOUSE");
    }

    s.host().confirm(cfg);
    CHECK(s.host().phase() == net::SetupSession::Phase::Confirming);
    CHECK(s.host().pending_seats() == 0b1110);

    s.pump(20);

    CHECK(s.host().phase() == net::SetupSession::Phase::Final);
    CHECK(s.host().peer_acked());
    CHECK(s.host().acked_seats() == 0b1110);  // each guest answered for its OWN seat
    CHECK(s.host().pending_seats() == 0);
    for (std::size_t i = 1; i < 4; ++i) {
        INFO("guest " << i);
        REQUIRE(s.peer(i).has_final_config());
        CHECK(s.peer(i).phase() == net::SetupSession::Phase::Final);
        CHECK(sim::test::match_config_diffs(cfg, s.peer(i).final_config()).empty());
    }
}

TEST_CASE("a 4-peer star: one silent guest keeps the host OUT of Final") {
    // THE REGRESSION THIS WHOLE CHANGE EXISTS FOR. With an unattributed ack the
    // host latched Final on the first one to arrive and started the match while a
    // third peer was still building its board — a guaranteed tick-0 desync.
    StarPeers s(4, /*timeout_ms=*/100000);  // long enough that the wait is the test
    s.host().confirm(sim::test::distinctive_match_config());

    // Seat 3 never pumps: it neither receives the chunks nor answers them.
    s.pump(40, /*skip=*/3);

    CHECK(s.peer(1).phase() == net::SetupSession::Phase::Final);  // the two live guests are done
    CHECK(s.peer(2).phase() == net::SetupSession::Phase::Final);
    CHECK(s.host().acked_seats() == 0b0110);
    CHECK(s.host().pending_seats() == 0b1000);
    CHECK_FALSE(s.host().peer_acked());
    CHECK(s.host().phase() == net::SetupSession::Phase::Confirming);  // NOT Final

    // And it converges the moment the straggler comes back — the host is still
    // re-sending, so no state had to be kept for it.
    s.pump(20);
    CHECK(s.host().phase() == net::SetupSession::Phase::Final);
    CHECK(s.host().pending_seats() == 0);
}

TEST_CASE("a 4-peer star: a guest that never acks times the host out, it does not hang") {
    StarPeers s(4, /*timeout_ms=*/1000);  // 60 ms per pump round
    s.host().confirm(sim::test::distinctive_match_config());
    s.pump(40, /*skip=*/2);

    CHECK(s.host().phase() == net::SetupSession::Phase::Failed);
    CHECK(s.host().failed());
    CHECK_FALSE(s.host().peer_acked());
    // The guests that DID get it are not punished for the third one's silence:
    // they hold the config and it is the host's screen that reports the failure.
    CHECK(s.peer(1).phase() == net::SetupSession::Phase::Final);
    CHECK(s.peer(3).phase() == net::SetupSession::Phase::Final);
}

TEST_CASE("a 4-peer star: re-confirming clears every seat's ack, not just one") {
    StarPeers s(4);
    sim::MatchConfig first = sim::test::distinctive_match_config();
    first.seed = 0x11111111u;
    s.host().confirm(first);
    s.pump(20);
    REQUIRE(s.host().peer_acked());

    sim::MatchConfig second = sim::test::distinctive_match_config();
    second.seed = 0x22222222u;
    s.host().confirm(second);
    CHECK(s.host().acked_seats() == 0);  // the old acks say nothing about these bytes
    CHECK(s.host().phase() == net::SetupSession::Phase::Confirming);

    s.pump(20);
    CHECK(s.host().peer_acked());
    for (std::size_t i = 1; i < 4; ++i) {
        INFO("guest " << i);
        CHECK(s.peer(i).final_config().seed == 0x22222222u);
    }
}

TEST_CASE("an ack from a seat the host is not waiting on cannot stand in for a real one") {
    // Over a star the hub reflects guest datagrams, so a guest sees the OTHER
    // guests' acks; and an ack could name a seat that is not in this match at
    // all. Neither may satisfy the host's mask.
    net::LoopbackLink link;
    net::LoopbackTransport ta(link, 0);
    net::LoopbackTransport tb(link, 1);
    // The host plays seat 0 and waits on seat 2 only — seat 1 is an AI slot, not
    // a peer.
    net::SetupSession host(ta, /*is_host=*/true, 0b001, 0b100);
    std::int64_t now = 0;

    host.confirm(sim::test::distinctive_match_config());
    host.step(now);
    link.step();

    // Forge the right revision and checksum but the wrong seat. The checksum has
    // to be the real one for the frame to be considered at all, so read it off
    // the chunk the host just sent.
    std::vector<std::uint8_t> chunk;
    REQUIRE(tb.poll(&chunk));
    net::Message m;
    REQUIRE(net::decode(chunk.data(), chunk.size(), &m));
    REQUIRE(m.type == net::MsgType::SetupChunk);

    for (const std::uint8_t seat : {std::uint8_t{0}, std::uint8_t{1}, std::uint8_t{9}}) {
        const std::vector<std::uint8_t> ack =
            net::encode_setup_ack(m.setup_chunk.revision, m.setup_chunk.checksum, seat);
        tb.send(ack.data(), ack.size());
    }
    for (int i = 0; i < 6; ++i) {
        host.step(now);
        link.step();
        now += 60;
    }
    CHECK_FALSE(host.peer_acked());
    CHECK(host.phase() == net::SetupSession::Phase::Confirming);

    // The seat it IS waiting on settles it.
    const std::vector<std::uint8_t> real =
        net::encode_setup_ack(m.setup_chunk.revision, m.setup_chunk.checksum, 2);
    tb.send(real.data(), real.size());
    for (int i = 0; i < 4; ++i) {
        host.step(now);
        link.step();
        now += 60;
    }
    CHECK(host.peer_acked());
    CHECK(host.phase() == net::SetupSession::Phase::Final);
}

TEST_CASE("a machine holding two seats acks for both") {
    // The host's mask is per SEAT, so a guest seating two humans must answer for
    // both or the host would wait out its timeout on the seat that never spoke.
    net::LoopbackLink link;
    net::LoopbackTransport ta(link, 0);
    net::LoopbackTransport tb(link, 1);
    net::SetupSession host(ta, /*is_host=*/true, 0b0001, 0b0110);
    net::SetupSession guest(tb, /*is_host=*/false, 0b0110, 0);
    std::int64_t now = 0;

    host.confirm(sim::test::distinctive_match_config());
    pump(link, host, guest, now, 16);

    REQUIRE(guest.has_final_config());
    CHECK(host.acked_seats() == 0b0110);
    CHECK(host.peer_acked());
    CHECK(host.phase() == net::SetupSession::Phase::Final);
}
