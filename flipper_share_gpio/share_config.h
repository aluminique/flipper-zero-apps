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
// into a transfer). Bench: ~2700 B/s effective at the earlier tight timing with
// USB-induced frame loss; the wider bands below trade a little raw rate for far
// less loss. Left conservative; REPLACE with the re-measured value.
#define FSH_PAYLOAD_THROUGHPUT_BPS 3000u

// No new block for this long -> the receiver GUI shows "stalled". Must comfortably
// exceed one carousel cycle so a block that only comes back next pass is not
// mistaken for a stall.
#define FSH_STALL_MS 15000u

// ===== Carousel / GPIO transport internals ===================================

// One ANNOUNCE per this many carousel frames. The receiver can only lock on an
// ANNOUNCE, but on this clean wire an ANNOUNCE decodes reliably, so it does not
// need to be frequent -- one every 8 frames still locks within a couple of
// seconds while cutting the steady-state overhead to ~12% (vs 25% at 4), which
// goes straight into DATA throughput. Only bounds initial-lock / re-lock latency.
// Named GPIO_CAROUSEL_ANNOUNCE_EVERY? No -- the shared engine hardcodes the RFID
// name; define it here so share.c stays byte-identical with the RFID app.
#define RFID_CAROUSEL_ANNOUNCE_EVERY 8u

// Bus pin. PA7 = GPIO header pin 2. One jumper wire (pin 2 <-> pin 2) plus GND.
// The line idles high: the receiver enables the internal pull-up and the sender
// drives it open-drain (pulls low for ticks, releases to the pull-up).
//
// Why PA7 specifically: the receiver reads the falling-edge time from a HARDWARE
// timer input-capture (TIM17_CH1, AF14 -- see gpio_transport.c) so that USB
// interrupt latency cannot distort the measured interval; PA7 is the header pin
// that routes to a free capture-capable timer channel (PA4 has only LPTIM2 PWM
// out; PA6/TIM16 is the beeper; PB3/TIM2 is the RFID timer). We use the TIM17 IRQ,
// not EXTI, so the expansion service's historical claim on pin-7's EXTI line does
// not apply -- but if that service ever fights us here, disable it in Settings.
#define GPIO_TP_GPIO (&gpio_ext_pa7)

// Sender single-slot mailbox: how long fsh_transport_send() blocks while the
// bit-bang worker is still emitting the previous frame (the backpressure that
// paces the carousel). A timeout drop is harmless -- the carousel re-sends.
#define GPIO_TP_SEND_TIMEOUT_MS 1000u

// Receiver delivery-queue depth (in packets), between the decode worker and the
// storage worker. Deep enough to ride out a storage hiccup under host (qFlipper)
// load. (The capture-interval ring lives in gpio_transport.c, sized there.)
#define GPIO_TP_DELIVER_DEPTH 16u
