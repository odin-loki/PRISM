// leandb text vectors.

#include <cctype>

#include "internal.hpp"
#include "prism/sim/term.hpp"  // mix64

namespace prism::leandb::detail {

const char* tbl_name(int t) {
    switch (t) {
        case 0: return "decls";
        case 1: return "problems";
        default: return "trace_theorems";
    }
}
int tbl_code(std::string_view n) { return n == "decls" ? 0 : n == "problems" ? 1 : 2; }
int use_class(std::string_view u) { return u == "library" ? 0 : u == "data" ? 1 : 2; }

std::vector<std::string> words(std::string_view text) {
    std::vector<std::string> out;
    std::string cur;
    auto flush = [&] {
        if (!cur.empty()) { out.push_back(std::move(cur)); cur.clear(); }
    };
    for (unsigned char ch : text) {
        if (std::isalnum(ch)) cur += static_cast<char>(std::tolower(ch));
        else if (ch >= 0x80) cur += static_cast<char>(ch);
        else flush();
    }
    flush();
    return out;
}

std::vector<std::uint64_t> simhash(std::string_view text) {
    int acc[256] = {0};
    for (const auto& w : words(text)) {
        std::uint64_t h = 0xcbf29ce484222325ULL;
        for (unsigned char c : w) h = (h ^ c) * 0x100000001b3ULL;
        for (int part = 0; part < 4; ++part) {
            const std::uint64_t x = prism::sim::mix64(h + static_cast<std::uint64_t>(part) * 0x9e3779b97f4a7c15ULL);
            for (int b = 0; b < 64; ++b) acc[part * 64 + b] += ((x >> b) & 1) ? 1 : -1;
        }
    }
    std::vector<std::uint64_t> out(4, 0);
    for (int i = 0; i < 256; ++i)
        if (acc[i] > 0) out[static_cast<std::size_t>(i / 64)] |= std::uint64_t{1} << (i % 64);
    return out;
}

}  // namespace prism::leandb::detail
