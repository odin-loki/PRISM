#include "prism/pir/folded_diag.hpp"

#include <map>

namespace prism::pir {
namespace {

namespace fs = std::filesystem;

std::vector<std::string> split_lines(const std::string& s) {
    std::vector<std::string> out;
    std::size_t pos = 0;
    while (pos <= s.size()) {
        auto nl = s.find('\n', pos);
        if (nl == std::string::npos) nl = s.size();
        std::string line = s.substr(pos, nl - pos);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        out.push_back(std::move(line));
        pos = nl + 1;
    }
    if (!out.empty() && out.back().empty()) out.pop_back();
    return out;
}

const std::map<std::string, std::string>& folding_flags() {
    static const std::map<std::string, std::string> k{
        {"-Wshift-sign-overflow", "INT-SHIFT-UB"},
        {"-Wshift-negative-value", "INT-SHIFT-UB"},
        {"-Wshift-overflow", "INT-SHIFT-UB"},
        {"-Winteger-overflow", "INT-SIGNED-OVF"},
    };
    return k;
}

}  // namespace

std::vector<FoldedUb> parse_folded_clang_diagnostics(const std::string& text, const fs::path& src) {
    std::vector<FoldedUb> out;
    for (auto& line : split_lines(text)) {
        auto w = line.find(": warning: ");
        if (w == std::string::npos) continue;
        auto lb = line.rfind(" [");
        if (lb == std::string::npos || line.back() != ']') continue;
        auto flag = line.substr(lb + 2, line.size() - lb - 3);
        auto it = folding_flags().find(flag);
        if (it == folding_flags().end()) continue;
        auto head = line.substr(0, w);
        auto c2 = head.rfind(':');
        if (c2 == std::string::npos) continue;
        auto c1 = head.rfind(':', c2 - 1);
        if (c1 == std::string::npos) continue;
        auto path = head.substr(0, c1);
        fs::path diag(path);
        if (diag != src) {
            std::error_code ec;
            bool ok = fs::equivalent(diag, src, ec);
            if (!ok)
                ok = diag.lexically_normal().generic_string() == src.lexically_normal().generic_string();
            if (!ok && diag.filename() == src.filename()) {
                const auto dpar = diag.parent_path();
                const auto spar = src.parent_path();
                if (dpar.empty() || dpar == "." || dpar.filename() == spar.filename()) ok = true;
            }
            if (!ok) continue;
        }
        FoldedUb f;
        try {
            f.line = std::stoi(head.substr(c1 + 1, c2 - c1 - 1));
            f.col = std::stoi(head.substr(c2 + 1));
        } catch (...) {
            continue;
        }
        f.cls = it->second;
        f.msg = line.substr(w + 11, lb - w - 11) + " [" + flag + "]";
        out.push_back(std::move(f));
    }
    return out;
}

}  // namespace prism::pir
