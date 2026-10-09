// MANIFEST.toml: validation, component lookup, linked-tree digests and
// version markers.
#include "prism/deps.hpp"

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <regex>
#include <sstream>

namespace prism::deps {

namespace {

bool is_lower_hex(std::string_view s, std::size_t n) {
    if (s.size() != n) return false;
    return std::all_of(s.begin(), s.end(),
                       [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); });
}

std::string slurp(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    if (!in) throw FetchError("cannot read " + p.string());
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

// Python repr() of a str (or None), as the reference messages print it.
std::string py_repr(const std::optional<std::string>& s) {
    if (!s) return "None";
    bool dq = s->find('\'') != std::string::npos && s->find('"') == std::string::npos;
    char q = dq ? '"' : '\'';
    std::string out(1, q);
    for (char c : *s) {
        if (c == q || c == '\\') out.push_back('\\');
        if (c == '\n') {
            out += "\\n";
            continue;
        }
        out.push_back(c);
    }
    out.push_back(q);
    return out;
}

std::string drift_key(const std::string& s) { return s.size() >= 2 ? s.substr(2) : std::string(); }

}  // namespace

Manifest read_manifest(const fs::path& path) {
    Manifest m;
    m.path = path;
    std::error_code ec;
    auto abs = fs::weakly_canonical(fs::absolute(path, ec), ec);
    if (ec) abs = path;
    m.root = abs.parent_path().parent_path();
    m.bytes = slurp(path);
    m.doc = parse_toml(m.bytes);
    return m;
}

Manifest load_manifest(const fs::path& path) {
    Manifest m = read_manifest(path);
    std::vector<std::string> seen;
    for (const auto& c : m.components()) {
        validate_component(c);
        auto name = c.str("name");
        if (std::find(seen.begin(), seen.end(), name) != seen.end())
            throw FetchError("manifest: duplicate component " + py_repr(name));
        seen.push_back(name);
    }
    return m;
}

void validate_component(const Table& c) {
    const Value* nv = c.find("name");
    if (!nv || !std::holds_alternative<std::string>(*nv) || std::get<std::string>(*nv).empty())
        throw FetchError("manifest: component without a name");
    const std::string name = std::get<std::string>(*nv);
    const Value* kv = c.find("kind");
    std::optional<std::string> kind;
    if (kv && std::holds_alternative<std::string>(*kv)) kind = std::get<std::string>(*kv);
    if (!kind || (*kind != "linked" && *kind != "external" && *kind != "system"))
        throw FetchError("manifest: " + name +
                         ": kind must be one of ('linked', 'external', 'system'), got " +
                         (kv && !kind ? std::string("a non-string") : py_repr(kind)));
    for (const char* key : {"url", "spdx", "version"})
        if (!c.truthy(key)) throw FetchError("manifest: " + name + ": missing " + key);
    if (*kind == "system") return;
    if (!is_lower_hex(c.str("commit"), 40))
        throw FetchError("manifest: " + name + ": commit must be 40 lowercase hex");
    if (!is_lower_hex(c.str("archive_sha256"), 64))
        throw FetchError("manifest: " + name + ": archive_sha256 must be 64 lowercase hex");
    if (*kind == "linked" && !c.truthy("path"))
        throw FetchError("manifest: " + name + ": linked component needs path");
}

std::vector<const Table*> components(const Manifest& m, std::string_view kind) {
    std::vector<const Table*> out;
    for (const auto& c : m.components())
        if (kind.empty() || c.str("kind") == kind) out.push_back(&c);
    return out;
}

const Table& find_component(const Manifest& m, std::string_view name) {
    for (const auto& c : m.components()) {
        if (c.str("name") == name) return c;
        auto st = c.list("stages");
        if (std::find(st.begin(), st.end(), name) != st.end()) return c;
    }
    throw FetchError(py_repr(std::string(name)) + " is not in third_party/MANIFEST.toml");
}

std::optional<fs::path> find_manifest(const fs::path& start) {
    std::error_code ec;
    fs::path dir = fs::absolute(start, ec);
    if (ec) return std::nullopt;
    while (true) {
        auto cand = dir / "third_party" / "MANIFEST.toml";
        if (fs::is_regular_file(cand, ec)) return cand;
        if (!dir.has_parent_path() || dir.parent_path() == dir) return std::nullopt;
        dir = dir.parent_path();
    }
}

fs::path tools_dir() {
    fs::path home;
#ifdef _WIN32
    if (const char* u = std::getenv("USERPROFILE")) home = u;
#else
    if (const char* u = std::getenv("HOME")) home = u;
#endif
    if (const char* env = std::getenv("PRISM_TOOLS_DIR"); env && *env) {
        std::string v(env);
        if (v == "~") return home;
        if (v.rfind("~/", 0) == 0) return home / v.substr(2);
        return fs::path(v);
    }
    return home / ".prism" / "tools";
}

fs::path install_dir(const Table& comp, const fs::path& base) {
    return (base.empty() ? tools_dir() : base) / comp.str("name") / comp.str("commit");
}

// ------------------------------------------------------------ tree digest

namespace {
bool ignored_file(const std::string& name) {
    auto ends = [&](std::string_view suf) {
        return name.size() >= suf.size() && name.compare(name.size() - suf.size(), suf.size(), suf) == 0;
    };
    return ends(".gguf") || ends(".pyc");
}

// os.walk(followlinks=False): a symlink to a directory is neither walked
// nor listed as a file; any other entry (including a dangling link) is.
void walk(const fs::path& dir, std::vector<fs::path>& out) {
    std::error_code ec;
    for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
        const auto& e = *it;
        std::error_code sec;
        bool is_dir = fs::is_directory(e.path(), sec);
        if (is_dir) {
            if (fs::is_symlink(e.symlink_status(sec))) continue;
            auto n = e.path().filename().string();
            if (n == "__pycache__" || n == ".git") continue;
            walk(e.path(), out);
            continue;
        }
        if (ignored_file(e.path().filename().string())) continue;
        out.push_back(e.path());
    }
    if (ec) throw FetchError("cannot list " + dir.string() + ": " + ec.message());
}

std::string rel_posix(const fs::path& p, const fs::path& base) {
    return p.lexically_relative(base).generic_string();
}
}  // namespace

std::vector<fs::path> tree_files(const fs::path& root) {
    std::vector<fs::path> out;
    walk(root, out);
    std::vector<std::pair<std::string, fs::path>> keyed;
    keyed.reserve(out.size());
    for (auto& p : out) keyed.emplace_back(rel_posix(p, root), p);
    std::sort(keyed.begin(), keyed.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    out.clear();
    for (auto& [_, p] : keyed) out.push_back(p);
    return out;
}

// Per-file hash for tree_sha256: LF-normalized so a CRLF working tree still
// matches the manifest (pinned from git archives on LF).
static std::string sha256_file_tree(const fs::path& p) {
    auto data = slurp(p);
    // Text files only: autocrlf must not change the pin; leave binaries byte-exact.
    if (data.find('\0') == std::string::npos) std::erase(data, '\r');
    return sha256_hex(data);
}

std::string tree_digest(const fs::path& root) {
    Sha256 h;
    for (const auto& p : tree_files(root)) {
        h.update(rel_posix(p, root));
        h.update(std::string_view("\0", 1));
        h.update(sha256_file_tree(p));
        h.update("\n");
    }
    return h.hex();
}

// ------------------------------------------------------------ version marker

std::string py_regex_to_ecma(std::string_view py) {
    std::string out;
    for (std::size_t i = 0; i < py.size(); ++i) {
        char c = py[i];
        if (c == '\\') {
            if (i + 1 >= py.size()) throw FetchError("regex ends in a backslash");
            char e = py[i + 1];
            if (e == 'A' || e == 'Z' || e == 'g' || (e >= '1' && e <= '9'))
                throw FetchError(std::string("regex escape \\") + e + " is not supported");
            out.push_back(c);
            out.push_back(e);
            ++i;
            continue;
        }
        if (c == '[') {
            // Copy the class verbatim; '.' is literal inside it in both dialects.
            std::size_t j = i + 1;
            if (j < py.size() && py[j] == '^') ++j;
            if (j < py.size() && py[j] == ']') ++j;
            for (; j < py.size() && py[j] != ']'; ++j)
                if (py[j] == '\\') ++j;
            if (j >= py.size()) throw FetchError("regex: unterminated character class");
            out.append(py.substr(i, j - i + 1));
            i = j;
            continue;
        }
        if (c == '(' && i + 1 < py.size() && py[i + 1] == '?') {
            char k = i + 2 < py.size() ? py[i + 2] : '\0';
            if (k != ':' && k != '=' && k != '!')
                throw FetchError("regex group (?" + std::string(1, k) + " is not supported");
        }
        if (c == '$') throw FetchError("regex '$' is not supported");
        if (c == '.') {
            out += "[\\s\\S]";  // re.S: '.' also matches a newline
            continue;
        }
        out.push_back(c);
    }
    return out;
}

std::optional<std::string> tree_version(const Table& comp, const fs::path& root) {
    auto rel = comp.str("tree_version_file");
    auto rx = comp.str("tree_version_regex");
    if (rel.empty() || rx.empty()) return std::nullopt;
    std::string text = slurp(root / comp.str("path") / rel);
    std::regex re;
    try {
        re = std::regex(py_regex_to_ecma(rx), std::regex::ECMAScript);
    } catch (const std::regex_error& e) {
        throw FetchError(comp.str("name") + ": bad tree_version_regex: " + e.what());
    }
    std::smatch m;
    if (!std::regex_search(text, m, re)) return std::string();
    std::string out;
    bool first = true;
    for (std::size_t g = 1; g < m.size(); ++g) {
        if (!m[g].matched) continue;
        if (!first) out.push_back('.');
        out += m[g].str();
        first = false;
    }
    return out;
}

std::vector<std::string> check_linked(const Table& comp, const fs::path& root) {
    std::vector<std::string> errs;
    const auto name = comp.str("name");
    const auto d = root / comp.str("path");
    std::error_code ec;
    if (!fs::is_directory(d, ec)) return {name + ": " + comp.str("path") + " missing"};
    if (comp.has("tree_version")) {
        std::optional<std::string> want = comp.str("tree_version");
        std::optional<std::string> got;
        try {
            got = tree_version(comp, root);
        } catch (const FetchError& e) {
            errs.push_back(e.what());
            return errs;
        }
        if (got != want)
            errs.push_back(name + ": in-tree version " + py_repr(got) + " != manifest " + py_repr(want));
    }
    auto digest = comp.str("tree_sha256");
    if (!digest.empty()) {
        std::string got;
        try {
            got = tree_digest(d);
        } catch (const FetchError& e) {
            errs.push_back(name + ": " + e.what());
            return errs;
        }
        if (got != digest)
            errs.push_back(name + ": tree_sha256 mismatch (in-tree source was modified)\n  expected " +
                           digest + "\n  got      " + got);
    } else {
        errs.push_back(name + ": no tree_sha256 in manifest");
    }
    return errs;
}

void sort_drift(std::vector<std::string>& lines) {
    std::sort(lines.begin(), lines.end(), [](const std::string& a, const std::string& b) {
        auto ka = drift_key(a), kb = drift_key(b);
        if (ka != kb) return ka < kb;
        return a.substr(0, 1) < b.substr(0, 1);
    });
}

std::vector<std::string> diff_against_upstream(const Table& comp, const fs::path& upstream,
                                               const fs::path& root) {
    const auto ours = root / comp.str("path");
    const auto up = comp.truthy("upstream_subdir") ? upstream / comp.str("upstream_subdir") : upstream;
    const auto only = comp.list("upstream_files");
    auto rels = [&](const fs::path& base) {
        std::vector<std::pair<std::string, fs::path>> m;
        for (auto& p : tree_files(base)) {
            auto k = rel_posix(p, base);
            if (!only.empty() && std::find(only.begin(), only.end(), k) == only.end()) continue;
            m.emplace_back(k, p);
        }
        return m;  // already sorted by key
    };
    auto a = rels(ours), b = rels(up);
    auto find = [](const auto& v, const std::string& k) -> const fs::path* {
        auto it = std::lower_bound(v.begin(), v.end(), k,
                                   [](const auto& e, const std::string& key) { return e.first < key; });
        return it != v.end() && it->first == k ? &it->second : nullptr;
    };
    std::vector<std::string> out;
    for (auto& [k, _] : b)
        if (!find(a, k)) out.push_back("D " + k);
    for (auto& [k, _] : a)
        if (!find(b, k)) out.push_back("A " + k);
    for (auto& [k, p] : a)
        if (auto q = find(b, k); q && sha256_file(p) != sha256_file(*q)) out.push_back("M " + k);
    sort_drift(out);
    return out;
}

}  // namespace prism::deps
