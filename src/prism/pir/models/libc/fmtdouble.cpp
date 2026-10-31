/* Exact sprintf/snprintf output (coverage item 1, sessions 10-11).
 * The translator lowers sprintf/snprintf with a literal format that has a
 * symbolic double to __prism_fmtv: the variadic arguments arrive in typed
 * slots (in call order within each class: 8 x int, 8 x long long, 8 x
 * double, 8 x pointer) and the whole format is rendered here: literal text,
 * %%, d i u o x X c s p and the double conversions (prism_fpfmt: the exact
 * bytes glibc prints), flags - + space # 0, width and precision including
 * '*'. Returns the full length; writes at most cap-1 characters and a NUL
 * when cap > 0. Also %n (all sizes), %Lf/%Le/%Lg/%La, %lc/%ls (C locale:
 * ASCII, else -1/EILSEQ) and the ' flag (no grouping in the C locale).
 */
#include "prism_model.h"
#include "prism_fpfmt.h"

// glibc %La for the x86 80-bit format (sysdeps/ieee754/ldbl-96
// printf_fphex): the leading hex digit is the top nibble of the explicit
// 64-bit mantissa, 15 fraction digits, exponent = biased - 16383 - 3; a
// carry out of the leading digit gives 0x1 and exponent + 4.
static int __prism_ldhex(char* t, long double v, int prec, bool upper, bool alt, bool plus, bool space) {
    prism_fpfmt::Bits b = prism_fpfmt::decompose(v);
    int len = 0;
    if (b.neg) t[len++] = '-'; else if (plus) t[len++] = '+'; else if (space) t[len++] = ' ';
    t[len++] = '0'; t[len++] = upper ? 'X' : 'x';
    const char* dig = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    unsigned long long m = b.m;
    long exp = m ? (long)b.e + 60 : 0;
    unsigned lead = (unsigned)(m >> 60);
    unsigned long long frac = m & ((1ull << 60) - 1);
    int nd = 15;
    if (prec >= 0 && prec < 15) {
        const int sh = 4 * (15 - prec);
        const unsigned long long rem = frac & ((1ull << sh) - 1), half = 1ull << (sh - 1);
        unsigned long long top = (((unsigned long long)lead << 60) | frac) >> sh;  // lead + kept digits
        if (rem > half || (rem == half && (top & 1))) ++top;
        if ((top >> (4 * prec)) > 0xf) { lead = 1; frac = 0; exp += 4; }
        else { lead = (unsigned)(top >> (4 * prec)); frac = (top & ((1ull << (4 * prec)) - 1)) << sh; }
        nd = prec;
    } else if (prec < 0) {
        while (nd > 0 && ((frac >> (4 * (15 - nd))) & 15) == 0) --nd;
    }
    t[len++] = dig[lead];
    if (nd > 0 || alt || prec > 0) t[len++] = '.';
    for (int z = 0; z < nd && z < 15; ++z) t[len++] = dig[(frac >> (4 * (14 - z))) & 15];
    for (int z = 15; z < prec; ++z) t[len++] = '0';
    t[len++] = upper ? 'P' : 'p';
    t[len++] = exp < 0 ? '-' : '+';
    unsigned long ue = (unsigned long)(exp < 0 ? -exp : exp);
    char r[8]; int nr = 0;
    do { r[nr++] = char('0' + ue % 10); ue /= 10; } while (ue);
    while (nr) t[len++] = r[--nr];
    return len;
}

// Renders the conversions whose first argument index lies in [ab, ae)
// (and the literal text after them; text before the first conversion goes
// with ab == 0), continuing from pos0 characters already produced. The
// typed slots hold the arguments with indices [ab, ab + 8) in order of
// their class. The translator chains calls over windows of the argument
// list, so there is no fixed cap on the number of arguments.
// Locale state for the printf model (setlocale below): LC_CTYPE UTF-8
// (C.UTF-8, en_US.UTF-8) and LC_NUMERIC grouping (en_US: ',' every 3).
extern "C" int __prism_lc_utf8 = 0;
extern "C" int __prism_lc_group = 0;
static const char* __prism_lc_names[13] = {"C", "C", "C", "C", "C", "C", "C", "C", "C", "C", "C", "C", "C"};
static int __prism_lc_eq(const char* a, const char* b) { while (*a && *a == *b) { ++a; ++b; } return *a == *b; }
// glibc categories: CTYPE 0, NUMERIC 1, TIME 2, COLLATE 3, MONETARY 4,
// MESSAGES 5, ALL 6, then PAPER .. IDENTIFICATION 7..12. The environment
// of an analysed run is empty, so "" selects "C". Locales other than C,
// POSIX, C.UTF-8 and en_US.UTF-8 are not installed: NULL.
extern "C" char* setlocale(int cat, const char* name) {
    if (cat < 0 || cat > 12) return nullptr;
    if (!name) return (char*)__prism_lc_names[cat];
    const char* canon;
    int utf8 = 0, group = 0;
    if (!*name || __prism_lc_eq(name, "C") || __prism_lc_eq(name, "POSIX")) canon = "C";
    else if (__prism_lc_eq(name, "C.UTF-8") || __prism_lc_eq(name, "C.utf8")) { canon = "C.UTF-8"; utf8 = 1; }
    else if (__prism_lc_eq(name, "en_US.UTF-8") || __prism_lc_eq(name, "en_US.utf8")) { canon = "en_US.UTF-8"; utf8 = 1; group = 1; }
    else return nullptr;
    if (cat == 6) { for (int k = 0; k < 13; ++k) __prism_lc_names[k] = canon; __prism_lc_utf8 = utf8; __prism_lc_group = group; }
    else { __prism_lc_names[cat] = canon; if (cat == 0) __prism_lc_utf8 = utf8; if (cat == 1) __prism_lc_group = group; }
    return (char*)canon;
}
// UTF-8 bytes of one wide character (wcrtomb): 0 for an invalid one.
static int __prism_wc8(unsigned w, char* o) {
    if (!__prism_lc_utf8) { if (w > 0x7f) return 0; o[0] = (char)w; return 1; }
    if (w < 0x80) { o[0] = (char)w; return 1; }
    if (w < 0x800) { o[0] = char(0xc0 | (w >> 6)); o[1] = char(0x80 | (w & 63)); return 2; }
    if (w >= 0xd800 && w < 0xe000) return 0;
    if (w < 0x10000) { o[0] = char(0xe0 | (w >> 12)); o[1] = char(0x80 | ((w >> 6) & 63)); o[2] = char(0x80 | (w & 63)); return 3; }
    if (w < 0x110000) { o[0] = char(0xf0 | (w >> 18)); o[1] = char(0x80 | ((w >> 12) & 63)); o[2] = char(0x80 | ((w >> 6) & 63)); o[3] = char(0x80 | (w & 63)); return 4; }
    return 0;
}
// ' grouping: insert ',' every 3 digits into the integer digits t[a, b).
static int __prism_group(char* t, int len, int a, int b, int capt) {
    const int nd = b - a;
    if (nd <= 3) return len;
    const int nc = (nd - 1) / 3;
    if (len + nc > capt) return len;
    for (int z = len - 1; z >= b; --z) t[z + nc] = t[z];
    int w = b + nc - 1;
    for (int z = b - 1, cnt = 0; z >= a; --z) { t[w--] = t[z]; if (++cnt % 3 == 0 && z > a) t[w--] = ','; }
    return len + nc;
}

// Argument classes per window (cls, 3 bits per argument from the
// translator): 0 i32, 1 i64, 2 double, 3 x86_fp80, 4 ptr, 5 __int128
// (two consecutive integer reads, low then high, as va_arg sees the pair
// of GP registers), 6 struct passed byval (its eightbytes, in order, from
// the copy the pointer slot addresses).
extern "C" int __prism_fmtv(char* buf, size_t cap, const char* fmt, int ab, int ae, int pos0, int clsw,
                            int i0, int i1, int i2, int i3, int i4, int i5, int i6, int i7,
                            long long j0, long long j1, long long j2, long long j3, long long j4, long long j5, long long j6, long long j7,
                            double d0, double d1, double d2, double d3, double d4, double d5, double d6, double d7,
                            long double l0, long double l1, long double l2, long double l3, long double l4, long double l5, long double l6, long double l7,
                            const char* p0, const char* p1, const char* p2, const char* p3, const char* p4, const char* p5, const char* p6, const char* p7,
                            __int128 q0, __int128 q1, __int128 q2, __int128 q3, __int128 q4, __int128 q5, __int128 q6, __int128 q7) {
    const __int128 qs[8] = {q0, q1, q2, q3, q4, q5, q6, q7};
    int nq = 0;
    const int is[8] = {i0, i1, i2, i3, i4, i5, i6, i7};
    const long long js[8] = {j0, j1, j2, j3, j4, j5, j6, j7};
    const double ds[8] = {d0, d1, d2, d3, d4, d5, d6, d7};
    const long double ls[8] = {l0, l1, l2, l3, l4, l5, l6, l7};
    int nl = 0;
    const char* const ps[8] = {p0, p1, p2, p3, p4, p5, p6, p7};
    int ni = 0, nj = 0, nd = 0, np = 0;
    if (pos0 < 0) return pos0;  // an earlier window failed (EILSEQ)
    size_t n = (size_t)pos0;
    int ai = 0;          // global index of the next argument
    bool on = ab == 0;   // the current conversion (and its trailing text) is ours
    auto win = [&]() { int a = ai++; return a >= ab && a < ab + 8; };
    auto clsof = [&](int a) { return (a >= ab && a < ab + 8) ? (clsw >> (3 * (a - ab))) & 7 : -1; };
    // a pending eightbyte from a __int128 (high half) or a byval struct
    bool pend = false; unsigned long long pendv = 0;
    const unsigned long long* bv = nullptr; int bvn = 0;
    // an integer read: i128 and byval arguments feed several reads
    auto gint = [&](bool wide) -> long long {
        if (pend) { pend = false; return (long long)pendv; }
        if (bv && bvn > 0) { --bvn; long long v = (long long)*bv++; if (!bvn) bv = nullptr; return v; }
        const int k = clsof(ai);
        if (k == 5) { win(); __int128 v = nq < 8 ? qs[nq++] : 0; pend = true; pendv = (unsigned long long)((unsigned __int128)v >> 64); return (long long)(unsigned long long)v; }
        if (k == 6) { win(); const char* sp = np < 8 ? ps[np++] : nullptr; if (!sp) return 0; bv = (const unsigned long long*)sp; bvn = 1; long long v = (long long)*bv++; return v; }
        if (k == 4) { win(); return (long long)(size_t)(np < 8 ? ps[np++] : nullptr); }
        if (k == 1 || (wide && k != 0)) { bool w = win(); return w && nj < 8 ? js[nj++] : 0; }
        bool w = win(); return w && ni < 8 ? is[ni++] : 0;
    };
    auto gi = [&]() -> int { return (int)gint(false); };
    auto gj = [&]() -> long long { return gint(true); };
    auto gd = [&]() -> double { bool w = win(); return w && nd < 8 ? ds[nd++] : 0.0; };
    auto gl = [&]() -> long double { bool w = win(); return w && nl < 8 ? ls[nl++] : 0.0L; };
    auto gp = [&]() -> const char* { bool w = win(); return w && np < 8 ? ps[np++] : nullptr; };
    auto put = [&](char c) { if (!on) return; if (cap && n + 1 < cap) buf[n] = c; ++n; };
    for (const char* p = fmt; *p; ++p) {
        if (*p != '%') { put(*p); continue; }
        ++p;
        if (*p == '%') { put('%'); continue; }
        on = ai >= ab && ai < ae;
        bool left = false, zero = false, plus = false, space = false, alt = false, group = false;
        for (;; ++p) {
            if (*p == '-') left = true; else if (*p == '0') zero = true; else if (*p == '+') plus = true;
            else if (*p == ' ') space = true; else if (*p == '#') alt = true;
            else if (*p == '\'') group = __prism_lc_group;  // thousands grouping (en_US: ',' / 3)
            else break;
        }
        int width = 0;
        if (*p == '*') { width = gi(); if (width < 0) { left = true; width = -width; } ++p; }
        else while (*p >= '0' && *p <= '9') width = width * 10 + (*p++ - '0');
        int prec = -1;
        if (*p == '.') {
            ++p; prec = 0;
            if (*p == '*') { prec = gi(); if (prec < 0) prec = -1; ++p; }
            else while (*p >= '0' && *p <= '9') prec = prec * 10 + (*p++ - '0');
        }
        int lm = 0;  // 0 int, -1 h, -2 hh, 1 long (64), 
        for (;; ++p) {
            if (*p == 'h') lm = lm == -1 ? -2 : -1;
            else if (*p == 'L') lm = 2;
            else if (*p == 'l' || *p == 'j' || *p == 'z' || *p == 't' || *p == 'q') lm = 1;
            else break;
        }
        const char c = *p;
        if (!c) break;
        char t[5000];  // %Lf of LDBL_MAX is 4933 integer digits
        int len = 0, k = 0;
        bool numeric = true, finite = true, hexpfx = false;
        if (c == 'd' || c == 'i' || c == 'u' || c == 'o' || c == 'x' || c == 'X') {
            const bool sg = c == 'd' || c == 'i';
            unsigned long long u; bool neg = false;
            if (lm == 1) { long long v = gj(); if (sg && v < 0) { neg = true; u = 0ull - (unsigned long long)v; } else u = (unsigned long long)v; }
            else {
                int v = gi();
                if (lm == -1) v = sg ? (int)(short)v : (int)(unsigned short)v;
                if (lm == -2) v = sg ? (int)(signed char)v : (int)(unsigned char)v;
                if (sg && v < 0) { neg = true; u = 0ull - (unsigned long long)(long long)v; } else u = sg ? (unsigned long long)v : (unsigned long long)(unsigned)v;
            }
            const unsigned base = (c == 'o') ? 8 : (c == 'x' || c == 'X') ? 16 : 10;
            const char* dig = c == 'X' ? "0123456789ABCDEF" : "0123456789abcdef";
            char r[70]; int nr = 0;
            for (unsigned long long w = u; w; w /= base) r[nr++] = dig[w % base];
            int zeros = prec > nr ? prec - nr : 0;
            if (prec < 0 && nr == 0) r[nr++] = '0';
            if (c == 'o' && alt && (nr == 0 || r[nr - 1] != '0') && zeros == 0) zeros = 1;
            if (neg) t[len++] = '-'; else if (sg && plus) t[len++] = '+'; else if (sg && space) t[len++] = ' ';
            if (alt && base == 16 && u) { t[len++] = '0'; t[len++] = c; hexpfx = true; }
            k = len;  // zero padding goes here
            for (int z = 0; z < zeros; ++z) t[len++] = '0';
            while (nr) t[len++] = r[--nr];
            if (group && base == 10) len = __prism_group(t, len, k, len, (int)sizeof t);
            if (prec >= 0) zero = false;
        } else if (c == 'n') {
            void* q = (void*)gp();
            if (on && q) {
                if (lm == -2) *(signed char*)q = (signed char)n;
                else if (lm == -1) *(short*)q = (short)n;
                else if (lm == 1) *(long long*)q = (long long)n;
                else *(int*)q = (int)n;
            }
            continue;
        } else if (c == 'c' && lm == 1) {  // %lc: wcrtomb in the current LC_CTYPE (C: ASCII; UTF-8), else EILSEQ
            const int w = gi(); numeric = false;
            char mb[4]; const int k8 = __prism_wc8((unsigned)w, mb);
            if (!k8) { if (on) return -1; continue; }
            for (int z = 0; z < k8; ++z) t[len++] = mb[z];
        } else if (c == 's' && lm == 1) {  // %ls: precision counts bytes
            const int* ws = (const int*)gp(); numeric = false;
            static const int nulls[] = {'(', 'n', 'u', 'l', 'l', ')', 0};
            if (!ws) ws = nulls;
            // bytes: whole characters only, up to the precision
            int sl = 0, nw = 0;
            for (; ws[nw]; ++nw) {
                char mb[4]; const int k8 = __prism_wc8((unsigned)ws[nw], mb);
                if (!k8) { if (on) return -1; break; }
                if (prec >= 0 && sl + k8 > prec) break;
                sl += k8;
            }
            const int pad = width > sl ? width - sl : 0;
            if (!left) for (int z = 0; z < pad; ++z) put(' ');
            for (int z = 0, b = 0; b < sl; ++z) { char mb[4]; const int k8 = __prism_wc8((unsigned)ws[z], mb); for (int y = 0; y < k8; ++y) put(mb[y]); b += k8; }
            if (left) for (int z = 0; z < pad; ++z) put(' ');
            continue;
        } else if (c == 'c') {
            t[len++] = (char)(gi()); numeric = false;
        } else if (c == 's') {
            const char* s = gp(); numeric = false;
            if (!s) s = "(null)";
            // long strings: emitted directly, padding computed on the length
            int sl = 0; while (s[sl] && (prec < 0 || sl < prec)) ++sl;
            const int pad = width > sl ? width - sl : 0;
            if (!left) for (int z = 0; z < pad; ++z) put(' ');
            for (int z = 0; z < sl; ++z) put(s[z]);
            if (left) for (int z = 0; z < pad; ++z) put(' ');
            continue;
        } else if (c == 'p') {
            const char* q = gp(); numeric = false;
            unsigned long long u = (unsigned long long)(size_t)q;
            if (!u) { const char* nl = "(nil)"; while (*nl) t[len++] = *nl++; }
            else { t[len++] = '0'; t[len++] = 'x'; char r[20]; int nr = 0; for (; u; u >>= 4) r[nr++] = "0123456789abcdef"[u & 15]; while (nr) t[len++] = r[--nr]; }
        } else if (c == 'f' || c == 'F' || c == 'e' || c == 'E' || c == 'g' || c == 'G' || c == 'a' || c == 'A') {
            const bool ldv = lm == 2;  // %Lf %Le %Lg %La
            const long double lv = ldv ? (gl()) : 0.0L;
            const double v = ldv ? 0.0 : (gd());
            unsigned fl = (alt ? prism_fpfmt::kAlt : 0u) | ((c == 'a' || c == 'A') ? prism_fpfmt::kPrintfExp : 0u);
            if (plus) fl |= prism_fpfmt::kPlus; else if (space) fl |= prism_fpfmt::kSpace;
            const int pr = prec >= 0 ? prec : ((c == 'a' || c == 'A') ? -1 : 6);
            const bool lfin = ldv && lv - lv == 0;
            len = (lfin && (c == 'a' || c == 'A')) ? __prism_ldhex(t, lv, prec, c == 'A', alt, plus, space)
                : ldv ? prism_fpfmt::to_chars(t, (int)sizeof t, lv, c, pr, fl) : prism_fpfmt::to_chars(t, (int)sizeof t, v, c, pr, fl);
            if (len > (int)sizeof t) len = (int)sizeof t;  // precision past ~4990: bytes past the buffer are not exact
            finite = ldv ? lv - lv == 0 : v - v == 0;
            if (k < len && (t[k] == '-' || t[k] == '+' || t[k] == ' ')) ++k;
            if ((c == 'a' || c == 'A') && k + 1 < len && t[k] == '0' && (t[k + 1] == 'x' || t[k + 1] == 'X')) k += 2;
            if (group && finite && (c == 'f' || c == 'F' || c == 'g' || c == 'G')) {
                int e = k; while (e < len && t[e] >= '0' && t[e] <= '9') ++e;
                if (e == len || t[e] != 'e' && t[e] != 'E') len = __prism_group(t, len, k, e, (int)sizeof t);
                else if (true) {}  // %g in exponent form: one integer digit
            }
            (void)hexpfx;
        } else {
            continue;  // not reached: the planner rejects other conversions
        }
        const int pad = width > len ? width - len : 0;
        const bool zp = !left && zero && numeric && finite;
        if (!left && !zp) for (int z = 0; z < pad; ++z) put(' ');
        if (zp) { for (int z = 0; z < k; ++z) put(t[z]); for (int z = 0; z < pad; ++z) put('0'); for (int z = k; z < len; ++z) put(t[z]); }
        else for (int z = 0; z < len; ++z) put(t[z]);
        if (left) for (int z = 0; z < pad; ++z) put(' ');
    }
    if (cap && ae == 0x7fffffff) buf[n < cap ? n : cap - 1] = 0;  // the last window terminates
    return (int)n;
}
