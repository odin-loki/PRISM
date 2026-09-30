// Polyglot stage: every language in the tree, not just C/C++.
// Tables and parsers: prism/polyglot.hpp; tests/cpp/test_polyglot.cpp pins them.
//
//   diagnostic            -> FAILED   (strength FINDS; extra.severity/rule/tool)
//   tool ran, silent      -> UNKNOWN  ("no diagnostics (not a proof)")
//   tool said something   -> ERROR    ("output not understood: <tail>") when no
//     no diagnostic parsed   diagnostic parsed and the output is not in PgTool::benign
//   tool crashed/unusable -> ERROR
//   tool timed out        -> TIMEOUT
//   tool missing          -> NOTRUN   (extra.install)
//   tool executes code    -> NOTRUN   without --allow-exec (Law 9; PgTool::executes)
//
// JSON and TOML syntax are checked in process (Tool::native); Python syntax
// by the scanned project's own python3 (compile() only), NOTRUN without it.
//
// Built-in scans read every text file in scope (id_rsa, key.pem, .npmrc,
// Dockerfile, ...); language tools go by extension.

#include "prism/stages.hpp"
#include "prism/laws.hpp"
#include "prism/polyglot.hpp"
#include "prism/threads.hpp"
#include "prism/regex.hpp"
#include "prism/sandbox.hpp"
#include "prism/scope.hpp"
#include "proc.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <future>
#include <map>
#include <set>
#include <string>
#include <system_error>
#include <vector>

namespace prism {
namespace fs = std::filesystem;

namespace {

const char* const STAGE = "polyglot";

// Extension -> language (prism/polyglot.py LANG_EXTS).
const std::map<std::string, std::string> kLangExts = {
    {".py", "python"}, {".pyi", "python"},
    {".js", "javascript"}, {".mjs", "javascript"}, {".cjs", "javascript"}, {".jsx", "javascript"},
    {".ts", "typescript"}, {".tsx", "typescript"}, {".mts", "typescript"}, {".cts", "typescript"},
    {".sh", "shell"}, {".bash", "shell"},
    {".go", "go"},
    {".rs", "rust"},
    {".rb", "ruby"},
    {".php", "php"},
    {".pl", "perl"}, {".pm", "perl"},
    {".lua", "lua"},
    {".json", "json"},
    {".toml", "toml"},
    {".yml", "yaml"}, {".yaml", "yaml"},
};

const std::set<std::string> kCFamilyExts = {".c", ".h", ".cc", ".cpp", ".cxx",
                                            ".hpp", ".hh", ".hxx", ".cu", ".i", ".ii"};

const std::set<std::string> kTextOnlyExts = {".env", ".ini", ".cfg", ".conf", ".properties",
                                             ".xml", ".java", ".kt", ".cs", ".swift",
                                             ".scala", ".sql", ".tf", ".gradle"};

// Skipped directories: one list for every stage (prism/scope.hpp).

constexpr std::size_t BATCH = 200;
#ifdef _WIN32
const char* const DEVNULL = "nul";
#else
const char* const DEVNULL = "/dev/null";
#endif

// python-syntax: compile() every file, never run it. -I: isolated mode, so
// neither the environment nor the working directory (maybe the scanned
// tree) is on sys.path. Output is one PRISM-SYNTAX or PRISM-OK line per file.
const char* const PY_COMPILE =
    "import sys\n"
    "for p in sys.argv[1:]:\n"
    "    try:\n"
    "        with open(p, 'rb') as fh:\n"
    "            compile(fh.read(), p, 'exec', dont_inherit=True)\n"
    "    except SyntaxError as ex:\n"
    "        print('PRISM-SYNTAX\\t%s\\t%d\\t%d\\t%s: %s' % (p, ex.lineno or 0, ex.offset or 0,"
    " type(ex).__name__, ex.msg))\n"
    "    except (OSError, ValueError) as ex:\n"
    "        print('PRISM-SYNTAX\\t%s\\t0\\t0\\t%s: %s' % (p, type(ex).__name__, ex))\n"
    "    else:\n"
    "        print('PRISM-OK\\t%s' % p)\n";

using PgTool = polyglot::Tool;
using PgCheck = polyglot::Check;
using polyglot::BuiltinScan;
using polyglot::ALLOW_MARKER;
using polyglot::MAX_FILE_BYTES;
using polyglot::MAX_FILES_PER_TOOL;
using polyglot::SNIFF_BYTES;

const char* const GCC_PATTERN =
    R"(^(?P<file>[^\s:][^:]*?):(?P<line>\d+):(?:(?P<col>\d+):)?\s*(?:(?P<sev>error|warning|note|info|style|fatal)\s*:\s*)?(?P<msg>.+)$)";

}  // namespace

namespace polyglot {

// The syntax rows come first: a file they find broken is kept away from the
// whole-program type checkers (run_polyglot).
const std::vector<Check>& checks() {
    static const std::vector<Check> C = {
        {"python-syntax", {"python"}, "syntax",
         {{"python", {"python3", "python"}, {"{exe}", "-I", "-c", PY_COMPILE, "{files}"},
           R"(^PRISM-SYNTAX\t(?P<file>[^\t]+)\t(?P<line>\d+)\t(?P<col>\d+)\t(?P<msg>.+)$)",
           false, {0}, 120.0, "", "", /*executes=*/false,
           /*benign=*/{R"(PRISM-OK\t.+)"}}},
         "install Python 3 (python3 on PATH)"},
        // Built in: always present, never NOTRUN.
        {"json-syntax", {"json"}, "syntax",
         {{"prism-json", {}, {}, "", false, {0}, 120.0, "", "", /*executes=*/false,
           /*benign=*/{}, /*native=*/json_syntax}},
         "built in"},
        {"toml-syntax", {"toml"}, "syntax",
         {{"prism-toml", {}, {}, "", false, {0}, 120.0, "", "", /*executes=*/false,
           /*benign=*/{}, /*native=*/toml_syntax}},
         "built in"},
        {"python-lint", {"python"}, "lint",
         {{"ruff", {"ruff"},
           {"{exe}", "check", "--output-format=concise", "--no-cache", "--quiet", "{files}"},
           R"(^(?P<file>.+?):(?P<line>\d+):(?P<col>\d+): (?P<rule>[A-Z]+\d+|[a-z][a-z0-9]*(?:-[a-z0-9]+)+|SyntaxError):? (?P<msg>.+)$)",
           false, {0, 1}, 120.0, "", "", /*executes=*/false,
           /*benign=*/{R"(All checks passed!)"}},
          {"pyflakes", {"pyflakes"}, {"{exe}", "{files}"},
           R"(^(?P<file>.+?):(?P<line>\d+):(?:(?P<col>\d+):?)?\s+(?P<msg>.+)$)"}},
         "pip install ruff  (or pyflakes)"},
        {"python-types", {"python"}, "type",
         {{"mypy", {"mypy"},
           {"{exe}", "--ignore-missing-imports", "--no-error-summary", "--show-column-numbers",
            "--no-color-output", "--no-incremental", "--cache-dir={devnull}", "--config-file=",
            "{files}"},
           R"(^(?P<file>.+?):(?P<line>\d+):(?:(?P<col>\d+):)? (?P<sev>error): (?P<msg>.+?)(?:\s+\[(?P<rule>[\w-]+)\])?$)",
           false, {0, 1}, 600.0, "", "", /*executes=*/false,
           /*benign=*/{R"(Success: no issues found in \d+ source files?)"}}},
         "pip install mypy"},
        {"javascript-syntax", {"javascript"}, "syntax",
         {{"node", {"node"}, {"{exe}", "--check", "{file}"},
           R"((?ms)^(?P<file>[^\n]+?):(?P<line>\d+)\n.*?^(?P<msg>(?:SyntaxError|Error)[^\n]*))",
           true, {0}}},
         "install Node.js (node on PATH)"},
        {"typescript-types", {"typescript"}, "type",
         {{"tsc", {"tsc"},
           {"{exe}", "--noEmit", "--pretty", "false", "--skipLibCheck", "--jsx", "preserve",
            "--allowJs", "{files}"},
           R"(^(?P<file>.+?)\((?P<line>\d+),(?P<col>\d+)\): (?P<sev>error|warning) (?P<rule>TS\d+): (?P<msg>.+)$)",
           false, {0, 1, 2}, 600.0}},
         "npm install -g typescript"},
        {"javascript-lint", {"javascript", "typescript"}, "lint",
         {{"eslint", {"eslint"}, {"{exe}", "--format", "unix", "{files}"},
           R"(^(?P<file>.+?):(?P<line>\d+):(?P<col>\d+): (?P<msg>.+?)(?: \[(?P<sev>Error|Warning)/(?P<rule>[^\]]+)\])?$)",
           false, {0, 1}, 120.0, "",
           R"(couldn't find (?:a|an) (?:eslint\.config|configuration file))", /*executes=*/true}},
         "npm install -g eslint  (needs an eslint.config.* in the project)"},
        {"shell-syntax", {"shell"}, "syntax",
         {{"bash", {"bash"}, {"{exe}", "-n", "{file}"},
           R"(^(?P<file>.+?): line (?P<line>\d+): (?P<msg>.+)$)", true, {0}}},
         "install bash"},
        {"shell-lint", {"shell"}, "lint",
         {{"shellcheck", {"shellcheck"}, {"{exe}", "-f", "gcc", "{files}"}, GCC_PATTERN}},
         "apt install shellcheck"},
        {"go-syntax", {"go"}, "syntax",
         {{"gofmt", {"gofmt"}, {"{exe}", "-e", "-l", "{files}"},
           R"(^(?P<file>.+?\.go):(?P<line>\d+):(?P<col>\d+): (?P<msg>.+)$)", false, {0}, 120.0, "",
           "", /*executes=*/false, /*benign=*/{R"(.+\.go)"}}},
         "install Go (gofmt on PATH)"},
        {"rust-lint", {"rust"}, "lint",
         {{"cargo-clippy", {"cargo"}, {"{exe}", "clippy", "--quiet", "--message-format=short"},
           R"(^(?P<file>[^\s:][^:]*?\.rs):(?P<line>\d+):(?P<col>\d+): (?P<sev>error|warning)(?:\[(?P<rule>[^\]]+)\])?: (?P<msg>.+)$)",
           false, {0, 101}, 900.0, "Cargo.toml", "", /*executes=*/true}},
         "install Rust (rustup component add clippy); needs a Cargo.toml"},
        {"ruby-syntax", {"ruby"}, "syntax",
         {{"ruby", {"ruby"}, {"{exe}", "-wc", "{file}"},
           R"(^(?:\S*ruby\S*: )?(?P<file>[^:\n]+?):(?P<line>\d+): (?:(?P<sev>warning): )?(?P<msg>.+)$)",
           true, {0}, 120.0, "", "", /*executes=*/false, /*benign=*/{R"(Syntax OK)"}}},
         "install Ruby"},
        {"php-syntax", {"php"}, "syntax",
         {{"php", {"php"}, {"{exe}", "-l", "{file}"},
           R"(^(?:PHP )?(?P<msg>(?:Parse|Fatal) error:.+?) in (?P<file>.+?) on line (?P<line>\d+)$)",
           true, {0}, 120.0, "", "", /*executes=*/false,
           /*benign=*/{R"(No syntax errors detected in .+)"}}},
         "install PHP CLI"},
        {"perl-syntax", {"perl"}, "syntax",
         {{"perl", {"perl"}, {"{exe}", "-c", "{file}"},
           R"(^(?P<msg>.+?) at (?P<file>.+?) line (?P<line>\d+)[.,])", true, {0}, 120.0, "", "",
           /*executes=*/true, /*benign=*/{R"(.+ syntax OK)"}}},
         "install Perl"},
        {"lua-syntax", {"lua"}, "syntax",
         {{"luac", {"luac", "luac5.4", "luac5.3"}, {"{exe}", "-p", "{file}"},
           R"(^\S*luac\S*: (?P<file>.+?):(?P<line>\d+): (?P<msg>.+)$)", true, {0}}},
         "install Lua (luac on PATH)"},
        {"yaml-lint", {"yaml"}, "lint",
         {{"yamllint", {"yamllint"}, {"{exe}", "-f", "parsable", "{files}"},
           R"(^(?P<file>.+?):(?P<line>\d+):(?P<col>\d+): \[(?P<sev>error|warning)\] (?P<msg>.+?)(?: \((?P<rule>[\w-]+)\))?$)"}},
         "pip install yamllint"},
    };
    return C;
}

}  // namespace polyglot

namespace {

const BuiltinScan kBuiltinScans[] = {
    {"VCS-CONFLICT-MARKER", R"(^(?:<{7}|>{7})(?: |$))", "unresolved merge conflict marker"},
    {"SECRET-PRIVATE-KEY",
     R"(-----BEGIN (?:RSA |EC |DSA |OPENSSH |PGP |ENCRYPTED )?PRIVATE KEY(?: BLOCK)?-----)",
     "private key committed in source"},
    {"SECRET-AWS-KEY", R"(\b(?:AKIA|ASIA)[0-9A-Z]{16}\b)", "AWS access key id in source"},
    {"SECRET-GITHUB-TOKEN", R"(\b(?:gh[pousr]_[A-Za-z0-9]{36,}|github_pat_[A-Za-z0-9_]{50,})\b)",
     "GitHub token in source"},
    {"SECRET-SLACK-TOKEN", R"(\bxox[abprs]-[A-Za-z0-9-]{10,}\b)", "Slack token in source"},
    {"SECRET-GOOGLE-API-KEY", R"(\bAIza[0-9A-Za-z_-]{35}\b)", "Google API key in source"},
    {"SECRET-STRIPE-KEY", R"(\b[sr]k_live_[0-9A-Za-z]{20,}\b)", "Stripe live secret key in source"},
};

std::string lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string trim(std::string s) {
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) s.erase(s.begin());
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.pop_back();
    return s;
}

std::string join(const std::vector<std::string>& v, const std::string& sep) {
    std::string o;
    for (std::size_t i = 0; i < v.size(); ++i) {
        if (i) o += sep;
        o += v[i];
    }
    return o;
}


std::string language_of(const fs::path& p) {
    auto it = kLangExts.find(lower(p.extension().string()));
    return it == kLangExts.end() ? std::string{} : it->second;
}

fs::path base_of(const fs::path& root) {
    std::error_code ec;
    return fs::is_directory(root, ec) ? root : root.parent_path();
}

std::string rel(const fs::path& p, const fs::path& root) {
    std::error_code ec;
    auto a = fs::weakly_canonical(p, ec);
    if (ec) return p.generic_string();
    auto b = fs::weakly_canonical(base_of(root), ec);
    if (ec) return p.generic_string();
    auto r = a.lexically_relative(b);
    if (r.empty() || r.native().starts_with(fs::path("..").native())) return p.generic_string();
    return r.generic_string();
}

Finding pg_finding(std::string_view status, std::string file, std::optional<int> line,
                   std::string cls, std::string message,
                   std::map<std::string, std::string> extra = {}) {
    Finding f;
    f.stage = STAGE;
    f.status = std::string(status);
    f.file = std::move(file);
    f.line = line;
    f.cls = std::move(cls);
    f.message = std::move(message);
    f.strength = std::string(laws::STRENGTH_FINDS);
    for (auto& [k, v] : extra)
        if (!v.empty()) f.extra[k] = v;
    return f;
}

}  // namespace

bool is_known_source(const fs::path& p) {
    auto e = lower(p.extension().string());
    return kLangExts.contains(e) || kCFamilyExts.contains(e) || kTextOnlyExts.contains(e) ||
           p.filename() == ".env";
}

bool is_text_file(const fs::path& p) {
    std::error_code ec;
    auto sz = fs::file_size(p, ec);
    if (ec || sz > MAX_FILE_BYTES) return false;
    std::ifstream in(p, std::ios::binary);
    if (!in) return false;
    std::string head(SNIFF_BYTES, '\0');
    in.read(head.data(), static_cast<std::streamsize>(head.size()));
    head.resize(static_cast<std::size_t>(in.gcount()));
    return head.find('\0') == std::string::npos;
}

namespace {

// Every regular file under root outside the skipped dirs. Sorted,
// depth-first: the same order as os.walk with sorted dirnames/filenames.
std::vector<fs::path> walk_files(const fs::path& root) {
    std::vector<fs::path> out;
    std::error_code ec;
    if (fs::is_regular_file(root, ec)) {
        out.push_back(root);
        return out;
    }
    if (!fs::is_directory(root, ec)) return out;
    std::vector<fs::path> stack{root};
    while (!stack.empty()) {
        auto dir = stack.back();
        stack.pop_back();
        std::vector<fs::path> files, dirs;
        for (auto it = fs::directory_iterator(dir, fs::directory_options::skip_permission_denied, ec);
             !ec && it != fs::directory_iterator(); it.increment(ec)) {
            std::error_code e2;
            if (it->is_directory(e2) && !it->is_symlink(e2)) {
                if (!scope::skip_dir(it->path().filename().string())) dirs.push_back(it->path());
            } else if (it->is_regular_file(e2)) {
                files.push_back(it->path());
            }
        }
        ec.clear();
        std::sort(files.begin(), files.end());
        std::sort(dirs.begin(), dirs.end());
        out.insert(out.end(), files.begin(), files.end());
        for (auto d = dirs.rbegin(); d != dirs.rend(); ++d) stack.push_back(*d);
    }
    return out;
}

}  // namespace

std::vector<fs::path> iter_polyglot_sources(const fs::path& root) {
    std::vector<fs::path> out;
    for (auto& p : walk_files(root))
        if (is_known_source(p)) out.push_back(p);
    return out;
}

std::vector<fs::path> iter_text_files(const fs::path& root) {
    std::vector<fs::path> out;
    for (auto& p : walk_files(root))
        if (is_text_file(p)) out.push_back(p);
    return out;
}

namespace polyglot {

std::span<const BuiltinScan> builtin_scans() { return kBuiltinScans; }
const std::map<std::string, std::string>& lang_exts() { return kLangExts; }
const std::set<std::string>& c_family_exts() { return kCFamilyExts; }
const std::set<std::string>& text_only_exts() { return kTextOnlyExts; }

std::vector<Finding> builtin_scan(const std::vector<fs::path>& files, const fs::path& root) {
    static const std::vector<std::pair<const BuiltinScan*, Regex>> rx = [] {
        std::vector<std::pair<const BuiltinScan*, Regex>> v;
        for (auto& s : kBuiltinScans) v.emplace_back(&s, Regex(s.pattern));
        return v;
    }();
    std::vector<Finding> out;
    int scanned = 0;
    for (auto& p : files) {
        if (!is_text_file(p)) continue;
        std::ifstream in(p, std::ios::binary);
        if (!in) continue;
        ++scanned;
        auto r = rel(p, root);
        std::string line;
        int n = 0;
        while (std::getline(in, line)) {
            ++n;
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.find(ALLOW_MARKER) != std::string::npos) continue;
            for (auto& [scan, re] : rx)
                if (re.search(line))
                    out.push_back(pg_finding(laws::FAILED, r, n, scan->cls, scan->message,
                                             {{"tool", "prism-builtin"}}));
        }
    }
    if (out.empty())
        out.push_back(pg_finding(laws::CLEAN, "", std::nullopt, "",
                                 "prism-builtin: " + std::to_string(scanned) +
                                     " files, no conflict markers or leaked credentials "
                                     "(not a proof)",
                                 {{"tool", "prism-builtin"}}));
    return out;
}

std::optional<fs::path> resolve(const PgTool& tool, const Config& cfg) {
    // --tool NAME=~/bin/x: the shell did not expand the ~ after '='.
    auto expand_user = [](const fs::path& p) -> fs::path {
        auto s = p.generic_string();
        if (s != "~" && !s.starts_with("~/")) return p;
#ifdef _WIN32
        const char* home = std::getenv("USERPROFILE");
#else
        const char* home = std::getenv("HOME");
#endif
        if (!home || !*home) return p;
        return s == "~" ? fs::path(home) : fs::path(home) / s.substr(2);
    };
    auto lookup = [&](const std::string& key) -> std::optional<fs::path> {
        auto it = cfg.tools.find(key);
        if (it == cfg.tools.end()) return std::nullopt;
        auto p = expand_user(it->second);
        std::error_code ec;
        if (fs::is_regular_file(p, ec)) return p;
        return std::nullopt;
    };
    if (tool.native) return std::nullopt;
    if (auto hit = lookup(tool.name)) return hit;
    for (auto& n : tool.exes)
        if (auto hit = lookup(n)) return hit;
    // The pinned ~/.prism/tools build (when the tool has a manifest row), then PATH.
    for (auto& n : tool.exes)
        if (auto hit = cfg.which_adapter(tool.name, {std::string_view(n)})) return hit;
    return std::nullopt;
}

}  // namespace polyglot

namespace {

std::vector<std::string> expand(const PgTool& tool, const std::string& exe,
                                const std::vector<std::string>& files) {
    std::vector<std::string> cmd;
    for (auto& a : tool.argv) {
        if (a == "{exe}") cmd.push_back(exe);
        else if (a == "{files}" || a == "{file}") cmd.insert(cmd.end(), files.begin(), files.end());
        else if (auto k = a.find("{devnull}"); k != std::string::npos)
            cmd.push_back(a.substr(0, k) + DEVNULL + a.substr(k + 9));
        else cmd.push_back(a);
    }
    return cmd;
}

}  // namespace

namespace polyglot {

std::vector<Finding> parse_output(const PgTool& tool, const std::string& text, const fs::path& root,
                                  const fs::path& cwd, const PgCheck& check) {
    Regex rx(tool.pattern, /*multiline=*/true);
    static const Regex fix_mark(R"(^\[\*\]\s*)");
    std::vector<Finding> out;
    for (auto& m : rx.finditer(text)) {
        auto sev = lower(m.named("sev"));
        if (sev == "note" || sev == "info") continue;
        auto file = trim(m.named("file"));
        fs::path fp(file);
        if (!cwd.empty() && !file.empty() && !fp.is_absolute()) fp = cwd / fp;
        auto msg = trim(m.named("msg"));
        if (auto fm = fix_mark.search_match(msg)) msg = msg.substr(fm->text.size());
        std::string cls = check.kind == "syntax" ? "SYNTAX-ERROR"
                          : check.kind == "type" ? "TYPE-ERROR"
                                                 : "LANG-LINT";
        if (sev == "warning" && check.kind == "syntax") cls = "LANG-LINT";
        std::optional<int> line;
        if (auto l = m.named("line"); !l.empty()) line = std::stoi(l);
        out.push_back(pg_finding(laws::FAILED, file.empty() ? std::string{} : rel(fp, root), line,
                                 cls, tool.name + ": " + msg,
                                 {{"tool", tool.name},
                                  {"rule", m.named("rule")},
                                  {"severity", sev},
                                  {"language", join(check.languages, "/")},
                                  {"check", check.group},
                                  {"col", m.named("col")}}));
    }
    return out;
}

// Output lines that are neither diagnostics nor the tool's known-benign
// chatter (prism/polyglot.py unexplained_output). Only asked when no
// diagnostic was kept: non-empty means PRISM did not understand the tool.
// Text the tool pattern matches (e.g. a dropped note) is understood.
std::string unexplained_output(const PgTool& tool, const std::string& raw) {
    std::string text;
    {
        Regex pat(tool.pattern, /*multiline=*/true);
        std::size_t at = 0;
        for (auto& m : pat.finditer(raw)) {
            auto b = static_cast<std::size_t>(m.spans[0].first);
            auto e = static_cast<std::size_t>(m.spans[0].second);
            if (b < at || e < b) continue;
            text += raw.substr(at, b - at);
            at = e;
        }
        text += raw.substr(std::min(at, raw.size()));
    }
    std::vector<Regex> rxs;
    for (auto& b : tool.benign) rxs.emplace_back("^(?:" + b + ")$");
    std::string rest;
    std::size_t pos = 0;
    while (pos < text.size()) {
        auto nl = text.find('\n', pos);
        if (nl == std::string::npos) nl = text.size();
        auto line = trim(text.substr(pos, nl - pos));
        pos = nl + 1;
        if (line.empty()) continue;
        bool ok = false;
        for (auto& r : rxs)
            if (r.search(line)) {
                ok = true;
                break;
            }
        if (ok) continue;
        if (!rest.empty()) rest += "\n";
        rest += line;
    }
    return rest;
}

namespace {

// nlohmann reports "parse error at line L, column C: <what>"; keep L, C and <what>.
struct JsonSax {
    std::string_view text;
    std::optional<SyntaxIssue> issue;
    bool null() { return true; }
    bool boolean(bool) { return true; }
    bool number_integer(nlohmann::json::number_integer_t) { return true; }
    bool number_unsigned(nlohmann::json::number_unsigned_t) { return true; }
    bool number_float(nlohmann::json::number_float_t, const std::string&) { return true; }
    bool string(std::string&) { return true; }
    bool binary(nlohmann::json::binary_t&) { return true; }
    bool start_object(std::size_t) { return true; }
    bool key(std::string&) { return true; }
    bool end_object() { return true; }
    bool start_array(std::size_t) { return true; }
    bool end_array() { return true; }
    bool parse_error(std::size_t pos, const std::string&, const nlohmann::detail::exception& ex) {
        std::string what = ex.what();
        SyntaxIssue out;
        static const Regex at(R"(at line (\d+), column (\d+): (.*)$)");
        if (auto m = at.search_match(what)) {
            out.line = std::stoi(m->group(1));
            out.col = std::stoi(m->group(2));
            what = m->group(3);
        } else {
            // No position in the text (e.g. empty input): count lines up to the
            // bytes read.
            if (auto k = what.find("] "); k != std::string::npos) what = what.substr(k + 2);
            if (auto k = what.find(": "); what.starts_with("parse error") && k != std::string::npos)
                what = what.substr(k + 2);
            out.line = 1;
            out.col = 1;
            for (std::size_t k = 0; k + 1 < pos && k < text.size(); ++k) {
                if (text[k] == '\n') {
                    ++out.line;
                    out.col = 1;
                } else {
                    ++out.col;
                }
            }
        }
        out.msg = "JSON: " + what;
        issue = std::move(out);
        return false;
    }
};

}  // namespace

std::optional<SyntaxIssue> json_syntax(std::string_view text) {
    JsonSax sax;
    sax.text = text;
    // A leading UTF-8 byte order mark is skipped by the parser; strings must be UTF-8.
    nlohmann::json::sax_parse(text.begin(), text.end(), &sax);
    return sax.issue;
}

}  // namespace polyglot

namespace {

struct Call {
    std::vector<std::string> files;
    fs::path cwd;
};

std::vector<Call> invocations(const PgTool& tool, const std::vector<fs::path>& files) {
    std::vector<Call> calls;
    if (!tool.cwd_marker.empty()) {
        std::vector<fs::path> dirs;
        for (auto& f : files) {
            for (auto d = f.parent_path(); !d.empty(); d = d.parent_path()) {
                std::error_code ec;
                if (fs::is_regular_file(d / tool.cwd_marker, ec)) {
                    if (std::find(dirs.begin(), dirs.end(), d) == dirs.end()) dirs.push_back(d);
                    break;
                }
                if (d == d.parent_path()) break;
            }
        }
        for (auto& d : dirs) calls.push_back({{}, d});
        return calls;
    }
    std::vector<std::string> names;
    for (auto& f : files) names.push_back(f.string());
    if (tool.per_file) {
        for (auto& n : names) calls.push_back({{n}, {}});
        return calls;
    }
    for (std::size_t i = 0; i < names.size(); i += BATCH)
        calls.push_back({{names.begin() + static_cast<std::ptrdiff_t>(i),
                          names.begin() + static_cast<std::ptrdiff_t>(std::min(names.size(), i + BATCH))},
                         {}});
    return calls;
}

// A built-in syntax checker over every file (no process, no batch limit).
std::vector<Finding> run_native(const PgCheck& check, const PgTool& tool,
                                const std::vector<fs::path>& files, const fs::path& root) {
    std::vector<Finding> out;
    const auto langs = join(check.languages, "/");
    for (auto& p : files) {
        std::ifstream in(p, std::ios::binary);
        if (!in) {
            out.push_back(pg_finding(laws::ERROR, rel(p, root), std::nullopt, "",
                                     tool.name + ": cannot read the file",
                                     {{"tool", tool.name}, {"check", check.group}}));
            continue;
        }
        std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        auto issue = tool.native(text);
        if (!issue) continue;
        std::optional<int> line;
        if (issue->line > 0) line = issue->line;
        out.push_back(pg_finding(laws::FAILED, rel(p, root), line, "SYNTAX-ERROR",
                                 tool.name + ": " + issue->msg,
                                 {{"tool", tool.name},
                                  {"language", langs},
                                  {"check", check.group},
                                  {"col", issue->col > 0 ? std::to_string(issue->col) : ""}}));
    }
    if (out.empty())
        out.push_back(pg_finding(laws::UNKNOWN, "", std::nullopt, "",
                                 tool.name + ": " + std::to_string(files.size()) + " " + langs +
                                     " file(s), no diagnostics (not a proof)",
                                 {{"tool", tool.name}, {"check", check.group}}));
    return out;
}

std::vector<Finding> run_check(const PgCheck& check, std::vector<fs::path> files,
                               const fs::path& root, const Config& cfg) {
    const PgTool* tool = nullptr;
    const PgTool* held = nullptr;  // present, but executes scanned code (Law 9)
    std::optional<fs::path> exe;
    for (auto& t : check.tools)
        if (t.native) return run_native(check, t, files, root);
    for (auto& t : check.tools) {
        auto found = polyglot::resolve(t, cfg);
        if (!found) continue;
        if (t.executes && !cfg.allow_exec) {
            if (!held) held = &t;
            continue;
        }
        exe = found;
        tool = &t;
        break;
    }
    auto langs = join(check.languages, "/");
    auto nfiles = std::to_string(files.size());
    if (!tool && held)
        return {sandbox::exec_notrun(STAGE, check.group + " (" + held->name + ")",
                                     {{"tool", held->name}, {"check", check.group}})};
    if (!tool) {
        std::vector<std::string> names;
        for (auto& t : check.tools) names.push_back(t.name);
        return {pg_finding(laws::NOTRUN, "", std::nullopt, "",
                           check.group + ": " + join(names, " / ") + " not found for " + nfiles +
                               " " + langs + " file(s)",
                           {{"install", check.install}, {"check", check.group}})};
    }
    std::vector<Finding> note;
    if (files.size() > MAX_FILES_PER_TOOL) {
        note.push_back(pg_finding(laws::UNKNOWN, "", std::nullopt, "",
                                  tool->name + ": checked first " +
                                      std::to_string(MAX_FILES_PER_TOOL) + " of " + nfiles + " " +
                                      langs + " files (rest not checked)",
                                  {{"tool", tool->name}, {"check", check.group}}));
        files.resize(MAX_FILES_PER_TOOL);
        nfiles = std::to_string(files.size());
    }
    auto calls = invocations(*tool, files);
    if (calls.empty())
        return {pg_finding(laws::NOTRUN, "", std::nullopt, "",
                           check.group + ": no " + tool->cwd_marker + " above " + nfiles + " " +
                               langs + " file(s); not checked",
                           {{"install", check.install}, {"check", check.group}})};

    const Regex unconfigured(tool->unconfigured.empty() ? std::string{}
                                                        : "(?i)" + tool->unconfigured);
    auto one = [&](const Call& call) -> std::vector<Finding> {
        auto r = detail::run_process(expand(*tool, exe->string(), call.files),
                                     tool->timeout, call.cwd);
        if (r.timed_out)
            return {pg_finding(laws::TIMEOUT, "", std::nullopt, "",
                               tool->name + " timed out after " +
                                   std::to_string(static_cast<int>(tool->timeout)) + "s",
                               {{"tool", tool->name}, {"check", check.group}})};
        if (!tool->unconfigured.empty() && unconfigured.search(r.text))
            return {pg_finding(laws::NOTRUN, "", std::nullopt, "",
                               tool->name + " present but the project has no config for it",
                               {{"install", check.install}, {"tool", tool->name},
                                {"check", check.group}})};
        auto hits = polyglot::parse_output(*tool, r.text, root, call.cwd, check);
        if (!hits.empty()) return hits;
        bool ok_rc = std::find(tool->ok_rcs.begin(), tool->ok_rcs.end(), r.rc) != tool->ok_rcs.end();
        if (r.failed || r.rc == 127 || (!ok_rc && !tool->per_file)) {
            auto t = trim(r.text);
            if (t.size() > 400) t = t.substr(t.size() - 400);
            return {pg_finding(laws::ERROR, "", std::nullopt, "",
                               tool->name + " exit " + std::to_string(r.rc) + ": " + t,
                               {{"tool", tool->name}, {"check", check.group}})};
        }
        if (!ok_rc) {
            auto t = trim(r.text);
            if (t.size() > 300) t = t.substr(t.size() - 300);
            return {pg_finding(laws::FAILED, call.files.empty() ? std::string{} : rel(call.files[0], root),
                               std::nullopt, check.kind == "syntax" ? "SYNTAX-ERROR" : "LANG-LINT",
                               tool->name + " exit " + std::to_string(r.rc) + ": " + t,
                               {{"tool", tool->name}, {"check", check.group}})};
        }
        if (auto rest = polyglot::unexplained_output(*tool, r.text); !rest.empty()) {
            if (rest.size() > 400) rest = rest.substr(rest.size() - 400);
            return {pg_finding(laws::ERROR, "", std::nullopt, "",
                               tool->name + ": output not understood: " + rest,
                               {{"tool", tool->name}, {"check", check.group}})};
        }
        return {};
    };

    std::vector<Finding> results;
    // --jobs 0 is "half the cores" here as everywhere (threads.hpp).
    std::size_t jobs = static_cast<std::size_t>(clamp_jobs(cfg.jobs));
    for (std::size_t i = 0; i < calls.size(); i += jobs) {
        std::vector<std::future<std::vector<Finding>>> futs;
        for (std::size_t k = i; k < std::min(calls.size(), i + jobs); ++k)
            futs.push_back(std::async(std::launch::async, one, std::cref(calls[k])));
        for (auto& f : futs) {
            auto part = f.get();
            results.insert(results.end(), part.begin(), part.end());
        }
    }
    if (results.empty())
        results.push_back(pg_finding(laws::UNKNOWN, "", std::nullopt, "",
                                     tool->name + ": " + nfiles + " " + langs +
                                         " file(s), no diagnostics (not a proof)",
                                     {{"tool", tool->name}, {"check", check.group}}));
    note.insert(note.end(), results.begin(), results.end());
    return note;
}

}  // namespace

std::vector<Finding> run_polyglot(const fs::path& root, const Config& cfg) {
    auto all_files = walk_files(root);
    if (all_files.empty())
        return {pg_finding(laws::UNKNOWN, "", std::nullopt, "", "no source files in scope")};
    // Built-in scans read every text file (builtin_scan sniffs); language
    // tools only files with their extension.
    auto out = polyglot::builtin_scan(all_files, root);
    std::vector<fs::path> files;
    for (auto& p : all_files)
        if (is_known_source(p)) files.push_back(p);
    std::map<std::string, std::vector<fs::path>> by_lang;
    for (auto& p : files)
        if (auto lang = language_of(p); !lang.empty()) by_lang[lang].push_back(p);
    // A file that does not parse makes whole-program type checkers (mypy, tsc)
    // abort and hide every other file's errors. It already has its
    // SYNTAX-ERROR; keep it away from the type checkers.
    std::set<std::string> broken;
    for (auto& check : polyglot::checks()) {
        std::vector<fs::path> mine;
        for (auto& lang : check.languages)
            if (auto it = by_lang.find(lang); it != by_lang.end())
                for (auto& p : it->second)
                    if (check.kind != "type" || !broken.contains(rel(p, root))) mine.push_back(p);
        if (mine.empty()) continue;
        auto part = run_check(check, mine, root, cfg);
        if (check.kind == "syntax")
            for (auto& f : part)
                if (f.status == laws::FAILED && f.cls == "SYNTAX-ERROR" && !f.file.empty())
                    broken.insert(f.file);
        out.insert(out.end(), part.begin(), part.end());
    }
    return out;
}

}  // namespace prism
