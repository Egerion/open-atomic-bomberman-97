#pragma once

#include <array>
#include <cstdint>
#include <string>

#include "bomber/input/input.hpp"    // SlotInputType
#include "bomber/net/protocol.hpp"   // net::SetupPreviewFrame / SetupSlotKind
#include "bomber/sim/constants.hpp"  // sim::kMaxPlayers

// The PURE half of the online setup stage: the mapping between this machine's
// 10-slot roster (the PLAYER INPUT screen's type/sub/team arrays) and the wire's
// SetupPreviewFrame, plus the level-index convention the LEVEL & ROUNDS screen
// uses. SDL-free and header-only on purpose — this is the part with real logic
// in it (the seat re-pointing below is easy to get subtly wrong), so it is
// unit-tested without a window (tests/game/test_net_setup_roster.cpp).
//
// The screen-side wiring (which session to pump, what to draw while waiting)
// lives in net_setup_link.hpp, which is where the RE citations for the
// host/guest split are.

namespace bomber::game {

inline constexpr bool seat_in_mask(std::uint16_t mask, int slot) {
    return (mask & static_cast<std::uint16_t>(1u << slot)) != 0;
}

// HOST: force every wire seat into the only roster shape the seat masks can
// actually simulate — each local seat to KEYBOARD (with the next key-set) and
// each remote seat to type 4 OTHER, which is the original's own marker for
// "someone else's player" (docs/re/network-screens.md §7, sub_40D372's
// `sub_421E33(slot, 4, 0)`). Every OTHER slot that still holds a human type from
// the last local match is cleared to OFF: a human outside the wire seats would
// be driven from local input on one peer and from nothing on the other, i.e. a
// guaranteed desync. AI slots are left alone — they are simulated identically on
// every peer and are how a match gets more players than it has machines.
//
// Bit-by-bit over both masks, so a 10-seat lobby is the same code as a pair.
inline void seed_net_host_roster(std::uint16_t local_seats, std::uint16_t remote_seats,
                                 std::array<int, sim::kMaxPlayers>& type,
                                 std::array<int, sim::kMaxPlayers>& sub) {
    int key_set = 0;
    for (int i = 0; i < sim::kMaxPlayers; ++i) {
        const auto slot = static_cast<std::size_t>(i);
        if (seat_in_mask(local_seats, i)) {
            type[slot] = static_cast<int>(SlotInputType::Keyboard);
            sub[slot] = key_set++;
        } else if (seat_in_mask(remote_seats, i)) {
            type[slot] = static_cast<int>(SlotInputType::Other);
            sub[slot] = 0;
        } else if (type[slot] != static_cast<int>(SlotInputType::Computer)) {
            type[slot] = static_cast<int>(SlotInputType::Off);
            sub[slot] = 0;
        }
    }
}

// HOST: this machine's roster -> the preview's roster half. Team play OFF zeroes
// every team byte, matching what MatchRunner::build_config will do to the
// confirmed config (`cfg.team.fill(0)`), so the guest's markers agree with what
// it eventually receives. The preview carries no team-MODE flag of its own; a
// non-zero byte IS the signal (see apply_preview_roster).
inline void fill_preview_roster(const std::array<int, sim::kMaxPlayers>& type,
                                const std::array<int, sim::kMaxPlayers>& team, bool team_play,
                                net::SetupPreviewFrame& out) {
    for (int i = 0; i < sim::kMaxPlayers; ++i) {
        const auto slot = static_cast<std::size_t>(i);
        switch (static_cast<SlotInputType>(type[slot])) {
            case SlotInputType::Computer: out.slots[slot] = net::SetupSlotKind::Ai; break;
            case SlotInputType::Keyboard:
            case SlotInputType::Joystick: out.slots[slot] = net::SetupSlotKind::Human; break;
            case SlotInputType::Other: out.slots[slot] = net::SetupSlotKind::Remote; break;
            case SlotInputType::Off: out.slots[slot] = net::SetupSlotKind::Off; break;
        }
        out.team[slot] = (team_play && team[slot] != 0) ? std::uint8_t{1} : std::uint8_t{0};
    }
}

// GUEST: the preview's roster half -> THIS machine's display roster. The host
// describes the roster from ITS seat (its own player is Human, ours is Remote),
// so the kinds are RE-POINTED here: only the seat we actually play renders as a
// local controller, every other human as type 4 OTHER — which is exactly what
// the original's kind-40 handler writes on each receiving machine.
inline void apply_preview_roster(const net::SetupPreviewFrame& p, std::uint16_t local_seats,
                                 std::array<int, sim::kMaxPlayers>& type,
                                 std::array<int, sim::kMaxPlayers>& sub,
                                 std::array<int, sim::kMaxPlayers>& team, bool& team_play) {
    bool any_team = false;
    int key_set = 0;
    for (int i = 0; i < sim::kMaxPlayers; ++i) {
        const auto slot = static_cast<std::size_t>(i);
        switch (p.slots[slot]) {
            case net::SetupSlotKind::Off:
                type[slot] = static_cast<int>(SlotInputType::Off);
                sub[slot] = 0;
                break;
            case net::SetupSlotKind::Ai:
                type[slot] = static_cast<int>(SlotInputType::Computer);
                sub[slot] = 0;
                break;
            case net::SetupSlotKind::Human:
            case net::SetupSlotKind::Remote:
                if (seat_in_mask(local_seats, i)) {
                    type[slot] = static_cast<int>(SlotInputType::Keyboard);
                    sub[slot] = key_set++;
                } else {
                    type[slot] = static_cast<int>(SlotInputType::Other);
                    sub[slot] = 0;
                }
                break;
        }
        team[slot] = p.team[slot] != 0 ? 1 : 0;
        if (p.team[slot] != 0) any_team = true;
    }
    team_play = any_team;
}

// The LEVEL & ROUNDS screen's level value is -1 for RANDOM; the preview field is
// a byte, so 255 carries that. A level this install's registry does not know (a
// custom map on the host) falls back to RANDOM for the sample-block swatch — the
// NAME still reads correctly because it travels as a string, and the board
// itself travels in the confirmed config, never as an index.
inline constexpr int preview_level_to_index(std::uint8_t level_index, int level_count) {
    if (level_index == 255) return -1;
    const int idx = static_cast<int>(level_index);
    return idx >= level_count ? -1 : idx;
}
inline constexpr std::uint8_t index_to_preview_level(int level) {
    return level < 0 ? std::uint8_t{255} : static_cast<std::uint8_t>(level & 0xFF);
}

// The level label rides the wire into the guest's display, and it comes from the
// HOST's MESSAGES.TXT — an editable 1997 text file. encode_setup_preview
// truncates at kSetupLevelNameMax but decode REJECTS a non-printable byte, so a
// stray control character would silently kill the whole preview stream rather
// than mangle one line. Filter to printable ASCII, the same posture
// lobby_screen.cpp's display_name takes with a server-supplied lobby name.
inline std::string wire_safe_level_name(const std::string& raw) {
    std::string out;
    for (const char c : raw) {
        if (out.size() >= net::kSetupLevelNameMax) break;
        const auto u = static_cast<unsigned char>(c);
        if (u >= 32 && u < 127) out += c;
    }
    return out;
}

}  // namespace bomber::game
