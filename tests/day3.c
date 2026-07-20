#define _POSIX_C_SOURCE 200809L

#include "course.h"
#include "test_harness.h"

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

enum {
    DAY3_MAX_CLIENT_FIXTURES = 8,
    DAY3_MAX_WORKLOADS = 8,
    DAY3_MAX_REPETITIONS = 16,
    DAY3_TEXT_SIZE = 128
};

typedef struct {
    char fixture_id[DAY3_TEXT_SIZE];
    uint32_t client_id;
    char class_name[DAY3_TEXT_SIZE];
    ClientClass class_id;
    size_t ready_bytes;
    uint64_t pause_ms;
    size_t queued_bytes;
    char expected_policy[DAY3_TEXT_SIZE];
} Day3ClientFixture;

typedef struct {
    char fixture_id[DAY3_TEXT_SIZE];
    size_t client_count;
    char classes[DAY3_TEXT_SIZE];
    size_t repetitions;
    char metric_clock[DAY3_TEXT_SIZE];
    char required_metrics[DAY3_TEXT_SIZE];
} Day3WorkloadFixture;

typedef struct {
    RuntimeStats stats;
    uint64_t elapsed_ns;
    bool call_ok;
    bool nonblocking_ok;
    bool budget_ok;
} Day3Sample;

typedef struct {
    const Day3WorkloadFixture *fixture;
    Day3Sample samples[DAY3_MAX_REPETITIONS];
    RuntimeStats aggregate;
    uint64_t p50_ns;
    uint64_t p95_ns;
    uint64_t p99_ns;
    uint64_t max_ns;
    bool all_calls_ok;
    bool all_nonblocking;
    bool all_bounded;
} Day3Run;

static void check_or_todo(TestContext *context, bool condition,
                          const char *label) {
    if (condition) {
        test_check(context, true, label);
    } else {
        test_todo(context, label);
    }
}

static void trim_line(char *line) {
    size_t length = strlen(line);

    while (length > 0 &&
           (line[length - 1] == '\n' || line[length - 1] == '\r')) {
        line[--length] = '\0';
    }
}

static size_t split_fields(char *line, char separator, char **fields,
                           size_t capacity) {
    size_t count = 0;
    char *start = line;

    if (capacity == 0) {
        return 0;
    }
    for (char *cursor = line;; cursor++) {
        if (*cursor != separator && *cursor != '\0') {
            continue;
        }
        if (count >= capacity) {
            return capacity + 1;
        }
        fields[count++] = start;
        if (*cursor == '\0') {
            break;
        }
        *cursor = '\0';
        start = cursor + 1;
    }
    return count;
}

static bool copy_text(char *destination, size_t capacity,
                      const char *source) {
    const size_t length = strlen(source);

    if (length >= capacity) {
        return false;
    }
    memcpy(destination, source, length + 1);
    return true;
}

static bool parse_u64(const char *text, uint64_t *value) {
    char *end = NULL;
    unsigned long long parsed;

    errno = 0;
    parsed = strtoull(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0') {
        return false;
    }
    *value = (uint64_t)parsed;
    return true;
}

static bool parse_size(const char *text, size_t *value) {
    uint64_t parsed;

    if (!parse_u64(text, &parsed) || parsed > SIZE_MAX) {
        return false;
    }
    *value = (size_t)parsed;
    return true;
}

static bool parse_class(const char *name, ClientClass *class_id) {
    if (strcmp(name, "fast") == 0) {
        *class_id = COURSE_CLIENT_FAST;
        return true;
    }
    if (strcmp(name, "slow") == 0) {
        *class_id = COURSE_CLIENT_SLOW;
        return true;
    }
    if (strcmp(name, "flood") == 0) {
        *class_id = COURSE_CLIENT_FLOOD;
        return true;
    }
    if (strcmp(name, "safety") == 0) {
        *class_id = COURSE_CLIENT_SAFETY;
        return true;
    }
    return false;
}

static bool load_client_fixtures(Day3ClientFixture *fixtures,
                                 size_t capacity, size_t *fixture_count) {
    static const char expected_header[] =
        "fixture_id,client_id,class,ready_bytes,pause_ms,queued_bytes,"
        "expected_policy";
    FILE *file = fopen("fixtures/day3/clients.csv", "r");
    char line[512];
    size_t count = 0;
    bool valid = true;

    if (file == NULL || fgets(line, sizeof(line), file) == NULL) {
        if (file != NULL) {
            fclose(file);
        }
        return false;
    }
    trim_line(line);
    valid = strcmp(line, expected_header) == 0;
    while (valid && fgets(line, sizeof(line), file) != NULL) {
        char *fields[7];
        uint64_t client_id;

        trim_line(line);
        if (line[0] == '\0') {
            continue;
        }
        if (count >= capacity || split_fields(line, ',', fields, 7) != 7 ||
            !copy_text(fixtures[count].fixture_id,
                       sizeof(fixtures[count].fixture_id), fields[0]) ||
            !parse_u64(fields[1], &client_id) || client_id > UINT32_MAX ||
            !copy_text(fixtures[count].class_name,
                       sizeof(fixtures[count].class_name), fields[2]) ||
            !parse_class(fields[2], &fixtures[count].class_id) ||
            !parse_size(fields[3], &fixtures[count].ready_bytes) ||
            !parse_u64(fields[4], &fixtures[count].pause_ms) ||
            !parse_size(fields[5], &fixtures[count].queued_bytes) ||
            !copy_text(fixtures[count].expected_policy,
                       sizeof(fixtures[count].expected_policy), fields[6])) {
            valid = false;
            break;
        }
        fixtures[count].client_id = (uint32_t)client_id;
        count++;
    }
    valid = valid && !ferror(file) && count > 0;
    fclose(file);
    *fixture_count = count;
    return valid;
}

static bool load_workload_fixtures(Day3WorkloadFixture *fixtures,
                                   size_t capacity, size_t *fixture_count) {
    static const char expected_header[] =
        "fixture_id,client_count,classes,repetitions,metric_clock,"
        "required_metrics";
    FILE *file = fopen("fixtures/day3/workloads.csv", "r");
    char line[512];
    size_t count = 0;
    bool valid = true;

    if (file == NULL || fgets(line, sizeof(line), file) == NULL) {
        if (file != NULL) {
            fclose(file);
        }
        return false;
    }
    trim_line(line);
    valid = strcmp(line, expected_header) == 0;
    while (valid && fgets(line, sizeof(line), file) != NULL) {
        char *fields[6];

        trim_line(line);
        if (line[0] == '\0') {
            continue;
        }
        if (count >= capacity || split_fields(line, ',', fields, 6) != 6 ||
            !copy_text(fixtures[count].fixture_id,
                       sizeof(fixtures[count].fixture_id), fields[0]) ||
            !parse_size(fields[1], &fixtures[count].client_count) ||
            !copy_text(fixtures[count].classes,
                       sizeof(fixtures[count].classes), fields[2]) ||
            !parse_size(fields[3], &fixtures[count].repetitions) ||
            !copy_text(fixtures[count].metric_clock,
                       sizeof(fixtures[count].metric_clock), fields[4]) ||
            !copy_text(fixtures[count].required_metrics,
                       sizeof(fixtures[count].required_metrics), fields[5])) {
            valid = false;
            break;
        }
        count++;
    }
    valid = valid && !ferror(file) && count > 0;
    fclose(file);
    *fixture_count = count;
    return valid;
}

static const Day3ClientFixture *find_client_fixture(
    const Day3ClientFixture *fixtures, size_t fixture_count,
    const char *class_name) {
    for (size_t index = 0; index < fixture_count; index++) {
        if (strcmp(fixtures[index].class_name, class_name) == 0) {
            return &fixtures[index];
        }
    }
    return NULL;
}

static size_t workload_classes(
    const Day3WorkloadFixture *workload,
    const Day3ClientFixture *client_fixtures, size_t client_fixture_count,
    const Day3ClientFixture **class_sequence, size_t capacity) {
    char classes[DAY3_TEXT_SIZE];
    char *fields[DAY3_MAX_CLIENT_FIXTURES];
    size_t field_count;

    if (!copy_text(classes, sizeof(classes), workload->classes)) {
        return 0;
    }
    field_count = split_fields(classes, '|', fields,
                               sizeof(fields) / sizeof(fields[0]));
    if (field_count == 0 || field_count > capacity) {
        return 0;
    }
    for (size_t index = 0; index < field_count; index++) {
        class_sequence[index] = find_client_fixture(
            client_fixtures, client_fixture_count, fields[index]);
        if (class_sequence[index] == NULL) {
            return 0;
        }
    }
    return field_count;
}

static bool client_fixture_contract_ok(const Day3ClientFixture *fixtures,
                                       size_t fixture_count) {
    const Day3ClientFixture *fast =
        find_client_fixture(fixtures, fixture_count, "fast");
    const Day3ClientFixture *slow =
        find_client_fixture(fixtures, fixture_count, "slow");
    const Day3ClientFixture *flood =
        find_client_fixture(fixtures, fixture_count, "flood");
    const Day3ClientFixture *safety =
        find_client_fixture(fixtures, fixture_count, "safety");

    return fixture_count == 4 && fast != NULL && slow != NULL &&
           flood != NULL && safety != NULL && fast->client_id == 1 &&
           slow->client_id == 2 && flood->client_id == 3 &&
           safety->client_id == 4 && fast->ready_bytes == 1 &&
           slow->ready_bytes == 1 && flood->ready_bytes == 64 &&
           safety->ready_bytes == 1 && slow->pause_ms == 120 &&
           flood->queued_bytes == COURSE_MAX_QUEUE_BYTES + 1 &&
           strcmp(fast->expected_policy, "dispatch_within_budget") == 0 &&
           strcmp(slow->expected_policy, "never_block_fast_or_safety") == 0 &&
           strcmp(flood->expected_policy, "drop_fail_closed") == 0 &&
           strcmp(safety->expected_policy, "dispatch_within_budget") == 0;
}

static bool workload_fixture_contract_ok(
    const Day3WorkloadFixture *workloads, size_t workload_count,
    const Day3ClientFixture *clients, size_t client_count) {
    bool found_one = false;
    bool found_ten = false;
    bool found_fifty = false;

    if (workload_count != 3) {
        return false;
    }
    for (size_t index = 0; index < workload_count; index++) {
        const Day3ClientFixture *sequence[DAY3_MAX_CLIENT_FIXTURES];
        const size_t sequence_count = workload_classes(
            &workloads[index], clients, client_count, sequence,
            sizeof(sequence) / sizeof(sequence[0]));
        const bool common_ok = workloads[index].repetitions == 5 &&
                               strcmp(workloads[index].metric_clock,
                                      "CLOCK_MONOTONIC") == 0 &&
                               strcmp(workloads[index].required_metrics,
                                      "p50|p95|p99|max|queue_age") == 0;

        if (!common_ok || workloads[index].client_count == 0 ||
            workloads[index].client_count > COURSE_MAX_CLIENTS) {
            return false;
        }
        if (workloads[index].client_count == 1) {
            found_one = sequence_count == 1 &&
                        sequence[0]->class_id == COURSE_CLIENT_FAST;
        } else if (workloads[index].client_count == 10) {
            found_ten = sequence_count == 4 &&
                        sequence[0]->class_id == COURSE_CLIENT_FAST &&
                        sequence[1]->class_id == COURSE_CLIENT_SLOW &&
                        sequence[2]->class_id == COURSE_CLIENT_FLOOD &&
                        sequence[3]->class_id == COURSE_CLIENT_SAFETY;
        } else if (workloads[index].client_count == 50) {
            found_fifty = sequence_count == 4 &&
                          sequence[0]->class_id == COURSE_CLIENT_FAST &&
                          sequence[1]->class_id == COURSE_CLIENT_SLOW &&
                          sequence[2]->class_id == COURSE_CLIENT_FLOOD &&
                          sequence[3]->class_id == COURSE_CLIENT_SAFETY;
        } else {
            return false;
        }
    }
    return found_one && found_ten && found_fifty;
}

static bool write_ready_bytes(int fd, size_t byte_count) {
    uint8_t bytes[COURSE_CLIENT_BUFFER_SIZE];
    size_t written = 0;

    if (byte_count == 0 || byte_count > sizeof(bytes)) {
        return false;
    }
    memset(bytes, 0x5a, byte_count);
    while (written < byte_count) {
        const ssize_t result =
            write(fd, bytes + written, byte_count - written);

        if (result < 0 && errno == EINTR) {
            continue;
        }
        if (result <= 0) {
            return false;
        }
        written += (size_t)result;
    }
    return true;
}

static Day3Sample run_sample(
    const Day3WorkloadFixture *workload,
    const Day3ClientFixture *client_fixtures, size_t client_fixture_count) {
    ClientState clients[COURSE_MAX_CLIENTS];
    int pipes[COURSE_MAX_CLIENTS][2];
    const Day3ClientFixture *class_sequence[DAY3_MAX_CLIENT_FIXTURES];
    const size_t class_count = workload_classes(
        workload, client_fixtures, client_fixture_count, class_sequence,
        sizeof(class_sequence) / sizeof(class_sequence[0]));
    Day3Sample sample;
    uint64_t start_ns;
    uint64_t end_ns;
    int result;

    memset(&sample, 0, sizeof(sample));
    memset(clients, 0, sizeof(clients));
    for (size_t index = 0; index < COURSE_MAX_CLIENTS; index++) {
        pipes[index][0] = -1;
        pipes[index][1] = -1;
    }
    sample.nonblocking_ok = true;
    sample.budget_ok = true;
    if (class_count == 0 || workload->client_count == 0 ||
        workload->client_count > COURSE_MAX_CLIENTS) {
        return sample;
    }

    for (size_t index = 0; index < workload->client_count; index++) {
        const Day3ClientFixture *fixture =
            class_sequence[index % class_count];
        const uint64_t now_ns = monotonic_ns();
        const uint64_t pause_ns =
            fixture->pause_ms <= UINT64_MAX / UINT64_C(1000000)
                ? fixture->pause_ms * UINT64_C(1000000)
                : UINT64_MAX;

        if (pipe(pipes[index]) != 0) {
            goto cleanup;
        }
        clients[index].fd = pipes[index][0];
        clients[index].client_id = (uint32_t)(index + 1);
        clients[index].class_id = fixture->class_id;
        clients[index].oldest_enqueue_ns =
            pause_ns < now_ns ? now_ns - pause_ns : 0;
        clients[index].queued_bytes = fixture->queued_bytes;
        if (set_nonblocking(clients[index].fd) != 0) {
            sample.nonblocking_ok = false;
        } else {
            const int flags = fcntl(clients[index].fd, F_GETFL, 0);

            sample.nonblocking_ok = sample.nonblocking_ok && flags >= 0 &&
                                    (flags & O_NONBLOCK) != 0;
        }
        if (!write_ready_bytes(pipes[index][1], fixture->ready_bytes)) {
            goto cleanup;
        }
    }

    start_ns = monotonic_ns();
    result = poll_service_once(clients, workload->client_count, 20,
                               &sample.stats);
    end_ns = monotonic_ns();
    sample.elapsed_ns = end_ns >= start_ns ? end_ns - start_ns : 0;
    sample.call_ok = result == (int)workload->client_count &&
                     sample.stats.ready_events == workload->client_count;
    for (size_t index = 0; index < workload->client_count; index++) {
        if (clients[index].work_this_turn > COURSE_WORK_BUDGET) {
            sample.budget_ok = false;
        }
    }

cleanup:
    for (size_t index = 0; index < workload->client_count; index++) {
        if (pipes[index][0] >= 0) {
            close(pipes[index][0]);
        }
        if (pipes[index][1] >= 0) {
            close(pipes[index][1]);
        }
    }
    return sample;
}

static void aggregate_stats(RuntimeStats *aggregate,
                            const RuntimeStats *sample) {
    aggregate->ready_events += sample->ready_events;
    aggregate->fast_dispatched += sample->fast_dispatched;
    aggregate->safety_dispatched += sample->safety_dispatched;
    aggregate->slow_dispatched += sample->slow_dispatched;
    aggregate->flood_dropped += sample->flood_dropped;
    if (sample->max_work_per_client > aggregate->max_work_per_client) {
        aggregate->max_work_per_client = sample->max_work_per_client;
    }
    if (sample->max_queue_age_ns > aggregate->max_queue_age_ns) {
        aggregate->max_queue_age_ns = sample->max_queue_age_ns;
    }
}

static void sort_u64(uint64_t *values, size_t count) {
    for (size_t index = 1; index < count; index++) {
        const uint64_t value = values[index];
        size_t position = index;

        while (position > 0 && values[position - 1] > value) {
            values[position] = values[position - 1];
            position--;
        }
        values[position] = value;
    }
}

static uint64_t nearest_rank(const uint64_t *sorted, size_t count,
                             size_t percentile) {
    const size_t rank = (percentile * count + 99) / 100;

    if (count == 0) {
        return 0;
    }
    return sorted[rank == 0 ? 0 : rank - 1];
}

static Day3Run run_workload(
    const Day3WorkloadFixture *workload,
    const Day3ClientFixture *client_fixtures, size_t client_fixture_count) {
    Day3Run run;
    uint64_t sorted[DAY3_MAX_REPETITIONS] = {0};

    memset(&run, 0, sizeof(run));
    run.fixture = workload;
    run.all_calls_ok = true;
    run.all_nonblocking = true;
    run.all_bounded = true;
    if (workload->repetitions == 0 ||
        workload->repetitions > DAY3_MAX_REPETITIONS) {
        run.all_calls_ok = false;
        run.all_nonblocking = false;
        run.all_bounded = false;
        return run;
    }
    for (size_t index = 0; index < workload->repetitions; index++) {
        run.samples[index] = run_sample(workload, client_fixtures,
                                        client_fixture_count);
        sorted[index] = run.samples[index].elapsed_ns;
        aggregate_stats(&run.aggregate, &run.samples[index].stats);
        run.all_calls_ok = run.all_calls_ok && run.samples[index].call_ok;
        run.all_nonblocking =
            run.all_nonblocking && run.samples[index].nonblocking_ok;
        run.all_bounded = run.all_bounded && run.samples[index].budget_ok;
    }
    sort_u64(sorted, workload->repetitions);
    run.p50_ns = nearest_rank(sorted, workload->repetitions, 50);
    run.p95_ns = nearest_rank(sorted, workload->repetitions, 95);
    run.p99_ns = nearest_rank(sorted, workload->repetitions, 99);
    run.max_ns = sorted[workload->repetitions - 1];
    return run;
}

static const Day3Run *find_run(const Day3Run *runs, size_t run_count,
                               size_t client_count) {
    for (size_t index = 0; index < run_count; index++) {
        if (runs[index].fixture->client_count == client_count) {
            return &runs[index];
        }
    }
    return NULL;
}

static bool run_behavior_ok(const Day3Run *run) {
    return run->all_calls_ok && run->all_nonblocking && run->all_bounded &&
           run->aggregate.max_work_per_client > 0 &&
           run->aggregate.max_work_per_client <= COURSE_WORK_BUDGET;
}

static void write_manifest(TestContext *context,
                           const Day3Run *runs, size_t run_count) {
    const char *image = getenv("COURSE_CONTAINER_IMAGE");
    const char *commit = getenv("COURSE_COMMIT");
    FILE *file = test_open_evidence(context, "manifest.json");

    if (file == NULL) {
        test_check(context, false, "evidence manifest is writable");
        return;
    }
    fprintf(file,
            "{\n  \"group\": \"%s\",\n"
            "  \"day\": 3,\n"
            "  \"input_tag\": \"transport_v1\",\n"
            "  \"output_tag\": \"runtime_poll_v1\",\n"
            "  \"seed\": %" PRIu64 ",\n"
            "  \"container_image\": \"%s\",\n"
            "  \"compiler\": \"%s\",\n"
            "  \"commit\": \"%s\",\n"
            "  \"fixture_workloads\": %zu,\n"
            "  \"fixture_repetitions_each\": %zu,\n"
            "  \"command\": \"make verify-day3\"\n}\n",
            context->group, context->seed,
            image == NULL ? "NOT_RECORDED" : image, __VERSION__,
            commit == NULL ? "NOT_A_GIT_CHECKOUT" : commit, run_count,
            run_count == 0 ? 0 : runs[0].fixture->repetitions);
    fclose(file);
}

static void write_evidence(TestContext *context, const Day3Run *runs,
                           size_t run_count, bool trace_is_complete) {
    const Day3Run *largest = find_run(runs, run_count, 50);
    FILE *file = test_open_evidence(context, "blocking-vs-poll.csv");

    if (file != NULL) {
        fprintf(file,
                "runtime,workload,client_count,path,repetitions,p50_ns,p95_ns,"
                "p99_ns,max_ns,queue_age_ns,status\n");
        for (size_t index = 0; index < run_count; index++) {
            fprintf(file,
                    "poll,%s,%zu,%s,%zu,%" PRIu64 ",%" PRIu64
                    ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%s\n",
                    runs[index].fixture->fixture_id,
                    runs[index].fixture->client_count,
                    runs[index].fixture->classes,
                    runs[index].fixture->repetitions, runs[index].p50_ns,
                    runs[index].p95_ns, runs[index].p99_ns,
                    runs[index].max_ns,
                    runs[index].aggregate.max_queue_age_ns,
                    run_behavior_ok(&runs[index]) ? "PASS"
                                                  : "TODO_NOT_IMPLEMENTED");
        }
        fclose(file);
    }

    file = test_open_evidence(context, "runtime-samples.csv");
    if (file != NULL) {
        fprintf(file,
                "fixture_id,client_count,repetition,elapsed_ns,ready_events,"
                "fast_dispatched,slow_dispatched,safety_dispatched,"
                "flood_dropped,max_work_per_client\n");
        for (size_t run_index = 0; run_index < run_count; run_index++) {
            for (size_t sample_index = 0;
                 sample_index < runs[run_index].fixture->repetitions;
                 sample_index++) {
                const Day3Sample *sample =
                    &runs[run_index].samples[sample_index];

                fprintf(file,
                        "%s,%zu,%zu,%" PRIu64 ",%zu,%zu,%zu,%zu,%zu,%zu\n",
                        runs[run_index].fixture->fixture_id,
                        runs[run_index].fixture->client_count,
                        sample_index + 1, sample->elapsed_ns,
                        sample->stats.ready_events,
                        sample->stats.fast_dispatched,
                        sample->stats.slow_dispatched,
                        sample->stats.safety_dispatched,
                        sample->stats.flood_dropped,
                        sample->stats.max_work_per_client);
            }
        }
        fclose(file);
    }

    file = test_open_evidence(context, "queue-policy.json");
    if (file != NULL) {
        fprintf(file,
                "{\"work_budget\":%d,\"client_buffer_bytes\":%d,"
                "\"max_queue_bytes\":%zu,\"max_queue_age_ns\":%" PRIu64
                ",\"poll_timeout_ms\":20,\"overload_action\":"
                "\"DROP_FAIL_CLOSED\"}\n",
                COURSE_WORK_BUDGET, COURSE_CLIENT_BUFFER_SIZE,
                COURSE_MAX_QUEUE_BYTES, COURSE_MAX_QUEUE_AGE_NS);
        fclose(file);
    }

    file = test_open_evidence(context, "freshness-clients.csv");
    if (file != NULL && largest != NULL) {
        fprintf(file,
                "fast_dispatched,slow_dispatched,fast_isolated\n%zu,%zu,%d\n",
                largest->aggregate.fast_dispatched,
                largest->aggregate.slow_dispatched,
                largest->aggregate.fast_dispatched > 0);
        fclose(file);
    } else if (file != NULL) {
        fclose(file);
    }
    file = test_open_evidence(context, "queue-location.csv");
    if (file != NULL) {
        fprintf(file,
                "trace_id,t_pub_ns,t_rx_ns,t_gate_ns,t_ack_ns,location,complete\n"
                "3001,100,110,120,130,runtime_poll,%d\n",
                trace_is_complete);
        fclose(file);
    }
    file = test_open_evidence(context, "priority-risk.csv");
    if (file != NULL && largest != NULL) {
        fprintf(file,
                "safety_dispatched,p99_latency_ns,within_budget\n%zu,%" PRIu64
                ",%d\n",
                largest->aggregate.safety_dispatched, largest->p99_ns,
                largest->aggregate.safety_dispatched > 0);
        fclose(file);
    } else if (file != NULL) {
        fclose(file);
    }
    file = test_open_evidence(context, "integrated-poll.log");
    if (file != NULL && largest != NULL) {
        fprintf(file,
                "fast=%zu safety=%zu slow=%zu flood_dropped=%zu "
                "max_work=%zu status=%s\n",
                largest->aggregate.fast_dispatched,
                largest->aggregate.safety_dispatched,
                largest->aggregate.slow_dispatched,
                largest->aggregate.flood_dropped,
                largest->aggregate.max_work_per_client,
                largest->aggregate.fast_dispatched > 0 &&
                        largest->aggregate.safety_dispatched > 0
                    ? "PASS"
                    : "TODO_NOT_IMPLEMENTED");
        fclose(file);
    } else if (file != NULL) {
        fclose(file);
    }
    write_manifest(context, runs, run_count);
}

int verify_day3(TestContext *context) {
    Day3ClientFixture clients[DAY3_MAX_CLIENT_FIXTURES];
    Day3WorkloadFixture workloads[DAY3_MAX_WORKLOADS];
    Day3Run runs[DAY3_MAX_WORKLOADS];
    size_t client_count = 0;
    size_t workload_count = 0;
    const bool clients_loaded = load_client_fixtures(
        clients, sizeof(clients) / sizeof(clients[0]), &client_count);
    const bool workloads_loaded = load_workload_fixtures(
        workloads, sizeof(workloads) / sizeof(workloads[0]), &workload_count);
    const bool fixture_contract =
        clients_loaded && workloads_loaded &&
        client_fixture_contract_ok(clients, client_count) &&
        workload_fixture_contract_ok(workloads, workload_count, clients,
                                     client_count);
    TraceRow good_trace = {
        .trace_id = 3001,
        .seq = 41,
        .t_pub_ns = 100,
        .t_rx_ns = 110,
        .t_gate_ns = 120,
        .t_ack_ns = 130,
        .verdict = COURSE_VERDICT_APPROVE,
        .reason = COURSE_REASON_NONE,
    };
    TraceRow bad_trace = good_trace;
    const Day3Run *one;
    const Day3Run *ten;
    const Day3Run *fifty;
    bool trace_is_complete;

    test_check(context, fixture_contract,
               "Day3 fixed client and workload fixtures match the contract");
    if (!fixture_contract) {
        return test_finish(context);
    }
    memset(runs, 0, sizeof(runs));
    for (size_t index = 0; index < workload_count; index++) {
        runs[index] = run_workload(&workloads[index], clients, client_count);
    }
    one = find_run(runs, workload_count, 1);
    ten = find_run(runs, workload_count, 10);
    fifty = find_run(runs, workload_count, 50);
    trace_is_complete = trace_complete(&good_trace);
    bad_trace.t_gate_ns = 90;

    if (test_group_enabled(context, "G1")) {
        ArmState earlier_state;
        ArmState later_state;
        bool progression_valid;

        memset(&earlier_state, 0, sizeof(earlier_state));
        earlier_state.seq = 30;
        earlier_state.t_mono_ns = INT64_C(3000000000);
        earlier_state.frame_id = COURSE_FRAME_ID_BASE;
        later_state = earlier_state;
        later_state.seq += 20;
        later_state.t_mono_ns += INT64_C(200000000);
        progression_valid = state_valid(&later_state, &earlier_state);
        check_or_todo(context, progression_valid,
                      "G1 owned state API preserves a valid source progression across slow consumption");
        check_or_todo(context,
                      fifty != NULL &&
                          fifty->aggregate.fast_dispatched > 0,
                      "G1 slow consumer does not starve fast state");
    }
    if (test_group_enabled(context, "G2")) {
        ArmCommand owned_command;
        ArmCommand owned_accepted;
        FreshnessGate owned_gate;
        TraceRow owned_trace;
        uint8_t owned_frame[COURSE_FRAME_V1_LEN];
        size_t owned_length = 0;
        Reason owned_reason;
        bool owned_encoded;

        memset(&owned_command, 0, sizeof(owned_command));
        memset(&owned_accepted, 0, sizeof(owned_accepted));
        memset(&owned_gate, 0, sizeof(owned_gate));
        memset(&owned_trace, 0, sizeof(owned_trace));
        memset(owned_frame, 0, sizeof(owned_frame));
        owned_command.seq = 31;
        owned_command.t_source_ns = UINT64_C(3000000000);
        owned_command.trace_id = context->seed + UINT64_C(31);
        owned_encoded = frame_encode(&owned_command, owned_frame,
                                     sizeof(owned_frame), &owned_length);
        owned_reason = transport_on_message(
            &owned_gate, owned_frame, owned_length, UINT64_C(3010000000),
            &owned_trace, &owned_accepted);
        if (!owned_encoded ||
            owned_reason == COURSE_REASON_STUDENT_TODO) {
            test_todo(context,
                      "G2 owned transport API emits a complete runtime-handoff trace");
        } else {
            test_check(context,
                       owned_reason == COURSE_REASON_NONE &&
                           owned_trace.trace_id == owned_command.trace_id &&
                           owned_trace.seq == owned_command.seq &&
                           owned_trace.t_pub_ns ==
                               owned_command.t_source_ns &&
                           owned_trace.t_rx_ns > 0 &&
                           owned_trace.t_rx_ns <= owned_trace.t_gate_ns &&
                           owned_trace.t_gate_ns <= owned_trace.t_ack_ns,
                       "G2 owned transport API emits a complete runtime-handoff trace");
        }
        check_or_todo(context, trace_is_complete,
                      "G2 four transport timestamps are complete");
        test_check(context, !trace_complete(&bad_trace),
                   "G2 rejects out-of-order timestamp evidence");
    }
    if (test_group_enabled(context, "G3")) {
        test_check(context,
                   one != NULL && ten != NULL && fifty != NULL &&
                       one->all_nonblocking && ten->all_nonblocking &&
                       fifty->all_nonblocking,
                   "G3 descriptors are O_NONBLOCK in all fixture repetitions");
        check_or_todo(context,
                      one != NULL && ten != NULL && fifty != NULL &&
                          run_behavior_ok(one) && run_behavior_ok(ten) &&
                          run_behavior_ok(fifty) &&
                          ten->aggregate.flood_dropped > 0 &&
                          fifty->aggregate.flood_dropped > 0,
                      "G3 owned runtime API services 1/10/50 clients for five repetitions "
                      "with bounded work and backpressure");
    }
    if (test_group_enabled(context, "G4")) {
        const SafetyDecision first = safety_gate(NULL, NULL, 0);
        const SafetyDecision second = safety_gate(NULL, NULL, 0);

        check_or_todo(context,
                      first.reason != COURSE_REASON_STUDENT_TODO &&
                          first.verdict == COURSE_VERDICT_REJECT &&
                          first.reason == COURSE_REASON_INTERNAL &&
                          first.verdict == second.verdict &&
                          first.reason == second.reason,
                      "G4 owned safety API gives a stable explicit result on repeated dispatch");
        check_or_todo(context,
                      fifty != NULL &&
                          fifty->aggregate.safety_dispatched > 0,
                      "G4 safety path remains ready under slow and flood load");
    }
    if (test_group_enabled(context, "G5")) {
        FreshnessGate first_freshness;
        FreshnessGate second_freshness;
        ActuatorWriter first_writer;
        ActuatorWriter second_writer;
        ArmState owned_state;
        TraceRow first_trace;
        TraceRow second_trace;
        const uint8_t invalid_frame[] = {0};
        Verdict first_verdict;
        Verdict second_verdict;

        memset(&first_freshness, 0, sizeof(first_freshness));
        memset(&second_freshness, 0, sizeof(second_freshness));
        memset(&owned_state, 0, sizeof(owned_state));
        memset(&first_trace, 0, sizeof(first_trace));
        memset(&second_trace, 0, sizeof(second_trace));
        writer_init(&first_writer, 35);
        writer_init(&second_writer, 36);
        first_verdict = gatekeeper_process(
            &first_freshness, &first_writer, &owned_state, invalid_frame,
            sizeof(invalid_frame), UINT64_C(3030000000), &first_trace);
        second_verdict = gatekeeper_process(
            &second_freshness, &second_writer, &owned_state, invalid_frame,
            sizeof(invalid_frame), UINT64_C(3030000000), &second_trace);
        check_or_todo(context,
                      first_trace.reason != COURSE_REASON_STUDENT_TODO &&
                          second_trace.reason != COURSE_REASON_STUDENT_TODO &&
                          first_verdict == COURSE_VERDICT_REJECT &&
                          second_verdict == first_verdict &&
                          first_trace.reason == COURSE_REASON_BAD_FRAME &&
                          second_trace.reason == first_trace.reason &&
                          first_writer.count == 0 && second_writer.count == 0,
                      "G5 owned gatekeeper handles repeated malformed frames deterministically without writing");
        check_or_todo(context,
                      fifty != NULL &&
                          fifty->aggregate.fast_dispatched > 0 &&
                          fifty->aggregate.safety_dispatched > 0,
                      "G5 integrated runtime serves fast and safety clients");
    }

    write_evidence(context, runs, workload_count, trace_is_complete);
    return test_finish(context);
}
