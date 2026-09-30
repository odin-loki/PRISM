// Licence firewall (roadmap Part 1.2).
//
// Fails when a `linked` component carries a copyleft SPDX licence. GPL,
// LGPL, AGPL, SSPL, EUPL, OSL, CPAL, CC-BY-SA, RPL, QPL are strong/network
// copyleft; MPL, EPL, CDDL, CPL, MS-RL, APSL are weak (file-level) copyleft
// and also fail unless the component name is in kAllowWeakCopyleft after
// review. External and system tools may carry any licence: PRISM runs them
// as separate, unmodified processes and never links them. A linked
// component without a licence (or NOASSERTION), or whose recorded licence
// file is gone from the tree, also fails.
#include "prism/deps.hpp"

#include <algorithm>
#include <cctype>
#include <regex>

namespace prism::deps {

namespace {

constexpr const char* kStrongCopyleft[] = {"GPL", "LGPL", "AGPL", "SSPL", "EUPL",
                                           "OSL", "CPAL", "CC-BY-SA", "RPL", "QPL"};
constexpr const char* kWeakCopyleft[] = {"MPL", "EPL", "CDDL", "CPL", "MS-RL", "APSL"};
// Linked components allowed to carry a weak-copyleft licence after legal
// review. Empty on purpose: add a name only with a reviewed justification.
constexpr const char* kAllowWeakCopyleft[] = {""};
// SPDX exceptions that make a copyleft licence safe to link (runtime-library style).
constexpr const char* kLinkExceptions[] = {"GCC-exception-3.1", "LLVM-exception",
                                           "Classpath-exception-2.0"};

std::string strip(std::string_view s) {
    std::size_t b = 0, e = s.size();
    while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;
    return std::string(s.substr(b, e - b));
}

int rank(const std::string& c) { return c == "strong" ? 2 : c == "weak" ? 1 : 0; }

}  // namespace

std::vector<std::string> spdx_ids(std::string_view expr) {
    std::string e(expr);
    std::replace(e.begin(), e.end(), '(', ' ');
    std::replace(e.begin(), e.end(), ')', ' ');
    e = strip(e);
    static const std::regex split(R"(\s+(?:AND|OR)\s+)");
    std::vector<std::string> out;
    for (std::sregex_token_iterator it(e.begin(), e.end(), split, -1), end; it != end; ++it) {
        auto p = strip(it->str());
        if (!p.empty()) out.push_back(p);
    }
    return out;
}

std::string classify(std::string_view spdx_id) {
    std::string_view base = spdx_id, exc;
    if (auto w = spdx_id.find(" WITH "); w != std::string_view::npos) {
        base = spdx_id.substr(0, w);
        exc = spdx_id.substr(w + 6);
    }
    std::string b = strip(base);
    for (char& c : b) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    const std::string x = strip(exc);
    for (const char* ok : kLinkExceptions)
        if (x == ok) return "permissive";
    auto starts = [&](const std::string& p) { return b.rfind(p, 0) == 0; };
    for (const char* tag : kStrongCopyleft) {
        std::string t(tag);
        if (b == t || starts(t + "-") || starts(t + "V")) return "strong";
    }
    for (const char* tag : kWeakCopyleft) {
        std::string t(tag);
        if (b == t || starts(t + "-")) return "weak";
    }
    return "permissive";
}

std::vector<std::string> licence_problems(const std::vector<Table>& comps, const fs::path& root) {
    std::vector<std::string> problems;
    for (const auto& c : comps) {
        if (c.str("kind") != "linked") continue;
        const std::string name = c.has("name") ? c.str("name") : "?";
        const std::string spdx = strip(c.str("spdx"));
        std::string upper = spdx;
        for (char& ch : upper) ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
        if (spdx.empty() || upper == "NOASSERTION") {
            problems.push_back(name + ": linked component without an SPDX licence");
            continue;
        }
        const auto ids = spdx_ids(spdx);
        if (ids.empty()) {
            problems.push_back(name + ": linked component without an SPDX licence");
            continue;
        }
        std::string worst = "permissive";
        if (spdx.find(" OR ") != std::string::npos && spdx.find(" AND ") == std::string::npos) {
            // "A OR B": one permissive choice is enough.
            bool perm = false, weak = false;
            for (const auto& i : ids) {
                auto k = classify(i);
                perm |= k == "permissive";
                weak |= k == "weak";
            }
            worst = perm ? "permissive" : weak ? "weak" : "strong";
        } else {
            for (const auto& i : ids)
                if (auto k = classify(i); rank(k) > rank(worst)) worst = k;
        }
        bool allowed_weak = std::any_of(std::begin(kAllowWeakCopyleft), std::end(kAllowWeakCopyleft),
                                        [&](const char* n) { return *n && name == n; });
        if (worst == "strong")
            problems.push_back(name + ": linked component is copyleft (" + spdx +
                               "); run it as an external process instead");
        else if (worst == "weak" && !allowed_weak)
            problems.push_back(name + ": linked component is weak copyleft (" + spdx +
                               "); needs review + kAllowWeakCopyleft (src/tools/prism_deps/licence.cpp)");
        std::string lf = c.str("licence_file");
        lf = lf.substr(0, lf.find(' '));
        const std::string path = c.str("path");
        std::error_code ec;
        if (!lf.empty() && !path.empty() && !fs::exists(root / path / lf, ec))
            problems.push_back(name + ": licence file " + path + "/" + lf + " is missing");
    }
    return problems;
}

}  // namespace prism::deps
