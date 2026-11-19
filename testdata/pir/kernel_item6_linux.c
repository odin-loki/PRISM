/* PIR tasks: r1 FALSE triage item 6 (Linux): per-lock scopes (--pir-lock-scopes), BPF helper argument types
 * (--pir-bpf-helpers) and the deep global harness (--pir-global-harness-deep). Run with `--kernel-api linux`.
 * `*_ok` functions are PROVED-ASSUMING with the option and the named assumption, `*_bad` functions are FAILED
 * with and without it (tests/cpp/test_pir_triage_item6.cpp). The declarations are the kernel's own. */

struct mutex { long owner; long wait[4]; };
void mutex_lock_nested(struct mutex *lock, unsigned int subclass);
void mutex_unlock(struct mutex *lock);
typedef struct { int raw; } raw_spinlock_t;
void _raw_spin_lock(raw_spinlock_t *lock);
void _raw_spin_unlock(raw_spinlock_t *lock);
void msleep(unsigned int msecs);
void __rcu_read_lock(void);
void __rcu_read_unlock(void);

static struct mutex lock_a, lock_b;
static raw_spinlock_t spin_a;

/* ---- --pir-lock-scopes ---- */

/* releases lock_a, which it never takes: its caller holds it (lock_b is balanced and still checked) */
void rel_a_ok(void) {
    mutex_lock_nested(&lock_b, 0);
    mutex_unlock(&lock_b);
    mutex_unlock(&lock_a);
}
/* the caller holds lock_a once: the second release is a double unlock */
void rel_a_twice_bad(void) {
    mutex_unlock(&lock_a);
    mutex_unlock(&lock_a);
}
/* takes lock_a and returns it held by design (rtnl_lock style); lock_b balanced */
void acq_a_ok(void) {
    mutex_lock_nested(&lock_b, 0);
    mutex_unlock(&lock_b);
    mutex_lock_nested(&lock_a, 0);
}
/* both ends visible: lock_a leaks on one path (a real imbalance) */
int leak_path_bad(int c) {
    mutex_lock_nested(&lock_a, 0);
    if (c) return 1;
    mutex_unlock(&lock_a);
    return 0;
}
/* both ends visible: lock_a is released unheld on one path */
void unheld_path_bad(int c) {
    if (c) mutex_lock_nested(&lock_a, 0);
    mutex_unlock(&lock_a);
}
/* balanced on every path */
void balanced_ok(int c) {
    mutex_lock_nested(&lock_a, 0);
    if (c) {
        mutex_unlock(&lock_a);
        return;
    }
    mutex_unlock(&lock_a);
}
/* the caller holds the spinlock: sleeping before the release is sleeping in atomic context */
void spin_caller_held_sleep_bad(void) {
    msleep(1);
    _raw_spin_unlock(&spin_a);
}
/* leaves an RCU read-side section its caller entered */
void rcu_unlock_only_ok(void) { __rcu_read_unlock(); }
/* enters an RCU read-side section and returns inside it by design */
void rcu_lock_only_ok(void) { __rcu_read_lock(); }
/* the caller's one RCU section is left twice */
void rcu_unlock_twice_bad(void) {
    __rcu_read_unlock();
    __rcu_read_unlock();
}

/* ---- --pir-bpf-helpers ---- */

typedef unsigned long long u64;
typedef unsigned int u32;
struct bpf_func_proto {
    u64 (*func)(u64 r1, u64 r2, u64 r3, u64 r4, u64 r5);
    _Bool gpl_only;
    _Bool pkt_access;
    _Bool might_sleep;
    _Bool allow_fastcall;
    int ret_type;
    union {
        struct { int arg1_type, arg2_type, arg3_type, arg4_type, arg5_type; };
        int arg_type[5];
    };
    union {
        struct { u32 *arg1_btf_id, *arg2_btf_id, *arg3_btf_id, *arg4_btf_id, *arg5_btf_id; };
        u32 *arg_btf_id[5];
        struct { unsigned long arg1_size, arg2_size, arg3_size, arg4_size, arg5_size; };
        unsigned long arg_size[5];
    };
    int *ret_btf_id;
    _Bool (*allowed)(const void *prog);
};
/* enum bpf_arg_type (include/linux/bpf.h, Linux 6.18) */
enum { ARG_PTR_TO_MEM = 4, ARG_CONST_SIZE = 6, ARG_CONST_SIZE_OR_ZERO = 7, ARG_PTR_TO_CTX = 8, ARG_ANYTHING = 9,
       PTR_MAYBE_NULL = 1 << 8 };
struct bctx { int len; int data[4]; };

/* BPF_CALL_2(helper_ctx_ok, struct bctx *, c, u32, n): the context is never NULL */
static u64 ____helper_ctx_ok(struct bctx *c, u32 n) { return (u64)(c->len & 0xff) + (n & 0xff); }
u64 helper_ctx_ok(u64 r1, u64 r2, u64 r3, u64 r4, u64 r5) {
    (void)r3; (void)r4; (void)r5;
    return ____helper_ctx_ok((struct bctx *)(unsigned long)r1, (u32)r2);
}
const struct bpf_func_proto helper_ctx_ok_proto = {
    .func = helper_ctx_ok, .arg1_type = ARG_PTR_TO_CTX, .arg2_type = ARG_ANYTHING,
};
/* the same body, but the proto allows NULL (ARG_PTR_TO_CTX_OR_NULL) */
u64 helper_ctx_null_bad(u64 r1, u64 r2, u64 r3, u64 r4, u64 r5) {
    (void)r3; (void)r4; (void)r5;
    return ____helper_ctx_ok((struct bctx *)(unsigned long)r1, (u32)r2);
}
const struct bpf_func_proto helper_ctx_null_bad_proto = {
    .func = helper_ctx_null_bad, .arg1_type = ARG_PTR_TO_CTX | PTR_MAYBE_NULL, .arg2_type = ARG_ANYTHING,
};
/* a scalar argument (ARG_ANYTHING) used as a pointer: not covered */
u64 helper_scalar_bad(u64 r1, u64 r2, u64 r3, u64 r4, u64 r5) {
    (void)r2; (void)r3; (void)r4; (void)r5;
    return (u64)*(int *)(unsigned long)r1;
}
const struct bpf_func_proto helper_scalar_bad_proto = { .func = helper_scalar_bad, .arg1_type = ARG_ANYTHING };
/* memory and its size: the last byte is in bounds */
u64 helper_mem_ok(u64 r1, u64 r2, u64 r3, u64 r4, u64 r5) {
    (void)r3; (void)r4; (void)r5;
    const unsigned char *p = (const unsigned char *)(unsigned long)r1;
    return r2 ? p[r2 - 1] : 0;
}
const struct bpf_func_proto helper_mem_ok_proto = {
    .func = helper_mem_ok, .arg1_type = ARG_PTR_TO_MEM, .arg2_type = ARG_CONST_SIZE_OR_ZERO,
};
/* one byte past the size the verifier checked */
u64 helper_mem_bad(u64 r1, u64 r2, u64 r3, u64 r4, u64 r5) {
    (void)r3; (void)r4; (void)r5;
    const unsigned char *p = (const unsigned char *)(unsigned long)r1;
    return p[r2];
}
const struct bpf_func_proto helper_mem_bad_proto = {
    .func = helper_mem_bad, .arg1_type = ARG_PTR_TO_MEM, .arg2_type = ARG_CONST_SIZE,
};

/* ---- --pir-global-harness-deep ---- */

struct regs { int ctrl; int status; };
struct gdev { struct regs *regs; int n; };
static struct gdev *the_dev;
/* the_dev (global harness) and the_dev->regs (deep) point to valid objects */
int nest_ok(void) { return the_dev->regs->status & 1; }
/* the field is valid, its value is not: dividing by it is a defect */
int nest_div_bad(void) { return 100 / the_dev->regs->ctrl; }

struct list_head { struct list_head *next, *prev; };
struct item { int key; int weight; struct list_head link; };
static struct list_head items = { &items, &items };
#define item_of(p) ((struct item *)((char *)(p) - __builtin_offsetof(struct item, link)))
/* list_for_each_entry over a static LIST_HEAD */
int list_count_ok(void) {
    int n = 0;
    for (struct list_head *p = items.next; p != &items; p = p->next) {
        if (item_of(p)->key == 7) return n;
        n = n < 100 ? n + 1 : n;
    }
    return n;
}
/* the entry's value is arbitrary: dividing by it is a defect */
int list_div_bad(void) {
    for (struct list_head *p = items.next; p != &items; p = p->next) return 100 / item_of(p)->weight;
    return 0;
}
