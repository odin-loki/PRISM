// Loop proofs by constrained Horn clauses (Z3's Spacer engine) and
// data-driven candidates, as a round of the Houdini machinery (included by
// houdini.inc; docs/PIR.md "Loop proofs by CHC (Spacer)").
//
// The CHC system is read off the loop-cut encoding Houdini already uses
// (Encoding::cut_mode): one predicate Inv_L per loop over the loop's
// arguments (the header phis, their entry values and the scalar values from
// before the loop that the loop reads), plus a flag that is false when the
// header is not reached (Inv_L(false, ...) is a fact, so assuming
// Inv_L(g, ...) in a body never cuts off a path that does not reach L).
//
//   base   A /\ g_L /\ hyp(loops encoded before L)           -> Inv_L(true, entry state)
//   step   A /\ latch /\ noviol /\ hyp(loops up to the latch) -> Inv_L(true, next state)
//   safety A /\ hyp(loops before the property) /\ viol       -> false
//
// A is the encoding's side constraints; g_M = "header of M reached and no
// property violated before it"; hyp assumes Inv_M(g_M, havoc state of M).
// These are exactly the base, step and final queries of the Houdini search
// with one unknown invariant per loop, so every model of the clauses is an
// invariant Houdini would accept, and the encoding over-approximates the
// cut program the same way (memory is not a predicate argument: the havocked
// bytes of the footprint are arbitrary in every clause, which only allows
// more runs).
//
// Nothing Spacer answers is trusted:
//  * "sat" (an invariant exists): the model's interpretation of each Inv_L is
//    split into its conjuncts, which become Houdini candidates; the normal
//    search then checks base, step and the final query in its own solver.
//    Only that proof promotes the function (PROVED-UNBOUNDED).
//  * "unsat" (a counterexample of the CHC system): no verdict is taken from
//    it; check_function replays it with the bounded encoding at deeper
//    unwinds, and only the bounded encoding's own FAILED is reported.
//  * unknown / out of time: no proof (BOUNDED or UNKNOWN stands).

// Arguments of loop L's predicate: header phis, their entry values, and the
// values defined before the loop that the loop's blocks read (at most 16).
// Floating-point and wider-than-64-bit values are left out (fewer arguments
// only weaken what an invariant can say).
std::vector<InvTerm> Houdini::loop_args(int L) const {
    std::vector<InvTerm> a;
    const int Hb = g_.loops[static_cast<std::size_t>(L)].header;
    const auto& H = fn_.blocks[static_cast<std::size_t>(Hb)];
    auto ok = [&](int v) {
        const auto& x = fn_.vars[static_cast<std::size_t>(v)];
        return !x.fp && x.width >= 1 && x.width <= 64;
    };
    for (std::size_t j = 0; j < H.phis.size(); ++j)
        if (ok(H.phis[j].dst))
            a.push_back(InvTerm{InvTerm::Phi, static_cast<int>(j), 0, fn_.vars[static_cast<std::size_t>(H.phis[j].dst)].width});
    for (std::size_t j = 0; j < H.phis.size(); ++j)
        if (ok(H.phis[j].dst))
            a.push_back(InvTerm{InvTerm::Entry, static_cast<int>(j), 0, fn_.vars[static_cast<std::size_t>(H.phis[j].dst)].width});
    std::set<int> sc;
    auto add = [&](const Arg& x) {
        if (!x.is_const && x.var >= 0 && scope_var(L, x.var) && ok(x.var)) sc.insert(x.var);
    };
    for (std::size_t b = 0; b < fn_.blocks.size(); ++b) {
        if (!in_loop(L, static_cast<int>(b))) continue;
        const auto& bl = fn_.blocks[b];
        for (auto& p : bl.phis)
            for (auto& [pb, x] : p.in) {
                (void)pb;
                add(x);
            }
        for (auto& s : bl.stmts)
            for (auto& x : s.args) add(x);
        if (bl.term.kind == Term::Br) add(bl.term.cond);
    }
    std::size_t n = 0;
    for (int v : sc) {
        if (n++ >= 16) break;
        a.push_back(InvTerm{InvTerm::Scope, v, 0, fn_.vars[static_cast<std::size_t>(v)].width});
    }
    return a;
}

namespace houdini_data {

// The CPU twin of prism::cuda::direction_bounds (include/prism/cuda_inv.hpp):
// the same exact int64 sums, so both devices give the same candidates.
inline void direction_bounds_cpu(const std::vector<int64_t>& S, int ns, int nf, const std::vector<int16_t>& idx,
                                 const std::vector<int8_t>& coef, int nd, int k, std::vector<int64_t>& mn,
                                 std::vector<int64_t>& mx) {
    mn.assign(static_cast<std::size_t>(nd), INT64_MAX);
    mx.assign(static_cast<std::size_t>(nd), INT64_MIN);
    for (int d = 0; d < nd; ++d) {
        int64_t lo = INT64_MAX, hi = INT64_MIN;
        const std::size_t o = static_cast<std::size_t>(d) * static_cast<std::size_t>(k);
        for (int s = 0; s < ns; ++s) {
            const int64_t* row = S.data() + static_cast<std::size_t>(s) * static_cast<std::size_t>(nf);
            int64_t v = 0;
            for (int j = 0; j < k; ++j) v += static_cast<int64_t>(coef[o + static_cast<std::size_t>(j)]) * row[idx[o + static_cast<std::size_t>(j)]];
            lo = v < lo ? v : lo;
            hi = v > hi ? v : hi;
        }
        mn[static_cast<std::size_t>(d)] = lo;
        mx[static_cast<std::size_t>(d)] = hi;
    }
}

inline int64_t sext64(uint64_t v, unsigned w) {
    if (w == 0 || w >= 64) return static_cast<int64_t>(v);
    const uint64_t m = (uint64_t{1} << w) - 1, sb = uint64_t{1} << (w - 1);
    v &= m;
    return static_cast<int64_t>((v ^ sb) - sb);
}


// An integer basis of the affine relations c . x + c0 == 0 that hold on every
// sample (the affine hull of the samples, Karr-style): exact fraction-free
// elimination over __int128, the rows kept in reduced echelon form. Empty when
// a number grows past 2^100 or a basis coefficient past 2^20 (no candidates
// from such a loop, never a wrong one: each is only a candidate).
inline std::vector<std::vector<int64_t>> affine_hull_eqs(const std::vector<int64_t>& flat, int ns, int nf) {
    using i128 = __int128;
    const int n = nf + 1;
    const i128 big = static_cast<i128>(1) << 100;
    auto ab = [](i128 x) { return x < 0 ? -x : x; };
    auto gcd = [&](i128 a, i128 b) {
        a = ab(a);
        b = ab(b);
        while (b) {
            const i128 t = a % b;
            a = b;
            b = t;
        }
        return a;
    };
    auto norm = [&](std::vector<i128>& r) {
        i128 g = 0;
        for (auto x : r) g = gcd(g, x);
        if (g > 1)
            for (auto& x : r) x /= g;
    };
    std::vector<std::vector<i128>> R;
    std::vector<int> piv;
    for (int s = 0; s < ns && static_cast<int>(R.size()) < n; ++s) {
        std::vector<i128> r(static_cast<std::size_t>(n));
        for (int j = 0; j < nf; ++j) r[static_cast<std::size_t>(j)] = flat[static_cast<std::size_t>(s) * static_cast<std::size_t>(nf) + static_cast<std::size_t>(j)];
        r[static_cast<std::size_t>(nf)] = 1;
        for (std::size_t k = 0; k < R.size(); ++k) {
            const i128 a = r[static_cast<std::size_t>(piv[k])];
            if (!a) continue;
            const i128 b = R[k][static_cast<std::size_t>(piv[k])];
            for (int j = 0; j < n; ++j) {
                r[static_cast<std::size_t>(j)] = r[static_cast<std::size_t>(j)] * b - R[k][static_cast<std::size_t>(j)] * a;
                if (ab(r[static_cast<std::size_t>(j)]) > big) return {};
            }
            norm(r);
        }
        int p = -1;
        for (int j = 0; j < n && p < 0; ++j)
            if (r[static_cast<std::size_t>(j)]) p = j;
        if (p < 0) continue;
        // keep the form reduced: clear column p in the other rows
        for (std::size_t k = 0; k < R.size(); ++k) {
            const i128 a = R[k][static_cast<std::size_t>(p)];
            if (!a) continue;
            const i128 b = r[static_cast<std::size_t>(p)];
            for (int j = 0; j < n; ++j) {
                R[k][static_cast<std::size_t>(j)] = R[k][static_cast<std::size_t>(j)] * b - r[static_cast<std::size_t>(j)] * a;
                if (ab(R[k][static_cast<std::size_t>(j)]) > big) return {};
            }
            norm(R[k]);
        }
        R.push_back(std::move(r));
        piv.push_back(p);
    }
    std::vector<char> is_piv(static_cast<std::size_t>(n), 0);
    for (int p : piv) is_piv[static_cast<std::size_t>(p)] = 1;
    std::vector<std::vector<int64_t>> out;
    for (int f = 0; f < n; ++f) {
        if (is_piv[static_cast<std::size_t>(f)]) continue;
        // c[f] = lcm of the pivots of the rows that mention f; c[piv_k] = -R[k][f] * c[f] / R[k][piv_k]
        i128 cf = 1;
        for (std::size_t k = 0; k < R.size(); ++k)
            if (R[k][static_cast<std::size_t>(f)]) {
                const i128 pv = ab(R[k][static_cast<std::size_t>(piv[k])]);
                cf = cf / gcd(cf, pv) * pv;
                if (cf > big) return {};
            }
        std::vector<i128> c(static_cast<std::size_t>(n), 0);
        c[static_cast<std::size_t>(f)] = cf;
        for (std::size_t k = 0; k < R.size(); ++k)
            c[static_cast<std::size_t>(piv[k])] = -R[k][static_cast<std::size_t>(f)] * (cf / R[k][static_cast<std::size_t>(piv[k])]);
        norm(c);
        bool ok = true;
        std::vector<int64_t> o(static_cast<std::size_t>(n));
        for (int j = 0; j < n && ok; ++j) {
            const i128 x = c[static_cast<std::size_t>(j)];
            if (j < nf ? ab(x) > (static_cast<i128>(1) << 20) : ab(x) > (static_cast<i128>(1) << 62)) ok = false;
            o[static_cast<std::size_t>(j)] = static_cast<int64_t>(x);
        }
        if (ok) out.push_back(std::move(o));
    }
    return out;
}


}  // namespace houdini_data

// Data-driven candidates (the GPU-assisted path). Concrete runs of the
// function (the PIR interpreter, as in concrete_filter) sample the loop
// arguments at every header visit; for every direction d over at most four
// arguments (coefficients -2..2) the minimum and maximum of d . x over the
// samples are computed (on the GPU when built with CUDA, else the CPU loop;
// bit-identical); one whose maximum (minimum) is 0 gives the inequality
// d . x <= 0 (>= 0). Equalities come from the affine hull of the samples
// (affine_hull_eqs), at most 48 candidates per loop. These are guesses that hold on the samples only: they
// go to Houdini like every template and are proved (base, step, final
// query) or dropped. Arguments are read as signed values of their width;
// the arithmetic is exact (|x| < 2^31 on every sample, |d| <= 2, 4 terms;
// the SMT form uses 96-bit sums).
void Houdini::data_candidates(HoudiniOut& out) {
    namespace hc = houdini_concrete;
    namespace hd = houdini_data;
    data_cands_.clear();
    const std::size_t nL = g_.loops.size();
    if (nL == 0) return;
    constexpr std::size_t kMaxSamples = 4096;
    std::vector<std::vector<std::vector<int64_t>>> samples(nL);
    std::vector<std::vector<uint64_t>> entry(nL);
    std::vector<char> have_entry(nL, 0);
    std::map<int, std::vector<int>> loops_at;
    for (std::size_t L = 0; L < nL; ++L) loops_at[g_.loops[L].header].push_back(static_cast<int>(L));
    std::size_t full = 0;
    std::vector<std::size_t> run_n(nL, 0);  // samples per loop in this run (at most 128: several runs, not one)
    InterpHook hook = [&](int b, int prev, const std::vector<uint64_t>& val) -> bool {
        auto it = loops_at.find(b);
        if (it == loops_at.end()) return true;
        auto& hb = fn_.blocks[static_cast<std::size_t>(b)];
        std::vector<uint64_t> ph(hb.phis.size());
        for (std::size_t j = 0; j < hb.phis.size(); ++j) ph[j] = val.at(static_cast<std::size_t>(hb.phis[j].dst));
        for (int L : it->second) {
            auto& lp = g_.loops[static_cast<std::size_t>(L)];
            const bool from_inside =
                prev >= 0 && static_cast<std::size_t>(prev) < lp.body.size() && lp.body[static_cast<std::size_t>(prev)];
            if (!from_inside) {
                entry[static_cast<std::size_t>(L)] = ph;
                have_entry[static_cast<std::size_t>(L)] = 1;
            }
            if (!have_entry[static_cast<std::size_t>(L)]) continue;
            auto& S = samples[static_cast<std::size_t>(L)];
            if (S.size() >= kMaxSamples || run_n[static_cast<std::size_t>(L)] >= 128) continue;
            ++run_n[static_cast<std::size_t>(L)];
            std::vector<int64_t> row;
            for (auto& t : largs_[static_cast<std::size_t>(L)]) {
                uint64_t v = 0;
                if (t.kind == InvTerm::Phi) v = ph.at(static_cast<std::size_t>(t.idx));
                else if (t.kind == InvTerm::Entry) v = entry[static_cast<std::size_t>(L)].at(static_cast<std::size_t>(t.idx));
                else v = val.at(static_cast<std::size_t>(t.idx));
                row.push_back(hd::sext64(v, t.w));
            }
            S.push_back(std::move(row));
            if (S.size() == kMaxSamples && ++full == nL) return false;
        }
        return true;
    };
    std::vector<unsigned> pw;
    for (int p : fn_.params) pw.push_back(fn_.vars[static_cast<std::size_t>(p)].width);
    uint64_t seed = 0xda7a5eed0f1a7e5ull;
    const prism::clock::Stopwatch t0;
    int runs = 0;
    for (int run = 0; run < 96 && full < nL && t0.elapsed_s() < 2.0; ++run) {
        std::vector<uint64_t> args(pw.size());
        for (std::size_t j = 0; j < pw.size(); ++j) {
            const unsigned w = pw[j];
            const uint64_t M = hc::mask_w(w);
            uint64_t v = 0;
            switch (run) {
                case 0: v = 0; break;
                case 1: v = 1; break;
                case 2: v = M; break;
                case 3: v = 2; break;
                case 4: v = 7; break;
                default: {
                    const uint64_t r = hc::splitmix(seed);
                    switch (r % 4) {
                        case 0: v = r >> 60; break;
                        case 1: v = (r >> 32) % 300; break;
                        case 2: v = ~(r >> 58); break;
                        default: v = (r >> 40) % 70000; break;
                    }
                }
            }
            args[j] = v & M;
        }
        for (auto& en : have_entry) en = 0;
        for (auto& rn : run_n) rn = 0;
        (void)interpret(fn_, args, 200'000, hook);
        ++runs;
    }
    const double sample_s = t0.elapsed_s();
    // directions per loop, and their bounds
    std::size_t total_samples = 0, total_dirs = 0;
    double bounds_ms = 0, cpu_ms = 0;
    std::string device = "cpu";
    const char* dv = std::getenv("PRISM_PIR_CHC_DEVICE");
    const std::string want = dv && *dv ? dv : "cuda";
    bool mismatch = false;
    for (std::size_t L = 0; L < nL; ++L) {
        auto& S = samples[L];
        total_samples += S.size();
        if (S.size() < 2) continue;
        const auto& A = largs_[L];
        // usable features: |v| < 2^31 on every sample; at most 24 (and only
        // the first 12 in four-term directions)
        std::vector<int> F;
        for (std::size_t i = 0; i < A.size() && F.size() < 24; ++i) {
            bool small = true;
            for (auto& r : S)
                if (r[i] >= (int64_t{1} << 31) || r[i] <= -(int64_t{1} << 31)) {
                    small = false;
                    break;
                }
            if (small) F.push_back(static_cast<int>(i));
        }
        const int nf = static_cast<int>(F.size());
        if (nf == 0) continue;
        const int ns = static_cast<int>(S.size());
        std::vector<int64_t> flat(static_cast<std::size_t>(ns) * static_cast<std::size_t>(nf));
        for (int s = 0; s < ns; ++s)
            for (int j = 0; j < nf; ++j)
                flat[static_cast<std::size_t>(s) * static_cast<std::size_t>(nf) + static_cast<std::size_t>(j)] =
                    S[static_cast<std::size_t>(s)][static_cast<std::size_t>(F[static_cast<std::size_t>(j)])];
        auto has_phi = [&](const std::vector<int>& sub) {
            for (int j : sub)
                if (A[static_cast<std::size_t>(F[static_cast<std::size_t>(j)])].kind == InvTerm::Phi) return true;
            return false;
        };
        struct Cand {
            int K;
            int mag;
            std::vector<std::pair<int, int>> lin;
            Op rel;
            int64_t k;
        };
        std::vector<Cand> picked;
        // equalities: a basis of the affine hull of the samples
        for (auto& rel : hd::affine_hull_eqs(flat, ns, nf)) {
            std::vector<std::pair<int, int>> lin;
            int mag = 0;
            bool phi = false;
            for (int j = 0; j < nf; ++j)
                if (rel[static_cast<std::size_t>(j)]) {
                    lin.emplace_back(F[static_cast<std::size_t>(j)], static_cast<int>(rel[static_cast<std::size_t>(j)]));
                    mag += static_cast<int>(std::min<int64_t>(1 << 20, std::abs(rel[static_cast<std::size_t>(j)])));
                    if (A[static_cast<std::size_t>(F[static_cast<std::size_t>(j)])].kind == InvTerm::Phi) phi = true;
                }
            if (phi && !lin.empty()) picked.push_back(Cand{0, mag, lin, Op::Eq, -rel[static_cast<std::size_t>(nf)]});
        }
        for (int K = 1; K <= 4; ++K) {
            const int lim = K == 4 ? std::min(nf, 12) : nf;
            if (lim < K) break;
            std::vector<int16_t> idx;
            std::vector<int8_t> coef;
            std::vector<int> sub(static_cast<std::size_t>(K));
            static const int8_t kC[] = {-2, -1, 1, 2};
            std::function<void(int, int)> subsets = [&](int pos, int from) {
                if (pos == K) {
                    if (!has_phi(sub)) return;
                    // coefficient vectors: first one positive (d and -d give the same pair of bounds);
                    // K = 1 only +1; not all even (2d is d)
                    const int nc = K == 1 ? 1 : 1 << (2 * K);
                    for (int m = 0; m < nc; ++m) {
                        int8_t cs[4];
                        bool all_even = true;
                        for (int j = 0; j < K; ++j) {
                            cs[j] = K == 1 ? int8_t{1} : kC[(m >> (2 * j)) & 3];
                            if (cs[j] & 1) all_even = false;
                        }
                        if (cs[0] < 0 || all_even) continue;
                        for (int j = 0; j < K; ++j) {
                            idx.push_back(static_cast<int16_t>(sub[static_cast<std::size_t>(j)]));
                            coef.push_back(cs[j]);
                        }
                    }
                    return;
                }
                for (int i = from; i < lim; ++i) {
                    sub[static_cast<std::size_t>(pos)] = i;
                    subsets(pos + 1, i + 1);
                }
            };
            subsets(0, 0);
            const int nd = static_cast<int>(coef.size() / static_cast<std::size_t>(K));
            if (nd == 0) continue;
            total_dirs += static_cast<std::size_t>(nd);
            std::vector<int64_t> mn(static_cast<std::size_t>(nd)), mx(static_cast<std::size_t>(nd));
            bool done = false;
            const prism::clock::Stopwatch tb;
#ifdef PRISM_HAS_CUDA
            if (want != "cpu") {
                double kms = -1;
                if (prism::cuda::direction_bounds(flat.data(), ns, nf, idx.data(), coef.data(), nd, K, mn.data(), mx.data(),
                                                  &kms) == 0) {
                    done = true;
                    device = "cuda";
                }
            }
#endif
            if (!done) hd::direction_bounds_cpu(flat, ns, nf, idx, coef, nd, K, mn, mx);
            bounds_ms += tb.elapsed_s() * 1000;
            if (done && want == "both") {  // the CPU loop too: its time, and the same bounds (else no candidates)
                std::vector<int64_t> mn2, mx2;
                const prism::clock::Stopwatch tc;
                hd::direction_bounds_cpu(flat, ns, nf, idx, coef, nd, K, mn2, mx2);
                cpu_ms += tc.elapsed_s() * 1000;
                if (mn2 != mn || mx2 != mx) mismatch = true;
            }
            for (int d = 0; d < nd; ++d) {
                const std::size_t o = static_cast<std::size_t>(d) * static_cast<std::size_t>(K);
                std::vector<std::pair<int, int>> lin;
                int mag = 0;
                for (int j = 0; j < K; ++j) {
                    lin.emplace_back(F[static_cast<std::size_t>(idx[o + static_cast<std::size_t>(j)])], coef[o + static_cast<std::size_t>(j)]);
                    mag += std::abs(coef[o + static_cast<std::size_t>(j)]);
                }
                const int64_t lo = mn[static_cast<std::size_t>(d)], hi = mx[static_cast<std::size_t>(d)];
                if (lo == hi) continue;  // an equality: in the affine hull basis already
                if (hi == 0) picked.push_back(Cand{K, mag, lin, Op::Sle, 0});
                if (lo == 0) picked.push_back(Cand{K, mag, lin, Op::Sge, 0});
            }
        }
        std::stable_sort(picked.begin(), picked.end(), [](const Cand& a, const Cand& b) {
            return a.K != b.K ? a.K < b.K : a.mag < b.mag;
        });
        constexpr std::size_t kPerLoop = 48;
        if (picked.size() > kPerLoop) picked.resize(kPerLoop);
        const auto& H = fn_.blocks[static_cast<std::size_t>(g_.loops[L].header)];
        auto aname = [&](int i) {
            const auto& t = A[static_cast<std::size_t>(i)];
            if (t.kind == InvTerm::Scope) return "%" + fn_.vars[static_cast<std::size_t>(t.idx)].name;
            const std::string n = "%" + fn_.vars[static_cast<std::size_t>(H.phis[static_cast<std::size_t>(t.idx)].dst)].name;
            return t.kind == InvTerm::Entry ? "entry(" + n + ")" : n;
        };
        for (auto& p : picked) {
            InvCand k;
            k.kind = InvCand::Data;
            k.loop = static_cast<int>(L);
            k.lin = p.lin;
            k.rel = p.rel;
            k.k = p.k;
            std::string t;
            for (auto& [i, cf] : p.lin) t += (t.empty() ? "" : " + ") + std::to_string(cf) + "*" + aname(i);
            t += std::string(p.rel == Op::Eq ? " == " : p.rel == Op::Sle ? " <= " : " >= ") + std::to_string(p.k) + " [int]";
            k.txt = t;
            k.size = 2 * p.lin.size() + 2;
            data_cands_.push_back(std::move(k));
        }
    }
    if (mismatch) data_cands_.clear();  // never: the two devices compute the same sums (reported)
    char buf[256];
    std::snprintf(buf, sizeof buf, "%zu sample(s) from %d run(s) in %.2f s; %zu direction(s); %zu candidate(s); device %s%s",
                  total_samples, runs, sample_s, total_dirs, data_cands_.size(), device.c_str(),
                  mismatch ? "; GPU/CPU bounds DIFFER (candidates dropped)" : "");
    out.data = buf;
    out.data_bounds_ms = total_dirs ? bounds_ms : -1;
    out.data_cpu_ms = want == "both" && device == "cuda" ? cpu_ms : -1;
}


// Outside candidates of this function (PRISM_PIR_LOOP_CANDIDATES): each row
// whose header line is a loop of the function (fn.loop_locs) and whose
// expression parses, with every identifier resolved to one of the loop's
// predicate arguments (a header phi by its C name: IR "x" or "x.N"; entry(x)
// its entry value; a value from before the loop by name), becomes an Ext
// candidate. Returns how many rows the function had.
std::size_t Houdini::ext_candidates(HoudiniOut& out) {
    namespace hd = houdini_data;
    const auto rows = hd::ext_candidates_for(fn_.name);
    if (rows.empty()) return 0;
    std::size_t parsed = 0;
    for (auto& [line, ex, src] : rows) {
        int L = -1;
        for (std::size_t j = 0; j < g_.loops.size() && L < 0; ++j) {
            auto loc = fn_.loop_locs.find(g_.loops[j].header);
            if (loc != fn_.loop_locs.end() && loc->second.first == line) L = static_cast<int>(j);
        }
        if (L < 0) continue;
        hd::ExtParser p(ex);
        auto ast = p.parse();
        if (!ast) continue;
        bool ok = true;
        std::function<void(const hd::ExtNode&)> chk = [&](const hd::ExtNode& n) {
            if (n.k == hd::ExtNode::Id || n.k == hd::ExtNode::Entry)
                if (ext_arg(L, n.id, n.k == hd::ExtNode::Entry) < 0) ok = false;
            for (auto& c : n.kids) chk(c);
        };
        chk(*ast);
        if (!ok) continue;
        ++parsed;
        InvCand k;
        k.kind = InvCand::Ext;
        k.loop = L;
        k.lem = static_cast<int>(ext_ast_.size());
        k.txt = ex + (src.empty() ? std::string() : " [" + src + "]");
        k.size = ex.size();
        ext_ast_.push_back(std::move(*ast));
        data_cands_.push_back(std::move(k));
    }
    out.ext = std::to_string(rows.size()) + " given, " + std::to_string(parsed) + " parsed and matched to a loop";
    return rows.size();
}

// The predicate argument an outside candidate's identifier names (-1: none).
int Houdini::ext_arg(int L, const std::string& id, bool entry) const {
    const auto& A = largs_.at(static_cast<std::size_t>(L));
    const auto& H = fn_.blocks[static_cast<std::size_t>(g_.loops[static_cast<std::size_t>(L)].header)];
    auto named = [&](int v) {
        const std::string& n = fn_.vars[static_cast<std::size_t>(v)].name;
        return n == id || (n.size() > id.size() && n.compare(0, id.size(), id) == 0 && n[id.size()] == '.');
    };
    for (std::size_t i = 0; i < A.size(); ++i) {
        const auto& t = A[i];
        if ((t.kind == InvTerm::Phi && !entry) || (t.kind == InvTerm::Entry && entry))
            if (named(H.phis[static_cast<std::size_t>(t.idx)].dst)) return static_cast<int>(i);
    }
    if (entry) return -1;
    for (std::size_t i = 0; i < A.size(); ++i)
        if (A[i].kind == InvTerm::Scope && named(A[i].idx)) return static_cast<int>(i);
    return -1;
}

HoudiniOut Houdini::run_chc(int mode) {
    ptr_ = pointer_vars(fn_);
    def_.assign(fn_.vars.size(), nullptr);
    for (auto& b : fn_.blocks)
        for (auto& s : b.stmts)
            if (s.kind == Stmt::Assign && s.dst >= 0) def_[static_cast<std::size_t>(s.dst)] = &s;
    signed_use_.assign(fn_.vars.size(), 0);
    chc_mode_ = mode;
    env_.cache_dir.clear();  // a CHC round is never cached (Spacer's lemmas are re-checked every time)
    largs_.clear();
    for (std::size_t L = 0; L < g_.loops.size(); ++L) largs_.push_back(loop_args(static_cast<int>(L)));
    HoudiniOut d;
    std::size_t data_n = 0;
    if (mode == 2) data_candidates(d);
    const std::size_t ext_n = ext_candidates(d);  // PRISM_PIR_LOOP_CANDIDATES (outside candidates)
    if (mode == 2 || ext_n > 0) {
        data_n = data_cands_.size();
        if (!data_cands_.empty()) {
            // the data-only pass: Houdini over the data-driven candidates
            chc_skip_spacer_ = true;
            HoudiniOut first = run_search();
            chc_skip_spacer_ = false;
            if (first.proved) {
                first.engine = mode == 4 ? "outside" : ext_n > 0 ? (mode == 2 ? "data+outside" : "outside") : "data";
                first.ext = d.ext;
                first.data = d.data;
                first.data_bounds_ms = d.data_bounds_ms;
                first.data_cpu_ms = d.data_cpu_ms;
                first.chc = "not asked (the data-driven candidates closed every loop)";
                first.inv_size = 0;
                for (auto& k : first.cands)
                    if (k.alive && (k.kind == InvCand::Data || k.kind == InvCand::Ext)) first.inv_size += k.size;
                return first;
            }
            if (mode == 4) {  // outside candidates only (PRISM_PIR_CHC unset): no Spacer
                first.engine = "outside";
                first.ext = d.ext;
                std::size_t kept = 0;
                for (auto& k : first.cands)
                    if (k.alive && k.kind == InvCand::Ext) ++kept;
                first.ext += ", " + std::to_string(kept) + " inductive, no proof";
                return first;
            }
            // its survivors (inductive, not enough) are Spacer's hints and stay candidates
            std::vector<InvCand> keep;
            for (auto& k : first.cands)
                if (k.alive) keep.push_back(k);
            data_cands_ = std::move(keep);
            d.data += "; data-only pass: " + std::to_string(data_cands_.size()) + " inductive of " + std::to_string(data_n) +
                      ", no proof (" + first.why + ")";
        }
    }
    HoudiniOut out = run_search();
    out.data = d.data;
    out.ext = d.ext;
    out.data_bounds_ms = d.data_bounds_ms;
    out.data_cpu_ms = d.data_cpu_ms;
    if (out.engine.empty()) out.engine = mode == 2 ? "spacer+data" : "spacer";
    out.inv_size = 0;
    for (auto& k : out.cands)
        if (k.alive && (k.kind == InvCand::Chc || k.kind == InvCand::Data || k.kind == InvCand::Ext)) out.inv_size += k.size;
    return out;
}

std::string Houdini::chc_differential() {
    chc_solve_only_ = true;
    HoudiniOut o = run_chc(1);
    if (o.chc.empty()) return "not asked: " + o.why;
    if (o.chc_cex) return "cex";
    if (o.chc.rfind("invariant found", 0) == 0) return "safe";
    return o.chc;
}

// PRISM_PIR_CHC_DIFF (check_function): Spacer's answer on a function's CHC
// system, without any re-check. Used on FAILED functions only, where the
// answer must not be "safe" (the over-approximation test).
std::string chc_differential_of(const Function& fn, const EncodeOptions& eo, double timeout_s) {
    PtrModelScope pms(fn.ptr);
    auto g = analyze(fn);
    if (!g.unencoded.empty()) return "not encoded";
    for (auto& L : g.loops)
        for (std::size_t b = 0; b < L.body.size(); ++b) {
            if (!L.body.at(b)) continue;
            for (auto& s : fn.blocks[b].stmts)
                if (s.kind == Stmt::Alloc || s.kind == Stmt::Free || s.kind == Stmt::StackRestore || s.kind == Stmt::Revive)
                    return "not attempted (allocation or free in a loop)";
        }
    try {
        Houdini hd(fn, g, eo, timeout_s);
        return hd.chc_differential();
    } catch (const EncodeFail& f) {
        return "not encoded: " + f.msg;
    } catch (const mem::EncodeError& f) {
        return "not encoded: " + f.msg;
    } catch (const z3::exception& ex) {
        return std::string("z3: ") + ex.msg();
    }
}

// Why a loop "allocates or frees" (extra.loop_alloc; diagnostics only):
// statement counts over the loops' blocks, the allocation kinds, and how many
// stack allocations are freed (end of lifetime) inside the same loop, i.e.
// locals of a callee inlined into the loop body.
std::string loop_alloc_summary(const Function& fn, const CfgInfo& g) {
    int alloc = 0, stack = 0, heap = 0, other = 0, freed = 0, frees = 0, revive = 0, restore = 0;
    for (auto& L : g.loops) {
        std::set<int> dsts, freed_vars;
        for (std::size_t b = 0; b < L.body.size(); ++b) {
            if (!L.body.at(b)) continue;
            for (auto& s : fn.blocks[b].stmts) {
                if (s.kind == Stmt::Alloc) {
                    ++alloc;
                    if (s.mkind == MemKind::Stack) ++stack;
                    else if (s.mkind == MemKind::Heap || s.mkind == MemKind::New || s.mkind == MemKind::NewArr) ++heap;
                    else ++other;
                    if (s.mkind == MemKind::Stack) dsts.insert(s.dst);
                } else if (s.kind == Stmt::Free) {
                    ++frees;
                    if (!s.args.empty() && !s.args[0].is_const) freed_vars.insert(s.args[0].var);
                } else if (s.kind == Stmt::Revive) {
                    ++revive;
                } else if (s.kind == Stmt::StackRestore) {
                    ++restore;
                }
            }
        }
        for (int d : dsts)
            if (freed_vars.count(d)) ++freed;
    }
    return std::to_string(alloc) + " alloc (" + std::to_string(stack) + " stack, " + std::to_string(heap) + " heap, " +
           std::to_string(other) + " other; " + std::to_string(freed) + " stack freed in the same loop), " +
           std::to_string(frees) + " free, " + std::to_string(revive) + " revive, " + std::to_string(restore) +
           " stackrestore";
}
