// Minimal stub for LLVM-IR backend M3 body-walk.
// Provides gs_print_i64 for the test-suite smoke check.
#include <stdio.h>
#include <stdint.h>

void gs_print_i64(int64_t val) {
    printf("%lld\n", (long long)val);
}