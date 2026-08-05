#pragma once

// Pulse-distance modem for the one-way GPIO carousel link. Pure C, NO furi
// includes, so it round-trips on the host test harness (tools/modem_test.c). All
// timings and tolerances come from gpio_modem_config.h.
//
// The line idles HIGH. The sender emits a short LOW tick at every bit boundary
// and encodes each bit in the fall-to-fall interval (short = 0, long = 1, very
// long = frame-start SYNC). The receiver only timestamps falling edges and feeds
// the intervals here. There is no modem CRC -- the engine's packet CRC16 and the
// final MD5 do the filtering.
//
// Frame on the wire (falling edges): [tick A] SYNC [tick B] bit0 bit1 ... where
// each labelled gap is one fall-to-fall period. Bytes are LSB-first, in the
// order [len][packet bytes]. `len` is the flipper-share packet length.

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#include "gpio_modem_config.h"

// One SYNC period plus 8 bit periods per byte, over [len] + up to MAX_PACKET
// bytes. This is the length of the encoder's period list for a maximal frame.
#define GPIO_MODEM_MAX_PERIODS (1u + 8u * (1u + GPIO_MODEM_MAX_PACKET))

// ===== Encoder (sender) =====================================================
//
// Produces the list of fall-to-fall periods for one frame. The sender emits an
// initial tick, then for each period P: holds the line high for (P - TICK_US)
// and emits the next tick. The first period is always SYNC.

typedef struct {
    uint16_t period_us[GPIO_MODEM_MAX_PERIODS]; // fall-to-fall periods, in order
    size_t n; // number of valid periods
    size_t idx; // next period to emit
} GpioModemEnc;

// Load a packet (len bytes, 1..GPIO_MODEM_MAX_PACKET) as the next frame. Resets
// the cursor. A len outside range yields an empty (idle) encoder.
void gpio_modem_enc_set_frame(GpioModemEnc* enc, const uint8_t* packet, size_t len);

// Pull the next fall-to-fall period (microseconds). Returns false when the frame
// is exhausted -- the caller then holds the line idle-high until the next frame.
bool gpio_modem_enc_next(GpioModemEnc* enc, uint32_t* period_us);

// True while the current frame still has periods to emit.
static inline bool gpio_modem_enc_busy(const GpioModemEnc* enc) {
    return enc->idx < enc->n;
}

// ===== Decoder (receiver) ===================================================
//
// Fed one fall-to-fall interval per falling edge. Returns a packet length (>=1)
// into out[0..cap) when a frame closes, else 0.

typedef enum {
    GpioModemHunt = 0, // waiting for a SYNC period
    GpioModemRead, // locked, accumulating [len] + packet bytes
} GpioModemDecState;

typedef struct {
    GpioModemDecState state;
    uint8_t cur_byte; // byte being assembled, LSB-first
    int bit_in_byte; // 0..7
    uint32_t byte_idx; // 0 = len byte, then 1..len = packet bytes
    uint32_t need_bytes; // 1 + len once len is known (0 = len not read yet)
    uint8_t frame[1u + GPIO_MODEM_MAX_PACKET]; // [len][packet]
} GpioModemDec;

// Reset the decoder to SYNC-hunt (call on init and on any teardown).
void gpio_modem_dec_reset(GpioModemDec* dec);

// Feed one fall-to-fall interval. When a frame closes, writes the packet bytes
// into out[0..cap) and returns the packet length (>=1); otherwise returns 0. A
// SYNC (re)locks the decoder; an out-of-band interval or a bad length resets it
// to hunt.
size_t gpio_modem_dec_feed(GpioModemDec* dec, uint32_t period_us, uint8_t* out, size_t out_cap);
