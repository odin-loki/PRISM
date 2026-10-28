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

/* strcasecmp compares the bytes of a and b, folding only ASCII A-Z into a-z.
 * This is not a locale comparison.
 * It stops at the first difference or the first NUL.
 * It does not read past that byte.
 * A null a or a null b is STR-NULL-ARG and returns 0 before any read.
 * This is not a proof of the comparison.
 * This is not PROVED and not CLEAN. */
int strcasecmp(const char *a, const char *b) {
    __prism_check(a != 0, "STR-NULL-ARG", "strcasecmp() called with unchecked pointer parameter a");
    if (a == 0) return 0;
    __prism_check(b != 0, "STR-NULL-ARG", "strcasecmp() called with unchecked pointer parameter b");
    if (b == 0) return 0;
    size_t i = 0;
    for (;;) {
        unsigned char ca = uc(a, i);
        unsigned char cb = uc(b, i);
        if (ca >= 'A' && ca <= 'Z') ca = (unsigned char)(ca - 'A' + 'a');
        if (cb >= 'A' && cb <= 'Z') cb = (unsigned char)(cb - 'A' + 'a');
        if (ca != cb) return ca > cb ? 1 : -1;
        if (!ca) return 0;
        ++i;
    }
}

/* strncasecmp compares at most n bytes of a and b, folding only ASCII A-Z into a-z.
 * This is not a locale comparison.
 * It stops at the first difference, the first NUL, or n bytes.
 * It does not read the byte at index n.
 * A null a or a null b is STR-NULL-ARG and returns 0 before any read.
 * n == 0 returns 0 and does not read.
 * This is not a proof of the comparison.
 * This is not PROVED and not CLEAN. */
int strncasecmp(const char *a, const char *b, size_t n) {
    __prism_check(a != 0, "STR-NULL-ARG", "strncasecmp() called with unchecked pointer parameter a");
    if (a == 0) return 0;
    __prism_check(b != 0, "STR-NULL-ARG", "strncasecmp() called with unchecked pointer parameter b");
    if (b == 0) return 0;
    if (n == 0) return 0;
    size_t i = 0;
    while (i + 1 < n) {
        unsigned char ca = uc(a, i);
        unsigned char cb = uc(b, i);
        if (ca >= 'A' && ca <= 'Z') ca = (unsigned char)(ca - 'A' + 'a');
        if (cb >= 'A' && cb <= 'Z') cb = (unsigned char)(cb - 'A' + 'a');
        if (!ca || ca != cb) break;
        ++i;
    }
    {
        unsigned char ca = uc(a, i);
        unsigned char cb = uc(b, i);
        if (ca >= 'A' && ca <= 'Z') ca = (unsigned char)(ca - 'A' + 'a');
        if (cb >= 'A' && cb <= 'Z') cb = (unsigned char)(cb - 'A' + 'a');
        if (ca != cb) return ca > cb ? 1 : -1;
    }
    return 0;
}

/* strcoll compares the unsigned bytes of a and b.
 * This is not a locale collation and it does not call setlocale.
 * It stops at the first difference or the first NUL.
 * It does not read past that NUL.
 * A null a or a null b is STR-NULL-ARG and returns 0 before any read.
 * This is not a proof of the comparison.
 * This is not PROVED and not CLEAN. */
int strcoll(const char *a, const char *b) {
    __prism_check(a != 0, "STR-NULL-ARG", "strcoll() called with unchecked pointer parameter a");
    if (a == 0) return 0;
    __prism_check(b != 0, "STR-NULL-ARG", "strcoll() called with unchecked pointer parameter b");
    if (b == 0) return 0;
    size_t i = 0;
    for (;;) {
        const unsigned char ca = uc(a, i);
        const unsigned char cb = uc(b, i);
        if (ca != cb) return ca > cb ? 1 : -1;
        if (!ca) return 0;
        ++i;
    }
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

/* C23 7.26.6.2: memset that is not optimised away; same contract as memset */
void *memset_explicit(void *d, int c, size_t n) {
    if (n == 0) return d;
    __prism_check(d != 0, "STR-NULL-ARG", "memset_explicit() called with unchecked pointer parameter d");
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

/* memccpy copies bytes from s into d until c is copied, or n bytes are
 * copied, whichever comes first. The stop byte is (unsigned char)(c & 0xff),
 * the same conversion memchr uses.
 * n == 0 returns a null pointer and does not read d or s.
 * A null d or a null s with n > 0 is STR-NULL-ARG and returns a null
 * pointer before any read or write.
 * The copy does not read or write index i for i >= n.
 * If c is copied, the return is a pointer to the next byte of d.
 * If c is not found, the return is a null pointer.
 * This is not a proof of the copy.
 * This is not PROVED and not CLEAN. */
void *memccpy(void *d, const void *s, int c, size_t n) {
    if (n == 0) return 0;
    __prism_check(d != 0, "STR-NULL-ARG", "memccpy() called with unchecked pointer parameter d");
    if (d == 0) return 0;
    __prism_check(s != 0, "STR-NULL-ARG", "memccpy() called with unchecked pointer parameter s");
    if (s == 0) return 0;
    unsigned char *dd = (unsigned char *)d;
    const unsigned char *ss = (const unsigned char *)s;
    const unsigned char want = (unsigned char)(c & 0xff);
    for (size_t i = 0; i < n; ++i) {
        dd[i] = ss[i];
        if (dd[i] == want) return (void *)(dd + i + 1);
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

/* strndup returns a fresh copy of at most n bytes of s, plus a trailing NUL.
 * A null s is STR-NULL-ARG and returns a null pointer before strlen and
 * before any allocation. That path does not read s.
 * n == 0 does not read s. It allocates one byte, stores a NUL, and returns
 * that pointer. Allocation failure returns a null pointer.
 * Otherwise the copy is min(n, strlen(s)) bytes and a trailing NUL. The
 * scan does not read s[i] for i >= n, and it does not read past the
 * terminator. A copy length of SIZE_MAX has no room for the trailing
 * NUL: that path is MEM-OOB-WRITE and __builtin_trap, and it does not
 * call malloc.
 * This is not a proof of the copy.
 * This is not PROVED and not CLEAN. */
char *strndup(const char *s, size_t n) {
    __prism_check(s != 0, "STR-NULL-ARG", "strndup() called with unchecked pointer parameter s");
    if (s == 0) return 0;
    if (n == 0) {
        char *p = (char *)malloc(1);
        if (!p) return 0;
        p[0] = 0;
        return p;
    }
    size_t len = 0;
    while (len < n && s[len]) ++len;
    if (len == ~(size_t)0) {
        __prism_check(0, "MEM-OOB-WRITE", "strndup length does not fit in size_t");
        __builtin_trap();
    }
    char *p = (char *)malloc(len + 1);
    if (!p) return 0;
    for (size_t i = 0; i < len; ++i) p[i] = s[i];
    p[len] = 0;
    return p;
}

}  // extern "C"

