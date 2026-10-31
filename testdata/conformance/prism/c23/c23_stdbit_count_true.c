// PRISM conformance task c23/c23_stdbit_count_true.c: expected true (no-div0)
#include <stdbit.h>
int c23_stdbit_count_true(int x) {
    return 100 / (int)(stdc_count_ones((unsigned)x) + 1u);
}
