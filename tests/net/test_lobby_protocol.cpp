// bomber::net lobby control-plane protocol tests (ADR-0011): the C++ encoders +
// parser must match the FROZEN wire contract in services/matchmaker/PROTOCOL.md
// byte-for-byte. Pure JSON round-trips — no sockets, no server. Built only under
// BOMBER_ENABLE_LOBBY (needs nlohmann/json), so it runs in the GUI presets.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <nlohmann/json.hpp>

#include "bomber/net/lobby_messages.hpp"

using namespace bomber::net;  // NOLINT(google-build-using-namespace) — test-local
using json = nlohmann::json;

TEST_CASE("hex_hash formats build_hash as the 0x wire form") {
    CHECK(hex_hash(0xA1B2C3D4u) == "0xA1B2C3D4");
    CHECK(hex_hash(0u) == "0x00000000");
    CHECK(hex_hash(0xC0FFEE01u) == "0xC0FFEE01");
}

TEST_CASE("client->server encoders produce PROTOCOL.md-shaped frames") {
    const json c = json::parse(encode_create_lobby("private", "Ege's game", 4, 0xA1B2C3D4u, "Ege"));
    CHECK(c["type"] == "CreateLobby");
    CHECK(c["visibility"] == "private");
    CHECK(c["name"] == "Ege's game");
    CHECK(c["max_seats"] == 4);
    CHECK(c["build_hash"] == "0xA1B2C3D4");
    CHECK(c["player"] == "Ege");

    const json jn = json::parse(encode_join_by_code("K7Q2MP", 0xA1B2C3D4u, "Ada"));
    CHECK(jn["type"] == "JoinByCode");
    CHECK(jn["code"] == "K7Q2MP");
    CHECK(jn["build_hash"] == "0xA1B2C3D4");

    const json sr = json::parse(encode_set_ready(true));
    CHECK(sr["type"] == "SetReady");
    CHECK(sr["ready"] == true);

    const json hb = json::parse(encode_heartbeat());
    CHECK(hb["type"] == "Heartbeat");

    const json sm = json::parse(encode_start_match("L1", "TOK", 2, 0xC0FFEE01u));
    CHECK(sm["type"] == "StartMatch");
    CHECK(sm["lobby_id"] == "L1");
    CHECK(sm["host_token"] == "TOK");
    CHECK(sm["input_delay"] == 2);
    CHECK(sm["match_config_digest"] == "0xC0FFEE01");

    const std::vector<LobbyCandidate> cands = {{"host", "192.168.1.9:41234", ""},
                                               {"relay", "relay.example:3478", "a1"}};
    const json cj = json::parse(encode_candidates("L1", 1, cands));
    CHECK(cj["type"] == "Candidates");
    CHECK(cj["seat"] == 1);
    REQUIRE(cj["list"].size() == 2);
    CHECK(cj["list"][0]["kind"] == "host");
    CHECK_FALSE(cj["list"][0].contains("alloc"));  // empty alloc is omitted
    CHECK(cj["list"][1]["alloc"] == "a1");

    const json ra = json::parse(encode_reanchor("K7Q2MP", "deadbeef"));
    CHECK(ra["type"] == "ReanchorLobby");
    CHECK(ra["roster_digest"] == "deadbeef");

    const json mo = json::parse(encode_match_over("L1"));
    CHECK(mo["type"] == "MatchOver");
}

// One SUBCASE per server frame rather than seven anonymous scopes in one body:
// the frames are independent, and a failure now names the frame that broke
// instead of only "decodes every server frame".
TEST_CASE("parse_server_message decodes every server frame") {
    SUBCASE("LobbyCreated") {
        const auto m = parse_server_message(
            R"({"type":"LobbyCreated","code":"K7Q2MP","lobby_id":"L","host_token":"T","your_seat":0})");
        CHECK(m.type == LobbyMsgType::LobbyCreated);
        CHECK(m.code == "K7Q2MP");
        CHECK(m.lobby_id == "L");
        CHECK(m.host_token == "T");
        CHECK(m.your_seat == 0);
    }
    SUBCASE("JoinAccepted carries the whole roster") {
        const auto m = parse_server_message(
            R"({"type":"JoinAccepted","lobby_id":"L","your_seat":1,)"
            R"("roster":[{"seat":0,"name":"Ege","ready":false,"is_host":true},)"
            R"({"seat":1,"name":"Ada","ready":true,"is_host":false}],"host_candidates":[]})");
        CHECK(m.type == LobbyMsgType::JoinAccepted);
        CHECK(m.your_seat == 1);
        REQUIRE(m.roster.size() == 2);
        CHECK(m.roster[0].name == "Ege");
        CHECK(m.roster[0].is_host);
        CHECK(m.roster[1].seat == 1);
        CHECK(m.roster[1].ready);
    }
    SUBCASE("StartMatch carries seed, seats, delay and topology") {
        const auto m = parse_server_message(
            R"({"type":"StartMatch","seed":1592371220,"seat_assign":[0,1],)"
            R"("match_config_digest":"0xC0FFEE01","input_delay":2,)"
            R"("topology":{"hub_seat":0},"local_seats_mask":2})");
        CHECK(m.type == LobbyMsgType::StartMatch);
        CHECK(m.seed == 1592371220u);
        REQUIRE(m.seat_assign.size() == 2);
        CHECK(m.seat_assign[1] == 1);
        CHECK(m.input_delay == 2);
        CHECK(m.hub_seat == 0);
        CHECK(m.local_seats_mask == 2);
    }
    SUBCASE("JoinRejected") {
        const auto m = parse_server_message(R"({"type":"JoinRejected","reason":"build_mismatch"})");
        CHECK(m.type == LobbyMsgType::JoinRejected);
        CHECK(m.reason == "build_mismatch");
    }
    SUBCASE("PeerCandidates") {
        const auto m = parse_server_message(
            R"({"type":"PeerCandidates","seat":0,"list":[{"kind":"host","addr":"192.168.1.9:41234"}]})");
        CHECK(m.type == LobbyMsgType::PeerCandidates);
        CHECK(m.candidates_seat == 0);
        REQUIRE(m.candidates.size() == 1);
        CHECK(m.candidates[0].kind == "host");
        CHECK(m.candidates[0].addr == "192.168.1.9:41234");
    }
    SUBCASE("PublicList") {
        const auto m = parse_server_message(
            R"({"type":"PublicList","lobbies":[{"code":"K7Q2MP","name":"g","players":2,"max":4,"build_ok":true}]})");
        CHECK(m.type == LobbyMsgType::PublicList);
        REQUIRE(m.lobbies.size() == 1);
        CHECK(m.lobbies[0].code == "K7Q2MP");
        CHECK(m.lobbies[0].players == 2);
        CHECK(m.lobbies[0].build_ok);
    }
    SUBCASE("Error") {
        const auto m = parse_server_message(R"({"type":"Error","code":"not_host","message":"nope"})");
        CHECK(m.type == LobbyMsgType::Error);
        CHECK(m.error_code == "not_host");
        CHECK(m.error_message == "nope");
    }
    SUBCASE("anything unrecognised is Unknown, never a throw and never acted on") {
        CHECK(parse_server_message("not json").type == LobbyMsgType::Unknown);
        CHECK(parse_server_message(R"({"type":"Nonsense"})").type == LobbyMsgType::Unknown);
        CHECK(parse_server_message("[]").type == LobbyMsgType::Unknown);
    }
}

TEST_CASE("STUN probe/reply match PROTOCOL.md §2") {
    const json p = json::parse(encode_stun_probe("tok-1"));
    CHECK(p["type"] == "StunProbe");
    CHECK(p["nonce"] == "tok-1");  // opaque STRING, not a number

    std::string nonce;
    std::string addr;
    REQUIRE(parse_stun_reply(R"({"type":"StunReply","nonce":"tok-1","your_addr":"81.2.3.4:52001"})",
                             &nonce, &addr));
    CHECK(nonce == "tok-1");
    CHECK(addr == "81.2.3.4:52001");

    // Anything that is not a well-formed StunReply is rejected, never thrown on.
    CHECK_FALSE(parse_stun_reply("garbage", &nonce, &addr));
    CHECK_FALSE(parse_stun_reply(R"({"type":"StunProbe","nonce":"x"})", &nonce, &addr));
    CHECK_FALSE(parse_stun_reply(R"({"type":"StunReply","nonce":"x"})", &nonce, &addr));
}

TEST_CASE("Chat encodes only the body and parses the server's attribution") {
    // PROTOCOL.md §7. The client frame carries the text and NOTHING else: the
    // server reads seat and name off its own roster, and sending them would only
    // tempt a client to lie about who is speaking.
    const json c = json::parse(encode_chat("gl hf"));
    CHECK(c["type"] == "Chat");
    CHECK(c["text"] == "gl hf");
    CHECK_FALSE(c.contains("seat"));
    CHECK_FALSE(c.contains("name"));

    const LobbyServerMessage m =
        parse_server_message(R"({"type":"Chat","seat":1,"name":"Ada","text":"gl hf"})");
    REQUIRE(m.type == LobbyMsgType::Chat);
    CHECK(m.chat_seat == 1);
    CHECK(m.chat_name == "Ada");
    CHECK(m.chat_text == "gl hf");

    // A frame missing its fields still parses (never throws); the flow's own
    // sanitising is what decides whether there is a message worth showing.
    const LobbyServerMessage bare = parse_server_message(R"({"type":"Chat"})");
    CHECK(bare.type == LobbyMsgType::Chat);
    CHECK(bare.chat_seat == -1);
    CHECK(bare.chat_text.empty());
}

TEST_CASE("sanitize_chat_* reduces untrusted text to what the FON can draw") {
    // Applied on BOTH sides of the socket: on send so we never ask the server to
    // relay bytes we could not draw, and on receive because a relayed frame is
    // still another player's typing.
    CHECK(sanitize_chat_text("hello") == "hello");
    CHECK(sanitize_chat_text("  padded  ") == "padded");        // blanks carry nothing
    CHECK(sanitize_chat_text("tab\there") == "tabhere");        // control codes dropped
    CHECK(sanitize_chat_text("a\nb") == "ab");                  // no line breaks in one line
    CHECK(sanitize_chat_text("merhaba d\xC3\xBCnya") == "merhaba dnya");  // FON is ASCII-only
    CHECK(sanitize_chat_text("").empty());
    CHECK(sanitize_chat_text("   ").empty());
    CHECK(sanitize_chat_text("\x01\x02").empty());

    // Clamped to the server's own cap, so a message is never rejected for length
    // by the far end after we already accepted it locally.
    const std::string over(kChatMaxBytes + 40, 'x');
    CHECK(sanitize_chat_text(over).size() == kChatMaxBytes);
    const std::string exact(kChatMaxBytes, 'y');
    CHECK(sanitize_chat_text(exact) == exact);

    // Names get the tighter clamp: one long name must not push a whole line of
    // chat off the panel.
    CHECK(sanitize_chat_name("Ada") == "Ada");
    CHECK(sanitize_chat_name(std::string(64, 'N')).size() == kChatMaxNameBytes);
}

TEST_CASE("split_host_port handles IPv4, bracketed IPv6, and junk") {
    std::string host;
    std::uint16_t port = 0;

    REQUIRE(split_host_port("81.2.3.4:52001", &host, &port));
    CHECK(host == "81.2.3.4");
    CHECK(port == 52001);

    REQUIRE(split_host_port("[::1]:52001", &host, &port));
    CHECK(host == "::1");
    CHECK(port == 52001);

    CHECK_FALSE(split_host_port("no-colon", &host, &port));
    CHECK_FALSE(split_host_port("1.2.3.4:", &host, &port));
    CHECK_FALSE(split_host_port(":52001", &host, &port));
    CHECK_FALSE(split_host_port("1.2.3.4:0", &host, &port));       // port 0 is not usable
    CHECK_FALSE(split_host_port("1.2.3.4:70000", &host, &port));   // out of range
    CHECK_FALSE(split_host_port("1.2.3.4:52a01", &host, &port));   // non-numeric
}
