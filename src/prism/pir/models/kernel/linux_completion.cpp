/* Linux kernel completion wait spec (docs/KERNEL_MODELS.md), W1 group g3 (the extern function only).
 *
 * Reference semantics (kernel/sched/completion.c, include/linux/completion.h; Linux 6.18):
 *   wait_for_completion_timeout(x, timeout): waits up to timeout jiffies; returns 0 when the
 *   wait timed out, otherwise the remaining time (> 0, at most timeout). The completion is
 *   read and updated by the kernel. No failure path besides the timeout.
 * requires: x points to a valid struct completion (named in the assumption).
 * Assumptions introduced (named in the verdict):
 *   - the wait itself, the sleeping-context rules and the completion's wait-queue update are
 *     not modelled; the result is 0 or an arbitrary value (a superset of the real results, so
 *     callers that check for 0 are explored in both branches)
 * Not specified here (see NEEDS REVIEW in docs/KERNEL_MODELS.md): dma_map_single_attrs,
 *   dma_mapping_error, dma_unmap_single_attrs, nla_data, nla_get_u32, brelse (static inline in
 *   the headers, so a spec would not be linked), dev_kfree_skb_any (struct sk_buff comes from
 *   its own cache, not the kmalloc family), qdisc_dev and params_rate (layout not certain).
 */
#include "kernel_model.h"

extern "C" {

struct completion { unsigned int done; };

/* kernel/sched/completion.c: unsigned long wait_for_completion_timeout(struct completion *x, unsigned long timeout) */
unsigned long wait_for_completion_timeout(struct completion *x, unsigned long timeout) {
    ASSUME("linux-g3-wait_for_completion_timeout: returns 0 when the wait timed out, otherwise the remaining time (> 0, at most timeout); the model returns 0 or an arbitrary value, a superset of the real results; the wait, sleeping-context rules and the wait-queue update are not modelled; x must be a valid struct completion");
    (void)timeout;
    __prism_read_range(x, sizeof(struct completion));
    unsigned long r;
    __prism_havoc_bytes(&r, sizeof(r));
    return CHOICE() ? 0 : r;
}

} /* extern "C" */
