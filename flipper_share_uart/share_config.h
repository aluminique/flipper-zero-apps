#pragma once

// Per-app tunables for Flipper Share over a 3-wire UART link (USART1, pins 13/14).
// This is the single place to tweak the transport and the transport-dependent
// engine knobs; changing anything here requires a recompile. The engine
// (share.c/share.h) and the shared scenes read these but define none of them, so
// the engine files stay byte-identical across all new Flipper Share apps.

// ===== Engine-facing tunables (consumed by share.c / the scenes) =============

// Name of this transport, substituted into every UI string ("Send via ...").
#define FSH_TRANSPORT_NAME "UART"

// File-data bytes per DATA packet. 512 -> a 521-byte packet, ~23 ms of line time
// at 230400 baud, which back-pressures the engine's send loop nicely.
#define FSH_DATA_LENGTH 512u

// Announce cadence: fast lock, the line is cheap.
#define FSH_ANNOUNCE_INTERVAL_MS 1000u
#define FSH_ANNOUNCE_CONNECTED_MS 3000u

// Receiver re-request timeout (RTT is milliseconds) and sender post-RX gap.
#define FSH_RX_TIMEOUT_MS 200u
#define FSH_TX_TIMEOUT_MS 5u // full duplex, no turnaround needed

// Engine tick period (TX pacing comes from the blocking send) and CONNECTED-idle
// revert.
#define FSH_IDLE_TICK_MS 5u
#define FSH_CONNECTED_IDLE_MS 5000u

// Full duplex: no collisions, so no re-request backoff.
#define FSH_REQUEST_JITTER_MS 0u

// Nominal payload throughput for the ETA estimate before a measured rate exists.
// REPLACE with the measured value after the bench.
#define FSH_PAYLOAD_THROUGHPUT_BPS 20000u

// No new block for this long -> the receiver GUI shows "stalled".
#define FSH_STALL_MS 3000u

// ===== UART transport internals (consumed by uart_transport.c) ===============

// Line rate. 230400 is proven in the firmware uart_echo demo; raise only after bench.
#define UART_TP_BAUD 230400u

// COBS-encoded frame cap: a 521-byte packet + COBS overhead (len/254) + delimiter.
#define UART_TP_FRAME_MAX 528u

// RX stream buffer: ~4 max frames of headroom.
#define UART_TP_RX_STREAM_SIZE 2048u

// Deframer worker stop-flag poll period.
#define UART_TP_RX_POLL_MS 25u
