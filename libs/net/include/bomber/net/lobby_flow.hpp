#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "bomber/net/lobby_messages.hpp"
#include "bomber/net/rendezvous.hpp"
#include "bomber/net/stun_client.hpp"
#include "bomber/net/udp_transport.hpp"

// The online-lobby STATE MACHINE (ADR-0011, design §5.1): one SDL-free object
// that drives the whole pre-match sequence — connect to the matchmaker, create
// or join a lobby, gather candidates (local + STUN reflexive), exchange them,
// wait in the room, and on the host's START punch a direct UDP path — then hands
// the caller a CONNECTED transport plus the authoritative match parameters.
//
// Keeping it here (not in libs/game) means the GUI screen is thin: it renders
// phase()/roster()/code() and forwards the user's intents, while every protocol
// rule lives in one headless-testable place. handle_server_message() is public
// precisely so tests can drive the machine with synthetic frames and assert the
// transitions without a live server.
//
// Clock-injected like Rendezvous/StunClient: step(now_ms) takes the caller's
// monotonic clock, so libs/net stays clock-free.
namespace bomber::net {

class LobbyClient;

class LobbyFlow {
public:
    enum class Phase {
        Idle,        // nothing started yet
        Connecting,  // WebSocket opening; the create/join request is queued
        InLobby,     // in the waiting room: roster live, candidates exchanging
        Rendezvous,  // START received; punching a path to the peer
        Ready,       // punched — match_start() is valid, the transport is connected
        Failed,      // error() explains; the GUI returns to the menu
    };

    struct Config {
        std::string server_url;              // "ws://host:8080/ws"
        std::string stun_host;               // matchmaker host for the UDP STUN echo
        std::uint16_t stun_port = 8081;
        std::string player_name = "PLAYER";
        std::uint32_t build_hash = 0;        // ours; the server rejects mismatches
    };

    // Everything the caller needs to start the deterministic match, straight
    // from the server's authoritative StartMatch (design §1.6).
    struct MatchStart {
        std::uint32_t seed = 0;
        int input_delay = 2;
        std::uint16_t local_seats_mask = 0;  // the seats THIS peer owns
        int hub_seat = 0;
        std::vector<int> seat_assign;
    };

    // `game_transport` must already be bound() — the SAME socket used for STUN,
    // the punch, and the match, so the NAT binding stays the one peers punched.
    LobbyFlow(Config cfg, UdpTransport& game_transport, LobbyClient& client);
    ~LobbyFlow();
    LobbyFlow(const LobbyFlow&) = delete;
    LobbyFlow& operator=(const LobbyFlow&) = delete;

    // --- user intents (safe to call any time; ignored when out of phase) ---
    void host_lobby(const std::string& lobby_name, bool is_public, int max_seats);
    void join_lobby(const std::string& code);
    void set_ready(bool ready);
    void start_match();  // host only; the server validates all-ready

    // Pump the whole machine once (WS drain, STUN, candidates, heartbeat, punch).
    void step(std::int64_t now_ms);

    // Apply one decoded server frame. Called by step(); public for tests.
    void handle_server_message(const LobbyServerMessage& msg);

    Phase phase() const { return phase_; }
    const std::string& code() const { return code_; }
    const std::vector<RosterEntry>& roster() const { return roster_; }
    int my_seat() const { return my_seat_; }
    bool is_host() const { return my_seat_ >= 0 && my_seat_ == host_seat_; }
    const std::string& error() const { return error_; }
    // Valid once phase() == Ready.
    const MatchStart& match_start() const { return match_start_; }
    // Round-trip time of the punched path in ms (0 until Ready).
    int rtt_ms() const;

private:
    enum class Pending { None, Create, Join };

    void fail(const std::string& why);
    void begin_candidate_gathering(std::int64_t now_ms);
    void publish_candidates();
    void begin_rendezvous(std::int64_t now_ms);

    Config cfg_;
    UdpTransport& transport_;
    LobbyClient& client_;

    Phase phase_ = Phase::Idle;
    std::string error_;

    // queued intent until the socket is open
    Pending pending_ = Pending::None;
    std::string pending_name_;
    bool pending_public_ = false;
    int pending_max_seats_ = 2;
    std::string pending_code_;

    // lobby identity
    std::string code_;
    std::string lobby_id_;
    std::string host_token_;
    int my_seat_ = -1;
    int host_seat_ = 0;
    std::vector<RosterEntry> roster_;

    // candidates
    std::unique_ptr<StunClient> stun_;
    bool candidates_sent_ = false;
    std::vector<LobbyCandidate> local_candidates_;
    // peer candidates by seat (index = seat)
    std::vector<std::vector<LobbyCandidate>> peer_candidates_;

    std::int64_t last_heartbeat_ms_ = -1;

    MatchStart match_start_;
    std::unique_ptr<Rendezvous> punch_;
};

}  // namespace bomber::net
