# Flipper Share GPIO — direct file transfer between Flippers over a GPIO wire

> **⚠️ WARNING:** Flipper Share GPIO is an **experimental-only** app, it is not recommended for regular use.
> Consider using other Flipper Share transports (NFC, Sub-GHz, IR) for everyday file transfer.

## Overview

**Flipper Share GPIO** transfers any file directly from one Flipper Zero to another over a
plain **GPIO jumper wire** — no extra hardware, no external components, phone, computer,
internet or radio needed. Jumper pin 4 ↔ pin 4 and GND ↔ GND.

It is a one-way (carousel) link built on a small custom **pulse-distance modem**: the line
idles high, the sender marks each bit boundary with a short LOW tick (a fast falling edge) and
encodes the bit in the fall-to-fall interval, like an NEC IR remote. Only falling edges are
timed, so the slow open-drain rise (internal pull-up, no external resistor) never matters. The
sender broadcasts announce+blocks round-robin; the receiver's block bitmap fills in over passes.

Transfer speed is roughly **4–5 KB/s** from the timing budget (bench measurement pending).

Other Flipper Share transports (Sub-GHz, IR, NFC & more): [github.com/lomalkin/flipper-zero-apps](https://github.com/lomalkin/flipper-zero-apps)

Features:

- Works out of the box on any Flipper Zero — two GPIO header pins, one jumper wire plus a
  ground wire.
- Integrity check with an MD5 hash after reception; per-packet CRC16.
- Resumes automatically: unplug and reconnect the wire mid-transfer and the receiver's block
  bitmap picks up the missing blocks on a later pass.
- Torrent-like progress bar on the receiver; filename/size and ETA on the sender.

A brief wire interruption is harmless — the receiver resumes on the next carousel pass.
Received files are saved to **/ext/inbox/**.

# Notes

See the full [README.md](https://github.com/lomalkin/flipper-zero-apps/blob/-/flipper_share_gpio/README.md) for the pulse-distance modem and protocol description.

Source code of the latest version is [here](https://github.com/lomalkin/flipper-zero-apps/blob/-/flipper_share_gpio). Please feel free to open issues and PRs.

# Credits

Derived from Flipper Share. The pulse-distance modem and one-way GPIO transport are original,
all through the official external app API.
