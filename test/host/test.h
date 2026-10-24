#pragma once

/* Minimal test harness. Every test runs in its own forked process: module state (static
 * variables) starts fresh, and a crash fails only that test. */

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

typedef struct {
    const char *name;
    void (*fn)(void);
} test_t;

#define TEST(fn) { #fn, fn }

#define CHECK(cond) do { \
    if (!(cond)) { \
        fprintf(stderr, "    %s:%d: CHECK(%s) failed\n", __FILE__, __LINE__, #cond); \
        exit(1); \
    } \
} while (0)

#define CHECK_EQ_INT(actual, expected) do { \
    long long a_ = (actual), e_ = (expected); \
    if (a_ != e_) { \
        fprintf(stderr, "    %s:%d: %s is %lld, expected %lld\n", __FILE__, __LINE__, #actual, a_, e_); \
        exit(1); \
    } \
} while (0)

#define CHECK_EQ_STR(actual, expected) do { \
    const char *a_ = (actual), *e_ = (expected); \
    if (a_ == NULL || strcmp(a_, e_) != 0) { \
        fprintf(stderr, "    %s:%d: %s is \"%s\", expected \"%s\"\n", __FILE__, __LINE__, #actual, \
                a_ ? a_ : "(null)", e_); \
        exit(1); \
    } \
} while (0)

static inline int run_tests(const test_t *tests, size_t count)
{
    int failed = 0;
    for (size_t i = 0; i < count; i++) {
        fflush(stdout);
        pid_t pid = fork();
        if (pid == 0) {
            tests[i].fn();
            exit(0);
        }
        int status = 0;
        waitpid(pid, &status, 0);
        bool ok = WIFEXITED(status) && WEXITSTATUS(status) == 0;
        printf("%s %s\n", ok ? "PASS" : "FAIL", tests[i].name);
        failed += !ok;
    }
    printf("%zu tests, %d failed\n", count, failed);
    return failed ? 1 : 0;
}
