#include "prism/abi_model.hpp"

#include <cctype>
#include <string>

namespace prism {

AbiModel abi_lp64() { return AbiModel{}; }

AbiModel abi_ilp32() {
    AbiModel a;
    a.long_w = 32;
    a.size_w = 32;
    a.ptr_w = 32;
    return a;
}

namespace {

std::string upper_ascii(std::string_view s) {
    std::string out(s);
    for (auto& c : out) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return out;
}

}  // namespace

AbiModel abi_from_data_model(std::string_view data_model) {
    if (upper_ascii(data_model) == "ILP32") return abi_ilp32();
    return abi_lp64();
}

bool is_ilp32(std::string_view data_model) { return upper_ascii(data_model) == "ILP32"; }

}  // namespace prism
