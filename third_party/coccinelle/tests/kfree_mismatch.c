 /* Excerpt taken from linux kernel (linux/net/netfilter/nf_conntrack_core.c) */

__cacheline_aligned_in_smp DEFINE_SPINLOCK(nf_conntrack_expect_lock);
static DEFINE_SPINLOCK(nf_conntrack_locks_all_lock);