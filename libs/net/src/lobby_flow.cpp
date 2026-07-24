#include "bomber/net/lobby_flow.hpp"

#include <utility>

#include "bomber/net/lobby_client.hpp"

namespace bomber::net {

namespace {
// Presence keep-alive cadence. The server drops a member that misses several of
// these (PROTOCOL.md §3 Heartbeat), so stay comfortably under its window.
constexpr std::int64_t kHeartbeatMs = 5000;
// How long to wait for the punch before giving up. Relay fallback (Phase 2)
// takes over from here once it exists.
constexpr int kPunchTimeoutMs = 5000;
}  // namespace

LobbyFlow::LobbyFlow(Config cfg, UdpTransport& game_transport, LobbyClient& client)
    : cfg_(std::move(cfg)), transport_(game_transport), client_(client) {}

LobbyFlow::~LobbyFlow() = default;

int LobbyFlow::rtt_ms() const {
    return punch_ ? punch_->rtt_ms() : 0;
}

void LobbyFlow::fail(const std::string& why) {
    error_ = why;
    phase_ = Phase::Failed;
}

void LobbyFlow::host_lobby(const std::string& lobby_name, bool is_public, int max_seats) {
    if (phase_ != Phase::Idle && phase_ != Phase::Failed) return;
    error_.clear();
    pending_ = Pending::Create;
    pending_name_ = lobby_name;
    pending_public_ = is_public;
    pending_max_seats_ = max_seats;
    phase_ = Phase::Connecting;
    if (!client_.is_open()) client_.connect(cfg_.server_url);
}

void LobbyFlow::join_lobby(const std::string& code) {
    if (phase_ != Phase::Idle && phase_ != Phase::Failed) return;
    error_.clear();
    pending_ = Pending::Join;
    pending_code_ = code;
    phase_ = Phase::Connecting;
    if (!client_.is_open()) client_.connect(cfg_.server_url);
}

void LobbyFlow::set_ready(bool ready) {
    if (phase_ != Phase::InLobby) return;
    client_.set_ready(ready);
}

void LobbyFlow::start_match() {
    // Host-only; the server re-validates host_token + all-ready anyway, and
    // answers Error{not_host,not_all_ready,...} which surfaces through
    // handle_server_message.
    if (phase_ != Phase::InLobby || !is_host()) return;
    client_.start_match(lobby_id_, host_token_, match_start_.input_delay,
                        /*match_config_digest=*/0);
}

void LobbyFlow::begin_candidate_gathering(std::int64_t now_ms) {
    // The "host" (LAN) candidate: the address this machine uses toward the
    // matchmaker, paired with the bound game port. On a LAN-only setup (no
    // reachable STUN) this alone is enough to punch.
    local_candidates_.clear();
    const std::string lan = local_ip_toward(cfg_.stun_host, cfg_.stun_port);
    if (!lan.empty())
        local_candidates_.push_back(
            LobbyCandidate{"host", lan + ":" + std::to_string(transport_.local_port()), ""});

    // The "reflexive" candidate comes from the STUN echo, probed out of the SAME
    // socket so the NAT binding matches the one the peer will punch.
    stun_ = std::make_unique<StunClient>(transport_, cfg_.stun_host, cfg_.stun_port,
                                         "seat" + std::to_string(my_seat_) + "-" + lobby_id_);
    candidates_sent_ = false;
    stun_->step(now_ms);
}

void LobbyFlow::publish_candidates() {
    if (candidates_sent_) return;
    if (stun_ && stun_->ok())
        local_candidates_.push_back(LobbyCandidate{"reflexive", stun_->reflexive_addr(), ""});
    // Even with zero candidates we post: the peer then knows we produced none
    // (and, for a same-machine test, its own poll_from still learns our source).
    client_.send_candidates(lobby_id_, my_seat_, local_candidates_);
    candidates_sent_ = true;
}

void LobbyFlow::begin_rendezvous(std::int64_t now_ms) {
    // Punch toward every OTHER seat's candidates. For 2P that is exactly the one
    // peer; the N>2 star (guests punch only the hub) lands in Phase 4.
    std::vector<Rendezvous::Candidate> targets;
    for (std::size_t seat = 0; seat < peer_candidates_.size(); ++seat) {
        if (static_cast<int>(seat) == my_seat_) continue;
        for (const LobbyCandidate& c : peer_candidates_[seat]) {
            std::string h;
            std::uint16_t p = 0;
            if (split_host_port(c.addr, &h, &p)) targets.push_back({h, p});
        }
    }
    if (targets.empty()) {
        fail("NO PEER ADDRESS - CANNOT CONNECT");
        return;
    }
    // Distinct per-seat nonce so each peer recognises only its OWN ping echoed
    // back (a shared nonce could latch on the peer's ping before ours got out).
    const std::uint32_t nonce =
        match_start_.seed ^ (0x9E3779B1u * static_cast<std::uint32_t>(my_seat_ + 1));
    punch_ = std::make_unique<Rendezvous>(transport_, std::move(targets), nonce, kPunchTimeoutMs);
    phase_ = Phase::Rendezvous;
    punch_->step(now_ms);
}

void LobbyFlow::handle_server_message(const LobbyServerMessage& msg) {
    switch (msg.type) {
        case LobbyMsgType::LobbyCreated:
            code_ = msg.code;
            lobby_id_ = msg.lobby_id;
            host_token_ = msg.host_token;
            my_seat_ = msg.your_seat;
            host_seat_ = msg.your_seat;  // the creator IS the host
            roster_.clear();
            roster_.push_back(RosterEntry{my_seat_, cfg_.player_name, false, true, -1});
            phase_ = Phase::InLobby;
            break;

        case LobbyMsgType::JoinAccepted:
            lobby_id_ = msg.lobby_id;
            my_seat_ = msg.your_seat;
            roster_ = msg.roster;
            for (const RosterEntry& e : roster_)
                if (e.is_host) host_seat_ = e.seat;
            phase_ = Phase::InLobby;
            break;

        case LobbyMsgType::JoinRejected:
            if (msg.reason == "build_mismatch")
                fail("VERSION MISMATCH - UPDATE THE GAME");
            else if (msg.reason == "full")
                fail("LOBBY IS FULL");
            else if (msg.reason == "in_progress")
                fail("MATCH ALREADY STARTED");
            else
                fail("LOBBY NOT FOUND");
            break;

        case LobbyMsgType::RosterUpdate:
            roster_ = msg.roster;
            for (const RosterEntry& e : roster_)
                if (e.is_host) host_seat_ = e.seat;
            break;

        case LobbyMsgType::PeerCandidates:
            if (msg.candidates_seat >= 0) {
                const auto seat = static_cast<std::size_t>(msg.candidates_seat);
                if (peer_candidates_.size() <= seat) peer_candidates_.resize(seat + 1);
                peer_candidates_[seat] = msg.candidates;
            }
            break;

        case LobbyMsgType::StartMatch:
            match_start_.seed = msg.seed;
            match_start_.input_delay = msg.input_delay > 0 ? msg.input_delay : 2;
            match_start_.local_seats_mask = msg.local_seats_mask;
            match_start_.hub_seat = msg.hub_seat;
            match_start_.seat_assign = msg.seat_assign;
            // begin_rendezvous needs a clock; step() picks this up next pump via
            // the Rendezvous-pending flag below.
            phase_ = Phase::Rendezvous;
            punch_.reset();  // built on the next step(), which has now_ms
            break;

        case LobbyMsgType::Error:
            // Control-plane rejections that are not fatal to the lobby itself
            // (e.g. not_all_ready when the host jumps the gun) surface as text
            // without tearing the room down.
            if (msg.error_code == "not_all_ready")
                error_ = "ALL PLAYERS MUST BE READY";
            else if (msg.error_code == "not_enough_players")
                error_ = "NEED AT LEAST TWO PLAYERS";
            else if (msg.error_code == "build_mismatch")
                fail("VERSION MISMATCH - UPDATE THE GAME");
            else
                error_ = msg.error_message.empty() ? msg.error_code : msg.error_message;
            break;

        case LobbyMsgType::ReanchorAccepted:
            code_ = msg.code;
            lobby_id_ = msg.lobby_id;
            host_token_ = msg.host_token;
            host_seat_ = my_seat_;  // we are the new hub (design §8.3)
            break;

        case LobbyMsgType::PublicList:
        case LobbyMsgType::HeartbeatAck:
        case LobbyMsgType::Unknown:
            break;
    }
}

void LobbyFlow::step(std::int64_t now_ms) {
    if (phase_ == Phase::Idle || phase_ == Phase::Failed) return;

    // Flush the queued create/join once the socket is actually up.
    if (pending_ != Pending::None && client_.is_open()) {
        if (pending_ == Pending::Create)
            client_.create_lobby(pending_public_ ? "public" : "private", pending_name_,
                                 pending_max_seats_, cfg_.build_hash, cfg_.player_name);
        else
            client_.join_by_code(pending_code_, cfg_.build_hash, cfg_.player_name);
        pending_ = Pending::None;
    }

    client_.poll_messages([this](const LobbyServerMessage& m) { handle_server_message(m); });

    if (phase_ == Phase::InLobby) {
        // Candidate gathering starts as soon as we know our seat, and publishes
        // when STUN resolves (or gives up — the LAN candidate still stands).
        if (!stun_ && !candidates_sent_ && my_seat_ >= 0) begin_candidate_gathering(now_ms);
        if (stun_ && !candidates_sent_) {
            stun_->step(now_ms);
            if (stun_->done()) publish_candidates();
        }
        if (last_heartbeat_ms_ < 0 || now_ms - last_heartbeat_ms_ >= kHeartbeatMs) {
            client_.heartbeat();
            last_heartbeat_ms_ = now_ms;
        }
    } else if (phase_ == Phase::Rendezvous) {
        if (!punch_) {
            begin_rendezvous(now_ms);
            return;  // fail() may have fired
        }
        punch_->step(now_ms);
        if (punch_->connected())
            phase_ = Phase::Ready;
        else if (punch_->failed())
            fail("COULD NOT REACH PLAYER - NAT BLOCKED");
    }
}

}  // namespace bomber::net
