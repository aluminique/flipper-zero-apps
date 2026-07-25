#include "uart_transport.h"
#include "uart_framing.h"
#include "share.h" // FSH_PACKET_MAX, FSH_WORKER_STOP_FLAG, fsh_transport_send/receive_callback

#include <furi.h>
#include <furi_hal.h>
#include <furi_hal_serial.h>
#include <furi_hal_serial_control.h>
#include <expansion/expansion.h>

#define TAG "UartTransport"

// The COBS frame buffer must hold the worst-case encoded packet plus the
// delimiter: len + len/254 (overhead) + 1 (leading code) + 1 (delimiter).
_Static_assert(
    UART_TP_FRAME_MAX >= FSH_PACKET_MAX + FSH_PACKET_MAX / 254 + 2,
    "UART_TP_FRAME_MAX too small for a COBS-encoded flipper-share packet");

typedef struct {
    Expansion* expansion;
    FuriHalSerialHandle* serial;
    FuriStreamBuffer* rx_stream; // RX ISR -> UartRxWorker (raw bytes)
    FuriThread* rx_worker;
    FuriMutex* tx_mutex;
    uint8_t tx_frame[UART_TP_FRAME_MAX]; // COBS-encoded packet + delimiter (under tx_mutex)
    volatile bool running;
} UartTransport;

// Owned by the scene lifecycle: init in on_enter, deinit in on_exit.
static UartTransport* uart_tp = NULL;

// ===== RX path ===============================================================

// Interrupt context (per the HAL warning): drain available bytes into the stream
// buffer, nothing else.
static void
    uart_tp_rx_isr(FuriHalSerialHandle* handle, FuriHalSerialRxEvent event, void* context) {
    UartTransport* tp = context;
    if(event & FuriHalSerialRxEventData) {
        while(furi_hal_serial_async_rx_available(handle)) {
            uint8_t b = furi_hal_serial_async_rx(handle);
            furi_stream_buffer_send(tp->rx_stream, &b, 1, 0);
        }
    }
}

// Deframer: accumulate bytes until the 0x00 delimiter, COBS-decode, deliver.
static int32_t uart_tp_rx_worker(void* context) {
    UartTransport* tp = context;
    uint8_t frame[UART_TP_FRAME_MAX];
    size_t frame_len = 0;
    uint8_t packet[UART_TP_FRAME_MAX]; // decoded <= encoded, so this always fits

    while(!(furi_thread_flags_get() & FSH_WORKER_STOP_FLAG)) {
        uint8_t b;
        size_t n = furi_stream_buffer_receive(
            tp->rx_stream, &b, 1, furi_ms_to_ticks(UART_TP_RX_POLL_MS));
        if(n != 1) continue;

        if(b == 0x00) {
            // End of frame: decode and deliver. A malformed frame (COBS decode
            // returns 0) or an over-length packet is dropped; ARQ re-requests.
            if(frame_len > 0) {
                size_t plen = uart_cobs_decode(frame, frame_len, packet);
                if(plen > 0 && plen <= FSH_PACKET_MAX) fsh_receive_callback(packet, plen);
                frame_len = 0;
            }
        } else if(frame_len < sizeof(frame)) {
            frame[frame_len++] = b;
        } else {
            // Oversize run without a delimiter: desync -> drop to the next delimiter.
            frame_len = 0;
        }
    }
    return 0;
}

// ===== TX path ===============================================================

void fsh_transport_send(const uint8_t* buf, size_t len) {
    UartTransport* tp = uart_tp;
    if(!tp || !tp->running) return; // not running — drop, ARQ recovers
    if(len < 1 || len > FSH_PACKET_MAX) {
        FURI_LOG_E(TAG, "bad packet length %zu", len);
        return;
    }

    // COBS-encode + delimiter, then blocking-write. The blocking TX at line rate
    // is the natural back-pressure that paces the engine's send loop.
    furi_mutex_acquire(tp->tx_mutex, FuriWaitForever);
    if(tp->running) {
        size_t n = uart_cobs_encode(buf, len, tp->tx_frame);
        tp->tx_frame[n++] = 0x00;
        furi_hal_serial_tx(tp->serial, tp->tx_frame, n);
        furi_hal_serial_tx_wait_complete(tp->serial);
    }
    furi_mutex_release(tp->tx_mutex);
}

// ===== Public API ============================================================

// Free everything allocated so far and restore the expansion service. Used by
// both the init failure paths and deinit.
static void uart_tp_teardown(UartTransport* tp) {
    if(tp->serial) {
        furi_hal_serial_deinit(tp->serial);
        furi_hal_serial_control_release(tp->serial);
    }
    if(tp->expansion) {
        expansion_enable(tp->expansion);
        furi_record_close(RECORD_EXPANSION);
    }
    if(tp->rx_stream) furi_stream_buffer_free(tp->rx_stream);
    if(tp->tx_mutex) furi_mutex_free(tp->tx_mutex);
    free(tp);
}

bool uart_transport_init(void) {
    furi_assert(uart_tp == NULL);

    UartTransport* tp = malloc(sizeof(UartTransport));
    memset(tp, 0, sizeof(*tp));

    // Disabling the expansion service before touching USART is mandatory (and
    // part of the expansion header's own contract). Keep the record open for the
    // app's lifetime. Reference: firmware applications/main/gpio/gpio_app.c.
    tp->expansion = furi_record_open(RECORD_EXPANSION);
    expansion_disable(tp->expansion);

    if(furi_hal_serial_control_is_busy(FuriHalSerialIdUsart)) {
        FURI_LOG_W(TAG, "USART busy");
        uart_tp_teardown(tp);
        return false;
    }

    tp->serial = furi_hal_serial_control_acquire(FuriHalSerialIdUsart);
    if(!tp->serial) {
        FURI_LOG_W(TAG, "USART acquire failed");
        uart_tp_teardown(tp);
        return false;
    }
    furi_hal_serial_init(tp->serial, UART_TP_BAUD);

    tp->rx_stream = furi_stream_buffer_alloc(UART_TP_RX_STREAM_SIZE, 1);
    tp->tx_mutex = furi_mutex_alloc(FuriMutexTypeNormal);
    tp->running = true;

    tp->rx_worker = furi_thread_alloc_ex("UartRxWorker", 2048, uart_tp_rx_worker, tp);
    furi_thread_start(tp->rx_worker);

    furi_hal_serial_async_rx_start(tp->serial, uart_tp_rx_isr, tp, false);

    uart_tp = tp; // publish only when fully started
    FURI_LOG_I(TAG, "started at %lu baud", (unsigned long)UART_TP_BAUD);
    return true;
}

void uart_transport_deinit(void) {
    UartTransport* tp = uart_tp;
    if(!tp) return;
    uart_tp = NULL; // sends become no-ops first

    furi_hal_serial_async_rx_stop(tp->serial);

    tp->running = false;
    if(tp->rx_worker) {
        furi_thread_flags_set(furi_thread_get_id(tp->rx_worker), FSH_WORKER_STOP_FLAG);
        furi_thread_join(tp->rx_worker);
        furi_thread_free(tp->rx_worker);
    }

    uart_tp_teardown(tp); // serial deinit/release, expansion re-enable, free buffers
    FURI_LOG_I(TAG, "stopped");
}
