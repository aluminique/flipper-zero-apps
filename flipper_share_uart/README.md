# Flipper Share UART — direct file transfer between Flippers over a 3-wire link

> **⚠️ WARNING:** Flipper Share UART is an **experimental-only** app, it is not recommended for regular use.
> Consider using other Flipper Share transports (NFC, Sub-GHz, IR) for everyday file transfer.

## Overview

**Flipper Share UART** transfers any file directly from one Flipper Zero to another over a
**3-wire UART link** on the GPIO header (USART1, pins 13/14, 230400 baud, full duplex) —
no phone, computer, internet or radio needed, just three jumper wires.

It is a rewrite of Flipper Share with the transport replaced by a COBS-framed serial link.
The basics of the classic flipper_share file-transfer protocol (resumable,
integrity-checked) are preserved.

Expected transfer speed is around **20 KB/s** — line-rate bound, by far the fastest
Flipper Share transport, but the only one that needs wires. This is an estimate from the
baud rate, not a bench measurement (see *Status* below).

Other Flipper Share transports (Sub-GHz, IR, NFC & more): [github.com/lomalkin/flipper-zero-apps](https://github.com/lomalkin/flipper-zero-apps)

Features:

- No extra hardware beyond three jumper wires — no adapter, no level shifter. Builds with
  `ufbt` against the official firmware; no firmware modification.
- Integrity check with an MD5 hash after reception; per-packet CRC16.
- Automatic retransmission of lost/corrupted packets. Pull a wire mid-transfer and
  reconnect — the receiver's block bitmap picks up where it left off.
- Full duplex and symmetric: no turnaround, no collisions, no jitter logic.
- Torrent-like progress bar on the receiver; filename/size and ETA on the sender.

# Wiring

| Flipper A | Flipper B |
|---|---|
| pin 13 (TX) | pin 14 (RX) |
| pin 14 (RX) | pin 13 (TX) |
| pin 8/11/18 (GND) | pin 8/11/18 (GND) |

Three jumper wires, **TX/RX crossed**, grounds common. The app's UI abbreviates this as
`13-14 X, GND-GND`.

# Usage

1. Wire the two Flippers as above.
2. On the receiving Flipper: open Flipper Share UART → **Receive via UART**.
3. On the sending Flipper: open Flipper Share UART → **Send via UART** → pick a file →
   **OK**. The receiver shows a progress bar and verifies the MD5 hash at the end; the
   file is saved to `/ext/inbox/`.

The sender shows the file name, size and a rough ETA. The receiver shows
"13-14 X, GND-GND / Waiting for announce..." until it locks, then the progress bar with
percentage and ETA.

The app takes over USART1 while a transfer scene is open: it disables the expansion
service and acquires the port, releasing both on exit. If something else already holds the
port, the scene shows **"UART port busy"** and does nothing — back out with the Back
button. (The CLI is unaffected; it runs over USB CDC.)

---

# Flipper Share UART protocol

Two layers: a **COBS-framed serial transport** (physical/link layer) under the existing
**file-transfer protocol** (selective-repeat ARQ). The file-transfer protocol is identical
to the other Flipper Share builds; only the transport differs.

## Physical / link layer — the UART transport

- **Port:** USART1 at `UART_TP_BAUD` (230400 baud, 8N1), full duplex, acquired through
  `furi_hal_serial_control_acquire`. The expansion service is disabled first — mandatory
  before taking the port, and required by the expansion header's own contract — and
  re-enabled on teardown.
- **Framing:** COBS (Consistent Overhead Byte Stuffing) with `0x00` as the frame
  delimiter. On the wire: `COBS_encode(packet) + 0x00`; encoded data never contains a zero
  byte, so the delimiter is unambiguous. Worst-case overhead is `len/254 + 1` bytes.
- **Desync recovery is inherent:** after any corruption the decoder drops bytes until the
  next `0x00` and resynchronizes. A damaged frame either fails COBS decode or fails the
  engine's length/CRC16 check — both are silent drops, and the ARQ re-requests. No
  transport CRC is added on top of the packet CRC16 and the whole-file MD5.
- **RX path:** the serial RX interrupt only drains bytes into a `FuriStreamBuffer` —
  nothing else runs in ISR context. A `UartRxWorker` thread accumulates bytes up to the
  delimiter, COBS-decodes, and calls `fsh_receive_callback` in thread context, which is
  what the engine expects. Frames longer than `UART_TP_FRAME_MAX` without a delimiter are
  discarded up to the next one.
- **TX path:** COBS-encode, append the delimiter, then transmit under a mutex with
  `furi_hal_serial_tx` + `tx_wait_complete`. The blocking write is deliberate: at 230400
  baud a 521-byte DATA packet occupies ~23 ms, and that backpressure paces the engine's
  send loop, so no outbound mailbox is needed (unlike the NFC and iButton transports).
- **Symmetry:** both sides run RX permanently. There is no turnaround and no contention,
  so `FSH_REQUEST_JITTER_MS` is 0 and the half-duplex timeouts are small.

## Packet structure

Every packet: `[version(1)][tx_id(1)][packet_type(1)][payload][crc16(2)]`. The payload
length depends on the type. `FSH_DATA_LENGTH` is 512 here — a 521-byte DATA packet, ~23 ms
of line time.

### `0x01` — Announce (control payload)

| Field       | Size     | Type                  |
|-------------|----------|-----------------------|
| `file_name` | 36 bytes | char[36], zero-padded |
| `file_size` | 4 bytes  | uint32_t              |
| `file_hash` | 16 bytes | MD5                   |

### `0x02` — Request range (control payload)

| Field     | Size    | Type     |
|-----------|---------|----------|
| `start`   | 4 bytes | uint32_t |
| `end`     | 4 bytes | uint32_t |
| padding   | rest    | zero     |

### `0x03` — Data (data payload)

| Field        | Size            | Type     |
|--------------|-----------------|----------|
| `block_num`  | 4 bytes         | uint32_t |
| `block_data` | FSH_DATA_LENGTH | raw data |

## Session

- **Sender** announces the file (name, size, MD5) until a receiver locks on, then streams
  the requested DATA blocks.
- **Receiver** locks to the first valid announce (`tx_id`), preallocates the file, and
  re-requests the missing block range on timeout. It writes each block once (duplicates
  ignored) and, when all blocks are in, computes the MD5 and compares it to the announced
  hash.
- Lost or corrupted packets are simply re-requested, so the transfer converges.

## Files

- `share.c` / `share.h` — shared file-transfer engine (byte-identical across the new
  Flipper Share apps).
- `uart_framing.c/.h` — pure-C COBS codec, no furi includes, host-testable.
- `uart_transport.c/.h` — HAL glue: port lifecycle, the RX ISR and deframer worker, the
  blocking TX path.
- `share_config.h` — all tunables (baud, frame size, buffer sizes, timeouts, throughput
  estimate).
- `md5_hash.c/.h` — MD5 for the integrity check.
- `share_app.c/.h`, `scenes/share_scene_*.c` — app shell and the five UI scenes.
- `tools/framing_test.c` — host test harness for the COBS codec
  (`cc tools/framing_test.c uart_framing.c`). Kept out of the FAP via
  `sources=["*.c*", "!tools"]`.

## Status

- Builds warning-clean with `ufbt` (SDK release 1.4.3 / API 87.1) and passes `APPCHK`, so
  all imports resolve on unmodified official firmware.
- The COBS codec is validated on the host: `tools/framing_test.c` passes 20007/20007
  checks — fixed vectors (empty, 1-byte, 61-byte control, 521-byte DATA, all-`0x00`,
  all-`0xFF`), 10000 random round-trips through COBS plus a byte-stream deframer, 10000
  corrupted frames with zero silent leaks, and delimiter resync after a truncated frame.
- **Not yet bench-tested on two devices.** `FSH_PAYLOAD_THROUGHPUT_BPS` is the `20000`
  estimate from the line rate, not a measured value; the ETA shown in the UI is only as
  good as that constant. Replace it (and the numbers in this README) once a real transfer
  has been timed.

## Non-goals / v2 ideas

DMA RX (`furi_hal_serial_dma_rx_start`) for lower ISR load; baud negotiation inside
ANNOUNCE (a protocol-compatible extension); LPUART1 as a second port option; single-wire
half-duplex USART mode for a 2-wire link.

# Credits

Derived from Flipper Share. The UART transport is built on the Flipper firmware
`furi_hal_serial` API, all through the official external app API.
