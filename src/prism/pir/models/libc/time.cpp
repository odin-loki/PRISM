/* <time.h> time, mktime, difftime, clock, clock_gettime, gmtime, and localtime
 * (docs/PIR.md "Library models").
 * The calendar value is unknown. An out-of-range month is not rolled into
 * a later year, and no epoch is computed. difftime is not a calendar proof.
 * clock is not a proof of elapsed processor time. clock_gettime is not a
 * proof of the current time. gmtime is not a proof of the calendar time.
 * localtime is not a proof of the local calendar and not a proof of a timezone. */
#include "prism_model.h"

#ifdef _WIN32
/* Windows x64 (UCRT): time_t is 64-bit although long is 32-bit (LLP64), and
 * <time.h> defines time, mktime, difftime, gmtime and localtime inline over
 * the _*64 functions modelled at the end of this file. */
typedef long long time_t;
extern "C" long long __VERIFIER_nondet_longlong(void);
#define PRISM_NONDET_TIME() __VERIFIER_nondet_longlong()
#else
typedef long time_t;
#define PRISM_NONDET_TIME() __VERIFIER_nondet_long()
#endif
typedef long clock_t;
typedef int clockid_t;

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

struct timespec {
    time_t tv_sec;
    long tv_nsec;
};

extern "C" {

/* time is not PROVED and not CLEAN. A null pointer stores nothing. */
time_t time(time_t *t) {
    time_t v = PRISM_NONDET_TIME();
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

/* difftime(time1, time0) is the difference time1 - time0 as a double, only
 * when that integer subtraction fits in time_t. When it does not fit, the
 * path is FUNC-CONTRACT and then __builtin_trap. It does not return a
 * wrapped difference and it does not return 0. The fitting path converts
 * that time_t difference to double. It does not read a tm. This is not a
 * calendar proof. difftime is not PROVED and not CLEAN. */
double difftime(time_t time1, time_t time0) {
    time_t diff = 0;
    if (__builtin_sub_overflow(time1, time0, &diff)) {
        __prism_check(0, "FUNC-CONTRACT", "difftime() difference does not fit in time_t");
        __builtin_trap();
    } else {
        return (double)diff;
    }
}

/* clock() returns a nondet clock_t. It does not return a constant.
 * It does not read a struct tm. It is not a proof of elapsed processor
 * time. clock is not PROVED and not CLEAN. */
clock_t clock(void) {
    clock_t v = __VERIFIER_nondet_long();
    return v;
}

/* A null timespec is PTR-NULL-DEREF and returns -1 before any store.
 * A live timespec gets tv_sec from __VERIFIER_nondet_long and tv_nsec in
 * 0..999999999. That is not a proof of the current time. The 0 return is
 * the success status, not a time. clock_gettime is not PROVED and not CLEAN. */
int clock_gettime(clockid_t clock_id, struct timespec *tp) {
    if (tp == 0) {
        __prism_check(0, "PTR-NULL-DEREF", "clock_gettime null timespec");
        return -1;
    }
    (void)clock_id;
    tp->tv_sec = PRISM_NONDET_TIME();
    long nsec = __VERIFIER_nondet_long();
    __prism_assume(nsec >= 0 && nsec <= 999999999);
    tp->tv_nsec = nsec;
    return 0;
}

/* A null time_t * is PTR-NULL-DEREF and returns 0 before any tm field is
 * written. A live time_t * returns a pointer to static storage. Each field
 * is nondet. tm_mon stays in 0..11 and tm_mday stays in 1..31, the ranges
 * this model already uses for a tm. The time_t value is not read and not
 * converted. This is not a proof of the calendar time.
 * gmtime is not PROVED and not CLEAN. */
struct tm *gmtime(const time_t *timer) {
    if (timer == 0) {
        __prism_check(0, "PTR-NULL-DEREF", "gmtime null time_t");
        return 0;
    }
    static struct tm storage;
    int sec = __VERIFIER_nondet_int();
    int min = __VERIFIER_nondet_int();
    int hour = __VERIFIER_nondet_int();
    int mday = __VERIFIER_nondet_int();
    int mon = __VERIFIER_nondet_int();
    int year = __VERIFIER_nondet_int();
    int wday = __VERIFIER_nondet_int();
    int yday = __VERIFIER_nondet_int();
    int isdst = __VERIFIER_nondet_int();
    __prism_assume(mon >= 0 && mon <= 11);
    __prism_assume(mday >= 1 && mday <= 31);
    storage.tm_sec = sec;
    storage.tm_min = min;
    storage.tm_hour = hour;
    storage.tm_mday = mday;
    storage.tm_mon = mon;
    storage.tm_year = year;
    storage.tm_wday = wday;
    storage.tm_yday = yday;
    storage.tm_isdst = isdst;
    return &storage;
}

/* A null time_t * is PTR-NULL-DEREF "localtime null time_t" and returns 0
 * before any field is written. A live time_t * returns a pointer to
 * static storage. Each field is nondet. The time_t value is not read and
 * not converted.
 * This is not a proof of the local calendar and not a proof of a timezone.
 * localtime is not PROVED and not CLEAN. */
struct tm *localtime(const time_t *timer) {
    if (timer == 0) {
        __prism_check(0, "PTR-NULL-DEREF", "localtime null time_t");
        return 0;
    }
    static struct tm storage;
    int sec = __VERIFIER_nondet_int();
    int min = __VERIFIER_nondet_int();
    int hour = __VERIFIER_nondet_int();
    int mday = __VERIFIER_nondet_int();
    int mon = __VERIFIER_nondet_int();
    int year = __VERIFIER_nondet_int();
    int wday = __VERIFIER_nondet_int();
    int yday = __VERIFIER_nondet_int();
    int isdst = __VERIFIER_nondet_int();
    storage.tm_sec = sec;
    storage.tm_min = min;
    storage.tm_hour = hour;
    storage.tm_mday = mday;
    storage.tm_mon = mon;
    storage.tm_year = year;
    storage.tm_wday = wday;
    storage.tm_yday = yday;
    storage.tm_isdst = isdst;
    return &storage;
}

#ifdef _WIN32
/* The UCRT's inline <time.h> wrappers call these; same models as above. */
time_t _time64(time_t *t) { return time(t); }
time_t _mktime64(struct tm *tp) { return mktime(tp); }
double _difftime64(time_t time1, time_t time0) { return difftime(time1, time0); }
struct tm *_gmtime64(const time_t *timer) { return gmtime(timer); }
struct tm *_localtime64(const time_t *timer) { return localtime(timer); }
#endif

}  // extern "C"
