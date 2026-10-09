#include "test_harness.h"

#include <cstdio>
#if defined(_MSC_VER) && defined(_DEBUG)
#include <crtdbg.h>
#endif

void run_stream_tests();
void run_header_tests();
void run_tri_tests();
void run_decode_tests();
void run_schema_tests();
void run_collision_tests();

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);   // a crash still shows how far we got
#if defined(_MSC_VER) && defined(_DEBUG)
    // Debug-CRT assertions (checked iterators) print instead of opening a dialog.
    _CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_ASSERT, _CRTDBG_FILE_STDERR);
    _CrtSetReportMode(_CRT_ERROR, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_ERROR, _CRTDBG_FILE_STDERR);
#endif
    std::printf("niffer tests\n");
    run_stream_tests();
    run_header_tests();
    run_tri_tests();
    run_decode_tests();
    run_schema_tests();
    run_collision_tests();
    std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
