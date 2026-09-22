// Polyglot stage: every language in the tree, not just C/C++.
// Port of the Python engine prism/polyglot.py — same tables, same statuses.
// tests/test_polyglot.py locks the two tables together.
//
//   diagnostic            -> FAILED   (strength FINDS; extra.severity/rule/tool)
//   tool ran, silent      -> UNKNOWN  ("no diagnostics (not a proof)")
//   tool crashed/unusable -> ERROR
//   tool timed out        -> TIMEOUT
//   tool missing          -> NOTRUN   (extra.install)

#include "prism/stages.hpp"
#include "prism/laws.hpp"
#include "prism/regex.hpp"
#include "proc.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <fstream>
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
                                            ".hpp", ".hh", ".hxx", ".cu"};

const std::set<std::string> kTextOnlyExts = {".env", ".ini", ".cfg", ".conf", ".properties",
                                             ".xml", ".java", ".kt", ".cs", ".swift",
                                             ".scala", ".sql", ".tf", ".gradle"};

const std::set<std::string> kSkipDirs = {
    ".git", "prism-out", "third_party", "build", "node_modules", "__pycache__",
    ".venv", "venv", "target", ".tox", ".mypy_cache", ".ruff_cache", ".pytest_cache",
};

constexpr std::uintmax_t MAX_FILE_BYTES = 2'000'000;
constexpr std::size_t MAX_FILES_PER_TOOL = 2000;
constexpr std::size_t BATCH = 200;
#ifdef _WIN32
const char* const DEVNULL = "nul";
#else
const char* const DEVNULL = "/dev/null";
#endif

// Byte-for-byte the Python engine SYNTAX_HELPER.
const char* const SYNTAX_HELPER = R"PRISMPY(
import json, re, sys
try:
    import tomllib
except ImportError:
    tomllib = None
for path in sys.argv[1:]:
    try:
        with open(path, "rb") as fh:
            data = fh.read()
    except OSError as ex:
        print("PRISM-SYNTAX\t%s\t0\t0\tunreadable: %s" % (path, ex))
        continue
    low = path.lower()
    try:
        if low.endswith((".py", ".pyi")):
            compile(data, path, "exec", dont_inherit=True)
        elif low.endswith(".json"):
            json.loads(data.decode("utf-8-sig"))
        elif low.endswith(".toml"):
            if tomllib is None:
                print("PRISM-NOTOML\t%s" % path)
                continue
            tomllib.loads(data.decode("utf-8"))
    except SyntaxError as ex:
        msg = "%s: %s" % (type(ex).__name__, ex.msg)
        print("PRISM-SYNTAX\t%s\t%d\t%d\t%s" % (path, ex.lineno or 0, ex.offset or 0, msg))
    except json.JSONDecodeError as ex:
        print("PRISM-SYNTAX\t%s\t%d\t%d\tJSON: %s" % (path, ex.lineno, ex.colno, ex.msg))
    except ValueError as ex:
        m = re.search(r"line (\d+), column (\d+)", str(ex))
        ln, col = (int(m.group(1)), int(m.group(2))) if m else (0, 0)
        print("PRISM-SYNTAX\t%s\t%d\t%d\t%s: %s" % (path, ln, col, type(ex).__name__, ex))
    else:
        print("PRISM-OK\t%s" % path)
)PRISMPY";

struct PgTool {
    std::string name;
    std::vector<std::string> exes;
    std::vector<std::string> argv;  // {exe} {helper} {files} {file}
    std::string pattern;
    bool per_file = false;
    std::vector<int> ok_rcs{0, 1};
    double timeout = 120.0;
    std::string cwd_marker{};
    std::string unconfigured{};
};

struct PgCheck {
    std::string group;
    std::vector<std::string> languages;
    std::string kind;  // syntax | lint | type
    std::vector<PgTool> tools;
    std::string install;
};

const char* const GCC_PATTERN =
    R"(^(?P<file>[^\s:][^:]*?):(?P<line>\d+):(?:(?P<col>\d+):)?\s*(?:(?P<sev>error|warning|note|info|style|fatal)\s*:\s*)?(?P<msg>.+)$)";

const std::vector<PgCheck>& checks() {
    static const std::vector<PgCheck> C = {
        {"python-syntax", {"python", "json", "toml"}, "syntax",
         {{"prism-syntax", {"python3", "python"}, {"{exe}", "{helper}", "{files}"},
           R"(^PRISM-SYNTAX\t(?P<file>[^\t]+)\t(?P<line>\d+)\t(?P<col>\d+)\t(?P<msg>.+)$)",
           false, {0}}},
         "install Python 3.11+ (python3 on PATH)"},
        {"python-lint", {"python"}, "lint",
         {{"ruff", {"ruff"},
           {"{exe}", "check", "--output-format=concise", "--no-cache", "--quiet", "{files}"},
           R"(^(?P<file>.+?):(?P<line>\d+):(?P<col>\d+): (?P<rule>[A-Z]+\d+) (?P<msg>.+)$)"},
          {"pyflakes", {"pyflakes"}, {"{exe}", "{files}"},
           R"(^(?P<file>.+?):(?P<line>\d+):(?:(?P<col>\d+):?)?\s+(?P<msg>.+)$)"}},
         "pip install ruff  (or pyflakes)"},
        {"python-types", {"python"}, "type",
         {{"mypy", {"mypy"},
           {"{exe}", "--ignore-missing-imports", "--no-error-summary", "--show-column-numbers",
            "--no-color-output", "--no-incremental", "--cache-dir={devnull}",
            "{files}"},
           R"(^(?P<file>.+?):(?P<line>\d+):(?:(?P<col>\d+):)? (?P<sev>error): (?P<msg>.+?)(?:\s+\[(?P<rule>[\w-]+)\])?$)",
           false, {0, 1}, 600.0}},
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
           R"(couldn't find (?:a|an) (?:eslint\.config|configuration file))"}},
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
           R"(^(?P<file>.+?\.go):(?P<line>\d+):(?P<col>\d+): (?P<msg>.+)$)", false, {0}}},
         "install Go (gofmt on PATH)"},
        {"rust-lint", {"rust"}, "lint",
         {{"cargo-clippy", {"cargo"}, {"{exe}", "clippy", "--quiet", "--message-format=short"},
           R"(^(?P<file>[^\s:][^:]*?\.rs):(?P<line>\d+):(?P<col>\d+): (?P<sev>error|warning)(?:\[(?P<rule>[^\]]+)\])?: (?P<msg>.+)$)",
           false, {0, 101}, 900.0, "Cargo.toml"}},
         "install Rust (rustup component add clippy); needs a Cargo.toml"},
        {"ruby-syntax", {"ruby"}, "syntax",
         {{"ruby", {"ruby"}, {"{exe}", "-wc", "{file}"},
           R"(^(?:\S*ruby\S*: )?(?P<file>[^:\n]+?):(?P<line>\d+): (?:(?P<sev>warning): )?(?P<msg>.+)$)",
           true, {0}}},
         "install Ruby"},
        {"php-syntax", {"php"}, "syntax",
         {{"php", {"php"}, {"{exe}", "-l", "{file}"},
           R"(^(?:PHP )?(?P<msg>(?:Parse|Fatal) error:.+?) in (?P<file>.+?) on line (?P<line>\d+)$)",
           true, {0}}},
         "install PHP CLI"},
        {"perl-syntax", {"perl"}, "syntax",
         {{"perl", {"perl"}, {"{exe}", "-c", "{file}"},
           R"(^(?P<msg>.+?) at (?P<file>.+?) line (?P<line>\d+)[.,])", true, {0}}},
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

struct BuiltinScan {
    const char* cls;
    const char* pattern;
    const char* message;
};

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

bool skip_dir(const std::string& name) {
    return kSkipDirs.contains(name) || name.starts_with("prism-out") || name.starts_with("build");
}

bool wanted_file(const fs::path& p) {
    auto e = lower(p.extension().string());
    return kLangExts.contains(e) || kCFamilyExts.contains(e) || kTextOnlyExts.contains(e) ||
           p.filename() == ".env";
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

std::vector<fs::path> iter_polyglot_sources(const fs::path& root) {
    std::vector<fs::path> out;
    std::error_code ec;
    if (fs::is_regular_file(root, ec)) {
        if (wanted_file(root)) out.push_back(root);
        return out;
    }
    if (!fs::is_directory(root, ec)) return out;
    // Sorted, depth-first, same order as os.walk with sorted dirnames/filenames.
    std::vector<fs::path> stack{root};
    while (!stack.empty()) {
        auto dir = stack.back();
        stack.pop_back();
        std::vector<fs::path> files, dirs;
        for (auto it = fs::directory_iterator(dir, fs::directory_options::skip_permission_denied, ec);
             !ec && it != fs::directory_iterator(); it.increment(ec)) {
            std::error_code e2;
            if (it->is_directory(e2) && !it->is_symlink(e2)) {
                if (!skip_dir(it->path().filename().string())) dirs.push_back(it->path());
            } else if (it->is_regular_file(e2) && wanted_file(it->path())) {
                files.push_back(it->path());
            }
        }
        std::sort(files.begin(), files.end());
        std::sort(dirs.begin(), dirs.end());
        out.insert(out.end(), files.begin(), files.end());
        for (auto d = dirs.rbegin(); d != dirs.rend(); ++d) stack.push_back(*d);
    }
    return out;
}

namespace {

std::vector<Finding> builtin_scan(const std::vector<fs::path>& files, const fs::path& root) {
    static const std::vector<std::pair<const BuiltinScan*, Regex>> rx = [] {
        std::vector<std::pair<const BuiltinScan*, Regex>> v;
        for (auto& s : kBuiltinScans) v.emplace_back(&s, Regex(s.pattern));
        return v;
    }();
    std::vector<Finding> out;
    int scanned = 0;
    for (auto& p : files) {
        std::error_code ec;
        auto sz = fs::file_size(p, ec);
        if (ec || sz > MAX_FILE_BYTES) continue;
        std::ifstream in(p, std::ios::binary);
        if (!in) continue;
        ++scanned;
        auto r = rel(p, root);
        std::string line;
        int n = 0;
        while (std::getline(in, line)) {
            ++n;
            if (!line.empty() && line.back() == '\r') line.pop_back();
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
    auto lookup = [&](const std::string& key) -> std::optional<fs::path> {
        auto it = cfg.tools.find(key);
        std::error_code ec;
        if (it != cfg.tools.end() && fs::is_regular_file(it->second, ec)) return it->second;
        return std::nullopt;
    };
    if (auto hit = lookup(tool.name)) return hit;
    for (auto& n : tool.exes)
        if (auto hit = lookup(n)) return hit;
    for (auto& n : tool.exes)
        if (auto hit = cfg.which({std::string_view(n)})) return hit;
    return std::nullopt;
}

std::vector<std::string> expand(const PgTool& tool, const std::string& exe,
                                const std::vector<std::string>& files, const std::string& helper) {
    std::vector<std::string> cmd;
    for (auto& a : tool.argv) {
        if (a == "{exe}") cmd.push_back(exe);
        else if (a == "{helper}") cmd.push_back(helper);
        else if (a == "{files}" || a == "{file}") cmd.insert(cmd.end(), files.begin(), files.end());
        else if (auto k = a.find("{devnull}"); k != std::string::npos)
            cmd.push_back(a.substr(0, k) + DEVNULL + a.substr(k + 9));
        else cmd.push_back(a);
    }
    return cmd;
}

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

struct HelperFile {
    fs::path dir;
    fs::path path;
    HelperFile() {
        auto ticks = std::chrono::high_resolution_clock::now().time_since_epoch().count();
        dir = fs::temp_directory_path() / ("prism_polyglot_" + std::to_string(ticks));
        std::error_code ec;
        fs::create_directories(dir, ec);
        path = dir / "prism_syntax.py";
        std::ofstream(path, std::ios::binary) << SYNTAX_HELPER;
    }
    ~HelperFile() {
        std::error_code ec;
        fs::remove_all(dir, ec);
    }
};

std::vector<Finding> run_check(const PgCheck& check, std::vector<fs::path> files,
                               const fs::path& root, const Config& cfg) {
    const PgTool* tool = nullptr;
    std::optional<fs::path> exe;
    for (auto& t : check.tools) {
        exe = resolve(t, cfg);
        if (exe) {
            tool = &t;
            break;
        }
    }
    auto langs = join(check.languages, "/");
    auto nfiles = std::to_string(files.size());
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

    HelperFile helper;
    const Regex unconfigured(tool->unconfigured.empty() ? std::string{}
                                                        : "(?i)" + tool->unconfigured);
    static const Regex notoml(R"(^PRISM-NOTOML\t(.+)$)", /*multiline=*/true);
    auto one = [&](const Call& call) -> std::vector<Finding> {
        auto r = detail::run_process(expand(*tool, exe->string(), call.files, helper.path.string()),
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
        auto hits = parse_output(*tool, r.text, root, call.cwd, check);
        for (auto& m : notoml.finditer(r.text))
            hits.push_back(pg_finding(laws::NOTRUN, rel(m.group(1), root), std::nullopt, "",
                                      "toml not checked: interpreter has no tomllib (Python < 3.11)",
                                      {{"install", check.install}, {"tool", tool->name},
                                       {"check", check.group}}));
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
        return {};
    };

    std::vector<Finding> results;
    std::size_t jobs = static_cast<std::size_t>(std::max(1, cfg.jobs));
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
    auto files = iter_polyglot_sources(root);
    if (files.empty())
        return {pg_finding(laws::UNKNOWN, "", std::nullopt, "", "no source files in scope")};
    auto out = builtin_scan(files, root);
    std::map<std::string, std::vector<fs::path>> by_lang;
    for (auto& p : files)
        if (auto lang = language_of(p); !lang.empty()) by_lang[lang].push_back(p);
    // A file that does not parse makes whole-program type checkers (mypy, tsc)
    // abort and hide every other file's errors. It already has its
    // SYNTAX-ERROR; keep it away from the type checkers.
    std::set<std::string> broken;
    for (auto& check : checks()) {
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
