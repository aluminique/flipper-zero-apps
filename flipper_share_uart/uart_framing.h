#pragma once

// COBS (Consistent Overhead Byte Stuffing) codec for the UART byte stream. Pure
// C, NO furi includes, so it round-trips on the host test harness
// (tools/framing_test.c). 0x00 is the frame delimiter; COBS-encoded output never
// contains a 0x00, so the delimiter unambiguously ends a frame on the wire.

#include <stdint.h>
#include <stddef.h>

// Encode `len` input bytes into `out` (which must hold at least
// len + len/254 + 1 bytes). Returns the encoded length. The caller appends the
// 0x00 delimiter separately.
size_t uart_cobs_encode(const uint8_t* in, size_t len, uint8_t* out);

// Decode `len` COBS bytes (WITHOUT the trailing 0x00 delimiter) into `out`.
// Returns the decoded length, or 0 on malformed input (an embedded 0x00 or a
// block that overruns the buffer).
size_t uart_cobs_decode(const uint8_t* in, size_t len, uint8_t* out);
