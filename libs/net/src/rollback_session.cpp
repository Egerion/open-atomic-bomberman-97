#include "bomber/net/rollback_session.hpp"

#include <algorithm>
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

// How much CLOCK SKEW (in ticks) this peer tolerates before it starts giving it
// back. Our lag minus the peer's own lag is twice the skew — the path delay is in
// both and cancels — so this is a threshold on 2x the skew, i.e. it engages once
// this peer is a full tick (50 ms) ahead of its partner. Small on purpose: the
// whole failure being cured is skew ACCUMULATING unnoticed until the prediction
// cap is the only thing left holding it, so it must be shed while there is still
// budget to spare. Too small and ordinary arrival noise would trip it: at 2 the
// margin is a factor of two over the +/-1 tick a pump-boundary can contribute.
constexpr int kRephaseAdvantageTicks = 2;

// How far BELOW the confirmed frontier snapshots are retained while host
// migration is enabled — the depth from which rewind_for_migration() can still
// un-confirm. It has to exceed the worst overshoot between two survivors, and
// that overshoot is bounded: a peer stops producing input `max_prediction` ticks
// past its own confirmed frontier, so a peer holding MORE of the dying hub's
// input than its neighbour can only confirm `max_prediction` ticks past the tick
// the neighbour will propose. 64 (3.2 s at 20 Hz) is several times the largest
// sane cap, and costs 64 retained State copies only for a migration-enabled
// session — every other caller keeps prune()'s old behaviour exactly.
constexpr std::uint32_t kMigrationRewindWindow = 64;

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
      rollback_to_(start_tick),
      events_through_(start_tick),
      hub_(drop.host_seat),
      oldest_slot_(start_tick),
      local_next_(start_tick) {
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
            // The sender's OWN prediction depth, for free: an InputRange spans
            // [its confirmed, its head), so its length IS that distance. This is
            // the remote half of the frame-advantage comparison (see the
            // re-phasing note in the header) and the only reason it costs no wire
            // message.
            //
            // ONLY FROM A FRAME THAT IS NOT STALE. A sender's confirmed frontier
            // is monotonic, so a datagram whose `first_tick` sits below one we
            // have already seen from that seat overtook a newer one in flight —
            // and adopting its window would report a depth the peer left behind
            // some time ago. See peer_frontier_ in the header.
            for (int s = 0; s < sim::kMaxPlayers; ++s) {
                if ((remote & static_cast<std::uint16_t>(1U << s)) == 0) continue;
                const std::size_t si = static_cast<std::size_t>(s);
                if (peer_frontier_any_[si] && m.range.first_tick < peer_frontier_[si]) continue;
                peer_frontier_[si] = m.range.first_tick;
                peer_frontier_any_[si] = true;
                peer_depth_[si] = static_cast<int>(m.range.per_tick.size());
            }
            peer_heard_ = true;
            // THE ACK-RTT SAMPLE, and the reason this diagnostic needs no new
            // wire message: an InputRange normally begins at the SENDER'S
            // confirmed frontier (send_local(resend_from())), and that frontier
            // cannot pass a tick our input has not reached. So `first_tick` is an
            // acknowledgement of our own tick first_tick-1.
            //
            // ONE EXCEPTION, and it is deliberately left uncorrected: while a
            // HOST MIGRATION heals, resend_from() widens the window below the
            // sender's frontier (see there), so first_tick UNDERSTATES what the
            // sender has acknowledged and the RTT it yields reads high for those
            // few ticks. Diagnostics only — nothing here feeds a correctness
            // decision — and a migration is exactly when an inflated RTT is the
            // honest thing to show anyway, so it is not worth a wire field to fix.
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
        } else if (m.type == MsgType::HostLost) {
            // The hub is gone, announced by a survivor (design §8.1). Ignored
            // entirely when migration is off, so a session that never opted in
            // behaves exactly as it did before wire v9 — and ignored unless
            // Options row 12 is ON, because without it a lost peer is a
            // match-ending condition that detect_drops() already handles
            // locally on every peer, with no seat to hand to anyone.
            //
            // Accepted from ANY peer, unlike a Drop: there is no authority left
            // to check it against. What keeps that safe is that it may only name
            // the seat we currently believe is the hub — so it can never be used
            // to hand an ordinary guest's seat to the AI, which is the decree
            // MsgType::Drop still reserves to the elected hub.
            const int seat = static_cast<int>(m.host_lost.seat);  // decode() bounds-checked it
            if (migration_enabled() && drop_.revert_to_ai && seat == hub_ &&
                (all_seats_ & static_cast<std::uint16_t>(1U << seat)) != 0)
                adopt_host_lost(seat, m.host_lost.at_tick);
        } else if (m.type == MsgType::Hash) {
            note_peer_hash(m.hash.tick_index, m.hash.hash);
        } else if (m.type == MsgType::MatchCtl) {
            if (m.match_ctl.kind == MatchCtlKind::EndRound) {
                // Obeyed by EVERY peer including the host's own echo over a star
                // — idempotent, so re-sends and reordering are all no-ops. Only a
                // host ever sends one; a guest that somehow does is not obeyed,
                // because a guest's EndRound is not addressed to anyone (its own
                // Esc never reaches this class at all).
                // hosting(), not drop_.is_host: after a migration the machine
                // that must ignore an inbound EndRound is the ELECTED hub, which
                // may be this one. A promoted peer that still deferred to the
                // flag would obey an echo of its own announcement.
                if (!hosting()) schedule_end_round(m.match_ctl.at_tick);
            }
            // EndRoundRequest is DELIBERATELY IGNORED — see the authority note at
            // the top of rollback_session.hpp. It used to let any guest force-end
            // any round; the message is still decoded (wire v8 is unchanged, so a
            // peer on the previous build still connects) and simply does nothing.
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

const std::vector<sim::Event>* RollbackSession::events_of(std::uint32_t tick) const {
    // `tick` is the most recent simulated tick: the live State still holds its
    // events (nothing has ticked over them).
    if (tick + 1 == tick_) return &sim_->state().events;
    // Otherwise they are in the snapshot taken just before the NEXT tick ran.
    // Always present while `tick` is inside the un-pruned window, which is the
    // only range advance_confirmed() ever asks about.
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
    // two peers cover the same ticks even though their confirmation frontiers
    // sit at different places (see the header's note).
    for (std::uint32_t t = confirmed_; t < tick_; ++t)
        if (const std::vector<sim::Event>* ev = events_of(t))
            out.insert(out.end(), ev->begin(), ev->end());
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
        // This tick is FINAL, so its events are too: hand them to the confirmed
        // stream (header note, "THE CONFIRMED EVENT STREAM"). Deliberately the
        // same tick, the same snapshot and the same moment the hash above is
        // taken from — an accumulator fed from here is covered by the very
        // comparison that hash is about to be exchanged for.
        //
        // ONCE PER TICK, EVER — that is what `events_through_` is for, and it is
        // load-bearing only because HOST MIGRATION can move `confirmed_`
        // BACKWARDS (rewind_for_migration, the one place that happens). Without
        // the guard the re-walk from the adopted tick would push those ticks'
        // events into the stream a SECOND time, and an accumulator fed from it
        // would double-count every kill in that window — precisely the bug the
        // confirmed stream was introduced to fix, reintroduced through the one
        // door its author could not have known about. Events are excluded from
        // state_hash by design, so neither the desync check nor build_hash would
        // have said a word.
        if (confirmed_ >= events_through_) {
            if (const std::vector<sim::Event>* ev = events_of(confirmed_))
                confirmed_events_.insert(confirmed_events_.end(), ev->begin(), ev->end());
            events_through_ = confirmed_ + 1;
        }
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
    // Up to `local_next_`, not `tick_`: the ticks between the two are already
    // FILED local input that the peer will need before we ourselves reach them,
    // and sending them early is the whole point of the lead (see the header's
    // jitter note). At a lead of 0 the two are equal and this is the range it
    // always was.
    //
    // THE UPPER BOUND IS CHOSEN HERE, not at the call sites, and deliberately so:
    // `from` is already the caller's business (resend_from() widens it downward
    // while a migration heals) and there are five call sites. A lead that had to
    // be applied by hand at each of them is exactly the shape of bug the host
    // migration branch was re-implemented to avoid — a new call site written in
    // the narrow form merges cleanly and silently drops the widening.
    //
    // SIZE: the count rides in a u8. The widest this can be is the migration
    // rewind window (64) plus the prediction cap plus kMaxLocalLeadTicks — under
    // 80 ticks of one local seat, so a datagram well clear of both 255 entries
    // and the ~1200-byte safe UDP payload.
    if (local_next_ <= from) return;
    std::vector<sim::TickInputs> window;
    window.reserve(local_next_ - from);
    for (std::uint32_t t = from; t < local_next_; ++t) window.push_back(slots_[t].inputs);
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

int RollbackSession::hub_of(std::uint16_t live) const {
    if (!migration_enabled()) return -1;
    // The seat that STARTED as the hub keeps the role while it is live — it need
    // not be the lowest one (whoever pressed "host" got it). "Lowest surviving"
    // is the tie-break the survivors fall back on once it is gone, and it chains:
    // if the elected hub dies too, the next-lowest takes over by the same rule.
    if (drop_.host_seat < sim::kMaxPlayers &&
        (live & static_cast<std::uint16_t>(1U << drop_.host_seat)) != 0)
        return drop_.host_seat;
    for (int s = 0; s < sim::kMaxPlayers; ++s)
        if ((live & static_cast<std::uint16_t>(1U << s)) != 0) return s;
    return -1;  // everyone is gone; the caller is ending the match anyway
}

std::uint16_t RollbackSession::live_seats() const {
    // The same subtraction seats_awaited() makes, with the tick condition
    // dropped: every scheduled handoff counts, whether or not the frontier has
    // reached it. AI and empty slots were never in all_seats_, and a seat is
    // never removed for anything that lives in the hashed State (a DEAD player
    // still runs a machine and can still be the hub), so the answer depends on
    // the schedule alone — identical on every peer, and unchanged by a rollback.
    std::uint16_t m = all_seats_;
    if (dropped_ == 0) return m;
    for (int s = 0; s < sim::kMaxPlayers; ++s)
        if (handoff_[static_cast<std::size_t>(s)] != kNoHandoff)
            m = static_cast<std::uint16_t>(m & ~static_cast<std::uint16_t>(1U << s));
    return m;
}

bool RollbackSession::hosting() const {
    if (!migration_enabled()) return drop_.is_host;  // fixed role, pre-migration behaviour
    // Exactly one peer answers true, because exactly one machine owns the
    // elected seat. The role moves the moment the schedule records the old hub's
    // loss — deliberately not when the frontier crosses it; see the header's
    // retraction note on why the frontier gate deadlocks.
    return hub_ >= 0 && hub_ < sim::kMaxPlayers &&
           (local_seats_ & static_cast<std::uint16_t>(1U << hub_)) != 0;
}

void RollbackSession::set_migration_hold(bool held) {
    if (migration_hold_ == held) return;
    migration_hold_ = held;
    if (held) return;
    // See the header: the counters are already past the timeout when a migration
    // begins, so a release without this is a table-wide cascade one pump later.
    silence_.fill(0);
}

std::uint32_t RollbackSession::resend_from() const {
    // Normally confirmed_ is enough, and the invariant behind that is worth
    // stating because a migration is exactly where it fails: a peer missing an
    // older input is STALLED at it, so nobody can have finalised past it, so
    // nobody needs it re-sent.
    //
    // A severed star breaks that. Both survivors' knowledge of each other froze
    // at the instant the reflector died — at DIFFERENT points, because they lost
    // different amounts of its final output — so their frontiers sit several
    // ticks apart, and the peer that is behind needs inputs the peer that is
    // ahead has already finalised and stopped re-sending. Measured: a 3-seat
    // star test wedged eight ticks apart with the migration otherwise perfectly
    // converged.
    //
    // So while a migration heals, widen the window down to the oldest tick still
    // retained. One local seat's input is a byte per tick, so even 64 ticks is a
    // small datagram, and it lasts only until the frontier is clear of the
    // migration.
    //
    // HONEST COVERAGE NOTE: this widening is CARRIED FROM THE EARLIER BRANCH'S
    // MEASUREMENT, not proven by tests/net/test_host_migration.cpp — reverting it
    // leaves that suite green. The suite's divergence case does not reach the
    // wedge because the peer that is ahead gets REWOUND to the adopted tick, and
    // re-sending from a rewound frontier already covers what the peer behind
    // needs. The documented wedge needs a survivor to be behind on a tick BELOW
    // the migration tick, which this suite does not construct. Kept because the
    // failure it describes was observed on the earlier branch and the cost is a
    // few dozen bytes per datagram for a second or two; treat it as defensive,
    // and if it ever needs justifying, build that case first.
    if (!migration_healing()) return confirmed_;
    return oldest_slot_ < confirmed_ ? oldest_slot_ : confirmed_;
}

bool RollbackSession::rewind_for_migration(std::uint32_t at_tick) {
    // EVERY hash at or above `at_tick` — ours and every peer's — was computed
    // before this announcement existed, i.e. over a history that included the
    // dead hub's input, and describes ticks that are about to be re-simulated
    // with the seat on AI. They are not evidence of anything any more.
    //
    // Purging BOTH sides is what keeps the convergence quiet. The peer holding
    // the LOWER tick never rolls back at all — it simply starts confirming past
    // its own frontier — but the peer holding the higher one had already
    // confirmed and BROADCAST hashes for those ticks, so without this the lower
    // peer compares its correct post-handoff hash against a stale pre-handoff
    // one and reports a divergence that never happened. The cost is a few ticks
    // of lost desync coverage around the migration; both sides re-exchange from
    // there on.
    for (auto it = hash_.begin(); it != hash_.end();)
        it = (it->first >= at_tick) ? hash_.erase(it) : std::next(it);
    for (auto it = peer_hash_.begin(); it != peer_hash_.end();)
        it = (it->first >= at_tick) ? peer_hash_.erase(it) : std::next(it);

    if (at_tick >= confirmed_) return true;  // nothing final sits above it: an ordinary rollback
    // We finalised ticks the announcer never could, because we happened to
    // receive more of the dying hub's datagrams than it did. Those ticks were
    // simulated with the hub's real input and must be redone with the AI, so the
    // frontier itself has to move back — the one place this class un-confirms.
    // Legal only for a HOST loss: with a live hub there is a single scheduler and
    // its tick is a lower bound by construction (it hears every guest first), so
    // the same situation there means a genuine disagreement and stays loud.
    if (snapshots_.find(at_tick) == snapshots_.end())
        return false;  // older than the retained window
    confirmed_ = at_tick;
    return true;
}

void RollbackSession::adopt_host_lost(int seat, std::uint32_t at_tick) {
    const std::size_t si = static_cast<std::size_t>(seat);
    // Remember it as a host loss even when the tick is no news: this peer is now
    // one of the announcers, and the redundant re-send is the only thing keeping
    // the message alive with the hub gone.
    host_lost_ = static_cast<std::uint16_t>(host_lost_ | static_cast<std::uint16_t>(1U << seat));
    // How long the WIDE redundant re-send stays on (resend_from): until the
    // frontier is a whole rewind window clear of the migration tick, i.e. until
    // no survivor can still be waiting on an input either side has finalised.
    // Raised, never lowered, so a second host loss extends it.
    const std::uint32_t heal = at_tick + kMigrationRewindWindow;
    if (heal > heal_until_) heal_until_ = heal;
    if (handoff_[si] != kNoHandoff && at_tick >= handoff_[si]) return;  // ours is already lower
    // Lowering the tick is what makes concurrent announcements converge: it
    // re-schedules the handoff, and because apply_handoffs() re-asserts the
    // schedule at the head of every simulation of a tick, the re-simulation
    // schedule_handoff triggers lands on the same state everywhere.
    // false = out of the retained window; schedule_handoff then says so loudly.
    rewind_for_migration(at_tick);
    schedule_handoff(seat, at_tick);
}

void RollbackSession::broadcast_host_lost() {
    // Unlike broadcast_handoffs() this is NOT hub-only. The announcement is about
    // the hub, so the one machine the redundancy discipline usually leans on is
    // the machine that is gone; every peer holding the record re-sends it every
    // pump instead, which also carries it to a survivor that heard nothing.
    if (host_lost_ == 0) return;
    for (int s = 0; s < sim::kMaxPlayers; ++s) {
        if ((host_lost_ & static_cast<std::uint16_t>(1U << s)) == 0) continue;
        const std::vector<std::uint8_t> pkt =
            encode_host_lost(static_cast<std::uint8_t>(s), handoff_[static_cast<std::size_t>(s)]);
        transport_->send(pkt.data(), pkt.size());
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
        // NOTHING NEW IS DECLARED WHILE A MIGRATION IS STILL HEALING, and this
        // is a correctness guard rather than a nicety. When the hub dies the
        // star is severed, so EVERY survivor goes silent to EVERY other survivor
        // at the same instant — not just the corpse. Left ungated that silence
        // is read as evidence twice over, and both readings are wrong:
        //
        //   * the seat just elected to replace the hub is itself declared lost
        //     one timeout later, and the next, and the next — the election
        //     chains until every peer has elected ITSELF and the table has split
        //     into as many one-player games as there are survivors; and
        //   * the moment a survivor does take the role, it decrees an ordinary
        //     Drop on the OTHER survivor, which is equally silent and equally
        //     alive.
        //
        // Both were measured in this suite's 3-seat star before the guard: the
        // first left two survivors on different hubs, the second left the
        // elected hub having AI'd its only remaining peer, and both ended in a
        // desync. More silence during a known outage is not new information.
        // The chain rule stays intact for the case it is actually for — a
        // successor that dies once the star is working again, which this suite
        // also covers.
        if (migration_healing()) continue;
        // THE HUB'S OWN SEAT went silent. This is the case that used to hang:
        // a guest never mutates hashed State on its own authority, so with the
        // host gone nobody scheduled and every guest stalled forever. Now EVERY
        // survivor announces it (design §8.1) — there is no authority to defer
        // to, because the machine that had it is the one that died — under the
        // separate MsgType::HostLost tag, so accepting it from a non-hub does
        // not also make an ordinary guest-drop decree acceptable from one.
        if (migration_enabled() && s == hub_) {
            adopt_host_lost(s, remote_next_[si]);
            const std::vector<std::uint8_t> pkt =
                encode_host_lost(static_cast<std::uint8_t>(s), handoff_[si]);
            transport_->send(pkt.data(), pkt.size());
            continue;
        }
        // An ordinary guest drop stays a decree, and the decree is the ELECTED
        // hub's — which after a migration may well be this machine. hosting()
        // is what makes that the same line of code before and after.
        if (!hosting()) continue;
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
    if (!hosting() || dropped_ == 0) return;
    for (int s = 0; s < sim::kMaxPlayers; ++s) {
        const std::uint32_t at = handoff_[static_cast<std::size_t>(s)];
        if (at == kNoHandoff) continue;
        // A host loss is re-announced by broadcast_host_lost(), under its own
        // tag and by every holder rather than only by the hub. Re-sending it
        // here as a Drop as well would be the elected hub retroactively decreeing
        // its predecessor's death, which is precisely the authority the separate
        // tag exists to keep apart.
        if ((host_lost_ & static_cast<std::uint16_t>(1U << s)) != 0) continue;
        const std::vector<std::uint8_t> pkt = encode_drop(static_cast<std::uint8_t>(s), at);
        transport_->send(pkt.data(), pkt.size());
    }
}

void RollbackSession::request_end_round() {
    // HOST ONLY. A guest asking is not "a request that may be granted" — it is
    // nothing at all, so that the authority lives in one place instead of in the
    // caller's discipline. See the note at the top of the header.
    if (!drop_.is_host) return;
    if (aborted_ || end_tick_ != kNoEndRound) return;  // already ending: nothing to decide
    schedule_end_round(tick_ + static_cast<std::uint32_t>(max_prediction_) + kEndRoundSlackTicks);
    broadcast_end_round();  // don't wait a pump to say so
}

void RollbackSession::schedule_end_round(std::uint32_t at_tick) {
    if (end_tick_ != kNoEndRound && at_tick >= end_tick_) return;  // earliest wins
    end_tick_ = at_tick;
}

void RollbackSession::broadcast_end_round() {
    // The same redundancy broadcast_handoffs() uses, and for the same reason: a
    // peer that misses this keeps simulating a round the host has already left,
    // and there is no other channel that would ever tell it.
    if (!drop_.is_host || end_tick_ == kNoEndRound) return;
    const std::vector<std::uint8_t> pkt = encode_match_ctl(MatchCtlKind::EndRound, end_tick_);
    transport_->send(pkt.data(), pkt.size());
}

void RollbackSession::prune() {
    // Everything strictly below confirmed_ is final and never needed again
    // (rollback never targets a confirmed tick) — UNLESS host migration is on,
    // where rewind_for_migration() may move the frontier BACKWARDS onto a tick
    // whose snapshot would otherwise be gone. Then a fixed window below the
    // frontier is retained instead, and it is also what resend_from() re-sends
    // from while a migration heals.
    std::uint32_t keep_from = confirmed_;
    if (migration_enabled())
        keep_from = (confirmed_ > kMigrationRewindWindow) ? confirmed_ - kMigrationRewindWindow : 0;
    for (auto it = slots_.begin(); it != slots_.end();)
        it = (it->first < keep_from) ? slots_.erase(it) : std::next(it);
    for (auto it = snapshots_.begin(); it != snapshots_.end();)
        it = (it->first < keep_from) ? snapshots_.erase(it) : std::next(it);
    // The floor only ever RISES. confirmed_ can move back (rewind_for_migration)
    // but the ticks below it are gone for good once erased, so resend_from() must
    // never name one of them.
    if (keep_from > oldest_slot_) oldest_slot_ = keep_from;
    const std::uint32_t keep = (confirmed_ > kHashWindow) ? confirmed_ - kHashWindow : 0;
    for (auto it = hash_.begin(); it != hash_.end();)
        it = (it->first < keep) ? hash_.erase(it) : std::next(it);
    for (auto it = peer_hash_.begin(); it != peer_hash_.end();)
        it = (it->first < keep) ? peer_hash_.erase(it) : std::next(it);
}

int RollbackSession::local_lag() const {
    const std::uint16_t awaited = static_cast<std::uint16_t>(remote_seats_ & seats_awaited(tick_));
    int worst = 0;
    for (int s = 0; s < sim::kMaxPlayers; ++s) {
        if ((awaited & static_cast<std::uint16_t>(1U << s)) == 0) continue;
        const std::uint32_t next = remote_next_[static_cast<std::size_t>(s)];
        const int lag = static_cast<int>(tick_ > next ? tick_ - next : 0);
        if (lag > worst) worst = lag;
    }
    return worst;
}

int RollbackSession::peer_lag() const {
    const std::uint16_t awaited = static_cast<std::uint16_t>(remote_seats_ & seats_awaited(tick_));
    int worst = 0;
    for (int s = 0; s < sim::kMaxPlayers; ++s) {
        if ((awaited & static_cast<std::uint16_t>(1U << s)) == 0) continue;
        const int d = peer_depth_[static_cast<std::size_t>(s)];
        if (d > worst) worst = d;
    }
    return worst;
}

bool RollbackSession::rephase_eligible() const {
    // Nothing to compare against until a peer has actually spoken; and a session
    // whose partner is gone (handed to the AI, or the match already ending) has
    // no clock to align with.
    if (!peer_heard_ || aborted_ || end_tick_ != kNoEndRound) return false;
    if ((remote_seats_ & seats_awaited(tick_)) == 0) return false;
    // NOT WHILE A MIGRATION IS HEALING, and this is a genuine interaction rather
    // than caution. Both of the numbers this gates are meaningless there:
    // local_lag() is enormous because the frontier was pinned for the whole
    // outage while tick_ ran on, and peer_lag() is read from InputRange lengths
    // last received BEFORE the hub died, so it is stale by the length of the
    // outage. The difference would clear the threshold on every pump and hold a
    // tick each time — throttling precisely the peer that most needs to catch up
    // to the survivors it has just been reconnected to.
    //
    // Written as an ELIGIBILITY test rather than inside the advantage comparison
    // so that the arrival-variance absorber inherits it: `lead_target()` asks the
    // same question, and `peer_depth_spread()` is fed by the same stale
    // InputRange lengths. A lead taken off migration-poisoned readings would put
    // input lag on a peer whose real problem is that it is behind.
    if (migration_healing()) return false;
    return true;
}

int RollbackSession::frame_advantage() const {
    // LIKE FOR LIKE: the window we put on the wire against the window the peer put
    // on theirs. Both are (that peer's filing head - its confirmed frontier), so
    // when both carry a local lead the leads cancel and the comparison stays the
    // frame advantage it always was. At a lead of 0, `local_next_ - confirmed_` IS
    // local_lag() — our own seats are filed up to the head, so the confirmed
    // frontier can only be held back by a remote seat — and this line is the one
    // that shipped this morning.
    return static_cast<int>(local_next_ - confirmed_) - peer_lag();
}

void RollbackSession::note_advantage(bool eligible, int raw) {
    if (!eligible) {
        // Throw the window away rather than pad it (see the header). Both readings
        // are stale by however long the hub was gone, and the step back to live
        // values would read as arrival variance — buying a lead for a peer whose
        // actual problem is that it is behind and needs to catch up.
        advantage_count_ = 0;
        advantage_next_ = 0;
        return;
    }
    advantage_[advantage_next_] = raw;
    peer_window_[advantage_next_] = peer_lag();
    advantage_next_ = (advantage_next_ + 1) % advantage_.size();
    if (advantage_count_ < static_cast<int>(advantage_.size())) ++advantage_count_;
}

int RollbackSession::peer_depth_spread() const {
    // ARRIVAL VARIANCE AS THE PEER EXPERIENCES IT, for free, off the wire. The
    // LEVEL of the peer's prediction depth is the path's delay and is none of our
    // business — a slow link is still a perfectly playable one. Its SPREAD is the
    // part a lead can remove, and a steady path has none however slow it is.
    if (advantage_count_ < static_cast<int>(peer_window_.size())) return 0;
    const auto [lo, hi] = std::minmax_element(peer_window_.begin(), peer_window_.end());
    return *hi - *lo;
}

int RollbackSession::lead_target() const {
    if (!rephase_eligible()) return 0;  // no peer to help, or the round is ending
    const int want = peer_depth_spread() - kLeadDeadbandTicks;
    return std::clamp(want, 0, kMaxLocalLeadTicks);
}

void RollbackSession::update_lead() {
    const int target = lead_target();
    if (target > lead_) ++lead_;
    else if (target < lead_) --lead_;
}

void RollbackSession::file_local(const sim::TickInputs& local_input) {
    // Fill forward to the lead's head. `local_next_` never moves backwards and an
    // already-filed tick is never re-decided, which is what lets the lead change
    // in a live match: the peer may already have confirmed and hashed a tick we
    // filed, and re-deciding it would be a real desync (see the header note).
    //
    // Merged INTO the slot rather than assigned over it, because the slot may
    // already hold remote input that arrived before we reached this tick. A local
    // seat the host handed to the AI is skipped: the sim drives it from here.
    const std::uint32_t head = tick_ + static_cast<std::uint32_t>(lead_) + 1;
    while (local_next_ < head) {
        const std::uint16_t bits =
            static_cast<std::uint16_t>(local_seats_ & seats_awaited(local_next_));
        Slot& slot = slots_[local_next_];
        for (int s = 0; s < sim::kMaxPlayers; ++s) {
            const std::uint16_t bit = static_cast<std::uint16_t>(1U << s);
            if ((bits & bit) != 0)
                slot.inputs.players[static_cast<std::size_t>(s)] =
                    local_input.players[static_cast<std::size_t>(s)];
        }
        slot.confirmed = static_cast<std::uint16_t>(slot.confirmed | bits);
        // THIS is the instant our input for that tick first leaves — the send at
        // the bottom of this same pump carries it — and it is one end of the
        // ack-RTT (net_stats.hpp). Taken at FILING rather than at simulation
        // because with a lead those are different pumps, and dating the departure
        // from the later one would under-report the round trip by the lead. At a
        // lead of 0 they are the same pump and the same `now_ms`, so the sample is
        // the one this code always took.
        stats_.on_local_tick(local_next_);
        ++local_next_;
    }
}

int RollbackSession::sustained_advantage() const {
    // Not enough history to tell a skew from a burst yet, so claim no advantage
    // rather than guess at one. Costs the first second of a round, where the two
    // peers have not settled into a phase relationship worth correcting anyway.
    if (advantage_count_ < static_cast<int>(advantage_.size())) return 0;
    return *std::min_element(advantage_.begin(), advantage_.end());
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
    //
    // NOT while a migration is held. Every remote seat is ALREADY past the drop
    // timeout by then — that silence is how the hub's loss was detected — so
    // counting through the rewire would declare the entire surviving table
    // dropped, which is the one-lost-host-becomes-a-table-wide-cascade failure
    // set_migration_hold() exists to prevent.
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

    // THE MIGRATION STALL. Keep talking — into whatever the caller has attached,
    // possibly nothing at all yet — but simulate nothing, so the frontier, and
    // with it the floor of the retained rewind window, stays exactly where the
    // hub's death left it. prune() is deliberately not reached either.
    //
    // FIRST among the early returns on purpose: it is the strongest of them
    // ("the frontier must not move"), and it is the only one that must re-send
    // from the WIDE window rather than from confirmed_.
    if (migration_hold_) {
        send_local(resend_from());
        return;
    }

    // The round has reached its agreed abandon tick: simulate nothing more, but
    // KEEP PUMPING. A peer that is still short of the end tick needs our input
    // window to get there, and one that has not seen the announcement at all
    // needs the re-send above — so going quiet here would strand exactly the
    // peer this message exists to bring along. The caller stops calling us once
    // its own shell has moved on (round_ended()).
    if (round_ended()) {
        send_local(resend_from());
        return;
    }

    // RE-PHASE (see the header's two notes). This peer is running ahead of its
    // partner by more than the path explains, so give a tick back — on ALTERNATE
    // pumps, so the skew is shed at half rate rather than by freezing. Sits ABOVE
    // the cap check deliberately: the point is to spend the skew while there is
    // still prediction budget in hand, instead of arriving at the cap and living
    // there for the rest of the round. Counted separately from a cap stall — the
    // two are different events and conflating them would hide the cure inside the
    // symptom.
    //
    // The advantage is SAMPLED on every pump (including one we are about to hold,
    // and one where nothing is eligible) so the window below spans a fixed stretch
    // of time rather than a variable one; the DECISION is taken on the window's
    // minimum, which is what makes arrival variance unable to trigger it.
    const bool eligible = rephase_eligible();
    const int raw = eligible ? frame_advantage() : 0;
    note_advantage(eligible, raw);
    const int sustained = eligible ? sustained_advantage() : 0;
    // The decision is the window's MINIMUM, or — for an advantage larger than the
    // measured variance can explain — the current sample on its own, so that a
    // frozen peer's several ticks of skew are not left to spend the whole
    // prediction budget while the window fills. Both arms imply
    // `raw >= kRephaseAdvantageTicks`, which is what keeps this a subset of the
    // unfiltered controller's holds. See the header's jitter note for why each
    // half is shaped the way it is, and what was measured with and without them.
    const int spread = peer_depth_spread();
    const bool raw_asks = eligible && raw >= kRephaseAdvantageTicks;
    const bool sustained_asks = eligible && (sustained >= kRephaseAdvantageTicks ||
                                             raw >= spread + kRephaseAdvantageTicks);
    // "The raw reading wanted this tick and the filter kept it" — the absorber's
    // own meter, and the only way to see from the outside that it is doing
    // anything (net_stats.hpp). Not a decision input: nothing below reads it.
    stats_.on_timing(raw, sustained, !rephase_held_ && raw_asks && !sustained_asks, lead_, spread);
    if (!rephase_held_ && sustained_asks) {
        rephase_held_ = true;
        stats_.on_rephase_hold();
        send_local(resend_from());  // the peer still needs our window to catch up
        return;
    }
    rephase_held_ = false;

    // Hold at the prediction cap so the display never runs unboundedly ahead of
    // the peer (bounded memory + bounded re-sim on a correction). Keep re-sending
    // local input so the peer catches up and our confirmations advance.
    if (tick_ - confirmed_ >= static_cast<std::uint32_t>(max_prediction_)) {
        // A pump that could not simulate. THIS is what a netcode stutter is —
        // a displayed frame the game was not allowed to advance because a peer's
        // input had not arrived — so it is counted rather than merely happening.
        stats_.on_stall();
        send_local(resend_from());
        return;
    }

    // File the local sample. At a lead of 0 this is exactly "merge it into
    // slots_[tick_]", which is what this line was; with a lead it also fills the
    // ticks ahead of the head that the peer is about to need (the header's jitter
    // note). Ordered after the hold and cap checks on purpose: a pump that does
    // not simulate must not consume a sample either, or the lead would grow by one
    // for every stalled pump.
    update_lead();
    file_local(local_input);

    snapshots_[tick_] = sim_->state();
    apply_handoffs(tick_);  // same call the re-sim makes, so both paths agree
    const sim::TickInputs in = assemble(tick_);
    slots_[tick_].inputs = in;  // store actually-simulated inputs (predicted remote incl.) for compares
    sim_->tick(in);
    ++tick_;
    // Covers [resend_from(), local_next_) — BOTH widenings at once, and they are
    // independent by construction: resend_from() decides where the window STARTS
    // (confirmed_, or the oldest retained tick while a migration heals), the
    // lead decides where it ENDS. Neither needs to know about the other, and the
    // end is chosen inside send_local() so that every one of its five call sites
    // gets it without being widened by hand — which is the failure mode host
    // migration was re-implemented to avoid.
    //
    // `stats_.on_local_tick` is NOT called here any more: with a lead, a tick's
    // input first leaves in the pump that FILED it, which is earlier than the
    // pump that simulates it, so the ack-RTT is started in file_local() instead.
    // At a lead of 0 that is the same pump, the same `now_ms` and the same tick
    // number, so the sample is unchanged.
    send_local(resend_from());
    prune();
}

}  // namespace bomber::net
