#include "bomber/net/rollback_session.hpp"

#include <iterator>
#include <vector>

#include "bomber/net/input_codec.hpp"  // pack_input, for input equality
#include "bomber/net/protocol.hpp"

namespace bomber::net {

namespace {
constexpr std::uint32_t kHashWindow = 256;

bool same_input(const sim::PlayerInput& a, const sim::PlayerInput& b) {
    return pack_input(a) == pack_input(b);
}
}  // namespace

RollbackSession::RollbackSession(sim::Simulation& sim, std::uint16_t local_seats,
                                 std::uint16_t all_seats, int max_prediction, Transport& transport)
    : sim_(&sim),
      transport_(&transport),
      local_seats_(local_seats),
      all_seats_(all_seats),
      remote_seats_(static_cast<std::uint16_t>(all_seats & ~local_seats)),
      max_prediction_(max_prediction) {}

sim::TickInputs RollbackSession::assemble(std::uint32_t tick) {
    sim::TickInputs in;
    const Slot& slot = slots_[tick];
    for (int s = 0; s < sim::kMaxPlayers; ++s) {
        const std::uint16_t bit = static_cast<std::uint16_t>(1U << s);
        if ((all_seats_ & bit) == 0) continue;  // AI/absent seat: the sim drives it, stay neutral
        const std::size_t si = static_cast<std::size_t>(s);
        in.players[si] = ((slot.confirmed & bit) != 0) ? slot.inputs.players[si]
                                                       : last_remote_input_.players[si];  // predict
    }
    return in;
}

void RollbackSession::apply_remote(std::uint32_t tick, std::uint16_t seats,
                                   const sim::TickInputs& in) {
    if (tick < confirmed_) return;  // already fully confirmed — a late duplicate is inert
    Slot& slot = slots_[tick];
    for (int s = 0; s < sim::kMaxPlayers; ++s) {
        const std::uint16_t bit = static_cast<std::uint16_t>(1U << s);
        if ((seats & bit) == 0) continue;
        if ((slot.confirmed & bit) != 0) continue;  // seat already confirmed for this tick
        const std::size_t si = static_cast<std::size_t>(s);
        const sim::PlayerInput& val = in.players[si];
        // If this tick was ALREADY simulated speculatively with a prediction that
        // turns out wrong, schedule a rollback to the earliest such tick.
        if (tick < tick_ && !same_input(slot.inputs.players[si], val)) {
            if (!rollback_pending_ || tick < rollback_to_) {
                rollback_to_ = tick;
                rollback_pending_ = true;
            }
        }
        slot.inputs.players[si] = val;
        slot.confirmed = static_cast<std::uint16_t>(slot.confirmed | bit);
        if (tick >= last_remote_tick_[si]) {  // newest confirmation feeds future prediction
            last_remote_input_.players[si] = val;
            last_remote_tick_[si] = tick;
        }
    }
}

void RollbackSession::receive() {
    std::vector<std::uint8_t> pkt;
    while (transport_->poll(&pkt)) {
        Message m;
        if (!decode(pkt.data(), pkt.size(), &m)) continue;  // drop malformed (untrusted)
        if (m.type == MsgType::InputRange) {
            const std::uint16_t remote = static_cast<std::uint16_t>(m.range.seat_mask & remote_seats_);
            if (remote == 0) continue;
            for (std::size_t i = 0; i < m.range.per_tick.size(); ++i)
                apply_remote(m.range.first_tick + static_cast<std::uint32_t>(i), remote,
                             m.range.per_tick[i]);
        } else if (m.type == MsgType::Input) {
            const std::uint16_t remote = static_cast<std::uint16_t>(m.input.seat_mask & remote_seats_);
            if (remote != 0) apply_remote(m.input.tick_index, remote, m.input.inputs);
        } else {
            note_peer_hash(m.hash.tick_index, m.hash.hash);
        }
    }
}

void RollbackSession::resimulate(std::uint32_t from) {
    // Restore the state as it was BEFORE `from`, then replay to the speculative
    // head with corrected inputs (still predicting any seat not yet confirmed).
    // Cheap: value-type State restore + pure tick().
    const auto snap = snapshots_.find(from);
    if (snap == snapshots_.end()) return;
    sim_->state() = snap->second;
    for (std::uint32_t t = from; t < tick_; ++t) {
        snapshots_[t] = sim_->state();
        const sim::TickInputs in = assemble(t);
        slots_[t].inputs = in;  // remember what we (re)simulated, for later misprediction checks
        sim_->tick(in);
    }
}

void RollbackSession::advance_confirmed() {
    // Raise confirmed_ across the contiguous prefix of all-seats-known ticks. A
    // confirmed tick's inputs never change again, so its result is final —
    // exchange + check its hash here (speculative frames are NOT hashed).
    while (confirmed_ < tick_) {
        const auto slot = slots_.find(confirmed_);
        if (slot == slots_.end() || slot->second.confirmed != all_seats_) break;
        std::uint64_t h = 0;
        if (confirmed_ + 1 < tick_) {
            const auto after = snapshots_.find(confirmed_ + 1);  // state AFTER this tick
            h = (after != snapshots_.end()) ? sim::state_hash(after->second) : sim_->hash();
        } else {
            h = sim_->hash();  // confirmed_ is the most recent simulated tick
        }
        hash_[confirmed_] = h;
        const std::vector<std::uint8_t> hp = encode_hash(confirmed_, h);
        transport_->send(hp.data(), hp.size());
        const auto peer = peer_hash_.find(confirmed_);
        if (peer != peer_hash_.end()) {
            if (peer->second != h && !desynced_) {
                desynced_ = true;
                desync_tick_ = confirmed_;
            }
            peer_hash_.erase(peer);
        }
        ++confirmed_;
    }
}

void RollbackSession::send_local(std::uint32_t from) {
    if (tick_ <= from) return;
    std::vector<sim::TickInputs> window;
    window.reserve(tick_ - from);
    for (std::uint32_t t = from; t < tick_; ++t) window.push_back(slots_[t].inputs);
    const std::vector<std::uint8_t> pkt = encode_input_range(from, local_seats_, window);
    transport_->send(pkt.data(), pkt.size());
}

void RollbackSession::note_peer_hash(std::uint32_t tick, std::uint64_t peer_hash) {
    const auto ours = hash_.find(tick);
    if (ours != hash_.end()) {
        if (ours->second != peer_hash && !desynced_) {
            desynced_ = true;
            desync_tick_ = tick;
        }
    } else {
        peer_hash_[tick] = peer_hash;
    }
}

void RollbackSession::prune() {
    // Everything strictly below confirmed_ is final and never needed again
    // (rollback never targets a confirmed tick).
    for (auto it = slots_.begin(); it != slots_.end();)
        it = (it->first < confirmed_) ? slots_.erase(it) : std::next(it);
    for (auto it = snapshots_.begin(); it != snapshots_.end();)
        it = (it->first < confirmed_) ? snapshots_.erase(it) : std::next(it);
    const std::uint32_t keep = (confirmed_ > kHashWindow) ? confirmed_ - kHashWindow : 0;
    for (auto it = hash_.begin(); it != hash_.end();)
        it = (it->first < keep) ? hash_.erase(it) : std::next(it);
    for (auto it = peer_hash_.begin(); it != peer_hash_.end();)
        it = (it->first < keep) ? peer_hash_.erase(it) : std::next(it);
}

void RollbackSession::advance(const sim::TickInputs& local_input) {
    receive();
    if (rollback_pending_) {
        resimulate(rollback_to_);
        rollback_pending_ = false;
    }
    advance_confirmed();

    // Hold at the prediction cap so the display never runs unboundedly ahead of
    // the peer (bounded memory + bounded re-sim on a correction). Keep re-sending
    // local input so the peer catches up and our confirmations advance.
    if (tick_ - confirmed_ >= static_cast<std::uint32_t>(max_prediction_)) {
        send_local(confirmed_);
        return;
    }

    // Merge the local input INTO the tick's slot (which may already hold remote
    // input that arrived before we reached this tick — never wipe that).
    Slot& slot = slots_[tick_];
    for (int s = 0; s < sim::kMaxPlayers; ++s) {
        const std::uint16_t bit = static_cast<std::uint16_t>(1U << s);
        if ((local_seats_ & bit) != 0)
            slot.inputs.players[static_cast<std::size_t>(s)] =
                local_input.players[static_cast<std::size_t>(s)];
    }
    slot.confirmed = static_cast<std::uint16_t>(slot.confirmed | local_seats_);

    snapshots_[tick_] = sim_->state();
    const sim::TickInputs in = assemble(tick_);
    slots_[tick_].inputs = in;  // store actually-simulated inputs (predicted remote incl.) for compares
    sim_->tick(in);
    ++tick_;
    send_local(confirmed_);  // now covers [confirmed_, tick_): includes the just-simulated tick
    prune();
}

}  // namespace bomber::net
