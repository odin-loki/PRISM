#include "docscan.hpp"

#include "prism/regex.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <map>
#include <sstream>
#include <system_error>
#include <utility>

namespace fs = std::filesystem;

namespace prism::qa {

namespace {

// Python re.sub over prism::Regex (PCRE2, UTF + UCP).
template <class Fn>
std::string re_sub(const Regex& rx, const std::string& text, Fn&& repl) {
    std::string out;
    std::size_t at = 0;
    for (const auto& m : rx.finditer(text)) {
        const auto [s, e] = m.spans[0];
        if (s < 0 || static_cast<std::size_t>(s) < at) continue;
        out.append(text, at, static_cast<std::size_t>(s) - at);
        out += repl(m);
        at = static_cast<std::size_t>(e);
    }
    out.append(text, at, std::string::npos);
    return out;
}

std::string strip_fences(const std::string& text) {
    static const Regex fence("```.*?```", false, true);
    return re_sub(fence, text, [](const Match&) { return std::string(); });
}

void put_utf8(std::string& out, char32_t cp) {
    if (cp < 0x80) {
        out += static_cast<char>(cp);
    } else if (cp < 0x800) {
        out += static_cast<char>(0xC0 | (cp >> 6));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        out += static_cast<char>(0xE0 | (cp >> 12));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else {
        out += static_cast<char>(0xF0 | (cp >> 18));
        out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    }
}

char32_t lower_cp(char32_t c) {
    if (c < 0x80) return (c >= 'A' && c <= 'Z') ? c + 32 : c;
    if ((c >= 0xC0 && c <= 0xDE && c != 0xD7) || (c >= 0x391 && c <= 0x3AB && c != 0x3A2) ||
        (c >= 0x410 && c <= 0x42F))
        return c + 32;                                              // Latin-1, Greek, Cyrillic
    if (c >= 0x400 && c <= 0x40F) return c + 80;                    // Cyrillic Ѐ..Џ
    if (c >= 0x100 && c <= 0x17F && c != 0x130 && c != 0x138 && c != 0x149 && c != 0x17F) {
        // Latin Extended-A: pairs (upper even, lower odd), except the
        // 0x139..0x148 and 0x179..0x17E runs, where the upper case is odd
        const bool odd_run = (c >= 0x139 && c <= 0x148) || (c >= 0x179 && c <= 0x17E);
        if (odd_run ? (c % 2 == 1) : (c % 2 == 0)) return c + 1;
        return c;
    }
    return c;
}

// Lower case as GitHub's slugger (and Python's str.lower) does, for the
// alphabets a heading plausibly uses; other code points are kept.
std::string utf8_lower(const std::string& s) {
    std::string out;
    for (std::size_t i = 0; i < s.size();) {
        const auto b = static_cast<unsigned char>(s[i]);
        std::size_t len = b < 0x80 ? 1 : (b >> 5) == 6 ? 2 : (b >> 4) == 14 ? 3 : (b >> 3) == 30 ? 4 : 1;
        if (i + len > s.size()) len = 1;
        char32_t cp = len == 1 ? b : (len == 2 ? b & 0x1F : len == 3 ? b & 0x0F : b & 0x07);
        bool ok = true;
        for (std::size_t k = 1; k < len; ++k) {
            const auto c = static_cast<unsigned char>(s[i + k]);
            if ((c & 0xC0) != 0x80) ok = false;
            cp = (cp << 6) | (c & 0x3F);
        }
        if (!ok || (len == 1 && b >= 0x80)) {
            out += s[i];  // not UTF-8: kept as is
            ++i;
            continue;
        }
        put_utf8(out, lower_cp(cp));
        i += len;
    }
    return out;
}

std::string strip(std::string_view s) {
    const auto ws = " \t\r\n\f\v";
    const auto a = s.find_first_not_of(ws);
    if (a == std::string_view::npos) return {};
    const auto b = s.find_last_not_of(ws);
    return std::string(s.substr(a, b - a + 1));
}

// Every non-alphanumeric ASCII byte escaped (PCRE2 accepts \ before any of them).
std::string re_escape(std::string_view s) {
    std::string out;
    for (char c : s) {
        const auto u = static_cast<unsigned char>(c);
        if (u < 0x80 && !std::isalnum(u) && c != '_') out += '\\';
        out += c;
    }
    return out;
}

std::vector<std::string> split_lines(const std::string& text) {
    std::vector<std::string> out;
    std::size_t a = 0;
    while (a < text.size()) {
        auto b = text.find('\n', a);
        if (b == std::string::npos) b = text.size();
        std::string line = text.substr(a, b - a);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        out.push_back(std::move(line));
        a = b + 1;
    }
    return out;
}

std::string rel_to(const fs::path& p, const fs::path& base) {
    std::error_code ec;
    auto r = fs::relative(p, base, ec);
    if (ec || r.empty() || r.string().starts_with("..")) return p.generic_string();
    return r.generic_string();
}

const char* const TOP_DIRS[] = {"docs/",    "proofs/",  "tests/",     "tools/",   "src/",    "include/",
                                "prism/",   "scripts/", "testdata/",  "grammars/", ".github/", "third_party/"};
const char* const TOP_FILES[] = {"CMakeLists.txt", "pyproject.toml", "README.md", "CLAUDE.md", "LICENSE",
                                 "NOTICE",         "Dockerfile",     "requirements.txt"};

// last name component -> [(file, declared name)] over proofs/**/*.lean
using LeanIndex = std::map<std::string, std::vector<std::pair<fs::path, std::string>>>;

LeanIndex lean_index(const fs::path& repo) {
    static const Regex decl(R"(^\s*(?:@\[[^\]]*\]\s*)?(?:private\s+|protected\s+)?(?:theorem|lemma)\s+([\w.'!?]+))",
                            true);
    LeanIndex idx;
    std::vector<fs::path> files;
    std::error_code ec;
    const auto root = repo / "proofs";
    if (fs::is_directory(root, ec)) {
        for (auto it = fs::recursive_directory_iterator(root, ec); !ec && it != fs::recursive_directory_iterator();
             it.increment(ec)) {
            if (it->is_directory() && it->path().filename() == ".lake") {
                it.disable_recursion_pending();
                continue;
            }
            if (it->is_regular_file() && it->path().extension() == ".lean") files.push_back(it->path());
        }
    }
    std::sort(files.begin(), files.end());
    for (const auto& f : files) {
        for (const auto& m : decl.finditer(read_text(f))) {
            auto name = m.group(1);
            auto dot = name.rfind('.');
            idx[dot == std::string::npos ? name : name.substr(dot + 1)].emplace_back(f, name);
        }
    }
    return idx;
}

std::optional<std::string> check_theorem(const std::string& ref, const LeanIndex& idx) {
    std::vector<std::string> parts;
    std::size_t a = 0;
    for (;;) {
        auto b = ref.find('.', a);
        parts.push_back(ref.substr(a, b == std::string::npos ? std::string::npos : b - a));
        if (b == std::string::npos) break;
        a = b + 1;
    }
    const auto& last = parts.back();
    auto it = idx.find(last);
    if (it == idx.end() || it->second.empty()) return "thm:" + ref + ": no `theorem " + last + "` in proofs/**/*.lean";
    if (parts.size() == 1) return std::nullopt;
    std::string ns;
    for (std::size_t i = 0; i + 1 < parts.size(); ++i) ns += (i ? "." : "") + parts[i];
    for (const auto& [f, declared] : it->second) {
        if (declared.ends_with(ref)) return std::nullopt;
        const auto text = read_text(f);
        bool all = true;
        for (std::size_t i = 0; i + 1 < parts.size() && all; ++i)
            all = Regex("\\b" + re_escape(parts[i]) + "\\b").search(text);
        if (all) return std::nullopt;
    }
    return "thm:" + ref + ": `" + last + "` exists but not in namespace " + ns;
}

std::optional<std::string> check_path(const std::string& tok, const fs::path& repo) {
    std::string base = tok, member, anchor;
    if (auto p = base.find("::"); p != std::string::npos) {
        member = base.substr(p + 2);
        base = base.substr(0, p);
    }
    if (auto p = base.find('#'); p != std::string::npos) {
        anchor = base.substr(p + 1);
        base = base.substr(0, p);
    }
    std::string trimmed = base;
    while (!trimmed.empty() && trimmed.back() == '/') trimmed.pop_back();
    const auto path = repo / trimmed;
    std::error_code ec;
    if (!fs::exists(path, ec)) return tok + ": path does not exist";
    if (!member.empty()) {
        if (!fs::is_regular_file(path, ec)) return tok + ": " + base + " is not a file";
        if (read_text(path).find(member) == std::string::npos)
            return tok + ": `" + member + "` not found in " + base;
    }
    if (!anchor.empty()) {
        if (path.extension() != ".md") return tok + ": anchors are only checked in Markdown files";
        if (!anchors_of(path).contains(anchor)) return tok + ": no anchor #" + anchor + " in " + base;
    }
    return std::nullopt;
}

std::vector<fs::path> files_under(const fs::path& root, std::string_view ext) {
    std::vector<fs::path> out;
    std::error_code ec;
    if (!fs::is_directory(root, ec)) return out;
    for (auto it = fs::recursive_directory_iterator(root, ec); !ec && it != fs::recursive_directory_iterator();
         it.increment(ec))
        if (it->is_regular_file() && it->path().extension() == ext) out.push_back(it->path());
    std::sort(out.begin(), out.end());
    return out;
}

bool usage(const std::string& msg) {
    std::cerr << msg << "\n";
    return false;
}

}  // namespace

std::string read_text(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    if (!in) return {};
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

fs::path repo_root(const std::string& explicit_root) {
    if (!explicit_root.empty()) return fs::absolute(explicit_root);
    std::error_code ec;
    fs::path cur = fs::current_path(ec);
    for (fs::path p = cur; !p.empty(); p = p.parent_path()) {
        if (fs::exists(p / "tests" / "conformance" / "SOURCES.md", ec))
            return fs::weakly_canonical(p, ec);
        if (fs::exists(p / "include" / "prism" / "laws.hpp", ec) && fs::is_directory(p / "docs", ec))
            return fs::weakly_canonical(p, ec);
        if (p == p.parent_path()) break;
    }
#ifdef PRISM_SOURCE_DIR
    if (const char* root = PRISM_SOURCE_DIR; root && *root) return fs::path(root);
#endif
#ifdef PRISM_QA_SOURCE_DIR
    if (const char* root = PRISM_QA_SOURCE_DIR; root && *root) return fs::path(root);
#endif
    return cur;
}

std::string slug(std::string_view heading) {
    static const Regex link(R"(\[([^\]]*)\]\([^)]*\))");
    static const Regex punct(R"([^\w\- ])");
    std::string text = re_sub(link, std::string(heading), [](const Match& m) { return m.group(1); });
    text.erase(std::remove(text.begin(), text.end(), '`'), text.end());
    text = utf8_lower(strip(text));
    text = re_sub(punct, text, [](const Match&) { return std::string(); });
    std::replace(text.begin(), text.end(), ' ', '-');
    return text;
}

std::set<std::string> anchors_of_text(const std::string& text) {
    static const Regex heading(R"(^(#{1,6})\s+(.*?)\s*#*\s*$)", true);
    static const Regex explicit_a(R"re((?i)<a\s+(?:id|name)="([^"]+)")re");
    std::set<std::string> out;
    for (const auto& m : explicit_a.finditer(text)) out.insert(m.group(1));
    std::map<std::string, int> seen;
    for (const auto& m : heading.finditer(strip_fences(text))) {
        auto s = slug(m.group(2));
        const int n = seen[s]++;
        out.insert(n == 0 ? s : s + "-" + std::to_string(n));
    }
    return out;
}

std::set<std::string> anchors_of(const fs::path& md) { return anchors_of_text(read_text(md)); }

bool is_path_ref(std::string_view tok) {
    auto base = tok.substr(0, tok.find("::"));
    base = base.substr(0, base.find('#'));
    if (base.find_first_of(" *<") != std::string_view::npos) return false;
    for (auto* d : TOP_DIRS)
        if (base.starts_with(d)) return true;
    for (auto* f : TOP_FILES)
        if (base == f) return true;
    return false;
}

AssuranceResult assurance_scan(const fs::path& docs, const fs::path& repo) {
    static const Regex tick("`([^`\n]+)`");
    AssuranceResult res;
    const auto idx = lean_index(repo);
    std::vector<fs::path> mds;
    std::error_code ec;
    for (auto& e : fs::directory_iterator(docs, ec))
        if (e.is_regular_file() && e.path().extension() == ".md") mds.push_back(e.path());
    std::sort(mds.begin(), mds.end());
    for (const auto& md : mds) {
        ++res.counts.files;
        int n = 0;
        for (const auto& line : split_lines(read_text(md))) {
            ++n;
            for (const auto& m : tick.finditer(line)) {
                const auto tok = strip(m.group(1));
                if (tok.find('<') != std::string::npos) continue;  // a placeholder such as `thm:<Name>`
                std::optional<std::string> err;
                if (tok.starts_with("thm:")) {
                    ++res.counts.theorems;
                    err = check_theorem(tok.substr(4), idx);
                } else if (is_path_ref(tok)) {
                    ++res.counts.paths;
                    err = check_path(tok, repo);
                }
                if (err) res.errors.push_back(rel_to(md, repo) + ":" + std::to_string(n) + ": " + *err);
            }
        }
    }
    return res;
}

int assurance_main(const std::vector<std::string>& args, const fs::path& repo_in) {
    fs::path repo = repo_in;
    fs::path docs;
    for (std::size_t i = 0; i < args.size(); ++i) {
        if (args[i] == "--docs" && i + 1 < args.size()) docs = args[++i];
        else if (args[i] == "--repo" && i + 1 < args.size()) repo = fs::absolute(args[++i]);
        else if (args[i] == "-h" || args[i] == "--help") {
            std::cout << "assurance-check [--docs DIR] [--repo DIR]\n"
                         "Every artefact docs/assurance/*.md cites (backticked paths, path::name,\n"
                         "docs/<doc>.md#<anchor>, thm:Name) exists. Exit 1 with one line per missing one.\n";
            return 0;
        } else {
            usage("assurance-check: unknown argument " + args[i]);
            return 2;
        }
    }
    if (docs.empty()) docs = repo / "docs" / "assurance";
    std::error_code ec;
    if (!fs::is_directory(docs, ec)) {
        std::cerr << "assurance_check: " << docs.string() << " does not exist\n";
        return 2;
    }
    auto r = assurance_scan(docs, repo);
    for (const auto& e : r.errors) std::cout << e << "\n";
    std::cout << "assurance_check: " << r.counts.files << " documents, " << r.counts.paths << " artefact paths, "
              << r.counts.theorems << " theorem references, " << r.errors.size() << " missing\n";
    return r.errors.empty() ? 0 : 1;
}

std::vector<std::string> anchor_errors(const fs::path& repo) {
    static const Regex link(R"(\]\(([^)\s]*?)#([^)\s]+)\))");
    // anchor references written by the report writers (report.md / SARIF helpUri)
    static const Regex code_ref(R"(([A-Z_]+\.md)#([a-z0-9-]+))");
    struct Ref {
        fs::path src, tgt;
        std::string anchor;
    };
    std::vector<Ref> refs;
    auto mds = files_under(repo / "docs", ".md");
    std::error_code ec;
    if (fs::exists(repo / "README.md", ec)) mds.push_back(repo / "README.md");
    for (const auto& md : mds) {
        for (const auto& m : link.finditer(strip_fences(read_text(md)))) {
            const auto target = m.group(1);
            if (target.starts_with("http://") || target.starts_with("https://") || target.starts_with("mailto:"))
                continue;
            const fs::path tgt = target.empty() ? md : fs::weakly_canonical(md.parent_path() / target, ec);
            refs.push_back({md, tgt, m.group(2)});
        }
    }
    std::vector<fs::path> code = files_under(repo / "src", ".cpp");
    for (auto& h : files_under(repo / "include", ".hpp")) code.push_back(h);
    for (const auto& src : code)
        for (const auto& m : code_ref.finditer(read_text(src)))
            refs.push_back({src, repo / "docs" / m.group(1), m.group(2)});
    std::map<fs::path, std::set<std::string>> cache;
    std::vector<std::string> bad;
    for (const auto& r : refs) {
        if (!fs::exists(r.tgt, ec)) {
            bad.push_back(rel_to(r.src, repo) + ": " + r.tgt.string() + " does not exist");
            continue;
        }
        if (r.tgt.extension() != ".md") continue;
        auto it = cache.find(r.tgt);
        if (it == cache.end()) it = cache.emplace(r.tgt, anchors_of(r.tgt)).first;
        if (!it->second.contains(r.anchor))
            bad.push_back(rel_to(r.src, repo) + ": " + rel_to(r.tgt, repo) + "#" + r.anchor);
    }
    return bad;
}

int anchors_main(const std::vector<std::string>& args, const fs::path& repo_in) {
    fs::path repo = repo_in;
    for (std::size_t i = 0; i < args.size(); ++i) {
        if (args[i] == "--repo" && i + 1 < args.size()) repo = fs::absolute(args[++i]);
        else if (args[i] == "-h" || args[i] == "--help") {
            std::cout << "anchors [--repo DIR]\n"
                         "Every Markdown anchor link in docs/, README.md and the report writers\n"
                         "(src/**/*.cpp, include/**/*.hpp) points at a heading or <a id> that exists.\n";
            return 0;
        } else {
            usage("anchors: unknown argument " + args[i]);
            return 2;
        }
    }
    auto bad = anchor_errors(repo);
    for (const auto& b : bad) std::cout << b << "\n";
    std::cout << "anchors: " << bad.size() << " broken\n";
    return bad.empty() ? 0 : 1;
}

std::set<std::string> flags_of(std::string help_text) {
    // wrapped help text: "--allow-" / "exec" joined back
    static const Regex wrapped(R"((--[\w-]*-)\n\s+)");
    static const Regex long_flag(R"((?<![\w-])(--[A-Za-z][\w-]*))");
    static const Regex short_flag(R"((?:^\s*|\[|,\s)(-[A-Za-z])\b)", true);
    help_text = re_sub(wrapped, help_text, [](const Match& m) { return m.group(1); });
    std::set<std::string> out;
    for (const auto& m : long_flag.finditer(help_text)) out.insert(m.group(1));
    for (const auto& m : short_flag.finditer(help_text)) out.insert(m.group(1));
    return out;
}

bool documented(const std::string& flag, const std::string& guide) {
    return Regex("`[^`\n]*(?<![\\w-])" + re_escape(flag) + "(?![\\w-])[^`\n]*`").search(guide);
}

std::vector<std::string> stage_order_listed(const std::string& guide) {
    static const Regex sentence(R"(The stages run in a fixed order \(`--list-stages`\):(.*?)\. What)", false, true);
    auto m = sentence.search_match(guide);
    if (!m) return {};
    // whitespace runs (line breaks included) to one blank, then split on commas
    std::string flat;
    for (char c : m->group(1)) {
        const bool ws = c == ' ' || c == '\n' || c == '\t' || c == '\r';
        if (ws) {
            if (!flat.empty() && flat.back() != ' ') flat += ' ';
        } else {
            flat += c;
        }
    }
    std::vector<std::string> out;
    std::size_t a = 0;
    for (;;) {
        auto b = flat.find(',', a);
        out.push_back(strip(flat.substr(a, b == std::string::npos ? std::string::npos : b - a)));
        if (b == std::string::npos) break;
        a = b + 1;
    }
    return out;
}

}  // namespace prism::qa
