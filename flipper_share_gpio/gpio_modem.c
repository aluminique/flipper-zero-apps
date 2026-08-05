#include "gpio_modem.h"

#include <string.h>

// ===== Encoder ==============================================================

void gpio_modem_enc_set_frame(GpioModemEnc* enc, const uint8_t* packet, size_t len) {
    enc->n = 0;
    enc->idx = 0;
    if(len < 1 || len > GPIO_MODEM_MAX_PACKET) return; // empty -> idle

    // First period marks frame start.
    enc->period_us[enc->n++] = GPIO_MODEM_SYNC_US;

    // Then [len] followed by the packet bytes, each LSB-first, one period per bit.
    // Emitting len as the first byte lets the decoder size the frame on the fly.
    for(size_t i = 0; i <= len; i++) {
        uint8_t byte = (i == 0) ? (uint8_t)len : packet[i - 1];
        for(int b = 0; b < 8; b++) {
            bool bit = (byte >> b) & 1u;
            enc->period_us[enc->n++] = bit ? GPIO_MODEM_BIT1_US : GPIO_MODEM_BIT0_US;
        }
    }
}

bool gpio_modem_enc_next(GpioModemEnc* enc, uint32_t* period_us) {
    if(enc->idx >= enc->n) return false;
    *period_us = enc->period_us[enc->idx++];
    return true;
}

// ===== Decoder ==============================================================

void gpio_modem_dec_reset(GpioModemDec* dec) {
    memset(dec, 0, sizeof(*dec));
    dec->state = GpioModemHunt;
}

// Classify a fall-to-fall period into a symbol.
typedef enum {
    SymGlitch = 0, // runt / out-of-band low -> resync
    SymBit0,
    SymBit1,
    SymSync,
    SymIdle, // too long (inter-frame gap or a dropped edge) -> resync
} GpioModemSym;

static GpioModemSym gpio_modem_classify(uint32_t period_us) {
    if(period_us < GPIO_MODEM_TMIN_US) return SymGlitch;
    if(period_us < GPIO_MODEM_T01_US) return SymBit0;
    if(period_us < GPIO_MODEM_T1S_US) return SymBit1;
    if(period_us < GPIO_MODEM_TSI_US) return SymSync;
    return SymIdle;
}

size_t gpio_modem_dec_feed(GpioModemDec* dec, uint32_t period_us, uint8_t* out, size_t out_cap) {
    GpioModemSym sym = gpio_modem_classify(period_us);

    if(sym == SymSync) {
        // (Re)lock: start a fresh frame.
        dec->state = GpioModemRead;
        dec->cur_byte = 0;
        dec->bit_in_byte = 0;
        dec->byte_idx = 0;
        dec->need_bytes = 0;
        return 0;
    }

    if(sym == SymGlitch || sym == SymIdle) {
        // Nothing valid here -- wait for the next SYNC.
        dec->state = GpioModemHunt;
        return 0;
    }

    // A data bit. Ignore bits until we have locked on a SYNC.
    if(dec->state != GpioModemRead) return 0;

    uint8_t bit = (sym == SymBit1) ? 1u : 0u;
    dec->cur_byte |= (uint8_t)(bit << dec->bit_in_byte);
    dec->bit_in_byte++;
    if(dec->bit_in_byte < 8) return 0;

    // A full byte assembled.
    dec->frame[dec->byte_idx] = dec->cur_byte;
    dec->cur_byte = 0;
    dec->bit_in_byte = 0;

    if(dec->byte_idx == 0) {
        // Length byte. A bogus length (noise false-lock) bails out immediately.
        uint8_t len = dec->frame[0];
        if(len < 1 || len > GPIO_MODEM_MAX_PACKET) {
            dec->state = GpioModemHunt;
            return 0;
        }
        dec->need_bytes = 1u + len;
    }

    dec->byte_idx++;

    if(dec->need_bytes && dec->byte_idx >= dec->need_bytes) {
        // Frame complete: frame[1..len] is the packet.
        size_t plen = dec->need_bytes - 1u;
        dec->state = GpioModemHunt;
        if(plen <= out_cap) {
            memcpy(out, dec->frame + 1, plen);
            return plen;
        }
        return 0; // caller's buffer too small (should not happen) -> drop
    }

    return 0;
}
