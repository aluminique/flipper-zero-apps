# Flipper Share UART — implementation specification

Status: **specification, not implemented**. This document is a complete, self-contained
work order for implementing `flipper_share_uart` — direct file transfer between two
Flipper Zeros over a 3-wire UART link, using the Flipper Share v2 protocol. All design
decisions below are final; implement as written unless something is physically impossible,
and record any forced deviation in this README.

The app must build with `ufbt` against the official firmware SDK and run on unmodified
official firmware. Every firmware API named below is exported to FAPs (verified against
`targets/f7/api_symbols.csv`, API 87.x); re-verify against the current SDK before coding.

## 1. Goal and scope

- Transfer a file from one Flipper Zero to another over USART1 (GPIO header pins 13/14)
  at 230400 baud, full duplex.
- Reuse the Flipper Share v2 protocol engine from `../flipper_share_nfc/` unchanged in
  behavior: ANNOUNCE / REQUEST / DATA packets, CRC16, receiver-side block bitmap with
  resume, final MD5 verification.
- Expected effective throughput ~20 KB/s (line-rate bound); an 8 KB file transfers in
  well under a second, a 1 MB file in under a minute.

Out of scope (v2 candidates, do not implement): DMA RX, baud negotiation, LPUART1
support, single-wire half-duplex mode, flow control.

## 2. Wiring (user-facing)

| Flipper A | Flipper B |
|---|---|
| pin 13 (TX) | pin 14 (RX) |
| pin 14 (RX) | pin 13 (TX) |
| pin 8/11/18 (GND) | pin 8/11/18 (GND) |

Three jumper wires, TX/RX crossed. Document this table in the app's catalog description
and keep it in this README.

## 3. Architecture and naming (shared convention for all new transports)

New Flipper Share apps (`flipper_share_uart`, `flipper_share_ibutton`,
`flipper_share_rfid`) do NOT continue the per-app prefix pattern of the three legacy apps
(`fs_`/`ish_`/`nsh_`). Instead they share one neutral prefix, so the engine files stay
**byte-identical** across the new apps and fixes port by file copy. The three legacy apps
must not be modified.

Rules:

1. Engine files `share.c`, `share.h`, `md5_hash.c`, `md5_hash.h` are derived from
   `../flipper_share_nfc/nfc_share.c`, `nfc_share.h`, `md5_hash.c`, `md5_hash.h` by a
   mechanical rename: `nsh_` → `fsh_`, `NSH_` → `FSH_`, `NfcShare` → `Share`,
   `nfc_share` → `share`. Engine log TAG becomes `"FShare"`.
2. All transport-tunable constants are MOVED out of the engine into a new per-app header
   `share_config.h` (see section 6). `share.h` does `#include "share_config.h"` at the
   top and defines none of those constants itself.
3. **If `flipper_share_ibutton/` or `flipper_share_rfid/` already exists in this repo,
   copy `share.c`, `share.h`, `md5_hash.c`, `md5_hash.h` from it verbatim instead of
   redoing the rename.** After this task, the engine files must be byte-identical across
   all new apps that exist. (Exception: `#ifdef FSH_CAROUSEL` guarded blocks added by the
   RFID app are part of the canonical engine; keep them — they compile out here.)
4. App shell files are renamed the same way and are also transport-neutral:
   `share_app.c/.h` (entry point symbol `share_app`), `scenes/share_scene_*.c/.h`.
   UI strings that named the transport ("Send via NFC" etc.) must use the
   `FSH_TRANSPORT_NAME` macro from `share_config.h` instead of a literal.
5. Per-app files (the only ones that differ between new apps): `application.fam`, the
   icon, `share_config.h`, and the transport layer (here: `uart_transport.*`,
   `uart_framing.*`).

The engine ↔ transport contract (same as the NFC app, keep it):

- Engine calls `cb_send_bytes(buf, len)` → wired to `uart_transport_send()`.
- Transport delivers each complete received packet via
  `void fsh_receive_callback(const uint8_t* buf, size_t size)` (declared in `share.h`),
  from a thread context (never from ISR).
- Scene `on_enter`/`on_exit` call `uart_transport_init()` / `uart_transport_deinit()`.

## 4. Files to produce

```
flipper_share_uart/
├── application.fam
├── uart_share.png            # 10x10 1-bit icon; copy ../flipper_share_nfc/nfc_share.png as placeholder
├── README.md                 # this file, updated with measured numbers after bench
├── share.h / share.c         # engine (rename of nfc_share.*, constants moved to config)
├── share_config.h            # all tunables, section 6
├── md5_hash.h / md5_hash.c   # verbatim copy
├── share_app.h / share_app.c
├── scenes/share_scene_*.c/.h # menu, file_browser, show_file, send, receive
├── uart_transport.h / uart_transport.c
├── uart_framing.h / uart_framing.c    # pure COBS codec, no furi includes (host-testable)
├── tools/framing_test.c      # host test harness, see section 9
└── .github/workflows/build.yml        # copy from ../flipper_share_ir/
```

`application.fam`:

```python
App(
    appid="flipper_share_uart",
    name="Flipper Share UART",
    apptype=FlipperAppType.EXTERNAL,
    entry_point="share_app",
    stack_size=2 * 1024,
    fap_category="GPIO",
    fap_version="0.1",
    fap_icon="uart_share.png",
    fap_description="Direct file transfer between flippers via UART (pins 13/14)",
    fap_author="@lomalkin",
    fap_weburl="https://github.com/lomalkin/flipper-zero-apps/blob/-/flipper_share_uart",
)
```

## 5. Transport design

### 5.1 Port lifecycle

Firmware APIs (all exported): `furi_hal_serial_control_acquire/release/is_busy`
(`targets/f7/furi_hal/furi_hal_serial_control.h`), `furi_hal_serial_init/tx/
tx_wait_complete/async_rx_start/async_rx_stop/async_rx_available/async_rx`
(`targets/f7/furi_hal/furi_hal_serial.h`), `expansion_disable/expansion_enable`
(`applications/services/expansion/expansion.h`).

`bool uart_transport_init(void)` — called from the send/receive scene `on_enter`:

1. `Expansion* expansion = furi_record_open(RECORD_EXPANSION); expansion_disable(expansion);`
   — MANDATORY before acquiring the port, and required by the expansion header's own
   contract. Keep the record open for the app's lifetime. Reference pattern:
   firmware `applications/main/gpio/gpio_app.c`.
2. If `furi_hal_serial_control_is_busy(FuriHalSerialIdUsart)` → cleanup, return false.
3. `handle = furi_hal_serial_control_acquire(FuriHalSerialIdUsart)`; if NULL → cleanup,
   return false. (Acquiring also detaches any log output from this port automatically.)
4. `furi_hal_serial_init(handle, UART_TP_BAUD)`.
5. Allocate RX `FuriStreamBuffer` (size `UART_TP_RX_STREAM_SIZE`, trigger 1), start the
   deframer thread `UartRxWorker` (stack 2048), then
   `furi_hal_serial_async_rx_start(handle, rx_isr_cb, ctx, false)`.

On `false`, the scene must show "UART port busy" in its status line and not start the
protocol worker; the user backs out with the Back button.

`void uart_transport_deinit(void)` — reverse order: `furi_hal_serial_async_rx_stop`,
stop+join the worker thread, `furi_hal_serial_deinit`, `furi_hal_serial_control_release`,
`expansion_enable(expansion)`, `furi_record_close(RECORD_EXPANSION)`, free buffers.
The caller must have already stopped every thread that can call `uart_transport_send()`
(same rule as the NFC transport).

### 5.2 Framing: COBS over the byte stream

UART gives a byte stream; packets need delimiting. Use COBS (Consistent Overhead Byte
Stuffing) with `0x00` as the frame delimiter:

- On the wire: `COBS_encode(packet) + 0x00`. Encoded data never contains `0x00`.
- Encoder/decoder live in `uart_framing.c/.h` as pure C (no furi headers): 
  `size_t uart_cobs_encode(const uint8_t* in, size_t len, uint8_t* out);`
  `size_t uart_cobs_decode(const uint8_t* in, size_t len, uint8_t* out);` (returns 0 on
  malformed input). Worst-case encoded size = `len + len/254 + 1`; add
  `_Static_assert(UART_TP_FRAME_MAX >= FSH_PACKET_MAX + FSH_PACKET_MAX / 254 + 2, ...)`
  in `uart_transport.c`.
- Desync recovery is inherent: after any corruption, the decoder drops bytes until the
  next `0x00` and resynchronizes. A corrupted frame either fails COBS decode or fails the
  engine's length/CRC16 validation — both are silent drops; the protocol ARQ re-requests.

### 5.3 RX path

`rx_isr_cb` (ISR context, per the HAL header warning): while
`furi_hal_serial_async_rx_available(handle)` → read one byte with
`furi_hal_serial_async_rx(handle)` → `furi_stream_buffer_send(..., 0)`. No other work in
the ISR. (Same pattern as firmware `applications/debug/uart_echo/uart_echo.c`.)

`UartRxWorker` thread: blocking-read bytes from the stream buffer, accumulate into a
frame buffer until `0x00`, COBS-decode, and call `fsh_receive_callback(packet, len)`.
Oversized frames (> `UART_TP_FRAME_MAX` without a delimiter) are discarded up to the next
delimiter. Thread exits on a `FSH_WORKER_STOP_FLAG` thread flag; use
`furi_stream_buffer_receive` with a timeout of `UART_TP_RX_POLL_MS` so the flag is polled.

### 5.4 TX path

`void uart_transport_send(const uint8_t* buf, size_t len)`:
COBS-encode into a static TX buffer, append `0x00`, then under a `FuriMutex`:
`furi_hal_serial_tx(handle, frame, frame_len); furi_hal_serial_tx_wait_complete(handle);`.
Blocking TX is intentional — at 230400 baud a 521-byte DATA packet occupies ~23 ms and
this back-pressure naturally paces the engine's send loop; no mailbox is needed (unlike
NFC). If the transport is not running, drop the packet silently (ARQ recovers).

### 5.5 Duplex notes

The link is full duplex and symmetric; both sides run RX permanently. There is no
turnaround, no collision, and no jitter logic — the half-duplex constants in
`share_config.h` are set accordingly (zero jitter, small timeouts).

## 6. `share_config.h` — all constants

| Constant | Value | Rationale |
|---|---|---|
| `FSH_TRANSPORT_NAME` | `"UART"` | UI strings ("Send via " FSH_TRANSPORT_NAME ...) |
| `FSH_DATA_LENGTH` | `512` | file bytes per DATA packet → 521-byte packet, ~23 ms of line time |
| `FSH_ANNOUNCE_INTERVAL_MS` | `1000` | fast lock; line is cheap |
| `FSH_ANNOUNCE_CONNECTED_MS` | `3000` | keep as NFC |
| `FSH_RX_TIMEOUT_MS` | `200` | REQUEST retry; RTT is milliseconds |
| `FSH_TX_TIMEOUT_MS` | `5` | full duplex, no turnaround needed |
| `FSH_IDLE_TICK_MS` | `5` | engine tick; TX pacing comes from blocking send |
| `FSH_CONNECTED_IDLE_MS` | `5000` | keep as NFC |
| `FSH_REQUEST_JITTER_MS` | `0` | no collisions on full duplex |
| `FSH_PAYLOAD_THROUGHPUT_BPS` | `20000` | ETA estimate; REPLACE with measured value after bench |
| `FSH_STALL_MS` | `3000` | stall indicator |
| `UART_TP_BAUD` | `230400` | proven in firmware `uart_echo`; raise only after bench |
| `UART_TP_FRAME_MAX` | `528` | encoded 521-byte packet + COBS overhead + delimiter |
| `UART_TP_RX_STREAM_SIZE` | `2048` | ~4 max frames of headroom |
| `UART_TP_RX_POLL_MS` | `25` | worker stop-flag poll period |

Engine constants that do not depend on the transport (`FSH_HASH_CHUNK_SIZE`,
`FSH_PARTS_COUNT`, `FSH_ETA_*`, packet structure sizes) stay in `share.h`.

## 7. Scenes / UI

Five scenes as in the NFC app, renamed per section 3. Changes beyond the rename:

- All "via NFC" strings → `FSH_TRANSPORT_NAME`.
- Send/receive scenes: on `uart_transport_init() == false`, show "UART port busy" and do
  not start the worker (section 5.1). No `stop_field()` equivalent exists here; drop
  those calls.
- Add one static hint line to the send and receive scenes' idle state: `"13-14 X, GND-GND"`
  (wiring reminder).

## 8. Edge cases

- **Wire disconnected mid-transfer**: receiver keeps re-REQUESTing on `FSH_RX_TIMEOUT_MS`,
  sender keeps announcing at `FSH_ANNOUNCE_CONNECTED_MS`; on reconnect the block bitmap
  resumes. No transport code needed — verify in bench.
- **Noise / garbage on the line**: COBS resync + engine CRC16 + final MD5. No transport
  CRC is added (packet CRC16 is sufficient).
- **Port busy** (CLI on another channel is fine — CLI uses USB CDC; but another app or
  expansion module may hold USART): handled via `is_busy`/NULL-acquire → UI message.
- **TX during deinit**: forbidden by contract (caller stops protocol worker first);
  `uart_transport_send` also checks a `running` flag under the TX mutex.
- **Both sides same role**: two senders announce into each other — engine ignores
  unexpected packet types by design; nothing to do.

## 9. Testing

Host tests (no hardware):
- `tools/framing_test.c`: compile with `cc tools/framing_test.c uart_framing.c` (pattern:
  `../flipper_share_ir/tools/modem_test.c`). Round-trip: empty, 1-byte, 61-byte CTRL,
  521-byte DATA, all-0x00 payload, all-0xFF payload, random payloads ×10000; corrupt
  random bytes and verify decode failure or engine-level rejection; verify resync after a
  truncated frame. Zero failures required.

Bench (two Flippers, official firmware, app installed via `ufbt launch` / qFlipper):
1. Transfer an 8 KB file → completes, MD5 verdict OK on receiver.
2. Transfer a 1 MB file → completes; record wall-clock time; write the measured B/s into
   `FSH_PAYLOAD_THROUGHPUT_BPS` and into this README.
3. Pull the TX wire for ~5 s mid-transfer of the 1 MB file, reconnect → transfer resumes
   and completes with MD5 OK.
4. Start receiver before sender, and sender before receiver → both orders lock and
   complete.
5. Cancel on both sides mid-transfer → clean exit, no crash, expansion service restored
   (verify expansion settings menu still works after app exit).

Acceptance = all five bench points pass + host tests pass + `ufbt` build is warning-clean
(`-Werror` is on by default).

## 10. Non-goals / v2 ideas

DMA RX (`furi_hal_serial_dma_rx_start`) for lower ISR load; baud negotiation inside
ANNOUNCE (protocol-compatible extension); LPUART1 as a second port option; single-wire
half-duplex USART mode (2-wire link) via LL registers.

## 11. Implementation notes / forced deviations

Implemented as written except for the points below.

- **Engine copied verbatim from the RFID app (rule 3).** `share.c` / `share.h` /
  `md5_hash.*` are byte-identical to `flipper_share_rfid`'s engine, including the
  `#ifdef FSH_CAROUSEL` blocks — those compile out here because this app's
  `share_config.h` does not define `FSH_CAROUSEL`, so UART runs the classic
  ANNOUNCE/REQUEST/DATA flow unchanged. (`flipper_share_ibutton` shipped before the
  carousel blocks existed; its engine should get the same verbatim backport to complete
  the three-way byte-identity — not done here to leave that app's branch untouched.)
- **Engine TX symbol is the neutral `fsh_transport_send`** (mirror of `fsh_receive_callback`),
  not `uart_transport_send`, so the engine stays byte-identical across transports.
- **Modulo-by-zero guard carried in from the canonical engine.** `FSH_REQUEST_JITTER_MS`
  is 0 for this full-duplex link; the engine's re-request backoff reads the jitter into a
  local and guards `if(jitter)` before the modulo, so it builds clean under `-Werror` and
  never faults. (Introduced with the RFID app; part of the canonical engine.)
- **COBS codec validated on the host.** `tools/framing_test.c` (`cc tools/framing_test.c
  uart_framing.c`, `-Wall -Wextra -Werror`) passes 0/20007 checks: fixed vectors (empty,
  1-byte, 61-byte CTRL, 521-byte DATA, all-0x00, all-0xFF), 10000 random round-trips
  through COBS + a byte-stream deframer, 10000 corrupted frames (0 silent leaks — every
  corruption is a decode failure or a mismatch the engine CRC16 drops), and delimiter
  resync after a truncated frame.
- **API re-verified at SDK release 1.4.3 (API 87.1).** All `furi_hal_serial*`,
  `furi_hal_serial_control_*` and `expansion_*` symbols named in section 5 are exported.
  The RX interrupt only drains bytes into a stream buffer; the `UartRxWorker` thread
  deframes and decodes (delivering to `fsh_receive_callback` in thread context).

**Build status:** builds warning-clean with `ufbt` (SDK release 1.4.3 / API 87.1);
`APPCHK` passes on unmodified official firmware. `tools/` is kept out of the FAP via
`sources=["*.c*", "!tools"]` in `application.fam`.

**Bench:** not yet run (needs two devices + 3 jumper wires). `FSH_PAYLOAD_THROUGHPUT_BPS`
stays at the `20000` estimate until section 9 is executed.
