/* <string.h> operational models (docs/PIR.md "Library models").
 * Contracts are the C standard's; PIR checks them through the memory model. */
#include "prism_model.h"

namespace {

static unsigned char uc(const char *s, size_t i) {
    const unsigned char *p = (const unsigned char *)s;
    return p[i];
}

}  // namespace

extern "C" {

/* requires: s points to a NUL-terminated string (readable up to the NUL) */
size_t strlen(const char *s) {
    __prism_check(s != 0, "STR-NULL-ARG", "strlen() called with unchecked pointer parameter s");
    size_t n = 0;
    while (s[n]) n++;
    return n;
}

/* requires: n bytes of s readable, or a NUL within them */
size_t strnlen(const char *s, size_t n) {
    if (n == 0) return 0;
    __prism_check(s != 0, "STR-NULL-ARG", "strnlen() called with unchecked pointer parameter s");
    size_t i = 0;
    while (i < n && s[i]) i++;
    return i;
}

/* requires: s a string; d writable for strlen(s)+1 bytes; no overlap */
char *strcpy(char *d, const char *s) {
    __prism_check(d != 0, "STR-NULL-ARG", "strcpy() called with unchecked pointer parameter d");
    __prism_check(s != 0, "STR-NULL-ARG", "strcpy() called with unchecked pointer parameter s");
    size_t i = 0;
    while ((d[i] = s[i]) != 0) i++;
    return d;
}

/* requires: d writable for n bytes; s readable up to n bytes or its NUL */
char *strncpy(char *d, const char *s, size_t n) {
    __prism_check(d != 0, "STR-NULL-ARG", "strncpy() called with unchecked pointer parameter d");
    __prism_check(s != 0, "STR-NULL-ARG", "strncpy() called with unchecked pointer parameter s");
    size_t i = 0;
    for (; i < n && s[i]; i++) d[i] = s[i];
    for (; i < n; i++) d[i] = 0;
    return d;
}

/* requires: d and s strings; d writable for strlen(d)+strlen(s)+1 bytes */
char *strcat(char *d, const char *s) {
    __prism_check(d != 0, "STR-NULL-ARG", "strcat() called with unchecked pointer parameter d");
    size_t k = strlen(d);
    size_t i = 0;
    while ((d[k + i] = s[i]) != 0) i++;
    return d;
}

char *strncat(char *d, const char *s, size_t n) {
    __prism_check(d != 0, "STR-NULL-ARG", "strncat() called with unchecked pointer parameter d");
    size_t k = strlen(d);
    size_t i = 0;
    for (; i < n && s[i]; i++) d[k + i] = s[i];
    d[k + i] = 0;
    return d;
}

int strcmp(const char *a, const char *b) {
    __prism_check(a != 0, "STR-NULL-ARG", "strcmp() called with unchecked pointer parameter a");
    __prism_check(b != 0, "STR-NULL-ARG", "strcmp() called with unchecked pointer parameter b");
    size_t i = 0;
    while (uc(a, i) && uc(a, i) == uc(b, i)) ++i;
    {
        const unsigned char ca = uc(a, i);
        const unsigned char cb = uc(b, i);
        if (ca != cb) return ca > cb ? 1 : -1;
    }
    return 0;
}

int strncmp(const char *a, const char *b, size_t n) {
    __prism_check(a != 0, "STR-NULL-ARG", "strncmp() called with unchecked pointer parameter a");
    __prism_check(b != 0, "STR-NULL-ARG", "strncmp() called with unchecked pointer parameter b");
    if (n == 0) return 0;
    size_t i = 0;
    while (i + 1 < n) {
        const unsigned char ca = uc(a, i);
        const unsigned char cb = uc(b, i);
        if (!ca || ca != cb) break;
        ++i;
    }
    {
        const unsigned char ca = uc(a, i);
        const unsigned char cb = uc(b, i);
        if (ca != cb) return ca > cb ? 1 : -1;
    }
    return 0;
}

char *strchr(const char *s, int c) {
    __prism_check(s != 0, "STR-NULL-ARG", "strchr() called with unchecked pointer parameter s");
    const unsigned char *us = (const unsigned char *)s;
    const unsigned char want = (unsigned char)(c & 0xff);
    for (size_t i = 0;; ++i) {
        if (us[i] == want) return (char *)(s + i);
        if (!us[i]) return 0;
    }
}

char *strrchr(const char *s, int c) {
    __prism_check(s != 0, "STR-NULL-ARG", "strrchr() called with unchecked pointer parameter s");
    const char *r = 0;
    const unsigned char *us = (const unsigned char *)s;
    const unsigned char want = (unsigned char)(c & 0xff);
    for (size_t i = 0;; ++i) {
        if (us[i] == want) r = s + i;
        if (!us[i]) return (char *)r;
    }
}

/* strspn returns how many leading characters of s are in accept.
 * It does not read past the terminator of either string.
 * An empty s returns 0. An empty accept returns 0.
 * A null s or a null accept is STR-NULL-ARG and returns 0 without a scan.
 * This is not PROVED and not CLEAN. */
size_t strspn(const char *s, const char *accept) {
    __prism_check(s != 0, "STR-NULL-ARG", "strspn() called with unchecked pointer parameter s");
    if (s == 0) return 0;
    __prism_check(accept != 0, "STR-NULL-ARG", "strspn() called with unchecked pointer parameter accept");
    if (accept == 0) return 0;
    if (!uc(accept, 0)) return 0;
    if (!uc(s, 0)) return 0;
    size_t n = 0;
    while (uc(s, n)) {
        size_t j = 0;
        while (uc(accept, j) != 0 && uc(accept, j) != uc(s, n)) ++j;
        if (!uc(accept, j)) return n;
        ++n;
    }
    return n;
}

/* strcspn returns how many leading characters of s are not in accept.
 * An empty accept returns the length of s, and that length does not read past s.
 * The first character that is in accept stops the scan.
 * A null s or a null accept is STR-NULL-ARG and returns 0 without a scan.
 * This is not PROVED and not CLEAN. */
size_t strcspn(const char *s, const char *accept) {
    __prism_check(s != 0, "STR-NULL-ARG", "strcspn() called with unchecked pointer parameter s");
    if (s == 0) return 0;
    __prism_check(accept != 0, "STR-NULL-ARG", "strcspn() called with unchecked pointer parameter accept");
    if (accept == 0) return 0;
    if (!uc(accept, 0)) {
        size_t n = 0;
        while (uc(s, n)) ++n;
        return n;
    }
    if (!uc(s, 0)) return 0;
    size_t n = 0;
    while (uc(s, n)) {
        size_t j = 0;
        while (uc(accept, j) != 0 && uc(accept, j) != uc(s, n)) ++j;
        if (uc(accept, j)) return n;
        ++n;
    }
    return n;
}

/* strstr returns a pointer to the first match of needle in s, or a null
 * pointer when there is none.
 * An empty needle returns s and does not scan s.
 * It does not read past the terminator of either string.
 * A null s or a null needle is STR-NULL-ARG and returns a null pointer
 * without a scan.
 * This is not PROVED and not CLEAN. */
char *strstr(const char *s, const char *needle) {
    __prism_check(s != 0, "STR-NULL-ARG", "strstr() called with unchecked pointer parameter s");
    if (s == 0) return 0;
    __prism_check(needle != 0, "STR-NULL-ARG", "strstr() called with unchecked pointer parameter needle");
    if (needle == 0) return 0;
    if (!uc(needle, 0)) return (char *)s;
    size_t i = 0;
    while (uc(s, i)) {
        size_t j = 0;
        while (uc(needle, j) != 0 && uc(s, i + j) != 0 && uc(s, i + j) == uc(needle, j)) ++j;
        if (!uc(needle, j)) return (char *)(s + i);
        if (!uc(s, i + j)) return 0;
        ++i;
    }
    return 0;
}

/* strpbrk returns a pointer to the first byte of s that occurs in accept,
 * or a null pointer when none does.
 * An empty accept returns a null pointer.
 * It does not read past the terminator of either string.
 * A null s or a null accept is STR-NULL-ARG and returns a null pointer
 * without a scan.
 * This is not PROVED and not CLEAN. */
char *strpbrk(const char *s, const char *accept) {
    __prism_check(s != 0, "STR-NULL-ARG", "strpbrk() called with unchecked pointer parameter s");
    if (s == 0) return 0;
    __prism_check(accept != 0, "STR-NULL-ARG", "strpbrk() called with unchecked pointer parameter accept");
    if (accept == 0) return 0;
    if (!uc(accept, 0)) return 0;
    size_t i = 0;
    while (uc(s, i)) {
        size_t j = 0;
        while (uc(accept, j) != 0 && uc(accept, j) != uc(s, i)) ++j;
        if (uc(accept, j)) return (char *)(s + i);
        ++i;
    }
    return 0;
}

/* requires: n bytes of d writable and of s readable; no overlap (checked) */
void *memcpy(void *d, const void *s, size_t n) {
    if (n == 0) return d;
    __prism_check(d != 0, "STR-NULL-ARG", "memcpy() called with unchecked pointer parameter d");
    __prism_check(s != 0, "STR-NULL-ARG", "memcpy() called with unchecked pointer parameter s");
    __prism_memcpy(d, s, n, 0);
    return d;
}

void *memmove(void *d, const void *s, size_t n) {
    if (n == 0) return d;
    __prism_check(d != 0, "STR-NULL-ARG", "memmove() called with unchecked pointer parameter d");
    __prism_check(s != 0, "STR-NULL-ARG", "memmove() called with unchecked pointer parameter s");
    __prism_memcpy(d, s, n, 1);
    return d;
}

void *memset(void *d, int c, size_t n) {
    if (n == 0) return d;
    __prism_check(d != 0, "STR-NULL-ARG", "memset() called with unchecked pointer parameter d");
    __prism_memset(d, c, n);
    return d;
}

int memcmp(const void *a, const void *b, size_t n) {
    if (n == 0) return 0;
    __prism_check(a != 0, "STR-NULL-ARG", "memcmp() called with unchecked pointer parameter a");
    __prism_check(b != 0, "STR-NULL-ARG", "memcmp() called with unchecked pointer parameter b");
    const unsigned char *x = (const unsigned char *)a;
    const unsigned char *y = (const unsigned char *)b;
    for (size_t i = 0; i < n; i++)
        if (x[i] != y[i]) return (int)x[i] - (int)y[i];
    return 0;
}

void *memchr(const void *s, int c, size_t n) {
    if (n == 0) return 0;
    __prism_check(s != 0, "STR-NULL-ARG", "memchr() called with unchecked pointer parameter s");
    const unsigned char *x = (const unsigned char *)s;
    const unsigned char want = (unsigned char)(c & 0xff);
    for (size_t i = 0; i < n; i++)
        if (x[i] == want) return (void *)(x + i);
    return 0;
}

/* memrchr returns a pointer to the last of the n bytes at s whose unsigned
 * value equals (unsigned char)(c & 0xff), or a null pointer when none does.
 * n == 0 returns a null pointer and does not read.
 * A null s with n > 0 is STR-NULL-ARG and returns a null pointer. That path
 * does not index s.
 * The scan reads only those n bytes, from the last byte backward.
 * This is not PROVED and not CLEAN. */
void *memrchr(const void *s, int c, size_t n) {
    if (n == 0) return 0;
    __prism_check(s != 0, "STR-NULL-ARG", "memrchr() called with unchecked pointer parameter s");
    if (s == 0) return 0;
    const unsigned char *x = (const unsigned char *)s;
    const unsigned char want = (unsigned char)(c & 0xff);
    size_t i = n;
    while (i > 0) {
        --i;
        if (x[i] == want) return (void *)(x + i);
    }
    return 0;
}

void *malloc(size_t n);

char *strdup(const char *s) {
    __prism_check(s != 0, "STR-NULL-ARG", "strdup() called with unchecked pointer parameter s");
    size_t n = strlen(s);
    char *p = (char *)malloc(n + 1);
    if (!p) return 0;
    __prism_memcpy(p, s, n + 1, 0);
    return p;
}

}  // extern "C"

/* libstdc++'s byte hash behind std::type_info::hash_code and std::hash of
   strings (libsupc++ hash_bytes.cc). Its value is unspecified by the
   standard; only "equal bytes give equal hashes" is relied on, which any
   deterministic function of (bytes, seed) gives: FNV-1a here. */
namespace std {
size_t _Hash_bytes(const void *p, size_t n, size_t seed) {
    __prism_check(p != 0 || n == 0, "STR-NULL-ARG", "_Hash_bytes() called with a null pointer");
    const unsigned char *b = (const unsigned char *)p;
    size_t h = seed ^ (size_t)14695981039346656037ull;
    for (size_t i = 0; i < n; i++) h = (h ^ b[i]) * (size_t)1099511628211ull;
    return h;
}
}  // namespace std
