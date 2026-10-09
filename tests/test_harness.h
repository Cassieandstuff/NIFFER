#pragma once
// Dependency-free CHECK harness (havok-core tests model). Each test TU exposes a
// run_*() that test_main.cpp calls; CHECK increments global counters and prints
// on failure. No gtest/catch.
#include <cstdio>

inline int g_checks = 0;
inline int g_failures = 0;

#define CHECK(cond)                                                          \
    do {                                                                     \
        ++g_checks;                                                          \
        if (!(cond)) {                                                       \
            ++g_failures;                                                    \
            std::printf("  FAIL %s:%d  CHECK(%s)\n", __FILE__, __LINE__, #cond); \
        }                                                                    \
    } while (0)

#define CHECK_EQ(a, b)                                                       \
    do {                                                                     \
        ++g_checks;                                                          \
        if (!((a) == (b))) {                                                 \
            ++g_failures;                                                    \
            std::printf("  FAIL %s:%d  CHECK_EQ(%s, %s)\n", __FILE__, __LINE__, #a, #b); \
        }                                                                    \
    } while (0)
