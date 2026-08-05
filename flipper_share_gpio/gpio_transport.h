#pragma once

// 1-Wire transport for Flipper Share: a packet pipe over a plain GPIO wire
// (header pin 4 / PA4, jumpered pin 4 <-> pin 4 plus GND). Point-to-point link,
// exactly two devices, no ROM search. This is the generic-wire sibling of
// flipper_share_ibutton, which runs the same protocol on the iButton pad; here
// the bus is an ordinary GPIO, so there is no pad divider and the host supplies
// the pull-up from the STM32 internal resistor.
//
// Role mapping (mirrors the other transports, where the sender is the passive
// side): the RECEIVER is the 1-Wire HOST and drives the bus and all timing; the
// SENDER is the 1-Wire SLAVE (emulator) and answers in read slots.
//
//   - fsh_transport_send() (declared in share.h) is wired as the engine's
//     cb_send_bytes; it enqueues the packet for the next transaction.
//   - Each received packet is delivered to fsh_receive_callback() (declared in
//     share.h) from a thread context (the RX worker on the slave, the host
//     worker on the host) — never from the 1-Wire interrupt.
//
// Contact bounce / separation is handled by the link layer: every transaction
// is a fresh reset+presence, and the engine's block bitmap resumes the transfer
// after any drop.

#include <stdint.h>
#include <stddef.h>

typedef enum {
    GpioTransportModeSlave, // sender: 1-Wire slave (emulator), answers the host
    GpioTransportModeHost, // receiver: 1-Wire host, drives the bus
} GpioTransportMode;

// Allocate resources and start the 1-Wire stack in the given role.
void gpio_transport_init(GpioTransportMode mode);

// Stop the 1-Wire stack and free resources. The caller MUST have already
// stopped any thread that calls fsh_transport_send().
void gpio_transport_deinit(void);

// Pause bus activity once the transfer is finished, without tearing the
// transport down (that stays with gpio_transport_deinit on the scene
// thread). Thread-safe: only sets a flag observed by the host worker. No-op for
// the slave role (the emulation simply stops answering after deinit).
void gpio_transport_stop_field(void);
