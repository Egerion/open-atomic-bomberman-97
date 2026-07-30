// THE MATCH SHELL (wire v8): the two transitions that used to be local decisions
// and therefore used to end the connection.
//
//   1. Esc during an online round KILLED the session. It also stopped the local
//      sim while the peer kept ticking, so the two had simulated a different
//      number of ticks by the time anything looked at the frozen state. Now Esc
//      is a REQUEST to the host, the host announces one agreed end tick
//      (MatchCtlKind::EndRound), and both peers stop there with the round a draw.
//
//   2. A finished match dropped the transport, so a rematch meant going back
//      through the lobby — which by then has been reaped anyway. Now the host
//      announces the walk back to the setup screens (MatchCtlKind::Rematch) over
//      the same link (net::RematchSession).
//
// EVERY netplay case here drives BOTH peers over a LoopbackLink. A one-sided
// test is what let the previous connection bug through: it proves what the peer
// that pressed the key does and says nothing about the one that did not.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <cstdint>
#include <vector>

#include "bomber/net/protocol.hpp"
#include "bomber/net/rematch_session.hpp"
#include "bomber/net/rollback_session.hpp"
#include "bomber/net/round_rotation.hpp"
#include "bomber/net/transport.hpp"
#include "helpers.hpp"  // bomber::sim::test::open_config

using namespace bomber;  // NOLINT(google-build-using-namespace) — test-local
using bomber::sim::test::open_config;

namespace {

constexpr std::uint16_t kSeat0 = 0x1;
constexpr std::uint16_t kSeat1 = 0x2;
constexpr std::uint16_t kBoth = 0x3;

sim::TickInputs seat_input(int seat, const sim::PlayerInput& in) {
    sim::TickInputs t;
    t.players[static_cast<std::size_t>(seat)] = in;
    return t;
}

// The same scripted walk the rotation suite uses: enough real input that
// prediction genuinely mispredicts and the rollback path is exercised.
sim::PlayerInput scripted(int seat, std::uint32_t tick) {
    sim::PlayerInput in;
    switch ((tick + static_cast<std::uint32_t>(seat) * 3U) % 6U) {
        case 0: in.right = true; break;
        case 1: in.down = true; break;
        case 2: in.left = true; break;
        case 3: in.up = true; break;
        case 4: in.action1 = true; break;
        default: break;
    }
    return in;
}

// Two peers over one link, wired exactly as GameApp::run_netplay_match_seats
// wires them: seat 0 is the host, seat 1 the guest, prediction capped at 8.
struct Pair {
    explicit Pair(int latency = 0, int drop_every = 0)
        : link(latency, drop_every),
          ta(link, 0),
          tb(link, 1),
          sa(open_config()),
          sb(open_config()),
          host(sa, kSeat0, kBoth, 8, ta, net::DropPolicy{false, true, 0}),
          guest(sb, kSeat1, kBoth, 8, tb, net::DropPolicy{false, false, 0}) {}

    // One pump of both peers plus one step of the delivery clock.
    void pump() {
        host.advance(seat_input(0, scripted(0, host.predicted_tick())));
        guest.advance(seat_input(1, scripted(1, guest.predicted_tick())));
        link.step();
    }

    void pump_n(int n) {
        for (int i = 0; i < n; ++i) pump();
    }

    net::LoopbackLink link;
    net::LoopbackTransport ta;
    net::LoopbackTransport tb;
    sim::Simulation sa;
    sim::Simulation sb;
    net::RollbackSession host;
    net::RollbackSession guest;
};

}  // namespace

// --- the codec -------------------------------------------------------------

TEST_CASE("match ctl: the frame round-trips and rejects what a disagreeing peer would send") {
    for (const net::MatchCtlKind k :
         {net::MatchCtlKind::EndRound, net::MatchCtlKind::EndRoundRequest,
          net::MatchCtlKind::RematchWait, net::MatchCtlKind::Rematch}) {
        const std::vector<std::uint8_t> pkt = net::encode_match_ctl(k, 0xDEADBEEFu);
        CHECK(pkt.size() == 6);
        net::Message m;
        REQUIRE(net::decode(pkt.data(), pkt.size(), &m));
        CHECK(m.type == net::MsgType::MatchCtl);
        CHECK(m.match_ctl.kind == k);
        CHECK(m.match_ctl.at_tick == 0xDEADBEEFu);
    }

    net::Message m;
    // An unknown kind is a peer that disagrees with us about the protocol —
    // rejected at the wire, exactly as MsgType::Drop rejects an out-of-range seat.
    std::vector<std::uint8_t> bad = net::encode_match_ctl(net::MatchCtlKind::Rematch, 0);
    bad[1] = 4;
    CHECK_FALSE(net::decode(bad.data(), bad.size(), &m));
    // Short and long buffers both go, so a truncated or padded datagram can never
    // be read as a control message.
    const std::vector<std::uint8_t> ok = net::encode_match_ctl(net::MatchCtlKind::EndRound, 7);
    CHECK_FALSE(net::decode(ok.data(), ok.size() - 1, &m));
    std::vector<std::uint8_t> padded = ok;
    padded.push_back(0);
    CHECK_FALSE(net::decode(padded.data(), padded.size(), &m));
}

// --- transition 1: Esc during a round --------------------------------------

TEST_CASE("abandon: the host's Esc stops BOTH peers at one agreed tick") {
    Pair p(/*latency=*/4);
    p.pump_n(60);
    REQUIRE_FALSE(p.host.end_round_scheduled());
    REQUIRE_FALSE(p.guest.end_round_scheduled());

    const std::uint32_t at_press = p.host.predicted_tick();
    p.host.request_end_round();
    REQUIRE(p.host.end_round_scheduled());
    // The end tick is in the FUTURE, above anything any peer can already have
    // reached: the guest cannot be more than max_prediction past its confirmed
    // frontier, and its confirmed frontier cannot be above the host's own head.
    // An end tick a peer had already passed would leave it having simulated (and
    // tallied) more of the round than the host did.
    CHECK(p.host.end_round_tick() > at_press);
    CHECK(p.host.end_round_tick() >= at_press + 8);
    CHECK(p.guest.predicted_tick() < p.host.end_round_tick());

    // Both keep pumping normally until they reach it.
    for (int i = 0; i < 200 && !(p.host.round_ended() && p.guest.round_ended()); ++i) p.pump();

    CHECK(p.host.round_ended());
    CHECK(p.guest.round_ended());
    // THE PIN: one tick, on both peers, and neither simulated past it.
    CHECK(p.guest.end_round_tick() == p.host.end_round_tick());
    CHECK(p.host.predicted_tick() == p.host.end_round_tick());
    CHECK(p.guest.predicted_tick() == p.host.end_round_tick());
    CHECK_FALSE(p.host.desynced());
    CHECK_FALSE(p.guest.desynced());

    // advance() keeps receiving and re-sending after the end tick (it just stops
    // simulating), so the last speculative ticks still converge: pump a few more
    // and the two states are byte-identical.
    p.pump_n(20);
    CHECK(p.host.predicted_tick() == p.host.end_round_tick());
    CHECK(p.guest.predicted_tick() == p.host.end_round_tick());
    CHECK(p.host.confirmed_tick() == p.host.end_round_tick());
    CHECK(p.guest.confirmed_tick() == p.host.end_round_tick());
    CHECK(p.sa.hash() == p.sb.hash());
}

TEST_CASE("abandon: a GUEST cannot end a round, and does not even ASK") {
    // NARROWED 2026-07-30. A guest used to send MatchCtlKind::EndRoundRequest,
    // which the host converted into the decision — so any guest could force-end
    // any round at will, with no host confirmation. The owner asked for the
    // authority model that removes it, and it is also the safer one.
    //
    // The guest's peer is a bare transport nobody answers on, so every datagram
    // it emits is countable on the other side of the link.
    net::LoopbackLink link(/*latency=*/0);
    net::LoopbackTransport tg(link, 0);
    net::LoopbackTransport tsilent(link, 1);
    sim::Simulation sg(open_config());
    net::RollbackSession guest(sg, kSeat1, kBoth, /*max_prediction=*/8, tg,
                               net::DropPolicy{false, /*is_host=*/false, 0});

    guest.request_end_round();
    CHECK_FALSE(guest.end_round_scheduled());  // it did not decide...

    int requests = 0;
    for (int i = 0; i < 100; ++i) {
        guest.advance(seat_input(1, scripted(1, guest.predicted_tick())));
        link.step();
        std::vector<std::uint8_t> pkt;
        while (tsilent.poll(&pkt)) {
            net::Message m;
            if (net::decode(pkt.data(), pkt.size(), &m) && m.type == net::MsgType::MatchCtl &&
                m.match_ctl.kind == net::MatchCtlKind::EndRoundRequest)
                ++requests;
        }
    }
    CHECK_FALSE(guest.round_ended());
    CHECK_FALSE(guest.end_round_scheduled());
    CHECK(requests == 0);  // ...and it did not ask, either
}

TEST_CASE("abandon: a guest's Esc perturbs NEITHER peer's match") {
    // The requirement stated positively: after the narrowing, a guest pressing
    // Esc must leave both simulations exactly as they were — same ticks, same
    // hash, still progressing. Both peers are driven over the link, because a
    // one-sided test would prove only what the peer that pressed the key does.
    Pair p(/*latency=*/4);
    p.pump_n(60);
    const std::uint32_t host_tick = p.host.predicted_tick();
    const std::uint32_t guest_tick = p.guest.predicted_tick();

    p.guest.request_end_round();
    // Nothing scheduled, anywhere, immediately...
    CHECK_FALSE(p.guest.end_round_scheduled());
    CHECK_FALSE(p.host.end_round_scheduled());
    CHECK(p.host.predicted_tick() == host_tick);
    CHECK(p.guest.predicted_tick() == guest_tick);

    // ...nor after the request has had every chance to travel and be acted on.
    p.pump_n(120);
    CHECK_FALSE(p.host.end_round_scheduled());
    CHECK_FALSE(p.guest.end_round_scheduled());
    CHECK_FALSE(p.host.round_ended());
    CHECK_FALSE(p.guest.round_ended());
    // The match is still RUNNING, not merely un-ended — a test that only checked
    // "no end scheduled" would pass on two frozen peers.
    CHECK(p.host.predicted_tick() > host_tick + 100);
    CHECK(p.guest.predicted_tick() > guest_tick + 100);
    // ...and in agreement the whole way. Deliberately NOT a raw sa.hash() ==
    // sb.hash(): mid-match those are SPECULATIVE states, which differ between the
    // peers moment to moment by design — that is what rollback IS. desynced() is
    // the stronger claim and the right one: it is false only if every CONFIRMED
    // tick's hash matched, over all 180 of them, on both peers.
    CHECK_FALSE(p.host.desynced());
    CHECK_FALSE(p.guest.desynced());
    CHECK(p.host.confirmed_tick() > host_tick);
    CHECK(p.guest.confirmed_tick() > guest_tick);
}

TEST_CASE("abandon: a HOST ignores an EndRoundRequest from an older peer") {
    // The half that actually removes the griefing lever. kWireProtocolVersion is
    // unchanged at 8, so a peer running the PREVIOUS build still connects and
    // still sends this message — refusing to act on it is the only thing that
    // stops it driving us. The kind stays decodable on purpose (the codec case
    // above still round-trips it) so such a peer is turned away, not disconnected.
    Pair p(/*latency=*/0);
    p.pump_n(20);
    const std::vector<std::uint8_t> req =
        net::encode_match_ctl(net::MatchCtlKind::EndRoundRequest, 0);
    for (int i = 0; i < 10; ++i) {
        p.tb.send(req.data(), req.size());  // the guest side of the link
        p.link.step();
        p.host.advance(seat_input(0, scripted(0, p.host.predicted_tick())));
    }
    CHECK_FALSE(p.host.end_round_scheduled());
    CHECK_FALSE(p.host.round_ended());
}

TEST_CASE("abandon: a guest's forged EndRound is not obeyed either") {
    // A guest cannot skip the request and simply ANNOUNCE the decision: EndRound
    // is a host-to-guest message, so a host does not act on an inbound one. (Its
    // own is applied directly by request_end_round, never via the echo.)
    Pair p(/*latency=*/0);
    p.pump_n(20);
    const std::vector<std::uint8_t> forged =
        net::encode_match_ctl(net::MatchCtlKind::EndRound, p.host.predicted_tick() + 5);
    p.tb.send(forged.data(), forged.size());
    p.link.step();
    p.host.advance(seat_input(0, scripted(0, p.host.predicted_tick())));
    CHECK_FALSE(p.host.end_round_scheduled());
}

TEST_CASE("abandon: against a DEAD peer the host's stop never completes — hence the bail-out") {
    // WHY LEAVING CANNOT BE ALLOWED TO NEED THE NETWORK, pinned. The host presses
    // Esc against a peer that has stopped responding: the end tick is scheduled
    // and re-announced forever, but the round can never REACH it, because the
    // host stalls at the prediction cap waiting for input that is not coming. So
    // "press Esc and wait" is not an exit at all, and the only way out is the
    // local double-Esc bail-out (net_esc.hpp, whose own suite pins that the way
    // out stays open for exactly as long as this state persists).
    net::LoopbackLink link(/*latency=*/0);
    net::LoopbackTransport th(link, 0);
    net::LoopbackTransport tdead(link, 1);  // nobody ever pumps this side
    sim::Simulation sh(open_config());
    net::RollbackSession host(sh, kSeat0, kBoth, /*max_prediction=*/8, th,
                              net::DropPolicy{false, /*is_host=*/true, 0});

    for (int i = 0; i < 40; ++i) {
        host.advance(seat_input(0, scripted(0, host.predicted_tick())));
        link.step();
    }
    host.request_end_round();
    REQUIRE(host.end_round_scheduled());
    const std::uint32_t at = host.end_round_tick();

    for (int i = 0; i < 2000; ++i) {
        host.advance(seat_input(0, scripted(0, host.predicted_tick())));
        link.step();
    }
    // Two thousand pumps — a minute and a half of real time — and the round has
    // not ended and never will: the head is stuck at the cap, short of the tick.
    CHECK_FALSE(host.round_ended());
    CHECK(host.predicted_tick() < at);
    CHECK(host.stats().stall_pumps > 0);
    // And it is still shouting into the void, so a peer that came back would
    // still be told. The exit is the player's, not the protocol's.
    std::vector<std::uint8_t> pkt;
    int announcements = 0;
    while (tdead.poll(&pkt)) {
        net::Message m;
        if (net::decode(pkt.data(), pkt.size(), &m) && m.type == net::MsgType::MatchCtl &&
            m.match_ctl.kind == net::MatchCtlKind::EndRound)
            ++announcements;
    }
    CHECK(announcements > 1);
}

TEST_CASE("abandon: the host re-announces every pump — one datagram is not a protocol") {
    // The redundancy, pinned directly. The host's peer is silent, so nothing is
    // consumed and every EndRound the host emits is countable on the other side
    // of the link. This is the assertion that fails if the announcement is ever
    // reduced to a single fire-and-forget send: the loss case below cannot pin it
    // on its own, because a deterministic drop pattern may happen to spare the
    // one copy.
    net::LoopbackLink link(/*latency=*/0);
    net::LoopbackTransport th(link, 0);
    net::LoopbackTransport tsilent(link, 1);
    sim::Simulation sh(open_config());
    net::RollbackSession host(sh, kSeat0, kBoth, /*max_prediction=*/8, th,
                              net::DropPolicy{false, /*is_host=*/true, 0});

    host.request_end_round();
    int announcements = 0;
    for (int i = 0; i < 50; ++i) {
        host.advance(seat_input(0, scripted(0, host.predicted_tick())));
        link.step();
        std::vector<std::uint8_t> pkt;
        while (tsilent.poll(&pkt)) {
            net::Message m;
            if (net::decode(pkt.data(), pkt.size(), &m) && m.type == net::MsgType::MatchCtl &&
                m.match_ctl.kind == net::MatchCtlKind::EndRound)
                ++announcements;
        }
    }
    CHECK(announcements > 1);
}

TEST_CASE("abandon: the announcement survives packet loss") {
    // Every third datagram from each side is dropped, so several copies of
    // EndRound never arrive and the input window behind it is holed too. Both
    // peers still converge on the same tick.
    Pair p(/*latency=*/2, /*drop_every=*/3);
    p.pump_n(60);
    p.host.request_end_round();
    for (int i = 0; i < 400 && !(p.host.round_ended() && p.guest.round_ended()); ++i) p.pump();

    CHECK(p.host.round_ended());
    CHECK(p.guest.round_ended());
    CHECK(p.guest.end_round_tick() == p.host.end_round_tick());
    CHECK(p.host.predicted_tick() == p.guest.predicted_tick());
    CHECK_FALSE(p.host.desynced());
    CHECK_FALSE(p.guest.desynced());
}

TEST_CASE("abandon: an announcement is idempotent and order-free") {
    Pair p(/*latency=*/0);
    p.pump_n(20);
    p.host.request_end_round();
    const std::uint32_t first = p.host.end_round_tick();

    // A second press changes nothing (the decision is already made)...
    p.host.request_end_round();
    CHECK(p.host.end_round_tick() == first);

    // ...and a LATER tick arriving out of order loses to the earlier one, so the
    // peers converge whatever order the copies land in. An EARLIER one wins,
    // which is the same "earliest wins" rule the drop handoff uses.
    const std::vector<std::uint8_t> later =
        net::encode_match_ctl(net::MatchCtlKind::EndRound, first + 50);
    p.ta.send(later.data(), later.size());
    const std::vector<std::uint8_t> earlier =
        net::encode_match_ctl(net::MatchCtlKind::EndRound, first - 2);
    p.ta.send(earlier.data(), earlier.size());
    p.link.step();
    p.guest.advance(seat_input(1, scripted(1, p.guest.predicted_tick())));
    CHECK(p.guest.end_round_tick() == first - 2);
}

TEST_CASE("abandon: a round that is NOT abandoned reports nothing") {
    // The regression guard for the shell's own branch: end_round_scheduled() is
    // what tells "Esc, declare a draw" from "the round ended on its own terms",
    // and a quiet round must never claim the former.
    Pair p(/*latency=*/4);
    p.pump_n(120);
    CHECK_FALSE(p.host.end_round_scheduled());
    CHECK_FALSE(p.guest.end_round_scheduled());
    CHECK_FALSE(p.host.round_ended());
    CHECK_FALSE(p.guest.round_ended());
    CHECK(p.host.end_round_tick() == net::RollbackSession::kNoEndRound);
}

// --- transition 2: the match ended, back to setup ---------------------------

namespace {

struct RematchPair {
    explicit RematchPair(int latency = 0, int drop_every = 0, int timeout_ms = 30000)
        : link(latency, drop_every),
          ta(link, 0),
          tb(link, 1),
          host(ta, /*is_host=*/true, timeout_ms),
          guest(tb, /*is_host=*/false, timeout_ms) {}

    void pump(int ms_per_pump = 50) {
        now += ms_per_pump;
        host.step(now);
        guest.step(now);
        link.step();
    }

    net::LoopbackLink link;
    net::LoopbackTransport ta;
    net::LoopbackTransport tb;
    net::RematchSession host;
    net::RematchSession guest;
    std::int64_t now = 0;
};

}  // namespace

TEST_CASE("rematch: the guest waits for the host, then follows it back to setup") {
    RematchPair p;
    for (int i = 0; i < 40; ++i) p.pump();

    // THE PIN. The host is still reading the outcome screens, so the guest has
    // NOT moved — and, crucially, has not failed either: the host's liveness
    // keeps its timeout clock reset, which is what stops "still deciding" from
    // being read as "gone" and the session from being dropped.
    CHECK_FALSE(p.guest.ready());
    CHECK_FALSE(p.guest.failed());
    CHECK_FALSE(p.host.ready());
    CHECK_FALSE(p.host.failed());

    p.host.accept();
    CHECK(p.host.ready());  // the host leaves at once, announcing on the way
    for (int i = 0; i < 20 && !p.guest.ready(); ++i) p.pump();
    CHECK(p.guest.ready());
    CHECK_FALSE(p.guest.failed());
}

TEST_CASE("rematch: a host that takes its time is not mistaken for a host that left") {
    // THE POINT OF THE LIVENESS KIND, pinned. The timeout here is 1 s and the
    // host sits on its outcome screens for 5 — five times over — without ever
    // accepting. If the host went quiet while it was deciding, the guest would
    // read that as "gone", report THE HOST LEFT THE GAME, and drop the link:
    // exactly the disconnect this change exists to remove, moved a few seconds
    // later. RematchWait is what stops it.
    RematchPair p(/*latency=*/0, /*drop_every=*/0, /*timeout_ms=*/1000);
    for (int i = 0; i < 100; ++i) p.pump();  // 5 s of the host reading the board
    CHECK_FALSE(p.guest.failed());
    CHECK_FALSE(p.guest.ready());
    CHECK_FALSE(p.host.failed());  // the guest's own heartbeat does the same for the host

    p.host.accept();
    for (int i = 0; i < 20 && !p.guest.ready(); ++i) p.pump();
    CHECK(p.guest.ready());
    CHECK_FALSE(p.guest.failed());
}

TEST_CASE("rematch: the guest follows the host's SETUP traffic even if every Rematch is lost") {
    // The self-heal. The host accepts and walks into the setup stage; suppose no
    // copy of Rematch survives. Its first SetupSession preview says the same
    // thing implicitly and cannot be missed, because the host re-broadcasts it
    // for as long as it is on those screens.
    net::LoopbackLink link;
    net::LoopbackTransport ta(link, 0);
    net::LoopbackTransport tb(link, 1);
    net::RematchSession guest(tb, /*is_host=*/false);

    std::int64_t now = 0;
    for (int i = 0; i < 5; ++i) {
        now += 50;
        guest.step(now);
        link.step();
    }
    REQUIRE_FALSE(guest.ready());

    const std::vector<std::uint8_t> preview = net::encode_setup_preview(net::SetupPreviewFrame{});
    ta.send(preview.data(), preview.size());
    link.step();
    now += 50;
    guest.step(now);
    CHECK(guest.ready());
}

TEST_CASE("rematch: a peer that really is gone fails instead of hanging") {
    net::LoopbackLink link;
    net::LoopbackTransport tb(link, 1);
    net::RematchSession guest(tb, /*is_host=*/false, /*timeout_ms=*/2000);

    std::int64_t now = 0;
    for (int i = 0; i < 10; ++i) {
        now += 100;
        guest.step(now);
        link.step();
    }
    CHECK_FALSE(guest.failed());  // inside the window
    now += 5000;
    guest.step(now);
    CHECK(guest.failed());
    CHECK_FALSE(guest.ready());
}

TEST_CASE("rematch: a guest cannot announce the transition") {
    // Host authority, stated as a test: accept() on a guest is inert, and a guest
    // that sent Rematch anyway would be talking to a host that does not read it
    // (the host's own exit is its local player's call). So two guests can never
    // walk each other into a setup stage the host is not in.
    RematchPair p;
    p.guest.accept();
    for (int i = 0; i < 40; ++i) p.pump();
    CHECK_FALSE(p.guest.ready());
    CHECK_FALSE(p.host.ready());
}

TEST_CASE("rematch: the round-rotation numbering keeps walking across a match boundary") {
    // A rematch reuses the socket, so the FIRST round of match 2 must not sit in
    // the tick space match 1's last round was still sending into — the same
    // hazard round_tick_base exists to remove between rounds, one level up.
    // GameApp walks one counter across both, wrapping well inside uint32.
    constexpr int kWrap = 1024;
    const int last_of_match_1 = 3;
    const int first_of_match_2 = (last_of_match_1 + 1) % kWrap;
    CHECK(net::round_tick_base(first_of_match_2) > net::round_tick_base(last_of_match_1));
    CHECK(net::round_tick_base(first_of_match_2) - net::round_tick_base(last_of_match_1) ==
          net::kRoundTickStride);
    CHECK(net::round_tick_base(kWrap - 1) < 0xFFFFFFFFu);
}
