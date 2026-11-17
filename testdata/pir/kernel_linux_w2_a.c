/* PIR tasks: Linux kernel API specs, W2 group a (docs/KERNEL_MODELS.md). Run with
 * `--kernel-api linux`. The declarations are the kernel's own prototypes; opaque
 * kernel objects are stood for by local char/long objects. */

int strcmp(const char *, const char *);
int strncmp(const char *, const char *, unsigned long);
int memcmp(const void *, const void *, unsigned long);
int net_ratelimit(void);
void up_write(void *sem);
void up_read(void *sem);

int strcmp_ok(void) {
    char a[4] = { 'a', 'b', 'c', 0 }, b[4] = { 'a', 'b', 'd', 0 };
    if (strcmp(a, b) != -1) return -1;
    return strcmp(a, a) == 0 ? 0 : -1;
}
int strcmp_bad(void) { /* neither string is terminated: reads past the object */
    char a[2] = { 'a', 'b' }, b[2] = { 'a', 'b' };
    return strcmp(a, b);
}

int strncmp_ok(void) {
    char a[2] = { 'a', 'b' }, b[2] = { 'a', 'c' }; /* unterminated, but only 1 byte compared */
    return strncmp(a, b, 1) == 0 ? 0 : -1;
}
int strncmp_bad(void) { /* count 4 on 2-byte unterminated equal strings */
    char a[2] = { 'a', 'b' }, b[2] = { 'a', 'b' };
    return strncmp(a, b, 4);
}

int memcmp_ok(void) {
    unsigned char a[4] = { 1, 2, 3, 4 }, b[4] = { 1, 2, 3, 4 };
    int r = memcmp(a, b, 4);
    (void)r; /* any result is a normal return */
    return 0;
}
int memcmp_bad(void) { /* 8 bytes compared on 4-byte objects */
    unsigned char a[4] = { 1, 2, 3, 4 }, b[4] = { 1, 2, 3, 4 };
    return memcmp(a, b, 8);
}

int net_ratelimit_ok(void) {
    int r = net_ratelimit();
    return (r == 0 || r == 1) ? 0 : -1;
}
int net_ratelimit_bad(void) { /* the result may be 0 (suppressed): division by zero */
    return 100 / net_ratelimit();
}

int up_write_ok(void) {
    long sem[4] = { 0, 0, 0, 0 }; /* stands for a struct rw_semaphore */
    up_write(sem);
    return 0;
}
int up_write_bad(void) { /* sem is NULL */
    up_write((void *)0);
    return 0;
}

int up_read_ok(void) {
    long sem[4] = { 0, 0, 0, 0 };
    up_read(sem);
    return 0;
}
int up_read_bad(void) {
    up_read((void *)0);
    return 0;
}
