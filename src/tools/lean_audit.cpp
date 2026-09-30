// lean_audit: the source and axiom audits of PRISM's Lean proofs
// (proofs/check.sh, proofs/semantics/check.sh, proofs/refinement/check.sh,
// proofs/refinement/recheck.sh). Standard library only, one file, so the
// proof jobs build it with `${CXX:-c++} -std=c++23 -O2` without CMake or
// third_party/.
//
//   lean_audit scan --words W1,W2 [--toplevel K1,K2] PATH...
//       Every *.lean file under each PATH (a file, or a directory walked
//       recursively; anything inside a .lake directory is skipped), with
//       comments removed, is searched for the words (whole identifiers) and
//       for the keywords at the start of a line (`^\s*axiom`). Prints
//       "<path>: <word>" per hit, else "ok". Exit 1 on a hit.
//   lean_audit axioms LOG [--allowed A1,A2]
//       Reads `#print axioms` output: every "'decl' depends on axioms: [...]"
//       line must name only allowed axioms (default propext,
//       Classical.choice, Quot.sound). Prints "<decl>: ['X', ...]" per
//       offender, else "ok: only <allowed>". Exit 1 on an offender.
//
// Comments are removed by one lexer pass that follows Lean: `--` to the end
// of the line, `/- ... -/` nested, and neither inside a string literal.
// Newlines inside block comments are kept, and string contents are still
// scanned, so the scan errs towards reporting a word, never towards hiding
// one. Exit 2 on a usage or read error.

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#ifndef LEAN_AUDIT_NO_MAIN
#define LEAN_AUDIT_INLINE
#else
#define LEAN_AUDIT_INLINE inline
#endif

namespace lean_audit {
namespace fs = std::filesystem;

// Lean source without comments (see the file comment).
LEAN_AUDIT_INLINE std::string strip_comments(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    std::size_t i = 0;
    const std::size_t n = s.size();
    while (i < n) {
        char c = s[i];
        if (c == '"') {  // string literal, kept; comment markers inside do not count
            out += c;
            ++i;
            while (i < n && s[i] != '"') {
                if (s[i] == '\\' && i + 1 < n) out += s[i++];
                out += s[i++];
            }
            if (i < n) out += s[i++];
            continue;
        }
        if (c == '-' && i + 1 < n && s[i + 1] == '-') {
            while (i < n && s[i] != '\n') ++i;
            continue;
        }
        if (c == '/' && i + 1 < n && s[i + 1] == '-') {
            int depth = 1;
            i += 2;
            while (i < n && depth > 0) {
                if (s[i] == '/' && i + 1 < n && s[i + 1] == '-') {
                    ++depth;
                    i += 2;
                } else if (s[i] == '-' && i + 1 < n && s[i + 1] == '/') {
                    --depth;
                    i += 2;
                } else {
                    if (s[i] == '\n') out += '\n';
                    ++i;
                }
            }
            continue;
        }
        out += c;
        ++i;
    }
    return out;
}

// Identifier characters for the whole-word test. Bytes of multi-byte UTF-8
// characters count as separators, so a word next to one is still reported.
LEAN_AUDIT_INLINE bool word_char(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
}

struct Hit {
    std::size_t pos;
    std::string word;
};

// Whole-word hits of `words`, and `toplevel` keywords that start a line
// (after spaces), in text order.
LEAN_AUDIT_INLINE std::vector<Hit> find_hits(std::string_view code, const std::vector<std::string>& words,
                                             const std::vector<std::string>& toplevel) {
    std::vector<Hit> hits;
    auto whole = [&](std::size_t p, std::size_t len) {
        return (p == 0 || !word_char(code[p - 1])) && (p + len >= code.size() || !word_char(code[p + len]));
    };
    for (std::size_t p = 0; p < code.size(); ++p) {
        if (!word_char(code[p]) || (p > 0 && word_char(code[p - 1]))) continue;  // identifier starts only
        for (const auto& w : words)
            if (!w.empty() && code.compare(p, w.size(), w) == 0 && whole(p, w.size())) hits.push_back({p, w});
        std::size_t b = p;
        while (b > 0 && (code[b - 1] == ' ' || code[b - 1] == '\t' || code[b - 1] == '\r' || code[b - 1] == '\f' ||
                         code[b - 1] == '\v'))
            --b;
        if (b == 0 || code[b - 1] == '\n')
            for (const auto& k : toplevel)
                if (!k.empty() && code.compare(p, k.size(), k) == 0 && whole(p, k.size())) hits.push_back({p, k});
    }
    return hits;
}

LEAN_AUDIT_INLINE std::vector<std::string> split_list(std::string_view s) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : s) {
        if (c == ',') {
            if (!cur.empty()) out.push_back(cur);
            cur.clear();
        } else if (c != ' ') {
            cur += c;
        }
    }
    if (!cur.empty()) out.push_back(cur);
    return out;
}

LEAN_AUDIT_INLINE bool in_lake(const fs::path& p) {
    for (const auto& part : p)
        if (part == ".lake") return true;
    return false;
}

// The .lean files of one PATH argument, sorted; a directory is walked.
LEAN_AUDIT_INLINE std::vector<fs::path> lean_files(const fs::path& arg, std::error_code& ec) {
    std::vector<fs::path> out;
    if (fs::is_regular_file(arg, ec)) {
        out.push_back(arg);
        return out;
    }
    if (!fs::is_directory(arg, ec)) {
        if (!ec) ec = std::make_error_code(std::errc::no_such_file_or_directory);
        return out;
    }
    for (fs::recursive_directory_iterator it(arg, ec), end; it != end && !ec; it.increment(ec)) {
        std::error_code e2;
        if (it->is_directory(e2) && it->path().filename() == ".lake") {
            it.disable_recursion_pending();
            continue;
        }
        if (it->is_regular_file(e2) && it->path().extension() == ".lean") {
            auto rel = it->path().lexically_relative(arg);
            if (!in_lake(rel)) out.push_back(arg == "." ? rel : it->path());
        }
    }
    std::sort(out.begin(), out.end());
    return out;
}

LEAN_AUDIT_INLINE bool read_file(const fs::path& p, std::string& out) {
    std::ifstream in(p, std::ios::binary);
    if (!in) return false;
    std::stringstream ss;
    ss << in.rdbuf();
    out = ss.str();
    return true;
}

// Offending axiom lines of a `#print axioms` log: "<decl>: ['X', 'Y']".
LEAN_AUDIT_INLINE std::vector<std::string> axiom_offenders(std::string_view log, const std::set<std::string>& allowed) {
    std::vector<std::string> bad;
    std::istringstream in{std::string(log)};
    constexpr std::string_view kDep = "' depends on axioms: [";
    for (std::string line; std::getline(in, line);) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        auto d = line.find(kDep);
        if (d == std::string::npos) continue;
        if (d == 0) continue;
        auto q = line.rfind('\'', d - 1);
        if (q == std::string::npos) continue;
        std::string decl = line.substr(q + 1, d - q - 1);
        if (decl.empty() || decl.find('\'') != std::string::npos) continue;
        auto open = d + kDep.size();
        auto close = line.rfind(']');
        if (close == std::string::npos || close < open) continue;
        std::set<std::string> extra;
        for (auto& a : split_list(std::string_view(line).substr(open, close - open)))
            if (!allowed.count(a)) extra.insert(a);
        if (extra.empty()) continue;
        std::string row = decl + ": [";
        bool first = true;
        for (auto& a : extra) {
            row += (first ? "'" : ", '") + a + "'";
            first = false;
        }
        bad.push_back(row + "]");
    }
    return bad;
}

LEAN_AUDIT_INLINE int usage() {
    std::cerr << "usage: lean_audit scan --words W1,W2 [--toplevel K1,K2] PATH...\n"
                 "       lean_audit axioms LOG [--allowed A1,A2]\n";
    return 2;
}

LEAN_AUDIT_INLINE int run(int argc, char** argv, std::ostream& out, std::ostream& err) {
    if (argc < 2) return usage();
    const std::string cmd = argv[1];
    if (cmd == "scan") {
        std::vector<std::string> words, toplevel;
        std::vector<fs::path> paths;
        for (int i = 2; i < argc; ++i) {
            std::string a = argv[i];
            if (a == "--words" && i + 1 < argc) words = split_list(argv[++i]);
            else if (a == "--toplevel" && i + 1 < argc) toplevel = split_list(argv[++i]);
            else if (a.rfind("--", 0) == 0) return usage();
            else paths.emplace_back(a);
        }
        if (paths.empty() || (words.empty() && toplevel.empty())) return usage();
        std::vector<std::string> bad;
        for (const auto& arg : paths) {
            std::error_code ec;
            auto files = lean_files(arg, ec);
            if (ec) {
                err << "lean_audit: cannot read " << arg.string() << ": " << ec.message() << "\n";
                return 2;
            }
            for (const auto& f : files) {
                std::string text;
                if (!read_file(f, text)) {
                    err << "lean_audit: cannot read " << f.string() << "\n";
                    return 2;
                }
                for (const auto& h : find_hits(strip_comments(text), words, toplevel))
                    bad.push_back(f.generic_string() + ": " + h.word);
            }
        }
        for (const auto& b : bad) out << b << "\n";
        if (bad.empty()) out << "ok\n";
        return bad.empty() ? 0 : 1;
    }
    if (cmd == "axioms") {
        std::vector<std::string> allowed_list{"propext", "Classical.choice", "Quot.sound"};
        std::string log;
        for (int i = 2; i < argc; ++i) {
            std::string a = argv[i];
            if (a == "--allowed" && i + 1 < argc) allowed_list = split_list(argv[++i]);
            else if (a.rfind("--", 0) == 0 || !log.empty()) return usage();
            else log = a;
        }
        if (log.empty()) return usage();
        std::string text;
        if (!read_file(log, text)) {
            err << "lean_audit: cannot read " << log << "\n";
            return 2;
        }
        auto bad = axiom_offenders(text, std::set<std::string>(allowed_list.begin(), allowed_list.end()));
        for (const auto& b : bad) out << b << "\n";
        if (bad.empty()) {
            out << "ok: only";
            for (std::size_t i = 0; i < allowed_list.size(); ++i) out << (i ? " / " : " ") << allowed_list[i];
            out << "\n";
        }
        return bad.empty() ? 0 : 1;
    }
    return usage();
}

}  // namespace lean_audit

#ifndef LEAN_AUDIT_NO_MAIN
int main(int argc, char** argv) { return lean_audit::run(argc, argv, std::cout, std::cerr); }
#endif
