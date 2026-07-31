# The P2P data-plane wire format

The datagrams `libs/net` peers exchange directly with each other, over whichever
`Transport` won (`libs/net/include/bomber/net/protocol.hpp`). This is **not** the
matchmaker control plane — that is `services/matchmaker/PROTOCOL.md`, which is
frozen against a deployed server and documented on its own.

Every packet is a one-byte `MsgType` tag followed by its payload. All multi-byte
fields are little-endian. `decode()` returns false — leaving its output untouched
— on an unknown tag, a short buffer, or any payload that fails the checks below.

`kWireProtocolVersion` (`build_hash.hpp`) is bumped whenever anything on this
page changes shape; peers on different versions are refused at the lobby door
rather than allowed to desync mid-match.

## Layouts

| tag | # | payload | size |
|---|---|---|---|
| `Input` | 0 | `[input_codec frame]` — the seats in `seat_mask`, stamped `tick` | variable |
| `Hash` | 1 | `[tick u32][hash u64]` | 13 |
| `InputRange` | 2 | `[first_tick u32][count u8][seat_mask u16][count × (one packed byte per set seat)]` | variable |
| `Hello` | 3 | `[seed u32][is_ack u8]` | 6 |
| `Punch` | 4 | `[nonce u32][is_pong u8]` | 6 |
| `Drop` | 5 | `[seat u8][at_tick u32]` | 6 |
| `SetupPreview` | 6 | `[revision u32][level_index u8][rounds u8][name_len u8][name][10 × slot_kind u8][10 × team u8]` | 28–60 |
| `SetupChunk` | 7 | `[revision u32][total_len u32][checksum u32][chunk_count u8][chunk_index u8][payload]` | 15 + ≤1024 |
| `SetupAck` | 8 | `[revision u32][checksum u32][seat u8]` | 10 |
| `Probe` | 9 | `[nonce u32][seen_peer u8]` | 6 |
| `MatchCtl` | 10 | `[kind u8][at_tick u32]` | 6 |
| `HostLost` | 11 | `[seat u8][at_tick u32]` | 6 |

## Validation on decode

The rule they all share: **a field that indexes anything is bounds-checked at the
wire.** An out-of-range value means a peer that disagrees with us about the
protocol, and the `build_hash` door was supposed to have caught that.

- **`InputRange`** — `count` rides in a `u8`, so a range is at most 255 ticks; an
  empty range encodes nothing useful and is rejected. The widest range the
  rollback session ever sends is the migration rewind window (64) plus the
  prediction cap plus `kMaxLocalLeadTicks` — under 80 ticks of one seat, clear of
  both the 255 limit and the ~1200-byte safe UDP payload.
- **`Drop`, `HostLost`, `SetupAck`** — the seat index must be inside
  `[0, sim::kMaxPlayers)`; each indexes a per-seat array or mask.
- **`MatchCtl`** — the kind must not exceed `Rematch`.
- **`SetupPreview`** — the name must be no longer than `kSetupLevelNameMax`, every
  name byte must be printable ASCII (a hostile peer must not be able to push
  control characters into the host's level label), every slot kind must be at
  most `Remote`, and the buffer length must agree with `name_len`.
- **`SetupChunk`** — `total_len` must be nonzero and at most
  `kMaxMatchConfigBytes`; `chunk_count` must be exactly
  `ceil(total_len / kSetupChunkPayloadBytes)`; `chunk_index` must be inside that
  count; and the payload length must match the slice it claims to be. Together
  these mean a receiver can never be talked into a short or overlapping
  reassembly. 1024-byte payloads keep the whole datagram at 1039 bytes — under
  the safe UDP payload even after `RelayedTransport`'s 17-byte header, so nothing
  depends on IP fragmentation surviving the path.

## Relay header

`RelayedTransport` prepends its own fixed routing header, which the matchmaker's
forwarder reads and strips (`PROTOCOL.md` §6, frozen):

    [16 bytes alloc_id][1 byte seat][opaque payload …]

Outbound the `alloc_id` is ours and the seat is the DESTINATION; inbound the
`alloc_id` is ours again and the seat is the SENDER's, so a receiver always
strips the same 17 bytes. The relay never decodes the payload.
