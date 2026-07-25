#include "bomber/net/lobby_flow.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <utility>

#include "bomber/net/lobby_client.hpp"
#include "bomber/sim/constants.hpp"  // sim::kMaxPlayers

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

Transport& LobbyFlow::transport() {
    if (relay_) return *relay_;
    if (star_) return *star_;
    return transport_;
}

int LobbyFlow::peer_seat() const {
    // Datagrams go to the star hub — unless WE are the hub (or it is a plain 2P
    // lobby), in which case they go to the other occupied seat.
    if (match_start_.hub_seat != my_seat_) return match_start_.hub_seat;
    for (int seat : match_start_.seat_assign)
        if (seat != my_seat_) return seat;
    for (const RosterEntry& e : roster_)
        if (e.seat != my_seat_) return e.seat;
    return -1;
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

void LobbyFlow::browse_public() {
    // Browsing is not a commitment: we ride the control connection up, ask, and
    // drop back to Idle when the answer lands, so the player can still host or
    // join by code afterwards.
    if (phase_ != Phase::Idle && phase_ != Phase::Failed) return;
    error_.clear();
    pending_ = Pending::List;
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

bool LobbyFlow::send_chat(const std::string& text, std::int64_t now_ms) {
    // PORT-ONLY (PROTOCOL.md §7). Holding a seat is what makes a message
    // deliverable at all — the server refuses chat from a seatless connection,
    // so there is no point putting one on the wire.
    if (my_seat_ < 0) return false;
    const std::string clean = sanitize_chat_text(text);
    if (clean.empty()) return false;

    // The server's own token bucket, run locally: credit accrues with elapsed
    // time whether or not the last attempt went out. Refusing here beats letting
    // the server drop the line, because a drop is silent by design (§7.3) and
    // the player would never learn the message did not land.
    if (last_chat_ms_ >= 0) chat_credit_ms_ += now_ms - last_chat_ms_;
    last_chat_ms_ = now_ms;
    chat_credit_ms_ = std::min(chat_credit_ms_, kChatCreditPerMsgMs * kChatBurstMsgs);
    if (chat_credit_ms_ < kChatCreditPerMsgMs) return false;
    chat_credit_ms_ -= kChatCreditPerMsgMs;

    client_.send_chat(clean);
    return true;  // our own copy arrives with everyone else's, off the server's echo
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

// Every peer derives every seat's punch nonce the same way from the shared seed,
// so an inbound ping's nonce identifies WHICH SEAT sent it (Rendezvous's
// multi-peer attribution) — and each peer still recognises only its own echo.
std::uint32_t LobbyFlow::seat_nonce(int seat) const {
    return match_start_.seed ^ (0x9E3779B1u * static_cast<std::uint32_t>(seat + 1));
}

std::vector<Rendezvous::Candidate> LobbyFlow::candidates_of(int seat) const {
    std::vector<Rendezvous::Candidate> out;
    const auto idx = static_cast<std::size_t>(seat);
    if (seat < 0 || idx >= peer_candidates_.size()) return out;
    for (const LobbyCandidate& c : peer_candidates_[idx]) {
        std::string h;
        std::uint16_t p = 0;
        if (split_host_port(c.addr, &h, &p)) out.push_back({h, p});
    }
    return out;
}

void LobbyFlow::begin_rendezvous(std::int64_t now_ms) {
    phase_ = Phase::Rendezvous;
    // Which seats are in this match: the server's assignment when it sent one,
    // else the roster we already hold.
    std::vector<int> seats = match_start_.seat_assign;
    if (seats.empty())
        for (const RosterEntry& e : roster_) seats.push_back(e.seat);

    const bool am_hub = match_start_.hub_seat == my_seat_;
    const bool star = seats.size() > 2;

    if (star && am_hub) {
        // The HUB opens a path to every guest over its ONE socket. A single
        // multi-peer Rendezvous does them all: separate objects would each poll
        // the same socket and eat one another's datagrams.
        std::vector<Rendezvous::PeerSpec> specs;
        for (int seat : seats) {
            if (seat == my_seat_) continue;
            Rendezvous::PeerSpec s;
            s.seat = seat;
            s.nonce = seat_nonce(seat);
            s.candidates = candidates_of(seat);
            if (s.candidates.empty()) {
                fail("A PLAYER HAS NO REACHABLE ADDRESS");
                return;
            }
            specs.push_back(std::move(s));
        }
        if (specs.empty()) {
            fail("NO PEER ADDRESS - CANNOT CONNECT");
            return;
        }
        punch_ = std::make_unique<Rendezvous>(transport_, std::move(specs), seat_nonce(my_seat_),
                                             kPunchTimeoutMs);
    } else {
        // A GUEST punches only toward the hub (linear, not quadratic); in a plain
        // 2-seat lobby that hub IS the other player. The 2-peer form set_peer()s
        // the winner, which is all a guest needs — the hub reflects everyone else.
        const int target = star ? match_start_.hub_seat : peer_seat();
        std::vector<Rendezvous::Candidate> targets = candidates_of(target);
        if (targets.empty()) {
            fail("NO PEER ADDRESS - CANNOT CONNECT");
            return;
        }
        punch_ = std::make_unique<Rendezvous>(transport_, std::move(targets),
                                             seat_nonce(my_seat_), kPunchTimeoutMs);
    }
    punch_->step(now_ms);
}

void LobbyFlow::begin_relay_fallback() {
    // Symmetric NAT / CGNAT: the port the STUN server saw is not the port used
    // toward the peer, so no candidate pair can complete. Route through the
    // server's forwarder instead (ADR-0011 decision 3 — relay ships in v1).
    if (relay_requested_) return;
    // ... but ONLY for a 2-seat match. RelayedTransport addresses exactly ONE
    // destination seat (relayed_transport.hpp) and the star needs fan-out plus
    // guest↔guest reflection, so a relayed hub would silently deliver a guest's
    // input to nobody. Say so instead: a half-connected match desyncs on tick 0,
    // and there is no half-relayed topology to fall back to.
    if (std::popcount(match_start_.all_seats_mask) > 2) {
        fail("NO DIRECT PATH - RELAY NEEDS 2 PLAYERS");
        return;
    }
    relay_requested_ = true;
    phase_ = Phase::Relaying;
    client_.send(encode_allocate_relay(lobby_id_, my_seat_));
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
            // THE seat topology for everything downstream (session masks, setup
            // acks), taken from the server's authoritative seat_assign rather
            // than assumed. Falling back to the roster covers an older server
            // that omits the field; our own seat is folded in last so this peer
            // is never missing from the mask it plays in.
            match_start_.all_seats_mask = 0;
            for (const int seat : match_start_.seat_assign)
                if (seat >= 0 && seat < sim::kMaxPlayers)
                    match_start_.all_seats_mask |= static_cast<std::uint16_t>(1U << seat);
            if (match_start_.all_seats_mask == 0)
                for (const RosterEntry& e : roster_)
                    if (e.seat >= 0 && e.seat < sim::kMaxPlayers)
                        match_start_.all_seats_mask |= static_cast<std::uint16_t>(1U << e.seat);
            match_start_.all_seats_mask |= match_start_.local_seats_mask;
            // begin_rendezvous needs a clock; step() picks this up next pump via
            // the Rendezvous-pending flag below.
            phase_ = Phase::Rendezvous;
            punch_.reset();  // built on the next step(), which has now_ms
            break;

        case LobbyMsgType::Error:
            // Control-plane rejections that are not fatal to the lobby itself
            // (e.g. not_all_ready when the host jumps the gun) surface as text
            // without tearing the room down.
            if (phase_ == Phase::Relaying)
                // The relay was our last resort (the punch already failed), so a
                // rejection here — an older server without Phase 2, or an
                // allocation failure — ends the attempt rather than hanging.
                fail("RELAY UNAVAILABLE - CANNOT CONNECT");
            else if (msg.error_code == "not_all_ready")
                error_ = "ALL PLAYERS MUST BE READY";
            else if (msg.error_code == "not_enough_players")
                error_ = "NEED AT LEAST TWO PLAYERS";
            else if (msg.error_code == "build_mismatch")
                fail("VERSION MISMATCH - UPDATE THE GAME");
            else
                error_ = msg.error_message.empty() ? msg.error_code : msg.error_message;
            break;

        case LobbyMsgType::RelayAllocated: {
            std::string host;
            std::uint16_t port = 0;
            std::array<std::uint8_t, kRelayAllocIdBytes> alloc{};
            const int dst = peer_seat();
            if (!split_host_port(msg.relay_addr, &host, &port) ||
                !parse_alloc_id(msg.alloc_id, &alloc) || dst < 0) {
                fail("RELAY UNAVAILABLE - CANNOT CONNECT");
                break;
            }
            relay_ = std::make_unique<RelayedTransport>(transport_, host, port, alloc, dst);
            phase_ = Phase::Ready;  // relayed, but ready to play
            break;
        }

        case LobbyMsgType::ReanchorAccepted:
            code_ = msg.code;
            lobby_id_ = msg.lobby_id;
            host_token_ = msg.host_token;
            host_seat_ = my_seat_;  // we are the new hub (design §8.3)
            break;

        case LobbyMsgType::PublicList:
            public_lobbies_ = msg.lobbies;
            ++public_list_revision_;
            // A browse is finished the moment its answer arrives — drop back to
            // Idle so the player can pick a row (join_lobby) or host instead.
            if (phase_ == Phase::Connecting) phase_ = Phase::Idle;
            break;

        case LobbyMsgType::Chat: {
            // Another player's typing, arriving over a socket: untrusted input,
            // handled like every other frame in this codebase. Both halves are
            // re-reduced HERE rather than taken on the server's word — a client
            // must never render bytes merely because something relayed them.
            ChatLine line;
            line.seat = msg.chat_seat;
            line.name = sanitize_chat_name(msg.chat_name);
            line.text = sanitize_chat_text(msg.chat_text);
            if (line.text.empty()) break;  // nothing drawable survived: not a message
            if (line.name.empty()) line.name = "?";
            chat_log_.push_back(std::move(line));
            if (chat_log_.size() > kChatLogLines) chat_log_.erase(chat_log_.begin());
            ++chat_revision_;
            break;
        }

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
        else if (pending_ == Pending::Join)
            client_.join_by_code(pending_code_, cfg_.build_hash, cfg_.player_name);
        else  // Pending::List — the server flags each row build_ok against ours
            client_.list_public(cfg_.build_hash);
        pending_ = Pending::None;
    }

    client_.poll_messages([this](const LobbyServerMessage& m) { handle_server_message(m); });

    // Presence keep-alive, for every phase that still HOLDS a seat rather than
    // the waiting room alone. The control link now outlives the punch — the
    // online setup screens keep pumping this flow so lobby chat stays live
    // there — and a member that stops speaking is reaped (PROTOCOL.md §3).
    if (my_seat_ >= 0 && (phase_ == Phase::InLobby || phase_ == Phase::Ready) &&
        (last_heartbeat_ms_ < 0 || now_ms - last_heartbeat_ms_ >= kHeartbeatMs)) {
        client_.heartbeat();
        last_heartbeat_ms_ = now_ms;
    }

    if (phase_ == Phase::InLobby) {
        // Candidate gathering starts as soon as we know our seat, and publishes
        // when STUN resolves (or gives up — the LAN candidate still stands).
        if (!stun_ && !candidates_sent_ && my_seat_ >= 0) begin_candidate_gathering(now_ms);
        if (stun_ && !candidates_sent_) {
            stun_->step(now_ms);
            if (stun_->done()) publish_candidates();
        }
    } else if (phase_ == Phase::Rendezvous) {
        if (!punch_) {
            begin_rendezvous(now_ms);
            return;  // fail() may have fired
        }
        punch_->step(now_ms);
        if (punch_->connected()) {
            // The hub of a >2-seat match now knows every guest's punched address,
            // so wrap the socket in the star (fan-out + guest<->guest reflection).
            // A guest — and either side of a 2-seat match — needs nothing: its
            // socket is already set_peer'd to the one peer it talks to.
            if (punch_->winners().size() > 1) {
                std::vector<StarHubTransport::Guest> guests;
                guests.reserve(punch_->winners().size());
                for (const Rendezvous::Winner& w : punch_->winners())
                    guests.push_back({w.addr.host, w.addr.port});
                star_ = std::make_unique<StarHubTransport>(transport_, std::move(guests));
            }
            phase_ = Phase::Ready;
        } else if (punch_->failed()) {
            begin_relay_fallback();  // no direct path — go through the server
        }
    }
    // Phase::Relaying just waits for RelayAllocated (handled above); the poll
    // at the top of this function is what delivers it.
}

}  // namespace bomber::net
