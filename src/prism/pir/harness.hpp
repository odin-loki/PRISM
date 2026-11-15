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
};

struct HarnessNode {
    std::string name;                // "hw", "hw->@16"
    ir::Type type;                   // type of one element
    bool type_known = false;
    uint64_t elems = 1;
    uint64_t bytes = 0;              // object size
    std::string basis;               // why this type/size
    // pointer fields (byte offset) that get their own object
    std::vector<std::pair<uint64_t, std::shared_ptr<HarnessNode>>> nested;
    // pointer fields the function loads and dereferences that stay arbitrary
    // (past the depth bound, over the object cap, or no pointee type evidence)
    std::vector<uint64_t> unbound_fields;
};

struct HarnessPlan {
    std::shared_ptr<HarnessNode> root;
    std::vector<std::string> assumptions;  // one named line per choice
    int objects = 0;
};

// nullopt (and `why`): the parameter cannot be given an object (negative
// offsets such as container_of, an object too large for the model).
std::optional<HarnessPlan> plan_pointer_harness(const ir::Module& m, const Layout& lay, const ir::Function& f,
                                                const ir::Param& p, const HarnessOptions& opt, std::string* why);

}  // namespace prism::pir::pirmem
