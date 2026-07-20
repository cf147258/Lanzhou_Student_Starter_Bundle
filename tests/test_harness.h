#ifndef COURSE_TEST_HARNESS_H
#define COURSE_TEST_HARNESS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

typedef struct {
    const char *day;
    const char *group;
    const char *evidence_dir;
    uint64_t seed;
    size_t checks;
    size_t failures;
    size_t todo_failures;
    FILE *report;
} TestContext;

void test_context_init(TestContext *context, const char *day,
                           const char *group, const char *evidence_dir,
                           uint64_t seed, FILE *report);
void test_check(TestContext *context, bool condition,
                    const char *label);
void test_todo(TestContext *context, const char *label);
int test_finish(TestContext *context);
bool test_group_enabled(const TestContext *context,
                            const char *group);
bool test_ensure_dir(const char *path);
FILE *test_open_evidence(const TestContext *context,
                             const char *relative_path);

int verify_day1(TestContext *context);
int verify_day2(TestContext *context);
int verify_day3(TestContext *context);
int verify_day4(TestContext *context);
int verify_day5(TestContext *context);
int verify_smoke(TestContext *context);

#endif
