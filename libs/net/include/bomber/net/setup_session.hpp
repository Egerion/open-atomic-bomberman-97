#pragma once

#include <cstdint>
#include <vector>

#include "bomber/net/protocol.hpp"
#include "bomber/net/transport.hpp"
#include "bomber/sim/match_config.hpp"

// The HOST-AUTHORITATIVE match-setup layer, run between the hole-punch and the
// match itself so an online game gets a real map choice, AI slots and roster
// (docs/online-multiplayer-design.md §9).
//
// SHAPE, FROM THE ORIGINAL (docs/re/network-screens.md §7): both 1997 network
// screens hand off to sub_42A3F6, the SAME handler the local PLAY row uses, so
// the host makes every change and broadcasts it while guests are strictly
// read-only (every edit path is guarded by `sub_40C06A() != 1`). Reproduced over
// our own P2P transport AFTER the punch, so the matchmaker stays
// config-agnostic — the server never sees a MatchConfig.
//
// TWO PAYLOADS, AND THE DIFFERENCE IS LOAD-BEARING:
//
//   * the LIVE PREVIEW (SetupPreviewFrame) is a compact subset feeding the
//     guest's read-only display while the host edits. Allowed to be lossy;
//     re-sent on an interval to ride out UDP loss.
//   * the FINAL payload is the WHOLE serialized sim::MatchConfig, chunked, and
//     it is what Simulation is built from. Do NOT "optimise" it into a level
//     index, a digest or a delta: a guest whose .SCH, EXTRA<n>.RES, VALUELST or
//     custom-map list differs would build a different board from the same index
//     and desync on tick 0 (match_config_codec.hpp).
//
// Pump-based and clock-injected; all I/O happens inside step(), while publish()
// and confirm() only record intent. Rides the SAME Transport the punch produced;
// datagrams of other kinds are decoded and ignored.
//
// N PEERS, NOT TWO. The host tracks which of the GUEST seats it expects have
// acknowledged the CURRENT revision, so Phase::Final means every one of them
// holds the exact bytes rather than that somebody does — which is what makes it
// usable over a StarHubTransport. A guest stamps each ack with its own seat
// (wire v5), one per seat it owns, so the host never has to know the
// machine-to-seat grouping.
//
// ONE CALLER OBLIGATION, AND IT IS A SHARP EDGE: **one pump at a time.** This
// session and the match session both drain the same Transport, and whichever
// polls first CONSUMES the datagram. Stop pumping this one before starting the
// match one. That holds with a hub in the middle too: the hub's
// StarHubTransport reflects a guest's datagram INSIDE poll(), so whichever
// session is pumping is also the one keeping the star alive.
//
// NOT IMPLEMENTED, DELIBERATELY: the original's guest->host slot upload (kind 40
// from sub_410F81's tail). A guest contributes nothing to the roster. Adding it
// is a new MsgType plus a host-side merge and another wire-version bump.

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
    // The two roles' halves of step(): the host owes re-sends and can time out
    // waiting for acks, the guest is purely reactive.
    void step_host(std::int64_t now_ms);
    void step_guest(std::int64_t now_ms);
    bool resend_due(std::int64_t now_ms, int interval_ms) const;
    void drain(std::int64_t now_ms);
    void on_message(const Message& m, std::int64_t now_ms);
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
