#pragma once

#include <cstdint>

// WHICH PATH IS CARRYING THE MATCH — the single most diagnostic fact about a
// live netplay session, and the one the client used to have no way of saying.
//
// It matters because the three paths have completely different failure modes: a
// DIRECT match does not touch the matchmaker at all after tick 0, so a server
// hiccup cannot be the cause of anything the players see; a RELAYED match rides
// the server's forwarder every single datagram, so it can; and a STAR HUB match
// funnels every guest's traffic through one player's uplink, which is a
// bottleneck no server-side log will ever show.
//
// The identity lives on the TRANSPORT rather than being threaded down from
// LobbyFlow, because the transport is the object actually carrying the bytes:
// the CLI/direct-connect paths never build a LobbyFlow at all, and a flow's
// transport() reference is captured once by the caller (game_app.cpp), so asking
// the flow later would be asking a different question than "what am I sending
// over right now".
//
// Its own header, with no dependencies, so transport.hpp can name it without
// pulling in the statistics layer (which depends on bomber::sim).

namespace bomber::net {

enum class NetPath : std::uint8_t {
    Unknown = 0,  // a Transport that has not been taught to say (never expected in a match)
    Direct,       // UdpTransport — a punched peer-to-peer path; the server is out of the loop
    Relayed,      // RelayedTransport — every datagram is forwarded by the matchmaker
    StarHub,      // StarHubTransport — this machine is the hub fanning out to >2 seats
    Loopback,     // LoopbackTransport — headless tests, no sockets at all
};

// A short, stable, screen-sized label. Stable because it is also what lands in
// the end-of-session log line, which is meant to be greppable.
const char* path_name(NetPath p);

}  // namespace bomber::net
