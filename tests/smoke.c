#include "course.h"
#include "test_harness.h"

#include <stdio.h>
#include <string.h>

int verify_smoke(TestContext *context) {
    Simulator first;
    Simulator second;
    ActuatorWriter first_writer;
    ActuatorWriter second_writer;
    ArmCommand command;
    SafetyDecision approved = {
        .verdict = COURSE_VERDICT_APPROVE,
        .reason = COURSE_REASON_NONE,
    };
    SafetyDecision rejected = {
        .verdict = COURSE_VERDICT_REJECT,
        .reason = COURSE_REASON_INTERNAL,
    };
    FILE *manifest;

    memset(&command, 0, sizeof(command));
    command.seq = 1;
    command.trace_id = context->seed;
    simulator_reset(&first);
    simulator_reset(&second);
    memset(&first_writer, 0, sizeof(first_writer));
    memset(&second_writer, 0, sizeof(second_writer));
    first_writer.writer_id = 7;
    first_writer.write_count = 1;
    first_writer.audit_count = 1;
    first_writer.audit[0].trace_id = command.trace_id;
    first_writer.audit[0].command_seq = command.seq;
    first_writer.audit[0].verdict = COURSE_VERDICT_APPROVE;
    first_writer.audit[0].reason = COURSE_REASON_NONE;
    first_writer.audit[0].writer_id = first_writer.writer_id;
    second_writer = first_writer;
    test_check(context, context->seed == COURSE_FIXED_SEED,
                   "fixed seed is visible");
    test_check(context,
                   simulator_commit_from_writer(&first, &first_writer,
                                                &command, approved),
                   "simulator accepts an audited writer commit");
    test_check(context,
                   simulator_commit_from_writer(&second, &second_writer,
                                                &command, approved),
                   "second deterministic simulator run completes");
    test_check(context,
                   memcmp(&first, &second, sizeof(first)) == 0,
                   "same seed and command produce identical simulator state");
    test_check(context,
                   !simulator_commit_from_writer(&first, &first_writer,
                                                 &command, rejected),
                   "simulator blocks an unapproved direct commit");

    manifest = test_open_evidence(context, "manifest.json");
    if (manifest != NULL) {
        fprintf(manifest,
                "{\n  \"seed\": %llu,\n  \"language\": \"C\",\n"
                "  \"platform\": \"Linux container\",\n"
                "  \"starter_state\": \"compiles; daily gates intentionally red\"\n}\n",
                (unsigned long long)context->seed);
        fclose(manifest);
        test_check(context, true, "manifest written");
    } else {
        test_check(context, false, "manifest written");
    }
    return test_finish(context);
}
