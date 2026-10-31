/* Exact sprintf/snprintf output (coverage item 1, sessions 10-11).
 * The translator lowers sprintf/snprintf with a literal format that has a
 * symbolic double to __prism_fmtv: the variadic arguments arrive in typed
 * slots (in call order within each class: 8 x int, 8 x long long, 8 x
 * double, 8 x pointer) and the whole format is rendered here: literal text,
 * %%, d i u o x X c s p and the double conversions (prism_fpfmt: the exact
 * bytes glibc prints), flags - + space # 0, width and precision including
 * '*'. Returns the full length; writes at most cap-1 characters and a NUL
 * when cap > 0. Not handled (the planner keeps the bounded havoc): %n, %L,
 * %ls/%lc, the ' flag.
 */
#include "prism_model.h"
#include "prism_fpfmt.h"

extern "C" int __prism_fmtv(char* buf, size_t cap, const char* fmt,
                            int i0, int i1, int i2, int i3, int i4, int i5, int i6, int i7,
                            long long j0, long long j1, long long j2, long long j3, long long j4, long long j5, long long j6, long long j7,
                            double d0, double d1, double d2, double d3, double d4, double d5, double d6, double d7,
                            const char* p0, const char* p1, const char* p2, const char* p3, const char* p4, const char* p5, const char* p6, const char* p7) {
    const int is[8] = {i0, i1, i2, i3, i4, i5, i6, i7};
    const long long js[8] = {j0, j1, j2, j3, j4, j5, j6, j7};
    const double ds[8] = {d0, d1, d2, d3, d4, d5, d6, d7};
    const char* const ps[8] = {p0, p1, p2, p3, p4, p5, p6, p7};
    int ni = 0, nj = 0, nd = 0, np = 0;
    size_t n = 0;
    auto put = [&](char c) { if (cap && n + 1 < cap) buf[n] = c; ++n; };
    for (const char* p = fmt; *p; ++p) {
        if (*p != '%') { put(*p); continue; }
        ++p;
        if (*p == '%') { put('%'); continue; }
        bool left = false, zero = false, plus = false, space = false, alt = false;
        for (;; ++p) {
            if (*p == '-') left = true; else if (*p == '0') zero = true; else if (*p == '+') plus = true;
            else if (*p == ' ') space = true; else if (*p == '#') alt = true; else break;
        }
        int width = 0;
        if (*p == '*') { width = ni < 8 ? is[ni++] : 0; if (width < 0) { left = true; width = -width; } ++p; }
        else while (*p >= '0' && *p <= '9') width = width * 10 + (*p++ - '0');
        int prec = -1;
        if (*p == '.') {
            ++p; prec = 0;
            if (*p == '*') { prec = ni < 8 ? is[ni++] : 0; if (prec < 0) prec = -1; ++p; }
            else while (*p >= '0' && *p <= '9') prec = prec * 10 + (*p++ - '0');
        }
        int lm = 0;  // 0 int, -1 h, -2 hh, 1 long (64), 
        for (;; ++p) {
            if (*p == 'h') lm = lm == -1 ? -2 : -1;
            else if (*p == 'l' || *p == 'j' || *p == 'z' || *p == 't' || *p == 'q') lm = 1;
            else break;
        }
        const char c = *p;
        if (!c) break;
        char t[400];
        int len = 0, k = 0;
        bool numeric = true, finite = true, hexpfx = false;
        if (c == 'd' || c == 'i' || c == 'u' || c == 'o' || c == 'x' || c == 'X') {
            const bool sg = c == 'd' || c == 'i';
            unsigned long long u; bool neg = false;
            if (lm == 1) { long long v = nj < 8 ? js[nj++] : 0; if (sg && v < 0) { neg = true; u = 0ull - (unsigned long long)v; } else u = (unsigned long long)v; }
            else {
                int v = ni < 8 ? is[ni++] : 0;
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
            if (prec >= 0) zero = false;
        } else if (c == 'c') {
            t[len++] = (char)(ni < 8 ? is[ni++] : 0); numeric = false;
        } else if (c == 's') {
            const char* s = np < 8 ? ps[np++] : nullptr; numeric = false;
            if (!s) s = "(null)";
            // long strings: emitted directly, padding computed on the length
            int sl = 0; while (s[sl] && (prec < 0 || sl < prec)) ++sl;
            const int pad = width > sl ? width - sl : 0;
            if (!left) for (int z = 0; z < pad; ++z) put(' ');
            for (int z = 0; z < sl; ++z) put(s[z]);
            if (left) for (int z = 0; z < pad; ++z) put(' ');
            continue;
        } else if (c == 'p') {
            const char* q = np < 8 ? ps[np++] : nullptr; numeric = false;
            unsigned long long u = (unsigned long long)(size_t)q;
            if (!u) { const char* nl = "(nil)"; while (*nl) t[len++] = *nl++; }
            else { t[len++] = '0'; t[len++] = 'x'; char r[20]; int nr = 0; for (; u; u >>= 4) r[nr++] = "0123456789abcdef"[u & 15]; while (nr) t[len++] = r[--nr]; }
        } else if (c == 'f' || c == 'F' || c == 'e' || c == 'E' || c == 'g' || c == 'G' || c == 'a' || c == 'A') {
            const double v = nd < 8 ? ds[nd++] : 0;
            unsigned fl = (alt ? prism_fpfmt::kAlt : 0u) | ((c == 'a' || c == 'A') ? prism_fpfmt::kPrintfExp : 0u);
            if (plus) fl |= prism_fpfmt::kPlus; else if (space) fl |= prism_fpfmt::kSpace;
            const int pr = prec >= 0 ? prec : ((c == 'a' || c == 'A') ? -1 : 6);
            len = prism_fpfmt::to_chars(t, (int)sizeof t, v, c, pr, fl);
            if (len > (int)sizeof t) len = (int)sizeof t;  // %.400f and wider: bytes past 400 are not exact
            finite = v - v == 0;
            if (k < len && (t[k] == '-' || t[k] == '+' || t[k] == ' ')) ++k;
            if ((c == 'a' || c == 'A') && k + 1 < len && t[k] == '0' && (t[k + 1] == 'x' || t[k + 1] == 'X')) k += 2;
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
    if (cap) buf[n < cap ? n : cap - 1] = 0;
    return (int)n;
}
