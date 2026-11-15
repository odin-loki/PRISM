/* Linux delays, logging, BUG/WARN, list and refcount helper specs (docs/KERNEL_MODELS.md).
 *
 * Reference semantics (Linux 6.18):
 *   delays (include/linux/delay.h, kernel/time/sleep_timeout.c): msleep,
 *     usleep_range_state, __udelay, __const_udelay, __ndelay, __delay: no effect
 *     on memory; msleep_interruptible returns the remaining ms in [0, msecs].
 *   logging (include/linux/printk.h, dev_printk.h, dynamic_debug.h): _printk,
 *     _dev_err/warn/info/notice/crit/alert/emerg, _dev_printk, __dynamic_*_dbg,
 *     netdev_err/warn/info, __warn_printk, warn_slowpath_fmt: no effect on any
 *     memory the analysed function can see; the arguments are evaluated; the int
 *     result of _printk / ___ratelimit is arbitrary. dev_err_probe returns its
 *     `err` argument (drivers/base/core.c).
 *   BUG/WARN: panic() does not return: reaching it is reported (FUNC-CONTRACT,
 *     an unconditional failure of the path). WARN (warn_slowpath_fmt,
 *     __warn_printk) has no effect. On x86 BUG() is inline asm `ud2` followed by
 *     unreachable: the asm is modelled by asm-havoc and the `unreachable` that
 *     follows is the reported failure. ____wrong_branch_error (static-key type
 *     misuse) is reached only in a buggy use: reported.
 *   scnprintf(buf, size, fmt, ...): writes at most size bytes (NUL-terminated, if
 *     size > 0) and returns the length without the NUL, in [0, size-1] (0 when
 *     size is 0). snprintf returns the would-be length (arbitrary, >= 0).
 *     requires: buf is writable for size bytes.
 *   __list_add_valid_or_report / __list_del_entry_valid_or_report (lib/list_debug.c,
 *     CONFIG_DEBUG_LIST): true when the list is intact, false after reporting
 *     corruption: arbitrary, no effect. refcount_warn_saturate: reports, no effect.
 *   ktime_get(): arbitrary 64-bit time.
 * Assumptions introduced (named in the verdict): logging and debug checks have
 * no program-visible effect and their format arguments are not checked; delays
 * pass no time that could change memory (no other thread or device runs); the
 * result of ktime_get, lockdep and list debug checks is arbitrary.
 * Not specified here (needs review): seq_printf (writes a seq_file buffer of
 * unknown layout), sysfs_emit (PAGE_SIZE buffer contract), kfree_const,
 * kmem_cache_alloc (object size is the cache's), netdev_priv (inline).
 */
#include "kernel_model.h"

extern "C" {

#define DELAY_NOTE "linux-delay: delays have no effect on memory: no other thread, interrupt or device runs while the function sleeps"
#define LOG_NOTE "linux-log: logging has no effect on memory the function can see; its format arguments are not checked"

void msleep(unsigned int msecs) { (void)msecs; ASSUME(DELAY_NOTE); }
unsigned long msleep_interruptible(unsigned int msecs) {
    ASSUME(DELAY_NOTE);
    unsigned long left = __VERIFIER_nondet_ulong();
    __prism_assume(left <= msecs);
    return left;
}
void usleep_range_state(unsigned long min, unsigned long max, unsigned int state) {
    (void)min; (void)max; (void)state;
    ASSUME(DELAY_NOTE);
}
void __udelay(unsigned long usecs) { (void)usecs; ASSUME(DELAY_NOTE); }
void __const_udelay(unsigned long xloops) { (void)xloops; ASSUME(DELAY_NOTE); }
void __ndelay(unsigned long nsecs) { (void)nsecs; ASSUME(DELAY_NOTE); }
void __delay(unsigned long loops) { (void)loops; ASSUME(DELAY_NOTE); }
/* timekeeping.h: ktime_t ktime_get(void) */
long long ktime_get(void) { ASSUME("linux-time: ktime_get returns an arbitrary 64-bit time"); return __VERIFIER_nondet_long(); }

int _printk(const char *fmt, ...) { (void)fmt; ASSUME(LOG_NOTE); return __VERIFIER_nondet_int(); }
void _dev_err(const void *dev, const char *fmt, ...) { (void)dev; (void)fmt; ASSUME(LOG_NOTE); }
void _dev_warn(const void *dev, const char *fmt, ...) { (void)dev; (void)fmt; ASSUME(LOG_NOTE); }
void _dev_info(const void *dev, const char *fmt, ...) { (void)dev; (void)fmt; ASSUME(LOG_NOTE); }
void _dev_notice(const void *dev, const char *fmt, ...) { (void)dev; (void)fmt; ASSUME(LOG_NOTE); }
void _dev_crit(const void *dev, const char *fmt, ...) { (void)dev; (void)fmt; ASSUME(LOG_NOTE); }
void _dev_alert(const void *dev, const char *fmt, ...) { (void)dev; (void)fmt; ASSUME(LOG_NOTE); }
void _dev_emerg(const void *dev, const char *fmt, ...) { (void)dev; (void)fmt; ASSUME(LOG_NOTE); }
void _dev_printk(const char *level, const void *dev, const char *fmt, ...) { (void)level; (void)dev; (void)fmt; ASSUME(LOG_NOTE); }
void __dynamic_dev_dbg(void *desc, const void *dev, const char *fmt, ...) { (void)desc; (void)dev; (void)fmt; ASSUME(LOG_NOTE); }
void __dynamic_netdev_dbg(void *desc, const void *dev, const char *fmt, ...) { (void)desc; (void)dev; (void)fmt; ASSUME(LOG_NOTE); }
void __dynamic_pr_debug(void *desc, const char *fmt, ...) { (void)desc; (void)fmt; ASSUME(LOG_NOTE); }
void netdev_err(const void *dev, const char *fmt, ...) { (void)dev; (void)fmt; ASSUME(LOG_NOTE); }
void netdev_warn(const void *dev, const char *fmt, ...) { (void)dev; (void)fmt; ASSUME(LOG_NOTE); }
void netdev_info(const void *dev, const char *fmt, ...) { (void)dev; (void)fmt; ASSUME(LOG_NOTE); }
void __warn_printk(const char *fmt, ...) { (void)fmt; ASSUME("linux-warn: WARN has no effect on memory (execution continues)"); }
void warn_slowpath_fmt(const char *file, const int line, unsigned int taint, const char *fmt, ...) {
    (void)file; (void)line; (void)taint; (void)fmt;
    ASSUME("linux-warn: WARN has no effect on memory (execution continues)");
}
int ___ratelimit(void *rs, const char *func) { (void)rs; (void)func; ASSUME(LOG_NOTE); return CHOICE() ? 1 : 0; }
/* drivers/base/core.c: int dev_err_probe(const struct device *dev, int err, const char *fmt, ...): returns err */
int dev_err_probe(const void *dev, int err, const char *fmt, ...) { (void)dev; (void)fmt; ASSUME(LOG_NOTE); return err; }

/* panic.h: void panic(const char *fmt, ...) __noreturn: reaching it is a failure of the path */
void panic(const char *fmt, ...) {
    (void)fmt;
    ASSUME("linux-bug: panic() and static-key misuse do not return: reaching them is reported as FUNC-CONTRACT");
    __prism_check(0, "FUNC-CONTRACT", "panic() is reachable (the kernel does not return from it)");
    __builtin_trap();
}
/* jump_label.h: void ____wrong_branch_error(void): called only when a static key is used with the wrong type */
void ____wrong_branch_error(void) {
    ASSUME("linux-bug: panic() and static-key misuse do not return: reaching them is reported as FUNC-CONTRACT");
    __prism_check(0, "FUNC-CONTRACT", "static key used with the wrong type (____wrong_branch_error is reachable)");
    __builtin_trap();
}

/* sprintf.h: int scnprintf(char *buf, size_t size, const char *fmt, ...)
 * requires: buf is writable for size bytes */
int scnprintf(char *buf, size_t size, const char *fmt, ...) {
    (void)fmt;
    ASSUME("linux-format: the formatted output is arbitrary (a NUL-terminated string within size); the format and its arguments are not checked");
    if (size == 0) return 0;
    __prism_havoc_bytes(buf, size);
    int r = __VERIFIER_nondet_int();
    __prism_assume(r >= 0 && static_cast<size_t>(r) < size);
    buf[r] = 0;
    return r;
}
/* sprintf.h: int snprintf(char *buf, size_t size, const char *fmt, ...): the would-be length; writes <= size bytes
 * requires: buf is writable for size bytes */
int snprintf(char *buf, size_t size, const char *fmt, ...) {
    (void)fmt;
    ASSUME("linux-format: the formatted output is arbitrary (a NUL-terminated string within size); the format and its arguments are not checked");
    if (size) __prism_havoc_bytes(buf, size);
    int r = __VERIFIER_nondet_int();
    __prism_assume(r >= 0);
    if (size) buf[static_cast<size_t>(r) < size ? static_cast<size_t>(r) : size - 1] = 0;
    return r;
}
/* sprintf (unbounded in the kernel): the output length depends on kernel extensions (%pX): not modelled */
int sprintf(char *buf, const char *fmt, ...) {
    (void)buf; (void)fmt;
    __prism_incomplete("kernel sprintf: the output length (with %p extensions) is not modelled");
    return 0;
}

/* list_debug.c: bool __list_add_valid_or_report(struct list_head *new, *prev, *next): true = intact */
bool __list_add_valid_or_report(void *n, void *prev, void *next) {
    (void)n; (void)prev; (void)next;
    ASSUME("linux-list-debug: CONFIG_DEBUG_LIST validation returns an arbitrary result and has no effect");
    return CHOICE();
}
bool __list_del_entry_valid_or_report(void *entry) {
    (void)entry;
    ASSUME("linux-list-debug: CONFIG_DEBUG_LIST validation returns an arbitrary result and has no effect");
    return CHOICE();
}
/* refcount.h: void refcount_warn_saturate(refcount_t *r, enum refcount_saturation_type t): reports, no effect */
void refcount_warn_saturate(void *r, int t) {
    (void)r; (void)t;
    ASSUME("linux-warn: WARN has no effect on memory (execution continues)");
}

}  // extern "C"
