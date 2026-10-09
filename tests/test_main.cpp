#include "test_harness.h"

#include <cstdio>

void run_stream_tests();
void run_header_tests();
void run_tri_tests();
void run_decode_tests();

int main() {
    std::printf("niffer tests\n");
    run_stream_tests();
    run_header_tests();
    run_tri_tests();
    run_decode_tests();
    std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
