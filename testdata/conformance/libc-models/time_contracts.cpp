/* <time.h> models (src/prism/pir/models/libc/time.cpp). time's value is
 * unknown. mktime of month 99 is not a successful calendar time.
 * difftime of a difference that fits in time_t is that difference as a
 * double. A failed assert calls __assert_fail and is FUNC-CONTRACT.
 * This is not a calendar proof and not PROVED and not CLEAN. */
#include "harness.h"
#include <limits.h>
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

/* A difference that fits in time_t is that time_t value as a double.
 * Equal arguments are 0 because the difference is 0, not as a stand-in. */
int difftime_fit_true(void) {
    assert(difftime((time_t)5, (time_t)3) == 2.0);
    assert(difftime((time_t)3, (time_t)5) == -2.0);
    assert(difftime((time_t)4, (time_t)4) == 0.0);
    return 0;
}

/* Wrong contract: a difference that does not fit in time_t is not 0.
 * The assert is FUNC-CONTRACT. */
int difftime_nofit_false(void) {
    assert(difftime((time_t)LONG_MAX, (time_t)LONG_MIN) == 0.0);
    return 0;
}
