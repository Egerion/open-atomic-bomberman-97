#pragma once

#include <cstdint>
#include <string>
#include <vector>

// The lobby control-plane wire messages (ADR-0011), the C++ side of the FROZEN
// contract in services/matchmaker/PROTOCOL.md. One JSON object per WebSocket
// text frame, discriminated by a top-level "type" string with every payload
// field flattened alongside it. This header is pure data + free functions
// (encode client->server, parse server->client) so it is unit-testable with no
// socket or WebSocket dependency; LobbyClient wires it onto the live connection.
namespace bomber::net {

// --- shared sub-structures --------------------------------------------------

struct RosterEntry {
    int seat = 0;
    std::string name;
    bool ready = false;
    bool is_host = false;
    int rtt_to_host_ms = -1;  // optional in the wire; -1 = absent
};

struct LobbyCandidate {
    std::string kind;   // "host" | "reflexive" | "relay" (ICE-like priority)
    std::string addr;   // "ip:port"
    std::string alloc;  // relay allocation id — empty unless kind == "relay"
};

struct PublicLobby {
    std::string code;
    std::string name;
    int players = 0;
    int max = 0;
    bool build_ok = true;  // false when this browser's build_hash differs
};

// --- parsed server -> client message ---------------------------------------

enum class LobbyMsgType {
    Unknown,  // unparsable / unknown "type" (never acted on)
    LobbyCreated,
    JoinAccepted,
    JoinRejected,
    PublicList,
    RosterUpdate,
    HeartbeatAck,
    PeerCandidates,
    StartMatch,
    ReanchorAccepted,
    Error,
};

// One decoded server frame. Only the fields relevant to `type` are populated
// (a flat struct rather than a variant — matches protocol.hpp's Message style).
struct LobbyServerMessage {
    LobbyMsgType type = LobbyMsgType::Unknown;

    // LobbyCreated / ReanchorAccepted
    std::string code;
    std::string lobby_id;   // + JoinAccepted
    std::string host_token;
    int your_seat = -1;     // LobbyCreated / JoinAccepted

    // JoinAccepted / RosterUpdate
    std::vector<RosterEntry> roster;
    std::vector<LobbyCandidate> host_candidates;  // JoinAccepted

    std::string reason;  // JoinRejected: "not_found"|"full"|"build_mismatch"|"in_progress"

    std::vector<PublicLobby> lobbies;  // PublicList

    // PeerCandidates
    int candidates_seat = -1;
    std::vector<LobbyCandidate> candidates;

    // StartMatch (the parity payload; max_prediction is deliberately NOT sent)
    std::uint32_t seed = 0;
    std::vector<int> seat_assign;
    std::string match_config_digest;
    int input_delay = 0;
    int hub_seat = 0;
    std::uint16_t local_seats_mask = 0;

    // Error
    std::string error_code;
    std::string error_message;
};

// Parse one server JSON frame. Never throws — a malformed / unknown frame comes
// back as type == Unknown (server frames are treated as untrusted input).
LobbyServerMessage parse_server_message(const std::string& json);

// Format a uint32 build/config hash as the wire hex string "0x%08X"
// (PROTOCOL.md §1: build_hash is a hex STRING).
std::string hex_hash(std::uint32_t v);

// --- client -> server encoders (each returns one JSON text frame) -----------

std::string encode_create_lobby(const std::string& visibility, const std::string& name,
                                int max_seats, std::uint32_t build_hash, const std::string& player);
std::string encode_join_by_code(const std::string& code, std::uint32_t build_hash,
                                const std::string& player);
std::string encode_list_public(std::uint32_t build_hash);
std::string encode_set_ready(bool ready);
std::string encode_heartbeat();
std::string encode_candidates(const std::string& lobby_id, int seat,
                              const std::vector<LobbyCandidate>& list);
std::string encode_start_match(const std::string& lobby_id, const std::string& host_token,
                               int input_delay, std::uint32_t match_config_digest);
std::string encode_reanchor(const std::string& code, const std::string& roster_digest);
std::string encode_match_over(const std::string& lobby_id);

}  // namespace bomber::net
