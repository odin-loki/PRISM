#include "gen_random.hpp"

#include "support/pyrandom.hpp"
#include "support/task.hpp"

#include <algorithm>
#include <string>
#include <vector>

namespace prism::qa {

namespace {

const std::vector<std::string> TYPES = {"int",   "int",           "int",         "int",           "unsigned",
                                        "short", "unsigned short", "signed char", "unsigned char", "long long"};
const std::vector<long long> EDGE_CONSTS = {0,   1,    2,     3,     7,     8,       15,     16,     31,     32,
                                            100, 255, 1000, 46340, 46341, 65535, 65536, 1000000, 2147483647};

struct Var {
    std::string name;
    std::string typ;
};

std::string num(i128 v) { return i128_str(v); }

std::string join(const std::vector<std::string>& v, const std::string& sep) {
    std::string out;
    for (std::size_t i = 0; i < v.size(); ++i) {
        if (i) out += sep;
        out += v[i];
    }
    return out;
}

// In-house generator for the C subset PRISM claims to model: int / unsigned /
// short / char / long long parameters, locals, + - * / % << >> & | ^ ~ !
// unary minus, casts, ternaries, if/else, early-return guards, bounded for
// loops and compound assignment. Guards and masks are biased so that many
// functions are UB-free (so PRISM proves them) while a minority hide UB
// behind edge cases.
class Gen {
public:
    explicit Gen(PyRandom& r) : r(r) {}
    virtual ~Gen() = default;

    std::string konst() {
        double x = r.random();
        if (x < 0.6) return num(r.randint(0, 20));
        if (x < 0.9) return num(r.choice(EDGE_CONSTS));
        return "(" + num(-r.randint(1, 100)) + ")";
    }

    std::string leaf(const std::vector<Var>& env) {
        if (!env.empty() && r.random() < 0.7) return r.choice(env).name;
        return konst();
    }

    std::string expr(const std::vector<Var>& env, int depth) {
        if (depth <= 0 || r.random() < 0.25) return leaf(env);
        double k = r.random();
        std::string a = expr(env, depth - 1);
        if (k < 0.08) return "(-" + a + ")";
        if (k < 0.12) return "(~" + a + ")";
        if (k < 0.15) return "(!" + a + ")";
        if (k < 0.23) {
            static const std::vector<std::string> casts = {"int",       "short",     "unsigned",      "unsigned char",
                                                           "signed char", "long long", "unsigned short"};
            std::string t = r.choice(casts);
            return "((" + t + ")" + a + ")";
        }
        if (k < 0.30) {
            std::string c = cond(env, depth - 1);
            std::string e = expr(env, depth - 1);
            return "(" + c + " ? " + a + " : " + e + ")";
        }
        std::string b = expr(env, depth - 1);
        static const std::vector<std::string> ops = {"+", "+", "-", "-", "*", "*", "/", "%", "<<", ">>", "&", "|", "^"};
        std::string op = r.choice(ops);
        if (op == "/" || op == "%") {
            double s = r.random();
            if (s < 0.45) b = "(" + b + " | 1)";
            else if (s < 0.75) b = "((" + b + " & 15) + 1)";
        } else if (op == "<<" || op == ">>") {
            double s = r.random();
            if (s < 0.5) {
                static const std::vector<long long> m = {7, 15, 31};
                b = "(" + b + " & " + num(r.choice(m)) + ")";
            } else if (s < 0.8) {
                b = num(r.randint(0, 31));
            }
            if (op == "<<" && r.random() < 0.6) {
                static const std::vector<long long> m = {1, 3, 15, 255, 65535};
                a = "(" + a + " & " + num(r.choice(m)) + ")";
            }
        } else if (op == "*" && r.random() < 0.6) {
            static const std::vector<long long> m = {255, 1023, 65535};
            a = "(" + a + " & " + num(r.choice(m)) + ")";
        }
        return "(" + a + " " + op + " " + b + ")";
    }

    std::string cond(const std::vector<Var>& env, int depth) {
        std::string a = expr(env, std::max(0, depth - 1));
        static const std::vector<std::string> ops = {"<", ">", "<=", ">=", "==", "!="};
        std::string op = r.choice(ops);
        std::string c = konst();
        return "(" + a + " " + op + " " + c + ")";
    }

    std::string guard(const Var& v, const std::string& ind) {
        static const std::vector<long long> los = {0, -10, -100, -1000, -46340};
        static const std::vector<long long> his = {10, 100, 1000, 46340, 65535};
        std::string lo = num(r.choice(los));
        std::string hi = num(r.choice(his));
        return ind + "if (" + v.name + " < " + lo + " || " + v.name + " > " + hi + ") return 0;";
    }

    std::vector<std::string> stmt(const std::vector<Var>& env, const std::vector<Var>& locals, const std::string& ind,
                                  int depth, bool loop_ok) {
        double k = r.random();
        if (k < 0.35 && !locals.empty()) {
            const Var& v = r.choice(locals);
            static const std::vector<std::string> ops = {"=",  "+=", "-=", "*=", "^=", "|=",
                                                         "&=", "/=", "%=", "<<=", ">>="};
            std::string op = r.choice(ops);
            std::string rhs = expr(env, 2);
            if (op == "/=" || op == "%=") rhs = "((" + rhs + " & 7) + 1)";
            if (op == "<<=" || op == ">>=") rhs = "(" + rhs + " & 15)";
            return {ind + v.name + " " + op + " " + rhs + ";"};
        }
        if (k < 0.45 && !locals.empty()) {
            const Var& v = r.choice(locals);
            static const std::vector<std::string> inc = {"++", "--"};
            return {ind + v.name + r.choice(inc) + ";"};
        }
        if (k < 0.70 && depth > 0) {
            std::vector<std::string> out = {ind + "if " + cond(env, 2) + " {"};
            auto n = r.randint(1, 2);
            for (i128 i = 0; i < n; ++i) {
                auto s = stmt(env, locals, ind + "    ", depth - 1, loop_ok);
                out.insert(out.end(), s.begin(), s.end());
            }
            if (r.random() < 0.5) {
                out.push_back(ind + "} else {");
                auto s = stmt(env, locals, ind + "    ", depth - 1, loop_ok);
                out.insert(out.end(), s.begin(), s.end());
            }
            out.push_back(ind + "}");
            return out;
        }
        if (k < 0.85 && depth > 0 && loop_ok && !locals.empty()) {
            std::string i = "i" + num(r.randint(0, 999));
            std::string bound = num(r.randint(1, 6));
            const Var& v = r.choice(locals);
            std::vector<Var> inner = env;
            inner.push_back({i, "int"});
            std::string line0 = ind + "for (int " + i + " = 0; " + i + " < " + bound + "; " + i + "++) {";
            std::string line1 = ind + "    " + v.name + " += " + expr(inner, 1) + ";";
            return {line0, line1, ind + "}"};
        }
        if (env.empty()) return {};
        const Var& g = r.choice(env);
        return {guard(g, ind)};
    }

    virtual std::string function(const std::string& name) {
        std::vector<Var> params;
        auto np = r.randint(1, 3);
        for (i128 i = 0; i < np; ++i) params.push_back({"p" + num(i), r.choice(TYPES)});
        std::vector<Var> env = params;
        std::vector<std::string> body;
        for (const auto& p : params)
            if (r.random() < 0.6) body.push_back(guard(p, "    "));
        std::vector<Var> locals;
        auto nl = r.randint(1, 3);
        for (i128 j = 0; j < nl; ++j) {
            static const std::vector<std::string> lt = {"int", "int", "unsigned", "long long"};
            std::string t = r.choice(lt);
            Var v{"v" + num(j), t};
            body.push_back("    " + t + " " + v.name + " = " + expr(env, 2) + ";");
            env.push_back(v);
            locals.push_back(v);
        }
        auto ns = r.randint(1, 4);
        for (i128 i = 0; i < ns; ++i) {
            auto s = stmt(env, locals, "    ", 2, true);
            body.insert(body.end(), s.begin(), s.end());
        }
        body.push_back("    return (int)(" + expr(env, 2) + ");");
        return signature(name, params) + join(body, "\n") + "\n}\n";
    }

protected:
    PyRandom& r;

    static std::string signature(const std::string& name, const std::vector<Var>& params) {
        std::vector<std::string> sig;
        for (const auto& p : params) sig.push_back(p.typ + " " + p.name);
        return "int " + name + "(" + join(sig, ", ") + ") {\n";
    }
};

// Pointer programs for the pir memory model (docs/PIR.md "Memory model"): a
// stack or heap int array, indexed reads/writes, pointer walks, memcpy between
// arrays, free. Indices are masked or reduced so that most functions are
// memory-safe; a minority reach one past the end or use the heap array after
// free. Parameters stay scalar so functions can be driven.
class PtrGen : public Gen {
public:
    using Gen::Gen;

    std::string index(const std::vector<Var>& env, long long n, bool bug) {
        std::string e = expr(env, 1);
        if (bug) return "((unsigned)(" + e + ") % " + std::to_string(n + 1) + "u)";
        if ((n & (n - 1)) == 0) return "((" + e + ") & " + std::to_string(n - 1) + ")";
        return "((unsigned)(" + e + ") % " + std::to_string(n) + "u)";
    }

    std::string function(const std::string& name) override {
        static const std::vector<std::string> pt = {"int", "int", "unsigned", "short"};
        std::vector<Var> params;
        auto np = r.randint(1, 3);
        for (i128 i = 0; i < np; ++i) params.push_back({"p" + num(i), r.choice(pt)});
        std::vector<Var> env = params;
        std::vector<std::string> body;
        for (const auto& p : params)
            if (r.random() < 0.5) body.push_back(guard(p, "    "));
        long long n = static_cast<long long>(r.randint(2, 8));
        long long m = static_cast<long long>(r.randint(2, 8));
        bool heap = r.random() < 0.35;
        if (heap) {
            body.push_back("    int *a = malloc(" + std::to_string(n) + " * sizeof(int));");
            body.push_back("    if (!a) return 0;");
        } else {
            body.push_back("    int a[" + std::to_string(n) + "];");
        }
        body.push_back("    int b[" + std::to_string(m) + "];");
        body.push_back("    for (int k = 0; k < " + std::to_string(n) + "; k++) a[k] = k * " + num(r.randint(1, 9)) + ";");
        body.push_back("    for (int k = 0; k < " + std::to_string(m) + "; k++) b[k] = " + num(r.randint(0, 9)) + ";");
        body.push_back("    int v = 0;");
        auto ns = r.randint(2, 5);
        for (i128 s = 0; s < ns; ++s) {
            bool bug = r.random() < 0.12;
            double k = r.random();
            if (k < 0.35) {
                body.push_back("    v += a[" + index(env, n, bug) + "];");
            } else if (k < 0.6) {
                std::string ix = index(env, n, bug);
                body.push_back("    a[" + ix + "] = " + num(r.randint(0, 50)) + ";");
            } else if (k < 0.8) {
                body.push_back("    { int *q = a + " + index(env, n, bug) + "; v ^= *q; }");
            } else {
                long long cap = std::min(n, m) + (bug ? 1 : 0);
                body.push_back("    memcpy(b, a, ((unsigned)(" + expr(env, 1) + ") % " + std::to_string(cap + 1) +
                               "u) * sizeof(int));");
                body.push_back("    v += b[0];");
            }
        }
        if (heap) {
            body.push_back("    free(a);");
            if (r.random() < 0.1) body.push_back("    v += a[0];");
        }
        body.push_back("    return v;");
        return signature(name, params) + join(body, "\n") + "\n}\n";
    }
};

// Loops for the pir loop invariants (docs/PIR.md "Loop invariants"): heap byte
// strings and int counters whose loop bounds are parameters (up to 5000),
// filled, terminated, measured, copied, searched, compared and accumulated; a
// minority read or write one past an object, drop the terminator, divide by a
// counter that reaches 0 or overflow an accumulator. A function returns early
// unless 0 <= p0 <= LIMIT, so every run terminates and the loops still have no
// bound PRISM can unroll.
class LoopGen : public Gen {
public:
    using Gen::Gen;
    static constexpr int LIMIT = 5000;

    std::string function(const std::string& name) override {
        std::vector<std::string> body = {"    if (p0 > " + std::to_string(LIMIT) + "u) return 0;",
                                         "    unsigned n = p0 + 1;", "    int v = 0;"};
        bool bug = r.random() < 0.3;
        static const std::vector<std::string> all = {"fill", "len", "copy", "search", "sum", "rev", "count", "cmp"};
        auto nk = r.randint(1, 3);
        std::vector<std::string> kinds = r.sample(all, static_cast<std::size_t>(nk));
        std::string hazard = bug ? r.choice(kinds) : "";
        body.push_back("    char *s = malloc(n);");
        body.push_back("    if (!s) return 0;");
        // fill all bytes non-zero, then terminate
        std::string end = hazard == "fill" ? "n + 1" : "n";
        body.push_back("    for (unsigned i = 0; i < " + end + "; i++) s[i] = (char)(" + num(r.randint(1, 60)) +
                       " + (i & 7));");
        if (hazard != "len") body.push_back("    s[n - 1] = 0;");
        for (const auto& k : kinds) {
            if (k == "len") {
                body.push_back("    { unsigned k = 0; while (s[k]) k++; v += (int)(k & 255); }");
            } else if (k == "copy") {
                std::string size = hazard == "copy" ? "n - 1" : "n";
                body.push_back("    { char *d = malloc(" + size + " == 0 ? 1 : " + size +
                               "); if (!d) { free(s); return 0; }");
                body.push_back("      unsigned i = 0; while ((d[i] = s[i]) != 0) i++;");
                body.push_back("      v += d[i / 2]; free(d); }");
            } else if (k == "search") {
                std::string read = hazard == "search" ? "v += s[i];" : "if (i < n) v += s[i];";
                body.push_back("    { unsigned i = 0; for (; i < n && s[i] != (char)p1; i++) ; " + read + " }");
            } else if (k == "sum") {
                std::string grow = hazard == "sum" ? "acc = acc * 3 + s[i];" : "acc += s[i] & 63;";
                body.push_back("    { int acc = 0; for (unsigned i = 0; i < n; i++) " + grow + " v ^= acc; }");
            } else if (k == "rev") {
                std::string idx = hazard == "rev" ? "s[i]" : "s[i - 1]";
                body.push_back("    { for (unsigned i = n; i > 0; i--) v ^= " + idx + "; }");
            } else if (k == "count") {
                std::string start = hazard == "count" ? "0u" : "1u";
                body.push_back("    { unsigned x = " + start + "; for (unsigned i = 0; i < n; i++) x += 2u * (unsigned)p1;");
                body.push_back("      v += 100 / (int)(x & 1u); }");
            } else if (k == "cmp") {
                std::string off = hazard == "cmp" ? "n" : "n - 1";
                body.push_back("    { char *t = malloc(n); if (!t) { free(s); return 0; }");
                body.push_back("      for (unsigned i = 0; i < n; i++) t[i] = s[i];");
                body.push_back("      unsigned i = 0; while (i < " + off + " && s[i] == t[i]) i++;");
                body.push_back("      v += (int)(i & 7); v += t[i]; free(t); }");
            }
        }
        body.push_back("    free(s);");
        body.push_back("    return v;");
        return "int " + name + "(unsigned p0, int p1) {\n" + join(body, "\n") + "\n}\n";
    }
};

template <class G>
std::string program(long long seed, int nfuncs, const std::string& header) {
    PyRandom rng(seed);
    G g(rng);
    std::vector<std::string> parts = {header};
    for (int k = 0; k < nfuncs; ++k) parts.push_back(g.function("rf" + std::to_string(seed) + "_" + std::to_string(k)));
    return join(parts, "\n");
}

}  // namespace

std::string gen_inhouse(long long seed, int nfuncs) {
    return program<Gen>(seed, nfuncs,
                        "/* PRISM random soundness program, in-house generator, seed " + std::to_string(seed) + " */\n");
}

std::string gen_inhouse_ptr(long long seed, int nfuncs) {
    return program<PtrGen>(seed, nfuncs,
                           "/* PRISM random soundness program, pointer generator, seed " + std::to_string(seed) +
                               " */\n#include <stdlib.h>\n#include <string.h>\n");
}

std::string gen_inhouse_loop(long long seed, int nfuncs) {
    return program<LoopGen>(seed, nfuncs,
                            "/* PRISM random soundness program, loop generator, seed " + std::to_string(seed) +
                                " */\n#include <stdlib.h>\n#include <string.h>\n");
}

}  // namespace prism::qa
