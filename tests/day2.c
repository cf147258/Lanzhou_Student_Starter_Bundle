#include "course.h"
#include "test_harness.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
    MAX_TOPICS = 8,
    MAX_MESSAGES = 12,
    TEXT_CAPACITY = 64
};

typedef struct {
    char topic[TEXT_CAPACITY];
    int qos;
    char retention[TEXT_CAPACITY];
    char policy[TEXT_CAPACITY];
} TopicFixture;

typedef struct {
    char name[TEXT_CAPACITY];
    uint64_t seq;
    uint64_t t_source_ns;
    uint64_t now_ns;
    int qos;
    unsigned delivery_order;
    char expected_name[TEXT_CAPACITY];
    size_t expected_writes;
} MessageFixture;

typedef struct {
    MessageFixture fixture;
    ArmCommand command;
    Reason expected;
    Reason direct_reason;
    bool direct_latest_rule_ok;
    Reason transport_reason;
    bool encoded;
    bool transport_latest_rule_ok;
    TraceRow trace;
    size_t actuator_writes;
} MessageResult;

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

static size_t read_topic_fixtures(TopicFixture *topics, size_t capacity) {
    char line[512];
    FILE *file = open_fixture("fixtures/day2/topics.csv");
    size_t count = 0;

    if (file == NULL || fgets(line, sizeof(line), file) == NULL) {
        if (file != NULL) {
            fclose(file);
        }
        return 0;
    }
    while (count < capacity && fgets(line, sizeof(line), file) != NULL) {
        TopicFixture topic;
        int fields;

        memset(&topic, 0, sizeof(topic));
        fields = sscanf(line, "%63[^,],%d,%63[^,],%63[^\n]", topic.topic,
                        &topic.qos, topic.retention, topic.policy);
        if (fields != 4) {
            fclose(file);
            return 0;
        }
        topics[count++] = topic;
    }
    fclose(file);
    return count;
}

static size_t read_message_fixtures(MessageFixture *messages,
                                    size_t capacity) {
    char line[512];
    FILE *file = open_fixture("fixtures/day2/messages.csv");
    size_t count = 0;

    if (file == NULL || fgets(line, sizeof(line), file) == NULL) {
        if (file != NULL) {
            fclose(file);
        }
        return 0;
    }
    while (count < capacity && fgets(line, sizeof(line), file) != NULL) {
        MessageFixture message;
        int fields;

        memset(&message, 0, sizeof(message));
        fields = sscanf(line,
                        "%63[^,],%" SCNu64 ",%" SCNu64 ",%" SCNu64
                        ",%d,%u,%63[^,],%zu",
                        message.name, &message.seq, &message.t_source_ns,
                        &message.now_ns, &message.qos,
                        &message.delivery_order, message.expected_name,
                        &message.expected_writes);
        if (fields != 8) {
            fclose(file);
            return 0;
        }
        messages[count++] = message;
    }
    fclose(file);
    return count;
}

static Reason reason_from_name(const char *name) {
    if (strcmp(name, "NONE") == 0) {
        return COURSE_REASON_NONE;
    }
    if (strcmp(name, "BAD_FRAME") == 0) {
        return COURSE_REASON_BAD_FRAME;
    }
    if (strcmp(name, "NOT_NEW") == 0) {
        return COURSE_REASON_NOT_NEW;
    }
    if (strcmp(name, "CLOCK_ERROR") == 0) {
        return COURSE_REASON_CLOCK_ERROR;
    }
    if (strcmp(name, "STALE_COMMAND") == 0) {
        return COURSE_REASON_STALE_COMMAND;
    }
    return COURSE_REASON_INTERNAL;
}

static ArmCommand command_for(const MessageFixture *fixture, uint64_t seed) {
    ArmCommand command;

    memset(&command, 0, sizeof(command));
    command.seq = fixture->seq;
    command.t_source_ns = fixture->t_source_ns;
    command.trace_id = seed + (uint64_t)fixture->delivery_order;
    command.q_target_rad[0] = 0.10 + 0.01 * (double)fixture->delivery_order;
    command.q_target_rad[1] = -0.20;
    command.q_target_rad[2] = 0.30;
    return command;
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

static bool gate_equal(const FreshnessGate *left,
                       const FreshnessGate *right) {
    if (left->last_seq != right->last_seq ||
        left->has_last != right->has_last ||
        left->has_latest != right->has_latest) {
        return false;
    }
    if (!left->has_latest) {
        return true;
    }
    return command_wire_equal(&left->latest, &right->latest);
}

static bool latest_rule_holds(const FreshnessGate *before,
                              const FreshnessGate *after,
                              const ArmCommand *command, Reason reason) {
    if (reason == COURSE_REASON_NONE) {
        return after->has_last && after->last_seq == command->seq &&
               after->has_latest &&
               command_wire_equal(&after->latest, command);
    }
    return gate_equal(before, after);
}

static bool trace_has_four_stamps(const TraceRow *trace) {
    if (trace == NULL || trace->t_pub_ns == 0 || trace->t_rx_ns == 0 ||
        trace->t_gate_ns == 0 || trace->t_ack_ns == 0) {
        return false;
    }
    return trace->t_rx_ns <= trace->t_gate_ns &&
           trace->t_gate_ns <= trace->t_ack_ns;
}

static bool trace_matches_result(const TraceRow *trace,
                                 const ArmCommand *command, Reason reason) {
    const Verdict expected_verdict =
        reason == COURSE_REASON_NONE ? COURSE_VERDICT_APPROVE
                                     : COURSE_VERDICT_REJECT;

    return trace != NULL && trace->trace_id == command->trace_id &&
           trace->seq == command->seq &&
           trace->t_pub_ns == command->t_source_ns &&
           trace->verdict == expected_verdict && trace->reason == reason &&
           trace_has_four_stamps(trace);
}

static const TopicFixture *find_topic(const TopicFixture *topics,
                                      size_t topic_count,
                                      const char *name) {
    for (size_t index = 0; index < topic_count; index++) {
        if (strcmp(topics[index].topic, name) == 0) {
            return &topics[index];
        }
    }
    return NULL;
}

static const MessageResult *find_result(const MessageResult *results,
                                        size_t result_count,
                                        const char *name) {
    for (size_t index = 0; index < result_count; index++) {
        if (strcmp(results[index].fixture.name, name) == 0) {
            return &results[index];
        }
    }
    return NULL;
}

static void check_reason_or_todo(TestContext *context, Reason actual,
                                 Reason expected, const char *label) {
    if (actual == COURSE_REASON_STUDENT_TODO) {
        test_todo(context, label);
        return;
    }
    test_check(context, actual == expected, label);
}

static void check_transport_or_todo(TestContext *context,
                                    const MessageResult *result,
                                    bool condition, const char *label) {
    if (result == NULL || !result->encoded ||
        result->transport_reason == COURSE_REASON_STUDENT_TODO) {
        test_todo(context, label);
        return;
    }
    test_check(context, condition, label);
}

static bool write_freshness_csv(TestContext *context,
                                const MessageResult *results,
                                size_t result_count,
                                Reason invalid_reason,
                                size_t invalid_writes) {
    FILE *file = test_open_evidence(context, "qos-freshness.csv");

    if (file == NULL) {
        return false;
    }
    fprintf(file,
            "case,seq,t_source_ns,now_ns,qos,delivery_order,expected_reason,"
            "direct_reason,transport_reason,latest_rule_ok,actuator_writes,"
            "expected_actuator_writes,pass\n");
    for (size_t index = 0; index < result_count; index++) {
        const MessageResult *result = &results[index];
        const bool pass = result->direct_reason == result->expected &&
                          result->transport_reason == result->expected &&
                          result->direct_latest_rule_ok &&
                          result->transport_latest_rule_ok &&
                          result->actuator_writes ==
                              result->fixture.expected_writes;
        fprintf(file,
                "%s,%" PRIu64 ",%" PRIu64 ",%" PRIu64
                ",%d,%u,%s,%s,%s,%s,%zu,%zu,%s\n",
                result->fixture.name, result->fixture.seq,
                result->fixture.t_source_ns, result->fixture.now_ns,
                result->fixture.qos, result->fixture.delivery_order,
                reason_name(result->expected),
                reason_name(result->direct_reason),
                reason_name(result->transport_reason),
                result->direct_latest_rule_ok &&
                        result->transport_latest_rule_ok
                    ? "true"
                    : "false",
                result->actuator_writes,
                result->fixture.expected_writes, pass ? "true" : "false");
    }
    fprintf(file,
            "invalid_bitflip,0,0,0,1,0,BAD_FRAME,not-applicable,%s,true,"
            "%zu,0,%s\n",
            reason_name(invalid_reason), invalid_writes,
            invalid_reason == COURSE_REASON_BAD_FRAME && invalid_writes == 0
                ? "true"
                : "false");
    return fclose(file) == 0;
}

static bool write_topic_table(TestContext *context,
                              const TopicFixture *topics,
                              size_t topic_count) {
    FILE *file = test_open_evidence(context, "topic-table.txt");

    if (file == NULL) {
        return false;
    }
    fprintf(file, "topic | qos | retention | acceptance_policy\n");
    for (size_t index = 0; index < topic_count; index++) {
        fprintf(file, "%s | %d | %s | %s\n", topics[index].topic,
                topics[index].qos, topics[index].retention,
                topics[index].policy);
    }
    fprintf(file,
            "\nFrozen constants:\nstate=%s\ncommand=%s\nestop=%s\n",
            COURSE_TOPIC_STATE, COURSE_TOPIC_COMMAND, COURSE_TOPIC_ESTOP);
    return fclose(file) == 0;
}

static void write_trace_row(FILE *file, const char *case_name,
                            const TraceRow *trace) {
    fprintf(file,
            "{\"case\":\"%s\",\"trace_id\":%" PRIu64
            ",\"seq\":%" PRIu64 ",\"t_pub\":%" PRIu64
            ",\"t_rx\":%" PRIu64 ",\"t_gate\":%" PRIu64
            ",\"t_ack\":%" PRIu64 ",\"verdict\":\"%s\","
            "\"reason\":\"%s\"}\n",
            case_name, trace->trace_id, trace->seq, trace->t_pub_ns,
            trace->t_rx_ns, trace->t_gate_ns, trace->t_ack_ns,
            verdict_name(trace->verdict), reason_name(trace->reason));
}

static bool write_trace_log(TestContext *context,
                            const MessageResult *results,
                            size_t result_count,
                            const TraceRow *invalid_trace) {
    FILE *file = test_open_evidence(context, "trace.jsonl");

    if (file == NULL) {
        return false;
    }
    for (size_t index = 0; index < result_count; index++) {
        write_trace_row(file, results[index].fixture.name,
                        &results[index].trace);
    }
    write_trace_row(file, "invalid_bitflip", invalid_trace);
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
            "  \"gate\": \"verify-day2\",\n"
            "  \"group\": \"%s\",\n"
            "  \"day\": 2,\n"
            "  \"seed\": %" PRIu64 ",\n"
            "  \"input_tag\": \"frame_v1\",\n"
            "  \"output_tag\": \"transport_v1\",\n"
            "  \"fixture_version\": \"1.0.0\",\n"
            "  \"language\": \"C\",\n"
            "  \"platform\": \"Linux container\",\n"
            "  \"broker\": \"loopback-only Mosquitto\",\n"
            "  \"timebase\": \"CLOCK_MONOTONIC\",\n"
            "  \"max_command_age_ns\": %" PRIu64 ",\n"
            "  \"container_image\": \"%s\",\n"
            "  \"commit\": \"%s\",\n"
            "  \"frame_version\": %u,\n"
            "  \"simulator_only\": true,\n"
            "  \"checks_before_manifest\": %zu,\n"
            "  \"failures_before_manifest\": %zu,\n"
            "  \"todo_before_manifest\": %zu\n"
            "}\n",
            context->group, context->seed, COURSE_MAX_CMD_AGE_NS,
            image == NULL ? "NOT_RECORDED" : image,
            commit == NULL ? "NOT_A_GIT_CHECKOUT" : commit,
            COURSE_FRAME_VERSION, context->checks, context->failures,
            context->todo_failures);
    return fclose(file) == 0;
}

int verify_day2(TestContext *context) {
    TopicFixture topics[MAX_TOPICS];
    MessageFixture messages[MAX_MESSAGES];
    MessageResult results[MAX_MESSAGES];
    size_t topic_count;
    size_t message_count;
    FreshnessGate direct_gate;
    FreshnessGate transport_gate;
    TraceRow invalid_trace;
    ArmCommand invalid_accepted;
    uint8_t invalid_frame[COURSE_FRAME_V1_LEN];
    size_t invalid_length = 0;
    Reason invalid_reason = COURSE_REASON_STUDENT_TODO;
    size_t invalid_writes = 0;
    bool invalid_ready = false;
    ArmCommand estop_command;
    ArmCommand estop_decoded;
    uint8_t estop_frame[COURSE_FRAME_V1_LEN];
    size_t estop_length = 0;
    RxVerdict estop_verdict = COURSE_RX_REJECT;
    bool estop_ready = false;

    memset(topics, 0, sizeof(topics));
    memset(messages, 0, sizeof(messages));
    memset(results, 0, sizeof(results));
    memset(&direct_gate, 0, sizeof(direct_gate));
    memset(&transport_gate, 0, sizeof(transport_gate));
    memset(&invalid_trace, 0, sizeof(invalid_trace));
    memset(&invalid_accepted, 0, sizeof(invalid_accepted));
    memset(invalid_frame, 0, sizeof(invalid_frame));
    memset(&estop_command, 0, sizeof(estop_command));
    memset(&estop_decoded, 0, sizeof(estop_decoded));
    memset(estop_frame, 0, sizeof(estop_frame));

    topic_count = read_topic_fixtures(topics, MAX_TOPICS);
    message_count = read_message_fixtures(messages, MAX_MESSAGES);

    if (message_count > 0) {
        estop_command = command_for(&messages[0], context->seed);
        estop_command.seq = 100;
        estop_command.trace_id = context->seed + UINT64_C(100);
        estop_command.emergency = true;
        estop_ready = frame_encode(&estop_command, estop_frame,
                                   sizeof(estop_frame), &estop_length);
        if (estop_ready && estop_length == COURSE_FRAME_V1_LEN) {
            estop_verdict = frame_decode(estop_frame, estop_length, 99,
                                         &estop_decoded);
        }
    }

    for (size_t index = 0; index < message_count; index++) {
        MessageResult *result = &results[index];
        FreshnessGate before;

        memset(result, 0, sizeof(*result));
        result->fixture = messages[index];
        result->command = command_for(&messages[index], context->seed);
        result->expected = reason_from_name(messages[index].expected_name);
        before = direct_gate;
        result->direct_reason = freshness_accept(
            &direct_gate, &result->command, messages[index].now_ns);
        result->direct_latest_rule_ok = latest_rule_holds(
            &before, &direct_gate, &result->command, result->direct_reason);
    }

    for (size_t index = 0; index < message_count; index++) {
        MessageResult *result = &results[index];
        FreshnessGate before = transport_gate;
        ArmCommand accepted;
        uint8_t frame[COURSE_FRAME_V1_LEN];
        size_t written = 0;

        memset(&accepted, 0, sizeof(accepted));
        memset(frame, 0, sizeof(frame));
        memset(&result->trace, 0, sizeof(result->trace));
        result->encoded = frame_encode(&result->command, frame, sizeof(frame),
                                       &written);
        result->transport_reason = transport_on_message(
            &transport_gate, frame, written, messages[index].now_ns,
            &result->trace, &accepted);
        result->transport_latest_rule_ok = latest_rule_holds(
            &before, &transport_gate, &result->command,
            result->transport_reason);
        if (result->transport_reason == COURSE_REASON_NONE &&
            !command_wire_equal(&accepted, &result->command)) {
            result->transport_latest_rule_ok = false;
        }
        result->actuator_writes =
            result->transport_reason == COURSE_REASON_NONE ? 1U : 0U;
    }

    if (message_count > 0) {
        const ArmCommand command = command_for(&messages[0], context->seed);
        FreshnessGate invalid_gate;

        memset(&invalid_gate, 0, sizeof(invalid_gate));
        invalid_ready = frame_encode(&command, invalid_frame,
                                     sizeof(invalid_frame), &invalid_length);
        if (invalid_ready && invalid_length == COURSE_FRAME_V1_LEN) {
            invalid_frame[31] ^= UINT8_C(0x04);
        }
        invalid_reason = transport_on_message(
            &invalid_gate, invalid_frame, invalid_length,
            messages[0].now_ns, &invalid_trace, &invalid_accepted);
        invalid_writes = invalid_reason == COURSE_REASON_NONE ? 1U : 0U;
        invalid_ready = invalid_ready &&
                        invalid_length == COURSE_FRAME_V1_LEN;
    }

    if (test_group_enabled(context, "G1")) {
        ArmState previous_state;
        ArmState next_state;
        ArmState repeated_state;
        const TopicFixture *state_topic =
            find_topic(topics, topic_count, COURSE_TOPIC_STATE);
        const MessageResult *normal =
            find_result(results, message_count, "normal");
        const MessageResult *duplicate =
            find_result(results, message_count, "duplicate");
        const MessageResult *stale =
            find_result(results, message_count, "stale_new");

        memset(&previous_state, 0, sizeof(previous_state));
        previous_state.seq = 20;
        previous_state.t_mono_ns = INT64_C(1000000000);
        previous_state.frame_id = COURSE_FRAME_ID_BASE;
        next_state = previous_state;
        next_state.seq++;
        next_state.t_mono_ns += COURSE_FIXED_STEP_NS;
        repeated_state = next_state;
        repeated_state.seq = previous_state.seq;

        if (!state_valid(&next_state, &previous_state)) {
            test_todo(context,
                      "G1 owned state API accepts a new monotonic sample");
        } else {
            test_check(context,
                       !state_valid(&repeated_state, &previous_state),
                       "G1 owned state API rejects a repeated sequence");
        }

        test_check(context, context->seed == COURSE_FIXED_SEED,
                   "G1 fixed freshness seed equals 20260719");
        test_check(context,
                   state_topic != NULL && state_topic->qos == 0 &&
                       strcmp(state_topic->retention, "latest") == 0 &&
                       strcmp(state_topic->policy,
                              "valid-and-fresh") == 0,
                   "G1 state topic is QoS 0 with latest valid fresh state");
        if (normal == NULL || duplicate == NULL || stale == NULL) {
            test_check(context, false,
                       "G1 normal duplicate and stale fixtures are present");
        } else {
            check_reason_or_todo(context, normal->direct_reason,
                                 COURSE_REASON_NONE,
                                 "G1 fresh state-age sample is accepted");
            check_reason_or_todo(context, duplicate->direct_reason,
                                 COURSE_REASON_NOT_NEW,
                                 "G1 duplicate sample is flagged");
            check_reason_or_todo(context, stale->direct_reason,
                                 COURSE_REASON_STALE_COMMAND,
                                 "G1 stale sample is flagged");
            if (normal->direct_reason == COURSE_REASON_STUDENT_TODO ||
                duplicate->direct_reason == COURSE_REASON_STUDENT_TODO ||
                stale->direct_reason == COURSE_REASON_STUDENT_TODO) {
                test_todo(context,
                          "G1 latest value changes only after acceptance");
            } else {
                test_check(context,
                           normal->direct_latest_rule_ok &&
                               duplicate->direct_latest_rule_ok &&
                               stale->direct_latest_rule_ok,
                           "G1 latest value changes only after acceptance");
            }
        }
    }

    if (test_group_enabled(context, "G2")) {
        const TopicFixture *state_topic =
            find_topic(topics, topic_count, COURSE_TOPIC_STATE);
        const TopicFixture *command_topic =
            find_topic(topics, topic_count, COURSE_TOPIC_COMMAND);
        const TopicFixture *estop_topic =
            find_topic(topics, topic_count, COURSE_TOPIC_ESTOP);

        test_check(context, topic_count == 3,
                   "G2 topic fixture contains exactly three frozen topics");
        test_check(context,
                   state_topic != NULL && state_topic->qos == 0 &&
                       command_topic != NULL && command_topic->qos == 1 &&
                       estop_topic != NULL && estop_topic->qos == 1,
                   "G2 state command and estop QoS choices are frozen");
        test_check(context,
                   command_topic != NULL &&
                       strcmp(command_topic->policy,
                              "valid-new-and-fresh") == 0 &&
                       estop_topic != NULL &&
                       strcmp(estop_topic->policy,
                              "preserve-source-age") == 0,
                   "G2 command freshness and estop source-age policies match");
        if (!estop_ready) {
            test_todo(context,
                      "G2 owned transport API preserves emergency type and source age");
        } else {
            test_check(context,
                       estop_length == COURSE_FRAME_V1_LEN &&
                           estop_frame[3] == COURSE_FRAME_TYPE_ESTOP &&
                           estop_verdict == COURSE_RX_ACCEPT &&
                           command_wire_equal(&estop_command,
                                              &estop_decoded) &&
                           estop_decoded.t_source_ns ==
                               estop_command.t_source_ns,
                       "G2 owned transport API preserves emergency type and source age");
        }
        test_check(context, message_count == 6,
                   "G2 six ordered freshness fixtures are loaded");
        for (size_t index = 0; index < message_count; index++) {
            char direct_label[144];
            char transport_label[144];
            char latest_label[144];

            (void)snprintf(direct_label, sizeof(direct_label),
                           "G2 %.48s has the expected freshness reason",
                           results[index].fixture.name);
            (void)snprintf(transport_label, sizeof(transport_label),
                           "G2 %.48s has the expected transport reason",
                           results[index].fixture.name);
            (void)snprintf(latest_label, sizeof(latest_label),
                           "G2 %.48s obeys accepted-only latest update",
                           results[index].fixture.name);
            check_reason_or_todo(context, results[index].direct_reason,
                                 results[index].expected, direct_label);
            check_transport_or_todo(
                context, &results[index],
                results[index].transport_reason == results[index].expected,
                transport_label);
            if (results[index].direct_reason ==
                    COURSE_REASON_STUDENT_TODO ||
                results[index].transport_reason ==
                    COURSE_REASON_STUDENT_TODO ||
                !results[index].encoded) {
                test_todo(context, latest_label);
            } else {
                test_check(context,
                           results[index].direct_latest_rule_ok &&
                               results[index].transport_latest_rule_ok,
                           latest_label);
            }
        }
    }

    if (test_group_enabled(context, "G3")) {
        TraceRow ordered_trace = {
            .trace_id = context->seed,
            .seq = 2,
            .t_pub_ns = UINT64_C(100),
            .t_rx_ns = UINT64_C(110),
            .t_gate_ns = UINT64_C(120),
            .t_ack_ns = UINT64_C(130),
            .verdict = COURSE_VERDICT_APPROVE,
            .reason = COURSE_REASON_NONE,
        };
        TraceRow reversed_trace = ordered_trace;
        const bool ordered_complete = trace_complete(&ordered_trace);

        reversed_trace.t_gate_ns = UINT64_C(90);
        if (!ordered_complete) {
            test_todo(context,
                      "G3 owned timing API accepts four ordered timestamps");
        } else {
            test_check(context, !trace_complete(&reversed_trace),
                       "G3 owned timing API rejects reversed timestamps");
        }
        for (size_t index = 0; index < message_count; index++) {
            char label[144];
            (void)snprintf(label, sizeof(label),
                           "G3 %.48s records four ordered transport timestamps",
                           results[index].fixture.name);
            check_transport_or_todo(
                context, &results[index],
                trace_has_four_stamps(&results[index].trace) &&
                    results[index].trace.t_pub_ns ==
                        results[index].command.t_source_ns,
                label);
        }
    }

    if (test_group_enabled(context, "G4")) {
        static const char *const rejected_cases[] = {
            "duplicate", "reordered_old", "stale_new", "clock_error"};
        ArmState boundary_state;
        SafetyDecision missing_command;

        memset(&boundary_state, 0, sizeof(boundary_state));
        boundary_state.seq = 22;
        boundary_state.t_mono_ns = INT64_C(1010000000);
        boundary_state.frame_id = COURSE_FRAME_ID_BASE;
        missing_command = safety_gate(&boundary_state, NULL,
                                      UINT64_C(1020000000));
        if (missing_command.reason == COURSE_REASON_STUDENT_TODO) {
            test_todo(context,
                      "G4 owned safety API rejects an absent command explicitly");
        } else {
            test_check(context,
                       missing_command.verdict == COURSE_VERDICT_REJECT &&
                           missing_command.reason == COURSE_REASON_INTERNAL,
                       "G4 owned safety API rejects an absent command explicitly");
        }

        for (size_t index = 0;
             index < sizeof(rejected_cases) / sizeof(rejected_cases[0]);
             index++) {
            const MessageResult *result =
                find_result(results, message_count, rejected_cases[index]);
            char label[144];
            (void)snprintf(label, sizeof(label),
                           "G4 %.48s rejection keeps its stable reason",
                           rejected_cases[index]);
            if (result == NULL) {
                test_check(context, false, label);
            } else {
                check_transport_or_todo(
                    context, result,
                    result->transport_reason == result->expected &&
                        result->trace.reason == result->expected &&
                        result->trace.verdict == COURSE_VERDICT_REJECT,
                    label);
            }
        }
        if (!invalid_ready ||
            invalid_reason == COURSE_REASON_STUDENT_TODO) {
            test_todo(context,
                      "G4 invalid frame is fail-closed with BAD_FRAME");
        } else {
            test_check(context,
                       invalid_reason == COURSE_REASON_BAD_FRAME &&
                           invalid_trace.reason == COURSE_REASON_BAD_FRAME &&
                           invalid_trace.verdict == COURSE_VERDICT_REJECT &&
                           invalid_writes == 0,
                       "G4 invalid frame is fail-closed with BAD_FRAME");
        }
    }

    if (test_group_enabled(context, "G5")) {
        FreshnessGate owned_freshness;
        ActuatorWriter owned_writer;
        ArmState owned_state;
        ArmCommand owned_command;
        TraceRow owned_trace;
        uint8_t owned_frame[COURSE_FRAME_V1_LEN];
        size_t owned_length = 0;
        bool owned_encoded = false;
        Verdict owned_verdict;

        memset(&owned_freshness, 0, sizeof(owned_freshness));
        memset(&owned_state, 0, sizeof(owned_state));
        memset(&owned_command, 0, sizeof(owned_command));
        memset(&owned_trace, 0, sizeof(owned_trace));
        memset(owned_frame, 0, sizeof(owned_frame));
        writer_init(&owned_writer, 25);
        owned_state.seq = 25;
        owned_state.t_mono_ns = INT64_C(1010000000);
        owned_state.frame_id = COURSE_FRAME_ID_BASE;
        if (message_count > 0) {
            owned_command = command_for(&messages[0], context->seed);
            owned_encoded = frame_encode(&owned_command, owned_frame,
                                         sizeof(owned_frame),
                                         &owned_length);
        }
        if (owned_encoded && owned_length == COURSE_FRAME_V1_LEN) {
            owned_frame[31] ^= UINT8_C(0x04);
        }
        owned_verdict = gatekeeper_process(
            &owned_freshness, &owned_writer, &owned_state, owned_frame,
            owned_length, UINT64_C(1020000000), &owned_trace);
        if (owned_trace.reason == COURSE_REASON_STUDENT_TODO) {
            test_todo(context,
                      "G5 owned gatekeeper stamps and rejects a corrupt frame");
        } else {
            test_check(context,
                       owned_encoded &&
                           owned_verdict == COURSE_VERDICT_REJECT &&
                           owned_trace.verdict == COURSE_VERDICT_REJECT &&
                           owned_trace.reason == COURSE_REASON_BAD_FRAME &&
                           owned_trace.t_gate_ns == UINT64_C(1020000000) &&
                           owned_trace.t_ack_ns >= owned_trace.t_gate_ns &&
                           owned_writer.count == 0,
                       "G5 owned gatekeeper stamps and rejects a corrupt frame");
        }
        for (size_t index = 0; index < message_count; index++) {
            char trace_label[144];
            char write_label[144];

            (void)snprintf(trace_label, sizeof(trace_label),
                           "G5 %.48s preserves input verdict output trace ID",
                           results[index].fixture.name);
            (void)snprintf(write_label, sizeof(write_label),
                           "G5 %.48s matches the expected actuator count",
                           results[index].fixture.name);
            check_transport_or_todo(
                context, &results[index],
                trace_matches_result(&results[index].trace,
                                     &results[index].command,
                                     results[index].expected),
                trace_label);
            check_transport_or_todo(
                context, &results[index],
                results[index].actuator_writes ==
                    results[index].fixture.expected_writes,
                write_label);
        }
        if (!invalid_ready ||
            invalid_reason == COURSE_REASON_STUDENT_TODO) {
            test_todo(context,
                      "G5 invalid frame produces zero simulator writes");
        } else {
            test_check(context,
                       invalid_reason == COURSE_REASON_BAD_FRAME &&
                           invalid_writes == 0,
                       "G5 invalid frame produces zero simulator writes");
        }
    }

    test_check(context,
               write_freshness_csv(context, results, message_count,
                                   invalid_reason, invalid_writes),
               "Day2 qos-freshness.csv written");
    test_check(context, write_topic_table(context, topics, topic_count),
               "Day2 topic-table.txt written");
    test_check(context,
               write_trace_log(context, results, message_count,
                               &invalid_trace),
               "Day2 trace.jsonl written");
    test_check(context, write_manifest(context),
               "Day2 manifest.json written");
    return test_finish(context);
}
