#pragma once

// UART transport for Flipper Share: a packet pipe over USART1 (GPIO pins 13/14)
// at UART_TP_BAUD, full duplex. Packets are COBS-framed (uart_framing.c) with a
// 0x00 delimiter. The link is symmetric — both roles run RX permanently — so
// there is no mode parameter and no field/turnaround handling.
//
//   - fsh_transport_send() (declared in share.h) is wired as the engine's
//     cb_send_bytes; it COBS-encodes and blocking-writes the frame (the blocking
//     TX paces the engine's send loop; no mailbox needed).
//   - Each decoded packet is delivered to fsh_receive_callback() from the
//     UartRxWorker thread (never from the RX interrupt).

#include <stdbool.h>

// Acquire USART1, start the deframer and async RX. Returns false if the port is
// busy / cannot be acquired — the scene then shows "UART port busy" and does not
// start the protocol worker.
bool uart_transport_init(void);

// Stop RX, join the worker, release the port and re-enable the expansion service.
// The caller MUST have already stopped every thread that calls
// uart_transport_send().
void uart_transport_deinit(void);
