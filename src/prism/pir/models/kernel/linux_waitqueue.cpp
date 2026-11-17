/* Linux kernel wait-queue specs (docs/KERNEL_MODELS.md), W1 group g2 (the extern functions only).
 *
 * Reference semantics (kernel/sched/wait.c, include/linux/wait.h; Linux 6.18):
 *   finish_wait(wq_head, wq_entry): sets the current task back to TASK_RUNNING and removes
 *     wq_entry from wq_head when it is queued. No failure path.
 *   prepare_to_wait_event(wq_head, wq_entry, state): queues wq_entry on wq_head (unless it
 *     is already queued) and sets the current task state; returns -ERESTARTSYS (-512) without
 *     queueing when a signal is pending for an interruptible state, else 0.
 * Assumptions introduced (named in the verdict):
 *   - the wait-queue list and spinlock manipulation are not modelled: the functions do not
 *     touch wq_head or wq_entry, so the caller must not rely on their contents afterwards
 *   - the signal-pending failure is an arbitrary choice; it is only possible for the
 *     interruptible states TASK_INTERRUPTIBLE (0x1) and TASK_WAKEKILL (0x100)
 *     (include/linux/sched.h, Linux 6.18)
 * Not specified here (see NEEDS REVIEW in docs/KERNEL_MODELS.md): init_wait_entry,
 *   i2c_get_clientdata, netif_running, ether_addr_copy, nla_put_u32, nla_total_size,
 *   dev_net, dma_alloc_coherent, the netdevice and i2c accessors (static inline in the headers).
 */
#include "kernel_model.h"

extern "C" {

/* kernel/sched/wait.c: void finish_wait(struct wait_queue_head *wq_head, struct wait_queue_entry *wq_entry) */
void finish_wait(void *wq_head, void *wq_entry) {
    ASSUME("linux-g2-finish_wait: sets the current task back to TASK_RUNNING and removes wq_entry from wq_head's list if it is queued (kernel/sched/wait.c, Linux 6.18); the list and spinlock manipulation is not modelled: the function does not touch wq_head or wq_entry, the caller must not rely on their contents afterwards");
    (void)wq_head;
    (void)wq_entry;
}

/* kernel/sched/wait.c: long prepare_to_wait_event(struct wait_queue_head *wq_head, struct wait_queue_entry *wq_entry, int state) */
long prepare_to_wait_event(void *wq_head, void *wq_entry, int state) {
    ASSUME("linux-g2-prepare_to_wait_event: queues wq_entry on wq_head unless already queued and sets the task state; returns -ERESTARTSYS (-512) only for an interruptible state (TASK_INTERRUPTIBLE 0x1 or TASK_WAKEKILL 0x100, include/linux/sched.h, Linux 6.18) with a signal pending, else 0; the list manipulation is not modelled and wq_head / wq_entry are not touched");
    (void)wq_head;
    (void)wq_entry;
    if ((state & (1 | 0x100)) && CHOICE()) return -512;
    return 0;
}

} /* extern "C" */
