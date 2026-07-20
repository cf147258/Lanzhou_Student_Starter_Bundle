#define _POSIX_C_SOURCE 200809L

#include "course.h"

#include <string.h>
#include <time.h>

uint64_t monotonic_ns(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        return 0;
    }
    return (uint64_t)ts.tv_sec * UINT64_C(1000000000) + (uint64_t)ts.tv_nsec;
}

const char *rx_verdict_name(RxVerdict verdict) {
    static const char *const names[] = {
        "NEED_MORE", "ACCEPT", "NACK_LENGTH", "NACK_CRC",
        "NACK_SEQUENCE", "NACK_VERSION", "REJECT"
    };
    const size_t count = sizeof(names) / sizeof(names[0]);
    return (unsigned)verdict < count ? names[verdict] : "UNKNOWN_RX";
}

const char *verdict_name(Verdict verdict) {
    static const char *const names[] = {
        "REJECT", "APPROVE", "FALLBACK", "DISCARD"
    };
    const size_t count = sizeof(names) / sizeof(names[0]);
    return (unsigned)verdict < count ? names[verdict] : "UNKNOWN_VERDICT";
}

const char *reason_name(Reason reason) {
    static const char *const names[] = {
        "NONE", "STUDENT_TODO", "BAD_FRAME", "NOT_NEW", "CLOCK_ERROR",
        "STALE_COMMAND", "STALE_STATE", "NONFINITE", "JOINT_RANGE",
        "RATE_LIMIT", "UNCERTAINTY", "QUEUE_FULL", "QUEUE_EXPIRED",
        "DEADLINE_MISS", "CONTROLLER_TIMEOUT", "CONTROLLER_INVALID",
        "LOW_CONFIDENCE", "CANCELLED", "INTERNAL"
    };
    const size_t count = sizeof(names) / sizeof(names[0]);
    return (unsigned)reason < count ? names[reason] : "UNKNOWN_REASON";
}

void simulator_reset(Simulator *simulator) {
    if (simulator != NULL) {
        memset(simulator, 0, sizeof(*simulator));
    }
}

bool simulator_commit_from_writer(Simulator *simulator,
                                  const ActuatorWriter *writer,
                                  const ArmCommand *command,
                                  SafetyDecision decision) {
    const AuditRow *audit;

    if (simulator == NULL || writer == NULL || command == NULL) {
        return false;
    }
    if (decision.verdict != COURSE_VERDICT_APPROVE ||
        decision.reason != COURSE_REASON_NONE || writer->writer_id == 0 ||
        writer->audit_count == 0 || writer->write_count == 0) {
        simulator->rejected_write_attempts++;
        return false;
    }
    audit = &writer->audit[writer->audit_count - 1];
    if (audit->trace_id != command->trace_id ||
        audit->command_seq != command->seq ||
        audit->verdict != COURSE_VERDICT_APPROVE ||
        audit->reason != COURSE_REASON_NONE ||
        audit->writer_id != writer->writer_id ||
        writer->write_count != simulator->write_count + 1) {
        simulator->rejected_write_attempts++;
        return false;
    }
    simulator->write_count++;
    simulator->last_command_seq = command->seq;
    simulator->last_trace_id = command->trace_id;
    simulator->last_writer_id = writer->writer_id;
    return true;
}
