// PIR memory model: symbolic memory for the encoder (SymMem) and concrete
// memory for the interpreter (ConcMem). See memory.hpp and docs/PIR.md
// "Memory model"; the Lean counterpart is proofs/semantics/PrismSem/Memory.lean
// (semantics) and MemEncode.lean (read-over-write encoding).

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

const SymMem::Vs* SymMem::vs_of(const z3::expr& e) const {
    if (!e.is_bv() || e.get_sort().bv_size() > 64) return nullptr;
    if (auto it = vs_memo_.find(e.id()); it != vs_memo_.end()) return it->second ? &*it->second : nullptr;
    vs_keep_.push_back(e);  // ids are only stable while the ast lives
    std::optional<Vs> out;
    uint64_t v = 0;
    auto add = [&](Vs& r, const z3::expr& g, uint64_t k) -> bool {
        for (auto& o : r)
            if (o.val == k) {
                o.guard = o.guard || g;
                return true;
            }
        if (r.size() >= kVsMax) return false;
        r.push_back(VsOpt{g, k});
        return true;
    };
    if (e.is_numeral()) {
        if (e.is_numeral_u64(v)) out = Vs{VsOpt{c_.bool_val(true), v}};
    } else if (e.is_app() && e.num_args() > 0) {
        const auto k = e.decl().decl_kind();
        if (k == Z3_OP_ITE) {
            const Vs* a = vs_of(e.arg(1));
            const Vs* b = a ? vs_of(e.arg(2)) : nullptr;
            if (a && b) {
                Vs r;
                const auto cnd = e.arg(0);
                bool ok = true;
                for (auto& o : *a) ok = ok && add(r, and2(cnd, o.guard), o.val);
                for (auto& o : *b) ok = ok && add(r, and2(!cnd, o.guard), o.val);
                if (ok) out = std::move(r);
            }
        } else {
            // an operation on value-set operands: every combination, evaluated
            std::vector<const Vs*> as;
            std::size_t combos = 1;
            bool ok = true;
            for (unsigned i = 0; i < e.num_args() && ok; ++i) {
                const Vs* a = vs_of(e.arg(i));
                if (!a) ok = false;
                else {
                    as.push_back(a);
                    combos *= a->size();
                    if (combos > kVsMax) ok = false;
                }
            }
            if (ok) {
                Vs r;
                std::vector<std::size_t> idx(as.size(), 0);
                const auto f = e.decl();
                for (std::size_t n = 0; n < combos && ok; ++n) {
                    z3::expr_vector args(c_);
                    z3::expr g = c_.bool_val(true);
                    for (std::size_t i = 0; i < as.size(); ++i) {
                        const auto& o = (*as[i])[idx[i]];
                        args.push_back(c_.bv_val(o.val, e.arg(static_cast<unsigned>(i)).get_sort().bv_size()));
                        g = and2(g, o.guard);
                    }
                    auto val = f(args).simplify();
                    uint64_t kv = 0;
                    if (!(val.is_numeral() && val.is_numeral_u64(kv))) ok = false;
                    else ok = add(r, g, kv);
                    for (std::size_t i = 0; i < idx.size(); ++i) {  // next combination
                        if (++idx[i] < as[i]->size()) break;
                        idx[i] = 0;
                    }
                }
                if (ok) out = std::move(r);
            }
        }
    }
    auto [it, _] = vs_memo_.emplace(e.id(), std::move(out));
    return it->second ? &*it->second : nullptr;
}

std::optional<std::vector<uint64_t>> SymMem::objs_of(const z3::expr& p) const {
    if (!p.is_bv() || p.get_sort().bv_size() != 64) return std::nullopt;
    const Vs* vs = vs_of(p);
    if (!vs) return std::nullopt;
    std::vector<uint64_t> os;
    for (auto& o : *vs)
        if (std::find(os.begin(), os.end(), obj_of(o.val)) == os.end()) os.push_back(obj_of(o.val));
    return os;
}

std::optional<uint64_t> SymMem::const_obj(const z3::expr& p) { return known(p); }

template <class F>
z3::expr SymMem::per_obj(const z3::expr& ptr, const z3::expr& dflt, F&& f) {
    if (auto k = const_obj(ptr)) {
        if (*k >= 1 && *k <= objs_.size()) return f(objs_[static_cast<std::size_t>(*k - 1)]);
        return dflt;
    }
    if (const Vs* vs = vs_of(ptr)) {
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
    auto& A = *arr_;
    if (e.kind == Entry::Byte) {
        A = z3::ite(e.guard, z3::store(A, e.addr, e.cell), A);
        return;
    }
    auto x = c_.bv_const(("mem!x" + std::to_string(fresh_++)).c_str(), 64);
    if (e.kind == Entry::HavocObj) {
        auto h = c_.constant(("mem!havocobj" + std::to_string(fresh_++)).c_str(),
                             c_.array_sort(c_.bv_sort(64), c_.bv_sort(cw_)));
        z3::expr val = z3::select(h, x);
        if (e.keep) val = val | (z3::select(A, x) & bv(uint64_t{1} << kCellInit, cw_));
        A = z3::lambda(x, z3::ite(e.guard && objid(x) == objid(e.addr), val, z3::select(A, x)));
        return;
    }
    z3::expr hit = e.guard && in_range(x, e.addr, e.len);
    z3::expr val = e.cell;
    if (e.kind == Entry::Copy) {
        val = z3::select(A, e.src + (x - e.addr));
    } else if (e.kind == Entry::Havoc) {
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

z3::expr SymMem::read_cell_log(const z3::expr& addr_in, std::size_t upto, bool use_prov) {
    auto key = std::make_tuple(addr_in.id(), upto, use_prov);
    if (auto it = memo_.find(key); it != memo_.end()) return it->second;
    keep_.push_back(addr_in);
    // Value sets: an address with several possible constants reads each of
    // them (under its guard); one constant is read as that numeral, so the
    // comparisons with other constant or value-set addresses are decided
    // here instead of by the solver.
    z3::expr addr = addr_in;
    std::optional<uint64_t> caddr;
    if (const Vs* av = vs_of(addr_in)) {
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
            if (o.val == *caddr) hit = hit ? *hit || o.guard : o.guard;
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
    for (std::size_t i = 0; i < upto; ++i) {
        auto& e = log_[i];
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

// The n bytes at constant address a as one value, when every write before
// `upto` that may reach them is a whole n-byte store at a (else nullopt):
// ite(cond_j, value_j, ...) over those stores, latest first, 0 when none
// wrote them. Byte for byte this is the read-over-write chain: an entry
// skipped here writes none of the bytes (a different address or object,
// decided on constants, or provenance as in read_cell_log).
std::optional<z3::expr> SymMem::word_at(uint64_t a, unsigned n, std::size_t upto) {
    const uint64_t ko = obj_of(a);
    z3::expr v = bv(0, 8 * n);
    for (std::size_t i = 0; i < upto; ++i) {
        auto& e = log_[i];
        if (auto kb = known(e.addr); kb && *kb != ko) continue;
        if (e.kind != Entry::Byte) {
            auto os = objs_of(e.addr);
            if (os && std::find(os->begin(), os->end(), ko) == os->end()) continue;
            return std::nullopt;  // a ranged write into a's object
        }
        const Vs* ev = vs_of(e.addr);
        if (!ev) return std::nullopt;  // an address without a value set
        std::optional<z3::expr> full;
        for (auto& o : *ev) {
            if (o.val < a || o.val - a >= n) continue;  // this byte is not in [a, a + n)
            // in range: only a whole n-byte store starting at a
            if (e.grp < 0 || stores_[static_cast<std::size_t>(e.grp)].n != n || o.val - e.grp_k != a) return std::nullopt;
            full = full ? *full || o.guard : o.guard;
        }
        if (!full || e.grp_k != 0) continue;  // the store's value is taken at its first byte
        v = z3::ite(and2(e.guard, *full), stores_[static_cast<std::size_t>(e.grp)].val, v);
    }
    return v;
}

SymMem::Loaded SymMem::load(const z3::expr& ptr, unsigned width, unsigned tag) {
    unsigned n = (width + 7) / 8;
    // word level (Bv, value sets): the value as an ite over whole stores
    std::optional<z3::expr> word;
    if (enc_ == MemEncoding::Bv && (n == 2 || n == 4 || n == 8))
        if (const Vs* av = vs_of(ptr)) {
            std::vector<z3::expr> ws;
            for (auto& o : *av) {
                auto w = word_at(o.val, n, log_.size());
                if (!w) break;
                ws.push_back(*w);
            }
            if (ws.size() == av->size()) {
                z3::expr r = ws.back();
                for (std::size_t i = ws.size() - 1; i-- > 0;) r = z3::ite((*av)[i].guard, ws[i], r);
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
        if (ko) note(a, *ko);
        auto cell = read_cell(a);
        auto byte = cell.extract(7, 0);
        val = val ? z3::concat(byte, *val) : byte;
        auto u = ~cell.extract(kCellInit, kCellInit);
        mask = mask ? z3::concat(u, *mask) : u;
        uninit = uninit || cell.extract(kCellInit, kCellInit) == bv(0, 1);
        if (tags_ && tag != 0) {
            auto t = cell.extract(kTagLo + 3, kTagLo);
            tagbad = tagbad || (t != bv(0, 4) && t != bv(tag, 4));
        }
    }
    auto v = *val;
    if (8 * n > width) v = v.extract(width - 1, 0);
    if (word) v = *word;
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
        val = val ? z3::concat(byte, *val) : byte;
        uninit = uninit || cell.extract(kCellInit, kCellInit) == bv(0, 1);
    }
    auto v = *val;
    if (8 * n > width) v = v.extract(width - 1, 0);
    auto one = z3::ite(uninit, bv(1, 1), bv(0, 1));
    return Loaded{v, one, bv(0, 1), one};
}

void SymMem::store(const z3::expr& guard, const z3::expr& ptr, const z3::expr& val, unsigned width,
                   const z3::expr& init, unsigned tag) {
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
