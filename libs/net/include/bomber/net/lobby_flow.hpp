#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "bomber/net/link_probe.hpp"
#include "bomber/net/lobby_messages.hpp"
#include "bomber/net/relayed_transport.hpp"
#include "bomber/net/rendezvous.hpp"
#include "bomber/net/star_hub_transport.hpp"
#include "bomber/net/stun_client.hpp"
#include "bomber/net/udp_transport.hpp"

// The online-lobby STATE MACHINE (ADR-0011, design §5.1): one SDL-free object
// driving the whole pre-match sequence — connect, create or join, gather and
// exchange candidates, wait in the room, punch on the host's START — and then
// handing the caller a CONNECTED transport plus the match parameters.
//
// Keeping it here rather than in libs/game means the GUI screen is thin, and
// every protocol rule lives in one headless-testable place.
// handle_server_message() is public precisely so tests can drive the machine
// with synthetic frames and no live server. Clock-injected.
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
        Relaying,    // no verified path; waiting for the server's relay allocation
        Verifying,   // a path exists; proving BOTH peers are on it (link_probe.hpp)
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
        // Every NETWORK seat in the match, derived from the server's final
        // seat->player binding: the `all_seats` a RollbackSession is built with
        // and the set a SetupSession collects acks over. AI slots are NOT here —
        // they are simulated identically everywhere from the shared config and
        // their input never crosses the wire.
        std::uint16_t all_seats_mask = 0;
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

    // --- mid-match relay failover (design §4.2; path_failover.hpp drives it) --
    //
    // The match keeps this flow alive by PUMPING step() (which heartbeats every
    // phase holding a seat, Ready included — that is what keeps the membership
    // AllocateRelay requires from being reaped ~30 s into the match). When the
    // failover engine fires, it asks HERE for the allocation; the answer rides
    // the same poll pump. No server change: handleAllocateRelay is ungated and
    // idempotent per (lobby, seat), PROTOCOL.md §6.1.

    // Ask for this seat's allocation while Ready. Idempotent; a no-op in any
    // other phase (the pre-match fallback owns those) or once a relay exists.
    void request_match_relay();
    // The relay wrapper, once RelayAllocated lands. The engine — not this flow —
    // verifies it and swaps the session onto it; phase stays Ready throughout.
    RelayedTransport* match_relay() { return relay_.get(); }
    // The server refused (any Error while the mid-match request was the only
    // outstanding one — the same reading the pre-match fallback applies).
    bool match_relay_refused() const { return match_relay_refused_; }
    // Does the live roster still hold `seat`? A seat that VANISHED mid-match is
    // a peer whose membership is gone (it quit, or was reaped) and whose own
    // AllocateRelay can only be refused. Presence proves nothing — the reaper
    // takes up to 40 s (design §4.2's timing asymmetry) — so callers may act
    // only on absence.
    bool roster_has_seat(int seat) const;
    // This peer's probe nonce, for the failover's LinkProbe — the same per-seat
    // derivation the punch used, so the two ends recognise each other's probes.
    std::uint32_t local_probe_nonce() const { return seat_nonce(my_seat_); }

    // The most recent public match list, and a counter the browser screen can
    // watch to know a fresh answer arrived (rather than polling for changes).
    const std::vector<PublicLobby>& public_lobbies() const { return public_lobbies_; }
    unsigned public_list_revision() const { return public_list_revision_; }

    // --- PORT-ONLY lobby chat (PROTOCOL.md §7) ---
    //
    // NOT a reverse-engineered feature: the 1997 game has no chat. It rides the
    // control plane, the only link players share while still in the lobby.
    //
    // The token bucket is mirrored EXACTLY from the server's, so the client
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
    // Keep the STUN probe turning and re-publish once it resolves. Runs in BOTH
    // InLobby and Rendezvous — the host does not wait for us, so START can land
    // with our reflexive candidate still in flight — which is why it is one
    // function rather than the same four lines written twice.
    void pump_stun(std::int64_t now_ms);
    // Send the queued create/join/list once the control socket is actually open.
    void flush_pending();
    // Presence keep-alive for every phase that still HOLDS a seat.
    void send_heartbeat(std::int64_t now_ms);
    // The Rendezvous phase's own pump: punch, then wrap the socket in the star if
    // this machine is the hub of one, then hand over to verification.
    void step_rendezvous(std::int64_t now_ms);
    void begin_rendezvous(std::int64_t now_ms);
    // Are the addresses this peer must punch toward all in hand? START does not
    // wait for the candidate exchange, so "missing" means "not yet" until
    // kCandidateWaitMs has passed — and fail()s after it.
    bool peer_candidates_ready(const std::vector<int>& seats, bool star, bool am_hub,
                               std::int64_t now_ms);
    bool build_hub_punch(const std::vector<int>& seats);  // multi-peer, hub only
    bool build_guest_punch(bool star);                    // 2-peer, toward the hub
    void begin_relay_fallback();  // no verified path -> ask the server for an allocation
    // Enter Verifying on whatever transport() currently is: build the probe (if it
    // is not already up), pump it, and act on its verdict.
    void step_verify(std::int64_t now_ms);
    // Does this match have a relay fallback at all? RelayedTransport addresses
    // exactly ONE destination seat, so only a 2-seat match can escalate.
    //
    // IT DOES NOT DECIDE WHETHER TO VERIFY — every topology verifies, and gating
    // that on this was a shipped bug (design §4.1). What it decides is only what
    // happens when verification EXPIRES: escalate to the relay, or fail, because
    // a star has nowhere to converge TO.
    bool can_relay() const;
    // Every peer derives every seat's punch nonce identically from the shared
    // seed, so an inbound ping's nonce names its sender's seat.
    std::uint32_t seat_nonce(int seat) const;
    std::vector<Rendezvous::Candidate> candidates_of(int seat) const;
    // The seat this peer exchanges datagrams with: the star hub, or (as the hub
    // itself, or in a 2P lobby) the other occupied seat.
    int peer_seat() const;

    // --- the arms of handle_server_message() ---------------------------------
    //
    // That function is a FLAT dispatch over LobbyMsgType and stays one: a
    // strategy hierarchy over a handful of fixed wire messages is a pattern
    // solving no problem we have (coding-standards §8's opening rule — NOT its
    // faithful-port exception, which protects the 1997 binary's switches;
    // LobbyMsgType is port-invented, ADR-0011). Named for what they decide, not
    // for the message that triggers them.
    void adopt_roster(const std::vector<RosterEntry>& roster);
    void fail_join_rejected(const std::string& reason);
    void adopt_match_start(const LobbyServerMessage& msg);
    // THE seat topology for everything downstream (session masks, setup acks),
    // taken from the server's authoritative seat_assign rather than assumed.
    std::uint16_t derive_all_seats_mask() const;
    void report_server_error(const LobbyServerMessage& msg);
    void adopt_peer_candidates(const LobbyServerMessage& msg);
    void adopt_relay_allocation(const LobbyServerMessage& msg);
    void append_chat_line(const LobbyServerMessage& msg);

    // Members are grouped by ALIGNMENT, not by topic: interleaving them by topic
    // cost ~35 bytes of padding, which
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
    // The mutual path check that stands between a punch (or an allocation) and
    // Ready. Rebuilt from scratch when the path changes, because "does THIS path
    // carry?" is a question about one transport.
    std::unique_ptr<LinkProbe> probe_;
    // Relay fallback (Phase 2). Requested only after no direct path verified;
    // once the allocation arrives the wrapper becomes the match transport.
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
    // When the punch began. The DIRECT verification budget is measured from
    // here, not from our own success, so it still covers the peer's whole punch
    // window however early we won ours (step_verify).
    std::int64_t punch_start_ms_ = -1;
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
    // The mid-match (Ready-phase) allocation, kept apart from relay_requested_:
    // the pre-match fallback moves the PHASE and the failover must not.
    bool match_relay_requested_ = false;
    bool match_relay_refused_ = false;
};

}  // namespace bomber::net
