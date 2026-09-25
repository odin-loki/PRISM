// printf-family operational model (roadmap 2.6, docs/PIR.md "Library
// models"): with a literal format, every conversion is matched against the
// argument list (count and IR type, C17 7.21.6.1p2 and p9), %n is reported
// (FMT-PERCENT-N), %s arguments must be NUL-terminated strings (checked by
// the strlen model), and sprintf/snprintf write their output into the
// destination buffer (bounded by the maximal output length).

#include "translate_mem.hpp"

#include <algorithm>
#include <cctype>
#include <map>

namespace prism::pir::pirmem {

namespace {

struct Fn {
    int fmt;
    int buf;
    int size;
};

const std::map<std::string, Fn>& format_fns() {
    static const std::map<std::string, Fn> k{
        {"printf", {0, -1, -1}}, {"fprintf", {1, -1, -1}}, {"dprintf", {1, -1, -1}},
        {"sprintf", {1, 0, -1}}, {"snprintf", {2, 0, 1}},
    };
    return k;
}

bool is_int_ty(const ir::Type& t, unsigned bits) { return t.kind == ir::Type::Int && t.bits == bits; }

}  // namespace

bool is_format_function(const std::string& callee) {
    return format_fns().count(callee) || callee == "vprintf" || callee == "vfprintf" || callee == "vsprintf" ||
           callee == "vsnprintf" || callee == "scanf" || callee == "fscanf" || callee == "sscanf";
}

FormatPlan plan_format(const std::string& callee, const std::optional<std::string>& fmt,
                       const std::vector<ir::Type>& args, const std::vector<std::optional<uint64_t>>& consts) {
    FormatPlan p;
    if (!is_format_function(callee)) return p;
    p.handled = true;
    auto it = format_fns().find(callee);
    if (it == format_fns().end()) {
        p.unencoded = "UNENCODED: call @" + callee + " (va_list / scanf formats are not modelled)";
        return p;
    }
    p.fmt_arg = it->second.fmt;
    p.buf_arg = it->second.buf;
    p.size_arg = it->second.size;
    if (!fmt) {
        p.unencoded = "UNENCODED: call @" + callee + " with a non-literal format string";
        return p;
    }
    const std::string& f = *fmt;
    std::size_t next = static_cast<std::size_t>(p.fmt_arg) + 1;
    uint64_t len = 0;
    bool bounded = true;
    auto take = [&](const char* what, std::string_view conv, auto&& ok) {
        if (next >= args.size()) {
            p.violations.emplace_back("FMT-ARGS", callee + " format '%" + std::string(conv) +
                                                      "' has no matching argument (C17 7.21.6.1p2)");
            return;
        }
        if (!ok(args[next]))
            p.violations.emplace_back("FMT-ARGS", callee + " argument " + std::to_string(next + 1) + " has type " +
                                                      args[next].text + ", '%" + std::string(conv) + "' expects " +
                                                      what + " (C17 7.21.6.1p9)");
        ++next;
    };
    for (std::size_t i = 0; i < f.size(); ++i) {
        if (f[i] != '%') {
            ++len;
            continue;
        }
        std::size_t start = i++;
        if (i < f.size() && f[i] == '%') {
            ++len;
            continue;
        }
        bool sign_flag = false, alt_flag = false;
        while (i < f.size() && std::string_view("-+ #0'").find(f[i]) != std::string_view::npos) {
            if (f[i] == '+' || f[i] == ' ') sign_flag = true;
            if (f[i] == '#') alt_flag = true;
            if (f[i] == '\'') alt_flag = true;  // grouping: separators, no exact length
            ++i;
        }
        uint64_t width = 0, prec = 0;
        bool has_prec = false;
        if (i < f.size() && f[i] == '*') {
            take("int", "*", [](const ir::Type& t) { return is_int_ty(t, 32); });
            bounded = false;
            ++i;
        } else {
            while (i < f.size() && std::isdigit(static_cast<unsigned char>(f[i]))) width = width * 10 + static_cast<uint64_t>(f[i++] - '0');
        }
        if (i < f.size() && f[i] == '.') {
            has_prec = true;
            ++i;
            if (i < f.size() && f[i] == '*') {
                take("int", ".*", [](const ir::Type& t) { return is_int_ty(t, 32); });
                bounded = false;
                ++i;
            } else {
                while (i < f.size() && std::isdigit(static_cast<unsigned char>(f[i]))) prec = prec * 10 + static_cast<uint64_t>(f[i++] - '0');
            }
        }
        std::string lenmod;
        while (i < f.size() && std::string_view("hljztLq").find(f[i]) != std::string_view::npos) lenmod += f[i++];
        if (i >= f.size()) {
            p.violations.emplace_back("FMT-ARGS", callee + " format ends inside a conversion specification");
            break;
        }
        char c = f[i];
        auto conv = f.substr(start + 1, i - start);
        bool wide = lenmod == "l" || lenmod == "ll" || lenmod == "j" || lenmod == "z" || lenmod == "t" || lenmod == "q";
        unsigned ib = wide ? 64 : 32;
        uint64_t m = 0;
        // The literal integer argument of this conversion, when the call passes one
        // of the conversion's own width (hh/h/j/... fall back to the type maximum).
        auto literal = [&]() -> std::optional<uint64_t> {
            if (alt_flag || (lenmod != "" && !wide)) return std::nullopt;
            if (next >= args.size() || next >= consts.size() || !consts[next]) return std::nullopt;
            if (!is_int_ty(args[next], ib)) return std::nullopt;
            return ib == 64 ? *consts[next] : (*consts[next] & 0xffffffffull);
        };
        auto digits = [](uint64_t v, unsigned base) {
            uint64_t n = 1;
            while (v >= base) {
                v /= base;
                ++n;
            }
            return n;
        };
        switch (c) {
            case 'd':
            case 'i': {
                auto lit = literal();
                take(wide ? "a 64-bit integer" : "int", conv, [&](const ir::Type& t) { return is_int_ty(t, ib); });
                m = wide ? 20 : 11;
                if (lit) {
                    int64_t v = ib == 64 ? static_cast<int64_t>(*lit) : static_cast<int32_t>(static_cast<uint32_t>(*lit));
                    uint64_t mag = v < 0 ? (0 - static_cast<uint64_t>(v)) : static_cast<uint64_t>(v);
                    m = digits(mag, 10) + ((v < 0 || sign_flag) ? 1 : 0);
                }
                break;
            }
            case 'u':
            case 'o':
            case 'x':
            case 'X': {
                auto lit = literal();
                take(wide ? "a 64-bit integer" : "unsigned int", conv, [&](const ir::Type& t) { return is_int_ty(t, ib); });
                m = c == 'o' ? (wide ? 22 : 11) : c == 'u' ? (wide ? 20 : 10) : (wide ? 16 : 8);
                if (lit) m = digits(*lit, c == 'o' ? 8 : c == 'u' ? 10 : 16);
                break;
            }
            case 'c':
                take("int", conv, [](const ir::Type& t) { return is_int_ty(t, 32); });
                m = 1;
                break;
            case 's':
                take("a string pointer", conv, [](const ir::Type& t) { return t.kind == ir::Type::Ptr; });
                if (!has_prec && next - 1 < args.size() && args[next - 1].kind == ir::Type::Ptr)
                    p.cstr_args.push_back(next - 1);
                bounded = bounded && has_prec;
                m = prec;
                break;
            case 'p':
                take("a pointer", conv, [](const ir::Type& t) { return t.kind == ir::Type::Ptr; });
                m = 18;
                break;
            case 'n':
                take("a pointer", conv, [](const ir::Type& t) { return t.kind == ir::Type::Ptr; });
                p.violations.emplace_back("FMT-PERCENT-N", callee + " format uses %n (writes through an argument)");
                break;
            case 'f':
            case 'F':
            case 'e':
            case 'E':
            case 'g':
            case 'G':
            case 'a':
            case 'A': {
                bool ld = lenmod == "L";
                take(ld ? "long double" : "double", conv, [&](const ir::Type& t) {
                    return t.kind == ir::Type::Float && t.text == (ld ? "x86_fp80" : "double");
                });
                m = (c == 'f' || c == 'F') ? 330 + prec : 32 + prec;
                break;
            }
            default:
                p.unencoded = "UNENCODED: " + callee + " conversion '%" + std::string(conv) + "'";
                return p;
        }
        len += std::max<uint64_t>({m, width, has_prec ? prec + 1 : 0});
    }
    if (p.buf_arg >= 0 && p.size_arg < 0) {
        if (!bounded) {
            p.unencoded = "UNENCODED: " + callee + " with %s or '*' (output length not bounded by the format)";
            return p;
        }
        p.max_len = len;
    }
    return p;
}

}  // namespace prism::pir::pirmem
