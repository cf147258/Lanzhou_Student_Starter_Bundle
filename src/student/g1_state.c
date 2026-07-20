#include "course.h"

#include <stddef.h>

bool state_valid(const ArmState *state, const ArmState *previous) {
    (void)state;
    (void)previous;
    /* DAY1_G1_TODO_A: finite values, coordinate frame, and joint limits. */
    /* DAY2_G1_TODO_A: reject non-increasing sequence or monotonic time. */
    /* DAY3_G1_TODO_A: accept a valid source progression even when a slow
       consumer observes a larger sequence and time gap. */
    /* DAY4_G1_TODO_A: reject excessive state uncertainty before handoff to
       the safety gate. */
    /* DAY5_G1_TODO_A: reject sequence or time regression during replay across
       the controller-host boundary. */
    return false; /* Fail closed until the state contract is implemented. */
}

void twin_step(ArmState *state, const ArmCommand *command,
                   int64_t step_ns) {
    (void)state;
    (void)command;
    (void)step_ns;
    /* DAY1_G1_TODO_B: deterministic fixed-step simulator update. */
}

uint64_t state_schema_hash(void) {
    /* DAY1_G1_TODO_C: hash the frozen field names, units and dimensions. */
    return 0;
}
