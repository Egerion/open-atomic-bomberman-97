#include "bomber/net/rollback_session.hpp"

#include <iterator>
#include <vector>

#include "bomber/net/input_codec.hpp"  // pack_input, for input equality
#include "bomber/net/protocol.hpp"

namespace bomber::net {

namespace {
constexpr std::uint32_t kHashWindow = 256;

// Slack above `host_tick + max_prediction`, which already bounds every peer's
// current tick (a peer cannot be more than the cap past its frontier, and its
// frontier cannot be above the host's head). An end tick a peer had already
// passed would leave it having simulated — and tallied — more of the round.
constexpr std::uint32_t kEndRoundSlackTicks = 12;

// How far BELOW the confirmed frontier snapshots are retained while migration is
// enabled — the depth rewind_for_migration() can un-confirm from. It must exceed
// the worst overshoot between two survivors, which `max_prediction` bounds; 64
// (3.2 s) is several times the largest sane cap.
constexpr std::uint32_t kMigrationRewindWindow = 64;

constexpr std::uint16_t seat_bit(int seat) {
    return static_cast<std::uint16_t>(1U << seat);
}

bool same_input(const sim::PlayerInput& a, const sim::PlayerInput& b) {
    return pack_input(a) == pack_input(b);
}

// Copy just `seats`' entries from `from` into `into`, leaving the rest alone.
void merge_seats(std::uint16_t seats, const sim::TickInputs& from, sim::TickInputs* into) {
    for (int s = 0; s < sim::kMaxPlayers; ++s) {
        if ((seats & seat_bit(s)) == 0) continue;
        const std::size_t si = static_cast<std::size_t>(s);
        into->players[si] = from.players[si];
    }
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
      rollback_to_(start_tick),
      events_through_(start_tick),
      hub_(drop.host_seat),
      oldest_slot_(start_tick),
      local_next_(start_tick) {
    // path() is asked ONCE, at the one moment the session and its transport are
    // certainly the pair that will carry this round.
    stats_.begin(transport.path(), remote_seats_, start_tick, max_prediction);
    handoff_.fill(kNoHandoff);
    // Not 0: a seat that never speaks would otherwise be handed off at tick 0,
    // below this round's confirmed_ and outside its snapshot window.
    remote_next_.fill(start_tick);
}

std::uint16_t RollbackSession::seats_awaited(std::uint32_t tick) const {
    std::uint16_t m = all_seats_;
    if (dropped_ == 0) return m;  // fast path: nobody has dropped
    for (int s = 0; s < sim::kMaxPlayers; ++s) {
        const std::uint32_t at = handoff_[static_cast<std::size_t>(s)];
        if (at != kNoHandoff && tick >= at) m = static_cast<std::uint16_t>(m & ~seat_bit(s));
    }
    return m;
}

std::uint16_t RollbackSession::awaited_remote_seats() const {
    return static_cast<std::uint16_t>(remote_seats_ & seats_awaited(tick_));
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
    // A handed-off seat drops out of `awaited`, so it is fed the SAME neutral
    // input an ordinary AI seat gets. That matters: the AI OR-latches the action
    // keys out of what it is handed, so leaking the dead peer's last predicted
    // input would make the seat behave differently on a peer that predicted
    // differently.
    const std::uint16_t awaited = seats_awaited(tick);
    for (int s = 0; s < sim::kMaxPlayers; ++s) {
        const std::uint16_t bit = seat_bit(s);
        if ((awaited & bit) == 0) continue;  // AI/absent/handed-off: the sim drives it
        const std::size_t si = static_cast<std::size_t>(s);
        in.players[si] = ((slot.confirmed & bit) != 0) ? slot.inputs.players[si]
                                                       : last_remote_input_.players[si];  // predict
    }
    return in;
}

void RollbackSession::request_rollback(std::uint32_t tick) {
    // The EARLIEST tick needing a redo wins, so several corrections in one pump
    // collapse into one re-simulation.
    if (rollback_pending_ && tick >= rollback_to_) return;
    rollback_to_ = tick;
    rollback_pending_ = true;
}

void RollbackSession::count_duplicate_input(std::uint16_t seats) {
    for (int s = 0; s < sim::kMaxPlayers; ++s)
        if ((seats & seat_bit(s)) != 0) stats_.on_dup_input(s);
}

void RollbackSession::apply_remote(std::uint32_t tick, std::uint16_t seats,
                                   const sim::TickInputs& in) {
    if (tick < confirmed_) {
        // A late duplicate is inert, but COUNTED: the redundancy window means
        // most arriving input is this, so the count is the cheapest proof traffic
        // is flowing. NOT a loss signal.
        count_duplicate_input(seats);
        return;
    }
    // Input from a seat already handed to the AI is inert too: the handoff is the
    // agreed truth, and honouring a straggler would diverge from the peers.
    seats = static_cast<std::uint16_t>(seats & seats_awaited(tick));
    if (seats == 0) return;
    Slot& slot = slots_[tick];
    for (int s = 0; s < sim::kMaxPlayers; ++s) {
        const std::uint16_t bit = seat_bit(s);
        if ((seats & bit) == 0) continue;
        const std::size_t si = static_cast<std::size_t>(s);
        if (tick + 1 > remote_next_[si]) remote_next_[si] = tick + 1;  // first tick still missing
        if ((slot.confirmed & bit) != 0) {
            stats_.on_dup_input(s);  // this tick's input from that seat is already held
            continue;
        }
        const sim::PlayerInput& val = in.players[si];
        // Already simulated speculatively with a prediction that turns out wrong.
        if (tick < tick_ && !same_input(slot.inputs.players[si], val)) request_rollback(tick);
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
        if ((seats & seat_bit(s)) != 0) stats_.on_input_from(s, first_tick);
}

void RollbackSession::receive() {
    std::vector<std::uint8_t> pkt;
    while (transport_->poll(&pkt)) {
        Message m;
        const bool decoded = decode(pkt.data(), pkt.size(), &m);
        stats_.on_datagram(decoded);
        if (decoded) on_message(m);  // malformed is dropped (untrusted)
    }
}

void RollbackSession::on_message(const Message& m) {
    // Hello/Punch/Probe/Setup* are pre-match traffic on the shared socket, not
    // ours to read; falling off this switch is how they are ignored. So is
    // every MatchCtlKind other than EndRound — see on_match_ctl().
    switch (m.type) {
        case MsgType::InputRange: on_input_range(m); break;
        case MsgType::Input: on_single_input(m); break;
        case MsgType::Drop: on_drop(m); break;
        case MsgType::HostLost: on_host_lost(m); break;
        case MsgType::Hash: note_peer_hash(m.hash.tick_index, m.hash.hash); break;
        case MsgType::MatchCtl: on_match_ctl(m); break;
        default: break;
    }
}

void RollbackSession::on_input_range(const Message& m) {
    const std::uint16_t remote = static_cast<std::uint16_t>(m.range.seat_mask & remote_seats_);
    if (remote == 0) return;
    heard(remote);
    // The sender's OWN prediction depth, for free: an InputRange spans [its
    // confirmed, its head), so its LENGTH is that distance. This is the remote
    // half of the frame-advantage comparison and the only reason it costs no wire
    // message (docs/net-rollback.md §1.2).
    sync_.note_peer_range(remote, m.range.first_tick, static_cast<int>(m.range.per_tick.size()));
    // `first_tick` is also the ack-RTT's basis (docs/net-rollback.md §4.1). While
    // a migration heals resend_from() widens the window below the frontier, so
    // the RTT reads high for a few ticks — diagnostics only, and an inflated RTT
    // during a migration is the honest thing to show anyway.
    note_input_seats(remote, m.range.first_tick);
    for (std::size_t i = 0; i < m.range.per_tick.size(); ++i)
        apply_remote(m.range.first_tick + static_cast<std::uint32_t>(i), remote,
                     m.range.per_tick[i]);
}

void RollbackSession::on_single_input(const Message& m) {
    const std::uint16_t remote = static_cast<std::uint16_t>(m.input.seat_mask & remote_seats_);
    if (remote == 0) return;
    heard(remote);
    // A single-tick Input carries no frontier (only LockstepSession sends these),
    // so it counts as traffic but yields no RTT sample: passing its tick would
    // read as an acknowledgement it is not.
    note_input_seats(remote, 0);
    apply_remote(m.input.tick_index, remote, m.input.inputs);
}

void RollbackSession::on_drop(const Message& m) {
    // Obeyed by EVERY peer including the host's own echo — idempotent, so the
    // redundant re-sends and any out-of-order copy are all no-ops.
    const int seat = static_cast<int>(m.drop.seat);  // decode() bounds-checked it
    if ((all_seats_ & seat_bit(seat)) == 0) return;
    schedule_handoff(seat, m.drop.at_tick);
}

void RollbackSession::on_host_lost(const Message& m) {
    // Accepted from ANY peer, unlike a Drop: there is no authority left to check
    // it against. What keeps that safe is that it may only name the seat we
    // currently believe IS the hub, so it can never hand an ordinary guest's seat
    // to the AI — the decree MsgType::Drop still reserves to the elected hub.
    // Ignored outright when migration is off or row 12 is off, where a lost peer
    // is a match-ending condition detect_drops() already handles locally.
    const int seat = static_cast<int>(m.host_lost.seat);  // decode() bounds-checked it
    if (!migration_enabled() || !drop_.revert_to_ai) return;
    if (seat != hub_ || (all_seats_ & seat_bit(seat)) == 0) return;
    adopt_host_lost(seat, m.host_lost.at_tick);
}

void RollbackSession::on_match_ctl(const Message& m) {
    // EndRoundRequest is DELIBERATELY IGNORED — it used to let any guest
    // force-end any round — while still being decoded, so a peer on the previous
    // build is turned away rather than disconnected. RematchWait and Rematch
    // belong to the post-match shell (rematch_session.hpp).
    if (m.match_ctl.kind != MatchCtlKind::EndRound) return;
    // hosting(), not drop_.is_host: a promoted peer deferring to the flag would
    // obey an echo of its own decree.
    if (hosting()) return;
    schedule_end_round(m.match_ctl.at_tick);
}

void RollbackSession::resimulate(std::uint32_t from) {
    // Restore the state as it was BEFORE `from`, then replay to the speculative
    // head with corrected inputs (still predicting any seat not yet confirmed).
    const auto snap = snapshots_.find(from);
    if (snap == snapshots_.end()) return;
    // Counted HERE, not at the call site: a rollback whose snapshot is gone
    // replays nothing, and recording it as work done would overstate the cost.
    stats_.on_rollback(from, tick_);
    sim_->state() = snap->second;
    for (std::uint32_t t = from; t < tick_; ++t) {
        snapshots_[t] = sim_->state();
        // THE SNAPSHOT TRAP, closed: a restored State carries whatever
        // `Player::ai` it had when the snapshot was taken, so re-asserting the
        // schedule on every replayed tick is what makes the flag a function of
        // the TICK rather than of when the message happened to arrive.
        apply_handoffs(t);
        const sim::TickInputs in = assemble(t);
        slots_[t].inputs = in;  // remember what we (re)simulated, for later compares
        sim_->tick(in);
    }
}

const std::vector<sim::Event>* RollbackSession::events_of(std::uint32_t tick) const {
    // `tick` is the most recent simulated tick: the live State still holds its
    // events. Otherwise they are in the snapshot taken just before the NEXT tick.
    if (tick + 1 == tick_) return &sim_->state().events;
    const auto after = snapshots_.find(tick + 1);
    return (after != snapshots_.end()) ? &after->second.events : nullptr;
}

void RollbackSession::drain_confirmed_events(std::vector<sim::Event>& out) {
    out.insert(out.end(), confirmed_events_.begin(), confirmed_events_.end());
    confirmed_events_.clear();
}

void RollbackSession::drain_remaining_events(std::vector<sim::Event>& out) {
    drain_confirmed_events(out);
    // The speculative tail, in tick order. Closing the range here is what makes
    // two peers cover the same ticks even though their frontiers differ.
    for (std::uint32_t t = confirmed_; t < tick_; ++t)
        if (const std::vector<sim::Event>* ev = events_of(t))
            out.insert(out.end(), ev->begin(), ev->end());
}

std::uint64_t RollbackSession::hash_after(std::uint32_t tick) const {
    if (tick + 1 >= tick_) return sim_->hash();  // the most recent simulated tick
    const auto after = snapshots_.find(tick + 1);
    return (after != snapshots_.end()) ? sim::state_hash(after->second) : sim_->hash();
}

void RollbackSession::emit_confirmed_events(std::uint32_t tick) {
    // ONCE PER TICK, EVER — load-bearing only because HOST MIGRATION can move
    // `confirmed_` BACKWARDS, and without the guard the re-walk would push those
    // ticks' events into the stream a SECOND time. Events are excluded from
    // state_hash by design, so neither the desync check nor build_hash would say
    // a word (docs/net-rollback.md §2.2).
    if (tick < events_through_) return;
    if (const std::vector<sim::Event>* ev = events_of(tick))
        confirmed_events_.insert(confirmed_events_.end(), ev->begin(), ev->end());
    events_through_ = tick + 1;
}

void RollbackSession::exchange_confirmed_hash(std::uint32_t tick, std::uint64_t h) {
    hash_[tick] = h;
    const std::vector<std::uint8_t> hp = encode_hash(tick, h);
    transport_->send(hp.data(), hp.size());
    const auto peer = peer_hash_.find(tick);
    if (peer == peer_hash_.end()) return;
    if (peer->second != h && !desynced_) {
        desynced_ = true;
        desync_tick_ = tick;
    }
    peer_hash_.erase(peer);
}

void RollbackSession::advance_confirmed() {
    // Raise confirmed_ across the contiguous prefix of all-seats-known ticks. A
    // confirmed tick's inputs never change again, so its result is final: its
    // hash is exchanged here (speculative frames are NOT hashed) and its events
    // go to the confirmed stream from the SAME snapshot at the SAME moment, so an
    // accumulator fed from there is covered by that comparison.
    while (confirmed_ < tick_) {
        const auto slot = slots_.find(confirmed_);
        if (slot == slots_.end()) break;
        // A handed-off seat is NO LONGER awaited, so a dead peer stops holding
        // the frontier — and with it the hash exchange, the pruning and the
        // prediction cap — hostage. This is what un-stalls the match.
        const std::uint16_t awaited = seats_awaited(confirmed_);
        if ((slot->second.confirmed & awaited) != awaited) break;
        emit_confirmed_events(confirmed_);
        exchange_confirmed_hash(confirmed_, hash_after(confirmed_));
        ++confirmed_;
    }
}

void RollbackSession::send_local(std::uint32_t from) {
    // Up to `local_next_`, not `tick_`: the ticks between are already FILED input
    // the peer needs before we reach them, and sending them early is the point of
    // the lead. THE UPPER BOUND IS CHOSEN HERE rather than at the five call
    // sites, so a new call site cannot silently drop the widening.
    //
    // SIZE: the count rides in a u8, and the widest this gets is the rewind
    // window (64) + the prediction cap + kMaxLocalLeadTicks — under 80 ticks of
    // one seat, clear of both 255 entries and the ~1200-byte safe UDP payload.
    if (local_next_ <= from) return;
    std::vector<sim::TickInputs> window;
    window.reserve(local_next_ - from);
    for (std::uint32_t t = from; t < local_next_; ++t) window.push_back(slots_[t].inputs);
    const std::vector<std::uint8_t> pkt = encode_input_range(from, local_seats_, window);
    transport_->send(pkt.data(), pkt.size());
}

void RollbackSession::note_peer_hash(std::uint32_t tick, std::uint64_t peer_hash) {
    const auto ours = hash_.find(tick);
    if (ours == hash_.end()) {
        peer_hash_[tick] = peer_hash;  // compare once we confirm this tick
        return;
    }
    if (ours->second == peer_hash || desynced_) return;
    desynced_ = true;
    desync_tick_ = tick;
}

void RollbackSession::heard(std::uint16_t seats) {
    for (int s = 0; s < sim::kMaxPlayers; ++s)
        if ((seats & seat_bit(s)) != 0) silence_[static_cast<std::size_t>(s)] = 0;
}

void RollbackSession::schedule_handoff(int seat, std::uint32_t at_tick) {
    const std::size_t si = static_cast<std::size_t>(seat);
    // Order-free and idempotent: the EARLIEST announced tick wins, so a
    // duplicate, a re-send and an out-of-order copy all reduce to a no-op, and
    // every peer converges whatever order the copies arrive in.
    if (handoff_[si] != kNoHandoff && at_tick >= handoff_[si]) return;
    handoff_[si] = at_tick;
    dropped_ = static_cast<std::uint16_t>(dropped_ | seat_bit(seat));

    if (at_tick >= tick_) return;  // not simulated yet — apply_handoffs catches it in order
    if (at_tick < confirmed_) {
        // Unrecoverable: we have already agreed a hash for a tick the host re-ran
        // with the AI, which is only reachable if we held a LATER input from the
        // dropped seat than the host did. Say so loudly rather than drift.
        if (!desynced_) {
            desynced_ = true;
            desync_tick_ = at_tick;
        }
        return;
    }
    // Already speculated past the handoff tick: redo those ticks with the seat on
    // AI. Legal because at_tick >= confirmed_, and prune() discards only below it.
    request_rollback(at_tick);
}

int RollbackSession::hub_of(std::uint16_t live) const {
    if (!migration_enabled()) return -1;
    // The seat that STARTED as hub keeps the role while live — it need not be the
    // lowest (whoever pressed Host got it). "Lowest surviving" is the fallback
    // once it is gone, and it chains if the elected hub dies too.
    if (drop_.host_seat < sim::kMaxPlayers && (live & seat_bit(drop_.host_seat)) != 0)
        return drop_.host_seat;
    for (int s = 0; s < sim::kMaxPlayers; ++s)
        if ((live & seat_bit(s)) != 0) return s;
    return -1;  // everyone is gone; the caller is ending the match anyway
}

std::uint16_t RollbackSession::live_seats() const {
    // seats_awaited()'s subtraction with the tick condition dropped: every
    // scheduled handoff counts, whether or not the frontier has reached it.
    std::uint16_t m = all_seats_;
    if (dropped_ == 0) return m;
    for (int s = 0; s < sim::kMaxPlayers; ++s)
        if (handoff_[static_cast<std::size_t>(s)] != kNoHandoff)
            m = static_cast<std::uint16_t>(m & ~seat_bit(s));
    return m;
}

bool RollbackSession::hosting() const {
    if (!migration_enabled()) return drop_.is_host;  // fixed role, pre-migration behaviour
    // Exactly one peer answers true, because exactly one machine owns the elected
    // seat. The role moves the moment the SCHEDULE records the old hub's loss,
    // deliberately not when the frontier crosses it — the frontier gate deadlocks
    // (design §8.2's retraction).
    return hub_ >= 0 && hub_ < sim::kMaxPlayers && (local_seats_ & seat_bit(hub_)) != 0;
}

void RollbackSession::set_migration_hold(bool held) {
    if (migration_hold_ == held) return;
    migration_hold_ = held;
    if (held) return;
    // The counters are already past the timeout when a migration begins, so a
    // release without this is a table-wide cascade one pump later.
    silence_.fill(0);
}

std::uint32_t RollbackSession::resend_from() const {
    // Normally confirmed_ is enough, on an invariant a migration is exactly where
    // it fails: a peer missing an older input is STALLED at it, so nobody can
    // have finalised past it. A severed star breaks that — both survivors'
    // knowledge froze at DIFFERENT points, so the peer that is behind needs
    // inputs the peer that is ahead has finalised and stopped re-sending.
    // Measured on a 3-seat star that wedged eight ticks apart; defensive rather
    // than covered here (docs/net-rollback.md §3).
    //
    // A mid-match one-way outage (path_failover.hpp) is the 2-seat instance of
    // the SAME asymmetry: the hearing side finalises the deaf side's last ~cap
    // ticks and walks its window past the hole. Measured before
    // widen_resend_window existed: the healed link reconnected two live peers
    // whose frontiers then never moved again.
    if (!migration_healing() && !resend_widened()) return confirmed_;
    return oldest_slot_ < confirmed_ ? oldest_slot_ : confirmed_;
}

void RollbackSession::widen_resend_window() {
    // Raised, never lowered, exactly as heal_until_ is — asking again while a
    // widening is still open can only extend it.
    const std::uint32_t until = confirmed_ + kMigrationRewindWindow;
    if (until > widen_until_) widen_until_ = until;
}

bool RollbackSession::rewind_for_migration(std::uint32_t at_tick) {
    // EVERY hash at or above `at_tick` was computed over a history that included
    // the dead hub's input. Purging BOTH sides is what keeps the convergence
    // quiet: the peer holding the LOWER tick never rolls back at all, so without
    // this it compares its correct post-handoff hash against a stale pre-handoff
    // one and reports a divergence that never happened.
    for (auto it = hash_.begin(); it != hash_.end();)
        it = (it->first >= at_tick) ? hash_.erase(it) : std::next(it);
    for (auto it = peer_hash_.begin(); it != peer_hash_.end();)
        it = (it->first >= at_tick) ? peer_hash_.erase(it) : std::next(it);

    if (at_tick >= confirmed_) return true;  // nothing final above it: an ordinary rollback
    // We finalised ticks the announcer never could, so the frontier itself moves
    // back. Legal ONLY for a host loss: with a live hub there is a single
    // scheduler whose tick is a lower bound by construction, so the same
    // situation there is a genuine disagreement and stays loud.
    if (snapshots_.find(at_tick) == snapshots_.end()) return false;  // older than the window
    confirmed_ = at_tick;
    return true;
}

void RollbackSession::adopt_host_lost(int seat, std::uint32_t at_tick) {
    const std::size_t si = static_cast<std::size_t>(seat);
    // Recorded even when the tick is no news: this peer is now one of the
    // announcers, and the redundant re-send is the only thing keeping the
    // message alive with the hub gone.
    host_lost_ = static_cast<std::uint16_t>(host_lost_ | seat_bit(seat));
    // The WIDE re-send stays on until the frontier is a whole rewind window clear
    // of the migration tick. Raised, never lowered.
    const std::uint32_t heal = at_tick + kMigrationRewindWindow;
    if (heal > heal_until_) heal_until_ = heal;
    if (handoff_[si] != kNoHandoff && at_tick >= handoff_[si]) return;  // ours is already lower
    // LOWERING the tick is what makes concurrent announcements converge, and it
    // is safe because apply_handoffs() re-asserts the schedule at the head of
    // every simulation of a tick. A false return here needs no branch —
    // schedule_handoff then says so loudly.
    rewind_for_migration(at_tick);
    schedule_handoff(seat, at_tick);
}

void RollbackSession::broadcast_host_lost() {
    // NOT hub-only, unlike broadcast_handoffs(): the announcement is about the
    // hub, so the one machine the redundancy discipline usually leans on is the
    // machine that is gone. Every peer holding the record re-sends it.
    if (host_lost_ == 0) return;
    for (int s = 0; s < sim::kMaxPlayers; ++s) {
        if ((host_lost_ & seat_bit(s)) == 0) continue;
        const std::vector<std::uint8_t> pkt =
            encode_host_lost(static_cast<std::uint8_t>(s), handoff_[static_cast<std::size_t>(s)]);
        transport_->send(pkt.data(), pkt.size());
    }
}

bool RollbackSession::silence_exceeded(int seat) {
    const std::uint16_t bit = seat_bit(seat);
    if ((remote_seats_ & bit) == 0) return false;
    const std::size_t si = static_cast<std::size_t>(seat);
    if (handoff_[si] != kNoHandoff) return false;  // already handed off; stop counting
    return ++silence_[si] >= drop_.timeout_ticks;
}

void RollbackSession::abort_on_drop(int seat) {
    // Options row 12 OFF: there is no legal way to keep simulating a seat nobody
    // will ever provide input for. Every peer detects this for itself (no message
    // needed) and ends the match the same way.
    dropped_ = static_cast<std::uint16_t>(dropped_ | seat_bit(seat));
    aborted_ = true;
}

void RollbackSession::announce_host_lost(int seat) {
    // THE HUB'S OWN SEAT went silent — the case that used to hang, because the
    // scheduler was the machine that died. EVERY survivor announces it, under the
    // separate MsgType::HostLost tag so that accepting it from a non-hub does not
    // also make an ordinary guest-drop decree acceptable from one (design §8.1).
    const std::size_t si = static_cast<std::size_t>(seat);
    adopt_host_lost(seat, remote_next_[si]);
    const std::vector<std::uint8_t> pkt =
        encode_host_lost(static_cast<std::uint8_t>(seat), handoff_[si]);
    transport_->send(pkt.data(), pkt.size());
}

void RollbackSession::announce_drop(int seat) {
    // The handoff tick is the FIRST tick we hold no input for (DropFrame explains
    // why that is retroactive), and remote_next_ starts at the session's own base
    // so a seat that never spoke is handed over from this round's first tick.
    const std::size_t si = static_cast<std::size_t>(seat);
    schedule_handoff(seat, remote_next_[si]);
    const std::vector<std::uint8_t> pkt =
        encode_drop(static_cast<std::uint8_t>(seat), handoff_[si]);
    transport_->send(pkt.data(), pkt.size());
}

void RollbackSession::detect_drops() {
    if (drop_.timeout_ticks <= 0 || aborted_) return;
    for (int s = 0; s < sim::kMaxPlayers; ++s) {
        if (!silence_exceeded(s)) continue;
        if (!drop_.revert_to_ai) {
            abort_on_drop(s);
            return;
        }
        // NOTHING NEW IS DECLARED WHILE A MIGRATION IS HEALING — a correctness
        // guard, not a nicety. A severed star goes silent EVERYWHERE at once, not
        // just at the corpse, and ungated that silence chains the election until
        // every peer has elected ITSELF. Measured before the guard (design §8.2).
        if (migration_healing()) continue;
        if (migration_enabled() && s == hub_) {
            announce_host_lost(s);
            continue;
        }
        // An ordinary guest drop stays a decree, and the decree is the ELECTED
        // hub's — which after a migration may well be this machine.
        if (hosting()) announce_drop(s);
    }
}

void RollbackSession::broadcast_handoffs() {
    // send_local()'s redundancy: UDP loses packets, so re-announce every pump. A
    // peer that has not received it is still stalled at exactly `at_tick`, so a
    // late copy always lands inside the rollback window — no deadline to miss,
    // only a delay to shorten.
    if (!hosting() || dropped_ == 0) return;
    for (int s = 0; s < sim::kMaxPlayers; ++s) {
        const std::uint32_t at = handoff_[static_cast<std::size_t>(s)];
        if (at == kNoHandoff) continue;
        // A host loss goes out through broadcast_host_lost() under its own tag.
        // Re-sending it here as a Drop would be the elected hub retroactively
        // decreeing its predecessor's death — the authority the separate tag
        // exists to keep apart.
        if ((host_lost_ & seat_bit(s)) != 0) continue;
        const std::vector<std::uint8_t> pkt = encode_drop(static_cast<std::uint8_t>(s), at);
        transport_->send(pkt.data(), pkt.size());
    }
}

void RollbackSession::request_end_round() {
    // A guest asking is not "a request that may be granted", it is nothing at
    // all, so the authority lives here instead of in the caller's discipline.
    // hosting(), not drop_.is_host (design §10).
    if (!hosting()) return;
    if (aborted_ || end_tick_ != kNoEndRound) return;  // already ending: nothing to decide
    schedule_end_round(tick_ + static_cast<std::uint32_t>(max_prediction_) + kEndRoundSlackTicks);
    broadcast_end_round();  // don't wait a pump to say so
}

void RollbackSession::schedule_end_round(std::uint32_t at_tick) {
    if (end_tick_ != kNoEndRound && at_tick >= end_tick_) return;  // earliest wins
    end_tick_ = at_tick;
}

void RollbackSession::broadcast_end_round() {
    // A peer that misses this keeps simulating a round the host has already left,
    // and no other channel would tell it. Reads the hub_ computed by the PREVIOUS
    // pump, as broadcast_handoffs() and detect_drops() do — 50 ms of staleness in
    // a decision that is idempotent and re-sent every pump.
    if (!hosting() || end_tick_ == kNoEndRound) return;
    const std::vector<std::uint8_t> pkt = encode_match_ctl(MatchCtlKind::EndRound, end_tick_);
    transport_->send(pkt.data(), pkt.size());
}

void RollbackSession::prune() {
    // Everything strictly below confirmed_ is final and never needed again —
    // with two exceptions that both keep a rewind window: host migration, where
    // rewind_for_migration() may move the frontier BACKWARDS onto a tick whose
    // snapshot would otherwise be gone, and the mid-match path failover, where
    // resend_from() must re-send ticks the peer's one-way outage swallowed
    // (widen_resend_window). The second cannot be gated on anything known here —
    // by the time the switch happens the hole is already below confirmed_ and
    // pruned, so the window is retained UNCONDITIONALLY: ~64 ticks of slots and
    // snapshots, the price migration-enabled sessions always paid.
    const std::uint32_t keep_from =
        (confirmed_ > kMigrationRewindWindow) ? confirmed_ - kMigrationRewindWindow : 0;
    for (auto it = slots_.begin(); it != slots_.end();)
        it = (it->first < keep_from) ? slots_.erase(it) : std::next(it);
    for (auto it = snapshots_.begin(); it != snapshots_.end();)
        it = (it->first < keep_from) ? snapshots_.erase(it) : std::next(it);
    // The floor only ever RISES: confirmed_ can move back, but ticks below it are
    // gone for good once erased, so resend_from() must never name one.
    if (keep_from > oldest_slot_) oldest_slot_ = keep_from;
    const std::uint32_t keep = (confirmed_ > kHashWindow) ? confirmed_ - kHashWindow : 0;
    for (auto it = hash_.begin(); it != hash_.end();)
        it = (it->first < keep) ? hash_.erase(it) : std::next(it);
    for (auto it = peer_hash_.begin(); it != peer_hash_.end();)
        it = (it->first < keep) ? peer_hash_.erase(it) : std::next(it);
}

bool RollbackSession::rephase_eligible() const {
    // Nothing to compare against until a peer has spoken; and a session whose
    // partner is gone, or whose round is ending, has no clock to align with.
    if (!sync_.peer_heard() || aborted_ || end_tick_ != kNoEndRound) return false;
    if (awaited_remote_seats() == 0) return false;
    // NOT WHILE A MIGRATION IS HEALING: both numbers are meaningless there — our
    // half is enormous because the frontier was pinned for the whole outage, and
    // peer_lag() is read from lengths received BEFORE the hub died — so the
    // difference would clear the threshold every pump and throttle precisely the
    // peer that most needs to catch up. Written as ELIGIBILITY rather than inside
    // the comparison so the variance absorber inherits it too.
    return !migration_healing();
}

void RollbackSession::file_local(const sim::TickInputs& local_input) {
    // Fill forward to the lead's head. `local_next_` NEVER moves backwards and an
    // already-filed tick is never re-decided — that is the one rule that lets the
    // lead change mid-match, because the peer may already have confirmed and
    // hashed a tick we filed (docs/net-rollback.md §1.4).
    //
    // MERGED into the slot rather than assigned over it: the slot may already
    // hold remote input that arrived before we reached this tick.
    const std::uint32_t head = tick_ + static_cast<std::uint32_t>(sync_.lead()) + 1;
    while (local_next_ < head) {
        const std::uint16_t bits =
            static_cast<std::uint16_t>(local_seats_ & seats_awaited(local_next_));
        Slot& slot = slots_[local_next_];
        merge_seats(bits, local_input, &slot.inputs);
        slot.confirmed = static_cast<std::uint16_t>(slot.confirmed | bits);
        // The instant our input for that tick first leaves, and one end of the
        // ack-RTT. Taken at FILING rather than at simulation, because with a lead
        // those are different pumps and the later one would under-report.
        stats_.on_local_tick(local_next_);
        ++local_next_;
    }
}

bool RollbackSession::simulation_allowed(bool eligible) {
    // FIRST among these on purpose: the migration stall is the strongest ("the
    // frontier must not move") and the only one that must re-send from the WIDE
    // window rather than from confirmed_.
    if (migration_hold_) return false;
    // The round has reached its agreed abandon tick: simulate nothing more, but
    // KEEP PUMPING. A peer short of the end tick needs our window to get there,
    // and one that has not seen the announcement needs the re-send.
    if (round_ended()) return false;

    // RE-PHASE. ABOVE the cap check deliberately: spend the skew while there is
    // still prediction budget, instead of arriving at the cap and living there
    // for the rest of the round. Counted separately from a cap stall, because
    // conflating them would hide the cure inside the symptom.
    //
    // LIKE FOR LIKE: `local_next_ - confirmed_` is the window we put on the wire
    // against the window the peer put on theirs, so when both carry a lead the
    // leads cancel.
    const TimeSyncController::Decision timing =
        sync_.pump(eligible, static_cast<int>(local_next_ - confirmed_), awaited_remote_seats());
    stats_.on_timing(
        {timing.raw, timing.sustained, timing.spread, sync_.lead(), timing.suppressed});
    if (timing.hold) {
        stats_.on_rephase_hold();
        return false;
    }

    // Hold at the prediction cap so the display never runs unboundedly ahead of
    // the peer (bounded memory, bounded re-sim on a correction).
    if (tick_ - confirmed_ < static_cast<std::uint32_t>(max_prediction_)) return true;
    // A pump that could not simulate. THIS is what a netcode stutter is — a
    // displayed frame the game was not allowed to advance because a peer's input
    // had not arrived — so it is counted rather than merely happening.
    stats_.on_stall();
    return false;
}

void RollbackSession::simulate_one_tick(const sim::TickInputs& local_input) {
    file_local(local_input);
    snapshots_[tick_] = sim_->state();
    apply_handoffs(tick_);  // same call the re-sim makes, so both paths agree
    const sim::TickInputs in = assemble(tick_);
    slots_[tick_].inputs = in;  // store what we actually simulated, for later compares
    sim_->tick(in);
    ++tick_;
}

void RollbackSession::advance(const sim::TickInputs& local_input, std::int64_t now_ms) {
    // THE DIAGNOSTIC BRACKET. `now_ms` goes no further than this line — the
    // simulation below never sees it, so no wall clock can reach a tick
    // (determinism rule 1) and no hashed field can depend on one.
    stats_.begin_pump(now_ms);
    advance_impl(local_input);
    stats_.end_pump({tick_, confirmed_, remote_next_, dropped_, desynced_, desync_tick_, aborted_});
}

void RollbackSession::advance_impl(const sim::TickInputs& local_input) {
    if (aborted_) return;  // a peer was lost with Options row 12 OFF: over, not hung
    receive();
    // BEFORE the rollback, so a handoff decided this pump folds into the same
    // re-simulation. NOT while a migration is held: every remote seat is already
    // past the drop timeout by then — that silence is HOW the hub's loss was
    // detected — so counting on would declare the whole surviving table dropped.
    if (!migration_hold_) detect_drops();
    if (aborted_) return;

    if (rollback_pending_) {
        resimulate(rollback_to_);
        rollback_pending_ = false;
    }
    advance_confirmed();
    broadcast_handoffs();
    broadcast_end_round();
    // THE ELECTION, re-evaluated every pump (design §8.2). A pure function of the
    // drop schedule, so every peer computes the same answer with nothing
    // exchanged, and it re-derives correctly after any rollback.
    hub_ = hub_of(live_seats());
    broadcast_host_lost();

    const bool eligible = rephase_eligible();
    if (!simulation_allowed(eligible)) {
        send_local(resend_from());  // the peer still needs our window to catch up
        return;
    }
    // A pump that does not simulate must not consume a lead sample either, or the
    // lead would grow by one for every stalled pump — hence after the checks.
    sync_.update_lead(eligible);
    simulate_one_tick(local_input);
    // Covers [resend_from(), local_next_) — BOTH widenings at once, independent
    // by construction: resend_from() decides where the window STARTS, the lead
    // where it ENDS, and the end is chosen inside send_local().
    send_local(resend_from());
    prune();
}

}  // namespace bomber::net
