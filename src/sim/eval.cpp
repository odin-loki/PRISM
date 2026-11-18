// prism::sim semantic signature: evaluate a canonical term on fixed seeded
// assignments (see include/prism/sim/symbolic_metric.hpp for the contract).

#include <algorithm>
#include <map>
#include <memory>

#include "prism/sim/symbolic_metric.hpp"

namespace prism::sim {
namespace {

struct arrv {
    bool is_const = false;
    word cv = 0;
    std::uint64_t base = 0;
    std::vector<std::pair<word, word>> over;  // sorted by key, unique
};
struct val {
    word v = 0;
    std::shared_ptr<const arrv> a;
};

word mask_of(unsigned w) { return w >= kMaxWidth ? ~word(0) : ((word(1) << w) - 1); }
word hash_word(std::uint64_t seed, word v) {
    std::uint64_t h = hash_combine(seed, static_cast<std::uint64_t>(v));
#if PRISM_HAS_INT128
    h = hash_combine(h, static_cast<std::uint64_t>(v >> 64));
    return (static_cast<word>(mix64(h ^ 0x77)) << 64) | mix64(h);
#else
    return h;
#endif
}
sword sx(word v, unsigned w) {
    if (w == 0) return 0;
    if (w >= kMaxWidth) return static_cast<sword>(v);
    const word m = word(1) << (w - 1);
    return static_cast<sword>((v ^ m) - m);
}
bool msb(word v, unsigned w) { return w > 0 && ((v >> (w - 1)) & 1) != 0; }

word elem_default(const arrv& a, word key, const sort_t& s) {
    if (a.is_const) return a.cv;
    const word h = hash_word(a.base, key);
    if (s.width == 0) return h & 1;
    return h & mask_of(s.width);
}
word array_select(const arrv& a, word key, const sort_t& s) {
    auto it = std::lower_bound(a.over.begin(), a.over.end(), key, [](const auto& p, word k) { return p.first < k; });
    if (it != a.over.end() && it->first == key) return it->second;
    return elem_default(a, key, s);
}
std::shared_ptr<const arrv> array_store(const arrv& a, word key, word v) {
    auto n = std::make_shared<arrv>(a);
    auto it = std::lower_bound(n->over.begin(), n->over.end(), key, [](const auto& p, word k) { return p.first < k; });
    if (it != n->over.end() && it->first == key) it->second = v;
    else n->over.insert(it, {key, v});
    return n;
}
bool array_equal(const arrv& a, const arrv& b, const sort_t& s) {
    // Equal when they agree on every key either side mentions and have the same background.
    if (a.is_const != b.is_const) return false;
    if (a.is_const ? a.cv != b.cv : a.base != b.base) return false;
    for (const auto& [k, v] : a.over)
        if (array_select(b, k, s) != v) return false;
    for (const auto& [k, v] : b.over)
        if (array_select(a, k, s) != v) return false;
    return true;
}
std::uint64_t array_hash(const arrv& a) {
    std::uint64_t h = hash_combine(a.is_const ? 1 : 2, a.is_const ? static_cast<std::uint64_t>(a.cv) : a.base);
    for (const auto& [k, v] : a.over) {
        h = hash_combine(hash_combine(h, static_cast<std::uint64_t>(k)), static_cast<std::uint64_t>(v));
    }
    return h;
}

bool wide(const sort_t& s) { return s.kind == sk::bv && s.width > kMaxWidth; }

struct probe_gen {
    std::uint64_t seed;
    unsigned k;
    static constexpr unsigned kDiag = 24, kCorner = 88;

    word bv_corner(unsigned idx, unsigned w) const {
        const word m = mask_of(w);
        const word min = w ? word(1) << (w - 1) : 0;
        const word c55 = ((~word(0)) / 3) & m, caa = (c55 << 1) & m;
        const word list[] = {0, 1, m, min, min - 1, 2, 4, 16, c55, caa, 3, 255 & m};
        return list[idx % 12] & m;
    }
    sword int_corner(unsigned idx) const {
        static const sword list[] = {0, 1, -1, 2, -2, 3, 7, -8, 16, 100, -100, 1000};
        return list[idx % 12];
    }
    val make(const sort_t& s, unsigned slot, unsigned sort_id) const {
        const std::uint64_t h = hash_combine(hash_combine(seed, k), hash_combine(slot, sort_id));
        val r;
        const bool diag = k < kDiag, corner = k < kCorner;
        const unsigned cidx = diag ? k : static_cast<unsigned>(h >> 20);
        switch (s.kind) {
            case sk::boolean: r.v = diag ? ((k >> 1) & 1) ^ (k & 1) : (h >> 7) & 1; break;
            case sk::bv:
                if (s.width > kMaxWidth) break;
                r.v = corner ? bv_corner(cidx, s.width) : (hash_word(h, 0) & mask_of(s.width));
                break;
            case sk::integer:
                r.v = corner ? static_cast<word>(int_corner(cidx)) : static_cast<word>(static_cast<sword>(h % 2001) - 1000);
                break;
            case sk::array: {
                auto a = std::make_shared<arrv>();
                a->base = h;
                r.a = a;
                break;
            }
            default: r.v = h & 0xff; break;
        }
        return r;
    }
};

// One forward pass over the nodes for one probe. `slot[var ordinal]` is the
// permuted slot inside the variable's sort group.
struct evaluator {
    const term& t;
    std::vector<val> vals;
    explicit evaluator(const term& tt) : t(tt), vals(tt.nodes.size()) {}

    val opaque(const node& n) {
        std::uint64_t h = hash_combine(n.tag, static_cast<std::uint64_t>(n.o));
        for (auto k : n.kids) {
            const val& kv = vals[k];
            h = hash_combine(h, kv.a ? array_hash(*kv.a) : static_cast<std::uint64_t>(kv.v));
#if PRISM_HAS_INT128
            if (!kv.a) h = hash_combine(h, static_cast<std::uint64_t>(kv.v >> 64));
#endif
        }
        val r;
        switch (n.s.kind) {
            case sk::boolean: r.v = h & 1; break;
            case sk::bv: r.v = n.s.width <= kMaxWidth ? (hash_word(h, 0) & mask_of(n.s.width)) : hash_word(h, 0); break;
            case sk::array: { auto a = std::make_shared<arrv>(); a->base = h; r.a = a; break; }
            default: r.v = static_cast<word>(static_cast<sword>(h % 4001) - 2000); break;
        }
        return r;
    }

    void run(const probe_gen& pg, const std::vector<unsigned>& slot, const std::vector<unsigned>& sort_ids) {
        for (std::size_t i = 0; i < t.nodes.size() && i <= t.root; ++i) vals[i] = step(t.nodes[i], pg, slot, sort_ids);
    }

    val step(const node& n, const probe_gen& pg, const std::vector<unsigned>& slot, const std::vector<unsigned>& sort_ids) {
        val r;
        const unsigned w = n.s.width;
        const auto& K = n.kids;
        auto V = [&](std::size_t i) -> word { return vals[K[i]].v; };
        auto kw = [&](std::size_t i) -> unsigned { return t.nodes[K[i]].s.width; };
        if (n.o == op::var) return pg.make(n.s, slot[n.aux], sort_ids[n.aux]);
        if (wide(n.s)) return opaque(n);
        for (auto k : K)
            if (wide(t.nodes[k].s)) return opaque(n);
        const word m = mask_of(w);
        switch (n.o) {
            case op::tru: r.v = 1; break;
            case op::fls: r.v = 0; break;
            case op::lit: case op::ilit: r.v = n.val; break;
            case op::not_: r.v = !V(0); break;
            case op::and_: r.v = 1; for (std::size_t i = 0; i < K.size(); ++i) r.v &= V(i); break;
            case op::or_: r.v = 0; for (std::size_t i = 0; i < K.size(); ++i) r.v |= V(i); break;
            case op::xor_: r.v = 0; for (std::size_t i = 0; i < K.size(); ++i) r.v ^= V(i); break;
            case op::implies: r.v = (!V(0)) | V(1); break;
            case op::ite: return vals[K[V(0) ? 1 : 2]];
            case op::eq: {
                bool all = true;
                for (std::size_t i = 1; i < K.size(); ++i) {
                    const val &a = vals[K[0]], &b = vals[K[i]];
                    all = all && (a.a && b.a ? array_equal(*a.a, *b.a, t.nodes[K[0]].s) : a.v == b.v);
                }
                r.v = all;
                break;
            }
            case op::distinct: {
                bool all = true;
                for (std::size_t i = 0; i < K.size(); ++i)
                    for (std::size_t j = i + 1; j < K.size(); ++j) all = all && vals[K[i]].v != vals[K[j]].v;
                r.v = all;
                break;
            }
            case op::bvnot: r.v = ~V(0) & m; break;
            case op::bvneg: r.v = (word(0) - V(0)) & m; break;
            case op::bvand: r.v = m; for (std::size_t i = 0; i < K.size(); ++i) r.v &= V(i); break;
            case op::bvor: r.v = 0; for (std::size_t i = 0; i < K.size(); ++i) r.v |= V(i); break;
            case op::bvxor: r.v = 0; for (std::size_t i = 0; i < K.size(); ++i) r.v ^= V(i); break;
            case op::bvnand: r.v = ~(V(0) & V(1)) & m; break;
            case op::bvnor: r.v = ~(V(0) | V(1)) & m; break;
            case op::bvxnor: r.v = ~(V(0) ^ V(1)) & m; break;
            case op::bvadd: r.v = 0; for (std::size_t i = 0; i < K.size(); ++i) r.v += V(i); r.v &= m; break;
            case op::bvmul: r.v = 1; for (std::size_t i = 0; i < K.size(); ++i) r.v *= V(i); r.v &= m; break;
            case op::bvsub: r.v = (V(0) - V(1)) & m; break;
            case op::bvudiv: r.v = V(1) == 0 ? m : (V(0) / V(1)); break;
            case op::bvurem: r.v = V(1) == 0 ? V(0) : (V(0) % V(1)); break;
            case op::bvsdiv: case op::bvsrem: case op::bvsmod: {
                const word a = V(0), b = V(1);
                const bool na = msb(a, w), nb = msb(b, w);
                const word ua = na ? (word(0) - a) & m : a, ub = nb ? (word(0) - b) & m : b;
                if (n.o == op::bvsdiv) {
                    const word q = ub == 0 ? m : ua / ub;  // udiv semantics for /0
                    r.v = (na != nb ? (word(0) - q) : q) & m;
                    if (ub == 0) r.v = na ? 1 : m;  // SMT-LIB: s / 0 = 1 for s < 0, all ones otherwise
                } else if (n.o == op::bvsrem) {
                    const word q = ub == 0 ? ua : ua % ub;
                    r.v = (na ? (word(0) - q) : q) & m;
                } else {
                    const word u = ub == 0 ? ua : ua % ub;
                    if (ub == 0) r.v = a;
                    else if (u == 0) r.v = 0;
                    else if (!na && !nb) r.v = u;
                    else if (na && !nb) r.v = ((word(0) - u) + b) & m;
                    else if (!na && nb) r.v = (u + b) & m;
                    else r.v = (word(0) - u) & m;
                }
                break;
            }
            case op::bvshl: r.v = V(1) >= w ? 0 : (V(0) << static_cast<unsigned>(V(1))) & m; break;
            case op::bvlshr: r.v = V(1) >= w ? 0 : V(0) >> static_cast<unsigned>(V(1)); break;
            case op::bvashr: {
                const sword s = sx(V(0), w);
                const unsigned sh = V(1) >= w ? (w ? w - 1 : 0) : static_cast<unsigned>(V(1));
                r.v = static_cast<word>(s >> sh) & m;
                break;
            }
            case op::concat: {
                word acc = 0;
                for (std::size_t i = 0; i < K.size(); ++i) acc = (kw(i) >= kMaxWidth ? word(0) : acc << kw(i)) | V(i);
                r.v = acc & m;
                break;
            }
            case op::extract: r.v = (V(0) >> n.aux2) & m; break;
            case op::zext: r.v = V(0); break;
            case op::sext: r.v = static_cast<word>(sx(V(0), kw(0))) & m; break;
            case op::repeat: {
                word acc = 0;
                for (unsigned i = 0; i < n.aux; ++i) acc = (kw(0) >= kMaxWidth ? word(0) : acc << kw(0)) | V(0);
                r.v = acc & m;
                break;
            }
            case op::rotl: case op::rotr: {
                const unsigned a = w ? n.aux % w : 0;
                const unsigned l = n.o == op::rotl ? a : (w - a) % (w ? w : 1);
                const word x = V(0);
                r.v = l == 0 ? x : (((x << l) | (x >> (w - l))) & m);
                break;
            }
            case op::bvult: r.v = V(0) < V(1); break;
            case op::bvule: r.v = V(0) <= V(1); break;
            case op::bvugt: r.v = V(0) > V(1); break;
            case op::bvuge: r.v = V(0) >= V(1); break;
            case op::bvslt: r.v = sx(V(0), kw(0)) < sx(V(1), kw(1)); break;
            case op::bvsle: r.v = sx(V(0), kw(0)) <= sx(V(1), kw(1)); break;
            case op::bvsgt: r.v = sx(V(0), kw(0)) > sx(V(1), kw(1)); break;
            case op::bvsge: r.v = sx(V(0), kw(0)) >= sx(V(1), kw(1)); break;
            case op::bvcomp: r.v = V(0) == V(1); break;
            case op::iadd: r.v = 0; for (std::size_t i = 0; i < K.size(); ++i) r.v += V(i); break;
            case op::isub: r.v = V(0) - V(1); break;
            case op::imul: r.v = 1; for (std::size_t i = 0; i < K.size(); ++i) r.v *= V(i); break;
            case op::ineg: r.v = word(0) - V(0); break;
            case op::ile: r.v = static_cast<sword>(V(0)) <= static_cast<sword>(V(1)); break;
            case op::ilt: r.v = static_cast<sword>(V(0)) < static_cast<sword>(V(1)); break;
            case op::ige: r.v = static_cast<sword>(V(0)) >= static_cast<sword>(V(1)); break;
            case op::igt: r.v = static_cast<sword>(V(0)) > static_cast<sword>(V(1)); break;
            case op::idiv: case op::imod: {
                const sword a = static_cast<sword>(V(0)), b = static_cast<sword>(V(1));
                sword q = 0, rem = a;
                if (b != 0) {
                    q = a / b;
                    rem = a % b;
                    if (rem < 0) {
                        if (b > 0) { q -= 1; rem += b; } else { q += 1; rem -= b; }
                    }
                }
                r.v = static_cast<word>(n.o == op::idiv ? q : rem);
                break;
            }
            case op::select: {
                const val& arr = vals[K[0]];
                if (!arr.a || t.nodes[K[0]].s.depth != 1) return opaque(n);
                r.v = array_select(*arr.a, V(1), t.nodes[K[0]].s);
                break;
            }
            case op::store: {
                const val& arr = vals[K[0]];
                if (!arr.a || n.s.depth != 1) return opaque(n);
                r.a = array_store(*arr.a, V(1), V(2));
                break;
            }
            case op::constarr: {
                if (n.s.depth != 1) return opaque(n);
                auto a = std::make_shared<arrv>();
                a->is_const = true;
                a->cv = V(0);
                r.a = a;
                break;
            }
            default: return opaque(n);
        }
        return r;
    }
};

std::uint64_t out_bits(const val& v, const sort_t& s, const std::vector<val>&) {
    if (s.kind == sk::boolean) return (v.v & 1) ? 3 : 0;
    std::uint64_t h = v.a ? array_hash(*v.a) : hash_combine(static_cast<std::uint64_t>(v.v), 0x51);
#if PRISM_HAS_INT128
    if (!v.a) h = hash_combine(h, static_cast<std::uint64_t>(v.v >> 64));
#endif
    return mix64(h) & 3;
}

unsigned sort_id(const sort_t& s) {
    return static_cast<unsigned>(hash_combine(hash_combine(static_cast<std::uint64_t>(s.kind), s.width), hash_combine(s.idx_width, s.depth)) & 0xffffff);
}

}  // namespace

std::vector<std::uint64_t> semantic_signature(const term& t, const sim_options& opt, bool* constant) {
    const unsigned K = opt.probes;
    std::vector<std::uint64_t> best;
    if (constant) *constant = true;
    if (t.nodes.empty()) {
        best.assign((2 * K + 63) / 64, 0);
        return best;
    }
    // variable sorts by ordinal
    std::vector<sort_t> vsort(t.n_vars);
    for (const auto& n : t.nodes)
        if (n.o == op::var && n.aux < t.n_vars) vsort[n.aux] = n.s;
    std::vector<unsigned> sids(t.n_vars);
    for (unsigned i = 0; i < t.n_vars; ++i) sids[i] = sort_id(vsort[i]);
    // sort groups: ordinals of equal sort, in ordinal order
    std::map<unsigned, std::vector<unsigned>> groups;
    for (unsigned i = 0; i < t.n_vars; ++i) groups[sids[i]].push_back(i);
    std::uint64_t nperm = 1;
    for (auto& [id, g] : groups) {
        for (std::size_t f = 2; f <= g.size() && nperm <= kMaxPerms; ++f) nperm *= f;
        if (nperm > kMaxPerms) break;
    }
    const bool permute = nperm <= kMaxPerms;
    std::vector<std::vector<unsigned>> perm;  // per group, a permutation of 0..size-1
    for (auto& [id, g] : groups) {
        perm.emplace_back(g.size());
        for (std::size_t i = 0; i < g.size(); ++i) perm.back()[i] = static_cast<unsigned>(i);
    }
    evaluator ev(t);
    std::vector<unsigned> slot(t.n_vars);
    const sort_t root_sort = t.nodes[t.root].s;
    for (;;) {
        std::size_t gi = 0;
        for (auto& [id, g] : groups) {
            for (std::size_t i = 0; i < g.size(); ++i) slot[g[i]] = perm[gi][i];
            ++gi;
        }
        std::vector<std::uint64_t> sig((2 * K + 63) / 64, 0);
        bool all_same = true;
        std::uint64_t first = 0;
        for (unsigned k = 0; k < K; ++k) {
            ev.run(probe_gen{opt.seed, k}, slot, sids);
            const std::uint64_t b = out_bits(ev.vals[t.root], root_sort, ev.vals);
            if (k == 0) first = b;
            else if (b != first) all_same = false;
            sig[(2 * k) / 64] |= b << ((2 * k) % 64);
        }
        if (best.empty() || sig < best) {
            best = std::move(sig);
            if (constant) *constant = all_same;
        }
        if (!permute) break;
        // odometer over the group permutations
        std::size_t g = 0;
        for (; g < perm.size(); ++g) {
            if (std::next_permutation(perm[g].begin(), perm[g].end())) break;
        }
        if (g == perm.size()) break;
    }
    return best;
}

}  // namespace prism::sim
