// LLVM memory instructions -> PIR memory statements and property checks
// (docs/PIR.md "Memory model"). Law 8: every check below is inserted by
// PRISM; nothing depends on a compiler flag.
//
//   load/store            null, wild pointer, use after free / end of
//                         lifetime, out of bounds, misalignment, write to a
//                         read-only object, uninitialised read, effective
//                         type (--strict-aliasing only)
//   getelementptr         offset overflow, arithmetic on null, beyond one
//                         past the end (inbounds) / out of the object
//   icmp <, <=, ... ptr   pointers into different objects
//   ptr - ptr             pointers into different objects
//   memcpy/memmove/memset range checks, memcpy overlap
//   free/delete/delete[]  invalid free, double free, mismatched allocator
//   alloca (VLA)          size not positive / too large
//   return                pointer to the function's own stack object

#include "translate_mem.hpp"

#include "fp.hpp"
#include "memory.hpp"

#include <algorithm>
#include <cctype>

namespace prism::pir::pirmem {

namespace {

int64_t sext64(uint64_t v, unsigned w) {
    if (w >= 64) return static_cast<int64_t>(v);
    uint64_t sign = uint64_t{1} << (w - 1);
    v &= (uint64_t{1} << w) - 1;
    return static_cast<int64_t>((v ^ sign) - sign);
}

uint64_t align_to(uint64_t v, uint64_t a) { return a <= 1 ? v : (v + a - 1) / a * a; }

uint64_t count_of(const ir::Type& t) {
    // "[N x T]" / "<N x T>"
    std::size_t i = 1;
    while (i < t.text.size() && t.text[i] == ' ') ++i;
    uint64_t n = 0;
    while (i < t.text.size() && std::isdigit(static_cast<unsigned char>(t.text[i])))
        n = n * 10 + static_cast<uint64_t>(t.text[i++] - '0');
    return n;
}

}  // namespace

// ---------------------------------------------------------------------------
// Layout
// ---------------------------------------------------------------------------

Layout::Layout(const ir::Module& m) : m_(m) {
    int_align_ = {{1, 1}, {8, 1}, {16, 2}, {32, 4}, {64, 8}, {128, 16}};
    std::string dl = m.datalayout;
    std::size_t pos = 0;
    while (pos < dl.size()) {
        auto e = dl.find('-', pos);
        if (e == std::string::npos) e = dl.size();
        auto spec = dl.substr(pos, e - pos);
        pos = e + 1;
        auto nums = [&](std::size_t from) {
            std::vector<unsigned> v;
            std::size_t k = from;
            while (k <= spec.size()) {
                auto c = spec.find(':', k);
                if (c == std::string::npos) c = spec.size();
                try {
                    v.push_back(static_cast<unsigned>(std::stoul(spec.substr(k, c - k))));
                } catch (...) {
                    v.push_back(0);
                }
                k = c + 1;
            }
            return v;
        };
        if (spec.size() > 1 && spec[0] == 'i' && std::isdigit(static_cast<unsigned char>(spec[1]))) {
            auto v = nums(1);
            if (v.size() >= 2 && v[0] && v[1]) int_align_[v[0]] = v[1] / 8;
        } else if (spec.size() > 1 && spec[0] == 'p' && (spec[1] == ':' || spec.starts_with("p0:"))) {
            auto v = nums(spec[1] == ':' ? 2 : 3);
            if (v.size() >= 2 && v[0] && v[1]) {
                ptr_bytes_ = v[0] / 8;
                ptr_align_ = v[1] / 8;
            }
        }
    }
}

const ir::Type& Layout::resolve(const ir::Type& t) const {
    if (t.kind == ir::Type::Struct && t.elems.empty() && t.text.starts_with("%")) {
        auto it = m_.types.find(t.text.substr(1));
        if (it == m_.types.end() || it->second.kind != ir::Type::Struct)
            throw Unenc{"UNENCODED: opaque or unknown type " + t.text};
        return it->second;
    }
    return t;
}

unsigned Layout::align(const ir::Type& t0) const {
    const auto& t = resolve(t0);
    switch (t.kind) {
        case ir::Type::Int: {
            auto it = int_align_.lower_bound(t.bits);
            if (it != int_align_.end()) return it->second;
            return int_align_.rbegin()->second;
        }
        case ir::Type::Ptr: return ptr_align_;
        case ir::Type::Float:
            if (t.text == "half" || t.text == "bfloat") return 2;
            if (t.text == "float") return 4;
            if (t.text == "double") return 8;
            return 16;
        case ir::Type::Array: return align(t.elems.at(0));
        case ir::Type::Vector: {
            auto s = store_size(t);
            unsigned a = 1;
            while (a < s && a < 64) a *= 2;
            return a;
        }
        case ir::Type::Struct: {
            if (t.text.starts_with("<{")) return 1;
            unsigned a = 1;
            for (auto& e : t.elems) a = std::max(a, align(e));
            return a;
        }
        default: throw Unenc{"UNENCODED: memory of type " + t.text};
    }
}

uint64_t Layout::store_size(const ir::Type& t0) const {
    const auto& t = resolve(t0);
    switch (t.kind) {
        case ir::Type::Int: return (t.bits + 7) / 8;
        case ir::Type::Ptr: return ptr_bytes_;
        case ir::Type::Float:
            if (t.text == "half" || t.text == "bfloat") return 2;
            if (t.text == "float") return 4;
            if (t.text == "double") return 8;
            if (t.text == "x86_fp80") return 10;
            return 16;
        case ir::Type::Vector: return count_of(t) * store_size(t.elems.at(0));
        default: return alloc_size(t);
    }
}

uint64_t Layout::alloc_size(const ir::Type& t0) const {
    const auto& t = resolve(t0);
    switch (t.kind) {
        case ir::Type::Array: {
            auto n = count_of(t);
            auto e = alloc_size(t.elems.at(0));
            if (e && n > kMaxObjSize / e) throw Unenc{"UNENCODED: object larger than 2^47 bytes (" + t.text + ")"};
            return n * e;
        }
        case ir::Type::Struct: {
            bool packed = t.text.starts_with("<{");
            uint64_t off = 0;
            for (auto& e : t.elems) {
                if (!packed) off = align_to(off, align(e));
                off += alloc_size(e);
            }
            return packed ? off : align_to(off, align(t));
        }
        default: return align_to(store_size(t), align(t));
    }
}

uint64_t Layout::field_offset(const ir::Type& st0, unsigned idx) const {
    const auto& st = resolve(st0);
    bool packed = st.text.starts_with("<{");
    uint64_t off = 0;
    for (unsigned i = 0; i < st.elems.size(); ++i) {
        if (!packed) off = align_to(off, align(st.elems[i]));
        if (i == idx) return off;
        off += alloc_size(st.elems[i]);
    }
    throw Unenc{"UNENCODED: struct field index out of range in " + st.text};
}

const ir::Type& Layout::field_type(const ir::Type& st0, unsigned idx) const {
    const auto& st = resolve(st0);
    if (idx >= st.elems.size()) throw Unenc{"UNENCODED: struct field index out of range in " + st.text};
    return st.elems[idx];
}

// ---------------------------------------------------------------------------
// MemTr
// ---------------------------------------------------------------------------

MemTr::MemTr(TrApi& t, const ir::Module& m) : t_(t), lay_(m) {}

unsigned MemTr::value_width(const ir::Type& t, std::string_view what) const {
    if (t.kind == ir::Type::Ptr) {
        if (t.text != "ptr") throw Unenc{"UNENCODED: " + std::string(what) + " " + t.text};
        return kPtrW;
    }
    if (t.kind == ir::Type::Float) {
        // half / float / double: IEEE bits (docs/PIR.md "Floating point")
        if (auto w = fp::width_of(t)) return *w;
        throw Unenc{"UNENCODED: " + std::string(what) + " " + t.text + " (floating-point format not modelled)"};
    }
    if (t.kind != ir::Type::Int) throw Unenc{"UNENCODED: " + std::string(what) + " " + t.text};
    if (t.bits == 0 || t.bits > 64)
        throw Unenc{"UNENCODED: " + std::string(what) + " " + t.text + " (wider than 64 bits)"};
    return t.bits;
}

unsigned MemTr::tag_of(const ir::Type& ty) const {
    if (!t_.options().strict_aliasing) return 0;
    if (auto w = fp::width_of(ty)) return *w == 16 ? 7u : *w == 32 ? 8u : 9u;  // half / float / double
    return mem::type_tag(ty.kind == ir::Type::Int ? ty.bits : 64, ty.kind == ir::Type::Ptr);
}

Arg MemTr::obj(int b, Arg p) { return t_.assign(b, Op::LShr, 64, {p, c64(kObjShift)}, "obj"); }
Arg MemTr::off(int b, Arg p) { return t_.assign(b, Op::And, 64, {p, c64(kOffMask)}, "off"); }

Arg MemTr::ptr_add(int b, Arg p, Arg delta, int dst) {
    Stmt s;
    s.kind = Stmt::Assign;
    s.op = Op::Add;
    s.dst = dst >= 0 ? dst : t_.newvar("_p" + std::to_string(t_.fn().vars.size()), kPtrW);
    s.args = {p, delta};
    s.ptr_arith = true;
    t_.push(b, s);
    return Arg::v(s.dst, kPtrW);
}

// Past this many non-zero initialiser stores a large (> 4096-byte) global is
// havocked instead (see MemTr::global).
constexpr uint64_t kMaxInitStores = 256;

// Stores emit_init pushes for an initialiser (zero leaves cost none), up to `cap`.
uint64_t init_store_count(const Layout& lay, const ir::Type& ty, const ir::Value& v, uint64_t cap) {
    switch (v.kind) {
        case ir::Value::Int:
        case ir::Value::Fp: return v.bits != 0 ? 1 : 0;
        case ir::Value::Zero:
        case ir::Value::Null: return 0;
        case ir::Value::Str: {
            uint64_t n = 0;
            for (char ch : v.bytes) n += ch != 0;
            return n;
        }
        case ir::Value::Aggregate: {
            const auto& rt = lay.resolve(ty);
            uint64_t n = 0;
            for (std::size_t k = 0; k < v.elems.size() && n < cap; ++k) {
                const ir::Type& et = rt.kind == ir::Type::Array    ? rt.elems.at(0)
                                     : rt.kind == ir::Type::Struct ? lay.field_type(rt, static_cast<unsigned>(k))
                                                                   : ty;
                n += init_store_count(lay, et, v.elems[k].v, cap - n);
            }
            return n;
        }
        default: return 1;  // one store (or an UNENCODED refusal in emit_init)
    }
}

namespace {
bool flat_rec(const Layout& lay, uint64_t off, const ir::Type& ty, const ir::Value& v, std::vector<InitStore>& out) {
    switch (v.kind) {
        case ir::Value::Int:
            if (ty.kind != ir::Type::Int || ty.bits == 0 || ty.bits > 64) return false;
            if (v.bits != 0) out.push_back({off, ty.bits, v.bits});
            return true;
        case ir::Value::Fp: {
            auto w = fp::width_of(ty);
            if (ty.kind != ir::Type::Float || !w) return false;
            if (v.bits != 0) out.push_back({off, *w, v.bits});
            return true;
        }
        case ir::Value::Zero:
        case ir::Value::Null: return true;
        case ir::Value::Str:
            for (std::size_t k = 0; k < v.bytes.size(); ++k)
                if (v.bytes[k] != 0) out.push_back({off + k, 8, static_cast<unsigned char>(v.bytes[k])});
            return true;
        case ir::Value::Aggregate: {
            const auto& rt = lay.resolve(ty);
            if (rt.kind == ir::Type::Array) {
                auto esz = lay.alloc_size(rt.elems.at(0));
                for (std::size_t k = 0; k < v.elems.size(); ++k)
                    if (!flat_rec(lay, off + k * esz, rt.elems[0], v.elems[k].v, out)) return false;
                return true;
            }
            if (rt.kind == ir::Type::Struct) {
                for (std::size_t k = 0; k < v.elems.size(); ++k)
                    if (!flat_rec(lay, off + lay.field_offset(rt, static_cast<unsigned>(k)),
                                  lay.field_type(rt, static_cast<unsigned>(k)), v.elems[k].v, out))
                        return false;
                return true;
            }
            return false;
        }
        default: return false;
    }
}
}  // namespace

std::optional<std::vector<InitStore>> flat_init(const Layout& lay, const ir::Type& ty, const ir::Value& v) {
    std::vector<InitStore> out;
    try {
        if (!flat_rec(lay, 0, ty, v, out)) return std::nullopt;
    } catch (const Unenc&) {
        return std::nullopt;
    } catch (const std::out_of_range&) {
        return std::nullopt;
    }
    return out;
}

std::optional<EntryGlobal> entry_global(const ir::Module& m, const Layout& lay, const std::string& name,
                                        bool initial) {
    // the choices of MemTr::global, for the cases without special objects
    const auto* g = m.find_global(name);
    if (!g || g->thread_local_) return std::nullopt;
    if (name == "stdin" || name == "stdout" || name == "stderr" || name.starts_with("_ZT")) return std::nullopt;
    EntryGlobal e;
    e.g = g;
    try {
        e.size = lay.alloc_size(g->ty);
        e.align = std::max(g->align, lay.align(g->ty));
    } catch (const Unenc&) {
        return std::nullopt;
    }
    if (g->external && e.size == 0) return std::nullopt;
    e.kind = g->is_const ? MemKind::Const : MemKind::Static;
    e.init = !g->external && (g->is_const || initial) ? 1 : 2;
    if (e.init == 1 && !g->init.empty()) {
        auto flat = flat_init(lay, g->ty, g->init[0].v);
        if (e.size > 4096 && init_store_count(lay, g->ty, g->init[0].v, kMaxInitStores + 1) > kMaxInitStores) {
            e.init = 2;  // a large table: arbitrary bytes (MemTr::global)
        } else {
            if (!flat) return std::nullopt;
            e.stores = std::move(*flat);
        }
    }
    return e;
}

std::vector<std::string> entry_globals(const ir::Module& m, const Layout& lay, const ir::Function& f, bool initial) {
    std::vector<std::string> out;
    auto see = [&](const ir::Operand& o) {
        if (o.v.kind == ir::Value::Global && o.ty.kind == ir::Type::Ptr &&
            std::find(out.begin(), out.end(), o.v.name) == out.end() && entry_global(m, lay, o.v.name, initial))
            out.push_back(o.v.name);
    };
    for (auto& bl : f.blocks)
        for (auto& in : bl.insts) {
            for (auto& o : in.ops) see(o);
            for (auto& [o, _] : in.incoming) see(o);
        }
    return out;
}

void MemTr::preassign_globals(const ir::Function& f) {
    for (auto& name : entry_globals(t_.module(), lay_, f, t_.options().globals_initial)) {
        if (globals_.count(name)) continue;
        globals_[name] = Arg::v(t_.newvar("@" + name, kPtrW), kPtrW);
        pending_.push_back(name);
    }
}

void MemTr::emit_entry_globals() {
    for (auto& name : pending_) {
        auto e = *entry_global(t_.module(), lay_, name, t_.options().globals_initial);
        mark_memory();
        if (!e.g->is_const) t_.fn().mutable_globals = true;
        Stmt s;
        s.kind = Stmt::Alloc;
        s.dst = globals_[name].var;
        s.args = {c64(e.size)};
        s.mkind = e.kind;
        s.init = e.init;
        s.align = e.align;
        s.msg = "@" + name;
        t_.push(-1, s);
        Arg p = globals_[name];
        for (auto& st : e.stores) {  // as emit_init
            Stmt w;
            w.kind = Stmt::Store;
            w.args = {st.off ? ptr_add(-1, p, c64(st.off)) : p, Arg::c(st.w, st.bits), Arg::c(1, 1)};
            t_.push(-1, w);
        }
    }
    pending_.clear();
}

Arg MemTr::global(const std::string& name) {
    if (auto it = globals_.find(name); it != globals_.end()) return it->second;
    const auto& m = t_.module();
    const auto* g = m.find_global(name);
    if (!g) {
        if (m.find(name) || std::find(m.declarations.begin(), m.declarations.end(), name) != m.declarations.end()) {
            mark_memory();
            return t_.fn_addr(name);  // function address: calls through it dispatch (translate_ctl.inc)
        }
        throw Unenc{"UNENCODED: global @" + name + " (not parsed)"};
    }
    if (g->thread_local_) throw Unenc{"UNENCODED: thread_local global @" + name};
    auto size = lay_.alloc_size(g->ty);
    if (g->external && name.starts_with("_ZTI") && name.size() > 4) {
        // A typeinfo object of the C++ runtime (fundamental and pointer types,
        // library classes), declared `external constant ptr`: the Itanium ABI
        // std::type_info layout { vptr, const char* __name } with __name the
        // mangled type ("i" for @_ZTIi, as the runtime's _ZTS strings), so
        // typeid(int).name(), == and before() read defined bytes. The vptr is
        // arbitrary (PRISM does not model the runtime's type_info vtables).
        mark_memory();
        Stmt s;
        s.kind = Stmt::Alloc;
        s.dst = t_.newvar("@" + name, kPtrW);
        s.args = {c64(16)};
        s.mkind = MemKind::Const;
        s.init = 2;
        s.align = 8;
        s.msg = "@" + name + " (C++ runtime type_info)";
        t_.push(-1, s);
        Arg p = Arg::v(s.dst, kPtrW);
        globals_[name] = p;
        const std::string nm = name.substr(4);
        Stmt n;
        n.kind = Stmt::Alloc;
        n.dst = t_.newvar("@_ZTS" + nm, kPtrW);
        n.args = {c64(nm.size() + 1)};
        n.mkind = MemKind::Const;
        n.init = 1;
        n.align = 1;
        n.msg = "@_ZTS" + nm + " (C++ runtime type name)";
        t_.push(-1, n);
        Arg np = Arg::v(n.dst, kPtrW);
        ir::Value str;
        str.kind = ir::Value::Str;
        str.bytes = nm;
        str.bytes.push_back('\0');
        emit_init(np, 0, ir::Type{ir::Type::Array, 0, "", {ir::Type{ir::Type::Int, 8, "i8", {}}}}, str, "_ZTS" + nm);
        Stmt st;
        st.kind = Stmt::Store;
        st.args = {ptr_add(-1, p, c64(8)), np, Arg::c(1, 1)};
        t_.push(-1, st);
        return p;
    }
    if (g->external && size == 0) {
        // The C++ runtime's typeinfo vtables (referenced from every class's
        // typeinfo, which vtables point to): an object PRISM knows nothing
        // about. Any access through it is out of bounds (reported), never
        // assumed to read anything particular.
        if (!name.starts_with("_ZTVN10__cxxabiv1")) throw Unenc{"UNENCODED: extern object of unknown size @" + name};
        mark_memory();
        Stmt s;
        s.kind = Stmt::Alloc;
        s.dst = t_.newvar("@" + name, kPtrW);
        s.args = {c64(0)};
        s.mkind = MemKind::Const;
        s.init = 1;
        s.align = 16;
        s.msg = "@" + name + " (C++ runtime, contents not modelled)";
        t_.push(-1, s);
        return globals_[name] = Arg::v(s.dst, kPtrW);
    }
    mark_memory();
    const bool stream = g->external && (name == "stdin" || name == "stdout" || name == "stderr");
    int init = 2;
    if (stream) init = 1;
    else if (g->external) init = 2;
    else if (g->is_const || t_.options().globals_initial) init = 1;
    if (!g->is_const) t_.fn().mutable_globals = true;
    // Large tables with many non-zero entries: arbitrary initialised bytes (an
    // over-approximation, never a wrong proof). The zero fill itself is one
    // memory entry whatever the size, so a large all-zero (or mostly zero)
    // global keeps its exact contents (F8: `struct S a[1024];` reads 0).
    bool big = init == 1 && size > 4096 && !g->init.empty() &&
               init_stores(g->ty, g->init[0].v, kMaxInitStores + 1) > kMaxInitStores;
    if (big) init = 2;
    Stmt s;
    s.kind = Stmt::Alloc;
    s.dst = t_.newvar("@" + name, kPtrW);
    s.args = {c64(size)};
    s.mkind = g->is_const ? MemKind::Const : MemKind::Static;
    s.init = init;
    s.align = std::max(g->align, lay_.align(g->ty));
    s.msg = "@" + name;
    t_.push(-1, s);
    Arg p = Arg::v(s.dst, kPtrW);
    globals_[name] = p;
    if (stream) {
        // stdin/stdout/stderr point to valid FILE objects
        auto f = alloc(-1, c64(216), MemKind::File, 2, name, 0);
        Stmt st;
        st.kind = Stmt::Store;
        st.args = {p, f, Arg::c(1, 1)};
        t_.push(-1, st);
    } else if (init == 1 && !g->init.empty()) {
        emit_init(p, 0, g->ty, g->init[0].v, name);
    }
    return p;
}

uint64_t MemTr::init_stores(const ir::Type& ty, const ir::Value& v, uint64_t cap) const {
    return init_store_count(lay_, ty, v, cap);
}

void MemTr::emit_init(Arg base, uint64_t offs, const ir::Type& ty, const ir::Value& v, const std::string& gname) {
    auto at = [&](uint64_t o) { return o ? ptr_add(-1, base, c64(o)) : base; };
    auto put = [&](uint64_t o, Arg val) {
        Stmt st;
        st.kind = Stmt::Store;
        st.args = {at(o), val, Arg::c(1, 1)};
        t_.push(-1, st);
    };
    switch (v.kind) {
        case ir::Value::Int: {
            auto w = value_width(ty, "initializer of");
            if (v.bits != 0) put(offs, Arg::c(w, v.bits));
            return;
        }
        case ir::Value::Fp: {
            auto w = value_width(ty, "initializer of");
            if (v.bits != 0) put(offs, Arg::c(w, v.bits));
            return;
        }
        case ir::Value::Zero:
        case ir::Value::Null: return;
        case ir::Value::Undef:
        case ir::Value::Poison:
            if (ty.kind == ir::Type::Int || ty.kind == ir::Type::Ptr) {
                // undef bytes: every load may read another value, so they are
                // stored as uninitialised (refinement finding 4), like `store undef`
                Stmt st;
                st.kind = Stmt::Store;
                st.args = {at(offs), t_.havoc(-1, value_width(ty, "initializer of"), false, false), Arg::c(1, 0)};
                t_.push(-1, st);
            }
            return;  // aggregate undef: padding
        case ir::Value::Global: put(offs, global(v.name)); return;
        case ir::Value::ConstExpr:
            if (v.ce_op == "getelementptr") {
                put(offs, const_gep(-1, v, 0));
                return;
            }
            if ((v.ce_op == "inttoptr" || v.ce_op == "bitcast") && !v.elems.empty() &&
                v.elems[0].v.kind == ir::Value::Int && v.elems[0].v.bits == 0)
                return;
            throw Unenc{"UNENCODED: initializer of @" + gname + " (" + v.ce_op + " constant expression)"};
        case ir::Value::Str:
            for (std::size_t k = 0; k < v.bytes.size(); ++k)
                if (v.bytes[k] != 0) put(offs + k, Arg::c(8, static_cast<unsigned char>(v.bytes[k])));
            return;
        case ir::Value::Aggregate: {
            const auto& rt = lay_.resolve(ty);
            if (rt.kind == ir::Type::Array) {
                auto esz = lay_.alloc_size(rt.elems.at(0));
                for (std::size_t k = 0; k < v.elems.size(); ++k)
                    emit_init(base, offs + k * esz, rt.elems[0], v.elems[k].v, gname);
                return;
            }
            if (rt.kind == ir::Type::Struct) {
                for (std::size_t k = 0; k < v.elems.size(); ++k)
                    emit_init(base, offs + lay_.field_offset(rt, static_cast<unsigned>(k)),
                              lay_.field_type(rt, static_cast<unsigned>(k)), v.elems[k].v, gname);
                return;
            }
            throw Unenc{"UNENCODED: initializer of @" + gname + " (" + ty.text + ")"};
        }
        default: throw Unenc{"UNENCODED: initializer of @" + gname + " (" + v.text + ")"};
    }
}

Arg MemTr::const_gep(int b, const ir::Value& ce, int line) {
    (void)line;
    if (ce.elems.empty()) throw Unenc{"UNENCODED: constant getelementptr"};
    const auto& bo = ce.elems[0];
    Arg base;
    if (bo.v.kind == ir::Value::Global) base = global(bo.v.name);
    else if (bo.v.kind == ir::Value::Null) base = c64(0);
    else if (bo.v.kind == ir::Value::ConstExpr && bo.v.ce_op == "getelementptr") base = const_gep(b, bo.v, line);
    else throw Unenc{"UNENCODED: constant getelementptr on " + bo.v.text};
    __int128 total = 0;
    const ir::Type* t = &ce.ce_ty;
    for (std::size_t k = 1; k < ce.elems.size(); ++k) {
        const auto& ix = ce.elems[k];
        if (ix.v.kind != ir::Value::Int || ix.ty.kind != ir::Type::Int)
            throw Unenc{"UNENCODED: constant getelementptr with a non-constant index"};
        int64_t i = sext64(ix.v.bits, ix.ty.bits);
        if (k == 1) {
            total += static_cast<__int128>(i) * static_cast<__int128>(lay_.alloc_size(*t));
            continue;
        }
        const auto& rt = lay_.resolve(*t);
        if (rt.kind == ir::Type::Struct) {
            total += static_cast<__int128>(lay_.field_offset(rt, static_cast<unsigned>(i)));
            t = &lay_.field_type(rt, static_cast<unsigned>(i));
        } else if (rt.kind == ir::Type::Array) {
            t = &rt.elems.at(0);
            total += static_cast<__int128>(i) * static_cast<__int128>(lay_.alloc_size(*t));
        } else {
            throw Unenc{"UNENCODED: constant getelementptr into " + rt.text};
        }
    }
    if (total == 0) return base;
    if (total > static_cast<__int128>(kMaxObjSize) || total < -static_cast<__int128>(kMaxObjSize))
        throw Unenc{"UNENCODED: constant pointer offset out of range"};
    mark_memory();
    return ptr_add(b, base, c64(static_cast<uint64_t>(static_cast<int64_t>(total))));
}

std::optional<std::string> MemTr::const_string(const ir::Value& v) const {
    const ir::Value* g = &v;
    uint64_t skip = 0;
    if (v.kind == ir::Value::ConstExpr && v.ce_op == "getelementptr" && !v.elems.empty()) {
        g = &v.elems[0].v;
        for (std::size_t k = 1; k < v.elems.size(); ++k) {
            if (v.elems[k].v.kind != ir::Value::Int) return std::nullopt;
            skip += v.elems[k].v.bits;  // i8 arrays: index = byte offset
        }
    }
    if (g->kind != ir::Value::Global) return std::nullopt;
    const auto* gl = t_.module().find_global(g->name);
    if (!gl || !gl->is_const || gl->init.empty() || gl->init[0].v.kind != ir::Value::Str) return std::nullopt;
    const auto& bytes = gl->init[0].v.bytes;
    if (skip > bytes.size()) return std::nullopt;
    auto s = bytes.substr(static_cast<std::size_t>(skip));
    auto nul = s.find('\0');
    if (nul == std::string::npos) return std::nullopt;
    return s.substr(0, nul);
}

void MemTr::alloca_(int b, int dst, const ir::Type& ety, unsigned al, std::optional<Arg> count,
                    std::optional<Arg> count_src, const std::string& name, int line) {
    mark_memory();
    auto esz = lay_.alloc_size(ety);
    Stmt s;
    s.kind = Stmt::Alloc;
    s.dst = dst;
    s.mkind = MemKind::Stack;
    s.init = 0;
    s.align = al ? al : lay_.align(ety);
    s.msg = name;
    if (!count || count->is_const) {
        uint64_t n = count ? count->bits : 1;
        if (esz && n > kMaxObjSize / esz) throw Unenc{"UNENCODED: stack object larger than 2^47 bytes"};
        s.args = {c64(esz * n)};
    } else {
        Arg cnt = count->width < 64 ? t_.assign(b, Op::ZExt, 64, {*count}, "vla") : *count;
        if (count_src)
            t_.check(b, p2(b, Op::Sle, *count_src, Arg::c(count_src->width, 0)), "vla", "MEM-VLA-SIZE",
                     "variable-length array size is not positive (C17 6.7.6.2p5)", line);
        else
            t_.check(b, p2(b, Op::Eq, cnt, c64(0)), "vla", "MEM-VLA-SIZE", "variable-length array of size 0", line);
        Arg bytes = cnt;
        if (esz != 1) {
            t_.check(b, p2(b, Op::UMulOvf, cnt, c64(esz)), "vla", "MEM-VLA-SIZE",
                     "variable-length array size in bytes overflows", line);
            bytes = t_.assign(b, Op::Mul, 64, {cnt, c64(esz)}, "vla");
        }
        t_.check(b, p2(b, Op::Uge, bytes, c64(kMaxObjSize)), "vla", "MEM-VLA-SIZE",
                 "variable-length array larger than 2^47 bytes", line);
        s.args = {bytes};
    }
    t_.push(b, s);
}

void MemTr::access_checks(int b, Arg p, Arg n, bool write, unsigned al, int line, std::optional<Arg> guard) {
    // the checks read the object table (a model's __prism_read_range may be
    // the only memory statement of a function, e.g. putc(c, NULL))
    mark_memory();
    auto G = [&](Arg c) { return guard ? band(b, *guard, c) : c; };
    Arg o = obj(b, p);
    Arg f = off(b, p);
    Arg kind = t_.assign(b, Op::ObjKind, 8, {p}, "kind");
    Arg live = t_.assign(b, Op::ObjLive, 1, {p}, "live");
    Arg size = t_.assign(b, Op::ObjSize, 64, {p}, "size");
    Arg k0 = Arg::c(8, 0);
    const char* rw = write ? "write" : "read";
    t_.check(b, G(p2(b, Op::Eq, o, c64(0))), "null", "PTR-NULL-DEREF",
             std::string(rw) + " through a null pointer", line);
    t_.check(b, G(band(b, p2(b, Op::Ne, o, c64(0)), p2(b, Op::Eq, kind, k0))), "wild", "PTR-INVALID-DEREF",
             std::string(rw) + " through a pointer that does not point to any object", line);
    Arg known = p2(b, Op::Ne, kind, k0);
    t_.check(b, G(band(b, known, p2(b, Op::Eq, live, Arg::c(1, 0)))), "uaf", "MEM-UAF",
             std::string(rw) + " of an object after free or after the end of its lifetime", line);
    Arg end = t_.assign(b, Op::Add, 64, {f, n}, "end");
    Arg oob = bor(b, p2(b, Op::Ugt, end, size), p2(b, Op::UAddOvf, f, n));
    t_.check(b, G(band(b, live, oob)), write ? "oob-write" : "oob-read", write ? "MEM-OOB-WRITE" : "MEM-OOB-READ",
             std::string(rw) + " out of the bounds of its object", line);
    if (al > 1) {
        Arg low = t_.assign(b, Op::And, 64, {f, c64(al - 1)}, "al");
        Arg oal = t_.assign(b, Op::ObjAlign, 64, {p}, "oal");
        Arg mis = bor(b, p2(b, Op::Ne, low, c64(0)), p2(b, Op::Ult, oal, c64(al)));
        t_.check(b, G(band(b, live, mis)), "align", "MEM-MISALIGNED",
                 std::string(rw) + " at an address that is not a multiple of " + std::to_string(al), line);
    }
    if (write)
        t_.check(b, G(band(b, live, p2(b, Op::Eq, kind, Arg::c(8, static_cast<uint64_t>(MemKind::Const))))),
                 "write-const", "MEM-WRITE-CONST", "write to a read-only object (string literal or const object)",
                 line);
}

int MemTr::load(int b, int dst, const ir::Type& ty, Arg ptr, unsigned al, int line, bool check_uninit) {
    mark_memory();
    auto w = value_width(ty, "load of");
    auto n = lay_.store_size(ty);
    access_checks(b, ptr, c64(n), false, al, line);
    Stmt s;
    s.kind = Stmt::Load;
    s.dst = dst;
    s.args = {ptr};
    s.dst2 = t_.newvar("_ldu" + std::to_string(t_.fn().vars.size()), check_uninit ? 1u : static_cast<unsigned>(n));
    s.tag = tag_of(ty);
    if (s.tag) s.dst3 = t_.newvar("_ldt" + std::to_string(t_.fn().vars.size()), 1);
    (void)w;
    t_.push(b, s);
    if (check_uninit) t_.check(b, Arg::v(s.dst2, 1), "uninit", "UNINIT-READ", "read of uninitialised memory", line);
    if (s.dst3 >= 0)
        t_.check(b, Arg::v(s.dst3, 1), "strict-alias", "MEM-STRICT-ALIAS",
                 "object accessed through an lvalue of an incompatible type (effective type, C11 6.5p7)", line);
    return s.dst2;
}

void MemTr::store(int b, Arg ptr, Arg val, Arg init, const ir::Type& ty, unsigned al, int line) {
    mark_memory();
    auto w = value_width(ty, "store of");
    auto n = lay_.store_size(ty);
    access_checks(b, ptr, c64(n), true, al, line);
    val.width = w;
    Stmt s;
    s.kind = Stmt::Store;
    s.args = {ptr, val, init};
    s.tag = tag_of(ty);
    t_.push(b, s);
}

void MemTr::gep(int b, int dst, Arg base, const ir::Type& ety, const std::vector<Arg>& idx, bool inbounds,
                int line, int use, bool base_last_field) {
    mark_memory();
    __int128 cst = 0;
    std::optional<Arg> var;
    const ir::Type* t = &ety;
    bool last_field = base_last_field;  // the current type is the last field of a struct (flexible array)
    auto add_var = [&](Arg x) {
        if (!var) {
            var = x;
            return;
        }
        t_.check(b, p2(b, Op::SAddOvf, *var, x), "ptr-arith", "MEM-PTR-ARITH", "pointer offset computation overflows",
                 line);
        var = t_.assign(b, Op::Add, 64, {*var, x}, "delta");
    };
    for (std::size_t k = 0; k < idx.size(); ++k) {
        uint64_t scale = 0;
        if (k == 0) {
            scale = lay_.alloc_size(ety);
        } else {
            const auto& rt = lay_.resolve(*t);
            if (rt.kind == ir::Type::Struct) {
                if (!idx[k].is_const) throw Unenc{"UNENCODED: getelementptr with a variable struct index"};
                auto fi = static_cast<unsigned>(idx[k].bits);
                cst += static_cast<__int128>(lay_.field_offset(rt, fi));
                t = &lay_.field_type(rt, fi);
                last_field = fi + 1 == rt.elems.size();
                continue;
            }
            if (rt.kind != ir::Type::Array) throw Unenc{"UNENCODED: getelementptr into " + rt.text};
            // Array bounds (C17 6.5.6p8, J.2 "a[1][7] in int a[4][5]"): an
            // index into a declared array stays in that array, also when the
            // array is nested in a larger object. Exempt: [0 x T] and the last
            // field of a struct (flexible array / struct hack).
            uint64_t n = count_of(rt);
            if (!last_field && n > 0) {
                const Arg& ix = idx[k];
                Arg s64 = ix.width < 64 ? (ix.is_const ? c64(static_cast<uint64_t>(sext64(ix.bits, ix.width)))
                                                       : t_.assign(b, Op::SExt, 64, {ix}, "idx"))
                                        : ix;
                Arg bad = p2(b, use ? Op::Uge : Op::Ugt, s64, c64(n));
                const char* cls = use == 1 ? "MEM-OOB-READ" : use == 2 ? "MEM-OOB-WRITE" : "MEM-PTR-ARITH";
                t_.check(b, bad, use == 1 ? "oob-read" : use == 2 ? "oob-write" : "ptr-arith", cls,
                         "array index out of the bounds of its array (" + rt.text + ")", line);
            }
            last_field = false;
            t = &rt.elems.at(0);
            scale = lay_.alloc_size(*t);
        }
        const Arg& ix = idx[k];
        if (ix.is_const) {
            cst += static_cast<__int128>(sext64(ix.bits, ix.width)) * static_cast<__int128>(scale);
            if (cst > static_cast<__int128>(INT64_MAX) || cst < static_cast<__int128>(INT64_MIN))
                throw Unenc{"UNENCODED: constant pointer offset overflows"};
            continue;
        }
        Arg s = ix.width < 64 ? t_.assign(b, Op::SExt, 64, {ix}, "idx") : ix;
        if (scale == 0) continue;
        if (scale != 1) {
            t_.check(b, p2(b, Op::SMulOvf, s, c64(scale)), "ptr-arith", "MEM-PTR-ARITH",
                     "pointer offset computation overflows", line);
            s = t_.assign(b, Op::Mul, 64, {s, c64(scale)}, "idx");
        }
        add_var(s);
    }
    Arg delta = c64(static_cast<uint64_t>(static_cast<int64_t>(cst)));
    if (var) {
        if (cst != 0) add_var(delta);
        delta = *var;
    }
    if (!var && cst == 0) {
        Stmt s;
        s.kind = Stmt::Assign;
        s.op = Op::Copy;
        s.dst = dst;
        s.args = {base};
        s.ptr_arith = true;
        t_.push(b, s);
        return;
    }
    ptr_add(b, base, delta, dst);
    Arg res = Arg::v(dst, kPtrW);
    Arg o0 = obj(b, base);
    Arg o1 = obj(b, res);
    if (inbounds) {
        t_.check(b, band(b, p2(b, Op::Eq, o0, c64(0)), p2(b, Op::Ne, delta, c64(0))), "ptr-arith", "MEM-PTR-ARITH",
                 "pointer arithmetic on a null pointer", line);
        Arg kind = t_.assign(b, Op::ObjKind, 8, {base}, "kind");
        Arg size = t_.assign(b, Op::ObjSize, 64, {base}, "size");
        Arg bad = bor(b, p2(b, Op::Ne, o1, o0), p2(b, Op::Ugt, off(b, res), size));
        t_.check(b, band(b, p2(b, Op::Ne, kind, Arg::c(8, 0)), bad), "ptr-arith", "MEM-PTR-ARITH",
                 "pointer arithmetic beyond one past the end of the object (or before its start)", line);
    } else {
        t_.check(b, band(b, p2(b, Op::Ne, o0, c64(0)), p2(b, Op::Ne, o1, o0)), "ptr-arith", "MEM-PTR-ARITH",
                 "pointer arithmetic leaves the object's address range", line);
    }
}

void MemTr::icmp(int b, int dst, const std::string& pred, Arg x, Arg y, int line) {
    static const std::map<std::string, Op> preds{
        {"eq", Op::Eq},   {"ne", Op::Ne},   {"ugt", Op::Ugt}, {"uge", Op::Uge}, {"ult", Op::Ult},
        {"ule", Op::Ule}, {"sgt", Op::Ugt}, {"sge", Op::Uge}, {"slt", Op::Ult}, {"sle", Op::Ule}};
    auto it = preds.find(pred);
    if (it == preds.end()) throw Unenc{"UNENCODED: icmp " + pred + " ptr"};
    x.width = y.width = kPtrW;
    if (pred != "eq" && pred != "ne")
        t_.check(b, p2(b, Op::Ne, obj(b, x), obj(b, y)), "ptr-cmp", "PTR-COMPARE",
                 "relational comparison of pointers into different objects (C17 6.5.8p5)", line);
    Stmt s;
    s.kind = Stmt::Assign;
    s.op = it->second;
    s.dst = dst;
    s.args = {x, y};
    t_.push(b, s);
}

void MemTr::ptr_sub_check(int b, Arg x, Arg y, int line) {
    t_.check(b, p2(b, Op::Ne, obj(b, x), obj(b, y)), "ptr-sub", "PTR-COMPARE",
             "subtraction of pointers into different objects (C17 6.5.6p9)", line);
}

void MemTr::memcpy_(int b, Arg dst, Arg src, Arg len, bool move, int line) {
    mark_memory();
    Arg n = len.width < 64 ? t_.assign(b, Op::ZExt, 64, {len}, "len") : len;
    Arg nz = p2(b, Op::Ne, n, c64(0));
    access_checks(b, dst, n, true, 1, line, nz);
    access_checks(b, src, n, false, 1, line, nz);
    if (!move) {
        Arg fd = off(b, dst), fs = off(b, src);
        Arg a = p2(b, Op::Ult, fd, t_.assign(b, Op::Add, 64, {fs, n}, "e"));
        Arg c = p2(b, Op::Ult, fs, t_.assign(b, Op::Add, 64, {fd, n}, "e"));
        Arg same = p2(b, Op::Eq, obj(b, dst), obj(b, src));
        t_.check(b, band(b, nz, band(b, same, band(b, a, c))), "overlap", "MEM-OVERLAP",
                 "memcpy with overlapping source and destination (use memmove)", line);
    }
    Stmt s;
    s.kind = Stmt::MemCpy;
    s.args = {dst, src, n};
    t_.push(b, s);
}

void MemTr::memset_(int b, Arg dst, Arg byte, Arg len, int line) {
    mark_memory();
    Arg n = len.width < 64 ? t_.assign(b, Op::ZExt, 64, {len}, "len") : len;
    Arg nz = p2(b, Op::Ne, n, c64(0));
    access_checks(b, dst, n, true, 1, line, nz);
    if (byte.width != 8) byte = byte.width > 8 ? t_.assign(b, Op::Trunc, 8, {byte}, "byte")
                                                : t_.assign(b, Op::ZExt, 8, {byte}, "byte");
    Stmt s;
    s.kind = Stmt::MemSet;
    s.args = {dst, byte, n};
    t_.push(b, s);
}

Arg MemTr::alloc(int b, Arg size, MemKind k, int init, const std::string& what, int line, int dst) {
    (void)line;
    mark_memory();
    Arg n = size.width < 64 ? t_.assign(b, Op::ZExt, 64, {size}, "sz") : size;
    if (!n.is_const) t_.assume(b, p2(b, Op::Ult, n, c64(kMaxObjSize)));
    else if (n.bits >= kMaxObjSize) throw Unenc{"UNENCODED: allocation larger than 2^47 bytes"};
    Stmt s;
    s.kind = Stmt::Alloc;
    s.dst = dst >= 0 ? dst : t_.newvar("_obj" + std::to_string(t_.fn().vars.size()), kPtrW);
    s.args = {n};
    s.mkind = k;
    s.init = init;
    s.align = 16;
    s.msg = what;
    t_.push(b, s);
    return Arg::v(s.dst, kPtrW);
}

void MemTr::dealloc(int b, Arg ptr, MemKind expected, const std::string& what, int line, bool kill) {
    mark_memory();
    Arg o = obj(b, ptr);
    Arg nonnull = p2(b, Op::Ne, o, c64(0));
    Arg kind = t_.assign(b, Op::ObjKind, 8, {ptr}, "kind");
    Arg live = t_.assign(b, Op::ObjLive, 1, {ptr}, "live");
    auto K = [&](MemKind m) { return Arg::c(8, static_cast<uint64_t>(m)); };
    t_.check(b, band(b, nonnull, p2(b, Op::Eq, kind, K(MemKind::None))), "free-invalid", "MEM-INVALID-FREE",
             what + " of a pointer that does not point to an allocated object", line);
    Arg known = band(b, nonnull, p2(b, Op::Ne, kind, K(MemKind::None)));
    t_.check(b, band(b, known, p2(b, Op::Ne, off(b, ptr), c64(0))), "free-invalid", "MEM-INVALID-FREE",
             what + " of a pointer that is not the start of its object", line);
    Arg heapish = bor(b, p2(b, Op::Eq, kind, K(MemKind::Heap)),
                      bor(b, p2(b, Op::Eq, kind, K(MemKind::New)), p2(b, Op::Eq, kind, K(MemKind::NewArr))));
    Arg other_kind = band(b, known, p2(b, Op::Ne, kind, K(expected)));
    if (expected == MemKind::File) {
        t_.check(b, other_kind, "free-invalid", "MEM-INVALID-FREE", what + " of a pointer that is not an open FILE",
                 line);
    } else {
        t_.check(b, band(b, other_kind, heapish), "free-mismatch", "MEM-MISMATCHED-FREE",
                 what + " of memory from a different allocator (malloc/free, new/delete, new[]/delete[])", line);
        t_.check(b, band(b, other_kind, p2(b, Op::Eq, heapish, Arg::c(1, 0))), "free-invalid", "MEM-INVALID-FREE",
                 what + " of memory that was not dynamically allocated (stack, static, literal or parameter object)",
                 line);
    }
    t_.check(b, band(b, known, band(b, p2(b, Op::Eq, kind, K(expected)), p2(b, Op::Eq, live, Arg::c(1, 0)))),
             "double-free", "MEM-DOUBLE-FREE", what + " of memory that was already released", line);
    if (kill) end_lifetime(b, ptr);
}

void MemTr::end_lifetime(int b, Arg ptr) {
    Stmt s;
    s.kind = Stmt::Free;
    s.args = {ptr};
    t_.push(b, s);
}

void MemTr::stack_escape_check(int b, Arg ret, const std::vector<Arg>& own, int line) {
    if (own.empty()) return;
    Arg ro = obj(b, ret);
    std::optional<Arg> hit;
    for (auto& a : own) {
        Arg e = p2(b, Op::Eq, ro, obj(b, a));
        hit = hit ? bor(b, *hit, e) : e;
    }
    t_.check(b, band(b, *hit, p2(b, Op::Ne, ro, c64(0))), "stack-escape", "MEM-STACK-ESCAPE",
             "returns a pointer to its own stack object (dangling after the return)", line);
}

Arg MemTr::obj_size_remaining(int b, Arg ptr) {
    mark_memory();
    Arg size = t_.assign(b, Op::ObjSize, 64, {ptr}, "size");
    Arg f = off(b, ptr);
    Arg rem = t_.assign(b, Op::Sub, 64, {size, f}, "rem");
    return t_.assign(b, Op::Select, 64, {p2(b, Op::Ugt, f, size), c64(0), rem}, "rem");
}

void MemTr::read_range(int b, Arg ptr, Arg len, int line) {
    Arg n = len.width < 64 ? t_.assign(b, Op::ZExt, 64, {len}, "len") : len;
    access_checks(b, ptr, n, false, 1, line, p2(b, Op::Ne, n, c64(0)));
}

void MemTr::write_havoc(int b, Arg ptr, Arg len, int line) {
    Arg n = len.width < 64 ? t_.assign(b, Op::ZExt, 64, {len}, "len") : len;
    access_checks(b, ptr, n, true, 1, line, p2(b, Op::Ne, n, c64(0)));
    Arg tmp = alloc(b, n, MemKind::Extern, 2, "input bytes", line);
    Stmt s;
    s.kind = Stmt::MemCpy;
    s.args = {ptr, tmp, n};
    t_.push(b, s);
    end_lifetime(b, tmp);
}

Arg MemTr::fresh_cstr(int b, MemKind k, int line) {
    Arg n = t_.havoc(b, 64, false, true);
    t_.fn().nondet = true;
    t_.assume(b, p2(b, Op::Ult, n, c64(4096)));
    Arg p = alloc(b, t_.assign(b, Op::Add, 64, {n, c64(1)}, "len"), k, 2, "environment string", line);
    Stmt st;
    st.kind = Stmt::Store;
    st.args = {ptr_add(b, p, n), Arg::c(8, 0), Arg::c(1, 1)};
    t_.push(b, st);
    return p;
}

Arg MemTr::stack_save(int b) {
    mark_memory();
    Stmt s;
    s.kind = Stmt::StackSave;
    s.dst = t_.newvar("_ss" + std::to_string(t_.fn().vars.size()), 64);
    t_.push(b, s);
    return Arg::v(s.dst, 64);
}

void MemTr::stack_restore(int b, Arg token) {
    Stmt s;
    s.kind = Stmt::StackRestore;
    s.args = {token};
    t_.push(b, s);
}

}  // namespace prism::pir::pirmem
