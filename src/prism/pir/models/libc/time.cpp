/* <time.h> time and mktime (docs/PIR.md "Library models").
 * The calendar value is unknown. An out-of-range month is not rolled into
 * a later year, and no epoch is computed. */
#include "prism_model.h"

typedef long time_t;

struct tm {
    int tm_sec;
    int tm_min;
    int tm_hour;
    int tm_mday;
    int tm_mon;
    int tm_year;
    int tm_wday;
    int tm_yday;
    int tm_isdst;
};

extern "C" {

/* time is not PROVED and not CLEAN. A null pointer stores nothing. */
time_t time(time_t *t) {
    time_t v = __VERIFIER_nondet_long();
    if (t != 0) {
        *t = v;
    }
    return v;
}

/* Month 99 is not normalized into a later year. An out-of-range month or
 * day returns (time_t)-1 and is not a successful calendar time of 0.
 * No epoch is computed. mktime is not PROVED and not CLEAN. */
time_t mktime(struct tm *tp) {
    if (tp == 0) {
        __prism_check(0, "PTR-NULL-DEREF", "mktime() called with a null pointer");
        return (time_t)-1;
    }
    int mon = tp->tm_mon;
    int mday = tp->tm_mday;
    if (mon < 0 || mon > 11 || mday < 1 || mday > 31) {
        __prism_check(0, "FUNC-CONTRACT", "mktime() month or day is outside 0..11 or 1..31");
        return (time_t)-1;
    }
    /* In range: still not an epoch. This path is not a calendar time. */
    __prism_assume(0);
    return (time_t)-1;
}

}  // extern "C"
