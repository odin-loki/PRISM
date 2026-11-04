// Correct C++ subscripts the vector/string index lint used to miss as
// guarded. Reduced from src/prism/sandbox.cpp bwrap_argv, models.cpp
// utf8_with_replacement, svcomp/witness.cpp utf8_seq_len, conc/stage.cpp
// weak_memory_order, pir/memory.cpp vs_bin and shallow_vs, and
// astlint_checks.cpp str_null_arg.
#include <cstddef>
#include <string>
#include <vector>

std::string first_arg(const std::vector<std::string>& argv) {
    if (!argv.empty()) return argv[0];
    return "";
}

bool is_cont(const std::string& text, std::size_t k) {
    const auto n = text.size();
    return k < n && (static_cast<unsigned char>(text[k]) & 0xC0) == 0x80;
}

int lead_byte(const std::string& s, std::size_t i) {
    const std::size_t n = s.size();
    if (i >= n) return -1;
    return static_cast<unsigned char>(s[i]);
}

bool left_ident(const std::string& text) {
    std::size_t p = 0;
    while ((p = text.find("tok", p)) != std::string::npos) {
        if (p > 0 && text[p - 1] == '_') return true;
        ++p;
    }
    return false;
}

std::size_t pair_sum(std::size_t a) {
    std::vector<std::size_t> idx{0, 0};
    idx[0] = a;
    return idx[0] + idx[1];
}

int arg_at(const std::vector<int>& args, int idx) {
    if (idx < 0 || idx >= static_cast<int>(args.size())) return 0;
    return args[static_cast<std::size_t>(idx)];
}

int shape_of(const std::vector<std::vector<int>>& ops) {
    int shape_i = -1;
    for (std::size_t i = 0; i < ops.size(); ++i) {
        if (shape_i < 0) shape_i = static_cast<int>(i);
        else if (ops[i].size() != ops[static_cast<std::size_t>(shape_i)].size()) return -1;
    }
    return shape_i;
}
