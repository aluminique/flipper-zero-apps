# Flipper Share GPIO — direct file transfer between Flippers over a GPIO wire

> **⚠️ WARNING:** Flipper Share GPIO is an **experimental-only** app, it is not recommended for regular use.
> Consider using other Flipper Share transports (NFC, Sub-GHz, IR) for everyday file transfer.

## Overview

**Flipper Share GPIO** transfers any file directly from one Flipper Zero to another over a
plain **GPIO jumper wire** — no extra hardware, no external components, no phone, computer,
internet or radio needed. Jumper **pin 4 ↔ pin 4** and **GND ↔ GND**, and the transfer runs.

It is a one-way (carousel) link built on a small custom **pulse-distance modem**. The line
idles high; the sender marks every bit boundary with a short LOW "tick" — a fast, actively
driven falling edge — and encodes each bit in the **time between consecutive falling edges**
(short = 0, long = 1, very long = frame start), exactly like an NEC infrared remote. The
receiver only ever timestamps falling edges, so the slow rise of an open-drain wire (there is
no external pull-up, only the chip's internal ~40 kΩ) is never on the critical path.

Transfer speed is roughly **4–5 KB/s** from the modem timing budget (bench measurement
pending). The high-level file-transfer protocol — resumable, integrity-checked — is the same
as the other Flipper Share builds.

Other Flipper Share transports (Sub-GHz, IR, NFC & more): [github.com/lomalkin/flipper-zero-apps](https://github.com/lomalkin/flipper-zero-apps)

Features:

- Works out of the box on any Flipper Zero — two GPIO header pins, one jumper wire plus a
  ground wire. Builds with `ufbt` against the official firmware; no firmware modification.
- Integrity check with an MD5 hash after reception; per-packet CRC16.
- Resumes automatically: unplug and reconnect the wire mid-transfer and the receiver's block
  bitmap picks up the missing blocks on a later carousel pass.
- One-way on the wire (carousel): the sender broadcasts continuously, the receiver never
  transmits — so there is no handshake and nothing to get out of sync.
- Torrent-like progress bar on the receiver; filename/size and ETA on the sender.

# Usage

1. Wire **pin 4 ↔ pin 4** and **GND ↔ GND** between the two Flippers (any GND pin).
2. On the receiving Flipper: open Flipper Share GPIO → **Receive via GPIO**.
3. On the sending Flipper: open Flipper Share GPIO → **Send via GPIO** → pick a file → **OK**.
4. Hold the connection until it completes. The receiver shows a progress bar and verifies the
   MD5 at the end; the file is saved to `/ext/inbox/`.

The sender shows the file name, size and a rough ETA. The receiver shows
"Waiting for announce..." until it locks, then the progress bar with percentage and ETA. A
brief wire interruption is harmless — the receiver simply resumes on the next carousel pass.

---

# Flipper Share GPIO protocol

Two layers: a **pulse-distance modem** (physical/link layer) under the existing
**file-transfer protocol**, run in a one-way **carousel** mode. The file-transfer protocol is
identical to the other Flipper Share builds; only the transport differs.

## Why this design (the physics)

A single open-drain wire pulled up only by the STM32's internal ~40 kΩ resistor has a strongly
**asymmetric** edge behaviour: the **falling** edge is fast (actively driven to ground in well
under a microsecond) while the **rising** edge is slow (an RC ramp of several microseconds
through the weak pull-up). A faster bidirectional scheme (1-Wire overdrive) fails here because
it must sample the bus right after a rising edge, inside a ~1–3 µs window the slow ramp cannot
meet. This modem sidesteps that entirely: **all information is carried on falling edges**, and
the slow rise only has to finish somewhere inside the following gap — it is never timed.

## Physical / link layer — the pulse-distance modem (`gpio_modem.*`)

- **Idle:** the line rests high (receiver pull-up enabled; the sender drives it open-drain and
  releases between ticks).
- **Tick:** the sender pulls the line low for `GPIO_MODEM_TICK_US` (~3 µs) — a clean falling
  edge — then releases it.
- **Symbol = fall-to-fall interval:** the bit is the time from one falling edge to the next:

  | Interval | Meaning |
  |---|---|
  | ~13 µs (`GPIO_MODEM_BIT0_US`) | data bit `0` |
  | ~23 µs (`GPIO_MODEM_BIT1_US`) | data bit `1` |
  | ~38 µs (`GPIO_MODEM_SYNC_US`) | frame-start SYNC |
  | ≥ ~52 µs | inter-frame idle / invalid → resync |

  The decoder classifies each interval into one of these wide bands (~5 µs of slack on every
  threshold), so ISR jitter has little to bite on. There is **no modem CRC** — the engine's
  packet CRC16 and the whole-file MD5 do the filtering; a mangled frame is dropped and the
  carousel re-sends it.
- **Frame:** `SYNC` then `[len]` and the packet bytes, each byte LSB-first, one interval per
  bit. `len` is the flipper-share packet length; a bogus length (a noise false-lock) is
  rejected immediately and the decoder returns to hunting for the next SYNC.
- **Sender timing:** the frame is bit-banged on a **high-priority thread** from the
  deterministic DWT cycle counter, with every falling edge pinned to an absolute point on the
  time grid so jitter cannot accumulate across a frame. It is **not** done inside a critical
  section, so interrupts (Bluetooth/USB) stay serviced and the system stays healthy; a rare
  preemption only stretches the one edge it lands on, costing at most one re-sent frame.
- **Receiver:** a falling-edge EXTI interrupt timestamps each edge and hands the interval to a
  worker thread, which feeds the decoder and passes completed packets to a separate delivery
  thread (so the engine's storage I/O never stalls the decode path).

## One-way carousel

The single wire has no return channel, so the engine runs in **carousel** mode
(`FSH_CAROUSEL`, the same mode the RFID app uses):

- The **sender** broadcasts unconditionally: every `RFID_CAROUSEL_ANNOUNCE_EVERY`-th frame is
  an ANNOUNCE (file name, size, MD5); the rest walk the file's DATA blocks round-robin.
- The **receiver** locks to the first ANNOUNCE, preallocates the file, and writes each DATA
  block once (duplicates ignored). Blocks missed on one pass are picked up on a later pass —
  the block bitmap converges the transfer without any back-channel. When every block is in, it
  computes the MD5 and compares it to the announced hash.

## Timing budget

One 64-byte DATA packet is 73 bytes → `(1 + 73) × 8 = 592` bit intervals plus a SYNC, at
~18 µs average → a ~11 ms frame. With one ANNOUNCE every 4 frames, ~192 payload bytes go out
per ~41 ms → **~4.5 KB/s** (estimate; `FSH_PAYLOAD_THROUGHPUT_BPS` is replaced with the
measured rate after a bench run). The sender bit-bangs each frame back-to-back; the transport's
single-slot outbound mailbox provides the backpressure that paces the engine's carousel loop.

## Packet structure

Every packet: `[version(1)][tx_id(1)][packet_type(1)][payload][crc16(2)]`. The payload length
depends on the type.

### `0x01` — Announce (control payload)

| Field       | Size     | Type                  |
|-------------|----------|-----------------------|
| `file_name` | 36 bytes | char[36], zero-padded |
| `file_size` | 4 bytes  | uint32_t              |
| `file_hash` | 16 bytes | MD5                   |

### `0x03` — Data (data payload)

| Field        | Size            | Type     |
|--------------|-----------------|----------|
| `block_num`  | 4 bytes         | uint32_t |
| `block_data` | FSH_DATA_LENGTH | raw data |

(The `0x02` Request packet exists in the engine but is unused here — the carousel receiver
never transmits.)

## Files

- `share.c` / `share.h` — shared file-transfer engine (byte-identical across the new Flipper
  Share apps; the carousel path is behind `#ifdef FSH_CAROUSEL`).
- `gpio_modem.c/.h`, `gpio_modem_config.h` — the pulse-distance modem: pure C, no firmware
  dependencies, so it round-trips on the host test harness.
- `tools/modem_test.c` — host test harness (`cc … modem_test.c gpio_modem.c`), 4078 checks.
- `gpio_transport.c/.h` — hardware glue: the sender bit-bang worker, the receiver EXTI capture
  and the decode/delivery workers.
- `share_config.h` — all tunables (bus pin, carousel cadence, mailbox depths, throughput
  estimate); the modem timings live in `gpio_modem_config.h`.
- `md5_hash.c/.h` — MD5 for the integrity check.
- `share_app.c/.h`, `scenes/share_scene_*.c` — app shell and the UI scenes.

# Credits

Derived from Flipper Share. The pulse-distance modem and one-way GPIO transport are original;
everything runs through the official external app API, no firmware modification.
