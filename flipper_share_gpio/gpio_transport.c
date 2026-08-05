#include "gpio_transport.h"
#include "gpio_modem.h"
#include "share.h" // FSH_PACKET_MAX, fsh_transport_send / fsh_receive_callback

#include <furi.h>
#include <furi_hal.h> // furi_hal_gpio_*, DWT->CYCCNT, furi_hal_cortex_instructions_per_microsecond
#include <furi_hal_interrupt.h> // furi_hal_interrupt_set_isr (own the TIM17 capture IRQ)
#include <furi_hal_bus.h> // furi_hal_bus_enable/disable (TIM17 clock)
#include <stm32wbxx_ll_tim.h> // LL_TIM_* input-capture configuration

#include "share_config.h" // GPIO_TP_* pin and tunables

#define TAG "GpioTransport"

// Receiver capture peripheral: TIM17 channel 1 on PA7 (AF14), 1 us tick. The
// falling edge latches CCR1 in HARDWARE, so the capture ISR reads the exact edge
// time no matter how late it runs -- a USB interrupt delaying the ISR no longer
// distorts the measured interval, which is what made the software EXTI+DWT
// timestamp lossy under USB load. TIM17 is otherwise used only by the NFC stack,
// which is inactive while this app runs.
#define GPIO_TP_CAP_TIM TIM17
#define GPIO_TP_CAP_BUS FuriHalBusTIM17
#define GPIO_TP_CAP_IRQ FuriHalInterruptIdTim1TrgComTim17
#define GPIO_TP_CAP_AF GpioAltFn14TIM17

typedef struct {
    uint8_t len;
    uint8_t data[FSH_PACKET_MAX];
} GpioTpPacket;

typedef struct {
    GpioTransportMode mode;
    volatile bool worker_stop;

    // Sender: a high-priority worker bit-bangs one frame per mailbox slot.
    FuriThread* tx_worker;
    FuriMessageQueue* tx_q; // single-slot pending frame (send -> tx worker)
    GpioModemEnc enc; // touched only by the tx worker

    // Receiver: the EXTI ISR hands fall-to-fall intervals to the rx worker, which
    // decodes and forwards to a separate delivery worker so the engine's storage
    // I/O never stalls the decode path.
    FuriThread* rx_worker;
    FuriThread* deliver_worker;
    FuriStreamBuffer* rx_stream; // EXTI ISR -> rx worker (intervals, microseconds)
    FuriMessageQueue* deliver_q; // rx worker -> delivery worker
    GpioModemDec dec; // touched only by the rx worker
    volatile uint16_t rx_last_ccr; // capture ISR: TIM17 CCR1 of the previous falling edge
    volatile bool rx_have_last;
    volatile bool field_on;
} GpioTransport;

// Owned by the scene lifecycle: init in on_enter, deinit in on_exit.
static GpioTransport* gpio_tp = NULL;

// ===== Engine -> transport (TX) ==============================================

void fsh_transport_send(const uint8_t* buf, size_t len) {
    GpioTransport* tp = gpio_tp;
    if(!tp) return; // not running -- drop, the carousel re-sends
    if(tp->mode != GpioTransportModeSender) return; // receiver never transmits (one-way)
    if(len < 1 || len > FSH_PACKET_MAX) {
        FURI_LOG_E(TAG, "bad packet length %zu", len);
        return;
    }

    GpioTpPacket pkt;
    pkt.len = (uint8_t)len;
    memcpy(pkt.data, buf, len);
    // Single-slot mailbox: blocks while the bit-bang worker is still emitting the
    // previous frame (the backpressure that paces the engine's carousel loop). A
    // timeout drop is harmless -- the carousel re-sends the block next pass.
    if(furi_message_queue_put(tp->tx_q, &pkt, furi_ms_to_ticks(GPIO_TP_SEND_TIMEOUT_MS)) !=
       FuriStatusOk) {
        FURI_LOG_D(TAG, "tx slot busy, frame dropped");
    }
}

// ===== Sender (bit-bang) =====================================================
// The frame is bit-banged from the deterministic DWT cycle counter on a
// high-priority thread, NOT inside a critical section: interrupts (BT/USB) stay
// serviced, so the system stays healthy. Each falling edge is pinned to an
// absolute time on the DWT grid, so timing jitter cannot accumulate across a
// frame -- a preemption only stretches the one edge it lands on. A rare mangled
// frame fails the engine's CRC16 and the carousel simply re-sends it.

static inline void gpio_tp_wait_until(uint32_t target_cyc) {
    while((int32_t)(DWT->CYCCNT - target_cyc) < 0) {
    }
}

static void gpio_tp_emit_frame(GpioTransport* tp, const uint8_t* data, uint8_t len) {
    gpio_modem_enc_set_frame(&tp->enc, data, len);
    const uint32_t cyc = furi_hal_cortex_instructions_per_microsecond();
    const uint32_t tick_cyc = GPIO_MODEM_TICK_US * cyc;

    // First tick (falling edge A). Anchor the absolute timeline right after it.
    furi_hal_gpio_write(GPIO_TP_GPIO, false);
    uint32_t t = DWT->CYCCNT;

    uint32_t period_us;
    while(gpio_modem_enc_next(&tp->enc, &period_us)) {
        // Release (rising edge) TICK_US after the fall, then hold high until the
        // next fall lands exactly `period_us` after the previous fall.
        gpio_tp_wait_until(t + tick_cyc);
        furi_hal_gpio_write(GPIO_TP_GPIO, true);
        t += period_us * cyc;
        gpio_tp_wait_until(t);
        furi_hal_gpio_write(GPIO_TP_GPIO, false); // next falling edge
    }
    // Release after the last tick and return to idle-high.
    gpio_tp_wait_until(t + tick_cyc);
    furi_hal_gpio_write(GPIO_TP_GPIO, true);
}

static int32_t gpio_tp_tx_worker(void* context) {
    GpioTransport* tp = context;
    GpioTpPacket pkt;
    while(!tp->worker_stop) {
        if(furi_message_queue_get(tp->tx_q, &pkt, furi_ms_to_ticks(50)) == FuriStatusOk) {
            gpio_tp_emit_frame(tp, pkt.data, pkt.len);
        }
    }
    return 0;
}

// ===== Receiver (EXTI + decode) ==============================================

// Timer capture interrupt: TIM17 CH1 latched the falling edge into CCR1 in
// hardware. Read it (which clears the flag), compute the interval since the
// previous edge, and hand it (microseconds) to the rx worker. Because CCR1 holds
// the true edge time, this ISR running late (e.g. behind a USB interrupt) does
// not distort the interval.
static void gpio_tp_capture_isr(void* context) {
    GpioTransport* tp = context;
    if(!LL_TIM_IsActiveFlag_CC1(GPIO_TP_CAP_TIM)) return;
    uint16_t ccr = (uint16_t)LL_TIM_IC_GetCaptureCH1(GPIO_TP_CAP_TIM); // read clears CC1IF
    if(tp->rx_have_last) {
        // 1 us/tick, 16-bit counter: the unsigned subtraction wraps correctly for
        // any interval up to 65 ms. A missed edge (ISR delayed past a whole period)
        // just yields a larger interval, which the decoder treats as a resync.
        uint32_t interval = (uint16_t)(ccr - tp->rx_last_ccr);
        furi_stream_buffer_send(tp->rx_stream, &interval, sizeof(interval), 0);
    }
    tp->rx_last_ccr = ccr;
    tp->rx_have_last = true;
}

// Configure TIM17 CH1 to capture falling edges on PA7 at a 1 us tick and route
// its interrupt to gpio_tp_capture_isr. The pin is put in TIM17 AF with a pull-up
// so the line idles high.
static void gpio_tp_capture_start(GpioTransport* tp) {
    tp->rx_have_last = false;
    furi_hal_bus_enable(GPIO_TP_CAP_BUS);
    LL_TIM_SetPrescaler(
        GPIO_TP_CAP_TIM, furi_hal_cortex_instructions_per_microsecond() - 1u); // -> 1 us tick
    LL_TIM_SetAutoReload(GPIO_TP_CAP_TIM, 0xFFFFu);
    LL_TIM_SetCounterMode(GPIO_TP_CAP_TIM, LL_TIM_COUNTERMODE_UP);
    LL_TIM_IC_SetActiveInput(GPIO_TP_CAP_TIM, LL_TIM_CHANNEL_CH1, LL_TIM_ACTIVEINPUT_DIRECTTI);
    LL_TIM_IC_SetPrescaler(GPIO_TP_CAP_TIM, LL_TIM_CHANNEL_CH1, LL_TIM_ICPSC_DIV1);
    LL_TIM_IC_SetPolarity(GPIO_TP_CAP_TIM, LL_TIM_CHANNEL_CH1, LL_TIM_IC_POLARITY_FALLING);
    LL_TIM_IC_SetFilter(GPIO_TP_CAP_TIM, LL_TIM_CHANNEL_CH1, LL_TIM_IC_FILTER_FDIV1);
    // Run the capture ISR ABOVE the USB interrupt (which is Normal priority): the
    // ISR is tiny (read CCR1, push to the stream buffer), and if USB enumeration
    // could delay it past a whole bit period the capture would over-run and merge
    // two edges into one wrong interval -- exactly the corruption burst seen for a
    // few seconds after a cable is plugged into the receiver. Higher still permits
    // the ISR-safe furi_stream_buffer_send (only KamiSama forbids OS primitives).
    furi_hal_interrupt_set_isr_ex(
        GPIO_TP_CAP_IRQ, FuriHalInterruptPriorityHigher, gpio_tp_capture_isr, tp);
    LL_TIM_ClearFlag_CC1(GPIO_TP_CAP_TIM);
    LL_TIM_EnableIT_CC1(GPIO_TP_CAP_TIM);
    LL_TIM_CC_EnableChannel(GPIO_TP_CAP_TIM, LL_TIM_CHANNEL_CH1);
    LL_TIM_EnableCounter(GPIO_TP_CAP_TIM);
    furi_hal_gpio_init_ex(
        GPIO_TP_GPIO, GpioModeAltFunctionPushPull, GpioPullUp, GpioSpeedLow, GPIO_TP_CAP_AF);
}

static void gpio_tp_capture_stop(void) {
    LL_TIM_DisableCounter(GPIO_TP_CAP_TIM);
    LL_TIM_DisableIT_CC1(GPIO_TP_CAP_TIM);
    LL_TIM_CC_DisableChannel(GPIO_TP_CAP_TIM, LL_TIM_CHANNEL_CH1);
    furi_hal_interrupt_set_isr(GPIO_TP_CAP_IRQ, NULL, NULL);
    furi_hal_bus_disable(GPIO_TP_CAP_BUS);
}

// Delivers decoded packets to the engine (which does the storage I/O), off the
// decode path so a slow file write cannot back up the interval stream.
static int32_t gpio_tp_deliver_worker(void* context) {
    GpioTransport* tp = context;
    GpioTpPacket p;
    while(!tp->worker_stop) {
        if(furi_message_queue_get(tp->deliver_q, &p, furi_ms_to_ticks(50)) == FuriStatusOk) {
            fsh_receive_callback(p.data, p.len);
        }
    }
    return 0;
}

static int32_t gpio_tp_rx_worker(void* context) {
    GpioTransport* tp = context;
    uint8_t pkt[FSH_PACKET_MAX];
    uint32_t batch[64]; // drain many intervals per syscall

    while(!tp->worker_stop) {
        size_t got =
            furi_stream_buffer_receive(tp->rx_stream, batch, sizeof(batch), furi_ms_to_ticks(50));
        size_t nev = got / sizeof(uint32_t);
        for(size_t i = 0; i < nev; i++) {
            size_t len = gpio_modem_dec_feed(&tp->dec, batch[i], pkt, FSH_PACKET_MAX);
            if(len) {
                GpioTpPacket p;
                p.len = (uint8_t)len;
                memcpy(p.data, pkt, len);
                // Drop on a full queue -- the carousel re-sends the frame.
                furi_message_queue_put(tp->deliver_q, &p, 0);
            }
        }
    }
    return 0;
}

// ===== Public API ============================================================

void gpio_transport_init(GpioTransportMode mode) {
    furi_assert(gpio_tp == NULL);

    GpioTransport* tp = malloc(sizeof(GpioTransport));
    memset(tp, 0, sizeof(*tp));
    tp->mode = mode;
    tp->worker_stop = false;

    if(mode == GpioTransportModeSender) {
        tp->tx_q = furi_message_queue_alloc(1, sizeof(GpioTpPacket)); // single slot

        // Open-drain, idle high: the sender only ever pulls the line low for ticks
        // and releases it to the pull-up. Open-drain (not push-pull) keeps the bus
        // short-safe if the peer is miswired, and the ~10 us gaps easily cover the
        // pull-up rise time.
        furi_hal_gpio_write(GPIO_TP_GPIO, true);
        furi_hal_gpio_init(GPIO_TP_GPIO, GpioModeOutputOpenDrain, GpioPullUp, GpioSpeedLow);
        furi_hal_gpio_write(GPIO_TP_GPIO, true);

        tp->tx_worker = furi_thread_alloc_ex("GpioTxWorker", 1024, gpio_tp_tx_worker, tp);
        // High priority so the busy-wait bit-bang is not preempted for long
        // mid-frame; it still yields (blocks on the mailbox) between frames.
        furi_thread_set_priority(tp->tx_worker, FuriThreadPriorityHigh);
        furi_thread_start(tp->tx_worker);
    } else {
        gpio_modem_dec_reset(&tp->dec);
        tp->rx_stream =
            furi_stream_buffer_alloc(sizeof(uint32_t) * GPIO_TP_RX_STREAM_LEN, sizeof(uint32_t));
        tp->deliver_q = furi_message_queue_alloc(GPIO_TP_DELIVER_DEPTH, sizeof(GpioTpPacket));

        tp->deliver_worker =
            furi_thread_alloc_ex("GpioDeliver", 2048, gpio_tp_deliver_worker, tp);
        furi_thread_start(tp->deliver_worker);
        tp->rx_worker = furi_thread_alloc_ex("GpioRxWorker", 2048, gpio_tp_rx_worker, tp);
        furi_thread_set_priority(tp->rx_worker, FuriThreadPriorityHigh);
        furi_thread_start(tp->rx_worker);

        gpio_tp_capture_start(tp); // TIM17 CH1 hardware input-capture on PA7
        tp->field_on = true;
    }

    gpio_tp = tp; // publish only when fully started
    FURI_LOG_I(TAG, "started as %s", mode == GpioTransportModeSender ? "sender" : "receiver");
}

void gpio_transport_deinit(void) {
    GpioTransport* tp = gpio_tp;
    if(!tp) return;
    gpio_tp = NULL; // sends become no-ops first

    tp->worker_stop = true;

    if(tp->mode == GpioTransportModeSender) {
        if(tp->tx_worker) {
            furi_thread_join(tp->tx_worker);
            furi_thread_free(tp->tx_worker);
        }
        if(tp->tx_q) furi_message_queue_free(tp->tx_q);
    } else {
        if(tp->field_on) {
            gpio_tp_capture_stop(); // no more capture interrupts feed the stream
            tp->field_on = false;
        }
        if(tp->rx_worker) {
            furi_thread_join(tp->rx_worker); // stops decoding/enqueuing first
            furi_thread_free(tp->rx_worker);
        }
        if(tp->deliver_worker) {
            furi_thread_join(tp->deliver_worker); // then drain deliveries to the engine
            furi_thread_free(tp->deliver_worker);
        }
        if(tp->rx_stream) furi_stream_buffer_free(tp->rx_stream);
        if(tp->deliver_q) furi_message_queue_free(tp->deliver_q);
    }

    // Leave the pin in a known idle state (input, no pull) whichever role ran.
    furi_hal_gpio_init(GPIO_TP_GPIO, GpioModeAnalog, GpioPullNo, GpioSpeedLow);

    free(tp);
    FURI_LOG_I(TAG, "stopped");
}

void gpio_transport_stop_field(void) {
    GpioTransport* tp = gpio_tp;
    if(!tp || tp->mode != GpioTransportModeReceiver) return;
    if(tp->field_on) {
        gpio_tp_capture_stop();
        tp->field_on = false;
    }
}
