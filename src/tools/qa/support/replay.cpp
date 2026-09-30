#include "replay.hpp"

#include "proctree.hpp"
#include "pyrandom.hpp"

#include "prism/regex.hpp"

#include <algorithm>
#include <charconv>
#include <cstdlib>
#include <fstream>
#include <map>
#include <mutex>
#include <set>

#include <unistd.h>

namespace prism::qa {

const std::vector<std::string> SAN_FLAGS = {"-g", "-O0", "-w", "-fsanitize=undefined,address",
                                            "-fno-sanitize-recover=all"};
const std::vector<std::pair<std::string, std::string>> SAN_ENV = {
    {"ASAN_OPTIONS", "detect_leaks=0:abort_on_error=0:halt_on_error=1"},
    {"UBSAN_OPTIONS", "print_stacktrace=0:halt_on_error=1"},
};

namespace {

const std::map<std::string, std::pair<i128, i128>>& int_types() {
    static const std::map<std::string, std::pair<i128, i128>> k = [] {
        std::map<std::string, std::pair<i128, i128>> m;
        struct Row {
            std::vector<std::string> names;
            int bits;
            bool sign;
        };
        const std::vector<Row> rows = {
            {{"int", "signed", "signed int", "int32_t"}, 32, true},
            {{"unsigned", "unsigned int", "uint32_t"}, 32, false},
            {{"long", "long int", "signed long", "long long", "long long int", "int64_t", "ssize_t"}, 64, true},
            {{"unsigned long", "unsigned long int", "unsigned long long", "uint64_t", "size_t", "std::size_t"}, 64,
             false},
            {{"short", "short int", "signed short", "int16_t"}, 16, true},
            {{"unsigned short", "uint16_t"}, 16, false},
            {{"char", "signed char", "int8_t"}, 8, true},
            {{"unsigned char", "uint8_t"}, 8, false},
            {{"bool", "_Bool"}, 1, false},
        };
        for (const auto& r : rows)
            for (const auto& n : r.names) {
                i128 one = 1;
                m[n] = r.sign ? std::pair<i128, i128>{-(one << (r.bits - 1)), (one << (r.bits - 1)) - 1}
                              : std::pair<i128, i128>{0, (one << r.bits) - 1};
            }
        return m;
    }();
    return k;
}

std::string collapse_ws(const std::string& s) {
    std::string out;
    bool pending = false;
    for (char c : s) {
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\v' || c == '\f') {
            pending = !out.empty();
            continue;
        }
        if (pending) out += ' ';
        pending = false;
        out += c;
    }
    return out;
}

std::string strip(const std::string& s) {
    std::size_t a = 0, b = s.size();
    auto ws = [](char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\v' || c == '\f'; };
    while (a < b && ws(s[a])) ++a;
    while (b > a && ws(s[b - 1])) --b;
    return s.substr(a, b - a);
}

std::string re_escape(const std::string& s) {
    std::string out;
    for (char c : s) {
        if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '_')) out += '\\';
        out += c;
    }
    return out;
}

// Python floor modulo for a positive divisor
i128 floor_mod(i128 a, i128 m) {
    i128 r = a % m;
    return r < 0 ? r + m : r;
}

std::string lower(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string upper(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return s;
}

bool is_cxx(const std::string& lang) { return upper(lang).rfind("C+", 0) == 0; }

void write_file(const fs::path& p, const std::string& text) {
    std::ofstream o(p, std::ios::binary | std::ios::trunc);
    o << text;
}

std::vector<std::string> sandboxed(std::vector<std::string> cmd, const fs::path& work) {
    if (!bwrap_ok()) return cmd;
    std::vector<std::string> w = {"bwrap", "--ro-bind", "/", "/", "--dev", "/dev", "--proc", "/proc",
                                  "--tmpfs", "/tmp", "--ro-bind", work.string(), work.string(),
                                  "--unshare-all", "--die-with-parent", "--new-session"};
    w.insert(w.end(), cmd.begin(), cmd.end());
    return w;
}

}  // namespace

std::string path_key(const std::string& ident) {
    std::string out;
    for (char c : ident) {
        if (c == '/') out += "__";
        else out += c;
    }
    return out;
}

std::string py_float(double v) {
    char buf[64];
    auto res = std::to_chars(buf, buf + sizeof buf, v);
    std::string s(buf, res.ptr);
    if (s.find_first_of(".eEn") == std::string::npos) s += ".0";
    return s;
}

std::string utf8_clean(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    const auto* p = reinterpret_cast<const unsigned char*>(s.data());
    std::size_t n = s.size(), i = 0;
    while (i < n) {
        unsigned char c = p[i];
        std::size_t len = c < 0x80 ? 1 : (c >> 5) == 0x6 ? 2 : (c >> 4) == 0xE ? 3 : (c >> 3) == 0x1E ? 4 : 0;
        bool ok = len > 0 && i + len <= n;
        for (std::size_t k = 1; ok && k < len; ++k) ok = (p[i + k] & 0xC0) == 0x80;
        if (ok && len == 2) ok = c >= 0xC2;
        if (ok && len == 3) {
            unsigned cp = ((c & 0xFu) << 12) | ((p[i + 1] & 0x3Fu) << 6) | (p[i + 2] & 0x3Fu);
            ok = cp >= 0x800 && !(cp >= 0xD800 && cp <= 0xDFFF);
        }
        if (ok && len == 4) {
            unsigned cp = ((c & 0x7u) << 18) | ((p[i + 1] & 0x3Fu) << 12) | ((p[i + 2] & 0x3Fu) << 6) |
                          (p[i + 3] & 0x3Fu);
            ok = cp >= 0x10000 && cp <= 0x10FFFF;
        }
        if (!ok) {
            out += "\xEF\xBF\xBD";
            ++i;
            continue;
        }
        out.append(s, i, len);
        i += len;
    }
    return out;
}

ojson int_json(i128 v) {
    if (v >= 0 && v > static_cast<i128>(INT64_MAX)) return ojson(static_cast<std::uint64_t>(v));
    return ojson(static_cast<std::int64_t>(v));
}

std::string norm_type(const std::string& t) {
    static const Regex qual(R"(\b(const|volatile|register|static|inline|constexpr|extern)\b)");
    std::string out;
    std::size_t last = 0;
    for (const auto& m : qual.finditer(t)) {
        out.append(t, last, static_cast<std::size_t>(m.spans[0].first) - last);
        out += ' ';
        last = static_cast<std::size_t>(m.spans[0].second);
    }
    out.append(t, last, std::string::npos);
    for (auto& c : out)
        if (c == '&') c = ' ';
    return collapse_ws(out);
}

std::optional<std::pair<i128, i128>> type_range(const std::string& t0) {
    std::string t = norm_type(t0);
    const auto& m = int_types();
    if (auto it = m.find(t); it != m.end()) return it->second;
    static const Regex bitint(R"(^(unsigned\s+)?_BitInt\((\d+)\)\z)");
    if (auto mm = bitint.search_match(t)) {
        int bits = std::atoi(mm->group(2).c_str());
        if (bits < 1 || bits > 126) return std::nullopt;
        i128 one = 1;
        if (mm->groups.size() > 1 && mm->groups[1]) return std::pair<i128, i128>{0, (one << bits) - 1};
        return std::pair<i128, i128>{-(one << (bits - 1)), (one << (bits - 1)) - 1};
    }
    return std::nullopt;
}

std::optional<std::pair<std::string, Params>> find_signature(const std::string& text0, const std::string& fn) {
    const std::string text = utf8_clean(text0);
    Regex re(R"((?m)^[ \t]*(?:\[\[[^\]]*\]\]\s*)*(?P<ret>[\w:<>\s\*&]*?)\b)" + re_escape(fn) +
             R"(\s*\((?P<params>[^()]*)\)\s*(?:->\s*[\w:]+\s*)?(?:noexcept\s*)?\{)");
    auto m = re.search_match(text);
    if (!m) return std::nullopt;
    Params params;
    std::string raw = strip(m->named("params"));
    if (!raw.empty() && raw != "void") {
        static const Regex pre(R"(^(?P<t>.*?[\s\*&])(?P<n>\w+)\z)");
        std::size_t a = 0;
        for (;;) {
            std::size_t b = raw.find(',', a);
            std::string p = strip(raw.substr(a, b == std::string::npos ? std::string::npos : b - a));
            auto pm = pre.search_match(p);
            if (!pm) return std::nullopt;
            params.emplace_back(strip(pm->named("t")), pm->named("n"));
            if (b == std::string::npos) break;
            a = b + 1;
        }
    }
    return std::pair<std::string, Params>{strip(m->named("ret")), params};
}

std::string c_literal(i128 v, const std::string& typ) {
    const i128 one = 1;
    std::string lit;
    if (v == -(one << 63)) lit = "(-9223372036854775807LL - 1)";
    else if (v < 0) lit = "(" + i128_str(v) + "LL)";
    else if (v > (one << 63) - 1) lit = i128_str(v) + "ULL";
    else lit = i128_str(v) + "LL";
    return "(" + norm_type(typ) + ")" + lit;
}

std::optional<Params> scalar_params(const Task& task, const std::string& fn) {
    auto sig = find_signature(read_text(task.source), fn);
    if (!sig) return std::nullopt;
    for (const auto& [t, n] : sig->second)
        if (!type_range(t)) return std::nullopt;
    return sig->second;
}

std::string driver_source(const Task& task, const std::string& fn, const Params& params,
                          const std::vector<std::vector<i128>>& rows) {
    std::error_code ec;
    fs::path src = fs::weakly_canonical(fs::absolute(task.source, ec), ec);
    std::string inc = ojson(src.string()).dump(-1, ' ', false, ojson::error_handler_t::replace);
    std::vector<std::string> lines = {"#include " + inc, "#include <stddef.h>"};
    if (!params.empty()) {
        std::string fields;
        for (std::size_t i = 0; i < params.size(); ++i) {
            if (i) fields += ' ';
            fields += norm_type(params[i].first) + " p" + std::to_string(i) + ";";
        }
        lines.push_back("static struct { " + fields + " } prism_inputs[] = {");
        for (const auto& row : rows) {
            std::string vals;
            for (std::size_t i = 0; i < row.size() && i < params.size(); ++i) {
                if (i) vals += ", ";
                vals += c_literal(row[i], params[i].first);
            }
            lines.push_back("    { " + vals + " },");
        }
        lines.push_back("};");
        std::string call;
        for (std::size_t i = 0; i < params.size(); ++i) {
            if (i) call += ", ";
            call += "prism_inputs[k].p" + std::to_string(i);
        }
        lines.insert(lines.end(), {"int main(void) {",
                                   "    for (size_t k = 0; k < sizeof prism_inputs / sizeof prism_inputs[0]; ++k)",
                                   "        (void)" + fn + "(" + call + ");", "    return 0;", "}"});
    } else {
        lines.insert(lines.end(), {"int main(void) {", "    (void)" + fn + "();", "    return 0;", "}"});
    }
    std::string out;
    for (std::size_t i = 0; i < lines.size(); ++i) {
        if (i) out += '\n';
        out += lines[i];
    }
    return out + "\n";
}

bool bwrap_ok() {
    static std::once_flag once;
    static bool ok = false;
    std::call_once(once, [] {
        std::string exe = which("bwrap");
        if (exe.empty()) return;
        RunOpts o;
        o.timeout_s = 20;
        auto r = run_tree({exe, "--ro-bind", "/", "/", "--dev", "/dev", "--proc", "/proc", "--unshare-all",
                           "--die-with-parent", "true"},
                          o);
        ok = !r.start_failed && !r.timed_out && r.rc == 0;
    });
    return ok;
}

std::pair<std::string, std::string> sanitizer_compilers() {
    static std::once_flag once;
    static std::pair<std::string, std::string> cc;
    std::call_once(once, [] {
        const char* e1 = std::getenv("CC_SAN");
        const char* e2 = std::getenv("CXX_SAN");
        if (e1 && *e1 && e2 && *e2) {
            cc = {e1, e2};
            return;
        }
        cc = {"clang", "clang++"};
        for (const auto& [c, cxx] : std::vector<std::pair<std::string, std::string>>{{"clang", "clang++"},
                                                                                     {"gcc", "g++"}}) {
            if (which(c).empty()) continue;
            std::string tmpl = (fs::temp_directory_path() / "prism-sanprobe-XXXXXX").string();
            if (!::mkdtemp(tmpl.data())) continue;
            fs::path d = tmpl;
            write_file(d / "probe.c", "int main(void) { return 0; }\n");
            std::vector<std::string> cmd = {c, (d / "probe.c").string()};
            cmd.insert(cmd.end(), SAN_FLAGS.begin(), SAN_FLAGS.end());
            cmd.insert(cmd.end(), {"-o", (d / "probe").string()});
            RunOpts o;
            o.timeout_s = 120;
            auto r = run_tree(cmd, o);
            std::error_code ec;
            fs::remove_all(d, ec);
            if (!r.start_failed && !r.timed_out && r.rc == 0) {
                cc = {c, cxx};
                break;
            }
        }
    });
    return cc;
}

std::vector<std::string> compiler_for(const std::string& lang) {
    auto [cc, cxx] = sanitizer_compilers();
    std::string u = upper(lang);
    if (u == "C++" || u == "CXX" || u == "CPP") return {cxx, "-std=c++23", "-x", "c++"};
    return {cc, "-std=c2x", "-x", "c"};
}

Exec run_sanitized(const Task& task, const std::string& fn, const Params& params,
                   const std::vector<std::vector<i128>>& rows, const fs::path& work, double timeout) {
    std::error_code ec;
    fs::create_directories(work, ec);
    fs::path drv = work / (is_cxx(task.lang) ? "driver.cpp" : "driver.c");
    write_file(drv, driver_source(task, fn, params, rows));
    fs::path exe = work / "driver.bin";
    std::vector<std::string> cc = compiler_for(task.lang);
    cc.insert(cc.end(), {drv.string(), "-x", "none"});
    cc.insert(cc.end(), SAN_FLAGS.begin(), SAN_FLAGS.end());
    cc.insert(cc.end(), {"-o", exe.string()});
    if (!is_cxx(task.lang)) cc.push_back("-lm");
    RunOpts co;
    co.timeout_s = 180;
    auto r = run_tree(cc, co);
    if (r.start_failed) return {"compile-error", utf8_clean(r.err)};
    if (r.timed_out) return {"compile-error", "timed out after 180 seconds"};
    if (r.rc != 0) return {"compile-error", py_tail(utf8_clean(r.err.empty() ? r.out : r.err), 800)};
    RunOpts ro;
    ro.timeout_s = timeout;
    ro.env = SAN_ENV;
    auto x = run_tree(sandboxed({exe.string()}, work), ro);
    if (x.timed_out) return {"timeout", ">" + py_float(timeout) + "s"};
    std::string err = utf8_clean(x.err);
    // std::terminate (an exception escaping main or a noexcept function) is a
    // property violation for PRISM (roadmap 2.6), reported by the C++ runtime
    static const Regex hit_re(
        R"(runtime error: .*|ERROR: AddressSanitizer: \S+.*|terminate called .*|pure virtual method called)");
    if (auto hit = hit_re.search_match(err)) return {"ub", py_head(hit->group(0), 300)};
    if (x.rc != 0) return {"crash", "exit " + std::to_string(x.rc) + ": " + py_tail(err, 300)};
    return {"clean", ""};
}

Exec run_program(const Task& task, const fs::path& work, double timeout) {
    std::error_code ec;
    fs::create_directories(work, ec);
    fs::path exe = work / "prog.bin";
    std::string std = !task.std.empty() ? task.std : (is_cxx(task.lang) ? "c++17" : "c17");
    auto [cc, cxx] = sanitizer_compilers();
    std::vector<std::string> cmd = {is_cxx(task.lang) ? cxx : cc, "-std=" + std, task.source.string()};
    cmd.insert(cmd.end(), SAN_FLAGS.begin(), SAN_FLAGS.end());
    cmd.insert(cmd.end(), {"-UNDEBUG", "-o", exe.string()});
    RunOpts co;
    co.timeout_s = 300;
    auto r = run_tree(cmd, co);
    if (r.start_failed) return {"compile-error", utf8_clean(r.err)};
    if (r.timed_out) return {"compile-error", "timed out after 300 seconds"};
    if (r.rc != 0) return {"compile-error", py_tail(utf8_clean(r.err.empty() ? r.out : r.err), 400)};
    RunOpts ro;
    ro.timeout_s = timeout;
    ro.env = SAN_ENV;
    auto x = run_tree(sandboxed({exe.string()}, work), ro);
    if (x.timed_out) return {"timeout", ">" + py_float(timeout) + "s"};
    std::string err = utf8_clean(x.err);
    static const Regex hit_re(
        R"(runtime error: .*|ERROR: AddressSanitizer: \S+.*|Assertion `.*' failed|terminate called .*)");
    if (auto hit = hit_re.search_match(err)) return {"ub", py_head(hit->group(0), 300)};
    if (x.rc != 0) return {"crash", "exit " + std::to_string(x.rc) + ": " + py_tail(err, 300)};
    return {"clean", ""};
}

std::vector<i128> edge_values(i128 lo, i128 hi) {
    const i128 one = 1;
    std::set<i128> cand = {lo,    lo + 1, -1000000, -46341, -1000,    -64,       -33,       -32,    -31,
                           -2,    -1,     0,        1,      2,        3,         7,         8,      16,
                           31,    32,     33,       63,     64,       255,       256,       1000,   46340,
                           46341, 65535,  65536,    one << 30, hi - 1, hi};
    std::vector<i128> out;
    for (i128 v : cand)
        if (lo <= v && v <= hi) out.push_back(v);
    return out;
}

std::vector<std::vector<i128>> input_grid(const Params& params, int n_random, long long seed) {
    std::vector<std::pair<i128, i128>> ranges;
    for (const auto& [t, n] : params) ranges.push_back(type_range(t).value_or(std::pair<i128, i128>{0, 0}));
    std::vector<std::vector<i128>> rows = {{}};
    for (const auto& [lo, hi] : ranges) {
        std::vector<i128> ev = edge_values(lo, hi);
        if (rows.size() * ev.size() > 20000) {
            std::size_t step = std::max<std::size_t>(1, ev.size() / 8);
            std::vector<i128> thin;
            for (std::size_t i = 0; i < ev.size(); i += step) thin.push_back(ev[i]);
            ev = thin;
        }
        std::vector<std::vector<i128>> next;
        next.reserve(rows.size() * ev.size());
        for (const auto& r : rows)
            for (i128 v : ev) {
                auto row = r;
                row.push_back(v);
                next.push_back(std::move(row));
            }
        rows = std::move(next);
    }
    PyRandom rng(seed);
    for (int i = 0; i < (params.empty() ? 0 : n_random); ++i) {
        std::vector<i128> row;
        for (const auto& [lo, hi] : ranges) {
            double pick = rng.random();
            if (pick < 0.5) row.push_back(rng.randint(lo, hi));
            else row.push_back(std::max(lo, std::min(hi, rng.randint(-2000, 2000))));
        }
        rows.push_back(std::move(row));
    }
    return rows;
}

std::vector<std::pair<std::string, i128>> parse_cex(const std::string& cex) {
    static const Regex re(R"((\w+)\s*=\s*(#x[0-9a-fA-F]+|#b[01]+|-?0[xX][0-9a-fA-F]+|-?\d+|true|false))");
    std::vector<std::pair<std::string, i128>> vals;
    auto set = [&](const std::string& k, i128 v) {
        for (auto& [kk, vv] : vals)
            if (kk == k) {
                vv = v;
                return;
            }
        vals.emplace_back(k, v);
    };
    // digits into a value modulo 2^128 (bit patterns wider than any C scalar
    // keep their low bits; every scalar range spans a power of two)
    auto digits = [](const std::string& s, unsigned base) {
        unsigned __int128 v = 0;
        for (char c : s) {
            unsigned d = std::isdigit(static_cast<unsigned char>(c)) ? static_cast<unsigned>(c - '0')
                                                                     : static_cast<unsigned>(std::tolower(c) - 'a' + 10);
            v = v * base + d;
        }
        return v;
    };
    for (const auto& m : re.finditer(utf8_clean(cex))) {
        std::string name = m.group(1), v = m.group(2);
        if (v == "true" || v == "false") {
            set(name, v == "true" ? 1 : 0);
            continue;
        }
        bool neg = false;
        std::string body = v;
        if (!body.empty() && body[0] == '-') {
            neg = true;
            body = body.substr(1);
        }
        unsigned __int128 u;
        if (body.rfind("#x", 0) == 0) u = digits(body.substr(2), 16);
        else if (body.rfind("#b", 0) == 0) u = digits(body.substr(2), 2);
        else if (body.size() > 1 && body[0] == '0' && (body[1] == 'x' || body[1] == 'X')) u = digits(body.substr(2), 16);
        else u = digits(body, 10);
        i128 s = static_cast<i128>(u);
        set(name, neg ? -s : s);
    }
    return vals;
}

ojson replay(const Task& task, const std::string& fn, const std::string& cex, const fs::path& work) {
    ojson rec = ojson::object();
    if (task.origin != "prism") {
        rec["replay"] = "unsupported";
        rec["why"] = task.origin + " task: whole program with nondet inputs";
        return rec;
    }
    auto params = scalar_params(task, fn);
    if (!params) {
        rec["replay"] = "unsupported";
        rec["why"] = "non-scalar parameters";
        return rec;
    }
    auto vals = parse_cex(cex);
    if (!params->empty() && vals.empty()) {
        rec["replay"] = "no-cex";
        rec["why"] = "FAILED without parseable counterexample";
        return rec;
    }
    auto get = [&](const std::string& n) -> std::optional<i128> {
        for (const auto& [k, v] : vals)
            if (k == n) return v;
        return std::nullopt;
    };
    std::vector<std::string> missing;
    std::vector<i128> row;
    for (const auto& [t, n] : *params) {
        if (!get(n)) missing.push_back(n);
        auto [lo, hi] = type_range(t).value_or(std::pair<i128, i128>{0, 0});
        i128 v = get(n).value_or(0);
        // The solver reports bit patterns; reinterpret into the C type's range.
        i128 span = hi - lo + 1;
        if (v > hi || v < lo) v = floor_mod(v - lo, span) + lo;
        row.push_back(v);
    }
    Exec res = run_sanitized(task, fn, *params, {row}, work / "replay" / path_key(task.ident) / fn);
    rec["replay"] = res.outcome == "ub" ? "replayed" : "not-replayed";
    rec["outcome"] = res.outcome;
    rec["detail"] = res.detail;
    ojson inputs = ojson::object();
    for (std::size_t i = 0; i < params->size(); ++i) inputs[(*params)[i].second] = int_json(row[i]);
    rec["inputs"] = inputs;
    if (!missing.empty()) rec["missing_params"] = missing;
    if (task.sanitizer_blind.count(fn) && res.outcome == "clean") rec["replay"] = "sanitizer-blind";
    return rec;
}

}  // namespace prism::qa
