v0.1: EXPERIMENTAL: Flipper Share Wire — file transfer over a GPIO 1-Wire link
- New app derived from Flipper Share: the transport is a 1-Wire host/slave pair on a plain GPIO wire (`gpio_ext_pa4`, PA4 — header pin 4), jumpered pin 4 <-> pin 4 plus GND, built on the firmware `one_wire` API.
- Generic-wire sibling of Flipper Share iButton: same protocol, but the bus is an ordinary GPIO instead of the iButton pad, so there is no pad pull-up divider and the host supplies the pull-up from the STM32 internal resistor.
- Role mapping: the receiver drives the bus as the 1-Wire host, the sender answers as a slave (emulator); two custom commands (POLL `0xA1` / PUSH `0xA2`) carry one flipper-share packet per transaction.
- Deterministic host timing: each reset/read/write runs inside a short critical section, so a busy USB stack cannot stretch a 1-Wire slot and corrupt the byte.
- Resumable: the block bitmap picks up where the wire was interrupted; per-packet CRC16 and a whole-file MD5 check after reception.
- Control traffic (ANNOUNCE / REQUEST) has priority over DATA in the transport mailbox, so a DATA stream cannot starve it.
- Custom command codes avoid the standard 1-Wire ROM commands, so a foreign 1-Wire reader touching the sender gets nothing.
- Standard-speed slots; ~1.2 KB/s on the bench.
