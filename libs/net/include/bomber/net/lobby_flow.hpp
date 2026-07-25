#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "bomber/net/lobby_messages.hpp"
#include "bomber/net/relayed_transport.hpp"
#include "bomber/net/rendezvous.hpp"
#include "bomber/net/star_hub_transport.hpp"
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

// One line of PORT-ONLY lobby chat (PROTOCOL.md §7) as the flow keeps it: the
// speaker's seat and name plus the body, both already reduced to what the
// front-end font can draw. NOT an RE'd concept — the 1997 game has no chat.
struct ChatLine {
    std::string name;  // the speaker, by the same identity the roster shows
    std::string text;
    int seat = -1;  // the server's attribution; -1 only for a malformed frame
};

class LobbyFlow {
public:
    enum class Phase : std::uint8_t {
        Idle,        // nothing started yet
        Connecting,  // WebSocket opening; the create/join request is queued
        InLobby,     // in the waiting room: roster live, candidates exchanging
        Rendezvous,  // START received; punching a path to the peer
        Relaying,    // the punch failed; falling back through the server's relay
        Ready,       // connected — match_start() is valid, transport() is usable
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
    // Open the control connection (if needed) and ask for the public match list;
    // the answer lands in public_lobbies() and bumps public_list_revision().
    // Usable from Idle — browsing does not commit you to a lobby.
    void browse_public();
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

    // The transport to hand the RollbackSession once Ready: the bare UDP socket
    // on a direct path, or the relay wrapper when the punch failed. The session
    // above is byte-for-byte identical either way (ADR-0011 design §4).
    Transport& transport();
    bool is_relayed() const { return relay_ != nullptr; }

    // The most recent public match list, and a counter the browser screen can
    // watch to know a fresh answer arrived (rather than polling for changes).
    const std::vector<PublicLobby>& public_lobbies() const { return public_lobbies_; }
    unsigned public_list_revision() const { return public_list_revision_; }

    // --- PORT-ONLY lobby chat (PROTOCOL.md §7) ---
    //
    // NOT a reverse-engineered feature: the 1997 game has no chat. It lives on
    // the control plane because that is the only link players share while they
    // are still in the lobby, and it stays usable for as long as this object
    // does — which is now past the punch, through the online setup screens.
    //
    // The chat token bucket, mirrored EXACTLY from the server's so the client
    // refuses (and can say why) precisely where the server would drop.
    static constexpr std::int64_t kChatCreditPerMsgMs = 2000;  // what one message costs
    static constexpr int kChatBurstMsgs = 4;                   // how many may be banked
    static constexpr std::size_t kChatLogLines = 8;            // the ring the GUI renders

    // Send one line. Returns false — and sends NOTHING — when it is empty after
    // sanitising, when this peer holds no seat, or when the bucket is dry; the
    // caller can then keep the draft and say so instead of losing it to a
    // server-side drop.
    bool send_chat(const std::string& text, std::int64_t now_ms);

    // The last kChatLogLines messages, oldest first, and a counter that moves
    // whenever one arrives — the same shape public_lobbies()/
    // public_list_revision() use, so a screen can spot "something new" without
    // diffing the ring.
    const std::vector<ChatLine>& chat_log() const { return chat_log_; }
    unsigned chat_revision() const { return chat_revision_; }

private:
    enum class Pending : std::uint8_t { None, Create, Join, List };

    void fail(const std::string& why);
    void begin_candidate_gathering(std::int64_t now_ms);
    void publish_candidates();
    void begin_rendezvous(std::int64_t now_ms);
    void begin_relay_fallback();  // punch failed → ask the server for an allocation
    // Every peer derives every seat's punch nonce identically from the shared
    // seed, so an inbound ping's nonce names its sender's seat.
    std::uint32_t seat_nonce(int seat) const;
    std::vector<Rendezvous::Candidate> candidates_of(int seat) const;
    // The seat this peer exchanges datagrams with: the star hub, or (as the hub
    // itself, or in a 2P lobby) the other occupied seat.
    int peer_seat() const;

    // Members are grouped by ALIGNMENT, not by topic (the topics are called out
    // in the comments instead): pointer-sized first, then 4-byte, then the
    // 1-byte flags. Interleaving them by topic cost ~35 bytes of padding, which
    // clang-analyzer-optin.performance.Padding rightly flags.

    // --- pointer-aligned ---
    Config cfg_;
    UdpTransport& transport_;
    LobbyClient& client_;
    std::string error_;
    std::string pending_name_;  // queued intent until the socket is open
    std::string pending_code_;
    std::string code_;  // lobby identity
    std::string lobby_id_;
    std::string host_token_;
    std::vector<RosterEntry> roster_;
    std::unique_ptr<StunClient> stun_;                        // candidates
    std::vector<LobbyCandidate> local_candidates_;
    std::vector<std::vector<LobbyCandidate>> peer_candidates_;  // by seat (index = seat)
    // Public browsing (Phase 3): the last PublicList answer. Kept beside the
    // lobby state rather than in a separate object because it rides the same
    // control connection and the same poll pump.
    std::vector<PublicLobby> public_lobbies_;
    MatchStart match_start_;
    std::unique_ptr<Rendezvous> punch_;
    // Relay fallback (Phase 2). Requested only after the punch gives up; once
    // the allocation arrives the wrapper becomes the match transport.
    std::unique_ptr<RelayedTransport> relay_;
    // The >2-seat star (Phase 4): built on the HUB only, from the punched
    // address of every guest. Guests need nothing extra — their socket is
    // set_peer'd to the hub and the hub reflects the other seats' frames.
    std::unique_ptr<StarHubTransport> star_;
    // Lobby chat: the recent-message ring plus this peer's own send bucket.
    std::vector<ChatLine> chat_log_;
    std::int64_t last_heartbeat_ms_ = -1;
    // When START arrived with the peers' candidates not yet in hand (see
    // begin_rendezvous): the deadline is measured from here, not from the punch.
    std::int64_t candidate_wait_start_ms_ = -1;
    std::int64_t last_chat_ms_ = -1;  // the bucket's last refill instant
    std::int64_t chat_credit_ms_ = kChatCreditPerMsgMs * kChatBurstMsgs;

    // --- 4-byte ---
    int pending_max_seats_ = 2;
    int my_seat_ = -1;
    int host_seat_ = 0;
    unsigned public_list_revision_ = 0;
    unsigned chat_revision_ = 0;

    // --- 1-byte ---
    Phase phase_ = Phase::Idle;
    Pending pending_ = Pending::None;
    bool pending_public_ = false;
    bool candidates_sent_ = false;
    bool stun_pending_ = false;  // a reflexive candidate may still be added
    bool relay_requested_ = false;
};

}  // namespace bomber::net
