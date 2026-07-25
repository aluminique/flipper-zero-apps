// Host test harness for the UART COBS framing (no hardware, no furi).
// Build: cc -Wall -Wextra -O2 tools/framing_test.c uart_framing.c -o /tmp/framing_test
//
// Round-trips packets through COBS + a 0x00 delimiter, checks a byte-stream
// deframer recovers them, verifies corrupted frames are rejected (decode failure
// or a mismatch the engine CRC16 would catch), and that a truncated frame does
// not stop the next frame from decoding (delimiter resync).

#include "../uart_framing.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PKT_MAX 521 // 512 data + 9 overhead (FSH DATA packet for this app)
#define ENC_MAX (PKT_MAX + PKT_MAX / 254 + 2)

// Encode a packet to the wire: COBS(packet) + 0x00 delimiter. Returns wire length.
static size_t to_wire(const uint8_t* pkt, size_t len, uint8_t* wire) {
    size_t n = uart_cobs_encode(pkt, len, wire);
    wire[n++] = 0x00;
    return n;
}

// Minimal byte-stream deframer, mirroring UartRxWorker: accumulate until 0x00,
// then COBS-decode the accumulated frame. Calls sink() for every decoded frame
// (len 0 from a malformed frame is dropped). Oversized runs are discarded.
typedef void (*sink_fn)(const uint8_t* pkt, size_t len, void* ctx);
static void deframe(const uint8_t* stream, size_t n, sink_fn sink, void* ctx) {
    static uint8_t acc[ENC_MAX];
    static uint8_t dec[PKT_MAX + 8];
    size_t acc_len = 0;
    for(size_t i = 0; i < n; i++) {
        uint8_t b = stream[i];
        if(b == 0x00) {
            if(acc_len > 0) {
                size_t dl = uart_cobs_decode(acc, acc_len, dec);
                if(dl > 0) sink(dec, dl, ctx);
            }
            acc_len = 0;
        } else if(acc_len < sizeof(acc)) {
            acc[acc_len++] = b;
        } else {
            acc_len = 0; // oversize without delimiter -> drop to next delimiter
        }
    }
}

typedef struct {
    const uint8_t* expect;
    size_t expect_len;
    int matched;
    int count;
} Capture;

static void capture_sink(const uint8_t* pkt, size_t len, void* ctx) {
    Capture* c = ctx;
    c->count++;
    if(len == c->expect_len && memcmp(pkt, c->expect, len) == 0) c->matched = 1;
}

static int roundtrip(const uint8_t* pkt, size_t len) {
    uint8_t wire[ENC_MAX + 1];
    size_t n = to_wire(pkt, len, wire);
    Capture c = {pkt, len, 0, 0};
    deframe(wire, n, capture_sink, &c);
    return c.matched && c.count == 1;
}

static void fill_random(uint8_t* p, size_t len) {
    for(size_t i = 0; i < len; i++) p[i] = (uint8_t)(rand() & 0xFF);
}

int main(void) {
    srand(0xBEEF);
    int fails = 0, total = 0;
    uint8_t buf[PKT_MAX];

    // ---- Fixed vectors ----
    struct {
        const char* name;
        size_t len;
        int fill;
    } fixed[] = {
        {"empty", 0, -1}, {"1-byte", 1, -1}, {"61 CTRL", 61, -1},
        {"521 DATA", 521, -1}, {"all-0x00", 521, 0}, {"all-0xFF", 521, 0xFF},
    };
    for(size_t f = 0; f < sizeof(fixed) / sizeof(fixed[0]); f++) {
        if(fixed[f].fill < 0)
            fill_random(buf, fixed[f].len);
        else
            memset(buf, fixed[f].fill, fixed[f].len);
        total++;
        // The empty frame encodes to a single 0x01 then delimiter; the deframer
        // decodes it to a 0-length frame which is dropped (len 0). Treat as pass.
        if(fixed[f].len == 0) {
            uint8_t wire[4];
            size_t n = to_wire(buf, 0, wire);
            uint8_t dec[4];
            size_t dl = uart_cobs_decode(wire, n - 1, dec); // exclude delimiter
            if(dl != 0) {
                fails++;
                printf("FAIL empty encodes/decodes to %zu (want 0)\n", dl);
            }
            continue;
        }
        if(!roundtrip(buf, fixed[f].len)) {
            fails++;
            printf("FAIL roundtrip: %s\n", fixed[f].name);
        }
    }

    // ---- 10000 random frames ----
    int rt_fail = 0;
    for(int i = 0; i < 10000; i++) {
        size_t len = 1 + (rand() % PKT_MAX);
        fill_random(buf, len);
        total++;
        if(!roundtrip(buf, len)) rt_fail++;
    }
    fails += rt_fail;
    printf("random roundtrip: %d/10000 failed\n", rt_fail);

    // ---- Corruption: a flipped encoded byte must be rejected (decode fail or
    //      a decoded mismatch the engine CRC16 would drop) — never a silent leak.
    int leak = 0;
    for(int i = 0; i < 10000; i++) {
        size_t len = 1 + (rand() % PKT_MAX);
        fill_random(buf, len);
        uint8_t enc[ENC_MAX];
        size_t n = uart_cobs_encode(buf, len, enc);
        // flip one byte to a different value
        size_t pos = rand() % n;
        uint8_t orig = enc[pos];
        do {
            enc[pos] = (uint8_t)(rand() & 0xFF);
        } while(enc[pos] == orig);
        uint8_t dec[PKT_MAX + 8];
        size_t dl = uart_cobs_decode(enc, n, dec);
        total++;
        if(dl == len && memcmp(dec, buf, len) == 0) {
            leak++; // corruption produced the original packet -> would slip past
        }
    }
    fails += leak;
    printf("corruption leaks: %d/10000\n", leak);

    // ---- Truncated frame + next frame: delimiter resync ----
    {
        uint8_t a[300], b[400];
        fill_random(a, sizeof(a));
        fill_random(b, sizeof(b));
        uint8_t stream[3 * ENC_MAX];
        size_t n = 0;
        size_t na = uart_cobs_encode(a, sizeof(a), stream + n);
        n += na - 5; // truncate frame A (drop 5 bytes, no delimiter written yet)
        stream[n++] = 0x00; // delimiter closes the truncated (garbage) A -> dropped
        size_t nb = to_wire(b, sizeof(b), stream + n);
        n += nb;
        Capture c = {b, sizeof(b), 0, 0};
        deframe(stream, n, capture_sink, &c);
        total++;
        if(c.matched)
            printf("resync after truncation: OK\n");
        else {
            fails++;
            printf("FAIL resync after truncation\n");
        }
    }

    printf("\n==== %d/%d checks failed ====\n", fails, total);
    return fails ? 1 : 0;
}
