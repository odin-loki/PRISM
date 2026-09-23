// IR rewrites before opt for control-flow features (see lower_ctl.hpp).

#include "lower_ctl.hpp"

#include <sstream>
#include <vector>

namespace prism::pir::pirctl {

namespace {

bool calls_setjmp(const std::string& line) {
    for (const char* n : {"@_setjmp(", "@setjmp(", "@__sigsetjmp(", "@sigsetjmp("})
        if (line.find(n) != std::string::npos && line.find("call ") != std::string::npos) return true;
    return false;
}

}  // namespace

std::string keep_setjmp_locals(const std::string& ir) {
    std::vector<std::string> lines;
    {
        std::istringstream in(ir);
        std::string l;
        while (std::getline(in, l)) lines.push_back(l);
    }
    std::ostringstream out;
    bool any = false;
    for (std::size_t i = 0; i < lines.size(); ++i) {
        const auto& l = lines[i];
        if (!l.starts_with("define ")) {
            out << l << "\n";
            continue;
        }
        std::size_t end = i;
        bool sj = false;
        while (end < lines.size() && lines[end] != "}") sj = sj || calls_setjmp(lines[end++]);
        for (std::size_t k = i; k <= end && k < lines.size(); ++k) {
            out << lines[k] << "\n";
            if (!sj) continue;
            const auto& a = lines[k];
            auto eq = a.find(" = alloca ");
            auto pct = a.find('%');
            if (eq == std::string::npos || pct == std::string::npos || pct > eq) continue;
            out << "  call void @__prism.keep(ptr " << a.substr(pct, eq - pct) << ")\n";
            any = true;
        }
        i = end;
    }
    if (any) out << "declare void @__prism.keep(ptr)\n";
    return out.str();
}

std::string uninit_fp_locals(const std::string& ir) {
    std::istringstream in(ir);
    std::ostringstream out;
    std::string l;
    bool used[3] = {false, false, false};
    static const char* kTy[3] = {"half", "float", "double"};
    int uniq = 0;
    while (std::getline(in, l)) {
        out << l << "\n";
        auto a = l.find(" = alloca ");
        if (a == std::string::npos) continue;
        auto nm = l.substr(0, a);
        nm.erase(0, nm.find_first_not_of(' '));
        if (!nm.starts_with("%") || nm == "%retval") continue;
        auto rest = l.substr(a + 10);
        for (int k = 0; k < 3; ++k) {
            std::string t = kTy[k];
            if (!rest.starts_with(t)) continue;
            auto tail = rest.substr(t.size());
            if (!(tail.empty() || tail.starts_with(", align ")) || tail.find(", i") != std::string::npos) break;
            std::string v = "%__prism_uninit_fp." + std::to_string(uniq++);
            out << "  " << v << " = call " << t << " @__prism.uninit." << t << "()\n";
            out << "  store " << t << " " << v << ", ptr " << nm << "\n";
            used[k] = true;
            break;
        }
    }
    for (int k = 0; k < 3; ++k)
        if (used[k]) out << "declare " << kTy[k] << " @__prism.uninit." << kTy[k] << "()\n";
    return out.str();
}

}  // namespace prism::pir::pirctl
