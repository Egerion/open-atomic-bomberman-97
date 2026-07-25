#include "bomber/net/lobby_messages.hpp"

#include <cstdio>

#include <nlohmann/json.hpp>

namespace bomber::net {

namespace {

using nlohmann::json;

std::vector<RosterEntry> parse_roster(const json& arr) {
    std::vector<RosterEntry> out;
    if (!arr.is_array()) return out;
    for (const auto& e : arr) {
        if (!e.is_object()) continue;
        RosterEntry r;
        r.seat = e.value("seat", 0);
        r.name = e.value("name", std::string());
        r.ready = e.value("ready", false);
        r.is_host = e.value("is_host", false);
        r.rtt_to_host_ms = e.value("rtt_to_host_ms", -1);
        out.push_back(std::move(r));
    }
    return out;
}

std::vector<LobbyCandidate> parse_candidates(const json& arr) {
    std::vector<LobbyCandidate> out;
    if (!arr.is_array()) return out;
    for (const auto& e : arr) {
        if (!e.is_object()) continue;
        LobbyCandidate c;
        c.kind = e.value("kind", std::string());
        c.addr = e.value("addr", std::string());
        c.alloc = e.value("alloc", std::string());
        out.push_back(std::move(c));
    }
    return out;
}

// The shared reduction behind sanitize_chat_text / sanitize_chat_name: keep the
// codes the front-end FON can draw, drop everything else, trim the blanks that
// would only pad the panel, and clamp to `max_bytes`.
std::string sanitize_printable(const std::string& raw, std::size_t max_bytes) {
    std::string out;
    for (const char c : raw) {
        if (out.size() >= max_bytes) break;
        const auto u = static_cast<unsigned char>(c);
        if (u >= 32 && u < 127) out += c;
    }
    const std::size_t first = out.find_first_not_of(' ');
    if (first == std::string::npos) return {};
    return out.substr(first, out.find_last_not_of(' ') - first + 1);
}

}  // namespace

std::string sanitize_chat_text(const std::string& raw) {
    return sanitize_printable(raw, kChatMaxBytes);
}

std::string sanitize_chat_name(const std::string& raw) {
    return sanitize_printable(raw, kChatMaxNameBytes);
}

std::string hex_hash(std::uint32_t v) {
    char buf[11];
    std::snprintf(buf, sizeof(buf), "0x%08X", v);
    return std::string(buf);
}

LobbyServerMessage parse_server_message(const std::string& text) {
    LobbyServerMessage m;
    json j;
    try {
        j = json::parse(text);
    } catch (...) {
        return m;  // malformed → Unknown
    }
    if (!j.is_object()) return m;
    const std::string type = j.value("type", std::string());

    if (type == "LobbyCreated") {
        m.type = LobbyMsgType::LobbyCreated;
        m.code = j.value("code", std::string());
        m.lobby_id = j.value("lobby_id", std::string());
        m.host_token = j.value("host_token", std::string());
        m.your_seat = j.value("your_seat", -1);
    } else if (type == "JoinAccepted") {
        m.type = LobbyMsgType::JoinAccepted;
        m.lobby_id = j.value("lobby_id", std::string());
        m.your_seat = j.value("your_seat", -1);
        if (j.contains("roster")) m.roster = parse_roster(j["roster"]);
        if (j.contains("host_candidates")) m.host_candidates = parse_candidates(j["host_candidates"]);
    } else if (type == "JoinRejected") {
        m.type = LobbyMsgType::JoinRejected;
        m.reason = j.value("reason", std::string());
    } else if (type == "PublicList") {
        m.type = LobbyMsgType::PublicList;
        if (j.contains("lobbies") && j["lobbies"].is_array()) {
            for (const auto& e : j["lobbies"]) {
                if (!e.is_object()) continue;
                PublicLobby p;
                p.code = e.value("code", std::string());
                p.name = e.value("name", std::string());
                p.players = e.value("players", 0);
                p.max = e.value("max", 0);
                p.build_ok = e.value("build_ok", true);
                m.lobbies.push_back(std::move(p));
            }
        }
    } else if (type == "RosterUpdate") {
        m.type = LobbyMsgType::RosterUpdate;
        if (j.contains("roster")) m.roster = parse_roster(j["roster"]);
    } else if (type == "HeartbeatAck") {
        m.type = LobbyMsgType::HeartbeatAck;
    } else if (type == "PeerCandidates") {
        m.type = LobbyMsgType::PeerCandidates;
        m.candidates_seat = j.value("seat", -1);
        if (j.contains("list")) m.candidates = parse_candidates(j["list"]);
    } else if (type == "StartMatch") {
        m.type = LobbyMsgType::StartMatch;
        m.seed = j.value("seed", std::uint32_t{0});
        m.match_config_digest = j.value("match_config_digest", std::string());
        m.input_delay = j.value("input_delay", 0);
        m.local_seats_mask = static_cast<std::uint16_t>(j.value("local_seats_mask", 0));
        if (j.contains("topology") && j["topology"].is_object())
            m.hub_seat = j["topology"].value("hub_seat", 0);
        if (j.contains("seat_assign") && j["seat_assign"].is_array())
            for (const auto& s : j["seat_assign"])
                if (s.is_number_integer()) m.seat_assign.push_back(s.get<int>());
    } else if (type == "ReanchorAccepted") {
        m.type = LobbyMsgType::ReanchorAccepted;
        m.lobby_id = j.value("lobby_id", std::string());
        m.code = j.value("code", std::string());
        m.host_token = j.value("host_token", std::string());
    } else if (type == "RelayAllocated") {
        m.type = LobbyMsgType::RelayAllocated;
        m.relay_addr = j.value("relay_addr", std::string());
        m.alloc_id = j.value("alloc_id", std::string());
    } else if (type == "Chat") {
        // Stored raw here — parse_server_message's job is decoding, not policy.
        // The sanitisers run where the text is accepted for display (LobbyFlow).
        m.type = LobbyMsgType::Chat;
        m.chat_seat = j.value("seat", -1);
        m.chat_name = j.value("name", std::string());
        m.chat_text = j.value("text", std::string());
    } else if (type == "Error") {
        m.type = LobbyMsgType::Error;
        m.error_code = j.value("code", std::string());
        m.error_message = j.value("message", std::string());
    }
    return m;
}

std::string encode_stun_probe(const std::string& nonce) {
    json j;
    j["type"] = "StunProbe";
    j["nonce"] = nonce;
    return j.dump();
}

bool parse_stun_reply(const std::string& text, std::string* nonce, std::string* your_addr) {
    json j;
    try {
        j = json::parse(text);
    } catch (...) {
        return false;
    }
    if (!j.is_object() || j.value("type", std::string()) != "StunReply") return false;
    const std::string addr = j.value("your_addr", std::string());
    if (addr.empty()) return false;
    if (nonce) *nonce = j.value("nonce", std::string());
    if (your_addr) *your_addr = addr;
    return true;
}

bool split_host_port(const std::string& addr, std::string* host, std::uint16_t* port) {
    // Bracketed IPv6 ("[::1]:52001") keeps its colons inside the brackets, so
    // split on the LAST colon and strip the brackets.
    const std::size_t colon = addr.rfind(':');
    if (colon == std::string::npos || colon + 1 >= addr.size()) return false;
    std::string h = addr.substr(0, colon);
    if (h.size() >= 2 && h.front() == '[' && h.back() == ']') h = h.substr(1, h.size() - 2);
    if (h.empty()) return false;
    unsigned long p = 0;
    for (char c : addr.substr(colon + 1)) {
        if (c < '0' || c > '9') return false;
        p = p * 10 + static_cast<unsigned long>(c - '0');
        if (p > 65535) return false;
    }
    if (p == 0) return false;
    if (host) *host = h;
    if (port) *port = static_cast<std::uint16_t>(p);
    return true;
}

std::string encode_create_lobby(const std::string& visibility, const std::string& name,
                                int max_seats, std::uint32_t build_hash, const std::string& player) {
    json j;
    j["type"] = "CreateLobby";
    j["visibility"] = visibility;
    j["name"] = name;
    j["max_seats"] = max_seats;
    j["build_hash"] = hex_hash(build_hash);
    j["player"] = player;
    return j.dump();
}

std::string encode_join_by_code(const std::string& code, std::uint32_t build_hash,
                                const std::string& player) {
    json j;
    j["type"] = "JoinByCode";
    j["code"] = code;
    j["build_hash"] = hex_hash(build_hash);
    j["player"] = player;
    return j.dump();
}

std::string encode_list_public(std::uint32_t build_hash) {
    json j;
    j["type"] = "ListPublic";
    j["build_hash"] = hex_hash(build_hash);
    return j.dump();
}

std::string encode_set_ready(bool ready) {
    json j;
    j["type"] = "SetReady";
    j["ready"] = ready;
    return j.dump();
}

std::string encode_heartbeat() {
    json j;
    j["type"] = "Heartbeat";
    return j.dump();
}

std::string encode_candidates(const std::string& lobby_id, int seat,
                              const std::vector<LobbyCandidate>& list) {
    json j;
    j["type"] = "Candidates";
    j["lobby_id"] = lobby_id;
    j["seat"] = seat;
    json arr = json::array();
    for (const LobbyCandidate& c : list) {
        json e;
        e["kind"] = c.kind;
        e["addr"] = c.addr;
        if (!c.alloc.empty()) e["alloc"] = c.alloc;
        arr.push_back(std::move(e));
    }
    j["list"] = std::move(arr);
    return j.dump();
}

std::string encode_start_match(const std::string& lobby_id, const std::string& host_token,
                               int input_delay, std::uint32_t match_config_digest) {
    json j;
    j["type"] = "StartMatch";
    j["lobby_id"] = lobby_id;
    j["host_token"] = host_token;
    j["input_delay"] = input_delay;
    j["match_config_digest"] = hex_hash(match_config_digest);
    return j.dump();
}

std::string encode_reanchor(const std::string& code, const std::string& roster_digest) {
    json j;
    j["type"] = "ReanchorLobby";
    j["code"] = code;
    j["roster_digest"] = roster_digest;
    return j.dump();
}

std::string encode_match_over(const std::string& lobby_id) {
    json j;
    j["type"] = "MatchOver";
    j["lobby_id"] = lobby_id;
    return j.dump();
}

std::string encode_allocate_relay(const std::string& lobby_id, int seat) {
    json j;
    j["type"] = "AllocateRelay";
    j["lobby_id"] = lobby_id;
    j["seat"] = seat;
    return j.dump();
}

std::string encode_chat(const std::string& text) {
    // No seat and no name on purpose (PROTOCOL.md §7): the server reads both off
    // its own roster, and sending them would only invite a client to lie.
    json j;
    j["type"] = "Chat";
    j["text"] = text;
    return j.dump();
}

}  // namespace bomber::net
