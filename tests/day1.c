#include "course.h"
#include "test_harness.h"

#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
    MAX_POSES = 8,
    MAX_FAULTS = 8,
    NAME_CAPACITY = 40,
    ORDER_CAPACITY = 40,
    VERDICT_CAPACITY = 40
};

typedef struct {
    char name[NAME_CAPACITY];
    ArmState state;
} PoseFixture;

typedef struct {
    char name[NAME_CAPACITY];
    uint64_t seed;
    size_t byte_offset;
    unsigned mask;
    size_t truncate_to;
    char delivery_order[ORDER_CAPACITY];
    char expected_name[VERDICT_CAPACITY];
    size_t expected_writes;
} FaultFixture;

typedef struct {
    FaultFixture fixture;
    RxVerdict expected;
    RxVerdict actual;
    size_t actuator_writes;
    bool frame_ready;
} FaultResult;

static FILE *open_fixture(const char *relative_path) {
    char fallback[512];
    FILE *file = fopen(relative_path, "r");
    int count;

    if (file != NULL) {
        return file;
    }
    count = snprintf(fallback, sizeof(fallback),
                     "Lanzhou_Student_Starter_Bundle/%s", relative_path);
    if (count < 0 || (size_t)count >= sizeof(fallback)) {
        return NULL;
    }
    return fopen(fallback, "r");
}

static size_t read_pose_fixtures(PoseFixture *poses, size_t capacity) {
    char line[1024];
    FILE *file = open_fixture("fixtures/day1/known_poses.csv");
    size_t count = 0;

    if (file == NULL || fgets(line, sizeof(line), file) == NULL) {
        if (file != NULL) {
            fclose(file);
        }
        return 0;
    }

    while (count < capacity && fgets(line, sizeof(line), file) != NULL) {
        PoseFixture pose;
        unsigned frame_id;
        int fields;

        memset(&pose, 0, sizeof(pose));
        fields = sscanf(
            line,
            "%39[^,],%" SCNu64 ",%" SCNd64 ",%u,%lf,%lf,%lf,%lf,%lf,%lf,%f,%f,%f",
            pose.name, &pose.state.seq, &pose.state.t_mono_ns, &frame_id,
            &pose.state.q_rad[0], &pose.state.q_rad[1],
            &pose.state.q_rad[2], &pose.state.dq_rad_s[0],
            &pose.state.dq_rad_s[1], &pose.state.dq_rad_s[2],
            &pose.state.sigma_q_rad[0], &pose.state.sigma_q_rad[1],
            &pose.state.sigma_q_rad[2]);
        if (fields != 13 || frame_id > UINT8_MAX) {
            fclose(file);
            return 0;
        }
        pose.state.frame_id = (uint8_t)frame_id;
        poses[count++] = pose;
    }
    fclose(file);
    return count;
}

static size_t read_fault_fixtures(FaultFixture *faults, size_t capacity) {
    char line[1024];
    FILE *file = open_fixture("fixtures/day1/faults.csv");
    size_t count = 0;

    if (file == NULL || fgets(line, sizeof(line), file) == NULL) {
        if (file != NULL) {
            fclose(file);
        }
        return 0;
    }

    while (count < capacity && fgets(line, sizeof(line), file) != NULL) {
        FaultFixture fault;
        int fields;

        memset(&fault, 0, sizeof(fault));
        fields = sscanf(line,
                        "%39[^,],%" SCNu64 ",%zu,%u,%zu,%39[^,],%39[^,],%zu",
                        fault.name, &fault.seed, &fault.byte_offset,
                        &fault.mask, &fault.truncate_to,
                        fault.delivery_order, fault.expected_name,
                        &fault.expected_writes);
        if (fields != 8) {
            fclose(file);
            return 0;
        }
        faults[count++] = fault;
    }
    fclose(file);
    return count;
}

static RxVerdict verdict_from_name(const char *name) {
    if (strcmp(name, "ACCEPT") == 0) {
        return COURSE_RX_ACCEPT;
    }
    if (strcmp(name, "NACK_LENGTH") == 0) {
        return COURSE_RX_NACK_LENGTH;
    }
    if (strcmp(name, "NACK_CRC") == 0) {
        return COURSE_RX_NACK_CRC;
    }
    if (strcmp(name, "NACK_SEQUENCE") == 0) {
        return COURSE_RX_NACK_SEQUENCE;
    }
    if (strcmp(name, "NACK_VERSION") == 0) {
        return COURSE_RX_NACK_VERSION;
    }
    return COURSE_RX_REJECT;
}

static uint16_t load_u16_le(const uint8_t *bytes) {
    return (uint16_t)bytes[0] | (uint16_t)((uint16_t)bytes[1] << 8);
}

static uint64_t load_u64_le(const uint8_t *bytes) {
    uint64_t value = 0;

    for (size_t index = 0; index < 8; index++) {
        value |= (uint64_t)bytes[index] << (8U * index);
    }
    return value;
}

static bool command_wire_equal(const ArmCommand *left,
                               const ArmCommand *right) {
    if (left == NULL || right == NULL || left->seq != right->seq ||
        left->t_source_ns != right->t_source_ns ||
        left->trace_id != right->trace_id ||
        left->emergency != right->emergency) {
        return false;
    }
    for (size_t joint = 0; joint < COURSE_ARM_DOF; joint++) {
        if (left->q_target_rad[joint] != right->q_target_rad[joint]) {
            return false;
        }
    }
    return true;
}

static bool state_fields_equal(const ArmState *left, const ArmState *right) {
    if (left == NULL || right == NULL || left->seq != right->seq ||
        left->t_mono_ns != right->t_mono_ns ||
        left->frame_id != right->frame_id) {
        return false;
    }
    for (size_t joint = 0; joint < COURSE_ARM_DOF; joint++) {
        if (left->q_rad[joint] != right->q_rad[joint] ||
            left->dq_rad_s[joint] != right->dq_rad_s[joint] ||
            left->sigma_q_rad[joint] != right->sigma_q_rad[joint]) {
            return false;
        }
    }
    return true;
}

static double target_distance_squared(const ArmState *state,
                                      const ArmCommand *command) {
    double distance = 0.0;

    for (size_t joint = 0; joint < COURSE_ARM_DOF; joint++) {
        const double delta = command->q_target_rad[joint] - state->q_rad[joint];
        distance += delta * delta;
    }
    return distance;
}

static bool frame_layout_matches(const uint8_t *frame, size_t written,
                                 const ArmCommand *command) {
    if (frame == NULL || command == NULL || written != COURSE_FRAME_V1_LEN ||
        frame[0] != COURSE_FRAME_SYNC_0 ||
        frame[1] != COURSE_FRAME_SYNC_1 ||
        frame[2] != COURSE_FRAME_VERSION ||
        frame[3] != (command->emergency ? COURSE_FRAME_TYPE_ESTOP
                                        : COURSE_FRAME_TYPE_COMMAND) ||
        load_u16_le(frame + 4) != COURSE_COMMAND_PAYLOAD_LEN ||
        load_u64_le(frame + 6) != command->seq ||
        load_u64_le(frame + 14) != command->t_source_ns ||
        load_u64_le(frame + 22) != command->trace_id) {
        return false;
    }

    for (size_t joint = 0; joint < COURSE_ARM_DOF; joint++) {
        uint64_t expected_bits = 0;
        memcpy(&expected_bits, &command->q_target_rad[joint],
               sizeof(expected_bits));
        if (load_u64_le(frame + 30 + joint * 8) != expected_bits) {
            return false;
        }
    }
    return true;
}

static bool frame_crc_matches(const uint8_t *frame, size_t written) {
    uint16_t expected;

    if (frame == NULL || written != COURSE_FRAME_V1_LEN) {
        return false;
    }
    expected = crc16_ccitt_false(frame + 2, COURSE_FRAME_CRC_OFFSET - 2);
    return load_u16_le(frame + COURSE_FRAME_CRC_OFFSET) == expected;
}

static bool feed_frame(const uint8_t *frame, size_t length, uint64_t last_seq,
                       ArmCommand *decoded, RxVerdict *last_terminal) {
    FrameParser parser;
    bool accepted = false;

    frame_parser_init(&parser);
    if (last_terminal != NULL) {
        *last_terminal = COURSE_RX_NEED_MORE;
    }
    for (size_t index = 0; index < length; index++) {
        const RxVerdict verdict =
            frame_feed(&parser, frame[index], last_seq, decoded);
        if (verdict != COURSE_RX_NEED_MORE && last_terminal != NULL) {
            *last_terminal = verdict;
        }
        if (verdict == COURSE_RX_ACCEPT) {
            accepted = true;
        }
    }
    return accepted;
}

static bool feed_truncated_then_full(const uint8_t *frame, size_t full_length,
                                     size_t truncated_length,
                                     uint64_t last_seq) {
    FrameParser parser;
    ArmCommand decoded;
    bool accepted = false;

    if (frame == NULL || truncated_length >= full_length) {
        return false;
    }
    memset(&decoded, 0, sizeof(decoded));
    frame_parser_init(&parser);
    for (size_t index = 0; index < truncated_length; index++) {
        (void)frame_feed(&parser, frame[index], last_seq, &decoded);
    }
    for (size_t index = 0; index < full_length; index++) {
        if (frame_feed(&parser, frame[index], last_seq, &decoded) ==
            COURSE_RX_ACCEPT) {
            accepted = true;
        }
    }
    return accepted;
}

static void check_or_todo(TestContext *context, bool implemented,
                          bool condition, const char *label) {
    if (!implemented) {
        test_todo(context, label);
        return;
    }
    test_check(context, condition, label);
}

static bool write_baseline_trace(TestContext *context,
                                 const PoseFixture *poses,
                                 const bool *validity, size_t pose_count,
                                 uint64_t schema_hash,
                                 const ArmState *twin_first,
                                 const ArmState *twin_second) {
    FILE *file = test_open_evidence(context, "baseline-trace.csv");

    if (file == NULL) {
        return false;
    }
    fprintf(file,
            "case,run,seed,seq,t_mono_ns,frame_id,q0_rad,q1_rad,q2_rad,"
            "dq0_rad_s,dq1_rad_s,dq2_rad_s,sigma0_rad,sigma1_rad,"
            "sigma2_rad,state_valid,schema_hash\n");
    for (size_t index = 0; index < pose_count; index++) {
        const ArmState *state = &poses[index].state;
        fprintf(file,
                "%s,fixture,%" PRIu64 ",%" PRIu64 ",%" PRId64
                ",%u,%.9f,%.9f,%.9f,%.9f,%.9f,%.9f,%.9g,%.9g,%.9g,"
                "%s,%" PRIu64 "\n",
                poses[index].name, context->seed, state->seq,
                state->t_mono_ns, (unsigned)state->frame_id,
                state->q_rad[0], state->q_rad[1], state->q_rad[2],
                state->dq_rad_s[0], state->dq_rad_s[1],
                state->dq_rad_s[2], (double)state->sigma_q_rad[0],
                (double)state->sigma_q_rad[1],
                (double)state->sigma_q_rad[2],
                validity[index] ? "true" : "false", schema_hash);
    }
    if (twin_first != NULL && twin_second != NULL) {
        fprintf(file,
                "fixed_step_a,run_a,%" PRIu64 ",%" PRIu64 ",%" PRId64
                ",%u,%.9f,%.9f,%.9f,%.9f,%.9f,%.9f,%.9g,%.9g,%.9g,"
                "not-applicable,%" PRIu64 "\n",
                context->seed, twin_first->seq, twin_first->t_mono_ns,
                (unsigned)twin_first->frame_id, twin_first->q_rad[0],
                twin_first->q_rad[1], twin_first->q_rad[2],
                twin_first->dq_rad_s[0], twin_first->dq_rad_s[1],
                twin_first->dq_rad_s[2],
                (double)twin_first->sigma_q_rad[0],
                (double)twin_first->sigma_q_rad[1],
                (double)twin_first->sigma_q_rad[2], schema_hash);
        fprintf(file,
                "fixed_step_b,run_b,%" PRIu64 ",%" PRIu64 ",%" PRId64
                ",%u,%.9f,%.9f,%.9f,%.9f,%.9f,%.9f,%.9g,%.9g,%.9g,"
                "not-applicable,%" PRIu64 "\n",
                context->seed, twin_second->seq, twin_second->t_mono_ns,
                (unsigned)twin_second->frame_id, twin_second->q_rad[0],
                twin_second->q_rad[1], twin_second->q_rad[2],
                twin_second->dq_rad_s[0], twin_second->dq_rad_s[1],
                twin_second->dq_rad_s[2],
                (double)twin_second->sigma_q_rad[0],
                (double)twin_second->sigma_q_rad[1],
                (double)twin_second->sigma_q_rad[2], schema_hash);
    }
    return fclose(file) == 0;
}

static bool write_corruption_results(TestContext *context,
                                     const FaultResult *results,
                                     size_t result_count) {
    FILE *file = test_open_evidence(context, "corruption-results.csv");

    if (file == NULL) {
        return false;
    }
    fprintf(file,
            "case,seed,byte_offset,mask,truncate_to,delivery_order,"
            "expected_verdict,actual_verdict,actuator_writes,pass\n");
    for (size_t index = 0; index < result_count; index++) {
        const FaultResult *result = &results[index];
        const bool pass = result->frame_ready &&
                          result->actual == result->expected &&
                          result->actuator_writes ==
                              result->fixture.expected_writes;
        fprintf(file,
                "%s,%" PRIu64 ",%zu,0x%02X,%zu,%s,%s,%s,%zu,%s\n",
                result->fixture.name, result->fixture.seed,
                result->fixture.byte_offset, result->fixture.mask,
                result->fixture.truncate_to,
                result->fixture.delivery_order,
                rx_verdict_name(result->expected),
                rx_verdict_name(result->actual), result->actuator_writes,
                pass ? "true" : "false");
    }
    return fclose(file) == 0;
}

static bool write_wire_contract(TestContext *context) {
    FILE *file = test_open_evidence(context, "wire-contract.txt");

    if (file == NULL) {
        return false;
    }
    fprintf(file,
            "FrameV1 wire contract\n"
            "sync: offsets 0..1 = 0x%02X 0x%02X\n"
            "version: offset 2 = %u\n"
            "type: offset 3 = %u (command) or %u (emergency stop)\n"
            "payload_length: offsets 4..5, uint16 little-endian = %u\n"
            "sequence: offsets 6..13, uint64 little-endian\n"
            "source_time_ns: offsets 14..21, uint64 little-endian\n"
            "trace_id: offsets 22..29, uint64 little-endian\n"
            "joint_targets_rad: offsets 30..53, three IEEE-754 binary64 "
            "values, little-endian\n"
            "crc: offsets 54..55, uint16 little-endian\n"
            "crc_variant: CRC-16/CCITT-FALSE, poly=0x1021, init=0xFFFF, "
            "refin=false, refout=false, xorout=0x0000\n"
            "crc_coverage: offsets 2..53\n"
            "frame_length: %u bytes\n"
            "acceptance_order: length, version, CRC, sequence, payload\n"
            "safety: no ACCEPT means no actuator write\n",
            COURSE_FRAME_SYNC_0, COURSE_FRAME_SYNC_1,
            COURSE_FRAME_VERSION, COURSE_FRAME_TYPE_COMMAND,
            COURSE_FRAME_TYPE_ESTOP,
            COURSE_COMMAND_PAYLOAD_LEN, COURSE_FRAME_V1_LEN);
    return fclose(file) == 0;
}

static bool write_manifest(TestContext *context) {
    const char *image = getenv("COURSE_CONTAINER_IMAGE");
    const char *commit = getenv("COURSE_COMMIT");
    FILE *file = test_open_evidence(context, "manifest.json");

    if (file == NULL) {
        return false;
    }
    fprintf(file,
            "{\n"
            "  \"gate\": \"verify-day1\",\n"
            "  \"group\": \"%s\",\n"
            "  \"day\": 1,\n"
            "  \"seed\": %" PRIu64 ",\n"
            "  \"input_tag\": \"starter_repo\",\n"
            "  \"output_tag\": \"frame_v1\",\n"
            "  \"fixture_version\": \"1.0.0\",\n"
            "  \"language\": \"C\",\n"
            "  \"platform\": \"Linux container\",\n"
            "  \"container_image\": \"%s\",\n"
            "  \"commit\": \"%s\",\n"
            "  \"simulator_only\": true,\n"
            "  \"frame_version\": %u,\n"
            "  \"frame_length\": %u,\n"
            "  \"checks_before_manifest\": %zu,\n"
            "  \"failures_before_manifest\": %zu,\n"
            "  \"todo_before_manifest\": %zu\n"
            "}\n",
            context->group, context->seed,
            image == NULL ? "NOT_RECORDED" : image,
            commit == NULL ? "NOT_A_GIT_CHECKOUT" : commit,
            COURSE_FRAME_VERSION,
            COURSE_FRAME_V1_LEN, context->checks, context->failures,
            context->todo_failures);
    return fclose(file) == 0;
}

int verify_day1(TestContext *context) {
    PoseFixture poses[MAX_POSES];
    FaultFixture faults[MAX_FAULTS];
    FaultResult results[MAX_FAULTS];
    bool pose_validity[MAX_POSES];
    size_t pose_count;
    size_t fault_count;
    uint64_t schema_hash;
    ArmState twin_initial;
    ArmState twin_first;
    ArmState twin_second;
    ArmCommand command;
    ArmCommand decoded;
    uint8_t frame[COURSE_FRAME_V1_LEN];
    size_t written = 0;
    uint16_t crc_known;
    bool encoded;
    RxVerdict roundtrip;
    RxVerdict parser_terminal;
    bool parser_accept;
    bool parser_resync;
    bool any_pose_valid = false;
    bool twin_changed;
    bool state_stub;
    bool frame_stub;

    memset(poses, 0, sizeof(poses));
    memset(faults, 0, sizeof(faults));
    memset(results, 0, sizeof(results));
    memset(pose_validity, 0, sizeof(pose_validity));
    memset(&twin_initial, 0, sizeof(twin_initial));
    memset(&twin_first, 0, sizeof(twin_first));
    memset(&twin_second, 0, sizeof(twin_second));
    memset(&command, 0, sizeof(command));
    memset(&decoded, 0, sizeof(decoded));
    memset(frame, 0, sizeof(frame));

    pose_count = read_pose_fixtures(poses, MAX_POSES);
    fault_count = read_fault_fixtures(faults, MAX_FAULTS);
    schema_hash = state_schema_hash();

    for (size_t index = 0; index < pose_count; index++) {
        ArmState previous = poses[index].state;
        previous.seq = poses[index].state.seq == 0
                           ? 0
                           : poses[index].state.seq - 1;
        previous.t_mono_ns = poses[index].state.t_mono_ns -
                             COURSE_FIXED_STEP_NS;
        pose_validity[index] = state_valid(&poses[index].state, &previous);
        any_pose_valid = any_pose_valid || pose_validity[index];
    }

    if (pose_count > 0) {
        twin_initial = poses[0].state;
    } else {
        twin_initial.frame_id = COURSE_FRAME_ID_BASE;
        twin_initial.t_mono_ns = COURSE_FIXED_STEP_NS;
        twin_initial.seq = 1;
    }
    twin_first = twin_initial;
    twin_second = twin_initial;
    command.seq = twin_initial.seq + 1;
    command.t_source_ns = (uint64_t)(twin_initial.t_mono_ns +
                                    COURSE_FIXED_STEP_NS);
    command.trace_id = context->seed;
    for (size_t joint = 0; joint < COURSE_ARM_DOF; joint++) {
        command.q_target_rad[joint] =
            pose_count > 1 ? poses[1].state.q_rad[joint]
                           : 0.25 * (double)(joint + 1);
    }
    twin_step(&twin_first, &command, COURSE_FIXED_STEP_NS);
    twin_step(&twin_second, &command, COURSE_FIXED_STEP_NS);
    twin_changed = !state_fields_equal(&twin_initial, &twin_first);
    state_stub = schema_hash == 0 && !any_pose_valid && !twin_changed;

    crc_known = crc16_ccitt_false((const uint8_t *)"123456789", 9);
    encoded = frame_encode(&command, frame, sizeof(frame), &written);
    roundtrip = frame_decode(frame, written, command.seq - 1, &decoded);
    parser_accept = feed_frame(frame, written, command.seq - 1, &decoded,
                               &parser_terminal);
    parser_resync = written == COURSE_FRAME_V1_LEN &&
                    feed_truncated_then_full(frame, written, 31,
                                             command.seq - 1);
    frame_stub = !encoded && written == 0 &&
                 roundtrip == COURSE_RX_REJECT &&
                 parser_terminal == COURSE_RX_NEED_MORE;

    for (size_t index = 0; index < fault_count; index++) {
        FaultResult *result = &results[index];
        ArmCommand fault_command = command;
        ArmCommand fault_decoded;
        uint8_t fault_frame[COURSE_FRAME_V1_LEN];
        size_t fault_written = 0;

        memset(&fault_decoded, 0, sizeof(fault_decoded));
        memset(fault_frame, 0, sizeof(fault_frame));
        result->fixture = faults[index];
        result->expected = verdict_from_name(faults[index].expected_name);

        if (strcmp(faults[index].name, "reorder") == 0) {
            ArmCommand newer = command;
            ArmCommand older = command;
            uint8_t newer_frame[COURSE_FRAME_V1_LEN];
            uint8_t older_frame[COURSE_FRAME_V1_LEN];
            size_t newer_written = 0;
            size_t older_written = 0;

            memset(newer_frame, 0, sizeof(newer_frame));
            memset(older_frame, 0, sizeof(older_frame));
            newer.seq = 42;
            newer.trace_id = context->seed + 42;
            older.seq = 41;
            older.trace_id = context->seed + 41;
            result->frame_ready =
                frame_encode(&newer, newer_frame, sizeof(newer_frame),
                             &newer_written) &&
                frame_encode(&older, older_frame, sizeof(older_frame),
                             &older_written);
            (void)frame_decode(newer_frame, newer_written, 40,
                               &fault_decoded);
            result->actual = frame_decode(older_frame, older_written, 42,
                                          &fault_decoded);
            result->actuator_writes =
                result->actual == COURSE_RX_ACCEPT ? 1U : 0U;
            continue;
        }

        if (strcmp(faults[index].name, "unit-plus-bitflip") == 0) {
            fault_command.seq = 45;
            fault_command.q_target_rad[1] = 90.0;
        }
        result->frame_ready =
            frame_encode(&fault_command, fault_frame, sizeof(fault_frame),
                         &fault_written);
        if (strcmp(faults[index].name, "bitflip") == 0 ||
            strcmp(faults[index].name, "unit-plus-bitflip") == 0) {
            if (faults[index].byte_offset < fault_written) {
                fault_frame[faults[index].byte_offset] ^=
                    (uint8_t)faults[index].mask;
            }
            result->actual = frame_decode(fault_frame, fault_written, 0,
                                          &fault_decoded);
        } else if (strcmp(faults[index].name, "truncate") == 0) {
            result->actual = frame_decode(fault_frame,
                                          faults[index].truncate_to, 0,
                                          &fault_decoded);
        } else {
            result->actual = COURSE_RX_REJECT;
        }
        result->actuator_writes =
            result->actual == COURSE_RX_ACCEPT ? 1U : 0U;
    }

    if (test_group_enabled(context, "G1")) {
        test_check(context, context->seed == COURSE_FIXED_SEED,
                   "G1 fixed seed equals 20260719");
        test_check(context, pose_count == 3,
                   "G1 fixture contains exactly three known poses");
        check_or_todo(context, !state_stub, schema_hash != 0,
                      "G1 owned state API gives the frozen schema a nonzero hash");
        for (size_t index = 0; index < pose_count; index++) {
            char label[128];
            (void)snprintf(label, sizeof(label),
                           "G1 known pose %.39s satisfies the state contract",
                           poses[index].name);
            check_or_todo(context, !state_stub, pose_validity[index], label);
        }
        check_or_todo(context, !state_stub,
                      state_fields_equal(&twin_first, &twin_second),
                      "G1 repeated fixed-step runs are byte-stable by field");
        check_or_todo(context, !state_stub, twin_changed,
                      "G1 fixed-step twin advances a valid command");
        check_or_todo(context, !state_stub,
                      twin_first.t_mono_ns ==
                          twin_initial.t_mono_ns + COURSE_FIXED_STEP_NS,
                      "G1 twin advances the frozen monotonic step");
        check_or_todo(context, !state_stub,
                      target_distance_squared(&twin_first, &command) <=
                          target_distance_squared(&twin_initial, &command),
                      "G1 transform step does not move away from its target");
        if (pose_count > 0) {
            ArmState previous = poses[0].state;
            ArmState invalid = poses[0].state;
            previous.seq = 0;
            previous.t_mono_ns =
                poses[0].state.t_mono_ns - COURSE_FIXED_STEP_NS;
            invalid.q_rad[0] = NAN;
            check_or_todo(context, !state_stub,
                          !state_valid(&invalid, &previous),
                          "G1 non-finite state is rejected");
            invalid = poses[0].state;
            invalid.frame_id = (uint8_t)(COURSE_FRAME_ID_BASE + 1);
            check_or_todo(context, !state_stub,
                          !state_valid(&invalid, &previous),
                          "G1 wrong coordinate frame is rejected");
        }
    }

    if (test_group_enabled(context, "G2")) {
        check_or_todo(context, crc_known != 0, crc_known == UINT16_C(0x29B1),
                      "G2 owned transport API maps the CRC known vector to 0x29B1");
        check_or_todo(context, !frame_stub, encoded,
                      "G2 FrameV1 encoder accepts a valid command");
        check_or_todo(context, !frame_stub,
                      written == COURSE_FRAME_V1_LEN,
                      "G2 FrameV1 has the frozen wire length");
        check_or_todo(context, !frame_stub,
                      frame_layout_matches(frame, written, &command),
                      "G2 FrameV1 layout is canonical little-endian");
        check_or_todo(context, !frame_stub,
                      frame_crc_matches(frame, written),
                      "G2 frame CRC covers version through payload");
        check_or_todo(context, !frame_stub,
                      roundtrip == COURSE_RX_ACCEPT &&
                          command_wire_equal(&command, &decoded),
                      "G2 frame encode/decode round trip preserves payload");
        check_or_todo(context, !frame_stub,
                      parser_accept && parser_terminal == COURSE_RX_ACCEPT,
                      "G2 streaming parser accepts one complete frame");
        check_or_todo(context, !frame_stub, parser_resync,
                      "G2 parser resynchronizes after a truncated frame");
    }

    if (test_group_enabled(context, "G3")) {
        TraceRow baseline_trace = {
            .trace_id = context->seed,
            .seq = 1,
            .t_pub_ns = UINT64_C(10000000),
            .t_rx_ns = UINT64_C(11000000),
            .t_gate_ns = UINT64_C(12000000),
            .t_ack_ns = UINT64_C(13000000),
            .verdict = COURSE_VERDICT_APPROVE,
            .reason = COURSE_REASON_NONE,
        };
        const bool timing_contract_owned = trace_complete(&baseline_trace);
        bool monotonic = pose_count == 3;
        bool one_clock = pose_count == 3;

        for (size_t index = 1; index < pose_count; index++) {
            monotonic = monotonic &&
                        poses[index].state.t_mono_ns >
                            poses[index - 1].state.t_mono_ns;
            one_clock = one_clock &&
                        poses[index].state.t_mono_ns -
                                poses[index - 1].state.t_mono_ns ==
                            COURSE_FIXED_STEP_NS;
        }
        test_check(context, monotonic,
                   "G3 baseline pose events use monotonic time");
        test_check(context, one_clock,
                   "G3 baseline events share the frozen fixed-step clock");
        check_or_todo(context, timing_contract_owned,
                      timing_contract_owned,
                      "G3 owned timing API accepts one complete monotonic trace");
        check_or_todo(context, !frame_stub,
                      roundtrip == COURSE_RX_ACCEPT &&
                          decoded.t_source_ns == command.t_source_ns,
                      "G3 source timestamp survives the frame round trip");
    }

    if (test_group_enabled(context, "G4")) {
        const SafetyDecision missing_input = safety_gate(NULL, NULL, 0);

        check_or_todo(context,
                      missing_input.reason != COURSE_REASON_STUDENT_TODO,
                      missing_input.verdict == COURSE_VERDICT_REJECT &&
                          missing_input.reason == COURSE_REASON_INTERNAL,
                      "G4 owned safety API freezes missing input as REJECT INTERNAL");
        check_or_todo(context, !frame_stub, fault_count == 4,
                      "G4 four deterministic fault fixtures are loaded");
        for (size_t index = 0; index < fault_count; index++) {
            char verdict_label[128];
            char write_label[128];
            (void)snprintf(verdict_label, sizeof(verdict_label),
                           "G4 %.39s has the explicit frozen verdict",
                           results[index].fixture.name);
            (void)snprintf(write_label, sizeof(write_label),
                           "G4 %.39s produces zero corrupt actuator writes",
                           results[index].fixture.name);
            check_or_todo(context, !frame_stub && results[index].frame_ready,
                          results[index].actual == results[index].expected,
                          verdict_label);
            check_or_todo(context, !frame_stub && results[index].frame_ready,
                          results[index].actuator_writes == 0, write_label);
        }
    }

    if (test_group_enabled(context, "G5")) {
        FreshnessGate missing_freshness;
        ActuatorWriter missing_writer;
        TraceRow missing_trace;
        const ArmState missing_state = twin_initial;
        Verdict missing_verdict;
        ArmCommand clean_decoded;
        ArmCommand corrupt_decoded;
        uint8_t corrupt_frame[COURSE_FRAME_V1_LEN];
        RxVerdict clean_verdict;
        RxVerdict corrupt_verdict;

        memset(&clean_decoded, 0, sizeof(clean_decoded));
        memset(&corrupt_decoded, 0, sizeof(corrupt_decoded));
        memset(&missing_freshness, 0, sizeof(missing_freshness));
        memset(&missing_trace, 0, sizeof(missing_trace));
        writer_init(&missing_writer, 15);
        missing_verdict = gatekeeper_process(
            &missing_freshness, &missing_writer, &missing_state, NULL, 0,
            UINT64_C(15000000), &missing_trace);
        check_or_todo(context,
                      missing_trace.reason != COURSE_REASON_STUDENT_TODO,
                      missing_verdict == COURSE_VERDICT_REJECT &&
                          missing_trace.verdict == COURSE_VERDICT_REJECT &&
                          missing_trace.reason == COURSE_REASON_BAD_FRAME &&
                          missing_writer.count == 0,
                      "G5 owned gatekeeper rejects a missing frame before queue admission");
        memcpy(corrupt_frame, frame, sizeof(corrupt_frame));
        if (written == COURSE_FRAME_V1_LEN) {
            corrupt_frame[31] ^= UINT8_C(0x04);
        }
        clean_verdict = frame_decode(frame, written, command.seq - 1,
                                     &clean_decoded);
        corrupt_verdict = frame_decode(corrupt_frame, written,
                                       command.seq - 1, &corrupt_decoded);
        check_or_todo(context, !frame_stub,
                      clean_verdict == COURSE_RX_ACCEPT &&
                          clean_decoded.trace_id == command.trace_id,
                      "G5 clean frame reaches the actuation boundary with its trace ID");
        check_or_todo(context, !frame_stub,
                      corrupt_verdict == COURSE_RX_NACK_CRC &&
                          corrupt_verdict != COURSE_RX_ACCEPT,
                      "G5 corrupt frame cannot reach the actuation boundary");
    }

    test_check(context,
               write_baseline_trace(context, poses, pose_validity, pose_count,
                                    schema_hash, &twin_first, &twin_second),
               "Day1 baseline-trace.csv written");
    test_check(context,
               write_corruption_results(context, results, fault_count),
               "Day1 corruption-results.csv written");
    test_check(context, write_wire_contract(context),
               "Day1 wire-contract.txt written");
    test_check(context, write_manifest(context),
               "Day1 manifest.json written");
    return test_finish(context);
}
