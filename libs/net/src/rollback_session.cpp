#include "bomber/net/rollback_session.hpp"

#include <iterator>
#include <vector>

#include "bomber/net/input_codec.hpp"  // pack_input, for input equality
#include "bomber/net/protocol.hpp"

namespace bomber::net {

namespace {
constexpr std::uint32_t kHashWindow = 256;

// How far AHEAD of the host's own speculative head an abandon lands, on top of
// the prediction cap. The cap is the bound on how far any peer can have run past
// its confirmed frontier, and a peer's confirmed frontier can never be above the
// host's speculative head (it needs the host's input to get there) — so
// host_tick + max_prediction is an upper bound on every peer's current tick, and
// anything above it is a tick nobody has reached yet. That is the whole
// requirement: an end tick a peer had already passed would leave it having
// simulated and tallied more of the round than the host did. The extra ticks are
// slack, and incidentally give the abandon a fraction of a second of visible
// follow-through instead of a hard freeze on the keypress.
constexpr std::uint32_t kEndRoundSlackTicks = 12;


bool same_input(const sim::PlayerInput& a, const sim::PlayerInput& b) {
    return pack_input(a) == pack_input(b);
}
}  // namespace

RollbackSession::RollbackSession(sim::Simulation& sim, std::uint16_t local_seats,
                                 std::uint16_t all_seats, int max_prediction, Transport& transport,
                                 const DropPolicy& drop, std::uint32_t start_tick)
    : sim_(&sim),
      transport_(&transport),
      local_seats_(local_seats),
      all_seats_(all_seats),
      remote_seats_(static_cast<std::uint16_t>(all_seats & ~local_seats)),
      max_prediction_(max_prediction),
      drop_(drop),
      tick_(start_tick),
      confirmed_(start_tick),
      rollback_to_(start_tick) {
    // Diagnostics only (net_stats.hpp). transport.path() is asked ONCE here, at
    // the one moment the session and its transport are certainly the pair that
    // will carry this round — which is what makes "which path is carrying the
    // match" answerable from a `Transport&` the caller may have captured long ago.
    stats_.begin(transport.path(), remote_seats_, start_tick, max_prediction);
    handoff_.fill(kNoHandoff);
    // "First tick we hold no input for" starts at the session's own base, not 0 —
    // otherwise a seat that never speaks would be handed off at tick 0, which is
    // below this round's confirmed_ and outside its snapshot window.
    remote_next_.fill(start_tick);
}

std::uint16_t RollbackSession::seats_awaited(std::uint32_t tick) const {
    std::uint16_t m = all_seats_;
    if (dropped_ == 0) return m;  // fast path: nobody has dropped, nothing to subtract
    for (int s = 0; s < sim::kMaxPlayers; ++s) {
        const std::uint32_t at = handoff_[static_cast<std::size_t>(s)];
        if (at != kNoHandoff && tick >= at)
            m = static_cast<std::uint16_t>(m & ~static_cast<std::uint16_t>(1U << s));
    }
    return m;
}

void RollbackSession::apply_handoffs(std::uint32_t tick) {
    if (dropped_ == 0) return;
    sim::State& st = sim_->state();
    for (int s = 0; s < sim::kMaxPlayers; ++s) {
        const std::uint32_t at = handoff_[static_cast<std::size_t>(s)];
        if (at != kNoHandoff && tick >= at)
            st.players[static_cast<std::size_t>(s)].ai = true;  // idempotent by construction
    }
}

sim::TickInputs RollbackSession::assemble(std::uint32_t tick) {
    sim::TickInputs in;
    const Slot& slot = slots_[tick];
    // A handed-off seat drops out of `awaited` here, so it is fed the SAME
    // neutral input an ordinary AI seat gets. That matters: the AI OR-latches
    // the action keys out of the input it is handed, so leaking the dead peer's
    // last predicted input into its brain would make the seat behave differently
    // on a peer that predicted differently.
    const std::uint16_t awaited = seats_awaited(tick);
    for (int s = 0; s < sim::kMaxPlayers; ++s) {
        const std::uint16_t bit = static_cast<std::uint16_t>(1U << s);
        if ((awaited & bit) == 0) continue;  // AI/absent/handed-off: the sim drives it, stay neutral
        const std::size_t si = static_cast<std::size_t>(s);
        in.players[si] = ((slot.confirmed & bit) != 0) ? slot.inputs.players[si]
                                                       : last_remote_input_.players[si];  // predict
    }
    return in;
}

void RollbackSession::apply_remote(std::uint32_t tick, std::uint16_t seats,
                                   const sim::TickInputs& in) {
    if (tick < confirmed_) {
        // Already fully confirmed — a late duplicate is inert. Counted (not
        // ignored) because the redundancy window means MOST arriving input is
        // this, and the count is the cheapest proof that traffic is still
        // flowing; it is explicitly NOT a loss signal (net_stats.hpp).
        for (int s = 0; s < sim::kMaxPlayers; ++s)
            if ((seats & static_cast<std::uint16_t>(1U << s)) != 0) stats_.on_dup_input(s);
        return;
    }
    // Input from a seat that is already the AI's at this tick is inert too: the
    // handoff is the agreed truth, and honouring a straggler packet would both
    // diverge from the peers and trigger a pointless rollback.
    seats = static_cast<std::uint16_t>(seats & seats_awaited(tick));
    if (seats == 0) return;
    Slot& slot = slots_[tick];
    for (int s = 0; s < sim::kMaxPlayers; ++s) {
        const std::uint16_t bit = static_cast<std::uint16_t>(1U << s);
        if ((seats & bit) == 0) continue;
        const std::size_t si = static_cast<std::size_t>(s);
        if (tick + 1 > remote_next_[si]) remote_next_[si] = tick + 1;  // first tick still missing
        if ((slot.confirmed & bit) != 0) {
            stats_.on_dup_input(s);  // this tick's input from that seat is already held
            continue;
        }
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

void RollbackSession::note_input_seats(std::uint16_t seats, std::uint32_t first_tick) {
    for (int s = 0; s < sim::kMaxPlayers; ++s)
        if ((seats & static_cast<std::uint16_t>(1U << s)) != 0)
            stats_.on_input_from(s, first_tick);
}

void RollbackSession::receive() {
    std::vector<std::uint8_t> pkt;
    while (transport_->poll(&pkt)) {
        Message m;
        const bool decoded = decode(pkt.data(), pkt.size(), &m);
        stats_.on_datagram(decoded);
        if (!decoded) continue;  // drop malformed (untrusted)
        if (m.type == MsgType::InputRange) {
            const std::uint16_t remote = static_cast<std::uint16_t>(m.range.seat_mask & remote_seats_);
            if (remote == 0) continue;
            heard(remote);
            // THE ACK-RTT SAMPLE, and the reason this diagnostic needs no new
            // wire message: an InputRange always begins at the SENDER'S confirmed
            // frontier (send_local(confirmed_) on every path through advance()),
            // and that frontier cannot pass a tick our input has not reached. So
            // `first_tick` is an acknowledgement of our own tick first_tick-1.
            note_input_seats(remote, m.range.first_tick);
            for (std::size_t i = 0; i < m.range.per_tick.size(); ++i)
                apply_remote(m.range.first_tick + static_cast<std::uint32_t>(i), remote,
                             m.range.per_tick[i]);
        } else if (m.type == MsgType::Input) {
            const std::uint16_t remote = static_cast<std::uint16_t>(m.input.seat_mask & remote_seats_);
            if (remote == 0) continue;
            heard(remote);
            // A single-tick Input carries no frontier (only LockstepSession sends
            // these), so it counts as traffic but yields no RTT sample: passing
            // its tick would read as an acknowledgement it is not.
            note_input_seats(remote, 0);
            apply_remote(m.input.tick_index, remote, m.input.inputs);
        } else if (m.type == MsgType::Drop) {
            // Obeyed by EVERY peer including the host's own echo — idempotent, so
            // the redundant re-sends and any out-of-order copy are all no-ops.
            const int seat = static_cast<int>(m.drop.seat);  // decode() bounds-checked it
            if ((all_seats_ & static_cast<std::uint16_t>(1U << seat)) != 0)
                schedule_handoff(seat, m.drop.at_tick);
        } else if (m.type == MsgType::Hash) {
            note_peer_hash(m.hash.tick_index, m.hash.hash);
        } else if (m.type == MsgType::MatchCtl) {
            if (m.match_ctl.kind == MatchCtlKind::EndRound) {
                // Obeyed by EVERY peer including the host's own echo over a star
                // — idempotent, so re-sends and reordering are all no-ops.
                schedule_end_round(m.match_ctl.at_tick);
            } else if (m.match_ctl.kind == MatchCtlKind::EndRoundRequest && drop_.is_host) {
                // A guest asked. The host turns that into THE decision, on its
                // own clock, so the tick is still one nobody has passed. A guest
                // that receives this (a star reflects) ignores it.
                request_end_round();
            }
            // RematchWait/Rematch belong to the post-match shell (rematch_session
            // .hpp), which runs after this session is done — not ours to read.
        }
        // Hello/Punch: pre-match traffic on the shared socket, not ours to read.
    }
}

void RollbackSession::resimulate(std::uint32_t from) {
    // Restore the state as it was BEFORE `from`, then replay to the speculative
    // head with corrected inputs (still predicting any seat not yet confirmed).
    // Cheap: value-type State restore + pure tick().
    const auto snap = snapshots_.find(from);
    if (snap == snapshots_.end()) return;
    // Counted HERE, not at the call site: a rollback whose snapshot is gone
    // replays nothing, and recording it as work done would overstate the very
    // cost the overlay exists to show.
    stats_.on_rollback(from, tick_);
    sim_->state() = snap->second;
    for (std::uint32_t t = from; t < tick_; ++t) {
        snapshots_[t] = sim_->state();
        // THE snapshot trap, closed: the restored State carries whatever
        // `Player::ai` it had when the snapshot was taken — which for a snapshot
        // from before the handoff is `false`. Re-asserting the schedule here, on
        // every replayed tick, makes the flag a function of the tick rather than
        // of when the message happened to arrive, so the re-sim lands on exactly
        // the state the first pass produced.
        apply_handoffs(t);
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
        if (slot == slots_.end()) break;
        // A handed-off seat is NO LONGER awaited, so a dead peer stops holding
        // the confirmed frontier (and with it the hash exchange, the pruning and
        // the prediction cap) hostage. This is what un-stalls the match.
        const std::uint16_t awaited = seats_awaited(confirmed_);
        if ((slot->second.confirmed & awaited) != awaited) break;
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

void RollbackSession::heard(std::uint16_t seats) {
    for (int s = 0; s < sim::kMaxPlayers; ++s)
        if ((seats & static_cast<std::uint16_t>(1U << s)) != 0)
            silence_[static_cast<std::size_t>(s)] = 0;
}

void RollbackSession::schedule_handoff(int seat, std::uint32_t at_tick) {
    const std::size_t si = static_cast<std::size_t>(seat);
    // Order-free and idempotent: the EARLIEST announced tick wins, so a
    // duplicate, a re-send, and an out-of-order copy all reduce to a no-op and
    // every peer converges on the same tick whatever order the copies arrive in.
    if (handoff_[si] != kNoHandoff && at_tick >= handoff_[si]) return;
    handoff_[si] = at_tick;
    dropped_ = static_cast<std::uint16_t>(dropped_ | static_cast<std::uint16_t>(1U << seat));

    if (at_tick >= tick_) return;  // not simulated yet — apply_handoffs will catch it in order
    if (at_tick < confirmed_) {
        // Unrecoverable: those ticks are final on this peer, so it has already
        // agreed a hash for a tick the host re-ran with the AI. Only reachable
        // if we held a LATER input from the dropped seat than the host did (its
        // last packet reached us and not the host); say so loudly rather than
        // drift. The hash exchange would report it a moment later anyway.
        if (!desynced_) {
            desynced_ = true;
            desync_tick_ = at_tick;
        }
        return;
    }
    // Already speculated past the handoff tick: redo those ticks with the seat
    // on AI. Legal because at_tick >= confirmed_, and snapshots below confirmed_
    // are the only ones prune() discards.
    if (!rollback_pending_ || at_tick < rollback_to_) {
        rollback_to_ = at_tick;
        rollback_pending_ = true;
    }
}

void RollbackSession::detect_drops() {
    if (drop_.timeout_ticks <= 0 || aborted_) return;
    for (int s = 0; s < sim::kMaxPlayers; ++s) {
        const std::uint16_t bit = static_cast<std::uint16_t>(1U << s);
        if ((remote_seats_ & bit) == 0) continue;
        const std::size_t si = static_cast<std::size_t>(s);
        if (handoff_[si] != kNoHandoff) continue;  // already handed off; stop counting
        if (++silence_[si] < drop_.timeout_ticks) continue;

        if (!drop_.revert_to_ai) {
            // Options row 12 OFF: there is no legal way to keep simulating a
            // seat nobody will ever provide input for. Every peer detects this
            // for itself (no message needed) and ends the match the same way.
            dropped_ = static_cast<std::uint16_t>(dropped_ | bit);
            aborted_ = true;
            return;
        }
        // A guest never mutates hashed State on its own authority — it waits for
        // the host's Drop. LIMITATION: if the seat that went silent is the
        // HOST's, nobody schedules and the guests stay stalled. Curing that is
        // host migration (ADR-0011 Risks, its own bullet: deterministic
        // re-election of the lowest surviving seat), which is not implemented
        // yet; this increment covers guest drops only.
        if (!drop_.is_host) continue;
        // The handoff tick is the FIRST tick we hold no input for from this seat
        // — see DropFrame on why it is retroactive. remote_next_ starts at the
        // session's own base tick, so a seat that never sent anything at all is
        // correctly handed over from this round's very first tick.
        schedule_handoff(s, remote_next_[si]);
        const std::vector<std::uint8_t> pkt =
            encode_drop(static_cast<std::uint8_t>(s), handoff_[si]);
        transport_->send(pkt.data(), pkt.size());
    }
}

void RollbackSession::broadcast_handoffs() {
    // The same redundancy send_local() uses: UDP loses packets, so re-announce
    // every pump instead of trusting one datagram. A peer that has not received
    // it yet is still stalled at exactly `at_tick` (it cannot confirm past a
    // tick whose input never arrives), so a late copy always lands inside the
    // rollback window — there is no deadline to miss, only a delay to shorten.
    if (!drop_.is_host || dropped_ == 0) return;
    for (int s = 0; s < sim::kMaxPlayers; ++s) {
        const std::uint32_t at = handoff_[static_cast<std::size_t>(s)];
        if (at == kNoHandoff) continue;
        const std::vector<std::uint8_t> pkt = encode_drop(static_cast<std::uint8_t>(s), at);
        transport_->send(pkt.data(), pkt.size());
    }
}

void RollbackSession::request_end_round() {
    if (aborted_ || end_tick_ != kNoEndRound) return;  // already ending: nothing to decide
    if (drop_.is_host) {
        schedule_end_round(tick_ + static_cast<std::uint32_t>(max_prediction_) +
                           kEndRoundSlackTicks);
    } else {
        end_requested_ = true;  // broadcast_end_round() re-asks every pump
    }
    broadcast_end_round();  // don't wait a pump to say so
}

void RollbackSession::schedule_end_round(std::uint32_t at_tick) {
    if (end_tick_ != kNoEndRound && at_tick >= end_tick_) return;  // earliest wins
    end_tick_ = at_tick;
    end_requested_ = false;  // a guest's question has been answered
}

void RollbackSession::broadcast_end_round() {
    // The same redundancy broadcast_handoffs() uses, and for the same reason: a
    // peer that misses this keeps simulating a round the host has already left,
    // and there is no other channel that would ever tell it.
    if (end_tick_ != kNoEndRound) {
        if (!drop_.is_host) return;  // guests echo nothing; the host owns the decision
        const std::vector<std::uint8_t> pkt = encode_match_ctl(MatchCtlKind::EndRound, end_tick_);
        transport_->send(pkt.data(), pkt.size());
        return;
    }
    if (!end_requested_) return;
    const std::vector<std::uint8_t> pkt = encode_match_ctl(MatchCtlKind::EndRoundRequest, 0);
    transport_->send(pkt.data(), pkt.size());
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

void RollbackSession::advance(const sim::TickInputs& local_input, std::int64_t now_ms) {
    // The DIAGNOSTIC BRACKET. `now_ms` goes no further than this line — the
    // simulation below never sees it, so no wall clock can reach a tick
    // (determinism rule 1) and no hashed field can depend on one.
    stats_.begin_pump(now_ms);
    advance_impl(local_input);
    stats_.end_pump(tick_, confirmed_, remote_next_, dropped_, desynced_, desync_tick_, aborted_);
}

void RollbackSession::advance_impl(const sim::TickInputs& local_input) {
    if (aborted_) return;  // a peer was lost with Options row 12 OFF: the match is over, not hung
    receive();
    // Detection sits BEFORE the rollback so a handoff announced or decided this
    // pump is folded into the same re-simulation rather than the next one.
    detect_drops();
    if (aborted_) return;
    if (rollback_pending_) {
        resimulate(rollback_to_);
        rollback_pending_ = false;
    }
    advance_confirmed();
    broadcast_handoffs();
    broadcast_end_round();

    // The round has reached its agreed abandon tick: simulate nothing more, but
    // KEEP PUMPING. A peer that is still short of the end tick needs our input
    // window to get there, and one that has not seen the announcement at all
    // needs the re-send above — so going quiet here would strand exactly the
    // peer this message exists to bring along. The caller stops calling us once
    // its own shell has moved on (round_ended()).
    if (round_ended()) {
        send_local(confirmed_);
        return;
    }

    // Hold at the prediction cap so the display never runs unboundedly ahead of
    // the peer (bounded memory + bounded re-sim on a correction). Keep re-sending
    // local input so the peer catches up and our confirmations advance.
    if (tick_ - confirmed_ >= static_cast<std::uint32_t>(max_prediction_)) {
        // A pump that could not simulate. THIS is what a netcode stutter is —
        // a displayed frame the game was not allowed to advance because a peer's
        // input had not arrived — so it is counted rather than merely happening.
        stats_.on_stall();
        send_local(confirmed_);
        return;
    }

    // Merge the local input INTO the tick's slot (which may already hold remote
    // input that arrived before we reached this tick — never wipe that). A local
    // seat the host handed to the AI is skipped: the sim drives it from here.
    const std::uint16_t local_now = static_cast<std::uint16_t>(local_seats_ & seats_awaited(tick_));
    Slot& slot = slots_[tick_];
    for (int s = 0; s < sim::kMaxPlayers; ++s) {
        const std::uint16_t bit = static_cast<std::uint16_t>(1U << s);
        if ((local_now & bit) != 0)
            slot.inputs.players[static_cast<std::size_t>(s)] =
                local_input.players[static_cast<std::size_t>(s)];
    }
    slot.confirmed = static_cast<std::uint16_t>(slot.confirmed | local_now);

    snapshots_[tick_] = sim_->state();
    apply_handoffs(tick_);  // same call the re-sim makes, so both paths agree
    const sim::TickInputs in = assemble(tick_);
    slots_[tick_].inputs = in;  // store actually-simulated inputs (predicted remote incl.) for compares
    sim_->tick(in);
    ++tick_;
    // The tick we just simulated leaves in the send below, for the first time.
    // That instant is one end of the ack-RTT (net_stats.hpp); the other is the
    // peer's confirmed frontier rising past it.
    stats_.on_local_tick(tick_ - 1);
    send_local(confirmed_);  // now covers [confirmed_, tick_): includes the just-simulated tick
    prune();
}

}  // namespace bomber::net
