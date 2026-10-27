#pragma once

#include "prism/export.hpp"

#include <string_view>

namespace prism {

// C ABI widths for the hand BMC encoder and SV-COMP replay (ILP32 vs LP64).
struct AbiModel {
    int int_w = 32;
    int long_w = 64;
    int long_long_w = 64;
    int size_w = 64;
    int ptr_w = 64;
    int ptr_bytes() const { return ptr_w / 8; }
    int size_bytes() const { return size_w / 8; }
};

PRISM_API AbiModel abi_lp64();
PRISM_API AbiModel abi_ilp32();
// Only "ILP32" (any case) selects 32-bit long, size_t and pointers.
// "LP64", empty, and every other string stay LP64: a typo must not narrow widths.
PRISM_API AbiModel abi_from_data_model(std::string_view data_model);
PRISM_API bool is_ilp32(std::string_view data_model);

}  // namespace prism
