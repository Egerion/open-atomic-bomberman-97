#include "bomber/net/lockstep_session.hpp"

#include <iterator>
#include <vector>

#include "bomber/net/protocol.hpp"

namespace bomber::net {

namespace {
constexpr std::uint32_t kHashWindow = 256;  // keep this many recent hashes for late peer compares
}  // namespace

LockstepSession::LockstepSession(sim::Simulation& sim, std::uint16_t local_seats,
                                 std::uint16_t all_seats, int input_delay, Transport& transport)
    : sim_(&sim),
      transport_(&transport),
      local_seats_(local_seats),
      all_seats_(all_seats),
      delay_(input_delay) {
    // Input-delay warm-up: ticks [0, delay) run on neutral input for every seat,
    // so no peer has to send anything for them (both agree on neutral).
    const sim::TickInputs neutral;
    for (std::uint32_t t = 0; t < static_cast<std::uint32_t>(delay_); ++t) {
        inputs_[t] = neutral;
        have_[t] = all_seats_;
    }
    input_tick_ = static_cast<std::uint32_t>(delay_);
}

void LockstepSession::fill_seats(std::uint32_t tick, std::uint16_t seats,
                                 const sim::TickInputs& in) {
    sim::TickInputs& slot = inputs_[tick];
    for (int s = 0; s < sim::kMaxPlayers; ++s)
        if ((seats & (1U << s)) != 0)
            slot.players[static_cast<std::size_t>(s)] = in.players[static_cast<std::size_t>(s)];
    have_[tick] = static_cast<std::uint16_t>(have_[tick] | seats);
}

void LockstepSession::note_peer_hash(std::uint32_t tick, std::uint64_t peer_hash) {
    const auto ours = hash_.find(tick);
    if (ours == hash_.end()) {
        peer_hash_[tick] = peer_hash;  // compare once we simulate this tick
        return;
    }
    if (ours->second == peer_hash || desynced_) return;
    desynced_ = true;
    desync_tick_ = tick;
}

// The remote seats a datagram carries: ours echoed back is ignored.
std::uint16_t LockstepSession::remote_of(std::uint16_t seat_mask) const {
    return static_cast<std::uint16_t>(seat_mask & ~local_seats_);
}

void LockstepSession::on_single_input(const Message& m) {
    // Only ticks not yet confirmed — a late duplicate of a past tick is inert.
    const std::uint16_t remote = remote_of(m.input.seat_mask);
    if (remote == 0 || m.input.tick_index < tick_) return;
    fill_seats(m.input.tick_index, remote, m.input.inputs);
}

void LockstepSession::on_input_range(const Message& m) {
    const std::uint16_t remote = remote_of(m.range.seat_mask);
    if (remote == 0) return;
    for (std::size_t i = 0; i < m.range.per_tick.size(); ++i) {
        const std::uint32_t t = m.range.first_tick + static_cast<std::uint32_t>(i);
        if (t >= tick_) fill_seats(t, remote, m.range.per_tick[i]);
    }
}

void LockstepSession::on_message(const Message& m) {
    if (m.type == MsgType::Input) {
        on_single_input(m);
        return;
    }
    if (m.type == MsgType::InputRange) {
        on_input_range(m);
        return;
    }
    // KNOWN LATENT BUG, PRESERVED DELIBERATELY so that fixing it is a separate,
    // testable change rather than a side effect of a formatting pass: this is a
    // CATCH-ALL, so any message that is not an input frame is read as a peer
    // HASH. `Message` default-constructs every frame, so a Drop or a MatchCtl
    // arriving here files hash 0 against tick 0 and latches a spurious desync.
    // Not reachable today — this session type is constructed only in tests, over
    // a link carrying exactly Input, InputRange and Hash — but it is one new
    // message on that link away from being live. The fix is to test
    // `m.type == MsgType::Hash` here; it needs a test in tests/net that sends a
    // non-input, non-hash datagram and asserts desynced() stays false.
    note_peer_hash(m.hash.tick_index, m.hash.hash);
}

void LockstepSession::receive() {
    std::vector<std::uint8_t> pkt;
    while (transport_->poll(&pkt)) {
        Message m;
        if (decode(pkt.data(), pkt.size(), &m)) on_message(m);  // malformed is dropped (untrusted)
    }
}

void LockstepSession::prune() {
    const std::uint32_t keep_from = (tick_ > kHashWindow) ? tick_ - kHashWindow : 0;
    for (auto it = hash_.begin(); it != hash_.end();)
        it = (it->first < keep_from) ? hash_.erase(it) : std::next(it);
    for (auto it = peer_hash_.begin(); it != peer_hash_.end();)
        it = (it->first < keep_from) ? peer_hash_.erase(it) : std::next(it);
}

bool LockstepSession::advance(const sim::TickInputs& local_input) {
    // Produce + send the local seats' input for the next input frame, bounded to
    // `delay` ticks ahead of the confirmed tick so a stall cannot run local input
    // unboundedly forward (and cannot re-send a different value for an already
    // sent tick).
    if (input_tick_ <= tick_ + static_cast<std::uint32_t>(delay_)) {
        fill_seats(input_tick_, local_seats_, local_input);
        ++input_tick_;
    }

    // Re-send the ENTIRE un-confirmed local-input window every tick (redundancy):
    // a dropped datagram is recovered by any LATER one that still carries the
    // missing tick, so lockstep survives UDP loss with no acks/resends — as long
    // as fewer than `delay + 1` consecutive packets are lost.
    if (input_tick_ > tick_) {
        std::vector<sim::TickInputs> window;
        window.reserve(input_tick_ - tick_);
        for (std::uint32_t t = tick_; t < input_tick_; ++t) window.push_back(inputs_[t]);
        const std::vector<std::uint8_t> pkt = encode_input_range(tick_, local_seats_, window);
        transport_->send(pkt.data(), pkt.size());
    }

    receive();

    const auto hit = have_.find(tick_);
    if (hit == have_.end() || hit->second != all_seats_) return false;  // stall: remote input missing

    sim_->tick(inputs_[tick_]);
    const std::uint64_t h = sim_->hash();
    hash_[tick_] = h;

    const std::vector<std::uint8_t> hp = encode_hash(tick_, h);
    transport_->send(hp.data(), hp.size());

    // A peer hash that arrived before we reached this tick can be checked now.
    const auto peer = peer_hash_.find(tick_);
    if (peer != peer_hash_.end()) {
        if (peer->second != h && !desynced_) {
            desynced_ = true;
            desync_tick_ = tick_;
        }
        peer_hash_.erase(peer);
    }

    // The just-confirmed tick's inputs are consumed and never needed again.
    inputs_.erase(tick_);
    have_.erase(tick_);
    ++tick_;
    prune();
    return true;
}

}  // namespace bomber::net
