#pragma once

// Timing/behaviour knobs for the GPIO pulse-distance modem. Pure constants, no
// furi, so this header is shared verbatim by the firmware and the host test
// harness (tools/modem_test.c).
//
// The line idles HIGH (open-drain + pull-up). The sender marks each bit boundary
// with a short LOW "tick" (a fast, actively-driven falling edge) and encodes the
// bit in the TIME BETWEEN consecutive falling edges (pulse-distance, like an NEC
// IR remote). Only falling edges are ever timed, so the slow open-drain rise
// (RC through the internal pull-up) is never on the critical path -- it just has
// to finish inside the following gap before the next tick. All decisions are a
// simple "short vs long vs very-long interval" with wide bands, so there is very
// little for jitter to break.
//
// All values are microseconds, measured fall-to-fall.

// Low-pulse width of one tick: long enough for a clean falling edge and for the
// line to be unambiguously low, short compared to the smallest gap.
#define GPIO_MODEM_TICK_US 3u

// Nominal fall-to-fall periods the encoder emits.
#define GPIO_MODEM_BIT0_US 13u // short period  -> data bit 0
#define GPIO_MODEM_BIT1_US 23u // long period   -> data bit 1
#define GPIO_MODEM_SYNC_US 38u // sync period   -> frame start marker
#define GPIO_MODEM_IDLE_US 70u // inter-frame idle the sender holds high between frames

// Decoder classification thresholds on the measured fall-to-fall period. The
// bands are [TMIN,T01)=bit0, [T01,T1S)=bit1, [T1S,TSI)=sync, everything else
// (glitch below TMIN, or idle/garbage at/above TSI) -> resync. Each threshold
// sits midway between two nominal periods, so there is ~5 us of slack on every
// side against ISR jitter.
#define GPIO_MODEM_TMIN_US 6u // below this: runt glitch -> resync
#define GPIO_MODEM_T01_US 18u // bit0 | bit1 split   (between BIT0 and BIT1)
#define GPIO_MODEM_T1S_US 30u // bit1 | sync split   (between BIT1 and SYNC)
#define GPIO_MODEM_TSI_US 52u // sync | idle split   (between SYNC and IDLE)

// Hard cap on a packet the modem will carry. Kept just above the largest real
// flipper-share packet (a 64-byte DATA packet is 73 bytes) rather than the full
// 255 a length byte allows: a noise false-lock reads a random length byte and
// would otherwise stay blind for that many bytes, so a tight cap makes a bogus
// read bail out fast (len > cap -> immediate resync). The engine still validates
// the real packet via its own CRC16.
#define GPIO_MODEM_MAX_PACKET 80u
