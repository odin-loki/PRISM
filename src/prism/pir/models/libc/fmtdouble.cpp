/* Exact sprintf/snprintf output for one symbolic double (coverage item 1).
 * The translator lowers sprintf/snprintf whose literal format has exactly
 * one conversion, a double one (%f %F %e %E %g %G %a %A, no '*' and no '
 * flag), to __prism_fmt1d: literal text and %% are copied, the double is
 * formatted by the prism_fpfmt algorithm (exact decimal expansion, the
 * bytes glibc prints), then width / '-' / '0' padding is applied. Returns
 * the full length; writes at most cap-1 characters and a NUL when cap > 0.
 */
#include "prism_model.h"
#include "prism_fpfmt.h"

extern "C" int __prism_fmt1d(char* buf, size_t cap, const char* fmt, double v) {
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
        while (*p >= '0' && *p <= '9') width = width * 10 + (*p++ - '0');
        int prec = -1;
        if (*p == '.') { ++p; prec = 0; while (*p >= '0' && *p <= '9') prec = prec * 10 + (*p++ - '0'); }
        while (*p == 'l' || *p == 'h') ++p;
        const char c = *p;
        if (!c) break;
        unsigned fl = (alt ? prism_fpfmt::kAlt : 0u) | ((c == 'a' || c == 'A') ? prism_fpfmt::kPrintfExp : 0u);
        if (plus) fl |= prism_fpfmt::kPlus; else if (space) fl |= prism_fpfmt::kSpace;
        const int pr = prec >= 0 ? prec : ((c == 'a' || c == 'A') ? -1 : 6);
        char t[400];
        const int len = prism_fpfmt::to_chars(t, (int)sizeof t, v, c, pr, fl);
        const int pad = width > len ? width - len : 0;
        const bool finite = v - v == 0;
        if (!left && !(zero && finite)) for (int i = 0; i < pad; ++i) put(' ');
        int k = 0;
        if (!left && zero && finite) {
            // zeros go after the sign and a 0x prefix
            if (k < len && (t[k] == '-' || t[k] == '+' || t[k] == ' ')) put(t[k++]);
            if ((c == 'a' || c == 'A') && k + 1 < len && t[k] == '0' && (t[k + 1] == 'x' || t[k + 1] == 'X')) { put(t[k++]); put(t[k++]); }
            for (int i = 0; i < pad; ++i) put('0');
        }
        for (; k < len && k < (int)sizeof t; ++k) put(t[k]);
        if (left) for (int i = 0; i < pad; ++i) put(' ');
    }
    if (cap) buf[n < cap ? n : cap - 1] = 0;
    return (int)n;
}
