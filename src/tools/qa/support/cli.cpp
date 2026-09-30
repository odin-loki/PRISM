#include "cli.hpp"

#include <iostream>

namespace prism::qa {

std::string Args::get(const std::string& n, const std::string& dflt) const {
    auto it = values.find(n);
    if (it == values.end() || it->second.empty()) return dflt;
    return it->second.back();
}

bool parse_args(const std::string& prog, const std::string& desc, const std::vector<ArgSpec>& specs,
                const std::vector<std::string>& argv, Args& out, int& rc) {
    auto usage = [&](std::ostream& os) {
        os << "usage: " << prog << " [options]\n\n" << desc << "\n\noptions:\n";
        for (const auto& s : specs) {
            std::string names = s.short_name.empty() ? s.long_name : s.short_name + ", " + s.long_name;
            if (s.takes_value) names += " VALUE";
            os << "  " << names;
            if (names.size() < 28) os << std::string(28 - names.size(), ' ');
            else os << "\n  " << std::string(28, ' ');
            os << s.help << "\n";
        }
    };
    for (std::size_t i = 0; i < argv.size(); ++i) {
        const std::string& a = argv[i];
        if (a == "-h" || a == "--help") {
            usage(std::cout);
            rc = 0;
            return false;
        }
        const ArgSpec* spec = nullptr;
        std::string value;
        bool inline_value = false;
        for (const auto& s : specs) {
            if (a == s.long_name || (!s.short_name.empty() && a == s.short_name)) {
                spec = &s;
                break;
            }
            if (s.takes_value && a.rfind(s.long_name + "=", 0) == 0) {
                spec = &s;
                value = a.substr(s.long_name.size() + 1);
                inline_value = true;
                break;
            }
            if (s.takes_value && !s.short_name.empty() && a.size() > s.short_name.size() &&
                a.rfind(s.short_name, 0) == 0 && a.rfind("--", 0) != 0) {
                spec = &s;
                value = a.substr(s.short_name.size());
                inline_value = true;
                break;
            }
        }
        if (!spec) {
            usage(std::cerr);
            std::cerr << prog << ": error: unrecognized argument: " << a << "\n";
            rc = 2;
            return false;
        }
        if (!spec->takes_value) {
            out.flags.insert(spec->long_name);
            continue;
        }
        if (!inline_value) {
            if (i + 1 >= argv.size()) {
                std::cerr << prog << ": error: argument " << spec->long_name << ": expected one argument\n";
                rc = 2;
                return false;
            }
            value = argv[++i];
        }
        auto& vs = out.values[spec->long_name];
        if (!spec->repeat) vs.clear();
        vs.push_back(value);
    }
    rc = 0;
    return true;
}

}  // namespace prism::qa
