#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "course.h"
#include "test_harness.h"

#include <ctype.h>
#include <errno.h>
#include <inttypes.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/timerfd.h>
#include <unistd.h>

enum {
    DAY5_MAX_CONTROLLER_FIXTURES = 16,
    DAY5_MAX_RELEASE_FIXTURES = 16,
    DAY5_MAX_ESTOP_RUNS = 256,
    DAY5_TEXT_SIZE = 128,
    DAY5_JSON_SIZE = 4096
};

typedef enum {
    ACTION_EXPECT_USE = 0,
    ACTION_EXPECT_FALLBACK,
    ACTION_EXPECT_DISCARD,
    ACTION_EXPECT_FALLBACK_OR_DISCARD
} ActionExpectation;

typedef enum {
    VERDICT_EXPECT_APPROVE = 0,
    VERDICT_EXPECT_REJECT,
    VERDICT_EXPECT_FALLBACK,
    VERDICT_EXPECT_DISCARD,
    VERDICT_EXPECT_FALLBACK_OR_DISCARD
} VerdictExpectation;

typedef struct {
    char fixture_id[DAY5_TEXT_SIZE];
    char fault[DAY5_TEXT_SIZE];
    uint64_t generation;
    uint64_t produced_age_ns;
    double confidence;
    char value[DAY5_TEXT_SIZE];
    char expected_action_text[DAY5_TEXT_SIZE];
    ActionExpectation expected_action;
    Reason expected_reason;
} ControllerFixture;

typedef struct {
    char fixture_id[DAY5_TEXT_SIZE];
    char class_name[DAY5_TEXT_SIZE];
    char input[DAY5_TEXT_SIZE];
    char expected_verdict_text[DAY5_TEXT_SIZE];
    VerdictExpectation expected_verdict;
    size_t expected_writes;
} ReleaseFixture;

typedef struct {
    char fixture_id[DAY5_TEXT_SIZE];
    uint64_t seed;
    size_t repetitions;
    size_t planner_hz;
    size_t ordinary_source_count;
    size_t ready_source_count;
    uint64_t deadline_ns;
    size_t expected_passes;
    size_t expected_ordinary_ahead;
    bool source_sets_valid;
} EstopFixture;

typedef struct {
    size_t run;
    uint64_t trigger_ns;
    uint64_t observed_ns;
    uint64_t latency_ns;
    size_t ordinary_enqueued;
    size_t ordinary_backlog_after;
    size_t ordinary_ahead;
    bool passed;
} EstopObservation;

typedef struct {
    const ControllerFixture *fixture;
    ControllerAction action;
    Reason reason;
    bool passed;
} ControllerObservation;

typedef struct {
    const ReleaseFixture *fixture;
    Verdict verdict;
    Reason reason;
    size_t writes;
    uint64_t trace_id;
    bool passed;
} ReleaseObservation;

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

static bool parse_double(const char *text, double *value) {
    char *end = NULL;

    errno = 0;
    *value = strtod(text, &end);
    return errno == 0 && end != text && *end == '\0';
}

static bool parse_action_expectation(const char *text,
                                     ActionExpectation *expectation) {
    if (strcmp(text, "USE") == 0) {
        *expectation = ACTION_EXPECT_USE;
        return true;
    }
    if (strcmp(text, "FALLBACK") == 0) {
        *expectation = ACTION_EXPECT_FALLBACK;
        return true;
    }
    if (strcmp(text, "DISCARD") == 0) {
        *expectation = ACTION_EXPECT_DISCARD;
        return true;
    }
    if (strcmp(text, "FALLBACK_OR_DISCARD") == 0) {
        *expectation = ACTION_EXPECT_FALLBACK_OR_DISCARD;
        return true;
    }
    return false;
}

static bool parse_verdict_expectation(const char *text,
                                      VerdictExpectation *expectation) {
    if (strcmp(text, "APPROVE") == 0) {
        *expectation = VERDICT_EXPECT_APPROVE;
        return true;
    }
    if (strcmp(text, "REJECT") == 0) {
        *expectation = VERDICT_EXPECT_REJECT;
        return true;
    }
    if (strcmp(text, "FALLBACK") == 0) {
        *expectation = VERDICT_EXPECT_FALLBACK;
        return true;
    }
    if (strcmp(text, "DISCARD") == 0) {
        *expectation = VERDICT_EXPECT_DISCARD;
        return true;
    }
    if (strcmp(text, "FALLBACK_OR_DISCARD") == 0) {
        *expectation = VERDICT_EXPECT_FALLBACK_OR_DISCARD;
        return true;
    }
    return false;
}

static bool parse_reason(const char *text, Reason *reason) {
    struct ReasonName {
        const char *name;
        Reason value;
    };
    static const struct ReasonName names[] = {
        {"NONE", COURSE_REASON_NONE},
        {"CANCELLED", COURSE_REASON_CANCELLED},
        {"CONTROLLER_TIMEOUT", COURSE_REASON_CONTROLLER_TIMEOUT},
        {"CONTROLLER_INVALID", COURSE_REASON_CONTROLLER_INVALID},
        {"LOW_CONFIDENCE", COURSE_REASON_LOW_CONFIDENCE},
    };

    for (size_t index = 0; index < sizeof(names) / sizeof(names[0]); index++) {
        if (strcmp(text, names[index].name) == 0) {
            *reason = names[index].value;
            return true;
        }
    }
    return false;
}

static bool action_matches(ActionExpectation expected,
                           ControllerAction actual) {
    switch (expected) {
        case ACTION_EXPECT_USE:
            return actual == COURSE_CONTROLLER_USE;
        case ACTION_EXPECT_FALLBACK:
            return actual == COURSE_CONTROLLER_FALLBACK;
        case ACTION_EXPECT_DISCARD:
            return actual == COURSE_CONTROLLER_DISCARD;
        case ACTION_EXPECT_FALLBACK_OR_DISCARD:
            return actual == COURSE_CONTROLLER_FALLBACK ||
                   actual == COURSE_CONTROLLER_DISCARD;
    }
    return false;
}

static bool verdict_matches(VerdictExpectation expected, Verdict actual) {
    switch (expected) {
        case VERDICT_EXPECT_APPROVE:
            return actual == COURSE_VERDICT_APPROVE;
        case VERDICT_EXPECT_REJECT:
            return actual == COURSE_VERDICT_REJECT;
        case VERDICT_EXPECT_FALLBACK:
            return actual == COURSE_VERDICT_FALLBACK;
        case VERDICT_EXPECT_DISCARD:
            return actual == COURSE_VERDICT_DISCARD;
        case VERDICT_EXPECT_FALLBACK_OR_DISCARD:
            return actual == COURSE_VERDICT_FALLBACK ||
                   actual == COURSE_VERDICT_DISCARD;
    }
    return false;
}

static const char *controller_action_name(ControllerAction action) {
    switch (action) {
        case COURSE_CONTROLLER_USE:
            return "USE";
        case COURSE_CONTROLLER_FALLBACK:
            return "FALLBACK";
        case COURSE_CONTROLLER_DISCARD:
            return "DISCARD";
    }
    return "UNKNOWN";
}

static bool load_controller_fixtures(ControllerFixture *fixtures,
                                     size_t capacity,
                                     size_t *fixture_count) {
    static const char expected_header[] =
        "fixture_id,fault,generation,produced_age_ns,confidence,value,"
        "expected_action,expected_reason";
    FILE *file = fopen("fixtures/day5/controller_faults.csv", "r");
    char line[768];
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
        char *fields[8];

        trim_line(line);
        if (line[0] == '\0') {
            continue;
        }
        if (count >= capacity || split_fields(line, ',', fields, 8) != 8 ||
            !copy_text(fixtures[count].fixture_id,
                       sizeof(fixtures[count].fixture_id), fields[0]) ||
            !copy_text(fixtures[count].fault,
                       sizeof(fixtures[count].fault), fields[1]) ||
            !parse_u64(fields[2], &fixtures[count].generation) ||
            !parse_u64(fields[3], &fixtures[count].produced_age_ns) ||
            !parse_double(fields[4], &fixtures[count].confidence) ||
            !copy_text(fixtures[count].value,
                       sizeof(fixtures[count].value), fields[5]) ||
            !copy_text(fixtures[count].expected_action_text,
                       sizeof(fixtures[count].expected_action_text),
                       fields[6]) ||
            !parse_action_expectation(fields[6],
                                      &fixtures[count].expected_action) ||
            !parse_reason(fields[7], &fixtures[count].expected_reason)) {
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

static bool load_release_fixtures(ReleaseFixture *fixtures,
                                  size_t capacity, size_t *fixture_count) {
    static const char expected_header[] =
        "fixture_id,class,input,expected_verdict,expected_writes";
    FILE *file = fopen("fixtures/day5/release_fixtures.csv", "r");
    char line[768];
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
        char *fields[5];

        trim_line(line);
        if (line[0] == '\0') {
            continue;
        }
        if (count >= capacity || split_fields(line, ',', fields, 5) != 5 ||
            !copy_text(fixtures[count].fixture_id,
                       sizeof(fixtures[count].fixture_id), fields[0]) ||
            !copy_text(fixtures[count].class_name,
                       sizeof(fixtures[count].class_name), fields[1]) ||
            !copy_text(fixtures[count].input,
                       sizeof(fixtures[count].input), fields[2]) ||
            !copy_text(fixtures[count].expected_verdict_text,
                       sizeof(fixtures[count].expected_verdict_text),
                       fields[3]) ||
            !parse_verdict_expectation(fields[3],
                                       &fixtures[count].expected_verdict) ||
            !parse_size(fields[4], &fixtures[count].expected_writes)) {
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

static bool read_text_fixture(const char *path, char *buffer,
                              size_t capacity) {
    FILE *file = fopen(path, "r");
    size_t used;

    if (file == NULL || capacity < 2) {
        if (file != NULL) {
            fclose(file);
        }
        return false;
    }
    used = fread(buffer, 1, capacity - 1, file);
    if (ferror(file) || (!feof(file) && used == capacity - 1)) {
        fclose(file);
        return false;
    }
    buffer[used] = '\0';
    fclose(file);
    return true;
}

static const char *json_value_start(const char *json, const char *key) {
    char needle[DAY5_TEXT_SIZE];
    const char *match;
    const char *colon;
    const int written = snprintf(needle, sizeof(needle), "\"%s\"", key);

    if (written < 0 || (size_t)written >= sizeof(needle)) {
        return NULL;
    }
    match = strstr(json, needle);
    if (match == NULL) {
        return NULL;
    }
    colon = strchr(match + (size_t)written, ':');
    if (colon == NULL) {
        return NULL;
    }
    colon++;
    while (*colon != '\0' && isspace((unsigned char)*colon)) {
        colon++;
    }
    return colon;
}

static bool json_get_u64(const char *json, const char *key,
                         uint64_t *value) {
    const char *start = json_value_start(json, key);
    char *end = NULL;
    unsigned long long parsed;

    if (start == NULL) {
        return false;
    }
    errno = 0;
    parsed = strtoull(start, &end, 10);
    if (errno != 0 || end == start) {
        return false;
    }
    while (*end != '\0' && isspace((unsigned char)*end)) {
        end++;
    }
    if (*end != ',' && *end != '}') {
        return false;
    }
    *value = (uint64_t)parsed;
    return true;
}

static bool json_get_size(const char *json, const char *key, size_t *value) {
    uint64_t parsed;

    if (!json_get_u64(json, key, &parsed) || parsed > SIZE_MAX) {
        return false;
    }
    *value = (size_t)parsed;
    return true;
}

static bool json_get_string(const char *json, const char *key, char *value,
                            size_t capacity) {
    const char *start = json_value_start(json, key);
    const char *end;
    size_t length;

    if (start == NULL || *start != '"') {
        return false;
    }
    start++;
    end = strchr(start, '"');
    if (end == NULL) {
        return false;
    }
    length = (size_t)(end - start);
    if (length >= capacity) {
        return false;
    }
    memcpy(value, start, length);
    value[length] = '\0';
    return true;
}

static bool json_array_bounds(const char *json, const char *key,
                              const char **start_out,
                              const char **end_out) {
    const char *start = json_value_start(json, key);
    const char *end;

    if (start == NULL || *start != '[') {
        return false;
    }
    end = strchr(start + 1, ']');
    if (end == NULL) {
        return false;
    }
    *start_out = start + 1;
    *end_out = end;
    return true;
}

static size_t json_array_string_count(const char *json, const char *key) {
    const char *start;
    const char *end;
    const char *cursor;
    size_t count = 0;

    if (!json_array_bounds(json, key, &start, &end)) {
        return SIZE_MAX;
    }
    cursor = start;
    while (cursor < end) {
        const char *quote = memchr(cursor, '"', (size_t)(end - cursor));
        const char *close;

        if (quote == NULL) {
            break;
        }
        close = memchr(quote + 1, '"', (size_t)(end - quote - 1));
        if (close == NULL) {
            return SIZE_MAX;
        }
        count++;
        cursor = close + 1;
    }
    return count;
}

static bool json_array_contains(const char *json, const char *key,
                                const char *expected) {
    const char *start;
    const char *end;
    const char *cursor;
    const size_t expected_length = strlen(expected);

    if (!json_array_bounds(json, key, &start, &end)) {
        return false;
    }
    cursor = start;
    while (cursor < end) {
        const char *quote = memchr(cursor, '"', (size_t)(end - cursor));
        const char *close;

        if (quote == NULL) {
            break;
        }
        close = memchr(quote + 1, '"', (size_t)(end - quote - 1));
        if (close == NULL) {
            return false;
        }
        if ((size_t)(close - quote - 1) == expected_length &&
            memcmp(quote + 1, expected, expected_length) == 0) {
            return true;
        }
        cursor = close + 1;
    }
    return false;
}

static bool load_estop_fixture(EstopFixture *fixture) {
    char json[DAY5_JSON_SIZE];

    memset(fixture, 0, sizeof(*fixture));
    if (!read_text_fixture("fixtures/day5/estop_100.json", json,
                           sizeof(json)) ||
        !json_get_string(json, "fixture_id", fixture->fixture_id,
                         sizeof(fixture->fixture_id)) ||
        !json_get_u64(json, "seed", &fixture->seed) ||
        !json_get_size(json, "repetitions", &fixture->repetitions) ||
        !json_get_size(json, "planner_hz", &fixture->planner_hz) ||
        !json_get_u64(json, "deadline_ns", &fixture->deadline_ns) ||
        !json_get_size(json, "expected_passes", &fixture->expected_passes) ||
        !json_get_size(json, "expected_ordinary_ahead_after_preempt",
                       &fixture->expected_ordinary_ahead)) {
        return false;
    }
    fixture->ordinary_source_count =
        json_array_string_count(json, "ordinary_sources");
    fixture->ready_source_count =
        json_array_string_count(json, "ready_sources");
    fixture->source_sets_valid =
        fixture->ordinary_source_count == 4 &&
        json_array_contains(json, "ordinary_sources", "fast") &&
        json_array_contains(json, "ordinary_sources", "slow") &&
        json_array_contains(json, "ordinary_sources", "flood") &&
        json_array_contains(json, "ordinary_sources", "controller") &&
        fixture->ready_source_count == 4 &&
        json_array_contains(json, "ready_sources", "eventfd_estop") &&
        json_array_contains(json, "ready_sources", "controller") &&
        json_array_contains(json, "ready_sources", "timerfd") &&
        json_array_contains(json, "ready_sources", "motion");
    return true;
}

static const ControllerFixture *find_controller_fixture(
    const ControllerFixture *fixtures, size_t fixture_count,
    const char *fault) {
    for (size_t index = 0; index < fixture_count; index++) {
        if (strcmp(fixtures[index].fault, fault) == 0) {
            return &fixtures[index];
        }
    }
    return NULL;
}

static const ReleaseFixture *find_release_fixture(
    const ReleaseFixture *fixtures, size_t fixture_count,
    const char *class_name) {
    for (size_t index = 0; index < fixture_count; index++) {
        if (strcmp(fixtures[index].class_name, class_name) == 0) {
            return &fixtures[index];
        }
    }
    return NULL;
}

static bool controller_value_is_valid(const ControllerFixture *fixture) {
    double value;

    return strcmp(fixture->value, "finite") == 0 ||
           strcmp(fixture->value, "nan") == 0 ||
           parse_double(fixture->value, &value);
}

static bool controller_fixture_contract_ok(
    const ControllerFixture *fixtures, size_t fixture_count,
    uint64_t *active_generation) {
    static const char *const faults[] = {
        "happy", "late_generation", "stall",
        "nan",   "range",           "low_confidence",
    };
    const ControllerFixture *happy;
    const ControllerFixture *late;
    const ControllerFixture *stall;
    const ControllerFixture *nan_fault;
    const ControllerFixture *range;
    const ControllerFixture *low;

    if (fixture_count != sizeof(faults) / sizeof(faults[0])) {
        return false;
    }
    for (size_t index = 0; index < sizeof(faults) / sizeof(faults[0]);
         index++) {
        if (find_controller_fixture(fixtures, fixture_count, faults[index]) ==
            NULL) {
            return false;
        }
    }
    for (size_t index = 0; index < fixture_count; index++) {
        if (!controller_value_is_valid(&fixtures[index])) {
            return false;
        }
    }
    happy = find_controller_fixture(fixtures, fixture_count, "happy");
    late = find_controller_fixture(fixtures, fixture_count, "late_generation");
    stall = find_controller_fixture(fixtures, fixture_count, "stall");
    nan_fault = find_controller_fixture(fixtures, fixture_count, "nan");
    range = find_controller_fixture(fixtures, fixture_count, "range");
    low = find_controller_fixture(fixtures, fixture_count, "low_confidence");
    *active_generation = happy->generation;
    return happy->expected_action == ACTION_EXPECT_USE &&
           happy->expected_reason == COURSE_REASON_NONE &&
           happy->produced_age_ns <= COURSE_CONTROLLER_TIMEOUT_NS &&
           happy->confidence >= COURSE_MIN_CONTROLLER_CONFIDENCE &&
           late->generation != *active_generation &&
           late->expected_action == ACTION_EXPECT_DISCARD &&
           late->expected_reason == COURSE_REASON_CANCELLED &&
           stall->produced_age_ns > COURSE_CONTROLLER_TIMEOUT_NS &&
           stall->expected_action == ACTION_EXPECT_FALLBACK_OR_DISCARD &&
           stall->expected_reason == COURSE_REASON_CONTROLLER_TIMEOUT &&
           strcmp(nan_fault->value, "nan") == 0 &&
           nan_fault->expected_action == ACTION_EXPECT_FALLBACK_OR_DISCARD &&
           nan_fault->expected_reason == COURSE_REASON_CONTROLLER_INVALID &&
           range->expected_action == ACTION_EXPECT_FALLBACK_OR_DISCARD &&
           range->expected_reason == COURSE_REASON_CONTROLLER_INVALID &&
           low->confidence < COURSE_MIN_CONTROLLER_CONFIDENCE &&
           low->expected_action == ACTION_EXPECT_FALLBACK_OR_DISCARD &&
           low->expected_reason == COURSE_REASON_LOW_CONFIDENCE;
}

static bool release_fixture_contract_ok(const ReleaseFixture *fixtures,
                                        size_t fixture_count) {
    static const char *const classes[] = {
        "happy",   "corrupt", "stale", "unsafe",
        "reorder", "overload", "controller_fault",
    };

    if (fixture_count != sizeof(classes) / sizeof(classes[0])) {
        return false;
    }
    for (size_t index = 0; index < sizeof(classes) / sizeof(classes[0]);
         index++) {
        if (find_release_fixture(fixtures, fixture_count, classes[index]) ==
            NULL) {
            return false;
        }
    }
    return true;
}

static bool estop_fixture_contract_ok(const EstopFixture *fixture) {
    return strcmp(fixture->fixture_id, "E_STOP_100") == 0 &&
           fixture->seed == COURSE_FIXED_SEED &&
           fixture->repetitions == 100 && fixture->planner_hz == 50 &&
           fixture->source_sets_valid &&
           fixture->deadline_ns == COURSE_ESTOP_DEADLINE_NS &&
           fixture->expected_passes == fixture->repetitions &&
           fixture->expected_ordinary_ahead == 0 &&
           fixture->repetitions <= DAY5_MAX_ESTOP_RUNS;
}

static ArmState make_state(uint64_t now_ns) {
    ArmState state;

    memset(&state, 0, sizeof(state));
    state.seq = 501;
    state.t_mono_ns = (int64_t)(now_ns - UINT64_C(10000000));
    state.frame_id = COURSE_FRAME_ID_BASE;
    for (size_t joint = 0; joint < COURSE_ARM_DOF; joint++) {
        state.sigma_q_rad[joint] = 0.01f;
    }
    return state;
}

static ArmCommand make_command(uint64_t now_ns, uint64_t sequence) {
    ArmCommand command;

    memset(&command, 0, sizeof(command));
    command.seq = sequence;
    command.t_source_ns = now_ns - UINT64_C(5000000);
    command.trace_id = UINT64_C(5000) + sequence;
    command.q_target_rad[0] = 0.01;
    command.q_target_rad[1] = -0.01;
    command.q_target_rad[2] = 0.005;
    return command;
}

static bool add_epoll_fd(int epoll_fd, int fd) {
    struct epoll_event event;

    memset(&event, 0, sizeof(event));
    event.events = EPOLLIN;
    event.data.fd = fd;
    return epoll_ctl(epoll_fd, EPOLL_CTL_ADD, fd, &event) == 0;
}

static bool read_counter_value(int fd, uint64_t *value) {
    ssize_t result;

    do {
        result = read(fd, value, sizeof(*value));
    } while (result < 0 && errno == EINTR);
    return result == (ssize_t)sizeof(*value);
}

static void drain_counter(int fd) {
    uint64_t value;

    while (read_counter_value(fd, &value)) {
    }
}

static bool run_estop_fixture(const EstopFixture *fixture,
                              EstopObservation *observations,
                              size_t *passed_count) {
    int epoll_fd = -1;
    int wake_fd = -1;
    int controller_fd = -1;
    int timer_fd = -1;
    int motion_fd = -1;
    const uint64_t one = 1;
    bool all_passed = true;

    *passed_count = 0;
    epoll_fd = epoll_create1(EPOLL_CLOEXEC);
    wake_fd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    controller_fd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    timer_fd = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC | TFD_NONBLOCK);
    motion_fd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    if (epoll_fd < 0 || wake_fd < 0 || controller_fd < 0 || timer_fd < 0 ||
        motion_fd < 0 || !add_epoll_fd(epoll_fd, wake_fd) ||
        !add_epoll_fd(epoll_fd, controller_fd) ||
        !add_epoll_fd(epoll_fd, timer_fd) ||
        !add_epoll_fd(epoll_fd, motion_fd)) {
        all_passed = false;
        goto cleanup;
    }

    for (size_t index = 0; index < fixture->repetitions; index++) {
        struct itimerspec timer;
        ReadyEvent event;
        const uint64_t load_per_source =
            (uint64_t)(fixture->planner_hz / 5 + 1 + index % 3);
        const uint64_t controller_units = load_per_source;
        const uint64_t motion_units =
            load_per_source * (uint64_t)(fixture->ordinary_source_count - 1);
        uint64_t controller_backlog = 0;
        uint64_t motion_backlog = 0;
        uint64_t end_ns;
        bool counters_observed;
        bool timestamp_valid;
        int ready;

        drain_counter(wake_fd);
        drain_counter(controller_fd);
        drain_counter(timer_fd);
        drain_counter(motion_fd);
        memset(&timer, 0, sizeof(timer));
        timer.it_value.tv_nsec = 1;
        memset(&event, 0, sizeof(event));
        observations[index].run = index + 1;
        observations[index].ordinary_enqueued =
            (size_t)(controller_units + motion_units);

        if (write(controller_fd, &controller_units,
                  sizeof(controller_units)) !=
                (ssize_t)sizeof(controller_units) ||
            write(motion_fd, &motion_units, sizeof(motion_units)) !=
                (ssize_t)sizeof(motion_units) ||
            timerfd_settime(timer_fd, 0, &timer, NULL) != 0) {
            all_passed = false;
            break;
        }
        observations[index].trigger_ns = monotonic_ns();
        if (write(wake_fd, &one, sizeof(one)) != (ssize_t)sizeof(one)) {
            all_passed = false;
            break;
        }

        ready = epoll_wait_priority(
            epoll_fd, wake_fd, controller_fd, timer_fd, motion_fd, 20,
            fixture->seed + index, &event);
        end_ns = monotonic_ns();
        timestamp_valid =
            event.observed_ns >= observations[index].trigger_ns &&
            event.observed_ns <= end_ns;
        observations[index].observed_ns = event.observed_ns;
        observations[index].latency_ns =
            timestamp_valid
                ? event.observed_ns - observations[index].trigger_ns
                : (end_ns >= observations[index].trigger_ns
                       ? end_ns - observations[index].trigger_ns
                       : UINT64_MAX);
        counters_observed =
            read_counter_value(controller_fd, &controller_backlog) &&
            read_counter_value(motion_fd, &motion_backlog);
        observations[index].ordinary_backlog_after =
            counters_observed
                ? (size_t)(controller_backlog + motion_backlog)
                : 0;
        observations[index].ordinary_ahead =
            event.kind == COURSE_READY_ESTOP ? 0 : 1;
        observations[index].passed =
            ready > 0 && event.kind == COURSE_READY_ESTOP &&
            event.cancel_generation == fixture->seed + index &&
            timestamp_valid &&
            observations[index].latency_ns <= fixture->deadline_ns &&
            observations[index].ordinary_enqueued > 0 && counters_observed &&
            observations[index].ordinary_backlog_after ==
                observations[index].ordinary_enqueued &&
            observations[index].ordinary_ahead ==
                fixture->expected_ordinary_ahead;
        if (observations[index].passed) {
            (*passed_count)++;
        } else {
            all_passed = false;
        }

        drain_counter(wake_fd);
        drain_counter(timer_fd);
    }

cleanup:
    if (wake_fd >= 0) {
        close(wake_fd);
    }
    if (controller_fd >= 0) {
        close(controller_fd);
    }
    if (timer_fd >= 0) {
        close(timer_fd);
    }
    if (motion_fd >= 0) {
        close(motion_fd);
    }
    if (epoll_fd >= 0) {
        close(epoll_fd);
    }
    return all_passed && *passed_count == fixture->expected_passes;
}

static ControllerObservation run_controller_case(
    const ControllerFixture *fixture, uint64_t active_generation,
    uint64_t now_ns, const ArmState *state, uint64_t sequence) {
    ControllerObservation observation;
    ControllerResult result;
    ArmCommand output;
    Reason reason = COURSE_REASON_INTERNAL;
    double numeric_value = 0.01;
    bool value_ok = true;

    memset(&observation, 0, sizeof(observation));
    memset(&result, 0, sizeof(result));
    memset(&output, 0, sizeof(output));
    observation.fixture = fixture;
    result.command = make_command(now_ns, sequence);
    result.command.generation = fixture->generation;
    result.generation = fixture->generation;
    result.produced_ns = fixture->produced_age_ns < now_ns
                             ? now_ns - fixture->produced_age_ns
                             : 0;
    result.confidence = fixture->confidence;
    result.timed_out = strcmp(fixture->fault, "stall") == 0;
    if (strcmp(fixture->value, "nan") == 0) {
        result.command.q_target_rad[0] = NAN;
    } else if (strcmp(fixture->value, "finite") != 0) {
        value_ok = parse_double(fixture->value, &numeric_value);
        result.command.q_target_rad[0] = numeric_value;
    }

    observation.action = controller_sanitize(
        &result, active_generation, now_ns, state, &output, &reason);
    observation.reason = reason;
    observation.passed =
        value_ok && action_matches(fixture->expected_action,
                                   observation.action) &&
        observation.reason == fixture->expected_reason;
    if (fixture->expected_action == ACTION_EXPECT_USE) {
        observation.passed =
            observation.passed && output.seq == result.command.seq &&
            output.generation == active_generation;
    }
    return observation;
}

static const ControllerObservation *find_controller_observation(
    const ControllerObservation *observations, size_t observation_count,
    const char *fault) {
    for (size_t index = 0; index < observation_count; index++) {
        if (strcmp(observations[index].fixture->fault, fault) == 0) {
            return &observations[index];
        }
    }
    return NULL;
}

static bool controller_fault_release(
    const ReleaseFixture *fixture,
    const ControllerObservation *controllers, size_t controller_count,
    ReleaseObservation *observation) {
    char inputs[DAY5_TEXT_SIZE];
    char *faults[DAY5_MAX_CONTROLLER_FIXTURES];
    size_t fault_count;
    bool any_fallback = false;
    bool all_contained = true;

    if (!copy_text(inputs, sizeof(inputs), fixture->input)) {
        return false;
    }
    fault_count = split_fields(inputs, '|', faults,
                               sizeof(faults) / sizeof(faults[0]));
    if (fault_count == 0 ||
        fault_count > sizeof(faults) / sizeof(faults[0])) {
        return false;
    }
    for (size_t index = 0; index < fault_count; index++) {
        const ControllerObservation *controller =
            find_controller_observation(controllers, controller_count,
                                        faults[index]);

        if (controller == NULL) {
            return false;
        }
        all_contained = all_contained && controller->passed &&
                        (controller->action == COURSE_CONTROLLER_FALLBACK ||
                         controller->action == COURSE_CONTROLLER_DISCARD);
        any_fallback = any_fallback ||
                       controller->action == COURSE_CONTROLLER_FALLBACK;
        if (index == 0) {
            observation->reason = controller->reason;
        }
    }
    observation->verdict = any_fallback ? COURSE_VERDICT_FALLBACK
                                        : COURSE_VERDICT_DISCARD;
    observation->writes = 0;
    return all_contained;
}

static bool parse_input_u64(const char *input, const char *prefix,
                            uint64_t *value) {
    const size_t prefix_length = strlen(prefix);

    return strncmp(input, prefix, prefix_length) == 0 &&
           parse_u64(input + prefix_length, value);
}

static bool parse_input_double(const char *input, const char *prefix,
                               double *value) {
    const size_t prefix_length = strlen(prefix);

    return strncmp(input, prefix, prefix_length) == 0 &&
           parse_double(input + prefix_length, value);
}

static ReleaseObservation run_release_case(
    const ReleaseFixture *fixture, uint64_t now_ns,
    const ControllerObservation *controllers, size_t controller_count) {
    ReleaseObservation observation;
    ArmState state = make_state(now_ns);
    ArmCommand command = make_command(now_ns, 51);
    FreshnessGate freshness;
    ActuatorWriter writer;
    Simulator simulator;
    TraceRow trace;
    uint8_t frame[COURSE_FRAME_V1_LEN];
    size_t written = 0;
    bool input_ok = true;
    bool queue_ready = true;
    bool corrupt_frame = false;

    memset(&observation, 0, sizeof(observation));
    memset(&freshness, 0, sizeof(freshness));
    memset(&trace, 0, sizeof(trace));
    observation.fixture = fixture;
    observation.verdict = COURSE_VERDICT_REJECT;
    observation.reason = COURSE_REASON_STUDENT_TODO;
    observation.trace_id = command.trace_id;
    writer_init(&writer, 55);
    simulator_reset(&simulator);

    if (strcmp(fixture->class_name, "controller_fault") == 0) {
        input_ok = controller_fault_release(fixture, controllers,
                                            controller_count, &observation);
        observation.passed =
            input_ok && verdict_matches(fixture->expected_verdict,
                                        observation.verdict) &&
            observation.writes == fixture->expected_writes;
        return observation;
    }

    if (strcmp(fixture->input, "valid_fresh_safe") == 0) {
        input_ok = true;
    } else if (strcmp(fixture->input, "frame_bit_flip") == 0) {
        corrupt_frame = true;
    } else if (strncmp(fixture->input, "command_age_ns=",
                       strlen("command_age_ns=")) == 0) {
        uint64_t age_ns;

        input_ok = parse_input_u64(fixture->input, "command_age_ns=",
                                   &age_ns);
        command.t_source_ns = age_ns < now_ns ? now_ns - age_ns : 0;
    } else if (strncmp(fixture->input, "q_target_rad_0=",
                       strlen("q_target_rad_0=")) == 0) {
        input_ok = parse_input_double(fixture->input, "q_target_rad_0=",
                                      &command.q_target_rad[0]);
    } else if (strcmp(fixture->input, "sequence_not_new") == 0) {
        freshness.has_last = true;
        freshness.last_seq = command.seq;
    } else if (strcmp(fixture->input, "writer_queue_full") == 0) {
        for (size_t index = 0; index < COURSE_WRITER_QUEUE_CAPACITY; index++) {
            ArmCommand queued = make_command(now_ns, 100 + index);

            queue_ready = queue_ready && actuator_submit(&writer, &queued);
        }
    } else {
        input_ok = false;
    }

    if (!input_ok ||
        !frame_encode(&command, frame, sizeof(frame), &written) ||
        written != COURSE_FRAME_V1_LEN) {
        observation.passed = false;
        return observation;
    }
    if (corrupt_frame) {
        frame[20] ^= 0x01;
    }

    trace.trace_id = command.trace_id;
    trace.seq = command.seq;
    trace.t_pub_ns = now_ns - UINT64_C(3000000);
    trace.t_rx_ns = now_ns - UINT64_C(2000000);
    observation.verdict = gatekeeper_process(
        &freshness, &writer, &state, frame, written, now_ns, &trace);
    observation.reason = trace.reason;
    if (observation.verdict == COURSE_VERDICT_APPROVE) {
        (void)actuator_pump(&writer, &state, now_ns, &simulator);
    }
    observation.writes = simulator.write_count;
    observation.passed =
        queue_ready && verdict_matches(fixture->expected_verdict,
                                       observation.verdict) &&
        observation.writes == fixture->expected_writes;
    if (strcmp(fixture->class_name, "happy") == 0) {
        observation.passed = observation.passed && trace_complete(&trace);
    }
    return observation;
}

static const ReleaseObservation *find_release_observation(
    const ReleaseObservation *observations, size_t observation_count,
    const char *class_name) {
    for (size_t index = 0; index < observation_count; index++) {
        if (strcmp(observations[index].fixture->class_name, class_name) == 0) {
            return &observations[index];
        }
    }
    return NULL;
}

static bool release_passed(const ReleaseObservation *observations,
                           size_t observation_count,
                           const char *class_name) {
    const ReleaseObservation *observation = find_release_observation(
        observations, observation_count, class_name);

    return observation != NULL && observation->passed;
}

static void write_manifest(TestContext *context,
                           const EstopFixture *estop,
                           size_t controller_count,
                           size_t release_count) {
    const char *image = getenv("COURSE_CONTAINER_IMAGE");
    const char *commit = getenv("COURSE_COMMIT");
    FILE *file = test_open_evidence(context, "manifest.json");

    if (file == NULL) {
        test_check(context, false, "evidence manifest is writable");
        return;
    }
    fprintf(file,
            "{\n  \"group\": \"%s\",\n"
            "  \"day\": 5,\n"
            "  \"input_tag\": \"safety_v1\",\n"
            "  \"intermediate_tag\": \"runtime_async_v1\",\n"
            "  \"output_tag\": \"release_v1\",\n"
            "  \"seed\": %" PRIu64 ",\n"
            "  \"container_image\": \"%s\",\n"
            "  \"compiler\": \"%s\",\n"
            "  \"commit\": \"%s\",\n"
            "  \"planner_hz\": %zu,\n"
            "  \"estop_repetitions\": %zu,\n"
            "  \"estop_deadline_ns\": %" PRIu64 ",\n"
            "  \"controller_fixtures\": %zu,\n"
            "  \"release_fixtures\": %zu,\n"
            "  \"command\": \"make verify-day5\"\n}\n",
            context->group, context->seed,
            image == NULL ? "NOT_RECORDED" : image, __VERSION__,
            commit == NULL ? "NOT_A_GIT_CHECKOUT" : commit,
            estop->planner_hz,
            estop->repetitions, estop->deadline_ns, controller_count,
            release_count);
    fclose(file);
}

static void write_evidence(TestContext *context,
                           const EstopFixture *estop_fixture,
                           const EstopObservation *estops,
                           const ControllerObservation *controllers,
                           size_t controller_count,
                           const ReleaseObservation *releases,
                           size_t release_count, bool replay_ok,
                           bool release_ok) {
    FILE *file = test_open_evidence(context, "raw/estop_100.csv");

    if (file != NULL) {
        fprintf(file,
                "fixture_id,run,trigger_ns,observed_ns,latency_ns,deadline_ns,"
                "ordinary_enqueued,ordinary_backlog_after,ordinary_ahead,"
                "verdict\n");
        for (size_t index = 0; index < estop_fixture->repetitions; index++) {
            fprintf(file,
                    "%s,%zu,%" PRIu64 ",%" PRIu64 ",%" PRIu64
                    ",%" PRIu64 ",%zu,%zu,%zu,%s\n",
                    estop_fixture->fixture_id, estops[index].run,
                    estops[index].trigger_ns, estops[index].observed_ns,
                    estops[index].latency_ns, estop_fixture->deadline_ns,
                    estops[index].ordinary_enqueued,
                    estops[index].ordinary_backlog_after,
                    estops[index].ordinary_ahead,
                    estops[index].passed ? "PASS" : "DEADLINE_OR_PRIORITY_MISS");
        }
        fclose(file);
    }

    file = test_open_evidence(context, "raw/controller_faults.json");
    if (file != NULL) {
        fprintf(file, "[\n");
        for (size_t index = 0; index < controller_count; index++) {
            fprintf(file,
                    "  {\"fixture_id\":\"%s\",\"fault\":\"%s\","
                    "\"expected_action\":\"%s\",\"actual_action\":\"%s\","
                    "\"expected_reason\":\"%s\",\"actual_reason\":\"%s\","
                    "\"passed\":%s}%s\n",
                    controllers[index].fixture->fixture_id,
                    controllers[index].fixture->fault,
                    controllers[index].fixture->expected_action_text,
                    controller_action_name(controllers[index].action),
                    reason_name(controllers[index].fixture->expected_reason),
                    reason_name(controllers[index].reason),
                    controllers[index].passed ? "true" : "false",
                    index + 1 == controller_count ? "" : ",");
        }
        fprintf(file, "]\n");
        fclose(file);
    }

    file = test_open_evidence(context, "raw/release_traces.log");
    if (file != NULL) {
        fprintf(file,
                "fixture_id,class,input,trace_id,expected_verdict,"
                "actual_verdict,reason,expected_writes,actual_writes,"
                "replay_match,status\n");
        for (size_t index = 0; index < release_count; index++) {
            fprintf(file,
                    "%s,%s,%s,%" PRIu64 ",%s,%s,%s,%zu,%zu,%d,%s\n",
                    releases[index].fixture->fixture_id,
                    releases[index].fixture->class_name,
                    releases[index].fixture->input,
                    releases[index].trace_id,
                    releases[index].fixture->expected_verdict_text,
                    verdict_name(releases[index].verdict),
                    reason_name(releases[index].reason),
                    releases[index].fixture->expected_writes,
                    releases[index].writes, replay_ok,
                    releases[index].passed ? "PASS" : "TODO_NOT_IMPLEMENTED");
        }
        fclose(file);
    }

    file = test_open_evidence(context, "evidence-index.csv");
    if (file != NULL) {
        fprintf(file, "chapter,claim_id,evidence_path,trace_id,linked\n");
        for (size_t chapter = 1; chapter <= 10; chapter++) {
            fprintf(file,
                    "%zu,CH%zu_RELEASE,raw/release_traces.log,50%02zu,%d\n",
                    chapter, chapter, chapter, release_ok && replay_ok);
        }
        fclose(file);
    }
    write_manifest(context, estop_fixture, controller_count, release_count);
}

int verify_day5(TestContext *context) {
    ControllerFixture controller_fixtures[DAY5_MAX_CONTROLLER_FIXTURES];
    ReleaseFixture release_fixtures[DAY5_MAX_RELEASE_FIXTURES];
    EstopFixture estop_fixture;
    ControllerObservation controllers[DAY5_MAX_CONTROLLER_FIXTURES];
    ReleaseObservation releases[DAY5_MAX_RELEASE_FIXTURES];
    EstopObservation estops[DAY5_MAX_ESTOP_RUNS];
    size_t controller_count = 0;
    size_t release_count = 0;
    size_t estop_passed = 0;
    uint64_t active_generation = 0;
    const bool controllers_loaded = load_controller_fixtures(
        controller_fixtures,
        sizeof(controller_fixtures) / sizeof(controller_fixtures[0]),
        &controller_count);
    const bool releases_loaded = load_release_fixtures(
        release_fixtures,
        sizeof(release_fixtures) / sizeof(release_fixtures[0]),
        &release_count);
    const bool estop_loaded = load_estop_fixture(&estop_fixture);
    bool fixture_contract;
    uint64_t now_ns;
    ArmState state;
    TraceRow recorded = {
        .trace_id = 5999,
        .seq = 99,
        .t_pub_ns = 10,
        .t_rx_ns = 20,
        .t_gate_ns = 30,
        .t_ack_ns = 40,
        .verdict = COURSE_VERDICT_APPROVE,
        .reason = COURSE_REASON_NONE,
    };
    TraceRow replayed = recorded;
    const ControllerObservation *happy_controller;
    const ControllerObservation *late_controller;
    bool estop_ok;
    bool controller_ok = true;
    bool release_ok = true;
    bool replay_ok;

    fixture_contract =
        controllers_loaded && releases_loaded && estop_loaded &&
        controller_fixture_contract_ok(controller_fixtures, controller_count,
                                       &active_generation) &&
        release_fixture_contract_ok(release_fixtures, release_count) &&
        estop_fixture_contract_ok(&estop_fixture);
    test_check(context, fixture_contract,
               "Day5 e-stop, controller, and release fixtures match the contract");
    if (!fixture_contract) {
        return test_finish(context);
    }
    test_check(context, context->seed == estop_fixture.seed,
               "Day5 execution seed matches the fixed e-stop fixture");

    memset(controllers, 0, sizeof(controllers));
    memset(releases, 0, sizeof(releases));
    memset(estops, 0, sizeof(estops));
    now_ns = monotonic_ns();
    state = make_state(now_ns);
    for (size_t index = 0; index < controller_count; index++) {
        controllers[index] = run_controller_case(
            &controller_fixtures[index], active_generation, now_ns, &state,
            61 + index);
        controller_ok = controller_ok && controllers[index].passed;
    }
    estop_ok = run_estop_fixture(&estop_fixture, estops, &estop_passed);
    for (size_t index = 0; index < release_count; index++) {
        releases[index] = run_release_case(
            &release_fixtures[index], now_ns, controllers, controller_count);
        release_ok = release_ok && releases[index].passed;
    }
    replay_ok = trace_replay_matches(&recorded, &replayed);
    replayed.reason = COURSE_REASON_INTERNAL;
    happy_controller = find_controller_observation(controllers,
                                                   controller_count, "happy");
    late_controller = find_controller_observation(
        controllers, controller_count, "late_generation");

    if (test_group_enabled(context, "G1")) {
        ArmState replay_previous = state;
        ArmState replay_current = state;
        ArmState replay_regressed = state;

        replay_previous.seq--;
        replay_previous.t_mono_ns -= COURSE_FIXED_STEP_NS;
        replay_regressed.seq = replay_previous.seq;
        replay_regressed.t_mono_ns = replay_previous.t_mono_ns - 1;
        check_or_todo(context,
                      state_valid(&replay_current, &replay_previous) &&
                          !state_valid(&replay_regressed,
                                       &replay_previous),
                      "G1 owned state API rejects controller-host replay regression");
        check_or_todo(context,
                      happy_controller != NULL && happy_controller->passed,
                      "G1 fresh state survives the controller host");
    }
    if (test_group_enabled(context, "G2")) {
        FreshnessGate emergency_gate;
        ArmCommand emergency_command = make_command(now_ns, 52);
        ArmCommand emergency_accepted;
        TraceRow emergency_trace;
        uint8_t emergency_frame[COURSE_FRAME_V1_LEN];
        size_t emergency_length = 0;
        bool emergency_encoded;
        Reason emergency_reason;

        memset(&emergency_gate, 0, sizeof(emergency_gate));
        memset(&emergency_accepted, 0, sizeof(emergency_accepted));
        memset(&emergency_trace, 0, sizeof(emergency_trace));
        memset(emergency_frame, 0, sizeof(emergency_frame));
        emergency_command.emergency = true;
        emergency_encoded = frame_encode(
            &emergency_command, emergency_frame, sizeof(emergency_frame),
            &emergency_length);
        emergency_reason = transport_on_message(
            &emergency_gate, emergency_frame, emergency_length, now_ns,
            &emergency_trace, &emergency_accepted);
        check_or_todo(context,
                      emergency_encoded &&
                          emergency_reason != COURSE_REASON_STUDENT_TODO &&
                          emergency_reason == COURSE_REASON_NONE &&
                          emergency_accepted.emergency &&
                          emergency_accepted.t_source_ns ==
                              emergency_command.t_source_ns &&
                          emergency_trace.t_pub_ns ==
                              emergency_command.t_source_ns,
                      "G2 owned transport API preserves emergency type and source age");
        check_or_todo(context,
                      release_passed(releases, release_count, "corrupt") &&
                          release_passed(releases, release_count, "stale") &&
                          release_passed(releases, release_count, "reorder"),
                      "G2 corrupt stale and reordered fixture inputs never write");
    }
    if (test_group_enabled(context, "G3")) {
        check_or_todo(context,
                      estop_ok &&
                          estop_passed == estop_fixture.expected_passes,
                      "G3 owned runtime API makes all fixture e-stops preempt ordinary "
                      "backlog within the deadline");
    }
    if (test_group_enabled(context, "G4")) {
        ActuatorWriter unowned_writer;
        ActuatorWriter owned_writer;
        ArmCommand admission_command = make_command(now_ns, 54);
        bool unowned_admitted;
        bool owned_admitted;

        writer_init(&unowned_writer, 0);
        writer_init(&owned_writer, 54);
        unowned_admitted = actuator_submit(&unowned_writer,
                                           &admission_command);
        owned_admitted = actuator_submit(&owned_writer,
                                         &admission_command);
        check_or_todo(context,
                      !unowned_admitted && unowned_writer.count == 0 &&
                          owned_admitted && owned_writer.count == 1,
                      "G4 owned writer API rejects identity zero and admits the sole writer");
        check_or_todo(context,
                      release_passed(releases, release_count, "unsafe") &&
                          late_controller != NULL && late_controller->passed,
                      "G4 unsafe and cancelled work cannot bypass safety");
    }
    if (test_group_enabled(context, "G5")) {
        check_or_todo(context, controller_ok,
                      "G5 owned controller API contains fixture faults deterministically");
        check_or_todo(context, release_ok,
                      "G5 every release fixture agrees with observed behavior");
        check_or_todo(context,
                      replay_ok &&
                          !trace_replay_matches(&recorded, &replayed),
                      "G5 trace replay matches exact verdict and rejects drift");
    }

    write_evidence(context, &estop_fixture, estops, controllers,
                   controller_count, releases, release_count, replay_ok,
                   release_ok && controller_ok && estop_ok);
    return test_finish(context);
}
