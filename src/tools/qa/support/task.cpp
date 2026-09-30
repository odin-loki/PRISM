#include "task.hpp"

#include "yaml_subset.hpp"

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <sstream>

#ifndef PRISM_QA_SOURCE_DIR
#  define PRISM_QA_SOURCE_DIR ""
#endif

namespace prism::qa {

const std::set<std::string> PROOF = {"PROVED", "PROVED-UNBOUNDED", "PROVED-ASSUMING", "PROVED-CERTIFIED"};
const std::vector<std::string> BASE_STAGES = {"inventory", "classify", "bmc", "harness"};
const std::vector<std::string> VERDICT_STAGES = {"bmc", "harness", "pir", "conc"};
const std::set<std::string> SCOPED_STAGES = {"harness"};
const std::map<std::string, std::set<std::string>> STAGE_TASK_ORIGINS = {{"conc", {"concurrency"}}};
const std::map<std::string, std::set<std::string>> ORIGIN_STAGES = {{"concurrency", {"conc"}}};
const std::vector<std::string> DEFAULT_ROOTS = {"prism", "sv-comp", "concurrency", "esbmc-cpp", "libc-models"};
const std::set<std::string> PROPERTY_SCOPED = {"sv-comp", "concurrency", "esbmc-cpp"};
const std::string CERT_STAGE = "pir-certified";
const std::vector<std::string> KEEP_EXTRA = {
    "loops",           "properties",           "certificate",       "certificate_vcs",
    "certify_note",    "certified_mode",       "solver",            "certificate_bitblast",
    "certificate_scope", "certificate_combined", "k_induction"};
const std::vector<std::pair<std::string, std::string>> SV_PROPERTIES = {
    {"no-overflow.prp", "no-overflow"}, {"valid-memsafety.prp", "memsafety"}};

// ESBMC's default property set: assertions, bounds, pointer safety, division
// by zero (signed overflow, shifts and uninitialised reads are not checked
// without extra options, so a PRISM FAILED of those classes on a SUCCESSFUL
// task is "other property"). An uncaught exception / violated noexcept
// aborts the program and ESBMC reports it.
const std::map<std::string, std::set<std::string>> PROPERTY_CLASSES = {
    {"no-overflow", {"INT-SIGNED-OVF"}},
    {"no-div0", {"INT-DIV-ZERO"}},
    {"no-shift-ub", {"INT-SHIFT-UB"}},
    {"no-oob", {"MEM-OOB-READ", "MEM-OOB-WRITE", "MEM-PTR-ARITH"}},
    {"no-null-deref", {"MEM-NULL-DEREF", "NULL-DEREF", "PTR-NULL-DEREF"}},
    // pir memory model classes (docs/PIR.md "Memory model")
    {"memsafety",
     {"MEM-OOB-READ", "MEM-OOB-WRITE", "MEM-NULL-DEREF", "NULL-DEREF", "MEM-USE-AFTER-FREE", "PTR-NULL-DEREF",
      "PTR-INVALID-DEREF", "MEM-UAF", "MEM-DOUBLE-FREE", "MEM-INVALID-FREE", "MEM-MISMATCHED-FREE", "MEM-PTR-ARITH",
      "MEM-OVERLAP", "MEM-VLA-SIZE", "MEM-STACK-ESCAPE", "MEM-MISALIGNED", "MEM-WRITE-CONST", "PTR-COMPARE",
      "UNINIT-READ"}},
    // roadmap 2.6: floating point to integer conversion out of range, and
    // std::terminate reached by an exception (noexcept boundary, main)
    {"no-fp-cast", {"FLOAT-CAST-OVF"}},
    {"no-uncaught", {"CXX-UNCAUGHT", "CXX-THROW-NOEXCEPT", "CXX-TERMINATE"}},
    // concurrency tasks (whole programs; the verdict applies to main)
    {"norace", {"CONC-DATA-RACE"}},
    {"noassert", {"FUNC-CONTRACT"}},
    {"nodeadlock", {"CONC-DEADLOCK"}},
    {"esbmc-cpp",
     {"FUNC-CONTRACT", "INT-DIV-ZERO", "CXX-UNREACHABLE", "CXX-THROW-NOEXCEPT", "CXX-OPTIONAL-NULL",
      "CXX-VECTOR-INDEX", "CXX-ARRAY-INDEX", "CXX-DEQUE-INDEX", "CXX-BITSET-INDEX", "MEM-OOB-READ", "MEM-OOB-WRITE",
      "MEM-NULL-DEREF", "NULL-DEREF", "MEM-USE-AFTER-FREE", "PTR-NULL-DEREF", "PTR-INVALID-DEREF", "MEM-UAF",
      "MEM-DOUBLE-FREE", "MEM-INVALID-FREE", "MEM-MISMATCHED-FREE", "MEM-PTR-ARITH", "MEM-STACK-ESCAPE"}},
};

fs::path repo_root() {
    std::error_code ec;
    fs::path cur = fs::current_path(ec);
    for (fs::path p = cur; !p.empty(); p = p.parent_path()) {
        if (fs::exists(p / "tests" / "conformance" / "SOURCES.md", ec)) return fs::weakly_canonical(p, ec);
        if (p == p.parent_path()) break;
    }
    fs::path built = PRISM_QA_SOURCE_DIR;
    if (!built.empty()) return fs::weakly_canonical(built, ec);
    return cur;
}

fs::path suite_root() { return repo_root() / "tests" / "conformance"; }

bool Task::has(const std::string& fn) const {
    for (const auto& [k, v] : expected)
        if (k == fn) return true;
    return false;
}

bool Task::expected_of(const std::string& fn) const {
    for (const auto& [k, v] : expected)
        if (k == fn) return v;
    return false;
}

std::string read_text(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    if (!in) return {};
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

namespace {
bool cont(unsigned char c) { return (c & 0xC0) == 0x80; }
}  // namespace

std::string py_head(const std::string& s, std::size_t n) {
    std::size_t cps = 0;
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (cont(static_cast<unsigned char>(s[i]))) continue;
        if (cps == n) return s.substr(0, i);
        ++cps;
    }
    return s;
}

std::string py_tail(const std::string& s, std::size_t n) {
    if (n == 0) return {};
    std::size_t cps = 0;
    for (std::size_t i = s.size(); i-- > 0;) {
        if (cont(static_cast<unsigned char>(s[i]))) continue;
        if (++cps == n) return s.substr(i);
    }
    return s;
}

std::string i128_str(i128 v) {
    if (v == 0) return "0";
    bool neg = v < 0;
    unsigned __int128 u = neg ? static_cast<unsigned __int128>(-(v + 1)) + 1 : static_cast<unsigned __int128>(v);
    std::string s;
    while (u) {
        s.insert(s.begin(), static_cast<char>('0' + static_cast<int>(u % 10)));
        u /= 10;
    }
    return neg ? "-" + s : s;
}

namespace {

std::string strip_quotes(std::string s) {
    auto q = [](char c) { return c == '\'' || c == '"'; };
    std::size_t a = 0, b = s.size();
    while (a < b && q(s[a])) ++a;
    while (b > a && q(s[b - 1])) --b;
    return s.substr(a, b - a);
}

std::string node_str(const yaml::Node* n, const std::string& dflt) {
    if (!n) return dflt;
    return n->py_str();
}

// Python `data.get(key) or {}` / `or []`: absent, null, empty and falsy scalars read as empty
bool falsy(const yaml::Node* n) {
    if (!n || n->is_null()) return true;
    if (n->is_map()) return n->map.empty();
    if (n->is_seq()) return n->seq.empty();
    if (auto b = n->as_bool()) return !*b;
    if (auto i = n->as_int()) return *i == 0;
    return n->text.empty();
}

i128 node_int(const yaml::Node& n, const fs::path& yml) {
    if (auto i = n.as_int()) return *i;
    // int("...") of a quoted decimal
    const std::string& t = n.text;
    std::size_t k = 0;
    bool neg = false;
    std::string s = t;
    while (!s.empty() && (s.front() == ' ')) s.erase(s.begin());
    while (!s.empty() && (s.back() == ' ')) s.pop_back();
    if (!s.empty() && (s[0] == '-' || s[0] == '+')) {
        neg = s[0] == '-';
        k = 1;
    }
    if (k >= s.size()) throw yaml::Error(yml.string() + ": not an integer: " + t);
    i128 v = 0;
    for (; k < s.size(); ++k) {
        if (s[k] == '_') continue;
        if (s[k] < '0' || s[k] > '9') throw yaml::Error(yml.string() + ": not an integer: " + t);
        v = v * 10 + (s[k] - '0');
    }
    return neg ? -v : v;
}

std::vector<std::string> split(const std::string& s, char sep) {
    std::vector<std::string> out;
    std::size_t a = 0;
    for (;;) {
        std::size_t b = s.find(sep, a);
        out.push_back(s.substr(a, b == std::string::npos ? std::string::npos : b - a));
        if (b == std::string::npos) break;
        a = b + 1;
    }
    return out;
}

}  // namespace

std::optional<Task> load_task(const fs::path& yml, const fs::path& root) {
    yaml::Node data = yaml::load_file(yml);
    if (!data.is_map() || !data.has("input_files")) return std::nullopt;
    const yaml::Node* inp = data.get("input_files");
    if (inp->is_seq()) {
        if (inp->seq.empty()) throw yaml::Error(yml.string() + ": empty input_files");
        inp = &inp->seq.front();
    }
    Task t;
    t.yml = yml;
    t.source = yml.parent_path() / strip_quotes(inp->py_str());
    fs::path rel = yml.lexically_relative(root);
    std::vector<std::string> parts;
    for (const auto& p : rel) parts.push_back(p.string());
    t.ident = rel.generic_string();
    t.origin = parts.size() > 1 ? parts[0] : "prism";
    t.category = parts.size() > 2 ? parts[1] : t.origin;
    if (data.has("expected")) {
        const yaml::Node* e = data.get("expected");
        if (!falsy(e)) {
            if (!e->is_map()) throw yaml::Error(yml.string() + ": `expected` is not a mapping");
            for (const auto& [k, v] : e->map) t.expected.emplace_back(k, v.truthy_label());
        }
        t.lang = node_str(data.get("language"), "C");
        t.prop = node_str(data.get("property"), "");
        if (const auto* w = data.get("witness"); !falsy(w)) {
            if (!w->is_map()) throw yaml::Error(yml.string() + ": `witness` is not a mapping");
            for (const auto& [k, v] : w->map) {
                if (!v.is_seq()) throw yaml::Error(yml.string() + ": witness " + k + " is not a list");
                std::vector<i128> row;
                for (const auto& x : v.seq) row.push_back(node_int(x, yml));
                t.witness[k] = row;
            }
        }
        if (const auto* s = data.get("expect_status"); !falsy(s)) {
            if (!s->is_map()) throw yaml::Error(yml.string() + ": `expect_status` is not a mapping");
            for (const auto& [k, v] : s->map) t.expect_status[k] = v.py_str();
        }
        if (const auto* b = data.get("sanitizer_blind"); !falsy(b)) {
            if (!b->is_seq()) throw yaml::Error(yml.string() + ": `sanitizer_blind` is not a list");
            for (const auto& x : b->seq) t.sanitizer_blind.insert(x.py_str());
        }
        t.std = node_str(data.get("std"), "");
        if (const auto* d = data.get("deterministic")) t.deterministic = d->truthy_label();
        if (const auto* u = data.get("unwind"); !falsy(u)) t.unwind = static_cast<int>(node_int(*u, yml));
        if (const auto* to = data.get("timeout"); !falsy(to)) t.timeout = std::strtod(to->text.c_str(), nullptr);
        if (const auto* c = data.get("expect_class"); !falsy(c)) {
            if (!c->is_map()) throw yaml::Error(yml.string() + ": `expect_class` is not a mapping");
            for (const auto& [k, v] : c->map) {
                std::set<std::string> cls;
                for (auto& x : split(v.py_str(), '|')) cls.insert(x);
                t.expect_class[k] = cls;
            }
        }
        return t;
    }
    // SV-COMP task-definition format 2.0; a task with verdicts for several
    // suite properties is scored for the first in SV_PROPERTIES order
    // (no-overflow, the property the sv-comp subset is pinned for).
    const yaml::Node* props = data.get("properties");
    if (falsy(props)) return std::nullopt;
    if (!props->is_seq()) throw yaml::Error(yml.string() + ": `properties` is not a list");
    auto rank = [](const yaml::Node& p) {
        const yaml::Node* f = p.get("property_file");
        std::string name = fs::path(f ? f->py_str() : "").filename().string();
        for (std::size_t i = 0; i < SV_PROPERTIES.size(); ++i)
            if (SV_PROPERTIES[i].first == name) return i;
        return SV_PROPERTIES.size();
    };
    std::vector<const yaml::Node*> sorted;
    for (const auto& p : props->seq) sorted.push_back(&p);
    std::stable_sort(sorted.begin(), sorted.end(),
                     [&](const yaml::Node* a, const yaml::Node* b) { return rank(*a) < rank(*b); });
    for (const yaml::Node* p : sorted) {
        const yaml::Node* f = p->get("property_file");
        std::string pf = fs::path(f ? f->py_str() : "").filename().string();
        auto it = std::find_if(SV_PROPERTIES.begin(), SV_PROPERTIES.end(),
                               [&](const auto& kv) { return kv.first == pf; });
        if (it == SV_PROPERTIES.end() || !p->has("expected_verdict")) continue;
        const yaml::Node* opts = data.get("options");
        const yaml::Node* lang = opts && opts->is_map() ? opts->get("language") : nullptr;
        const yaml::Node* dm = opts && opts->is_map() ? opts->get("data_model") : nullptr;
        t.lang = node_str(lang, "C");
        t.prop = it->second;
        t.expected = {{"main", p->get("expected_verdict")->truthy_label()}};
        t.data_model = node_str(dm, "");
        return t;
    }
    return std::nullopt;
}

std::vector<fs::path> rglob_yml(const fs::path& root) {
    std::vector<fs::path> out;
    std::error_code ec;
    if (!fs::is_directory(root, ec)) return out;
    for (auto it = fs::recursive_directory_iterator(root, ec); !ec && it != fs::recursive_directory_iterator();
         it.increment(ec)) {
        if (it->path().extension() == ".yml" && !fs::is_directory(it->path(), ec)) out.push_back(it->path());
    }
    // Path ordering: component by component
    auto parts = [](const fs::path& p) {
        std::vector<std::string> v;
        for (const auto& c : p) v.push_back(c.string());
        return v;
    };
    std::sort(out.begin(), out.end(), [&](const fs::path& a, const fs::path& b) { return parts(a) < parts(b); });
    return out;
}

std::vector<Task> discover(const std::vector<fs::path>& roots) {
    std::vector<Task> tasks;
    const fs::path suite = suite_root();
    for (const auto& root : roots) {
        fs::path base = root.parent_path();
        auto rel = root.lexically_relative(suite);
        bool under = !rel.empty() && *rel.begin() != "..";
        if (under) base = suite;
        for (const auto& yml : rglob_yml(root)) {
            auto t = load_task(yml, base);
            std::error_code ec;
            if (t && fs::exists(t->source, ec)) tasks.push_back(std::move(*t));
        }
    }
    return tasks;
}

}  // namespace prism::qa
