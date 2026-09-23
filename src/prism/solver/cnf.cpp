// CNF, DIMACS, the variable map back to bitvector names, and the ProbSAT
// stochastic local search walker (roadmap 3.3 CPU reference; the CUDA kernel
// in src/cuda/probsat.cu mirrors this algorithm).

#include "prism/solver.hpp"
#include "internal.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <sstream>

namespace prism::solver {

std::string to_dimacs(const Cnf& cnf) {
    std::string out;
    out.reserve(cnf.clauses.size() * 12 + 32);
    out += "p cnf " + std::to_string(cnf.num_vars) + " " + std::to_string(cnf.clauses.size()) + "\n";
    for (const auto& c : cnf.clauses) {
        for (int l : c) { out += std::to_string(l); out += ' '; }
        out += "0\n";
    }
    return out;
}

std::optional<Cnf> parse_dimacs(std::string_view text, std::string* why) {
    Cnf cnf;
    std::size_t declared = 0;
    bool header = false;
    std::vector<int> cur;
    std::size_t s = 0;
    auto fail = [&](std::string m) -> std::optional<Cnf> {
        if (why) *why = std::move(m);
        return std::nullopt;
    };
    while (s < text.size()) {
        std::size_t e = text.find('\n', s);
        if (e == std::string_view::npos) e = text.size();
        std::string line(text.substr(s, e - s));
        s = e + 1;
        std::size_t f = line.find_first_not_of(" \t\r");
        if (f == std::string::npos || line[f] == 'c') continue;
        std::istringstream ls(line);
        if (line[f] == 'p') {
            std::string p, fmt;
            long long nv = 0, nc = 0;
            if (!(ls >> p >> fmt >> nv >> nc) || fmt != "cnf" || nv < 0 || nc < 0)
                return fail("bad DIMACS header");
            cnf.num_vars = static_cast<int>(nv);
            declared = static_cast<std::size_t>(nc);
            header = true;
            continue;
        }
        if (!header) return fail("clause before header");
        long long l;
        while (ls >> l) {
            if (l == 0) { cnf.clauses.push_back(cur); cur.clear(); continue; }
            if (std::llabs(l) > cnf.num_vars) return fail("literal out of range");
            cur.push_back(static_cast<int>(l));
        }
    }
    if (!cur.empty()) return fail("unterminated clause");
    if (cnf.clauses.size() != declared) return fail("clause count differs from header");
    return cnf;
}

namespace {

std::string bits_literal(const std::vector<bool>& bits) {  // LSB first
    const std::size_t w = bits.size();
    std::string out;
    if (w % 4 == 0) {
        out = "#x";
        for (std::size_t i = w; i >= 4; i -= 4) {
            int v = 0;
            for (std::size_t b = 0; b < 4; ++b) v |= bits[i - 4 + b] ? (1 << b) : 0;
            out += "0123456789abcdef"[v];
        }
    } else {
        out = "#b";
        for (std::size_t i = w; i-- > 0;) out += bits[i] ? '1' : '0';
    }
    return out;
}

}  // namespace

std::map<std::string, std::string> model_from_assignment(const Cnf& cnf,
                                                         const std::vector<std::int8_t>& a) {
    std::map<std::string, std::string> m;
    for (const auto& sym : cnf.symbols) {
        std::vector<bool> bits(sym.width, false);
        for (unsigned i = 0; i < sym.width && i < sym.vars.size(); ++i) {
            int v = sym.vars[i];
            // A bit the simplifier dropped is unconstrained: 0 is as good as any.
            if (v > 0 && std::size_t(v) < a.size()) bits[i] = a[std::size_t(v)] == 1;
        }
        m[sym.name] = sym.is_bool ? (bits[0] ? "true" : "false") : bits_literal(bits);
    }
    return m;
}

bool assignment_satisfies(const Cnf& cnf, const std::vector<std::int8_t>& a) {
    for (const auto& c : cnf.clauses) {
        bool sat = false;
        for (int l : c) {
            std::size_t v = std::size_t(std::abs(l));
            if (v >= a.size()) return false;
            if ((l > 0) == (a[v] == 1)) { sat = true; break; }
        }
        if (!sat) return false;
    }
    return true;
}

std::optional<std::vector<std::int8_t>> parse_sat_values(std::string_view out, int num_vars) {
    std::vector<std::int8_t> a(std::size_t(num_vars) + 1, 0);
    bool any = false, done = false;
    std::size_t s = 0;
    while (s < out.size()) {
        std::size_t e = out.find('\n', s);
        if (e == std::string_view::npos) e = out.size();
        auto line = out.substr(s, e - s);
        s = e + 1;
        if (line.size() < 2 || line[0] != 'v' || line[1] != ' ') continue;
        any = true;
        std::istringstream ls{std::string(line.substr(2))};
        long long l;
        while (ls >> l) {
            if (l == 0) { done = true; break; }
            auto v = std::llabs(l);
            if (v > num_vars) return std::nullopt;
            a[std::size_t(v)] = l > 0 ? 1 : 0;
        }
    }
    if (!any || !done) return std::nullopt;
    return a;
}

// ---------------------------------------------------------------- ProbSAT
namespace {

struct Rng {
    std::uint64_t s;
    explicit Rng(std::uint64_t seed) : s(seed ? seed : 0x9e3779b97f4a7c15ull) {}
    std::uint64_t next() {  // splitmix64
        std::uint64_t z = (s += 0x9e3779b97f4a7c15ull);
        z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
        z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
        return z ^ (z >> 31);
    }
    double unit() { return double(next() >> 11) * (1.0 / 9007199254740992.0); }
    std::size_t below(std::size_t n) { return std::size_t(next() % n); }
};

inline std::size_t lit_index(int l) { return 2 * std::size_t(std::abs(l)) + (l < 0 ? 1 : 0); }

}  // namespace

SlsResult probsat(const Cnf& cnf, const SlsOptions& opt) {
    SlsResult r;
    const std::size_t nv = std::size_t(std::max(cnf.num_vars, 0));
    Rng rng(opt.seed);
    r.assignment.assign(nv + 1, 0);
    for (std::size_t v = 1; v <= nv; ++v) r.assignment[v] = std::int8_t(rng.next() & 1);
    std::size_t maxlen = 0;
    for (const auto& c : cnf.clauses) {
        if (c.empty()) return r;  // the empty clause: nothing to find
        maxlen = std::max(maxlen, c.size());
    }
    // ProbSAT (Balint & Schoening 2012) polynomial break-only distribution.
    double cb = opt.cb;
    if (cb <= 0) cb = maxlen <= 3 ? 2.38 : maxlen <= 4 ? 3.0 : maxlen <= 5 ? 3.7 : 5.4;
    const double eps = opt.eps;

    const std::size_t nc = cnf.clauses.size();
    std::vector<std::vector<std::uint32_t>> occ(2 * nv + 2);
    for (std::size_t i = 0; i < nc; ++i)
        for (int l : cnf.clauses[i]) occ[lit_index(l)].push_back(std::uint32_t(i));
    std::vector<std::uint32_t> ntrue(nc, 0);
    std::vector<std::int64_t> where(nc, -1);
    std::vector<std::uint32_t> unsat;
    auto is_true = [&](int l) { return (l > 0) == (r.assignment[std::size_t(std::abs(l))] == 1); };
    for (std::size_t i = 0; i < nc; ++i) {
        for (int l : cnf.clauses[i]) ntrue[i] += is_true(l) ? 1u : 0u;
        if (ntrue[i] == 0) { where[i] = std::int64_t(unsat.size()); unsat.push_back(std::uint32_t(i)); }
    }
    auto drop = [&](std::uint32_t c) {
        auto pos = std::size_t(where[c]);
        std::uint32_t last = unsat.back();
        unsat[pos] = last;
        where[last] = std::int64_t(pos);
        unsat.pop_back();
        where[c] = -1;
    };
    // Lookup table (eps + break)^-cb for small breaks.
    std::vector<double> table(64);
    for (std::size_t b = 0; b < table.size(); ++b) table[b] = std::pow(eps + double(b), -cb);
    std::vector<double> probs;
    const double t0 = detail::now_s();
    while (!unsat.empty()) {
        if (opt.max_flips && r.flips >= opt.max_flips) return r;
        if ((r.flips & 1023u) == 0) {
            if (opt.stop && opt.stop->load(std::memory_order_relaxed)) return r;
            if (opt.timeout_s > 0 && detail::now_s() - t0 > opt.timeout_s) return r;
        }
        const auto& cl = cnf.clauses[unsat[rng.below(unsat.size())]];
        probs.resize(cl.size());
        double sum = 0;
        for (std::size_t j = 0; j < cl.size(); ++j) {
            // Flipping var(cl[j]) breaks every clause whose only true literal
            // is var's current (true) literal, i.e. -cl[j].
            std::size_t brk = 0;
            for (auto c : occ[lit_index(-cl[j])]) brk += ntrue[c] == 1 ? 1 : 0;
            probs[j] = brk < table.size() ? table[brk] : std::pow(eps + double(brk), -cb);
            sum += probs[j];
        }
        double pick = rng.unit() * sum;
        std::size_t j = 0;
        for (; j + 1 < cl.size(); ++j) {
            if (pick < probs[j]) break;
            pick -= probs[j];
        }
        const int lit = cl[j];  // currently false; becomes true
        const auto v = std::size_t(std::abs(lit));
        r.assignment[v] = std::int8_t(lit > 0 ? 1 : 0);
        ++r.flips;
        for (auto c : occ[lit_index(lit)]) {
            if (ntrue[c]++ == 0) drop(c);
        }
        for (auto c : occ[lit_index(-lit)]) {
            if (--ntrue[c] == 0) { where[c] = std::int64_t(unsat.size()); unsat.push_back(c); }
        }
    }
    r.found = true;
    return r;
}

}  // namespace prism::solver
