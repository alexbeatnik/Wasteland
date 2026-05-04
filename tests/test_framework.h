#ifndef TEST_FRAMEWORK_H
#define TEST_FRAMEWORK_H

#include <stdio.h>
#include <string.h>

static int _test_passed = 0;
static int _test_failed = 0;

#define ASSERT(expr) do { \
    if (!(expr)) { \
        fprintf(stderr, "  FAIL: %s:%d: %s\n", __FILE__, __LINE__, #expr); \
        _test_failed++; \
        return; \
    } \
} while(0)

#define ASSERT_EQ_INT(expected, actual) do { \
    int _e = (expected), _a = (actual); \
    if (_e != _a) { \
        fprintf(stderr, "  FAIL: %s:%d: expected %d, got %d\n", \
                __FILE__, __LINE__, _e, _a); \
        _test_failed++; \
        return; \
    } \
} while(0)

#define ASSERT_EQ_STR(expected, actual) do { \
    const char *_e = (expected), *_a = (actual); \
    if (strcmp(_e, _a) != 0) { \
        fprintf(stderr, "  FAIL: %s:%d: expected \"%s\", got \"%s\"\n", \
                __FILE__, __LINE__, _e, _a); \
        _test_failed++; \
        return; \
    } \
} while(0)

#define ASSERT_TRUE(expr)  ASSERT((expr) != 0)
#define ASSERT_FALSE(expr) ASSERT((expr) == 0)
#define ASSERT_NULL(ptr)   ASSERT((ptr) == NULL)
#define ASSERT_NOT_NULL(ptr) ASSERT((ptr) != NULL)

#define RUN_TEST(name) do { \
    printf("  %s ... ", #name); \
    int _before = _test_failed; \
    name(); \
    if (_test_failed == _before) { \
        printf("OK\n"); \
        _test_passed++; \
    } \
} while(0)

#define TEST_MAIN(name) \
    int main(int argc, char **argv) { \
        (void)argc; (void)argv; \
        printf("=== Wasteland Tests: %s ===\n\n", #name); \
        int _suites_before = _test_failed; \
        run_##name(); \
        printf("\n-----------------------\n"); \
        printf("Passed: %d  Failed: %d\n", _test_passed, _test_failed); \
        return _test_failed > _suites_before ? 1 : 0; \
    }

#endif
