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
// How long START waits for the peers' candidates to arrive before declaring them
// unreachable. Generous: it covers a server round trip plus a STUN probe that is
// still timing out, and costs nothing when the exchange already completed.
constexpr std::int64_t kCandidateWaitMs = 6000;

// How long to keep verifying the DIRECT path, measured from the START of the
// punch rather than from our own success. It has to outlast the PEER's punch
// window: winning ours in one round trip says nothing about how long the other
// side needs, and giving up before its kPunchTimeoutMs would relay a pair whose
// direct path was about to work. The slack covers the peer's own
// verification-and-linger on top.
constexpr int kDirectVerifyMs = kPunchTimeoutMs + 2000;
// How long to keep verifying a RELAY allocation. Long, because the peer may
// still be spending its whole punch window before it asks for its own handle,
// and the relay drops everything aimed at a seat that has not allocated yet
// (services/matchmaker/internal/relay/relay.go's drop_unknown_dst) — so the
// wait is genuinely "until the other side arrives", not a round trip.
constexpr int kRelayVerifyMs = 15000;
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
    stun_pending_ = true;
    stun_->step(now_ms);
    publish_candidates();  // the LAN address goes out NOW, not after STUN resolves
}

void LobbyFlow::publish_candidates() {
    // Posted TWICE on purpose: once the moment we know our LAN address, and again
    // when STUN adds the reflexive one. Waiting for STUN before the first post was
    // a real bug — the probe can take its full timeout (an unreachable or slow
    // echo), and nothing stops the host pressing START inside that window, so both
    // peers would reach the punch holding no address for each other and fail with
    // "NO PEER ADDRESS". Posting early closes that race; the server keeps the
    // latest list per seat, so the second post simply supersedes the first.
    if (candidates_sent_ && !stun_pending_) return;
    std::vector<LobbyCandidate> list = local_candidates_;
    if (stun_ && stun_->ok())
        list.push_back(LobbyCandidate{"reflexive", stun_->reflexive_addr(), ""});
    // Even with zero candidates we post: the peer then knows we produced none
    // (and, for a same-machine test, its own poll_from still learns our source).
    client_.send_candidates(lobby_id_, my_seat_, list);
    candidates_sent_ = true;
    if (stun_ && stun_->done()) stun_pending_ = false;  // nothing further to add
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

bool LobbyFlow::peer_candidates_ready(const std::vector<int>& seats, bool star, bool am_hub,
                                      std::int64_t now_ms) {
    // START does not wait for the candidate exchange, so a peer's addresses may
    // legitimately still be in flight when it lands. Missing candidates therefore
    // mean "not yet", not "never": hold in Rendezvous and let step() retry.
    // Failing on the first pump is what made a fast START fail with NO PEER
    // ADDRESS against a remote matchmaker while working locally, where the
    // exchange completes in microseconds.
    if (candidate_wait_start_ms_ < 0) candidate_wait_start_ms_ = now_ms;
    for (int seat : seats) {
        if (seat == my_seat_) continue;
        const bool must_reach = !star || am_hub || seat == match_start_.hub_seat;
        if (!must_reach || !candidates_of(seat).empty()) continue;
        if (now_ms - candidate_wait_start_ms_ >= kCandidateWaitMs)
            fail("NO PEER ADDRESS - CANNOT CONNECT");
        return false;  // retry next pump, unless the wait is spent
    }
    return true;
}

bool LobbyFlow::build_hub_punch(const std::vector<int>& seats) {
    // The HUB opens a path to every guest over its ONE socket. A single
    // multi-peer Rendezvous does them all: separate objects would each poll the
    // same socket and eat one another's datagrams.
    std::vector<Rendezvous::PeerSpec> specs;
    for (int seat : seats) {
        if (seat == my_seat_) continue;
        Rendezvous::PeerSpec s;
        s.seat = seat;
        s.nonce = seat_nonce(seat);
        s.candidates = candidates_of(seat);
        if (s.candidates.empty()) {
            fail("A PLAYER HAS NO REACHABLE ADDRESS");
            return false;
        }
        specs.push_back(std::move(s));
    }
    if (specs.empty()) {
        fail("NO PEER ADDRESS - CANNOT CONNECT");
        return false;
    }
    punch_ = std::make_unique<Rendezvous>(transport_, std::move(specs), seat_nonce(my_seat_),
                                          kPunchTimeoutMs);
    return true;
}

bool LobbyFlow::build_guest_punch(bool star) {
    // A GUEST punches only toward the hub (linear, not quadratic); in a plain
    // 2-seat lobby that hub IS the other player. The 2-peer form set_peer()s the
    // winner, which is all a guest needs — the hub reflects everyone else.
    const int target = star ? match_start_.hub_seat : peer_seat();
    std::vector<Rendezvous::Candidate> targets = candidates_of(target);
    if (targets.empty()) {
        fail("NO PEER ADDRESS - CANNOT CONNECT");
        return false;
    }
    punch_ = std::make_unique<Rendezvous>(transport_, std::move(targets), seat_nonce(my_seat_),
                                          kPunchTimeoutMs);
    return true;
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
    if (!peer_candidates_ready(seats, star, am_hub, now_ms)) return;

    const bool built = (star && am_hub) ? build_hub_punch(seats) : build_guest_punch(star);
    if (!built) return;
    punch_start_ms_ = now_ms;
    punch_->step(now_ms);
}

bool LobbyFlow::can_relay() const {
    return std::popcount(match_start_.all_seats_mask) <= 2;
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
    if (!can_relay()) {
        fail("NO DIRECT PATH - RELAY NEEDS 2 PLAYERS");
        return;
    }
    relay_requested_ = true;
    probe_.reset();  // whatever we were verifying, we are leaving it
    phase_ = Phase::Relaying;
    client_.send(encode_allocate_relay(lobby_id_, my_seat_));
}

void LobbyFlow::step_verify(std::int64_t now_ms) {
    // THE CONVERGENCE STEP (design §4.1). A punch outcome is per-peer and
    // unsynchronised, so nothing is handed to the match layer until the peer has
    // proved it is on the SAME path (link_probe.hpp).
    if (!probe_) {
        // Deadline for the DIRECT path is measured from the punch's start so it
        // outlasts the peer's own punch window; the relay's is a flat wait for
        // the other seat to allocate.
        int deadline = kRelayVerifyMs;
        if (!relay_) {
            const std::int64_t base = punch_start_ms_ >= 0 ? punch_start_ms_ : now_ms;
            const std::int64_t left = static_cast<std::int64_t>(kDirectVerifyMs) - (now_ms - base);
            deadline = static_cast<int>(std::max<std::int64_t>(left, kProbeIntervalMs * 4));
        }
        probe_ = std::make_unique<LinkProbe>(transport(), seat_nonce(my_seat_), deadline);
    }
    probe_->step(now_ms);
    if (probe_->verified()) {
        phase_ = Phase::Ready;
        return;
    }
    if (!probe_->expired()) return;

    if (relay_) {
        // The relay was the last resort and the peer never showed up on it.
        // Failing loudly beats handing the match layer a transport that carries
        // nothing — the silent half-match this phase exists to stop.
        fail("NO PATH TO THE OTHER PLAYER");
        return;
    }
    if (!can_relay()) {
        // A STAR has no relay to escalate to, so this is the end of the line, and
        // the wording is deliberate: our OWN punch very likely worked, and what
        // did not is the hub's with some other guest. "NO DIRECT PATH" would blame
        // the path this peer is standing on, which would be a lie.
        fail("A PLAYER COULD NOT BE REACHED");
        return;
    }
    // The direct path did not carry both ways. The relay is where both peers
    // converge, because it is the one path neither of them ever leaves.
    begin_relay_fallback();
}

void LobbyFlow::adopt_roster(const std::vector<RosterEntry>& roster) {
    roster_ = roster;
    for (const RosterEntry& e : roster_)
        if (e.is_host) host_seat_ = e.seat;
}

void LobbyFlow::fail_join_rejected(const std::string& reason) {
    // Every arm is fatal to the attempt — the room we asked for is not one we can
    // enter. The reasons are the server's (PROTOCOL.md §2 JoinRejected); an
    // unrecognised one becomes "no such lobby", the only thing a client can
    // honestly say about a code refused for a reason this build never heard of.
    if (reason == "build_mismatch") {
        fail("VERSION MISMATCH - UPDATE THE GAME");
        return;
    }
    if (reason == "full") {
        fail("LOBBY IS FULL");
        return;
    }
    if (reason == "in_progress") {
        fail("MATCH ALREADY STARTED");
        return;
    }
    fail("LOBBY NOT FOUND");
}

namespace {
// Fold one seat index into a mask, ignoring anything out of range.
void add_seat(std::uint16_t* mask, int seat) {
    if (seat >= 0 && seat < sim::kMaxPlayers) *mask |= static_cast<std::uint16_t>(1U << seat);
}
}  // namespace

std::uint16_t LobbyFlow::derive_all_seats_mask() const {
    // Falling back to the roster covers an older server that omits seat_assign;
    // our own seat is folded in last so this peer is never missing from the mask
    // it plays in.
    std::uint16_t mask = 0;
    for (const int seat : match_start_.seat_assign) add_seat(&mask, seat);
    if (mask == 0)
        for (const RosterEntry& e : roster_) add_seat(&mask, e.seat);
    return static_cast<std::uint16_t>(mask | match_start_.local_seats_mask);
}

void LobbyFlow::adopt_match_start(const LobbyServerMessage& msg) {
    match_start_.seed = msg.seed;
    match_start_.input_delay = msg.input_delay > 0 ? msg.input_delay : 2;
    match_start_.local_seats_mask = msg.local_seats_mask;
    match_start_.hub_seat = msg.hub_seat;
    match_start_.seat_assign = msg.seat_assign;
    match_start_.all_seats_mask = derive_all_seats_mask();
    // begin_rendezvous needs a clock; step() picks this up next pump via the
    // Rendezvous-pending flag below.
    phase_ = Phase::Rendezvous;
    punch_.reset();  // built on the next step(), which has now_ms
}

void LobbyFlow::report_server_error(const LobbyServerMessage& msg) {
    // Control-plane rejections that are not fatal to the lobby itself (e.g.
    // not_all_ready when the host jumps the gun) surface as text without tearing
    // the room down.
    //
    // The relay was our last resort (the punch already failed), so a rejection
    // while Relaying — an older server without Phase 2, or an allocation failure
    // — ends the attempt rather than hanging. It is checked on the PHASE and so
    // comes first; the rest are a table over the server's error code.
    if (phase_ == Phase::Relaying) {
        fail("RELAY UNAVAILABLE - CANNOT CONNECT");
        return;
    }
    if (msg.error_code == "build_mismatch") {
        fail("VERSION MISMATCH - UPDATE THE GAME");
        return;
    }
    if (msg.error_code == "not_all_ready") {
        error_ = "ALL PLAYERS MUST BE READY";
        return;
    }
    if (msg.error_code == "not_enough_players") {
        error_ = "NEED AT LEAST TWO PLAYERS";
        return;
    }
    error_ = msg.error_message.empty() ? msg.error_code : msg.error_message;
}

void LobbyFlow::adopt_peer_candidates(const LobbyServerMessage& msg) {
    // Indexed by seat, so the vector grows to fit whichever seat spoke.
    if (msg.candidates_seat < 0) return;
    const auto seat = static_cast<std::size_t>(msg.candidates_seat);
    if (peer_candidates_.size() <= seat) peer_candidates_.resize(seat + 1);
    peer_candidates_[seat] = msg.candidates;
}

void LobbyFlow::adopt_relay_allocation(const LobbyServerMessage& msg) {
    std::string host;
    std::uint16_t port = 0;
    std::array<std::uint8_t, kRelayAllocIdBytes> alloc{};
    const int dst = peer_seat();
    if (!split_host_port(msg.relay_addr, &host, &port) || !parse_alloc_id(msg.alloc_id, &alloc) ||
        dst < 0) {
        fail("RELAY UNAVAILABLE - CANNOT CONNECT");
        return;
    }
    relay_ = std::make_unique<RelayedTransport>(transport_, host, port, alloc, dst);
    // NOT Ready yet. An allocation is a handle, not a path: the relay drops
    // everything aimed at a seat that has not allocated its own
    // (drop_unknown_dst), and the peer only allocates when ITS punch gives up —
    // which may be seconds away or may never happen. Verify before handing this
    // to the match layer.
    probe_.reset();  // a new path: the old verdict is about the old one
    phase_ = Phase::Verifying;
}

void LobbyFlow::append_chat_line(const LobbyServerMessage& msg) {
    // Another player's typing, arriving over a socket: untrusted input, handled
    // like every other frame in this codebase. Both halves are re-reduced HERE
    // rather than taken on the server's word — a client must never render bytes
    // merely because something relayed them.
    ChatLine line;
    line.seat = msg.chat_seat;
    line.name = sanitize_chat_name(msg.chat_name);
    line.text = sanitize_chat_text(msg.chat_text);
    if (line.text.empty()) return;  // nothing drawable survived: not a message
    if (line.name.empty()) line.name = "?";
    chat_log_.push_back(std::move(line));
    if (chat_log_.size() > kChatLogLines) chat_log_.erase(chat_log_.begin());
    ++chat_revision_;
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
            adopt_roster(msg.roster);
            phase_ = Phase::InLobby;
            break;

        case LobbyMsgType::JoinRejected: fail_join_rejected(msg.reason); break;

        case LobbyMsgType::RosterUpdate: adopt_roster(msg.roster); break;

        case LobbyMsgType::PeerCandidates: adopt_peer_candidates(msg); break;

        case LobbyMsgType::StartMatch: adopt_match_start(msg); break;

        case LobbyMsgType::Error: report_server_error(msg); break;

        case LobbyMsgType::RelayAllocated: adopt_relay_allocation(msg); break;

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

        case LobbyMsgType::Chat: append_chat_line(msg); break;

        case LobbyMsgType::HeartbeatAck:
        case LobbyMsgType::Unknown:
            break;
    }
}

void LobbyFlow::flush_pending() {
    if (pending_ == Pending::None || !client_.is_open()) return;
    switch (pending_) {
        case Pending::Create:
            client_.create_lobby(pending_public_ ? "public" : "private", pending_name_,
                                 pending_max_seats_, cfg_.build_hash, cfg_.player_name);
            break;
        case Pending::Join:
            client_.join_by_code(pending_code_, cfg_.build_hash, cfg_.player_name);
            break;
        case Pending::List:
            client_.list_public(cfg_.build_hash);  // the server flags each row build_ok
            break;
        case Pending::None: break;
    }
    pending_ = Pending::None;
}

void LobbyFlow::send_heartbeat(std::int64_t now_ms) {
    // For every phase that still HOLDS a seat; a member that stops speaking is
    // reaped (PROTOCOL.md §3). The connect phases are included, not skipped: a
    // reaped member loses its RELAY ALLOCATION too, so going quiet while punching
    // would destroy the very handle the wait is for.
    //
    // The Idle/Failed test is NOT redundant with step()'s own early return: the
    // server poll runs in between, and a frame handled there can fail the flow
    // (JoinRejected, a fatal Error) inside this very pump. Keeping a seatless
    // corpse on the server's roster is exactly what this must not do.
    if (my_seat_ < 0 || phase_ == Phase::Idle || phase_ == Phase::Failed) return;
    if (last_heartbeat_ms_ >= 0 && now_ms - last_heartbeat_ms_ < kHeartbeatMs) return;
    client_.heartbeat();
    last_heartbeat_ms_ = now_ms;
}

void LobbyFlow::pump_stun(std::int64_t now_ms) {
    if (!stun_ || !stun_pending_) return;
    stun_->step(now_ms);
    if (stun_->done()) publish_candidates();
}

void LobbyFlow::step_rendezvous(std::int64_t now_ms) {
    // STUN may still be in flight when START lands (the host does not wait for
    // us). Keep pumping it so our reflexive candidate still reaches the peer.
    pump_stun(now_ms);
    if (!punch_) {
        begin_rendezvous(now_ms);
        return;  // may still be waiting for candidates, or fail() may have fired
    }
    punch_->step(now_ms);
    // failed() BEFORE connected(), and the order is load-bearing: an early return
    // on "not connected yet" would swallow the failure and leave the flow sitting
    // in Rendezvous forever with no relay ever requested.
    if (punch_->failed()) {
        begin_relay_fallback();  // no direct path — go through the server
        return;
    }
    if (!punch_->connected()) return;
    // The hub of a >2-seat match now knows every guest's punched address, so wrap
    // the socket in the star (fan-out + guest<->guest reflection). A guest — and
    // either side of a 2-seat match — needs nothing: its socket is already
    // set_peer'd to the one peer it talks to.
    if (punch_->winners().size() > 1) {
        std::vector<StarHubTransport::Guest> guests;
        guests.reserve(punch_->winners().size());
        for (const Rendezvous::Winner& w : punch_->winners())
            guests.push_back({w.addr.host, w.addr.port});
        star_ = std::make_unique<StarHubTransport>(transport_, std::move(guests));
    }
    // A punch proves the path to whoever received the PONG and says nothing about
    // what the OTHER end concluded, so NOBODY plays until the peer has proved it
    // is on the same path (link_probe.hpp). EVERY topology, including the star —
    // gating this on can_relay() was a shipped bug, and design §4.1 has the
    // failure it allowed and the honest limit that remains.
    phase_ = Phase::Verifying;
}

void LobbyFlow::step(std::int64_t now_ms) {
    if (phase_ == Phase::Idle || phase_ == Phase::Failed) return;
    flush_pending();
    client_.poll_messages([this](const LobbyServerMessage& m) { handle_server_message(m); });
    send_heartbeat(now_ms);

    switch (phase_) {
        case Phase::InLobby:
            // Candidate gathering starts as soon as we know our seat. The LAN
            // address is posted immediately; STUN keeps running and re-posts with
            // the reflexive one when it resolves (or gives up).
            if (!stun_ && !candidates_sent_ && my_seat_ >= 0) begin_candidate_gathering(now_ms);
            pump_stun(now_ms);
            break;
        case Phase::Rendezvous: step_rendezvous(now_ms); break;
        case Phase::Verifying: step_verify(now_ms); break;
        // Relaying just waits for RelayAllocated; the poll above delivers it.
        // Connecting waits for the socket, and Ready is the caller's business.
        case Phase::Connecting:
        case Phase::Relaying:
        case Phase::Ready:
        case Phase::Idle:
        case Phase::Failed: break;
    }
}

}  // namespace bomber::net
