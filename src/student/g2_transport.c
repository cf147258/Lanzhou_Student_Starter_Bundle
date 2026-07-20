#include "course.h"

#include <string.h>

uint16_t crc16_ccitt_false(const uint8_t *bytes, size_t length) {
    (void)bytes;
    (void)length;
    /* DAY1_G2_TODO_A: CRC-16/CCITT-FALSE, poly 0x1021, init 0xFFFF. */
    return 0;
}

bool frame_encode(const ArmCommand *command, uint8_t *out,
                      size_t capacity, size_t *written) {
    (void)command;
    (void)out;
    (void)capacity;
    if (written != NULL) {
        *written = 0;
    }
    /* DAY1_G2_TODO_B: write the frozen little-endian layout and type byte. */
    return false;
}

RxVerdict frame_decode(const uint8_t *bytes, size_t length,
                              uint64_t last_seq, ArmCommand *out) {
    (void)bytes;
    (void)length;
    (void)last_seq;
    if (out != NULL) {
        memset(out, 0, sizeof(*out));
    }
    /* DAY1_G2_TODO_C: length -> version/type -> CRC -> sequence -> payload. */
    return COURSE_RX_REJECT;
}

void frame_parser_init(FrameParser *parser) {
    if (parser != NULL) {
        memset(parser, 0, sizeof(*parser));
        parser->syncing = true;
    }
}

RxVerdict frame_feed(FrameParser *parser, uint8_t byte,
                            uint64_t last_seq, ArmCommand *out) {
    (void)parser;
    (void)byte;
    (void)last_seq;
    (void)out;
    /* DAY1_G2_TODO_D: sync, retain partial bytes, decode and resync. */
    return COURSE_RX_NEED_MORE;
}

Reason freshness_accept(FreshnessGate *gate,
                               const ArmCommand *command,
                               uint64_t now_ns) {
    (void)gate;
    (void)command;
    (void)now_ns;
    /* DAY2_G2_TODO_A: sequence, clock, age, then latest-value update. */
    return COURSE_REASON_STUDENT_TODO;
}

Reason transport_on_message(FreshnessGate *gate,
                                   const uint8_t *bytes, size_t length,
                                   uint64_t now_ns, TraceRow *trace,
                                   ArmCommand *accepted) {
    (void)gate;
    (void)bytes;
    (void)length;
    (void)now_ns;
    if (trace != NULL) {
        memset(trace, 0, sizeof(*trace));
        trace->verdict = COURSE_VERDICT_REJECT;
        trace->reason = COURSE_REASON_STUDENT_TODO;
    }
    if (accepted != NULL) {
        memset(accepted, 0, sizeof(*accepted));
    }
    /* DAY2_G2_TODO_B: decode, stamp t_rx, gate, latest, trace and ACK. */
    /* DAY3_G2_TODO_A: emit a complete four-stamp trace row for runtime
       handoff, including rejected messages. */
    /* DAY4_G2_TODO_A: preserve corrupt and stale rejection reasons. */
    /* DAY5_G2_TODO_A: preserve emergency source age for priority dispatch. */
    return COURSE_REASON_STUDENT_TODO;
}
