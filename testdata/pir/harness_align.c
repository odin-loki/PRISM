/* PIR tasks: the alignment of automatic harness objects (--pir-harness,
 * docs/PIR.md "Automatic pointer harness"). A harness object has its type's
 * own alignment, at least 16 bytes; an access that needs more than the type
 * guarantees is still a violation under the harness. */

struct __attribute__((aligned(64))) line {
    long w[8];
};

/* the type is 64-byte aligned: the 64-aligned read is in the harness object's alignment */
long line_first(struct line *l) { return l->w[0]; }

typedef long long128 __attribute__((aligned(128)));

/* the access claims 128-byte alignment that the pointee type does not have (NEEDS-HARNESS) */
long over_aligned(long *p) { return *(long128 *)p; }
