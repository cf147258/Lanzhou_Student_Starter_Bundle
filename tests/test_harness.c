#define _POSIX_C_SOURCE 200809L

#include "test_harness.h"

#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

void test_context_init(TestContext *context, const char *day,
                           const char *group, const char *evidence_dir,
                           uint64_t seed, FILE *report) {
    memset(context, 0, sizeof(*context));
    context->day = day;
    context->group = group;
    context->evidence_dir = evidence_dir;
    context->seed = seed;
    context->report = report == NULL ? stdout : report;
}

void test_check(TestContext *context, bool condition,
                    const char *label) {
    context->checks++;
    if (condition) {
        fprintf(context->report, "PASS %s.%s %s\n", context->day,
                context->group, label);
        return;
    }
    context->failures++;
    fprintf(context->report, "FAIL %s.%s %s\n", context->day,
            context->group, label);
}

void test_todo(TestContext *context, const char *label) {
    context->checks++;
    context->failures++;
    context->todo_failures++;
    fprintf(context->report, "TODO_NOT_IMPLEMENTED %s.%s %s\n",
            context->day, context->group, label);
}

int test_finish(TestContext *context) {
    fprintf(context->report,
            "SUMMARY %s.%s checks=%zu failures=%zu todo=%zu\n",
            context->day, context->group, context->checks,
            context->failures, context->todo_failures);
    return context->failures == 0 ? 0 : 1;
}

bool test_group_enabled(const TestContext *context,
                            const char *group) {
    return context->group == NULL || strcmp(context->group, "ALL") == 0 ||
           strcmp(context->group, group) == 0;
}

bool test_ensure_dir(const char *path) {
    char buffer[PATH_MAX];
    size_t length;

    if (path == NULL) {
        return false;
    }
    length = strlen(path);
    if (length == 0 || length >= sizeof(buffer)) {
        return false;
    }
    memcpy(buffer, path, length + 1);
    for (char *cursor = buffer + 1; *cursor != '\0'; cursor++) {
        if (*cursor != '/') {
            continue;
        }
        *cursor = '\0';
        if (mkdir(buffer, 0775) != 0 && errno != EEXIST) {
            return false;
        }
        *cursor = '/';
    }
    return mkdir(buffer, 0775) == 0 || errno == EEXIST;
}

FILE *test_open_evidence(const TestContext *context,
                             const char *relative_path) {
    char full_path[PATH_MAX];
    char parent[PATH_MAX];
    char *slash;
    int written;

    if (context->evidence_dir == NULL || relative_path == NULL) {
        return NULL;
    }
    written = snprintf(full_path, sizeof(full_path), "%s/%s",
                       context->evidence_dir, relative_path);
    if (written < 0 || (size_t)written >= sizeof(full_path)) {
        return NULL;
    }
    memcpy(parent, full_path, (size_t)written + 1);
    slash = strrchr(parent, '/');
    if (slash != NULL) {
        *slash = '\0';
        if (!test_ensure_dir(parent)) {
            return NULL;
        }
    }
    return fopen(full_path, "w");
}
