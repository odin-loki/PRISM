/* Linux kernel memory allocation specs (docs/KERNEL_MODELS.md).
 *
 * Reference semantics (include/linux/slab.h, mm/slub.c, mm/util.c,
 * include/linux/vmalloc.h, include/linux/gfp_types.h; Linux 6.18):
 *   allocation of `size` bytes: returns NULL (any call may fail, whatever the
 *   gfp flags: a superset of the real behaviour, so the failure path is always
 *   explored) or a pointer to a fresh object of `size` bytes that is
 *   uninitialised, or all zero when the gfp flags contain __GFP_ZERO.
 *   size 0 returns ZERO_SIZE_PTR in the kernel: modelled as a fresh object of
 *   size 0 (not dereferenceable, free of it is a no-op).
 *   kfree(p) / kvfree(p) / vfree(p): p NULL or ZERO_SIZE_PTR is a no-op, else p
 *   must be a live allocation of this family (double free, free of a stack
 *   object or of an interior pointer is reported).
 * Assumptions introduced (named in the verdict):
 *   - __GFP_ZERO is bit 0x100 (___GFP_ZERO = BIT(8), enum order in
 *     include/linux/gfp_types.h; no config option moves it)
 *   - sleeping-context rules (GFP_KERNEL in atomic context), __GFP_NOFAIL
 *     (the model may still fail), memory cgroup accounting: not modelled
 *   - kmalloc, vmalloc and devm_ memory share one allocation kind: kfree of
 *     vmalloc memory or the reverse is not reported
 *   - devm_*: the memory lives until the device is released; the release is not
 *     modelled in a per-function check, so a missing devm_kfree is no leak
 */
#include "kernel_model.h"

extern "C" {

#define GFP_ZERO 0x100u

static void *lx_alloc(size_t n, unsigned int gfp) {
    ASSUME("linux-alloc: allocation may fail (NULL) for any size and gfp flags; __GFP_ZERO is bit 0x100 (include/linux/gfp_types.h, Linux 6.18); size 0 gives a size-0 object (ZERO_SIZE_PTR); sleeping-context rules and __GFP_NOFAIL are not modelled");
    if (n >= PRISM_MAX_OBJ || CHOICE()) return 0;
    if (gfp & GFP_ZERO) return __prism_alloc(n, PRISM_HEAP, PRISM_ZERO);
    return __prism_alloc(n, PRISM_HEAP, PRISM_UNINIT);
}

static void lx_free(const void *p) {
    ASSUME("linux-free: kfree/kvfree/vfree of NULL or ZERO_SIZE_PTR (a size-0 object) is a no-op, otherwise the pointer must be a live allocation of the kmalloc, vmalloc or devm family (they share one allocation kind)");
    if (!p) return;
    if (__prism_obj_size(p) == 0) return; /* ZERO_SIZE_PTR: a size-0 object */
    __prism_free(const_cast<void *>(p), PRISM_HEAP);
}

/* slab.h: void *__kmalloc_noprof(size_t size, gfp_t flags) */
void *__kmalloc_noprof(size_t size, unsigned int flags) { return lx_alloc(size, flags); }
/* slab.h: void *__kmalloc_large_noprof(size_t size, gfp_t flags) */
void *__kmalloc_large_noprof(size_t size, unsigned int flags) { return lx_alloc(size, flags); }
/* slab.h: void *__kmalloc_cache_noprof(struct kmem_cache *s, gfp_t flags, size_t size) (size: the requested size) */
void *__kmalloc_cache_noprof(void *s, unsigned int flags, size_t size) { (void)s; return lx_alloc(size, flags); }
/* slab.h (CONFIG_SLAB_BUCKETS off): void *__kmalloc_node_noprof(size_t size, gfp_t flags, int node).
 * With buckets on the declaration has a fourth parameter: the call then stops at an arity mismatch. */
void *__kmalloc_node_noprof(size_t size, unsigned int flags, int node) { (void)node; return lx_alloc(size, flags); }
/* slab.h: __kmalloc_node_track_caller_noprof(size_t size, gfp_t flags, int node, unsigned long caller) */
void *__kmalloc_node_track_caller_noprof(size_t size, unsigned int flags, int node, unsigned long caller) {
    (void)node; (void)caller; return lx_alloc(size, flags);
}
/* slab.h (CONFIG_SLAB_BUCKETS off): void *__kvmalloc_node_noprof(size_t size, unsigned long align, gfp_t flags, int node) */
void *__kvmalloc_node_noprof(size_t size, unsigned long align, unsigned int flags, int node) {
    (void)align; (void)node; return lx_alloc(size, flags);
}
/* vmalloc.h */
void *vmalloc_noprof(unsigned long size) { return lx_alloc(size, 0); }
void *vzalloc_noprof(unsigned long size) { return lx_alloc(size, GFP_ZERO); }
void *vmalloc_user_noprof(unsigned long size) { return lx_alloc(size, GFP_ZERO); }
void *vmalloc_node_noprof(unsigned long size, int node) { (void)node; return lx_alloc(size, 0); }
void *vzalloc_node_noprof(unsigned long size, int node) { (void)node; return lx_alloc(size, GFP_ZERO); }
void *__vmalloc_noprof(unsigned long size, unsigned int gfp) { return lx_alloc(size, gfp); }
/* device.h: void *devm_kmalloc(struct device *dev, size_t size, gfp_t gfp); same contract, the device is only named */
void *devm_kmalloc(void *dev, size_t size, unsigned int gfp) {
    ASSUME("linux-devm: the allocation lives until the device is released; the release is not modelled, so a missing devm_kfree is no leak");
    (void)dev;
    return lx_alloc(size, gfp);
}

void kfree(const void *p) { lx_free(p); }
void kfree_sensitive(const void *p) { lx_free(p); }
void kvfree(const void *p) { lx_free(p); }
void vfree(const void *p) { lx_free(p); }
/* device.h: void devm_kfree(struct device *dev, const void *p) */
void devm_kfree(void *dev, const void *p) { (void)dev; lx_free(p); }

/* string.h: void *kmemdup_noprof(const void *src, size_t len, gfp_t gfp): a copy of len bytes of src, or NULL.
 * requires: src is readable for len bytes */
void *kmemdup_noprof(const void *src, size_t len, unsigned int gfp) {
    void *p = lx_alloc(len, gfp);
    if (p) __prism_memcpy(p, src, len, 0);
    return p;
}

/* string.h: char *kstrdup(const char *s, gfp_t gfp): NULL for s == NULL or on failure, else a copy with its NUL.
 * requires: s is NULL or a NUL-terminated string */
char *kstrdup(const char *s, unsigned int gfp) {
    if (!s) return 0;
    size_t n = strlen(s) + 1;
    char *p = static_cast<char *>(lx_alloc(n, gfp));
    if (p) __prism_memcpy(p, s, n, 0);
    return p;
}

/* slab.h: void *krealloc_node_align_noprof(const void *p, size_t new_size, unsigned long align, gfp_t flags, int nid)
 * (mm/slub.c krealloc: new_size 0 frees p and returns ZERO_SIZE_PTR; p NULL allocates; on failure NULL and p stays
 * valid; on success min(old, new) bytes are kept and, with __GFP_ZERO, the new tail is zero). The model always moves. */
void *krealloc_node_align_noprof(const void *p, size_t new_size, unsigned long align, unsigned int flags, int nid) {
    (void)align; (void)nid;
    ASSUME("linux-krealloc: the model always returns a new object (the real function may return the same pointer); the old object is freed on success; with __GFP_ZERO the new tail is zero");
    if (new_size == 0) {
        lx_free(p);
        return __prism_alloc(0, PRISM_HEAP, PRISM_UNINIT);
    }
    if (!p) return lx_alloc(new_size, flags);
    if (new_size >= PRISM_MAX_OBJ || CHOICE()) return 0;
    size_t old = __prism_obj_size(p);
    void *q = (flags & GFP_ZERO) ? __prism_alloc(new_size, PRISM_HEAP, PRISM_ZERO) : __prism_alloc(new_size, PRISM_HEAP, PRISM_UNINIT);
    __prism_memcpy(q, p, old < new_size ? old : new_size, 0);
    lx_free(p);
    return q;
}

/* slab.h: size_t kmalloc_size_roundup(size_t size): the bucket size, at least size */
size_t kmalloc_size_roundup(size_t size) {
    size_t r = __VERIFIER_nondet_ulong();
    __prism_assume(r >= size);
    return r;
}

}  // extern "C"
