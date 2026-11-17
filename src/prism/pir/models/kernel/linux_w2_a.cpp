/* Linux kernel string comparison, net_ratelimit and rwsem unlock specs (docs/KERNEL_MODELS.md), W2 group a.
 *
 * Reference semantics (Linux 6.18):
 *   int strcmp(const char *cs, const char *ct) (lib/string.c:278-291): compares the strings
 *     byte by byte as unsigned char up to and including the terminating NUL; returns -1 if
 *     the first differing byte of cs is smaller, 1 if larger, 0 if the strings are equal.
 *     The model runs the same loop, so every byte the real function reads is read.
 *   int strncmp(const char *cs, const char *ct, size_t count) (lib/string.c:302-316): the
 *     same, but stops after count bytes or at the common NUL; reads at most count bytes of each.
 *   int memcmp(const void *cs, const void *ct, size_t count) (lib/string.c:673-697): compares
 *     count bytes; returns 0 if equal, otherwise the difference of the first differing
 *     bytes (the word-at-a-time variant can differ in sign convention and magnitude): the
 *     model returns an arbitrary int on the unequal side (a superset of the real results).
 *   int net_ratelimit(void) (net/core/utils.c:38-41): returns __ratelimit() of a global state:
 *     1 if the message may be printed, 0 if it is suppressed.
 *   void up_read(struct rw_semaphore *), void up_write(struct rw_semaphore *)
 *     (kernel/locking/rwsem.c:1630-1644): release the semaphore; return nothing.
 * Failure paths: none of these fail; a violated precondition is a memory error.
 * requires: strcmp: cs and ct point to NUL-terminated strings (read up to and including the NUL);
 *   strncmp: cs and ct valid for reading up to count bytes (reads stop at a common NUL);
 *   memcmp: cs and ct valid for reading count bytes;
 *   up_read / up_write: sem points to a valid struct rw_semaphore (its first member, the count
 *   word, is accessed: one byte is checked).
 * Assumptions introduced (named in the verdict):
 *   - memcmp result is an arbitrary int (a superset of the real results)
 *   - net_ratelimit result is arbitrary 0 or 1; the global rate-limit state is not modelled
 *   - up_read / up_write: the semaphore count, waiter wake-up and lockdep state are not modelled,
 *     no effect on data memory
 * Not specified (see NEEDS_REVIEW): timer_init_key, skb_put, consume_skb, schedule.
 */
#include "kernel_model.h"

extern "C" {

/* include/linux/string.h: extern int strcmp(const char *, const char *) (lib/string.c:278-291) */
int strcmp(const char *cs, const char *ct) {
    ASSUME("linux-w2-strcmp: compares two NUL-terminated strings as unsigned bytes and returns -1, 0 or 1 exactly as lib/string.c:278-291 (Linux 6.18); both strings must be terminated inside their objects");
    unsigned char c1, c2;
    while (1) {
        c1 = *cs++;
        c2 = *ct++;
        if (c1 != c2)
            return c1 < c2 ? -1 : 1;
        if (!c1)
            break;
    }
    return 0;
}

/* include/linux/string.h: extern int strncmp(const char *, const char *, __kernel_size_t) (lib/string.c:302-316) */
int strncmp(const char *cs, const char *ct, unsigned long count) {
    ASSUME("linux-w2-strncmp: compares at most count bytes of two strings as unsigned bytes and returns -1, 0 or 1 exactly as lib/string.c:302-316 (Linux 6.18); at most count bytes of each are read");
    unsigned char c1, c2;
    while (count) {
        c1 = *cs++;
        c2 = *ct++;
        if (c1 != c2)
            return c1 < c2 ? -1 : 1;
        if (!c1)
            break;
        count--;
    }
    return 0;
}

/* include/linux/string.h: extern int memcmp(const void *, const void *, __kernel_size_t) (lib/string.c:673-697) */
int memcmp(const void *cs, const void *ct, unsigned long count) {
    ASSUME("linux-w2-memcmp: compares count bytes (lib/string.c:673-697, Linux 6.18); both ranges are read, the result is an arbitrary int (a superset of the real results: 0 if equal, else byte difference)");
    __prism_read_range((void *)cs, count);
    __prism_read_range((void *)ct, count);
    return __VERIFIER_nondet_int();
}

/* include/linux/net.h: int net_ratelimit(void) (net/core/utils.c:38-41) */
int net_ratelimit(void) {
    ASSUME("linux-w2-net_ratelimit: returns __ratelimit(&net_ratelimit_state), 1 or 0 (net/core/utils.c:38-41, Linux 6.18); the rate-limit state is not modelled, the result is an arbitrary 0 or 1");
    return CHOICE() ? 1 : 0;
}

/* include/linux/rwsem.h: extern void up_write(struct rw_semaphore *sem) (kernel/locking/rwsem.c:1640-1644) */
void up_write(void *sem) {
    ASSUME("linux-w2-up_write: releases the write lock of the rw_semaphore (kernel/locking/rwsem.c:1640-1644, Linux 6.18); the count, the waiter wake-up and lockdep are not modelled, no effect on data memory; sem must be a valid struct rw_semaphore");
    __prism_read_range(sem, 1);
}

/* include/linux/rwsem.h: extern void up_read(struct rw_semaphore *sem) (kernel/locking/rwsem.c:1630-1634) */
void up_read(void *sem) {
    ASSUME("linux-w2-up_read: releases a read lock of the rw_semaphore (kernel/locking/rwsem.c:1630-1634, Linux 6.18); the count, the waiter wake-up and lockdep are not modelled, no effect on data memory; sem must be a valid struct rw_semaphore");
    __prism_read_range(sem, 1);
}

} /* extern "C" */
