#pragma once

// Internal to prism_core: the concrete C interpreter (src/prism/stages/interp.cpp,
// the Python engine prism/concrete.py) used by concolic, fuse, rapid, muttest
// and execute.

#include "common.hpp"

namespace prism::stages_detail {

struct ReturnEx : std::exception {
    std::optional<int64_t> value;
    explicit ReturnEx(std::optional<int64_t> v = std::nullopt) : value(v) {}
};

bool type_is_unsigned(std::string_view typ);
int type_width(std::string_view typ);

// A C integer type as the concrete interpreter models it: width in bits
// (1 = _Bool, 8, 16, 32, 64) and signedness. Widths follow LP64, the same
// data model as the bmc encoder: long, size_t, ptrdiff_t and pointers are 64
// bits.
struct CT {
    int w = WIDTH;
    bool u = false;
    bool operator==(const CT&) const = default;
};
inline constexpr CT kInt{32, false};

// The scalar type named by `typ` (qualifiers and storage words dropped), or
// nullopt for a pointer, array, struct or unknown typedef.
std::optional<CT> scalar_ctype(std::string_view typ);

// v converted to t (C11 6.3.1.3): the value modulo 2^w, read as t. Values
// are kept as the mathematical value of their type, except unsigned 64-bit
// values, which are held as their bit pattern.
int64_t norm(int64_t v, CT t);

struct St {
    std::map<std::string, int64_t> vars;
    std::map<std::string, std::vector<int64_t>> arrays;
    std::map<std::string, CT> arr_t;  // element type of each array
    std::map<std::string, int> enums;
    std::map<std::string, CT> types;  // declared type of each scalar
    CT ret = kInt;
    bool ret_void = false;
    int steps = 0;
    St(const std::vector<std::pair<std::string, std::string>>& params, const Args& args,
       std::map<std::string, int> en)
        : enums(std::move(en)) {
        for (auto& [typ, name] : params) {
            if (name.empty()) continue;
            CT t = scalar_ctype(typ).value_or(CT{type_width(typ), type_is_unsigned(typ)});
            types[name] = t;
            int64_t raw = 0;
            auto it = args.find(name);
            if (it != args.end()) raw = it->second;
            vars[name] = norm(raw, t);
        }
    }
    void tick() {
        if (++steps > MAX_STEPS) throw ReturnEx(vars.contains("__ret") ? std::optional<int64_t>(vars["__ret"])
                                                                       : std::nullopt);
    }
};

int64_t eval_src(St& st, const std::string& src);

struct ExecResult {
    std::string ub;
    std::optional<int64_t> value;
    std::string error;
    int steps = 0;
};

ExecResult execute(const FunctionInfo& fn, const Args& args,
                   std::optional<std::map<std::string, int>> enums = std::nullopt);

// float/double in the signature or body: the concrete interpreter models
// integers only, so it must not run (or replay) such a function.
bool float_unencoded(const FunctionInfo& fn);

std::optional<bool> eval_cond(const FunctionInfo& fn, const Args& args, const std::string& cond);

}  // namespace prism::stages_detail
