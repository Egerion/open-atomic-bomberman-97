#include "bomber/net/setup_session.hpp"

#include <cstring>
#include <utility>

#include "bomber/net/match_config_codec.hpp"
#include "bomber/sim/constants.hpp"  // sim::kMaxPlayers

namespace bomber::net {

namespace {

// FNV-1a/32 over the serialized config. Not a security MAC — a peer that can
// forge chunks can forge this too. Its job is to prove a REASSEMBLY is intact
// and belongs to one confirmation: it stops a mix of two revisions' chunks (or
// a stale slice replayed by the network) from ever reaching decode, and it is
// what the ack echoes so the host knows the guest holds ITS bytes.
std::uint32_t blob_digest(const std::uint8_t* d, std::size_t n) {
    std::uint32_t h = 0x811C9DC5U;
    for (std::size_t i = 0; i < n; ++i) {
        h ^= d[i];
        h *= 0x01000193U;
    }
    return h;
}

}  // namespace

SetupSession::SetupSession(Transport& t, bool is_host, std::uint16_t local_seats,
                           std::uint16_t guest_seats, int timeout_ms)
    : transport_(&t),
      timeout_ms_(timeout_ms),
      local_seats_(local_seats),
      // A seat cannot be both ours and a guest's. Masking here rather than
      // trusting the caller keeps a mistake from making Final unreachable
      // forever: our own seat never sends an ack, so waiting for one would hang.
      guest_seats_(is_host ? static_cast<std::uint16_t>(guest_seats & ~local_seats)
                           : std::uint16_t{0}),
      is_host_(is_host) {}

void SetupSession::publish(const SetupPreviewFrame& preview) {
    if (!is_host_ || have_final_) return;  // guests are read-only; confirm() is final
    preview_ = preview;
    preview_.revision = ++revision_;
    preview_bytes_ = encode_setup_preview(preview_);
    have_preview_ = true;
    phase_ = Phase::Live;
    last_send_ms_ = -1;  // go out on the very next step(), not one interval later
}

void SetupSession::confirm(const sim::MatchConfig& cfg) {
    if (!is_host_) return;
    final_ = cfg;
    blob_ = encode_match_config(final_);
    blob_checksum_ = blob_digest(blob_.data(), blob_.size());
    ++revision_;
    have_final_ = true;
    // A new confirmation invalidates every ack for the old one: the guests must
    // acknowledge THESE bytes, not the ones they happen to be holding.
    acked_seats_ = 0;
    // With no guests to wait for there is nothing to be Confirming about.
    phase_ = all_acked() ? Phase::Final : Phase::Confirming;
    last_send_ms_ = -1;
    confirm_ms_ = -1;  // armed by the next step(), which knows `now`
}

bool SetupSession::resend_due(std::int64_t now_ms, int interval_ms) const {
    return last_send_ms_ < 0 || now_ms - last_send_ms_ >= interval_ms;  // -1 = nothing sent yet
}

void SetupSession::step_host(std::int64_t now_ms) {
    if (!have_final_) {
        // Still editing: the preview goes out lazily and nothing can time out.
        if (have_preview_ && resend_due(now_ms, kSetupPreviewResendMs)) {
            send_preview();
            last_send_ms_ = now_ms;
        }
        return;
    }
    if (all_acked()) return;  // every guest holds it — nothing left to send
    if (resend_due(now_ms, kSetupConfigResendMs)) {
        send_config_chunks();
        last_send_ms_ = now_ms;
    }
    if (confirm_ms_ >= 0 && now_ms - confirm_ms_ >= timeout_ms_) phase_ = Phase::Failed;
}

void SetupSession::step_guest(std::int64_t now_ms) {
    // Purely reactive. Silence while still waiting for (or watching) the host's
    // setup means the host is gone; a guest already in Final never expires,
    // because it has everything it needs.
    if (phase_ != Phase::Waiting && phase_ != Phase::Live) return;
    if (now_ms - last_rx_ms_ >= timeout_ms_) phase_ = Phase::Failed;
}

void SetupSession::step(std::int64_t now_ms) {
    if (phase_ == Phase::Failed) return;
    if (last_rx_ms_ < 0) last_rx_ms_ = now_ms;  // first pump seeds the liveness clock
    if (have_final_ && is_host_ && confirm_ms_ < 0) confirm_ms_ = now_ms;

    drain(now_ms);
    if (phase_ == Phase::Failed) return;
    if (is_host_) {
        step_host(now_ms);
        return;
    }
    step_guest(now_ms);
}

void SetupSession::on_message(const Message& m, std::int64_t now_ms) {
    // Each role reads only the other's frames; its own, reflected back off a star
    // hub, falls through. Anything else on the shared socket (a trailing
    // hole-punch PONG, say) does too.
    switch (m.type) {
        case MsgType::SetupPreview:
            if (!is_host_) on_preview(m.setup_preview, now_ms);
            break;
        case MsgType::SetupChunk:
            if (!is_host_) on_chunk(m.setup_chunk, now_ms);
            break;
        case MsgType::SetupAck:
            if (is_host_) on_ack(m.setup_ack);
            break;
        default: break;
    }
}

void SetupSession::drain(std::int64_t now_ms) {
    std::vector<std::uint8_t> buf;
    while (transport_->poll(&buf)) {
        Message m;
        if (decode(buf.data(), buf.size(), &m)) on_message(m, now_ms);  // untrusted: drop silently
    }
}

void SetupSession::on_preview(const SetupPreviewFrame& p, std::int64_t now_ms) {
    last_rx_ms_ = now_ms;
    if (have_final_) return;  // the confirmed config supersedes every preview
    // UDP reorders freely, so an older revision arriving after a newer one must
    // not roll the display back.
    if (have_preview_ && p.revision <= revision_) return;
    preview_ = p;
    revision_ = p.revision;
    have_preview_ = true;
    phase_ = Phase::Live;
}

void SetupSession::reset_reassembly(const SetupChunkFrame& c) {
    rx_revision_ = c.revision;
    rx_total_len_ = c.total_len;
    rx_checksum_ = c.checksum;
    rx_chunk_count_ = c.chunk_count;
    blob_.assign(c.total_len, 0);
    rx_have_mask_ = 0;
}

void SetupSession::on_chunk(const SetupChunkFrame& c, std::int64_t now_ms) {
    last_rx_ms_ = now_ms;
    // decode() already enforces this; belt and braces, because everything below
    // shifts by chunk_index.
    if (c.chunk_count == 0 || c.chunk_count > kMaxSetupChunks) return;

    if (have_final_ && c.revision <= revision_) {
        // Already latched this (or an older) confirmation. The host only keeps
        // sending because one of our acks was lost — or because ANOTHER guest is
        // still missing a chunk, since the star delivers its burst to all of us —
        // so re-ack instead of ignoring it. That is the whole ack-loss recovery,
        // and re-acking a revision the host already counted is a no-op there.
        if (c.revision == revision_ && c.checksum == blob_checksum_)
            send_acks(revision_, blob_checksum_);
        return;
    }
    // A NEWER revision supersedes: reassemble it while still holding the old
    // config, and swap only once the new one validates end to end.
    if (c.revision < rx_revision_) return;  // a stale slice replayed late

    // Any disagreement with the reassembly in progress means this chunk belongs
    // to a DIFFERENT confirmation: start that one from scratch rather than
    // blending the two. Nothing partial can ever be applied, because the config
    // is only decoded once every chunk of one identity is present.
    if (c.revision != rx_revision_ || c.total_len != rx_total_len_ || c.checksum != rx_checksum_ ||
        blob_.size() != c.total_len)
        reset_reassembly(c);

    const std::size_t begin = static_cast<std::size_t>(c.chunk_index) * kSetupChunkPayloadBytes;
    if (begin + c.payload.size() > blob_.size()) return;  // decode() guarantees this; be sure
    std::memcpy(blob_.data() + begin, c.payload.data(), c.payload.size());
    rx_have_mask_ |= static_cast<std::uint16_t>(1U << c.chunk_index);

    const auto full = static_cast<std::uint16_t>((1U << rx_chunk_count_) - 1U);
    if (rx_have_mask_ != full) return;  // still missing a slice — hold everything

    if (blob_digest(blob_.data(), blob_.size()) != rx_checksum_) {
        rx_have_mask_ = 0;  // corrupt or mixed: drop the lot, wait for the resend
        return;
    }
    sim::MatchConfig cfg;
    if (!decode_match_config(blob_.data(), blob_.size(), &cfg)) {
        rx_have_mask_ = 0;
        return;
    }

    final_ = std::move(cfg);
    revision_ = rx_revision_;
    blob_checksum_ = rx_checksum_;
    have_final_ = true;
    phase_ = Phase::Final;
    send_acks(revision_, blob_checksum_);
}

void SetupSession::on_ack(const SetupAckFrame& a) {
    if (!have_final_) return;
    if (a.revision != revision_ || a.checksum != blob_checksum_) return;  // an older confirmation
    const auto bit = static_cast<std::uint16_t>(1U << a.seat);
    // A seat we are not waiting on: our own echo off the star's reflection, a
    // spectator, or a peer that is simply not in this match. Never let it stand
    // in for a seat that still owes us an ack.
    if ((guest_seats_ & bit) == 0) return;
    acked_seats_ = static_cast<std::uint16_t>(acked_seats_ | bit);
    if (all_acked()) phase_ = Phase::Final;  // ... and ONLY then
}

void SetupSession::send_preview() {
    transport_->send(preview_bytes_.data(), preview_bytes_.size());
}

void SetupSession::send_config_chunks() {
    const std::size_t total = blob_.size();
    const std::size_t count = (total + kSetupChunkPayloadBytes - 1) / kSetupChunkPayloadBytes;
    // The whole blob goes out every round rather than only the slices the guest
    // is missing: there is no per-chunk feedback (the ack is all-or-nothing) and
    // the burst is two ~1 KB datagrams, so selective repair would cost more
    // protocol than it saves bytes.
    for (std::size_t index = 0; index < count; ++index) {
        const std::size_t off = index * kSetupChunkPayloadBytes;
        const std::size_t remaining = total - off;
        const std::size_t len =
            remaining < kSetupChunkPayloadBytes ? remaining : kSetupChunkPayloadBytes;
        SetupChunkFrame c;
        c.revision = revision_;
        c.total_len = static_cast<std::uint32_t>(total);
        c.checksum = blob_checksum_;
        c.chunk_count = static_cast<std::uint8_t>(count);
        c.chunk_index = static_cast<std::uint8_t>(index);
        c.payload.assign(blob_.begin() + static_cast<std::ptrdiff_t>(off),
                         blob_.begin() + static_cast<std::ptrdiff_t>(off + len));
        const std::vector<std::uint8_t> packet = encode_setup_chunk(c);
        transport_->send(packet.data(), packet.size());
    }
}

void SetupSession::send_acks(std::uint32_t revision, std::uint32_t checksum) {
    // ONE ACK PER SEAT WE OWN. The host clears a per-seat mask, so a machine
    // seating two humans must answer for both or the host would wait forever for
    // the seat that never spoke. Today the matchmaker gives each connection
    // exactly one seat (PROTOCOL.md §4 StartMatch), so this is a single 10-byte
    // datagram — but the rule costs nothing and removes the trap.
    for (int s = 0; s < sim::kMaxPlayers; ++s) {
        if ((local_seats_ & static_cast<std::uint16_t>(1U << s)) == 0) continue;
        const std::vector<std::uint8_t> packet =
            encode_setup_ack(revision, checksum, static_cast<std::uint8_t>(s));
        transport_->send(packet.data(), packet.size());
    }
}

}  // namespace bomber::net
