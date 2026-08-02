# Flipper Share UART — direct file transfer between Flippers over a 3-wire link

> **⚠️ WARNING:** Flipper Share UART is an **experimental-only** app, it is not recommended for regular use.
> Consider using other Flipper Share transports (NFC, Sub-GHz, IR) for everyday file transfer.

## Overview

**Flipper Share UART** transfers any file directly from one Flipper Zero to another over a
**3-wire UART link** on the GPIO header (USART1, pins 13/14, 230400 baud, full duplex) —
no phone, computer, internet or radio needed, just three jumper wires.

Expected transfer speed is around **20 KB/s** — line-rate bound, by far the fastest
Flipper Share transport, but the only one that needs wires. This is an estimate from the
baud rate, not yet a bench measurement.

**Wiring** — three jumper wires, TX/RX crossed, grounds common:

| Flipper A | Flipper B |
|---|---|
| pin 13 (TX) | pin 14 (RX) |
| pin 14 (RX) | pin 13 (TX) |
| pin 8/11/18 (GND) | pin 8/11/18 (GND) |

Other Flipper Share transports (Sub-GHz, IR, NFC & more): [github.com/lomalkin/flipper-zero-apps](https://github.com/lomalkin/flipper-zero-apps)

Features:

- No extra hardware beyond three jumper wires — no adapter, no level shifter.
- Integrity check with an MD5 hash after reception; per-packet CRC16.
- Resumes automatically: pull a wire mid-transfer and reconnect, and the receiver's block
  bitmap picks up where it left off.
- Full duplex and symmetric — no turnaround, no collisions.
- Torrent-like progress bar on the receiver; filename/size and ETA on the sender.

The app takes over USART1 while a transfer scene is open (the expansion service is disabled
and restored on exit). If another app already holds the port, the scene shows "UART port
busy" — back out with the Back button. The CLI is unaffected; it runs over USB CDC.
Received files are saved to **/ext/inbox/**.

# Notes

See the full [README.md](https://github.com/lomalkin/flipper-zero-apps/blob/-/flipper_share_uart/README.md) for the UART transport and protocol description.

Source code of the latest version is [here](https://github.com/lomalkin/flipper-zero-apps/blob/-/flipper_share_uart). Please feel free to open issues and PRs.

# Credits

Derived from Flipper Share. The UART transport is built on the Flipper firmware
`furi_hal_serial` API, all through the official external app API.
