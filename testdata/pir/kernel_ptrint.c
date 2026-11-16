/* ptrtoint / inttoptr: packed pointer round trips, the opaque integer-to-pointer
   assumption, and the ERR_PTR / IS_ERR / PTR_ERR idiom (linux kernel API). */
static int obj;

/* ptrtoint then inttoptr of the same value is the same pointer: PROVED, no assumption */
int roundtrip_ok(void) {
    int x = 5;
    unsigned long v = (unsigned long)&x;
    int *q = (int *)v;
    return *q;
}

/* an integer computed from a pointer is not a valid object: the dereference fails */
int opaque_bad(void) {
    int x = 1;
    unsigned long v = (unsigned long)&x + 8;
    return *(int *)v;
}

static inline long IS_ERR(const void *p) { return (unsigned long)p >= (unsigned long)-4095L; }
static inline void *ERR_PTR(long e) { return (void *)e; }
static inline long PTR_ERR(const void *p) { return (long)p; }

static int *make_ptr(int fail) {
    if (fail) return (int *)ERR_PTR(-12); /* -ENOMEM */
    return &obj;
}

/* the error branch returns the errno and never dereferences */
int errptr_ok(void) {
    int *p = make_ptr(1);
    if (IS_ERR(p)) return (int)PTR_ERR(p);
    *p = 7;
    return *p;
}

/* the checked success branch dereferences a real object */
int errptr_deref_ok(void) {
    int *p = make_ptr(0);
    if (IS_ERR(p)) return (int)PTR_ERR(p);
    *p = 7;
    return *p;
}

/* the error pointer is dereferenced without the IS_ERR check */
int errptr_bad(void) {
    int *p = make_ptr(1);
    *p = 7;
    return *p;
}
