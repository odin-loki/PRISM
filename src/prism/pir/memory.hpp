#pragma once

// PIR memory model internals (docs/PIR.md "Memory model"; Lean:
// proofs/semantics/PrismSem/Memory.lean, MemEncode.lean).
//
// A pointer is one 64-bit value: object id in bits 63..48 (0 = the null
// object, never allocated), byte offset in bits 47..0. Every byte of memory
// is a *cell*: bits 7..0 value, bit 8 "initialised", bits 12..9 the
// effective-type tag (only with --strict-aliasing). Objects carry size,
// liveness, allocation kind and base alignment.
//
// Symbolic memory (SymMem) is encoded in the topological order of the
// unrolled program with every update guarded by the reach condition of the
// block instance that performs it: on any path, the guards of block
// instances off the path are false, so one global state is exact (no merge
// at join points is needed). Object ids are constants: each Alloc instance
// of the unrolled DAG runs at most once per path and gets the next id.
//
//   MemEncoding::Array  bytes live in one SMT array (address -> cell);
//                       ranged writes (memcpy/memset/havoc) are lambdas.
//   MemEncoding::Bv     no arrays: a load is an ite chain over the guarded
//                       writes before it (read-over-write, the form of
//                       PrismSem/MemEncode.lean); arbitrary initial bytes are
//                       fresh variables with pairwise Ackermann constraints.

#include "prism/pir.hpp"

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <tuple>
#include <unordered_map>
#include <vector>

#ifdef PRISM_HAS_Z3
#  include <z3++.h>
#endif

namespace prism::pir::mem {

inline uint64_t make_ptr(uint64_t obj, uint64_t off) { return (obj << kObjShift) | (off & kOffMask); }
inline uint64_t obj_of(uint64_t p) { return p >> kObjShift; }
inline uint64_t off_of(uint64_t p) { return p & kOffMask; }

inline constexpr unsigned kCellInit = 8;   // bit index of "initialised"
inline constexpr unsigned kTagLo = 9;      // effective-type tag bits 12..9
inline unsigned cell_width(bool tags) { return tags ? 13u : 9u; }

// Effective-type tag of an access of `width` bits (0 = character / untyped).
unsigned type_tag(unsigned width, bool is_ptr);

#ifdef PRISM_HAS_Z3

struct EncodeError {
    std::string status;
    std::string msg;
};

class SymMem {
public:
    SymMem(z3::context& c, MemEncoding enc, bool tags, std::vector<z3::expr>& side);

    z3::expr alloc(const z3::expr& guard, const z3::expr& size, MemKind k, unsigned align, int init);
    void free(const z3::expr& guard, const z3::expr& ptr);
    void revive(const z3::expr& guard, const z3::expr& ptr);  // stack objects only (Stmt::Revive)
    z3::expr stack_save();  // i64 token: number of objects so far
    void stack_restore(const z3::expr& guard, const z3::expr& token);

    z3::expr size(const z3::expr& ptr);   // bv64
    z3::expr live(const z3::expr& ptr);   // bv1
    z3::expr kind(const z3::expr& ptr);   // bv8
    z3::expr align(const z3::expr& ptr);  // bv64

    struct Loaded {
        z3::expr val;     // bv<width>
        z3::expr uninit;  // bv1: some byte uninitialised
        z3::expr tagbad;  // bv1
        z3::expr mask;    // bv<bytes>: bit k = byte k uninitialised
    };
    Loaded load(const z3::expr& ptr, unsigned width, unsigned tag);
    void store(const z3::expr& guard, const z3::expr& ptr, const z3::expr& val, unsigned width,
               const z3::expr& init, unsigned tag);
    void copy(const z3::expr& guard, const z3::expr& dst, const z3::expr& src, const z3::expr& len);
    void set(const z3::expr& guard, const z3::expr& dst, const z3::expr& byte, const z3::expr& len);

    std::size_t objects() const { return objs_.size(); }

    // Invariant evaluation (encode.cpp, Houdini over loop cuts): a snapshot
    // of the memory state at one point of the encoding, and an *exact* load
    // from that state. The exact load never uses provenance (known()) to
    // skip writes, so its value is the byte content of that address in that
    // state for any pointer value, also one outside every object.
    struct Mark {
        std::size_t upto = 0;              // Bv: log entries before the point
        std::optional<z3::expr> arr;       // Array: the array at the point
    };
    Mark mark() const { return Mark{log_.size(), arr_}; }
    Loaded load_exact(const Mark& m, const z3::expr& ptr, unsigned width);

    // k-induction footprint havoc (docs/PIR.md "k-induction with memory"):
    // every byte of each listed object (all = every object allocated so far
    // except `const` ones) gets an arbitrary value and tag. Its initialised
    // flag becomes arbitrary too, or, with keep_init, "initialised before or
    // arbitrary" (old | fresh): a byte that may be uninitialised before stays
    // maybe-uninitialised, never assumed initialised. Liveness, size and
    // kind are unchanged. Returns the number of objects havocked.
    std::size_t havoc_objects(const z3::expr& guard, const std::vector<uint64_t>& ids, bool all, bool keep_init);
    // The same for the objects the given pointer values point to (symbolic
    // object ids; loop-cut footprints). Returns the number of pointers.
    std::size_t havoc_pointed(const z3::expr& guard, const std::vector<z3::expr>& ptrs, bool keep_init);

    // Provenance: `e` is known to point into object `obj` on every path
    // without a reported violation (pointer arithmetic is checked to stay in
    // its object). Used only to skip writes to other objects in a read.
    void note(const z3::expr& e, uint64_t obj) {
        keep_.push_back(e);  // ids are only stable while the ast lives
        prov_[e.id()] = obj;
    }
    std::optional<uint64_t> known(const z3::expr& e) const;

    // Value sets (docs/PIR.md "Memory model", "Value sets"): a bit-vector
    // term that equals one of at most kVsMax constants, each under a guard;
    // the guards are exhaustive (their disjunction is valid), so the term is
    // exactly ite(g1, k1, ite(g2, k2, ... kn)). Derived from the term's
    // structure: numerals, ite, and operations whose operands all have value
    // sets (evaluated per combination). A load from an address with a value
    // set reads each constant address; a load of n bytes at a constant
    // address, where every write that may reach it is a whole n-byte store
    // at that address, is the ite over those stores' values (word level), so
    // a pointer stored in memory and loaded again keeps its value set.
    // Everything is an equality of terms, never an assumption.
    struct VsOpt {
        z3::expr guard;
        uint64_t val;
    };
    using Vs = std::vector<VsOpt>;
    static constexpr std::size_t kVsMax = 16;
    const Vs* vs_of(const z3::expr& e) const;

private:
    struct Obj {
        uint64_t id;
        z3::expr size;
        MemKind kind;
        unsigned align;
        z3::expr alive;  // bool
    };
    struct Entry {
        enum Kind { Byte, Set, Copy, Havoc, HavocObj } kind;
        z3::expr guard, addr, len, src, cell;
        int havoc = -1;
        bool keep = false;  // HavocObj: initialised flag = old | fresh
        int grp = -1;       // Byte: the store it belongs to (stores_), byte grp_k of it
        unsigned grp_k = 0;
    };
    struct StoreRec {
        z3::expr val;  // the stored bytes as one value (8 * n bits)
        unsigned n;
    };
    std::vector<StoreRec> stores_;
    z3::context& c_;
    MemEncoding enc_;
    bool tags_;
    unsigned cw_;
    std::vector<z3::expr>& side_;
    std::vector<Obj> objs_;
    // Array mode
    std::optional<z3::expr> arr_;
    int fresh_ = 0;
    // Bv mode
    std::vector<Entry> log_;
    std::vector<std::vector<std::pair<z3::expr, z3::expr>>> havoc_reads_;
    std::map<std::pair<int, unsigned>, z3::expr> havoc_memo_;  // (entry, address ast) -> its value
    z3::expr havoc_read(const Entry& e, const z3::expr& addr, unsigned width, const char* base);
    std::map<std::tuple<unsigned, std::size_t, bool>, z3::expr> memo_;
    std::unordered_map<unsigned, uint64_t> prov_;
    std::vector<z3::expr> keep_;
    mutable std::unordered_map<unsigned, std::optional<Vs>> vs_memo_;
    mutable std::vector<z3::expr> vs_keep_;
    std::optional<z3::expr> word_at(uint64_t a, unsigned n, std::size_t upto);
    // the objects a pointer may point into, when its value set says so
    std::optional<std::vector<uint64_t>> objs_of(const z3::expr& p) const;

    z3::expr bv(uint64_t v, unsigned w) { return c_.bv_val(v, w); }
    z3::expr objid(const z3::expr& p) { return p.extract(63, kObjShift); }
    z3::expr off(const z3::expr& p) { return z3::zext(p.extract(kObjShift - 1, 0), 64 - kObjShift); }
    std::optional<uint64_t> const_obj(const z3::expr& p);
    z3::expr in_range(const z3::expr& a, const z3::expr& base, const z3::expr& len);
    z3::expr cell_of(const z3::expr& byte, const z3::expr& init, unsigned tag);
    z3::expr read_cell(const z3::expr& addr);
    z3::expr read_cell_log(const z3::expr& addr, std::size_t upto, bool use_prov = true);
    void add_entry(Entry e);
    template <class F>
    z3::expr per_obj(const z3::expr& ptr, const z3::expr& dflt, F&& f);
};

#endif  // PRISM_HAS_Z3

// Concrete memory for the PIR interpreter (translation validation). Same
// semantics as SymMem; object ids are allocated dynamically.
class ConcMem {
public:
    struct Cell {
        uint8_t val = 0;
        bool init = false;
        uint8_t tag = 0;
        bool taint = false;  // value depends on a havoc
    };
    struct Obj {
        uint64_t size = 0;
        bool live = false;
        MemKind kind = MemKind::None;
        unsigned align = 1;
        std::vector<Cell> cells;
    };
    std::vector<Obj> objs;  // objs[id - 1]
    bool tags = false;

    // Returns the pointer; fails (nullopt) when the size is too large to run.
    std::optional<uint64_t> alloc(uint64_t size, MemKind k, unsigned align, int init);
    void free(uint64_t ptr);
    void revive(uint64_t ptr);  // stack objects only (Stmt::Revive)
    uint64_t token() const { return objs.size(); }
    void stack_restore(uint64_t token);
    const Obj* obj(uint64_t ptr) const;
    uint64_t size(uint64_t ptr) const;
    bool live(uint64_t ptr) const;
    uint64_t kind(uint64_t ptr) const;
    uint64_t align(uint64_t ptr) const;
    // Accesses outside a live object read 0 / drop the write (the checks
    // before them already stopped the interpreter on those paths).
    Cell read(uint64_t addr) const;
    void write(uint64_t addr, Cell c);
};

}  // namespace prism::pir::mem
