# Flipper Share Wire — direct file transfer between Flippers over a GPIO wire

> **⚠️ WARNING:** Flipper Share Wire is an **experimental-only** app, it is not recommended for regular use.
> Consider using other Flipper Share transports (NFC, Sub-GHz, IR) for everyday file transfer.

## Overview

**Flipper Share Wire** transfers any file directly from one Flipper Zero to another over a
plain **GPIO jumper wire** — no extra hardware, phone, computer, internet or radio needed.
Jumper pin 4 ↔ pin 4 and GND ↔ GND. The receiver drives the bus as a 1-Wire host; the
sender answers as a 1-Wire slave.

It is the generic-wire sibling of Flipper Share iButton: the same protocol, but the bus is
an ordinary GPIO (PA4) instead of the iButton pad, so there is no pad pull-up divider and
the link is more predictable.

Actual transfer speed is around **1.2 KB/s** (bench-measured; e.g. 8 KB in ~7 s).

Other Flipper Share transports (Sub-GHz, IR, NFC & more): [github.com/lomalkin/flipper-zero-apps](https://github.com/lomalkin/flipper-zero-apps)

Features:

- Works out of the box on any Flipper Zero — two GPIO header pins, one jumper wire plus a
  ground wire.
- Integrity check with an MD5 hash after reception; per-packet CRC16.
- Resumes automatically: unplug and reconnect the wire mid-transfer and the receiver's block
  bitmap picks up where it left off.
- Torrent-like progress bar on the receiver; filename/size and ETA on the sender.

A brief wire interruption is harmless — every transaction starts with a 1-Wire
reset/presence pulse, so the link resynchronizes by itself. Received files are saved to
**/ext/inbox/**.

# Notes

See the full [README.md](https://github.com/lomalkin/flipper-zero-apps/blob/-/flipper_share_wire/README.md) for the 1-Wire transport and protocol description.

Source code of the latest version is [here](https://github.com/lomalkin/flipper-zero-apps/blob/-/flipper_share_wire). Please feel free to open issues and PRs.

# Credits

Derived from Flipper Share. The 1-Wire transport is built on the Flipper firmware
`one_wire` host/slave API, all through the official external app API.
