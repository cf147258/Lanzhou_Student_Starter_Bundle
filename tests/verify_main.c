#include "course.h"
#include "test_harness.h"

#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint64_t parse_seed(const char *value) {
    char *end = NULL;
    unsigned long long parsed;

    if (value == NULL || *value == '\0') {
        return COURSE_FIXED_SEED;
    }
    errno = 0;
    parsed = strtoull(value, &end, 10);
    if (errno != 0 || end == value || *end != '\0') {
        fprintf(stderr, "invalid seed: %s\n", value);
        exit(2);
    }
    return (uint64_t)parsed;
}

int main(int argc, char **argv) {
    const char *mode;
    const char *group;
    const char *evidence_dir;
    const char *seed_text;
    TestContext context;

    if (argc < 2 || argc > 3) {
        fprintf(stderr, "usage: %s smoke|day1|day2|day3|day4|day5 [ALL|G1..G5]\n",
                argv[0]);
        return 2;
    }
    mode = argv[1];
    group = argc == 3 ? argv[2] : "ALL";
    evidence_dir = getenv("COURSE_EVIDENCE_DIR");
    seed_text = getenv("COURSE_SEED");
    if (evidence_dir == NULL) {
        evidence_dir = "evidence/local";
    }
    test_context_init(&context, mode, group, evidence_dir,
                          parse_seed(seed_text), stdout);

    if (strcmp(mode, "smoke") == 0) {
        return verify_smoke(&context);
    }
    if (strcmp(mode, "day1") == 0) {
        return verify_day1(&context);
    }
    if (strcmp(mode, "day2") == 0) {
        return verify_day2(&context);
    }
    if (strcmp(mode, "day3") == 0) {
        return verify_day3(&context);
    }
    if (strcmp(mode, "day4") == 0) {
        return verify_day4(&context);
    }
    if (strcmp(mode, "day5") == 0) {
        return verify_day5(&context);
    }
    fprintf(stderr, "unknown verification mode: %s\n", mode);
    return 2;
}
