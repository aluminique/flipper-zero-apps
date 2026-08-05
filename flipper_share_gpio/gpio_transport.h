#pragma once

// One-way (carousel) GPIO transport for Flipper Share: a packet pipe over a
// single jumper wire (header pin 4 / PA4) plus GND, using the pulse-distance
// modem (gpio_modem.*). The SENDER bit-bangs a self-clocked stream of short LOW
// ticks and encodes each bit in the fall-to-fall interval; the RECEIVER only
// timestamps falling edges (EXTI) and decodes the intervals. There is no return
// channel -- the engine runs in carousel mode (FSH_CAROUSEL), so the sender
// broadcasts announce+blocks round-robin and the receiver's block bitmap fills
// in over passes. See the app README for the full design.
//
//   - fsh_transport_send() (declared in share.h) is wired as the engine's
//     cb_send_bytes; on the sender it queues a frame for the bit-bang worker, on
//     the receiver it is a no-op (the receiver never transmits).
//   - Each decoded packet is delivered to fsh_receive_callback() from the
//     delivery worker thread (never from the EXTI interrupt).

#include <stdint.h>
#include <stddef.h>

typedef enum {
    GpioTransportModeSender, // bit-bangs the pulse stream (open-drain, idle high)
    GpioTransportModeReceiver, // timestamps falling edges (EXTI) and decodes
} GpioTransportMode;

// Allocate resources and start the GPIO stack in the given role.
void gpio_transport_init(GpioTransportMode mode);

// Stop the GPIO stack, reset the pin and free resources. The caller MUST have
// already stopped any thread that calls fsh_transport_send().
void gpio_transport_deinit(void);

// Receiver only: stop the falling-edge capture after the transfer finalizes,
// without tearing the transport down (mirrors the NFC/RFID stop_field). No-op on
// the sender.
void gpio_transport_stop_field(void);
