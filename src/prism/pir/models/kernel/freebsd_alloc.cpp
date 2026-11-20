/* FreeBSD kernel memory allocation specs (docs/KERNEL_MODELS.md).
 *
 * Reference semantics (sys/sys/malloc.h, sys/kern/kern_malloc.c, malloc(9); FreeBSD 15):
 *   malloc(size, type, flags): a fresh object of `size` bytes, zero when flags has
 *   M_ZERO (0x0100), else uninitialised. With M_NOWAIT (0x0001) it may return NULL.
 *   Without M_NOWAIT (M_WAITOK, 0x0002) it never returns NULL: malloc(9) states
 *   "malloc(), mallocarray() and realloc() cannot return NULL if M_WAITOK is
 *   specified" (a request the kernel cannot satisfy panics: such a request, size
 *   >= PRISM_MAX_OBJ, ends the path here).
 *   free(addr, type): NULL is a no-op, else addr must be a live allocation of
 *   this family (double free, free of a non-allocation is reported).
 *   mallocarray(n, size, type, flags): as malloc(n * size); a multiplication
 *   overflow panics (kern_malloc.c): reported as FUNC-CONTRACT.
 * Assumptions introduced (named in the verdict):
 *   - M_WAITOK allocations succeed (malloc(9)); M_NOWAIT ones may fail
 *   - flags other than M_NOWAIT / M_WAITOK / M_ZERO (M_USE_RESERVE, M_NODUMP,
 *     M_EXEC, M_NOVM) have no effect on the result; the malloc_type accounting
 *     limits are not modelled
 *   - memory from malloc(), uma_zalloc and contigmalloc is one allocation kind
 * Not specified here (needs review): realloc, malloc_domainset, malloc_aligned,
 * uma_zalloc_arg (the object size is the zone's), contigmalloc, strdup.
 */
#include "kernel_model.h"

extern "C" {

#define M_NOWAIT 0x0001u
#define M_ZERO 0x0100u

static void *fb_alloc(size_t n, int flags) {
    ASSUME("freebsd-alloc: M_WAITOK allocations succeed (malloc(9)), M_NOWAIT allocations may fail; M_ZERO is 0x0100, M_NOWAIT 0x0001 (sys/malloc.h); other flags and malloc_type limits are not modelled");
    const bool nowait = (static_cast<unsigned int>(flags) & M_NOWAIT) != 0;
    if (nowait) {
        if (n >= PRISM_MAX_OBJ || CHOICE()) return 0;
    } else {
        __prism_assume(n < PRISM_MAX_OBJ); /* larger requests panic, they do not return */
    }
    if (static_cast<unsigned int>(flags) & M_ZERO) return __prism_alloc(n, PRISM_HEAP, PRISM_ZERO);
    return __prism_alloc(n, PRISM_HEAP, PRISM_UNINIT);
}

/* OpenZFS SPL (sys/contrib/openzfs/include/os/freebsd/spl/sys/kmem.h): kmem_alloc(size, kmflags) and kmem_zalloc are
 * zfs_kmem_alloc(size, kmflags [| M_ZERO]), KM_SLEEP is M_WAITOK and KM_NOSLEEP is M_NOWAIT: malloc(9) semantics. */
void *zfs_kmem_alloc(size_t size, int kmflags) {
    ASSUME("freebsd-kmem: zfs_kmem_alloc (kmem_alloc / kmem_zalloc) is malloc(9) with KM_SLEEP = M_WAITOK, KM_NOSLEEP = M_NOWAIT (spl/sys/kmem.h)");
    return fb_alloc(size, kmflags);
}
/* void zfs_kmem_free(void *buf, size_t size) (kmem_free): free(9) of a zfs_kmem_alloc object */
void zfs_kmem_free(void *buf, size_t size) {
    (void)size;
    ASSUME("freebsd-free: free of NULL is a no-op, otherwise the pointer must be a live malloc/uma allocation (one allocation kind)");
    if (buf) __prism_free(buf, PRISM_HEAP);
}
/* void *malloc(size_t size, struct malloc_type *type, int flags) */
void *malloc(size_t size, void *type, int flags) { (void)type; return fb_alloc(size, flags); }
/* void *mallocarray(size_t nmemb, size_t size, struct malloc_type *type, int flags) */
void *mallocarray(size_t nmemb, size_t size, void *type, int flags) {
    (void)type;
    size_t t;
    if (__builtin_mul_overflow(nmemb, size, &t)) {
        __prism_check(0, "FUNC-CONTRACT", "mallocarray: nmemb * size overflows (the kernel panics)");
        __builtin_trap();
    }
    return fb_alloc(t, flags);
}
/* void free(void *addr, struct malloc_type *type); void zfree(...) also clears the memory first */
void free(void *addr, void *type) {
    (void)type;
    ASSUME("freebsd-free: free of NULL is a no-op, otherwise the pointer must be a live malloc/uma allocation (one allocation kind)");
    if (addr) __prism_free(addr, PRISM_HEAP);
}
void zfree(void *addr, void *type) {
    (void)type;
    ASSUME("freebsd-free: free of NULL is a no-op, otherwise the pointer must be a live malloc/uma allocation (one allocation kind)");
    if (addr) __prism_free(addr, PRISM_HEAP);
}

}  // extern "C"
