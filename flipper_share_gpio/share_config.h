#pragma once

// Per-app tunables for Flipper Share over a one-way GPIO pulse link. This is the
// single place to tweak the transport and the transport-dependent engine knobs;
// changing anything here requires a recompile. The engine (share.c/share.h) and
// the shared scenes read these but define none of them, so the engine files
// stay byte-identical across all new Flipper Share apps.

// ===== Engine-facing tunables (consumed by share.c / the scenes) =============

// Name of this transport, substituted into every UI string ("Send via ...").
#define FSH_TRANSPORT_NAME "GPIO"

// Enables the one-way broadcast "carousel" engine mode. The single GPIO wire has
// no return channel, so the sender broadcasts announce+blocks round-robin and the
// receiver's block bitmap fills in over passes (same mode the RFID app uses). The
// #ifdef FSH_CAROUSEL blocks in share.c compile out in the other new apps, so the
// engine file stays byte-identical across them.
#define FSH_CAROUSEL

// File-data bytes per DATA packet. 64 -> a 73-byte packet -> a ~11 ms modem frame
// that the sender bit-bangs at high priority (see gpio_transport.c). Frame-error
// rate rises with length; raise to 128 only after the bench shows low loss.
#define FSH_DATA_LENGTH 64u

// Announce cadence (compiled out under carousel -- the sender streams uncondition-
// ally and interleaves ANNOUNCEs via GPIO_CAROUSEL_ANNOUNCE_EVERY). Kept nominal.
#define FSH_ANNOUNCE_INTERVAL_MS 1000u
#define FSH_ANNOUNCE_CONNECTED_MS 3000u

// Receiver re-request timeout and CONNECTED-idle revert (both compiled out under
// carousel -- the receiver never transmits). Kept nominal.
#define FSH_RX_TIMEOUT_MS 500u
#define FSH_CONNECTED_IDLE_MS 5000u

// Sender streams unconditionally (no post-RX gap in carousel).
#define FSH_TX_TIMEOUT_MS 0u

// Engine tick period. Pacing comes from the transport's blocking send, not this.
#define FSH_IDLE_TICK_MS 20u

// Receiver never transmits, so there is nothing to desynchronize.
#define FSH_REQUEST_JITTER_MS 0u

// Nominal payload throughput used for the ETA estimate before the measured
// session rate is available (the engine switches to the live rate a few seconds
// into a transfer). ESTIMATE from the modem timing budget (~11 ms per 64-byte
// DATA frame, one ANNOUNCE every 4 frames); REPLACE with the measured value once
// an overdrive-free carousel transfer has been timed on the bench.
#define FSH_PAYLOAD_THROUGHPUT_BPS 4500u

// No new block for this long -> the receiver GUI shows "stalled". Must comfortably
// exceed one carousel cycle so a block that only comes back next pass is not
// mistaken for a stall.
#define FSH_STALL_MS 15000u

// ===== Carousel / GPIO transport internals ===================================

// One ANNOUNCE per this many carousel frames. The receiver can only lock on an
// ANNOUNCE; sending one every 4 frames keeps the initial lock latency to a couple
// of seconds (25% overhead) while the rest of the stream is DATA. Once locked the
// receiver only needs DATA, so this only bounds lock / re-lock latency. Named
// GPIO_CAROUSEL_ANNOUNCE_EVERY? No -- the shared engine hardcodes the RFID name;
// define it here so share.c stays byte-identical with the RFID app.
#define RFID_CAROUSEL_ANNOUNCE_EVERY 4u

// Bus pin. PA4 = GPIO header pin 4, a plain GPIO with no on-board analog
// circuitry, bussed with one jumper wire (pin 4 <-> pin 4) plus GND. The line
// idles high: the receiver enables its internal pull-up and the sender drives it
// open-drain (pulls low for ticks, releases to the pull-up).
//
// Why not the iButton pad (PB14 / pin 17): each pad carries a hard 1 kOhm pull-up
// to its own 5 V rail (only powered from USB/OTG), so two connected pads form an
// unpredictable divider. Why PA4 specifically: the receiver needs an EXTI line for
// the falling-edge capture, and EXTI is shared by pin NUMBER across all ports --
// pin 7's line is taken by the expansion service (USART1 RX), while PA4's line (4)
// is free (otherwise only the inactive external-SPI chip-select).
#define GPIO_TP_GPIO (&gpio_ext_pa4)

// Sender single-slot mailbox: how long fsh_transport_send() blocks while the
// bit-bang worker is still emitting the previous frame (the backpressure that
// paces the carousel). A timeout drop is harmless -- the carousel re-sends.
#define GPIO_TP_SEND_TIMEOUT_MS 1000u

// Receiver interval-stream depth (in uint32_t intervals) and delivery-queue depth
// (in packets). The stream is sized to hold several frames' worth of falling-edge
// intervals so a scheduling hiccup in the rx worker cannot overflow it mid-frame.
#define GPIO_TP_RX_STREAM_LEN 2048u
#define GPIO_TP_DELIVER_DEPTH 8u
