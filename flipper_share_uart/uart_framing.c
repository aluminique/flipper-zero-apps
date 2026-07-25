#include "uart_framing.h"

// Classic "StuffData" COBS encoder: emit a code byte, then runs of non-zero
// bytes; a zero in the input closes the current run (the code byte is the
// distance to it), and a full 254-byte run is flushed with a 0xFF code.
size_t uart_cobs_encode(const uint8_t* in, size_t len, uint8_t* out) {
    size_t write = 0;
    size_t code_i = write++; // reserve the first code byte
    uint8_t code = 1;

    for(size_t read = 0; read < len; read++) {
        if(in[read] == 0) {
            out[code_i] = code;
            code_i = write++;
            code = 1;
        } else {
            out[write++] = in[read];
            code++;
            if(code == 0xFF) {
                out[code_i] = code;
                code_i = write++;
                code = 1;
            }
        }
    }
    out[code_i] = code;
    return write;
}

size_t uart_cobs_decode(const uint8_t* in, size_t len, uint8_t* out) {
    size_t read = 0, write = 0;

    while(read < len) {
        uint8_t code = in[read++];
        if(code == 0) return 0; // a literal zero must never appear inside COBS data

        for(uint8_t i = 1; i < code; i++) {
            if(read >= len) return 0; // block overruns the input
            uint8_t b = in[read++];
            if(b == 0) return 0; // no zeros allowed inside a block
            out[write++] = b;
        }
        // Each group implies a trailing zero, except a full (0xFF) group and the
        // final group (whose zero is the frame delimiter, not present here).
        if(code != 0xFF && read < len) out[write++] = 0;
    }
    return write;
}
