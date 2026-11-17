/* Linux kernel completion signal / wait-queue wake specs (docs/KERNEL_MODELS.md), W1 more.
 *
 * Reference semantics (include/linux/completion.h, kernel/sched/completion.c,
 * include/linux/wait.h, kernel/sched/wait.c; Linux 6.18):
 *   complete(x): marks one wait on the completion as done and wakes a waiter; returns nothing.
 *   wait_for_completion(x): sleeps until the completion is signalled; returns nothing.
 *   __wake_up(wq_head, mode, nr_exclusive, key): wakes waiters of the queue (wake_up(),
 *     wake_up_all(), wake_up_interruptible() expand to it); returns an int the callers do
 *     not rely on: the model returns an arbitrary int.
 * requires: x points to a valid struct completion (its first member, the 4-byte done
 *   counter, is read); wq_head points to a valid struct wait_queue_head (its first member,
 *   the lock, is accessed: one byte is checked).
 * Assumptions introduced (named in the verdict):
 *   - the done counter, the wait-queue contents, the wake-up itself, the sleep and the
 *     sleeping-context rules are not modelled: no effect on data memory
 *   - the result of __wake_up is arbitrary (a superset of the real results)
 */
#include "kernel_model.h"

extern "C" {

/* include/linux/completion.h: extern void complete(struct completion *) (kernel/sched/completion.c) */
void complete(void *x) {
    ASSUME("linux-w1-complete: signals the completion and wakes a waiter (kernel/sched/completion.c, Linux 6.18); the done counter, the wake-up and the wait queue are not modelled, no effect on data memory; x must be a valid struct completion");
    __prism_read_range(x, 4);
}

/* include/linux/completion.h: extern void wait_for_completion(struct completion *) */
void wait_for_completion(void *x) {
    ASSUME("linux-w1-wait_for_completion: sleeps until the completion is signalled (kernel/sched/completion.c, Linux 6.18); the wait, the done counter and the sleeping-context rules are not modelled, no effect on data memory; x must be a valid struct completion");
    __prism_read_range(x, 4);
}

/* include/linux/wait.h: int __wake_up(struct wait_queue_head *wq_head, unsigned int mode, int nr, void *key) */
int __wake_up(void *wq_head, unsigned int mode, int nr, void *key) {
    ASSUME("linux-w1-__wake_up: wakes waiters of wq_head (kernel/sched/wait.c, Linux 6.18); the queue contents and the wake-up are not modelled, no effect on data memory, the result is an arbitrary int (a superset of the real results); wq_head must be a valid struct wait_queue_head");
    (void)mode; (void)nr; (void)key;
    __prism_read_range(wq_head, 1);
    return __VERIFIER_nondet_int();
}

} /* extern "C" */
