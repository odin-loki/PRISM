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
    // --pir-ops-auto: the ops tables (explicit and unit-derived). A pointer field the function loads and calls
    // through whose (struct, index) has targets in it is bound to one of them instead of staying arbitrary.
    const std::vector<OpsField>* ops = nullptr;
    // the callee names must be defined or declared in the module
    const ir::Module* ops_module = nullptr;
};

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
    // pointer fields (byte offset) that get their own object
    std::vector<std::pair<uint64_t, std::shared_ptr<HarnessNode>>> nested;
    // pointer fields the function loads and dereferences that stay arbitrary
    // (past the depth bound, over the object cap, or no pointee type evidence)
    std::vector<uint64_t> unbound_fields;
    std::vector<HarnessFpField> fp_fields;
    // why a pointer field the function loads has no object of its own (recorded, see unbound_lines)
    int cut_depth = 0;     // past the depth bound
    int cut_cap = 0;       // over the object cap
    int cut_notype = 0;    // no pointee type evidence
};

struct HarnessPlan {
    std::shared_ptr<HarnessNode> root;
    std::vector<std::string> assumptions;  // one named line per choice
    int objects = 0;
    int depth_reached = 0;  // deepest nesting level of an object (0 = the parameter's own object)
};

// nullopt (and `why`): the parameter cannot be given an object (negative
// offsets such as container_of, an object too large for the model).
std::optional<HarnessPlan> plan_pointer_harness(const ir::Module& m, const Layout& lay, const ir::Function& f,
                                                const ir::Param& p, const HarnessOptions& opt, std::string* why);

}  // namespace prism::pir::pirmem
