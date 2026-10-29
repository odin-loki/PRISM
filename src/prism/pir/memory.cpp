// PIR memory model: symbolic memory for the encoder (SymMem) and concrete
// memory for the interpreter (ConcMem). See memory.hpp and docs/PIR.md
// "Memory model"; the Lean counterpart is proofs/semantics/PrismSem/Memory.lean
// (semantics) and MemEncode.lean (read-over-write encoding).

#include "prism/membudget.hpp"
#include "memory.hpp"

#include "prism/laws.hpp"

#include <algorithm>

namespace prism::pir {

const char* mem_kind_name(MemKind k) {
    switch (k) {
        case MemKind::None: return "none";
        case MemKind::Stack: return "stack";
        case MemKind::Heap: return "heap";
        case MemKind::Static: return "static";
        case MemKind::Const: return "const";
        case MemKind::New: return "new";
        case MemKind::NewArr: return "new[]";
        case MemKind::Extern: return "extern";
        case MemKind::File: return "FILE";
    }
    return "?";
}

namespace mem {

unsigned type_tag(unsigned width, bool is_ptr) {
    if (is_ptr) return 5;
    switch (width) {
        case 1:
        case 8: return 0;  // character types may alias everything
        case 16: return 2;
        case 32: return 3;
        case 64: return 4;
        default: return 6;
    }
}

#ifdef PRISM_HAS_Z3

SymMem::SymMem(z3::context& c, MemEncoding enc, bool tags, std::vector<z3::expr>& side)
    : c_(c), enc_(enc), tags_(tags), cw_(cell_width(tags)), side_(side) {
    if (enc_ == MemEncoding::Array) arr_ = z3::const_array(c_.bv_sort(64), bv(0, cw_));
}

std::optional<uint64_t> SymMem::known(const z3::expr& e) const {
    uint64_t v = 0;
    if (e.is_numeral() && e.is_numeral_u64(v)) return obj_of(v);
    auto it = prov_.find(e.id());
    if (it != prov_.end()) return it->second;
    // every value the term can take is in one object
    if (auto os = objs_of(e); os && os->size() == 1) return os->front();
    return std::nullopt;
}

namespace {
z3::expr and2(const z3::expr& a, const z3::expr& b) {
    if (a.is_true()) return b;
    if (b.is_true()) return a;
    return a && b;
}
}  // namespace

namespace {
uint64_t mask_w(unsigned w) { return w >= 64 ? ~uint64_t{0} : (uint64_t{1} << w) - 1; }
// Evaluates a bit-vector operation on constants (widths <= 64); nullopt:
// not one of these (the caller asks Z3 instead).
std::optional<uint64_t> eval_const(const z3::expr& e, const std::vector<uint64_t>& a) {
    const unsigned w = e.get_sort().bv_size();
    const auto k = e.decl().decl_kind();
    auto aw = [&](unsigned i) { return e.arg(i).get_sort().bv_size(); };
    uint64_t r = 0;
    switch (k) {
        case Z3_OP_BADD: for (auto x : a) r += x; break;
        case Z3_OP_BMUL: r = 1; for (auto x : a) r *= x; break;
        case Z3_OP_BSUB: r = a.at(0); for (std::size_t i = 1; i < a.size(); ++i) r -= a.at(i); break;
        case Z3_OP_BAND: r = ~uint64_t{0}; for (auto x : a) r &= x; break;
        case Z3_OP_BOR: for (auto x : a) r |= x; break;
        case Z3_OP_BXOR: for (auto x : a) r ^= x; break;
        case Z3_OP_BNOT: r = a.empty() ? 0 : ~a.at(0); break;
        case Z3_OP_BNEG: r = a.empty() ? 0 : uint64_t{0} - a.at(0); break;
        case Z3_OP_CONCAT:
            for (std::size_t i = 0; i < a.size(); ++i) {
                const unsigned w_i = aw(static_cast<unsigned>(i));
                const uint64_t shift = w_i >= 64 ? 0 : r << w_i;
                r = shift | a.at(i);
            }
            break;
        case Z3_OP_EXTRACT: r = a.empty() ? 0 : a.at(0) >> e.lo(); break;
        case Z3_OP_ZERO_EXT: r = a.empty() ? 0 : a.at(0); break;
        case Z3_OP_SIGN_EXT: {
            const unsigned iw = aw(0);
            r = a.empty() ? 0 : a.at(0);
            if (iw < 64 && ((r >> (iw - 1)) & 1u) != 0u) r |= ~mask_w(iw);
            break;
        }
        default: return std::nullopt;
    }
    return r & mask_w(w);
}
}  // namespace

// Law 7: one load can build millions of read-over-write terms (a symbolic
// address against a long write log, nested once per copy). The process RSS
// is checked every few thousand steps against the memory budget; past it the
// function ends with a reported UNKNOWN (encode.cpp maps this exception) and
// its context, with everything built here, is freed.
static void mem_guard() {
    if (membudget::over_budget_sampled(256))
        throw z3::exception("soft memory cap (memory budget) reached while encoding memory");
}

// Raw value sets: the options are kept apart even when two have the same
// value, so terms built the same way (the bytes of one read-over-write
// chain) have the same guard list and combine position by position.
void SymMem::seed_vs(const z3::expr& e, Vs vs) {
    // An empty set is not a value set (callers would treat the pointer as
    // success and read front()/back()). Refuse it; do not cache it.
    if (vs.empty() || vs.size() > kVsMax) return;
    vs_keep_.push_back(e);
    for (auto& o : vs) vs_keep_.push_back(o.guard);
    vs_seed_[e.id()] = std::move(vs);
    vs_memo_.erase(e.id());
    vs_merged_.erase(e.id());
}

std::optional<SymMem::Vs> SymMem::vs_bin(const z3::expr& e, const z3::expr& a, const z3::expr& b,
                                           Z3_decl_kind op) const {
    auto one = [&](const z3::expr& x) -> std::optional<Vs> {
        // Shallow only: vs_raw walks an arbitrary read-over-write term and
        // retains every subexpression it touches.
        if (auto it = vs_seed_.find(x.id()); it != vs_seed_.end()) {
            if (it->second.empty()) return std::nullopt;
            return it->second;
        }
        uint64_t n = 0;
        if (x.is_numeral() && x.is_numeral_u64(n)) return Vs{VsOpt{c_.bool_val(true), n}};
        return shallow_vs(x, kShallowDepth);
    };
    auto av = one(a);
    auto bv = one(b);
    if (!av || !bv) return std::nullopt;
    const z3::expr tt = c_.bool_val(true);
    bool zip = true;
    const Vs* shape = nullptr;
    std::size_t combos = 1;
    if (av->size() > 1) {
        shape = &*av;
        combos *= av->size();
    }
    if (bv->size() > 1) {
        combos *= bv->size();
        if (!shape) shape = &*bv;
        else if (bv->size() != shape->size()) zip = false;
        else
            for (std::size_t j = 0; j < bv->size() && zip; ++j)
                if ((*bv)[j].guard.id() != (*shape)[j].guard.id()) zip = false;
    }
    if (!zip && combos > kVsCombos) return std::nullopt;
    Vs r;
    const std::size_t n = !shape ? 1 : zip ? shape->size() : combos;
    std::vector<std::size_t> idx{0, 0};
    for (std::size_t m = 0; m < n; ++m) {
        const auto& ao = av->size() == 1 ? av->front() : zip ? av->at(m) : av->at(idx[0]);
        const auto& bo = bv->size() == 1 ? bv->front() : zip ? bv->at(m) : bv->at(idx[1]);
        z3::expr g = tt;
        if (zip && shape)
            g = shape->at(m).guard;
        else {
            if (av->size() > 1) g = and2(g, ao.guard);
            if (bv->size() > 1) g = and2(g, bo.guard);
        }
        std::optional<uint64_t> kv;
        if (op == Z3_OP_BADD)
            kv = eval_const(e, {ao.val, bo.val});
        else if (op == Z3_OP_BSUB)
            kv = eval_const(e, {ao.val, bo.val});
        else if (op == Z3_OP_BMUL)
            kv = eval_const(e, {ao.val, bo.val});
        if (!kv) return std::nullopt;
        r.push_back(VsOpt{g, *kv});
        if (!zip) {
            if (av->size() > 1) {
                if (++idx[0] < av->size()) continue;
                idx[0] = 0;
            }
            if (bv->size() > 1) ++idx[1];
            if (idx[1] >= bv->size()) idx[1] = 0;
        }
    }
    if (r.size() > kVsMax) return std::nullopt;
    return r;
}

// ite(c, a, ite(c, b, d)) is ite(c, a, d) and ite(c, ite(c, a, b), d) is
// ite(c, a, d) (also with not c; one-argument or/and are their argument).
// The encoder's phis give such terms (a value undefined on the path the
// outer condition excludes, e.g. uninit!N), and the dead leaf has no value
// set. Exact: equal terms, decided on AST identity only.
z3::expr SymMem::peel_ite(const z3::expr& c_in, z3::expr t, bool then_side) const {
    auto strip = [](z3::expr x) {
        while (x.is_app() && x.num_args() == 1 &&
               (x.decl().decl_kind() == Z3_OP_OR || x.decl().decl_kind() == Z3_OP_AND))
            x = x.arg(0);
        return x;
    };
    auto neg_of = [&](const z3::expr& x, const z3::expr& y) {
        return x.is_app() && x.decl().decl_kind() == Z3_OP_NOT && x.num_args() == 1 && strip(x.arg(0)).id() == y.id();
    };
    const z3::expr c = strip(c_in);
    for (int n = 0; n < 64 && t.is_app() && t.decl().decl_kind() == Z3_OP_ITE && t.num_args() == 3; ++n) {
        const z3::expr ic = strip(t.arg(0));
        const bool same = ic.id() == c.id(), opp = neg_of(ic, c) || neg_of(c, ic);
        if (!same && !opp) break;
        t = (then_side ? same : opp) ? t.arg(1) : t.arg(2);
    }
    return t;
}

void SymMem::seed_vs_bin(const z3::expr& sum, const z3::expr& a, const z3::expr& b, Z3_decl_kind op) {
    if (auto vs = vs_bin(sum, a, b, op)) seed_vs(sum, std::move(*vs));
}

// Bounded ite / arithmetic. A miss is not stored: vs_raw may still find a set
// later, and an empty vector must not be cached as a successful set.
// One walk memoises by expr id (the AST is a DAG). A miss from a short
// remaining depth stays a miss if the same node is reached again; that only
// skips a seed, and the byte model is still used.
std::optional<SymMem::Vs> SymMem::shallow_vs(const z3::expr& e, int depth) const {
    std::unordered_map<unsigned, std::optional<Vs>> seen;
    auto rec = [&](auto& self, const z3::expr& cur, int depth) -> std::optional<Vs> {
        if (auto it = seen.find(cur.id()); it != seen.end()) return it->second;
        mem_guard();
        auto store = [&](std::optional<Vs> r) {
            seen.emplace(cur.id(), r);
            return r;
        };
        if (!cur.is_bv() || cur.get_sort().bv_size() == 0 || cur.get_sort().bv_size() > 64)
            return store(std::nullopt);
        if (auto it = vs_seed_.find(cur.id()); it != vs_seed_.end()) {
            if (it->second.empty()) return store(std::nullopt);
            return store(it->second);
        }
        uint64_t nlit = 0;
        if (cur.is_numeral() && cur.is_numeral_u64(nlit))
            return store(Vs{VsOpt{c_.bool_val(true), nlit}});
        // A previous full walk that succeeded can be reused. A cached miss is
        // not success and is not trusted here (this walk is a different one).
        if (auto it = vs_memo_.find(cur.id()); it != vs_memo_.end() && it->second && !it->second->empty())
            return store(*it->second);
        if (depth <= 0 || !cur.is_app() || cur.num_args() == 0) return store(std::nullopt);
        const auto kind = cur.decl().decl_kind();
        if (kind == Z3_OP_ITE) {
            if (cur.num_args() != 3) return store(std::nullopt);
            const z3::expr cnd0 = cur.arg(0);
            const z3::expr ta = peel_ite(cnd0, cur.arg(1), true), tb = peel_ite(cnd0, cur.arg(2), false);
            auto a = self(self, ta, depth - 1);
            if (!a || a->empty()) return store(std::nullopt);
            auto b = self(self, tb, depth - 1);
            if (!b || b->empty()) return store(std::nullopt);
            const z3::expr cnd = cur.arg(0);
            if (cnd.is_true()) return store(std::move(a));
            if (cnd.is_false()) return store(std::move(b));
            if (a->size() + b->size() > kVsMax) return store(std::nullopt);
            Vs r;
            r.reserve(a->size() + b->size());
            for (auto& o : *a) r.push_back(VsOpt{and2(cnd, o.guard), o.val});
            for (auto& o : *b) r.push_back(VsOpt{and2(!cnd, o.guard), o.val});
            return store(std::move(r));
        }
        if (cur.num_args() > 2) return store(std::nullopt);
        switch (kind) {
            case Z3_OP_BADD:
            case Z3_OP_BSUB:
            case Z3_OP_BMUL:
            case Z3_OP_BAND:
            case Z3_OP_BSDIV:
            case Z3_OP_BUDIV:
            case Z3_OP_BSHL:
            case Z3_OP_BLSHR:
            case Z3_OP_BASHR:
            case Z3_OP_SIGN_EXT:
            case Z3_OP_ZERO_EXT:
            case Z3_OP_EXTRACT:
            case Z3_OP_CONCAT:
                break;
            default: return store(std::nullopt);
        }
        // x & mask, x a leaf (a parameter, not a load). The result really is
        // one of the 2^popcount(mask) residues; the guard is that equality.
        // Do not do this once x itself has a value set: the product of that
        // set is smaller and the full mask would add values x cannot take.
        if (kind == Z3_OP_BAND && cur.num_args() == 2) {
            const bool m0 = cur.arg(0).is_numeral();
            const bool m1 = cur.arg(1).is_numeral();
            if (m0 != m1) {
                const z3::expr& other = cur.arg(m0 ? 1 : 0);
                const bool leaf = other.is_app() && other.num_args() == 0 && !other.is_numeral();
                const bool other_seeded = vs_seed_.find(other.id()) != vs_seed_.end();
                uint64_t mask = 0;
                if (leaf && !other_seeded && cur.arg(m0 ? 0 : 1).is_numeral_u64(mask)) {
                    const unsigned width = cur.get_sort().bv_size();
                    unsigned bits = 0;
                    for (unsigned b = 0; b < width && b < 64; ++b)
                        if (((mask >> b) & 1u) != 0u) ++bits;
                    if (bits <= 5) {
                        std::vector<unsigned> pos;
                        pos.reserve(bits);
                        for (unsigned b = 0; b < width && b < 64; ++b)
                            if (((mask >> b) & 1u) != 0u) pos.push_back(b);
                        Vs masked;
                        const unsigned n = 1u << bits;
                        masked.reserve(n);
                        for (unsigned s = 0; s < n; ++s) {
                            uint64_t val = 0;
                            for (unsigned k = 0; k < pos.size(); ++k)
                                if ((s & (1u << k)) != 0u) val |= uint64_t{1} << pos[k];
                            z3::expr g = n == 1 ? c_.bool_val(true) : (cur == c_.bv_val(val, width));
                            masked.push_back(VsOpt{std::move(g), val});
                        }
                        return store(std::move(masked));
                    }
                }
            }
        }
        std::vector<Vs> ops;
        ops.reserve(cur.num_args());
        for (unsigned i = 0; i < cur.num_args(); ++i) {
            auto s = self(self, cur.arg(i), depth - 1);
            if (!s || s->empty()) return store(std::nullopt);
            ops.push_back(std::move(*s));
        }
        int shape_i = -1;
        bool zip = true;
        std::size_t combos = 1;
        for (std::size_t i = 0; i < ops.size(); ++i) {
            if (ops[i].size() <= 1) continue;
            combos *= ops[i].size();
            if (shape_i < 0) shape_i = static_cast<int>(i);
            else if (ops[i].size() != ops[static_cast<std::size_t>(shape_i)].size()) zip = false;
            else
                for (std::size_t j = 0; j < ops[i].size() && zip; ++j)
                    if (ops[i][j].guard.id() != ops[static_cast<std::size_t>(shape_i)][j].guard.id()) zip = false;
        }
        if (!zip && combos > kVsCombos) return store(std::nullopt);
        const std::size_t n = shape_i < 0 ? 1 : zip ? ops[static_cast<std::size_t>(shape_i)].size() : combos;
        if (n == 0 || n > kVsMax) return store(std::nullopt);
        const z3::expr tt = c_.bool_val(true);
        Vs r;
        r.reserve(n);
        std::vector<std::size_t> idx(ops.size(), 0);
        for (std::size_t m = 0; m < n; ++m) {
            std::vector<uint64_t> vals;
            vals.reserve(ops.size());
            z3::expr g = tt;
            for (std::size_t i = 0; i < ops.size(); ++i) {
                const auto& a = ops[i];
                const auto& o = a.size() <= 1 ? a[0] : zip ? a[m] : a[idx[i]];
                vals.push_back(o.val);
                // Same rule as vs_raw: a zipped multi-set uses the shape
                // guard (size-1 guards are dropped, as there). A product
                // conjoins every guard.
                if (!zip || a.size() <= 1) g = and2(g, o.guard);
            }
            if (zip && shape_i >= 0) g = ops[static_cast<std::size_t>(shape_i)][m].guard;
            std::optional<uint64_t> kv = eval_const(cur, vals);
            if (!kv) {
                z3::expr_vector args(c_);
                for (std::size_t i = 0; i < vals.size(); ++i)
                    args.push_back(c_.bv_val(vals[i], cur.arg(static_cast<unsigned>(i)).get_sort().bv_size()));
                auto val = cur.decl()(args).simplify();
                uint64_t x = 0;
                if (val.is_numeral() && val.is_numeral_u64(x)) kv = x;
            }
            if (!kv) return store(std::nullopt);
            r.push_back(VsOpt{std::move(g), *kv});
            if (!zip) {
                for (std::size_t i = 0; i < idx.size(); ++i) {
                    if (ops[i].size() <= 1) continue;
                    if (++idx[i] < ops[i].size()) break;
                    idx[i] = 0;
                }
            }
        }
        if (r.empty() || r.size() > kVsMax) return store(std::nullopt);
        return store(std::move(r));
    };
    return rec(rec, e, depth);
}

void SymMem::seed_plus_const(const z3::expr& dst, const z3::expr& base, uint64_t delta) {
    if (delta == 0 || dst.id() == base.id()) return;
    if (vs_seed_.find(dst.id()) != vs_seed_.end()) return;
    auto it = vs_seed_.find(base.id());
    if (it == vs_seed_.end() || it->second.empty() || it->second.size() > kVsMax) return;
    Vs r;
    r.reserve(it->second.size());
    for (auto& o : it->second) r.push_back(VsOpt{o.guard, o.val + delta});
    seed_vs(dst, std::move(r));
}

void SymMem::materialize_vs(const z3::expr& e) {
    if (!e.is_bv() || e.get_sort().bv_size() == 0 || e.get_sort().bv_size() > 64) return;
    if (auto it = vs_seed_.find(e.id()); it != vs_seed_.end() && !it->second.empty()) return;
    // Copy into a local before seed_vs. Seeding used to keep a pointer into
    // vs_memo_/vs_seed_ and then erase that slot (use after free), and it
    // copied the ite's else-guard onto the index variable. The else of
    // PRISM_INDEX_LE3 is "not 0, 1, or 2", which is not "index == 3", so that
    // transfer is not an equality. Seed only the term itself.
    auto vs = shallow_vs(e, kShallowDepth);
    if (!vs || vs->empty() || vs->size() > kVsMax) return;
    seed_vs(e, std::move(*vs));
}

const SymMem::Vs* SymMem::vs_raw(const z3::expr& e) const {
    if (!e.is_bv() || e.get_sort().bv_size() > 64) return nullptr;
    if (auto it = vs_seed_.find(e.id()); it != vs_seed_.end()) return &it->second;
    if (auto it = vs_memo_.find(e.id()); it != vs_memo_.end()) return it->second ? &*it->second : nullptr;
    // a long read-over-write chain of a symbolic address nests one ite per
    // write: past this depth the term has no value set (the worker thread's
    // stack stays small; only precision is lost)
    if (vs_depth_ > kVsDepth) return nullptr;
    struct Depth {
        int& d;
        explicit Depth(int& x) : d(x) { ++d; }
        ~Depth() { --d; }
    } depth(vs_depth_);
    vs_keep_.push_back(e);  // ids are only stable while the ast lives
    vs_memo_.emplace(e.id(), std::nullopt);
    std::optional<Vs> out;
    uint64_t v = 0;
    const z3::expr tt = c_.bool_val(true);
    if (e.is_numeral()) {
        if (e.is_numeral_u64(v)) out = Vs{VsOpt{tt, v}};
    } else if (e.is_app() && e.num_args() > 0) {
        const auto k = e.decl().decl_kind();
        if (k == Z3_OP_ITE) {
            const auto cnd = e.arg(0);
            const z3::expr ta = peel_ite(cnd, e.arg(1), true), tb = peel_ite(cnd, e.arg(2), false);
            const Vs* a = vs_raw(ta);
            const Vs* b = a ? vs_raw(tb) : nullptr;
            if (a && b) {
                if (cnd.is_true()) out = *a;
                else if (cnd.is_false()) out = *b;
                else {
                    // too many options kept apart: the branches' merged sets
                    if (a->size() + b->size() > kVsMax) {
                        a = vs_of(ta);
                        b = vs_of(tb);
                    }
                    if (a && b && a->size() + b->size() <= kVsMax) {
                        Vs r;
                        for (auto& o : *a) r.push_back(VsOpt{and2(cnd, o.guard), o.val});
                        for (auto& o : *b) r.push_back(VsOpt{and2(!cnd, o.guard), o.val});
                        out = std::move(r);
                    }
                }
            }
        } else if (k == Z3_OP_EXTRACT && e.arg(0).is_app() && e.arg(0).num_args() > 0 &&
                   (e.arg(0).decl().decl_kind() == Z3_OP_ITE || e.arg(0).decl().decl_kind() == Z3_OP_CONCAT ||
                    e.arg(0).decl().decl_kind() == Z3_OP_EXTRACT)) {
            // push the extract inward: through an ite (both branches), into
            // the concat operand(s) it covers, or into an inner extract
            const auto x = e.arg(0);
            const unsigned hi = e.hi(), lo = e.lo();
            const auto xk = x.decl().decl_kind();
            if (xk == Z3_OP_ITE) {
                out = copy_vs(vs_raw(z3::ite(x.arg(0), x.arg(1).extract(hi, lo), x.arg(2).extract(hi, lo))));
            } else if (xk == Z3_OP_EXTRACT) {
                out = copy_vs(vs_raw(x.arg(0).extract(hi + x.lo(), lo + x.lo())));
            } else {
                // concat: operands msb first
                std::vector<z3::expr> parts;
                unsigned top = x.get_sort().bv_size();
                for (unsigned i = 0; i < x.num_args(); ++i) {
                    const unsigned w = x.arg(i).get_sort().bv_size();
                    const unsigned b_hi = top - 1, b_lo = top - w;  // bits of operand i
                    top -= w;
                    if (b_lo > hi || b_hi < lo) continue;
                    const unsigned h = std::min(hi, b_hi) - b_lo, l = std::max(lo, b_lo) - b_lo;
                    parts.push_back(h == w - 1 && l == 0 ? x.arg(i) : x.arg(i).extract(h, l));
                }
                if (parts.size() == 1) out = copy_vs(vs_raw(parts.at(0)));
                else {
                    z3::expr_vector pv(c_);
                    for (auto& pp : parts) pv.push_back(pp);
                    out = copy_vs(vs_raw(z3::concat(pv)));
                }
            }
        } else {
            // an operation on value-set operands: position by position when
            // every operand with several options has the same guard list,
            // else every combination (at most kVsCombos)
            std::vector<const Vs*> as;
            bool ok = true;
            const Vs* shape = nullptr;
            bool zip = true;
            std::size_t combos = 1;
            // too many raw combinations: the operands' merged sets (equal
            // values or-ed; exact, only fewer options)
            bool merged = false;
            for (int pass = 0; pass < 2 && !merged; ++pass) {
            as.clear(); ok = true; shape = nullptr; zip = true; combos = 1;
            for (unsigned i = 0; i < e.num_args() && ok; ++i) {
                const Vs* a = pass == 0 ? vs_raw(e.arg(i)) : vs_of(e.arg(i));
                if (!a) {
                    ok = false;
                    break;
                }
                as.push_back(a);
                if (a->size() > 1) {
                    combos *= a->size();
                    if (combos > 1u << 20) combos = 1u << 20;
                    if (!shape) shape = a;
                    else if (a->size() != shape->size()) zip = false;
                    else
                        for (std::size_t j = 0; j < a->size() && zip; ++j)
                            if ((*a)[j].guard.id() != (*shape)[j].guard.id()) zip = false;
                }
            }
            if (!ok || zip || combos <= kVsCombos) break;
            merged = pass == 1;
            }
            if (ok && (zip || combos <= kVsCombos)) {
                Vs r;
                const std::size_t n = !shape ? 1 : zip ? shape->size() : combos;
                std::vector<std::size_t> idx(as.size(), 0);
                for (std::size_t m = 0; m < n && ok; ++m) {
                    std::vector<uint64_t> vals;
                    z3::expr g = tt;
                    for (std::size_t i = 0; i < as.size(); ++i) {
                        const auto& a = *as.at(i);
                        const auto& o = a.size() == 1 ? a[0] : zip ? a[m] : a[idx[i]];
                        vals.push_back(o.val);
                        if (!zip || a.size() == 1) g = and2(g, o.guard);
                    }
                    if (zip && shape) g = (*shape)[m].guard;
                    auto kv = eval_const(e, vals);
                    if (!kv) {
                        z3::expr_vector args(c_);
                        for (std::size_t i = 0; i < vals.size(); ++i)
                            args.push_back(c_.bv_val(vals.at(i), e.arg(static_cast<unsigned>(i)).get_sort().bv_size()));
                        auto val = e.decl()(args).simplify();
                        uint64_t x = 0;
                        if (val.is_numeral() && val.is_numeral_u64(x)) kv = x;
                    }
                    if (!kv) ok = false;
                    else r.push_back(VsOpt{g, *kv});
                    if (!zip)
                        for (std::size_t i = 0; i < idx.size(); ++i) {  // next combination
                            if (as.at(i)->size() == 1) continue;
                            if (++idx.at(i) < as.at(i)->size()) break;
                            idx.at(i) = 0;
                        }
                }
                if (ok) out = std::move(r);
            }
        }
    }
    // Empty is a miss, not a value set. Caching it as a present optional makes
    // vs_of/read_cell_log/per_obj take front()/back() or skip the byte model.
    if (!out || out->empty() || out->size() > kVsMax) out.reset();
    auto& slot = vs_memo_[e.id()];
    slot = std::move(out);
    return slot ? &*slot : nullptr;
}

std::optional<SymMem::Vs> SymMem::copy_vs(const Vs* v) const {
    if (!v) return std::nullopt;
    return *v;
}

// The value set with equal values merged (their guards or-ed).
const SymMem::Vs* SymMem::vs_of(const z3::expr& e) const {
    if (!e.is_bv() || e.get_sort().bv_size() > 64) return nullptr;
    if (auto it = vs_merged_.find(e.id()); it != vs_merged_.end()) return it->second ? &*it->second : nullptr;
    const Vs* raw = vs_raw(e);
    std::optional<Vs> out;
    if (raw && !raw->empty()) {
        Vs r;
        for (auto& o : *raw) {
            auto it = std::ranges::find_if(r, [&](const VsOpt& x) { return x.val == o.val; });
            if (it == r.end()) r.push_back(o);
            else it->guard = it->guard || o.guard;
        }
        if (r.size() == 1) r[0].guard = c_.bool_val(true);  // the guards are exhaustive
        out = std::move(r);
    }
    vs_keep_.push_back(e);
    auto [it, _] = vs_merged_.emplace(e.id(), std::move(out));
    return it->second ? &*it->second : nullptr;
}

std::optional<std::vector<uint64_t>> SymMem::objs_of(const z3::expr& p) const {
    if (!p.is_bv() || p.get_sort().bv_size() != 64) return std::nullopt;
    const Vs* vs = vs_of(p);
    if (!vs) return std::nullopt;
    std::vector<uint64_t> os;
    for (auto& o : *vs) {
        const uint64_t id = obj_of(o.val);
        if (std::ranges::find(os, id) == os.end()) os.push_back(id);
    }
    return os;
}

std::optional<uint64_t> SymMem::const_obj(const z3::expr& p) { return known(p); }

template <class F>
z3::expr SymMem::per_obj(const z3::expr& ptr, const z3::expr& dflt, F&& f) {
    if (auto k = const_obj(ptr)) {
        if (*k >= 1 && *k <= objs_.size()) return f(objs_[static_cast<std::size_t>(*k - 1)]);
        return dflt;
    }
    if (const Vs* vs = vs_of(ptr); vs && !vs->empty()) {
        // one term per possible object, under the guards of its values
        auto at = [&](uint64_t v) -> z3::expr {
            const uint64_t k = obj_of(v);
            return k >= 1 && k <= objs_.size() ? f(objs_[static_cast<std::size_t>(k - 1)]) : dflt;
        };
        z3::expr r = at(vs->back().val);
        for (std::size_t i = vs->size() - 1; i-- > 0;) r = z3::ite((*vs)[i].guard, at((*vs)[i].val), r);
        return r;
    }
    z3::expr r = dflt;
    auto id = objid(ptr);
    for (auto& o : objs_) r = z3::ite(id == bv(o.id, 64 - kObjShift), f(o), r);
    return r;
}

z3::expr SymMem::in_range(const z3::expr& a, const z3::expr& base, const z3::expr& len) {
    return objid(a) == objid(base) && z3::ult(off(a) - off(base), len);
}

z3::expr SymMem::cell_of(const z3::expr& byte, const z3::expr& init, unsigned tag) {
    auto low = z3::concat(init, byte);
    if (!tags_) return low;
    return z3::concat(bv(tag, 4), low);
}

z3::expr SymMem::alloc(const z3::expr& guard, const z3::expr& size, MemKind k, unsigned align, int init) {
    uint64_t id = objs_.size() + 1;
    if (id >= kFnObjBase)  // ids from kFnObjBase up are function addresses
        throw EncodeError{std::string(laws::UNKNOWN),
                          "more than " + std::to_string(kFnObjBase - 1) + " memory objects in the unrolling"};
    auto p = bv(make_ptr(id, 0), 64);
    objs_.push_back(Obj{id, size, k, std::max(1u, align), guard});
    if (init == 1) {
        add_entry(Entry{Entry::Set, guard, p, size, p, cell_of(bv(0, 8), bv(1, 1), 0)});
    } else if (init == 2) {
        add_entry(Entry{Entry::Havoc, guard, p, size, p, bv(0, cw_)});
    }
    return p;
}

void SymMem::free(const z3::expr& guard, const z3::expr& ptr) {
    if (auto k = const_obj(ptr)) {
        if (*k >= 1 && *k <= objs_.size()) {
            auto& o = objs_[static_cast<std::size_t>(*k - 1)];
            o.alive = o.alive && !guard;
        }
        return;
    }
    if (const Vs* vs = vs_of(ptr)) {
        for (auto& o : objs_) {
            std::optional<z3::expr> hit;
            for (auto& opt : *vs)
                if (obj_of(opt.val) == o.id) hit = hit ? *hit || opt.guard : opt.guard;
            if (hit) o.alive = o.alive && !(guard && *hit);
        }
        return;
    }
    auto id = objid(ptr);
    for (auto& o : objs_) o.alive = o.alive && !(guard && id == bv(o.id, 64 - kObjShift));
}

void SymMem::revive(const z3::expr& guard, const z3::expr& ptr) {
    if (auto k = const_obj(ptr)) {
        if (*k >= 1 && *k <= objs_.size()) {
            auto& o = objs_[static_cast<std::size_t>(*k - 1)];
            if (o.kind == MemKind::Stack) o.alive = o.alive || guard;
        }
        return;
    }
    auto id = objid(ptr);
    for (auto& o : objs_)
        if (o.kind == MemKind::Stack) o.alive = o.alive || (guard && id == bv(o.id, 64 - kObjShift));
}

z3::expr SymMem::stack_save() { return bv(objs_.size(), 64); }

void SymMem::stack_restore(const z3::expr& guard, const z3::expr& token) {
    for (auto& o : objs_)
        if (o.kind == MemKind::Stack) o.alive = o.alive && !(guard && z3::ult(token, bv(o.id, 64)));
}

z3::expr SymMem::size(const z3::expr& ptr) {
    return per_obj(ptr, bv(0, 64), [](const Obj& o) { return o.size; });
}

z3::expr SymMem::live(const z3::expr& ptr) {
    auto b = per_obj(ptr, c_.bool_val(false), [](const Obj& o) { return o.alive; });
    return z3::ite(b, bv(1, 1), bv(0, 1));
}

z3::expr SymMem::kind(const z3::expr& ptr) {
    return per_obj(ptr, bv(0, 8), [&](const Obj& o) { return bv(static_cast<uint64_t>(o.kind), 8); });
}

z3::expr SymMem::align(const z3::expr& ptr) {
    return per_obj(ptr, bv(1, 64), [&](const Obj& o) { return bv(o.align, 64); });
}

std::size_t SymMem::havoc_objects(const z3::expr& guard, const std::vector<uint64_t>& ids, bool all,
                                  bool keep_init) {
    std::vector<uint64_t> todo;
    if (all) {
        for (auto& o : objs_)
            if (o.kind != MemKind::Const) todo.push_back(o.id);
    } else {
        todo = ids;
    }
    for (auto id : todo) {
        auto p = bv(make_ptr(id, 0), 64);  // numeral: known(p) == id
        Entry e{Entry::HavocObj, guard, p, bv(0, 64), p, bv(0, cw_)};
        e.keep = keep_init;
        add_entry(std::move(e));
    }
    return todo.size();
}

std::size_t SymMem::havoc_pointed(const z3::expr& guard, const std::vector<z3::expr>& ptrs, bool keep_init) {
    for (auto& p : ptrs) {
        keep_.push_back(p);
        Entry e{Entry::HavocObj, guard, p, bv(0, 64), p, bv(0, cw_)};
        e.keep = keep_init;
        add_entry(std::move(e));
    }
    return ptrs.size();
}

void SymMem::add_entry(Entry e) {
    if (enc_ == MemEncoding::Bv) {
        if (e.kind == Entry::Havoc || e.kind == Entry::HavocObj) {
            e.havoc = static_cast<int>(havoc_reads_.size());
            havoc_reads_.emplace_back();
        }
        log_.push_back(std::move(e));
        return;
    }
    Entry live = std::move(e);
    auto& A = *arr_;
    if (live.kind == Entry::Byte) {
        A = z3::ite(live.guard, z3::store(A, live.addr, live.cell), A);
        return;
    }
    auto x = c_.bv_const(("mem!x" + std::to_string(fresh_++)).c_str(), 64);
    if (live.kind == Entry::HavocObj) {
        auto h = c_.constant(("mem!havocobj" + std::to_string(fresh_++)).c_str(),
                             c_.array_sort(c_.bv_sort(64), c_.bv_sort(cw_)));
        z3::expr val = z3::select(h, x);
        if (live.keep) val = val | (z3::select(A, x) & bv(uint64_t{1} << kCellInit, cw_));
        A = z3::lambda(x, z3::ite(live.guard && objid(x) == objid(live.addr), val, z3::select(A, x)));
        return;
    }
    z3::expr hit = live.guard && in_range(x, live.addr, live.len);
    z3::expr val = live.cell;
    if (live.kind == Entry::Copy) {
        val = z3::select(A, live.src + (x - live.addr));
    } else if (live.kind == Entry::Havoc) {
        auto h = c_.constant(("mem!havoc" + std::to_string(fresh_++)).c_str(),
                             c_.array_sort(c_.bv_sort(64), c_.bv_sort(8)));
        val = cell_of(z3::select(h, x), bv(1, 1), 0);
    }
    A = z3::lambda(x, z3::ite(hit, val, z3::select(A, x)));
}

z3::expr SymMem::read_cell(const z3::expr& addr) {
    if (enc_ == MemEncoding::Array) return z3::select(*arr_, addr);
    return read_cell_log(addr, log_.size());
}

// (guard, dst, src) options of a copy whose destination and source both have
// value sets: zipped when they share their guards, else the product (at most
// kVsCombos). Guards are exhaustive. nullopt: no such finite description.
std::optional<std::vector<std::tuple<z3::expr, uint64_t, uint64_t>>> SymMem::copy_alts(const z3::expr& dst,
                                                                                       const z3::expr& src) const {
    const Vs* dv = vs_of(dst);
    if (!dv || dv->empty()) return std::nullopt;
    const Vs* sv = vs_of(src);
    if (!sv || sv->empty()) return std::nullopt;
    std::vector<std::tuple<z3::expr, uint64_t, uint64_t>> out;
    const z3::expr tt = c_.bool_val(true);
    bool zip = dv->size() == sv->size() && dv->size() > 1;
    for (std::size_t j = 0; zip && j < dv->size(); ++j)
        if ((*dv)[j].guard.id() != (*sv)[j].guard.id()) zip = false;
    if (zip) {
        for (std::size_t j = 0; j < dv->size(); ++j) out.emplace_back((*dv)[j].guard, (*dv)[j].val, (*sv)[j].val);
        return out;
    }
    if (dv->size() * sv->size() > kVsCombos) return std::nullopt;
    for (auto& d : *dv)
        for (auto& s : *sv) {
            z3::expr g = dv->size() > 1 ? d.guard : tt;
            if (sv->size() > 1) g = and2(g, s.guard);
            out.emplace_back(g, d.val, s.val);
        }
    return out;
}

z3::expr SymMem::read_cell_log(const z3::expr& addr_in, std::size_t upto, bool use_prov) {
    auto key = std::make_tuple(addr_in.id(), upto, use_prov);
    if (auto it = memo_.find(key); it != memo_.end()) return it->second;
    mem_guard();
    keep_.push_back(addr_in);
    // Value sets: an address with several possible constants reads each of
    // them (under its guard); one constant is read as that numeral, so the
    // comparisons with other constant or value-set addresses are decided
    // here instead of by the solver.
    z3::expr addr = addr_in;
    std::optional<uint64_t> caddr;
    if (const Vs* av = vs_of(addr_in); av && !av->empty()) {
        if (av->size() > 1) {
            z3::expr v = read_cell_log(bv(av->back().val, 64), upto, use_prov);
            for (std::size_t i = av->size() - 1; i-- > 0;)
                v = z3::ite((*av)[i].guard, read_cell_log(bv((*av)[i].val, 64), upto, use_prov), v);
            memo_.emplace(key, v);
            return v;
        }
        caddr = av->front().val;
        addr = bv(*caddr, 64);
    }
    // the condition "entry address == addr", decided when both are known
    auto same = [&](const z3::expr& ea) -> std::optional<z3::expr> {
        if (!caddr) return addr == ea;
        const Vs* ev = vs_of(ea);
        if (!ev) return addr == ea;
        std::optional<z3::expr> hit;
        for (auto& o : *ev)
            if (o.val == *caddr)
                hit = hit.has_value() ? std::optional<z3::expr>(*hit || o.guard) : std::optional<z3::expr>(o.guard);
        return hit;  // nullopt: never equal
    };
    // false when the entry's object is known and differs from addr's
    auto other_obj = [&](const z3::expr& ea) {
        if (!caddr) return false;
        auto os = objs_of(ea);
        return os && std::find(os->begin(), os->end(), obj_of(*caddr)) == os->end();
    };
    auto ko = use_prov ? known(addr) : std::nullopt;
    z3::expr v = bv(0, cw_);  // never written: value 0, not initialised
    for (std::size_t i = 0; i < upto && i < log_.size(); ++i) {
        auto& e = log_.at(i);
        if (ko) {
            auto kb = known(e.addr);
            if (kb && *kb != *ko) continue;
        }
        if (e.kind != Entry::Byte && other_obj(e.addr)) continue;
        switch (e.kind) {
            case Entry::Byte: {
                auto eq = same(e.addr);
                if (!eq) break;
                v = z3::ite(and2(e.guard, *eq), e.cell, v);
                break;
            }
            case Entry::Set: v = z3::ite(e.guard && in_range(addr, e.addr, e.len), e.cell, v); break;
            case Entry::Copy: {
                // Constant address, destination and source with value sets
                // (e.g. a memmove at a PRISM_INDEX_LE3 position): read each
                // constant source byte instead of the symbolic src + (addr -
                // dst), whose read-over-write chain has no value set and
                // nests one level per earlier copy. Exact: the options'
                // guards are exhaustive, and per option the in-range test and
                // the source address are those of the original term.
                if (caddr) {
                    if (auto alts = copy_alts(e.addr, e.src)) {
                        const z3::expr ca = bv(*caddr, 64);
                        std::optional<z3::expr> chain;
                        for (std::size_t k = alts->size(); k-- > 0;) {
                            const auto& [g, dv, sv] = (*alts)[k];
                            z3::expr inr = in_range(ca, bv(dv, 64), e.len).simplify();
                            z3::expr cand = v;
                            if (!inr.is_false()) {
                                auto rd = read_cell_log(bv(sv + (*caddr - dv), 64), i, use_prov);
                                cand = inr.is_true() ? rd : z3::ite(inr, rd, v);
                            }
                            chain = chain.has_value() ? z3::ite(g, cand, *chain) : cand;
                        }
                        v = z3::ite(e.guard, *chain, v);
                        break;
                    }
                }
                auto src = e.src + (addr - e.addr);
                if (use_prov)
                    if (auto ks = known(e.src)) note(src, *ks);
                v = z3::ite(e.guard && in_range(addr, e.addr, e.len), read_cell_log(src, i, use_prov), v);
                break;
            }
            case Entry::Havoc: {
                auto h = havoc_read(e, addr, 8, "mem!h");
                v = z3::ite(e.guard && in_range(addr, e.addr, e.len), cell_of(h, bv(1, 1), 0), v);
                break;
            }
            case Entry::HavocObj: {
                // one arbitrary cell per address (value, initialised, tag)
                auto h = havoc_read(e, addr, cw_, "mem!ho");
                z3::expr cell = h;
                if (e.keep) cell = h | (v & bv(uint64_t{1} << kCellInit, cw_));
                v = z3::ite(e.guard && objid(addr) == objid(e.addr), cell, v);
                break;
            }
        }
    }
    memo_.emplace(key, v);
    return v;
}

// The arbitrary content of havoc entry e at addr: one value per address.
z3::expr SymMem::havoc_read(const Entry& e, const z3::expr& addr, unsigned width, const char* base) {
    // the same address expression reads the same value (no new constraints)
    auto key = std::make_pair(e.havoc, addr.id());
    if (auto it = havoc_memo_.find(key); it != havoc_memo_.end()) return it->second;
    mem_guard();
    auto h = c_.bv_const((std::string(base) + std::to_string(fresh_++)).c_str(), width);
    auto& reads = havoc_reads_[static_cast<std::size_t>(e.havoc)];
    uint64_t x1 = 0, x2 = 0;
    const bool n1 = addr.is_numeral() && addr.is_numeral_u64(x1);
    for (auto& [a2, h2] : reads) {
        if (n1 && a2.is_numeral() && a2.is_numeral_u64(x2) && x1 != x2) continue;  // never the same address
        side_.push_back(z3::implies(addr == a2, h == h2));
    }
    reads.emplace_back(addr, h);
    keep_.push_back(addr);
    havoc_memo_.emplace(key, h);
    return h;
}

// The n bytes at constant address a as one value (bits 8n-1..0, byte a in
// the low bits), built from the log entries before `upto` word by word:
// every entry that may write one of the bytes must write all of them
// (a store at least as wide containing them, a memset/memcpy/havoc of a
// constant length containing them, or a whole-object havoc); else nullopt
// and the caller keeps the byte-level value. Byte for byte this is the
// read-over-write chain of read_cell_log: an entry skipped here writes none
// of the bytes (a different address or object, decided on constants, or
// provenance as there), and a taken entry's value is the chain's cell value.
std::optional<z3::expr> SymMem::word_at(uint64_t a, unsigned n, std::size_t upto) {
    const uint64_t ko = obj_of(a);
    z3::expr v = bv(0, 8 * n);
    for (std::size_t i = 0; i < upto; ++i) {
        auto& e = log_[i];
        if (auto kb = known(e.addr); kb && *kb != ko) continue;
        const Vs* ev = vs_of(e.addr);
        if (e.kind == Entry::Byte) {
            if (!ev) return std::nullopt;  // an address without a value set
            for (auto& o : *ev) {
                if (o.val < a || o.val - a >= n) continue;  // this byte is not in [a, a + n)
                // in range: only a store that contains [a, a + n)
                if (e.grp < 0) return std::nullopt;  // byte entry outside a store
                const auto& st = stores_[static_cast<std::size_t>(e.grp)];
                const uint64_t b = o.val - e.grp_k;  // the store's first byte
                if (b > a || a - b + n > st.n) return std::nullopt;  // partial overlap
                if (o.val != a) continue;  // taken at the store's byte at a
                const unsigned lo = static_cast<unsigned>(8 * (a - b));
                auto x = st.n == n ? st.val : st.val.extract(lo + 8 * n - 1, lo);
                v = z3::ite(and2(e.guard, o.guard), x, v);
            }
            continue;
        }
        // ranged entries
        if (e.kind == Entry::HavocObj) {
            auto os = objs_of(e.addr);
            if (os && std::find(os->begin(), os->end(), ko) == os->end()) continue;
            if (!os || os->size() != 1) return std::nullopt;  // whole-object havoc of an unknown object
            std::optional<z3::expr> val;
            for (unsigned k = 0; k < n; ++k) {
                auto h = havoc_read(e, bv(a + k, 64), cw_, "mem!ho").extract(7, 0);
                if (!val.has_value()) val = h;
                else val = z3::concat(h, *val);
            }
            if (!val.has_value()) return std::nullopt;
            v = z3::ite(e.guard, *val, v);
            continue;
        }
        uint64_t len = 0;
        if (!ev || !(e.len.is_numeral() && e.len.is_numeral_u64(len)) || len >= (uint64_t{1} << 47)) {
            auto os = objs_of(e.addr);
            if (os && std::find(os->begin(), os->end(), ko) == os->end()) continue;
            return std::nullopt;  // ranged entry of a symbolic address or length
        }
        for (auto& o : *ev) {
            if (obj_of(o.val) != ko) continue;
            // [o.val, o.val + len) against [a, a + n)
            if (o.val + len <= a || o.val >= a + n) continue;
            if (!(o.val <= a && a + n <= o.val + len)) return std::nullopt;  // ranged entry covers part of the word
            z3::expr x = bv(0, 8 * n);
            if (e.kind == Entry::Set) {
                auto byte = e.cell.extract(7, 0);
                std::optional<z3::expr> val;
                for (unsigned k = 0; k < n; ++k) {
                    if (!val.has_value()) val = byte;
                    else val = z3::concat(byte, *val);
                }
                x = *val;
            } else if (e.kind == Entry::Havoc) {
                std::optional<z3::expr> val;
                for (unsigned k = 0; k < n; ++k) {
                    auto h = havoc_read(e, bv(a + k, 64), 8, "mem!h");
                    if (!val.has_value()) val = h;
                    else val = z3::concat(h, *val);
                }
                x = *val;
            } else {  // Copy: the source bytes at the time of the copy
                const Vs* sv = vs_of(e.src);
                if (!sv) return std::nullopt;  // copy from a source without a value set
                std::vector<std::pair<z3::expr, z3::expr>> alts;
                for (auto& so : *sv) {
                    auto w = word_split(so.val + (a - o.val), n, i);
                    if (!w) return std::nullopt;
                    alts.emplace_back(so.guard, *w);
                }
                x = alts.back().second;
                for (std::size_t j = alts.size() - 1; j-- > 0;) x = z3::ite(alts[j].first, alts[j].second, x);
            }
            v = z3::ite(and2(e.guard, o.guard), x, v);
        }
    }
    return v;
}

// word_at, else the two halves word by word (a value assembled from
// narrower stores, e.g. a struct's fields copied as one integer).
std::optional<z3::expr> SymMem::word_split(uint64_t a, unsigned n, std::size_t upto) {
    const auto key = std::make_tuple(a, n, upto);
    if (auto it = word_memo_.find(key); it != word_memo_.end()) return it->second;
    auto w = word_at(a, n, upto);
    if (!w && n > 1) {
        const unsigned h = n / 2;
        auto lo = word_split(a, h, upto);
        auto hi = lo ? word_split(a + h, n - h, upto) : std::nullopt;
        if (lo && hi) w = z3::concat(*hi, *lo);
    }
    word_memo_.emplace(key, w);
    return w;
}

SymMem::Loaded SymMem::load(const z3::expr& ptr, unsigned width, unsigned tag) {
    materialize_vs(ptr);
    unsigned n = (width + 7) / 8;
    // word level (Bv, value sets): the value as an ite over whole stores
    std::optional<z3::expr> word;
    if (enc_ == MemEncoding::Bv && (n == 2 || n == 4 || n == 8))
        if (const Vs* av = vs_of(ptr); av && !av->empty()) {
            std::vector<z3::expr> ws;
            for (auto& o : *av) {
                auto w = word_split(o.val, n, log_.size());
                if (!w) break;
                ws.push_back(*w);
            }
            if (ws.size() == av->size()) {
                z3::expr r = ws.back();
                for (std::size_t i = ws.size() - 1; i-- > 0;) r = z3::ite(av->at(i).guard, ws.at(i), r);
                if (8 * n > width) r = r.extract(width - 1, 0);
                word = r;
            }
        }
    auto ko = known(ptr);
    std::optional<z3::expr> val, mask;
    z3::expr uninit = c_.bool_val(false);
    z3::expr tagbad = c_.bool_val(false);
    for (unsigned k = 0; k < n; ++k) {
        auto a = k == 0 ? ptr : ptr + bv(k, 64);
        if (k != 0) seed_plus_const(a, ptr, k);
        if (ko) note(a, *ko);
        auto cell = read_cell(a);
        auto byte = cell.extract(7, 0);
        if (!val.has_value()) val = byte;
        else val = z3::concat(byte, *val);
        auto u = ~cell.extract(kCellInit, kCellInit);
        if (!mask.has_value()) mask = u;
        else mask = z3::concat(u, *mask);
        uninit = uninit || cell.extract(kCellInit, kCellInit) == bv(0, 1);
        if (tags_ && tag != 0) {
            auto t = cell.extract(kTagLo + 3, kTagLo);
            tagbad = tagbad || (t != bv(0, 4) && t != bv(tag, 4));
        }
    }
    auto v = *val;
    if (8 * n > width) v = v.extract(width - 1, 0);
    if (word) {
        v = *word;
        // A pointer stored and loaded as one word keeps that value set for the
        // next address calculation, without walking the byte-level term.
        materialize_vs(*word);
    }
    return Loaded{v, z3::ite(uninit, bv(1, 1), bv(0, 1)), z3::ite(tagbad, bv(1, 1), bv(0, 1)), *mask};
}

SymMem::Loaded SymMem::load_exact(const Mark& m, const z3::expr& ptr, unsigned width) {
    unsigned n = (width + 7) / 8;
    std::optional<z3::expr> val;
    z3::expr uninit = c_.bool_val(false);
    for (unsigned k = 0; k < n; ++k) {
        auto a = k == 0 ? ptr : ptr + bv(k, 64);
        auto cell = enc_ == MemEncoding::Array ? z3::select(*m.arr, a) : read_cell_log(a, m.upto, false);
        auto byte = cell.extract(7, 0);
        if (!val.has_value()) val = byte;
        else val = z3::concat(byte, *val);
        uninit = uninit || cell.extract(kCellInit, kCellInit) == bv(0, 1);
    }
    auto v = *val;
    if (8 * n > width) v = v.extract(width - 1, 0);
    auto one = z3::ite(uninit, bv(1, 1), bv(0, 1));
    return Loaded{v, one, bv(0, 1), one};
}

void SymMem::store(const z3::expr& guard, const z3::expr& ptr, const z3::expr& val, unsigned width,
                   const z3::expr& init, unsigned tag) {
    materialize_vs(ptr);
    unsigned n = (width + 7) / 8;
    auto ko = known(ptr);
    auto wide = 8 * n > width ? z3::zext(val, 8 * n - width) : val;
    // init: one bit for all bytes, or one bit per byte
    auto init_k = [&](unsigned k) { return init.get_sort().bv_size() == 1 ? init : init.extract(k, k); };
    if (enc_ == MemEncoding::Array) {
        auto A = *arr_;
        for (unsigned k = 0; k < n; ++k) {
            auto a = k == 0 ? ptr : ptr + bv(k, 64);
            A = z3::store(A, a, cell_of(wide.extract(8 * k + 7, 8 * k), init_k(k), tag));
        }
        arr_ = z3::ite(guard, A, *arr_);
        return;
    }
    const int grp = static_cast<int>(stores_.size());
    stores_.push_back(StoreRec{wide, n});
    for (unsigned k = 0; k < n; ++k) {
        auto a = k == 0 ? ptr : ptr + bv(k, 64);
        if (k != 0) seed_plus_const(a, ptr, k);
        if (ko) note(a, *ko);
        Entry e{Entry::Byte, guard, a, bv(1, 64), a, cell_of(wide.extract(8 * k + 7, 8 * k), init_k(k), tag)};
        e.grp = grp;
        e.grp_k = k;
        add_entry(std::move(e));
    }
}

void SymMem::copy(const z3::expr& guard, const z3::expr& dst, const z3::expr& src, const z3::expr& len) {
    add_entry(Entry{Entry::Copy, guard, dst, len, src, bv(0, cw_)});
}

void SymMem::set(const z3::expr& guard, const z3::expr& dst, const z3::expr& byte, const z3::expr& len) {
    add_entry(Entry{Entry::Set, guard, dst, len, dst, cell_of(byte, bv(1, 1), 0)});
}

#endif  // PRISM_HAS_Z3

// ---------------------------------------------------------------------------
// Concrete memory
// ---------------------------------------------------------------------------

std::optional<uint64_t> ConcMem::alloc(uint64_t size, MemKind k, unsigned align, int init) {
    if (size > (uint64_t{1} << 24) || objs.size() + 1 >= kFnObjBase) return std::nullopt;
    Obj o;
    o.size = size;
    o.live = true;
    o.kind = k;
    o.align = std::max(1u, align);
    o.cells.resize(static_cast<std::size_t>(size));
    for (auto& c : o.cells) {
        if (init == 1) c.init = true;
        if (init == 2) {
            c.init = true;
            c.taint = true;
        }
    }
    objs.push_back(std::move(o));
    return make_ptr(objs.size(), 0);
}

const ConcMem::Obj* ConcMem::obj(uint64_t ptr) const {
    auto id = obj_of(ptr);
    if (id == 0 || id > objs.size()) return nullptr;
    return &objs[static_cast<std::size_t>(id - 1)];
}

void ConcMem::free(uint64_t ptr) {
    auto id = obj_of(ptr);
    if (id == 0 || id > objs.size()) return;
    objs[static_cast<std::size_t>(id - 1)].live = false;
}

void ConcMem::revive(uint64_t ptr) {
    auto id = obj_of(ptr);
    if (id == 0 || id > objs.size()) return;
    auto& o = objs[static_cast<std::size_t>(id - 1)];
    if (o.kind == MemKind::Stack) o.live = true;
}

void ConcMem::stack_restore(uint64_t token) {
    for (std::size_t i = 0; i < objs.size(); ++i)
        if (objs[i].kind == MemKind::Stack && token < i + 1) objs[i].live = false;
}

uint64_t ConcMem::size(uint64_t ptr) const {
    auto* o = obj(ptr);
    return o ? o->size : 0;
}
bool ConcMem::live(uint64_t ptr) const {
    auto* o = obj(ptr);
    return o && o->live;
}
uint64_t ConcMem::kind(uint64_t ptr) const {
    auto* o = obj(ptr);
    return o ? static_cast<uint64_t>(o->kind) : 0;
}
uint64_t ConcMem::align(uint64_t ptr) const {
    auto* o = obj(ptr);
    return o ? o->align : 1;
}

ConcMem::Cell ConcMem::read(uint64_t addr) const {
    auto* o = obj(addr);
    auto off = off_of(addr);
    if (!o || off >= o->cells.size()) return Cell{};
    return o->cells[static_cast<std::size_t>(off)];
}

void ConcMem::write(uint64_t addr, Cell c) {
    auto id = obj_of(addr);
    if (id == 0 || id > objs.size()) return;
    auto& o = objs[static_cast<std::size_t>(id - 1)];
    auto off = off_of(addr);
    if (off >= o.cells.size()) return;
    o.cells[static_cast<std::size_t>(off)] = c;
}

}  // namespace mem
}  // namespace prism::pir
