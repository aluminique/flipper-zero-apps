v0.1: EXPERIMENTAL: Flipper Share UART — file transfer over a 3-wire link
- New app derived from Flipper Share: the transport is USART1 on the GPIO header (pins 13/14, 230400 baud, full duplex), three jumper wires with TX/RX crossed and grounds common.
- COBS framing with a `0x00` delimiter; a corrupted frame either fails the decode or fails the packet CRC16, and the decoder resynchronizes on the next delimiter.
- Resumable: the block bitmap picks up where the link dropped; per-packet CRC16 and a whole-file MD5 check after reception.
- DATA block size raised to 512 bytes (521-byte packet, ~23 ms of line time); the blocking TX paces the engine, so no outbound mailbox is needed.
- Port lifecycle: the expansion service is disabled while the port is held and restored on exit; if USART1 is already busy the scene reports "UART port busy" instead of starting.
- COBS codec validated on the host (`tools/framing_test.c`, 20007 checks: fixed vectors, 10000 random round-trips, 10000 corrupted frames, resync after truncation).
- ~20 KB/s expected from the line rate — not yet bench-measured.
