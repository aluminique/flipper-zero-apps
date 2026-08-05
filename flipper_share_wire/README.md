# Flipper Share Wire — direct file transfer between Flippers over a GPIO wire

> **⚠️ WARNING:** Flipper Share Wire is an **experimental-only** app, it is not recommended for regular use.
> Consider using other Flipper Share transports (NFC, Sub-GHz, IR) for everyday file transfer.

## Overview

**Flipper Share Wire** transfers any file directly from one Flipper Zero to another over a
plain **GPIO jumper wire** — no extra hardware, phone, computer, internet or radio needed.
Jumper **pin 4 ↔ pin 4** and **GND ↔ GND**, and the transfer runs.

It is the generic-wire sibling of Flipper Share iButton: the same resumable,
integrity-checked file-transfer protocol and the same 1-Wire host/slave transport, but the
bus is an ordinary GPIO (PA4) instead of the iButton pad. On a plain GPIO there is no pad
pull-up divider to fight, so the link is more predictable — the host just drives the pull-up
from the STM32's internal resistor.

Actual transfer speed is around **1.2 KB/s** (bench-measured; e.g. 8 KB in ~7 s).

Other Flipper Share transports (Sub-GHz, IR, NFC & more): [github.com/lomalkin/flipper-zero-apps](https://github.com/lomalkin/flipper-zero-apps)

Features:

- Works out of the box on any Flipper Zero — two GPIO pins on the header, one jumper wire
  plus a ground wire. Builds with `ufbt` against the official firmware; no firmware
  modification.
- Integrity check with an MD5 hash after reception; per-packet CRC16.
- Automatic retransmission of lost/corrupted packets — the transfer continues "until
  success". Unplugging and reconnecting the wire mid-transfer resumes where it left off.
- Half-duplex command/response link: the receiver drives the bus (1-Wire host), the sender
  answers as a 1-Wire slave (emulator).
- Torrent-like progress bar on the receiver; filename/size and ETA on the sender.

# Usage

1. Wire **pin 4 ↔ pin 4** and **GND ↔ GND** between the two Flippers (any GND pin).
2. On the receiving Flipper: open Flipper Share Wire → **Receive via Wire**.
3. On the sending Flipper: open Flipper Share Wire → **Send via Wire** → pick a file → **OK**.
4. Hold the connection until it completes. The receiver shows a progress bar and verifies
   the MD5 at the end; the file is saved to `/ext/inbox/`.

The sender shows the file name, size and a rough ETA. The receiver shows
"Waiting for announce..." until it locks, then the progress bar with percentage and ETA.
A brief wire interruption is harmless — every transaction starts with a 1-Wire
reset/presence pulse, so the link resynchronizes by itself.

---

# Flipper Share Wire protocol

Two layers: a **1-Wire transport** (physical/link layer) under the existing
**file-transfer protocol** (selective-repeat ARQ). The file-transfer protocol is identical
to the other Flipper Share builds; only the transport differs.

## Physical / link layer — the 1-Wire transport

- **Bus:** standard-speed 1-Wire on `gpio_ext_pa4` (PA4 — header pin 4). One slot ≈ 73 µs/bit
  → ~0.6 ms/byte. PA4 is a plain GPIO with a free EXTI line (needed by the slave emulator)
  and no on-board analog circuitry, so it is a clean point-to-point bus. Overdrive mode is
  not used: the slave side is a software bit-banger in a critical section.
- **Role mapping:** the **receiver** is the 1-Wire **host** — it drives the bus and owns all
  timing; the **sender** is the 1-Wire **slave** (emulator) and answers in read slots. This
  matches the other transports, where the sender is the passive side (NFC listener, RFID tag).
- **Deterministic host timing:** each reset/read/write runs inside a short `FURI_CRITICAL`
  section on the host, so a busy USB stack cannot stretch a 1-Wire slot and corrupt the byte.
- **Transactions:** the host drives every exchange as
  `reset → presence → command byte → payload`, using two custom commands:

  | Command | Direction after command byte | Payload |
  |---|---|---|
  | `WIRE_TP_CMD_POLL` `0xA1` | slave → host | `len(1)` + `len` packet bytes; `len = 0x00` means "nothing queued" |
  | `WIRE_TP_CMD_PUSH` `0xA2` | host → slave | `len(1)` + `len` packet bytes |

  `len` must be `1..FSH_PACKET_MAX`; anything else aborts the transaction on both sides and
  the next reset resynchronizes. The command codes deliberately avoid the standard 1-Wire ROM
  commands (`0x33` READ ROM, `0xCC` SKIP ROM, `0xF0` SEARCH ROM, …), so a foreign 1-Wire
  reader touching the sender gets nothing. There is no ROM search or addressing — this is a
  point-to-point link with exactly two devices.
- **No link-layer CRC:** integrity is the packet's own CRC16 (checked by the engine) plus the
  whole-file MD5. A corrupted transaction either fails in the 1-Wire driver or is dropped on
  CRC16 — both silent, and the ARQ re-requests.
- **Outbound mailbox:** the engine's `fsh_transport_send` enqueues packets into a 4-deep queue
  for DATA, plus a single latest-wins slot for control packets (ANNOUNCE / REQUEST) that is
  drained first, so a DATA stream can never starve control traffic. A full data queue blocks
  the sender for up to `WIRE_TP_SEND_TIMEOUT_MS`, which is the natural backpressure that paces
  the engine.
- **Resume:** if the wire is interrupted, the host simply sees no presence pulse and retries
  every `WIRE_TP_RECONNECT_MS`. On reconnect, the receiver's block bitmap re-requests only the
  missing blocks, so the transfer continues where it stopped.
- **ISR discipline:** the slave's command/reset callbacks run in the GPIO EXTI interrupt inside
  a `FURI_CRITICAL` section, so those paths use only zero-timeout `furi_message_queue_put/get`
  (which route to the `*FromISR` variants) and a `FURI_CRITICAL`-guarded control slot — no
  mutexes, no storage I/O, nothing blocking. Received frames are handed to a worker thread,
  which calls `fsh_receive_callback` in thread context, exactly as the engine expects.

## Timing budget

One DATA transaction is reset+presence (~1 ms) + command (1 B) + length (1 B) + packet (73 B)
≈ 45 ms, plus the `WIRE_TP_POLL_INTERVAL_MS` gap → ~19 packets/s × 64 payload bytes
≈ **1.2 KB/s**, which matches the bench.

The slave services each transaction inside interrupt/critical context (~45 ms per DATA
frame), which is this transport's main systemic constraint. `FSH_DATA_LENGTH` is kept at 64
for that reason; raising it to 128 is a pure config change once a long transfer is confirmed
healthy.

## Packet structure

Every packet: `[version(1)][tx_id(1)][packet_type(1)][payload][crc16(2)]`. The payload length
depends on the type.

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

- **Sender** announces the file (name, size, MD5) until a receiver locks on, then serves the
  requested DATA blocks in its POLL responses.
- **Receiver** locks to the first valid announce (`tx_id`), preallocates the file, and
  re-requests the missing block range on timeout. It writes each block once (duplicates
  ignored) and, when all blocks are in, computes the MD5 and compares it to the announced hash.
- Lost or corrupted packets are simply re-requested, so the transfer converges.

## Files

- `share.c` / `share.h` — shared file-transfer engine (byte-identical across the new Flipper
  Share apps).
- `wire_transport.c/.h` — 1-Wire glue: the host worker loop, the slave callbacks, the outbound
  mailbox and the RX worker.
- `share_config.h` — all tunables (bus pin, command codes, poll/reconnect timings, packet size,
  throughput estimate).
- `md5_hash.c/.h` — MD5 for the integrity check.
- `share_app.c/.h`, `scenes/share_scene_*.c` — app shell and the UI scenes.

# Credits

Derived from Flipper Share. The 1-Wire transport is built on the Flipper firmware `one_wire`
host/slave API, all through the official external app API.
