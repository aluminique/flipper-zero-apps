// Host test harness for the GPIO pulse-distance modem (no hardware, no furi).
// Build: cc -Wall -Wextra -Werror -O2 tools/modem_test.c gpio_modem.c -o /tmp/gpio_modem_test
//
// Encodes frames into fall-to-fall periods (what the receiver's falling-edge ISR
// would measure), optionally perturbs them (jitter, spurious/dropped edges,
// garbage), and checks the decoder recovers the original packet and always
// resynchronises for the next frame.

#include "../gpio_modem.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TEST_PACKET_MAX 73 // FSH_PACKET_MAX for this app (FSH_DATA_LENGTH = 64)

static int g_pass = 0;
static int g_fail = 0;

static void check(int cond, const char* what) {
    if(cond) {
        g_pass++;
    } else {
        g_fail++;
        printf("  FAIL: %s\n", what);
    }
}

// A growable stream of fall-to-fall periods fed to the decoder.
typedef struct {
    uint32_t p[16384];
    int n;
} Stream;

static void stream_push(Stream* s, uint32_t period) {
    if(s->n < (int)(sizeof(s->p) / sizeof(s->p[0]))) s->p[s->n++] = period;
}

// Append one encoded frame, preceded by an inter-frame idle gap (the interval the
// receiver measures on the frame's first tick).
static void stream_push_frame(Stream* s, const uint8_t* packet, size_t len) {
    stream_push(s, GPIO_MODEM_IDLE_US);
    GpioModemEnc enc;
    gpio_modem_enc_set_frame(&enc, packet, len);
    uint32_t period;
    while(gpio_modem_enc_next(&enc, &period)) stream_push(s, period);
}

// Feed a whole stream through a fresh decoder, collecting delivered packets.
typedef struct {
    uint8_t pkt[64][TEST_PACKET_MAX + 8];
    size_t len[64];
    int n;
} Delivered;

static void feed_stream(const Stream* s, Delivered* d) {
    GpioModemDec dec;
    gpio_modem_dec_reset(&dec);
    d->n = 0;
    for(int i = 0; i < s->n; i++) {
        uint8_t out[GPIO_MODEM_MAX_PACKET];
        size_t got = gpio_modem_dec_feed(&dec, s->p[i], out, sizeof(out));
        if(got > 0 && d->n < 64) {
            memcpy(d->pkt[d->n], out, got);
            d->len[d->n] = got;
            d->n++;
        }
    }
}

static void fill_packet(uint8_t* p, size_t len, unsigned seed) {
    for(size_t i = 0; i < len; i++) p[i] = (uint8_t)(seed * 31u + i * 7u + (i << 3));
}

static int packet_eq(const uint8_t* a, const uint8_t* b, size_t len) {
    return memcmp(a, b, len) == 0;
}

// ===== Tests ================================================================

// 1. Clean round-trip for every packet length.
static void test_roundtrip_all_lengths(void) {
    printf("test: clean round-trip, lengths 1..%d\n", TEST_PACKET_MAX);
    for(size_t len = 1; len <= TEST_PACKET_MAX; len++) {
        uint8_t pkt[TEST_PACKET_MAX];
        fill_packet(pkt, len, (unsigned)len);
        Stream s = {0};
        stream_push_frame(&s, pkt, len);
        Delivered d;
        feed_stream(&s, &d);
        check(d.n == 1 && d.len[0] == len && packet_eq(d.pkt[0], pkt, len), "round-trip");
    }
}

// 2. Jitter every period by +/- J and still decode.
static void test_jitter(int jitter) {
    printf("test: jitter +/- %d us\n", jitter);
    srand(1234u + (unsigned)jitter);
    for(int iter = 0; iter < 2000; iter++) {
        size_t len = 1u + (size_t)(rand() % TEST_PACKET_MAX);
        uint8_t pkt[TEST_PACKET_MAX];
        fill_packet(pkt, len, (unsigned)iter);
        Stream s = {0};
        stream_push_frame(&s, pkt, len);
        for(int i = 0; i < s.n; i++) {
            int j = (rand() % (2 * jitter + 1)) - jitter;
            long v = (long)s.p[i] + j;
            if(v < 1) v = 1;
            s.p[i] = (uint32_t)v;
        }
        Delivered d;
        feed_stream(&s, &d);
        check(d.n == 1 && d.len[0] == len && packet_eq(d.pkt[0], pkt, len), "jitter round-trip");
    }
}

// 3. Back-to-back frames both decode.
static void test_back_to_back(void) {
    printf("test: back-to-back frames\n");
    uint8_t a[40], b[73];
    fill_packet(a, sizeof(a), 7);
    fill_packet(b, sizeof(b), 9);
    Stream s = {0};
    stream_push_frame(&s, a, sizeof(a));
    stream_push_frame(&s, b, sizeof(b));
    Delivered d;
    feed_stream(&s, &d);
    check(
        d.n == 2 && d.len[0] == sizeof(a) && packet_eq(d.pkt[0], a, sizeof(a)) &&
            d.len[1] == sizeof(b) && packet_eq(d.pkt[1], b, sizeof(b)),
        "both frames delivered");
}

// 4. Garbage periods before a valid frame -> decoder hunts, then locks.
static void test_garbage_prefix(void) {
    printf("test: garbage prefix then a clean frame\n");
    srand(55u);
    uint8_t pkt[64];
    fill_packet(pkt, sizeof(pkt), 3);
    Stream s = {0};
    for(int i = 0; i < 200; i++) stream_push(&s, 1u + (uint32_t)(rand() % 90)); // noise
    stream_push_frame(&s, pkt, sizeof(pkt));
    Delivered d;
    feed_stream(&s, &d);
    // The last delivered packet must be our frame (noise may or may not fabricate
    // shorter bogus packets, which the engine CRC would drop -- but our clean
    // frame must come through intact).
    int found = 0;
    for(int i = 0; i < d.n; i++)
        if(d.len[i] == sizeof(pkt) && packet_eq(d.pkt[i], pkt, sizeof(pkt))) found = 1;
    check(found, "clean frame recovered after garbage");
}

// 5. A dropped falling edge (two periods merge into one) corrupts that frame but
//    must not stop the next clean frame from decoding.
static void test_dropped_edge_resync(void) {
    printf("test: dropped edge -> resync on next frame\n");
    uint8_t bad[50], good[60];
    fill_packet(bad, sizeof(bad), 11);
    fill_packet(good, sizeof(good), 13);

    Stream s = {0};
    // Frame 1 with a merged pair somewhere in the middle.
    {
        stream_push(&s, GPIO_MODEM_IDLE_US);
        GpioModemEnc enc;
        gpio_modem_enc_set_frame(&enc, bad, sizeof(bad));
        uint32_t period;
        int k = 0;
        uint32_t held = 0;
        while(gpio_modem_enc_next(&enc, &period)) {
            if(k == 30) {
                held = period; // drop this edge: merge into the next period
            } else if(held) {
                stream_push(&s, held + period);
                held = 0;
            } else {
                stream_push(&s, period);
            }
            k++;
        }
        if(held) stream_push(&s, held);
    }
    // Frame 2 is clean.
    stream_push_frame(&s, good, sizeof(good));

    Delivered d;
    feed_stream(&s, &d);
    int found_good = 0;
    for(int i = 0; i < d.n; i++)
        if(d.len[i] == sizeof(good) && packet_eq(d.pkt[i], good, sizeof(good))) found_good = 1;
    check(found_good, "next clean frame decoded after a dropped edge");
}

// 6. A spurious extra falling edge (one period split in two) likewise corrupts
//    only its own frame; the following clean frame still decodes.
static void test_spurious_edge_resync(void) {
    printf("test: spurious edge -> resync on next frame\n");
    uint8_t bad[45], good[70];
    fill_packet(bad, sizeof(bad), 21);
    fill_packet(good, sizeof(good), 23);

    Stream s = {0};
    {
        stream_push(&s, GPIO_MODEM_IDLE_US);
        GpioModemEnc enc;
        gpio_modem_enc_set_frame(&enc, bad, sizeof(bad));
        uint32_t period;
        int k = 0;
        while(gpio_modem_enc_next(&enc, &period)) {
            if(k == 20 && period > 6) {
                stream_push(&s, period / 2); // split one period into two edges
                stream_push(&s, period - period / 2);
            } else {
                stream_push(&s, period);
            }
            k++;
        }
    }
    stream_push_frame(&s, good, sizeof(good));

    Delivered d;
    feed_stream(&s, &d);
    int found_good = 0;
    for(int i = 0; i < d.n; i++)
        if(d.len[i] == sizeof(good) && packet_eq(d.pkt[i], good, sizeof(good))) found_good = 1;
    check(found_good, "next clean frame decoded after a spurious edge");
}

// 7. A SYNC immediately followed by a bogus length byte must not deliver a
//    packet and must not wedge the decoder (a following clean frame decodes).
static void test_bad_length_bail(void) {
    printf("test: bogus length after sync -> bail, then decode\n");
    Stream s = {0};
    stream_push(&s, GPIO_MODEM_IDLE_US);
    stream_push(&s, GPIO_MODEM_SYNC_US);
    for(int b = 0; b < 8; b++) stream_push(&s, GPIO_MODEM_BIT0_US); // len byte = 0 (invalid)
    uint8_t good[64];
    fill_packet(good, sizeof(good), 31);
    stream_push_frame(&s, good, sizeof(good));

    Delivered d;
    feed_stream(&s, &d);
    int found_good = 0, spurious = 0;
    for(int i = 0; i < d.n; i++) {
        if(d.len[i] == sizeof(good) && packet_eq(d.pkt[i], good, sizeof(good)))
            found_good = 1;
        else
            spurious = 1;
    }
    check(found_good && !spurious, "bailed on bad length, decoded the clean frame");
}

int main(void) {
    test_roundtrip_all_lengths();
    test_jitter(0);
    test_jitter(4);
    test_jitter(9); // near the ~10 us band slack -- USB-scale ISR jitter
    test_back_to_back();
    test_garbage_prefix();
    test_dropped_edge_resync();
    test_spurious_edge_resync();
    test_bad_length_bail();

    printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
