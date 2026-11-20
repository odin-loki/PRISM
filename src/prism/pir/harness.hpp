#pragma once

// Per-function harness for pointer parameters (docs/PIR.md "Automatic
// pointer harness"). The planner reads the IR of one function (and of the
// module functions it passes the pointer to) and decides, for one pointer
// parameter, what object it points to: the pointee type from how the
// function uses the pointer, the object size from that type's layout, and
// the nested objects its pointer fields point to, down to a bounded depth.
// Every choice is a named assumption (HarnessPlan::assumptions); the
// translator allocates the objects (translate.cpp bind_auto_harness) and the
// verdict can only be PROVED-ASSUMING / BOUNDED / NEEDS-HARNESS, never PROVED.

#include "prism/pir.hpp"
#include "translate_mem.hpp"

#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace prism::pir::pirmem {

struct HarnessOptions {
    int depth = 2;                 // nested pointer levels below the parameter's own object
    uint64_t elems = 16;           // element count of an array-like pointee (indexed or scalar) without annotation
    uint64_t unknown_bytes = 64;   // object size when the function never accesses the pointee's type
    int max_objects = 32;          // objects per parameter (the planner stops nesting past it)
    bool container_of = false;     // a negative constant offset from the parameter: allocate the containing object
    // --pir-global-harness-deep: a nested pointer field the function uses without type evidence (an MMIO base it only
    // passes to readl) still gets an object of unknown_bytes arbitrary bytes instead of staying arbitrary
    bool keep_untyped = false;
    // --pir-ops-auto: the ops tables (explicit and unit-derived). A pointer field the function loads and calls
    // through whose (struct, index) has targets in it is bound to one of them instead of staying arbitrary.
    const std::vector<OpsField>* ops = nullptr;
    // the callee names must be defined or declared in the module
    const ir::Module* ops_module = nullptr;
    // --pir-length-pairing: a pointer field indexed under a bound by an integer field of the same object gets an object
    // of that many elements (LengthPair below)
    bool length_pairing = false;
    // --pir-declared-types: a pointee without type evidence (and one smaller than its declaration) takes the type and
    // size its declaration has in the unit's debug information (ir::Module::ditypes), a named assumption
    bool declared_types = false;
    // --pir-param-preconditions for this parameter: its own object is of this type / at least this many bytes
    std::optional<ir::Type> root_type;
    uint64_t root_min_bytes = 0;
    // --pir-priv-areas: a struct-typed pointer derived at the end of the object (netdev_priv-style private area) sizes
    // the object to that offset plus the area (from a visible allocation, else the area struct), a named assumption
    bool priv_areas = false;
    // --pir-index-field-bounds: an integer field of the object that indexes an in-object array [N x T] is in [0, N-1]
    bool index_field_bounds = false;
    // --pir-heap-freed: an object the function passes to kfree / kvfree / free ... is a heap allocation (named)
    bool heap_freed = false;
};

// --pir-length-pairing (docs/PIR.md "Length pairing"): a (pointer, count) pair the IR itself shows. The pointer is
// indexed (a getelementptr with a non-constant first index) in a block that only the in-bound edge of a comparison
// `index < count` (or <=, !=) reaches, or it is advanced by one element per iteration of a loop that counts `count`
// down to 0 or up from 0. Every piece of evidence for one pointer must name the same count and element size; two
// different counts, or element sizes, make no pair. The pair is an assumption ("points to at least count elements"),
// never a fact: the object has max(count, 0) x elem_bytes bytes (count read as unsigned when a bound is an unsigned
// or != comparison) and the size must fit the pointer model.
struct LengthPair {
    std::string count;         // a parameter's IR name, or "@<byte offset>" of an integer field of the containing object
    uint64_t count_off = 0;    // field pairs: byte offset of the count in the containing object (member_off not included)
    unsigned count_bits = 0;   // field pairs: width of the count field
    ir::Type elem;             // element type of the indexing / advancing getelementptr
    uint64_t elem_bytes = 0;
    bool is_signed = false;    // every bound is a signed comparison
    std::string evidence;      // the first bound found, for the assumption text
};

// The (pointer parameter, integer parameter) pair of p in f, through f's own code and the module functions it passes
// both to (three calls deep). nullopt: no evidence, or ambiguous evidence.
std::optional<LengthPair> infer_param_length(const ir::Module& m, const Layout& lay, const ir::Function& f, const ir::Param& p);

// A function-pointer field of a harness object bound to the targets of its ops-table entry.
struct HarnessFpField {
    uint64_t offset = 0;                // byte offset in the object (member_off not included)
    std::string struct_type;            // "%struct.T"
    int index = 0;                      // field index in it
    std::vector<std::string> targets;   // the field holds one of these functions
    std::string role;                   // OpsField::role
};

struct HarnessNode {
    std::string name;                // "hw", "hw->@16"
    ir::Type type;                   // type of one element
    bool type_known = false;
    uint64_t elems = 1;
    uint64_t bytes = 0;              // object size
    std::string basis;               // why this type/size
    uint64_t member_off = 0;         // container_of: the parameter points this many bytes into the object
    unsigned align = 16;             // the object's alignment: its type's own (ABI / __aligned), at least 16 (harness_align)
    // pointer fields (byte offset) that get their own object
    std::vector<std::pair<uint64_t, std::shared_ptr<HarnessNode>>> nested;
    // pointer fields the function loads and dereferences that stay arbitrary
    // (past the depth bound, over the object cap, or no pointee type evidence)
    std::vector<uint64_t> unbound_fields;
    std::vector<HarnessFpField> fp_fields;
    // --pir-index-field-bounds: an integer field (byte offset, width) used as the index of an in-object array of
    // `count` elements; the field is assumed in [0, count - 1] (named, not discharged)
    struct IndexBound {
        uint64_t offset = 0;
        unsigned width = 0;
        uint64_t count = 0;
        uint64_t array_off = 0;
        std::string where;  // "%struct.S field K"
    };
    std::vector<IndexBound> index_bounds;
    bool heap = false;  // --pir-heap-freed: allocated as a heap object (the function frees it)
    // why a pointer field the function loads has no object of its own (recorded, see unbound_lines)
    int cut_depth = 0;     // past the depth bound
    int cut_cap = 0;       // over the object cap
    int cut_notype = 0;    // no pointee type evidence
    // --pir-length-pairing: the object has `count` elements, the count read from the containing object's field at
    // entry (bytes and elems are then 0). The node is flat: pointer fields of its elements are arbitrary values.
    std::optional<LengthPair> pair;
};

// Alignment of a harness object of type t: max(16, the type's alignment), at most 4096. A type declared with a larger
// alignment (____cacheline_aligned members, __aligned(64)) is aligned like that, as every object of the type is.
unsigned harness_align(const Layout& lay, const ir::Type& t);

struct HarnessPlan {
    std::shared_ptr<HarnessNode> root;
    std::vector<std::string> assumptions;  // one named line per choice
    int objects = 0;
    int depth_reached = 0;  // deepest nesting level of an object (0 = the parameter's own object)
    bool root_override_used = false;  // root_type / root_min_bytes changed the parameter's object
    int declared_used = 0;            // objects whose type or size came from the declaration (--pir-declared-types)
};

// The declared pointee of a pointer (debug information, full -g): its size and spelling, and the stripped
// DI type ref (to find the declared types of its members).
struct DeclaredPointee {
    uint64_t bytes = 0;
    std::string ref;        // stripped pointee type node
    std::string spelling;   // "struct mount", "u8", ...
    bool composite = false; // struct / union / array
    std::string ir_struct;  // "%struct.mount" when the module has that struct with the same size
};
// The declared pointee type of parameter p of f; nullopt without full debug information or a sized pointee.
std::optional<DeclaredPointee> declared_param_pointee(const ir::Module& m, const Layout& lay, const ir::Function& f,
                                                      const ir::Param& p);
// The declared pointee of the pointer member at byte `off` of an object of DI type `ref` (nested structs/arrays).
std::optional<DeclaredPointee> declared_member_pointee(const ir::Module& m, const Layout& lay, const std::string& ref,
                                                       uint64_t off);

// nullopt (and `why`): the parameter cannot be given an object (negative
// offsets such as container_of, an object too large for the model).
std::optional<HarnessPlan> plan_pointer_harness(const ir::Module& m, const Layout& lay, const ir::Function& f,
                                                const ir::Param& p, const HarnessOptions& opt, std::string* why);

// --pir-global-harness-deep: the same planner for an object reached through SSA values of several functions (the
// loads of a global's pointer, `roots`: function and value name), named `name` in the assumptions.
std::optional<HarnessPlan> plan_harness_roots(const ir::Module& m, const Layout& lay,
                                              const std::vector<std::pair<const ir::Function*, std::string>>& roots,
                                              const std::string& name, const HarnessOptions& opt, std::string* why);

}  // namespace prism::pir::pirmem
