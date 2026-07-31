#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <array>
#include <string>

#include "bomber/net/protocol.hpp"
#include "bomber/netui/net_setup_roster.hpp"

// The PURE half of the online setup stage (net_setup_roster.hpp): the roster
// mapping between a machine's 10 slots and the wire preview, and the level-index
// convention. The screens that use it need SDL, this logic does not — same split
// the app_flow / goldman_wheel / editor_grid suites already use.

using namespace bomber;
using bomber::game::SlotInputType;

namespace {

constexpr std::uint16_t kHostSeat = 0b01u;   // the host plays seat 0
constexpr std::uint16_t kGuestSeat = 0b10u;  // the guest plays seat 1

int slot_type(const std::array<int, sim::kMaxPlayers>& a, int i) {
    return a[static_cast<std::size_t>(i)];
}

// A machine's ten local slots: the three parallel arrays every helper here
// reads or writes together, so a case names one thing instead of three.
struct Roster {
    std::array<int, sim::kMaxPlayers> type{};
    std::array<int, sim::kMaxPlayers> sub{};
    std::array<int, sim::kMaxPlayers> team{};
};

// The host's roster as it stands once the wire seats are seeded and the shared
// AI slots are filled in: seat 0 its own human, seat 1 the peer, 2-3 AI.
Roster seeded_host_roster() {
    Roster r;
    game::seed_net_host_roster(kHostSeat, kGuestSeat, r.type, r.sub);
    r.type[2] = static_cast<int>(SlotInputType::Computer);
    r.type[3] = static_cast<int>(SlotInputType::Computer);
    return r;
}

// A preview frame taken through the REAL codec, because a frame the decoder
// rejects is a dead guest — an in-memory hand-off would not notice.
net::SetupPreviewFrame round_tripped(const net::SetupPreviewFrame& sent) {
    net::Message decoded;
    const std::vector<std::uint8_t> bytes = net::encode_setup_preview(sent);
    REQUIRE(net::decode(bytes.data(), bytes.size(), &decoded));
    return decoded.setup_preview;
}

}  // namespace

TEST_CASE("host roster seeding pins the two wire seats") {
    // Whatever the last LOCAL match left behind: two keyboards, a joystick and
    // an AI spread over the first four slots.
    std::array<int, sim::kMaxPlayers> type{};
    std::array<int, sim::kMaxPlayers> sub{};
    type[0] = static_cast<int>(SlotInputType::Keyboard);
    type[1] = static_cast<int>(SlotInputType::Keyboard);
    sub[1] = 1;
    type[2] = static_cast<int>(SlotInputType::Joystick);
    type[3] = static_cast<int>(SlotInputType::Computer);

    game::seed_net_host_roster(kHostSeat, kGuestSeat, type, sub);

    CHECK(slot_type(type, 0) == static_cast<int>(SlotInputType::Keyboard));  // ours
    CHECK(sub[0] == 0);
    CHECK(slot_type(type, 1) == static_cast<int>(SlotInputType::Other));  // the peer's
    CHECK(sub[1] == 0);
    // A second local human cannot be carried by a two-seat wire — cleared.
    CHECK(slot_type(type, 2) == static_cast<int>(SlotInputType::Off));
    // An AI is simulated on both peers from the shared config — left alone.
    CHECK(slot_type(type, 3) == static_cast<int>(SlotInputType::Computer));
}

TEST_CASE("the host's preview frame names each seat's kind") {
    const Roster host = seeded_host_roster();
    net::SetupPreviewFrame p;
    game::fill_preview_roster(host.type, host.team, /*team_play=*/false, p);
    CHECK(p.slots[0] == net::SetupSlotKind::Human);
    CHECK(p.slots[1] == net::SetupSlotKind::Remote);
    CHECK(p.slots[2] == net::SetupSlotKind::Ai);
    CHECK(p.slots[4] == net::SetupSlotKind::Off);
}

TEST_CASE("preview round-trip re-points the seats for the guest") {
    const Roster host = seeded_host_roster();
    net::SetupPreviewFrame p;
    game::fill_preview_roster(host.type, host.team, /*team_play=*/false, p);

    // GUEST side: the SAME frame, seen from seat 1. The host's own human must
    // read as OTHER here and OUR seat as a local controller — the flip is the
    // whole point (sub_40D372's "someone else's player").
    Roster guest;
    bool gteam_play = true;  // must be cleared: no team byte is set
    game::apply_preview_roster(round_tripped(p), kGuestSeat, guest.type, guest.sub, guest.team,
                               gteam_play);

    CHECK(slot_type(guest.type, 0) == static_cast<int>(SlotInputType::Other));
    CHECK(slot_type(guest.type, 1) == static_cast<int>(SlotInputType::Keyboard));
    CHECK(guest.sub[1] == 0);
    CHECK(slot_type(guest.type, 2) == static_cast<int>(SlotInputType::Computer));
    CHECK(slot_type(guest.type, 4) == static_cast<int>(SlotInputType::Off));
    CHECK(gteam_play == false);
}

TEST_CASE("team play off zeroes every team byte; on survives the trip") {
    Roster host = seeded_host_roster();
    host.team[1] = 1;  // the alternating sub_4049C0 default

    net::SetupPreviewFrame off;
    game::fill_preview_roster(host.type, host.team, /*team_play=*/false, off);
    CHECK(off.team[1] == 0);

    net::SetupPreviewFrame on;
    game::fill_preview_roster(host.type, host.team, /*team_play=*/true, on);
    CHECK(on.team[0] == 0);
    CHECK(on.team[1] == 1);

    Roster guest;
    bool gteam_play = false;
    game::apply_preview_roster(on, kGuestSeat, guest.type, guest.sub, guest.team, gteam_play);
    CHECK(gteam_play == true);
    CHECK(guest.team[1] == 1);

    gteam_play = true;
    game::apply_preview_roster(off, kGuestSeat, guest.type, guest.sub, guest.team, gteam_play);
    CHECK(gteam_play == false);
}

TEST_CASE("level index carries RANDOM and clamps an unknown map") {
    CHECK(game::index_to_preview_level(-1) == 255);
    CHECK(game::index_to_preview_level(7) == 7);
    CHECK(game::preview_level_to_index(255, 11) == -1);
    CHECK(game::preview_level_to_index(7, 11) == 7);
    // A custom map the guest's registry does not have: the swatch falls back to
    // RANDOM rather than indexing art that is not there.
    CHECK(game::preview_level_to_index(30, 11) == -1);
}

TEST_CASE("a level name is clamped and stripped so the codec accepts it") {
    // Non-printable bytes would make decode() REJECT the whole preview, which
    // would silently kill the guest's display rather than mangle one line.
    const std::string dirty = std::string(
        "BOM\x01"
        "B\x7f"
        "ER");
    const std::string clean = game::wire_safe_level_name(dirty);
    CHECK(clean == "BOMBER");

    const std::string huge(80, 'X');
    CHECK(game::wire_safe_level_name(huge).size() == net::kSetupLevelNameMax);

    net::SetupPreviewFrame p;
    p.level_name = game::wire_safe_level_name(dirty);
    net::Message decoded;
    const std::vector<std::uint8_t> bytes = net::encode_setup_preview(p);
    REQUIRE(net::decode(bytes.data(), bytes.size(), &decoded));
    CHECK(decoded.setup_preview.level_name == "BOMBER");
}
