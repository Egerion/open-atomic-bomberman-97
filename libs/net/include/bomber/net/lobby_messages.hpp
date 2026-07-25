#pragma once

#include <cstddef>
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

enum class LobbyMsgType : std::uint8_t {
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
    RelayAllocated,
    Chat,  // PORT-ONLY lobby chat (PROTOCOL.md §7) — no original counterpart
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

    // RelayAllocated (PROTOCOL.md §6): where to route when the punch failed.
    std::string relay_addr;  // "host:port" of the UDP forwarder
    std::string alloc_id;    // 32 hex chars; parse_alloc_id() turns it into bytes

    // Chat (PROTOCOL.md §7). The seat and the name are the SERVER's, taken from
    // its roster — the sender cannot choose either. Still untrusted input: it is
    // another player's typing, so sanitize_chat_* before rendering a byte of it.
    std::string chat_name;
    std::string chat_text;
    int chat_seat = -1;

    // Error
    std::string error_code;
    std::string error_message;
};

// Parse one server JSON frame. Never throws — a malformed / unknown frame comes
// back as type == Unknown (server frames are treated as untrusted input).
LobbyServerMessage parse_server_message(const std::string& json);

// --- STUN discovery datagrams (PROTOCOL.md §2, UDP not WebSocket) -----------

// {"type":"StunProbe","nonce":"<opaque>"} — `nonce` is an opaque STRING the
// server echoes verbatim (a string, not a number, to dodge JSON precision).
std::string encode_stun_probe(const std::string& nonce);

// Parse {"type":"StunReply","nonce":"…","your_addr":"81.2.3.4:52001"}. Returns
// false for anything else (untrusted input; never throws).
bool parse_stun_reply(const std::string& json, std::string* nonce, std::string* your_addr);

// Split "ip:port" into its parts, accepting the bracketed IPv6 form
// ("[::1]:52001" → "::1", 52001). Returns false on a malformed address.
bool split_host_port(const std::string& addr, std::string* host, std::uint16_t* port);

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
// {"type":"AllocateRelay","lobby_id":"…","seat":n} — asked for only after the
// punch fails (PROTOCOL.md §6); the reply carries relay_addr + alloc_id.
std::string encode_allocate_relay(const std::string& lobby_id, int seat);

// --- lobby chat (PROTOCOL.md §7) --------------------------------------------
//
// A PORT-ONLY FEATURE: the 1997 game has no chat at all. It rides the control
// plane because players talk in the LOBBY, before any hole punch exists.
//
// {"type":"Chat","text":"…"} — the body and nothing else. A seat or a name in a
// client frame is ignored by the server, so this carries neither.
std::string encode_chat(const std::string& text);

// The wire cap on one line, matching the server's (PROTOCOL.md §7.2). The server
// REJECTS an over-long body rather than trimming it, so a client that wants its
// message delivered clamps before sending.
inline constexpr std::size_t kChatMaxBytes = 120;
// The displayed cap on a speaker's name. The roster carries whatever the peer
// typed into its options; this is what keeps one long name from pushing a whole
// line of chat off the panel.
inline constexpr std::size_t kChatMaxNameBytes = 16;

// Reduce untrusted chat text to what this client can actually put on screen:
// printable ASCII (the 1997 FON covers nothing else — lobby_screen.cpp's
// display_name takes the same line with server-supplied lobby names), trimmed of
// surrounding blanks and clamped to `kChatMaxBytes`. Returns "" when nothing
// renderable survives, which callers treat as "there is no message here".
//
// Applied on BOTH sides of the socket, deliberately: on send so we never ask the
// server to relay bytes we could not draw ourselves, and on receive because a
// frame is untrusted input no matter which server relayed it.
std::string sanitize_chat_text(const std::string& raw);
// The same reduction at `kChatMaxNameBytes`, for a speaker's name.
std::string sanitize_chat_name(const std::string& raw);

}  // namespace bomber::net
