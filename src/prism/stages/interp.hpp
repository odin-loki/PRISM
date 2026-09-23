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

struct St {
    std::map<std::string, int64_t> vars;
    std::map<std::string, std::vector<int64_t>> arrays;
    std::map<std::string, int> enums;
    std::unordered_set<std::string> uns;
    std::map<std::string, int> bits;
    int steps = 0;
    St(const std::vector<std::pair<std::string, std::string>>& params, const std::map<std::string, int>& args,
       std::map<std::string, int> en)
        : enums(std::move(en)) {
        for (auto& [typ, name] : params) {
            if (name.empty()) continue;
            if (type_is_unsigned(typ)) uns.insert(name);
            int w = type_width(typ);
            bits[name] = w;
            int raw = 0;
            auto it = args.find(name);
            if (it != args.end()) raw = it->second;
            vars[name] = w <= 32 ? i32(raw) : raw;
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

ExecResult execute(const FunctionInfo& fn, const std::map<std::string, int>& args,
                   std::optional<std::map<std::string, int>> enums = std::nullopt);

std::optional<bool> eval_cond(const FunctionInfo& fn, const Args& args, const std::string& cond);

}  // namespace prism::stages_detail
