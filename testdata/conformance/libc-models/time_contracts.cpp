/* <time.h> models (src/prism/pir/models/libc/time.cpp). time's value is
 * unknown. mktime of month 99 is not a successful calendar time. */
#include "harness.h"
#include "../../../src/prism/pir/models/libc/time.cpp"

/* A null argument is allowed and stores nothing. */
int time_null_true(void) {
    time_t v = time(0);
    (void)v;
    return 0;
}

/* A non-null argument receives the same value the function returns. */
int time_store_true(void) {
    time_t slot = 0;
    time_t v = time(&slot);
    assert(slot == v);
    return 0;
}

/* Wrong contract: month 99 is not a successful calendar time of 0. */
int mktime_month99_false(void) {
    struct tm t;
    t.tm_mon = 99;
    t.tm_mday = 1;
    assert(mktime(&t) == 0);
    return 0;
}
