#pragma once

#include <cstdint>
#include <vector>

#include "bomber/net/protocol.hpp"
#include "bomber/net/transport.hpp"
#include "bomber/sim/match_config.hpp"

// The HOST-AUTHORITATIVE match-setup layer: the live sync that runs between the
// hole-punch and the match itself, so an online game finally gets a real map
// choice, AI slots and roster instead of the hard-coded canonical config
// netplay used to fall back on.
//
// SHAPE, FROM THE ORIGINAL (docs/re/network-screens.md §7). Both 1997 network
// screens hand off to sub_42A3F6 — the SAME handler the local PLAY row uses —
// so map selection and AI/CPU slot assignment for a net game come from the
// ORDINARY roster (sub_410F81) and level (sub_406DDE) screens. There, the host
// makes every change and broadcasts it (roster slot kind 40, team kind 58,
// level index kind 43, round count kind 44) while guests are strictly
// read-only: every edit path is guarded by `sub_40C06A() != 1` and a guest that
// presses an edit key just gets SFX 40. We reproduce that shape over our own
// P2P transport, AFTER the punch, so the matchmaker stays config-agnostic
// (ADR-0011: the server never sees a MatchConfig).
//
// TWO PAYLOADS, AND THE DIFFERENCE IS LOAD-BEARING:
//
//   * The LIVE PREVIEW (SetupPreviewFrame) is a compact subset — per-slot kind,
//     level index + name, round count, team flags. It exists only to feed the
//     guest's READ-ONLY display while the host is still editing, so it is
//     allowed to be lossy and is re-sent on an interval to ride out UDP loss.
//
//   * The FINAL payload is the WHOLE serialized sim::MatchConfig
//     (match_config_codec.hpp), chunked. It is what Simulation is built from.
//     Do NOT "optimise" it into a level index, a digest, or a delta: every peer
//     must feed Simulation byte-identical input, and a guest whose .SCH,
//     EXTRA<n>.RES, VALUELST or custom-map list differs would otherwise build a
//     different board from the same index and desync on tick 0. The preview may
//     be approximate; the final may not.
//
// Pump-based and CLOCK-INJECTED like Rendezvous / StunClient / LobbyFlow:
// step(now_ms) takes the caller's monotonic clock, so libs/net stays clock-free
// and the whole flow is deterministically testable. All I/O happens inside
// step(); publish() and confirm() only record intent. It rides the SAME
// Transport the punch produced (connected by then) and the match session takes
// over afterwards — datagrams of other kinds (a trailing hole-punch PONG, say)
// are decoded and ignored.
//
// N PEERS, NOT TWO (the former "obligation 2", discharged). The host is handed
// the mask of every GUEST seat it expects and tracks which of them have
// acknowledged the CURRENT revision; Phase::Final means EVERY one of them holds
// the exact bytes, not merely that somebody does. That is what makes this usable
// over a StarHubTransport (>2 seats, ADR-0011 decisions 2+4), where the host's
// send() fans out to every guest and each guest answers independently. A guest
// stamps its ack with its own seat (SetupAckFrame::seat, wire v5) — one ack per
// seat it owns, so a machine holding several seats needs no special case and the
// host never has to know the machine↔seat grouping.
//
// ONE CALLER OBLIGATION, and it is a sharp edge:
//
//   ONE PUMP AT A TIME. This session and the match session (Lockstep/Rollback)
//   both drain the same Transport, and whichever polls first CONSUMES the
//   datagram. Stop pumping this one before you start the match one; do not
//   overlap them. This holds unchanged with a hub in the middle: the hub's
//   StarHubTransport reflects a guest's datagram to the other guests INSIDE
//   poll(), so whichever session is pumping is also the one keeping the star
//   alive — which is exactly why only one of them may be running.
//
// NOT IMPLEMENTED, DELIBERATELY: the original's guest->host slot upload (kind
// 40 from sub_410F81's tail, "how a machine contributes its local humans/AI to
// the shared roster"). This layer is host-drives-everything only; a guest
// contributes nothing to the roster. Adding it later is a new MsgType plus a
// host-side merge, and needs another kWireProtocolVersion bump.

namespace bomber::net {

// Re-send intervals, in the caller's monotonic milliseconds. The preview is
// cosmetic so it goes out lazily; the confirmed config is on the critical path
// to match start, so its (2-chunk, ~2 KB) burst repeats faster until the guest
// acknowledges — then sending stops entirely.
inline constexpr int kSetupPreviewResendMs = 250;
inline constexpr int kSetupConfigResendMs = 200;

class SetupSession {
public:
    enum class Phase : std::uint8_t {
        Waiting,     // no setup information yet (guest: the host has not spoken)
        Live,        // a live preview is current; the host is still editing
        Confirming,  // HOST only: the final config is in flight, awaiting the ack
        Final,       // the authoritative config is available on BOTH peers
        Failed,      // timed out — see the timeout rules below
    };

    // `t` is BORROWED and must outlive the session (the caller owns the socket
    // and hands the SAME transport on to the match session afterwards).
    //
    // `local_seats` — the seats THIS machine owns. A GUEST stamps one ack per
    //     set bit, which is how the host tells whose acknowledgement it just
    //     read; it must be NON-ZERO on a guest, because a guest with no seats
    //     can send no ack and the host would simply time out waiting. On the
    //     host it is only used to keep `guest_seats` honest.
    // `guest_seats` — HOST only: every seat the host expects an ack FROM, i.e.
    //     the match's network seats minus its own. Phase::Final is reached only
    //     once all of them have acked the current revision. An EMPTY mask means
    //     there is nobody to wait for, so confirm() lands straight in Final.
    //     Ignored on a guest.
    //
    // `timeout_ms` guards the two waits that can hang, and ONLY those:
    //   GUEST — no setup datagram at all for this long while in Waiting/Live
    //           (the host re-sends on an interval, so silence means it is gone).
    //   HOST  — not every expected ack within this long after confirm().
    // Neither the host's editing time nor a peer sitting in Final ever expires.
    SetupSession(Transport& t, bool is_host, std::uint16_t local_seats, std::uint16_t guest_seats,
                 int timeout_ms = 30000);

    // --- host intents (a guest ignores both: read-only, per sub_40C06A) -------

    // Replace the live preview and start broadcasting it. Bumps revision().
    // Ignored once confirm() has been called — confirm is the point of no
    // return for this session, mirroring the original's Enter (kind 32
    // sub_40F064(901/902), which pushes guests out of the shared screens).
    void publish(const SetupPreviewFrame& preview);

    // Commit `cfg` as THE match config: it is serialized, chunked and
    // broadcast until the guest acknowledges the exact same bytes. Bumps
    // revision(); calling it again supersedes the previous confirmation.
    void confirm(const sim::MatchConfig& cfg);

    // --- pump ---------------------------------------------------------------

    // Drain inbound, then (re)send whatever this role owes. Safe every frame.
    void step(std::int64_t now_ms);

    // --- readable state (what the GUI renders) -------------------------------

    Phase phase() const { return phase_; }
    bool is_host() const { return is_host_; }
    bool failed() const { return phase_ == Phase::Failed; }

    // The newest setup information held here: the host's own on a host, the
    // latest accepted broadcast on a guest. Older revisions arriving late (UDP
    // reorders freely) are discarded.
    std::uint32_t revision() const { return revision_; }

    bool has_preview() const { return have_preview_; }
    const SetupPreviewFrame& preview() const { return preview_; }

    // The authoritative config. On a HOST this is true from confirm() on, i.e.
    // during Confirming as well as Final — phase() is what says whether the
    // guest has it too, so start the match on Phase::Final, not on this.
    bool has_final_config() const { return have_final_; }
    const sim::MatchConfig& final_config() const { return final_; }

    // HOST only: EVERY expected guest reassembled and decoded OUR exact bytes
    // (each ack carries the blob checksum, not just the revision). Always false
    // on a guest, where the equivalent signal is phase() == Final.
    bool peer_acked() const { return is_host_ && have_final_ && all_acked(); }

    // HOST only, for a progress display: which guest seats have acknowledged the
    // current revision, and which are still outstanding. Both are 0 on a guest.
    std::uint16_t acked_seats() const { return acked_seats_; }
    std::uint16_t pending_seats() const {
        return static_cast<std::uint16_t>(guest_seats_ & ~acked_seats_);
    }

private:
    void drain(std::int64_t now_ms);
    void on_preview(const SetupPreviewFrame& p, std::int64_t now_ms);
    void on_chunk(const SetupChunkFrame& c, std::int64_t now_ms);
    void on_ack(const SetupAckFrame& a);
    void send_preview();
    void send_config_chunks();
    void send_acks(std::uint32_t revision, std::uint32_t checksum);
    void reset_reassembly(const SetupChunkFrame& c);
    bool all_acked() const { return (acked_seats_ & guest_seats_) == guest_seats_; }

    // Members are grouped by ALIGNMENT, not by topic (as in LobbyFlow):
    // interleaving them by topic costs padding that
    // clang-analyzer-optin.performance.Padding rightly flags.

    // --- pointer-aligned ---
    Transport* transport_;
    sim::MatchConfig final_;                   // valid once have_final_
    SetupPreviewFrame preview_;                // valid once have_preview_
    std::vector<std::uint8_t> blob_;           // host: the encoded config; guest: reassembly
    std::vector<std::uint8_t> preview_bytes_;  // cached encoding, re-sent verbatim
    std::int64_t last_send_ms_ = -1;           // -1 = nothing sent yet, so step() sends at once
    std::int64_t last_rx_ms_ = -1;             // guest liveness clock
    std::int64_t confirm_ms_ = -1;             // host ack deadline base

    // --- 4-byte ---
    int timeout_ms_;
    std::uint32_t revision_ = 0;
    std::uint32_t blob_checksum_ = 0;  // of blob_ once have_final_
    std::uint32_t rx_revision_ = 0;    // the reassembly in progress (guest)
    std::uint32_t rx_total_len_ = 0;
    std::uint32_t rx_checksum_ = 0;

    // --- 2-byte ---
    std::uint16_t rx_have_mask_ = 0;  // bit i = chunk i present
    std::uint16_t local_seats_;       // guest: the seats each ack is stamped with
    std::uint16_t guest_seats_;       // host: every seat that must ack
    std::uint16_t acked_seats_ = 0;   // host: which of them have, for THIS revision

    // --- 1-byte ---
    std::uint8_t rx_chunk_count_ = 0;
    Phase phase_ = Phase::Waiting;
    bool is_host_;
    bool have_preview_ = false;
    bool have_final_ = false;
};

}  // namespace bomber::net
