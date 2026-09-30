// Polyglot stage (src/prism/polyglot.cpp, src/prism/toml_syntax.cpp):
// every language in the tree, not just C/C++.
//
// Laws: a missing tool is NOTRUN (with install), a silent tool is UNKNOWN
// (not a proof), a diagnostic is FAILED, output PRISM does not understand
// is ERROR, and a tool that runs scanned code is NOTRUN without --allow-exec.

#include <doctest/doctest.h>

#include "prism/config.hpp"
#include "prism/laws.hpp"
#include "prism/polyglot.hpp"
#include "prism/regex.hpp"
#include "prism/sandbox.hpp"
#include "prism/stages.hpp"
#include "prism/taxonomy.hpp"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <random>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;
namespace pg = prism::polyglot;

namespace {

struct Tree {
    fs::path dir;
    Tree() {
        std::random_device rd;
        dir = fs::temp_directory_path() / ("prism_pg_t_" + std::to_string(rd()) + std::to_string(rd()));
        fs::create_directories(dir);
    }
    explicit Tree(const std::map<std::string, std::string>& files) : Tree() {
        for (auto& [k, v] : files) put(k, v);
    }
    void put(const std::string& rel, const std::string& text) const {
        auto p = dir / rel;
        fs::create_directories(p.parent_path());
        std::ofstream(p, std::ios::binary) << text;
    }
    std::vector<prism::Finding> run(bool allow_exec = false,
                                    std::map<std::string, fs::path> tools = {}) const {
        auto cfg = prism::default_config();
        cfg.root = dir;
        cfg.jobs = 2;
        cfg.allow_exec = allow_exec;
        cfg.tools = std::move(tools);
        return prism::run_polyglot(dir, cfg);
    }
    ~Tree() {
        std::error_code ec;
        fs::remove_all(dir, ec);
    }
};

std::string xo(const prism::Finding& f, const char* key) {
    auto it = f.extra.find(key);
    return it == f.extra.end() ? std::string() : it->second;
}

std::vector<prism::Finding> rows_of(const std::vector<prism::Finding>& out, const std::string& group) {
    std::vector<prism::Finding> r;
    for (auto& f : out)
        if (xo(f, "check") == group) r.push_back(f);
    return r;
}

std::string join(const std::vector<std::string>& v, const char* sep = ",") {
    std::string o;
    for (std::size_t i = 0; i < v.size(); ++i) o += (i ? sep : "") + v[i];
    return o;
}

const pg::Tool& tool_named(const std::string& name, const pg::Check** check = nullptr) {
    for (auto& c : pg::checks())
        for (auto& t : c.tools)
            if (t.name == name) {
                if (check) *check = &c;
                return t;
            }
    FAIL("no tool " << name);
    throw 0;
}

// Canned output: "/r/" is the scan root (testdata here, as the parsers only
// make names relative to it).
fs::path testdata() { return fs::path(PRISM_SOURCE_DIR) / "testdata"; }

std::vector<prism::Finding> parse(const std::string& name, std::string text) {
    const pg::Check* c = nullptr;
    auto& t = tool_named(name, &c);
    const auto root = testdata().generic_string() + "/";
    for (std::size_t k; (k = text.find("/r/")) != std::string::npos;) text.replace(k, 3, root);
    return pg::parse_output(t, text, testdata(), {}, *c);
}

#ifndef _WIN32
struct EnvGuard {
    std::string name, was;
    bool had = false;
    EnvGuard(const char* n, const std::string& value) : name(n) {
        if (const char* v = std::getenv(n)) {
            had = true;
            was = v;
        }
        setenv(n, value.c_str(), 1);
    }
    ~EnvGuard() {
        if (had) setenv(name.c_str(), was.c_str(), 1);
        else unsetenv(name.c_str());
    }
};

// A stand-in tool: logs its argv (one line per call) and prints `out`.
fs::path fake_tool(const fs::path& dir, const std::string& name, const fs::path& log,
                   const std::string& out = "", int rc = 0) {
    auto p = dir / name;
    std::ofstream(p, std::ios::binary) << "#!/bin/sh\necho \"$@\" >> '" << log.string() << "'\n"
                                       << (out.empty() ? "" : "printf '%s\\n' '" + out + "'\n")
                                       << "exit " << rc << "\n";
    fs::permissions(p, fs::perms::owner_all);
    return p;
}

std::string slurp(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}
#endif

}  // namespace

// ---- the table (replaces the Python/C++ table lock) -------------------------

TEST_CASE("polyglot table: groups, tools, argv, install, executes and benign are pinned") {
    std::vector<std::string> got;
    for (auto& c : pg::checks()) {
        got.push_back(c.group + "|" + join(c.languages) + "|" + c.kind + "|" + c.install);
        for (auto& t : c.tools) {
            std::vector<std::string> argv;
            for (auto& a : t.argv) argv.push_back(a.find('\n') != std::string::npos ? "<script>" : a);
            std::vector<std::string> rcs;
            for (int r : t.ok_rcs) rcs.push_back(std::to_string(r));
            got.push_back("  " + t.name + "|" + join(t.exes) + "|" + join(argv, " ") + "|" +
                          (t.per_file ? "per-file" : "batch") + "|" + join(rcs) + "|" +
                          std::to_string(static_cast<int>(t.timeout)) + "|" + t.cwd_marker + "|" +
                          (t.executes ? "EXEC" : "") + "|" + join(t.benign, " ; ") + "|" +
                          (t.native ? "native" : ""));
        }
    }
    const std::vector<std::string> want = {
        "python-syntax|python|syntax|install Python 3 (python3 on PATH)",
        "  python|python3,python|{exe} -I -c <script> {files}|batch|0|120|||PRISM-OK\\t.+|",
        "json-syntax|json|syntax|built in",
        "  prism-json|||batch|0|120||||native",
        "toml-syntax|toml|syntax|built in",
        "  prism-toml|||batch|0|120||||native",
        "python-lint|python|lint|pip install ruff  (or pyflakes)",
        "  ruff|ruff|{exe} check --output-format=concise --no-cache --quiet {files}|batch|0,1|120|||"
        "All checks passed!|",
        "  pyflakes|pyflakes|{exe} {files}|batch|0,1|120||||",
        "python-types|python|type|pip install mypy",
        "  mypy|mypy|{exe} --ignore-missing-imports --no-error-summary --show-column-numbers "
        "--no-color-output --no-incremental --cache-dir={devnull} --config-file= {files}|batch|0,1|600|||"
        "Success: no issues found in \\d+ source files?|",
        "javascript-syntax|javascript|syntax|install Node.js (node on PATH)",
        "  node|node|{exe} --check {file}|per-file|0|120||||",
        "typescript-types|typescript|type|npm install -g typescript",
        "  tsc|tsc|{exe} --noEmit --pretty false --skipLibCheck --jsx preserve --allowJs {files}|batch|"
        "0,1,2|600||||",
        "javascript-lint|javascript,typescript|lint|npm install -g eslint  (needs an eslint.config.* in "
        "the project)",
        "  eslint|eslint|{exe} --format unix {files}|batch|0,1|120||EXEC||",
        "shell-syntax|shell|syntax|install bash",
        "  bash|bash|{exe} -n {file}|per-file|0|120||||",
        "shell-lint|shell|lint|apt install shellcheck",
        "  shellcheck|shellcheck|{exe} -f gcc {files}|batch|0,1|120||||",
        "go-syntax|go|syntax|install Go (gofmt on PATH)",
        "  gofmt|gofmt|{exe} -e -l {files}|batch|0|120|||.+\\.go|",
        "rust-lint|rust|lint|install Rust (rustup component add clippy); needs a Cargo.toml",
        "  cargo-clippy|cargo|{exe} clippy --quiet --message-format=short|batch|0,101|900|Cargo.toml|"
        "EXEC||",
        "ruby-syntax|ruby|syntax|install Ruby",
        "  ruby|ruby|{exe} -wc {file}|per-file|0|120|||Syntax OK|",
        "php-syntax|php|syntax|install PHP CLI",
        "  php|php|{exe} -l {file}|per-file|0|120|||No syntax errors detected in .+|",
        "perl-syntax|perl|syntax|install Perl",
        "  perl|perl|{exe} -c {file}|per-file|0|120||EXEC|.+ syntax OK|",
        "lua-syntax|lua|syntax|install Lua (luac on PATH)",
        "  luac|luac,luac5.4,luac5.3|{exe} -p {file}|per-file|0|120||||",
        "yaml-lint|yaml|lint|pip install yamllint",
        "  yamllint|yamllint|{exe} -f parsable {files}|batch|0,1|120||||",
    };
    CHECK(got == want);
    for (std::size_t i = 0; i < std::max(got.size(), want.size()); ++i)
        if (i >= got.size() || i >= want.size() || got[i] != want[i]) {
            MESSAGE("first drift at row " << i << ": " << (i < got.size() ? got[i] : "<none>"));
            break;
        }

    // Law 9: exactly these run scanned code; mypy never reads the project's config.
    std::set<std::string> exec;
    for (auto& c : pg::checks())
        for (auto& t : c.tools) {
            if (t.executes) exec.insert(t.name);
            // Every pattern and benign line is a valid regex; native rows have neither.
            if (t.native) {
                CHECK(t.pattern.empty());
                CHECK(t.exes.empty());
            } else {
                CHECK_NOTHROW(prism::Regex(t.pattern, true));
            }
            for (auto& b : t.benign) CHECK_NOTHROW(prism::Regex("^(?:" + b + ")$"));
        }
    CHECK(exec == std::set<std::string>{"perl", "cargo-clippy", "eslint"});
    auto& mypy = tool_named("mypy");
    CHECK(std::find(mypy.argv.begin(), mypy.argv.end(), "--config-file=") != mypy.argv.end());
    // The Python syntax row compiles, never runs, and keeps the scanned tree off sys.path.
    auto& py = tool_named("python");
    REQUIRE(py.argv.size() == 5);
    CHECK(py.argv[1] == "-I");
    CHECK(py.argv[3].find("compile(") != std::string::npos);
    CHECK(py.argv[3].find("exec(") == std::string::npos);
}

TEST_CASE("polyglot table: extensions, text-only and C-family sets, builtin scans") {
    const std::map<std::string, std::string> want = {
        {".py", "python"}, {".pyi", "python"}, {".js", "javascript"}, {".mjs", "javascript"},
        {".cjs", "javascript"}, {".jsx", "javascript"}, {".ts", "typescript"}, {".tsx", "typescript"},
        {".mts", "typescript"}, {".cts", "typescript"}, {".sh", "shell"}, {".bash", "shell"},
        {".go", "go"}, {".rs", "rust"}, {".rb", "ruby"}, {".php", "php"}, {".pl", "perl"},
        {".pm", "perl"}, {".lua", "lua"}, {".json", "json"}, {".toml", "toml"}, {".yml", "yaml"},
        {".yaml", "yaml"},
    };
    CHECK(pg::lang_exts() == want);
    CHECK(pg::c_family_exts() == std::set<std::string>{".c", ".h", ".cc", ".cpp", ".cxx", ".hpp",
                                                      ".hh", ".hxx", ".cu", ".i", ".ii"});
    CHECK(pg::text_only_exts() == std::set<std::string>{".env", ".ini", ".cfg", ".conf",
                                                       ".properties", ".xml", ".java", ".kt", ".cs",
                                                       ".swift", ".scala", ".sql", ".tf", ".gradle"});
    // Every language a check covers has an extension, and every language has a check.
    std::set<std::string> covered, langs;
    for (auto& c : pg::checks())
        for (auto& l : c.languages) covered.insert(l);
    for (auto& [e, l] : pg::lang_exts()) langs.insert(l);
    CHECK(covered == langs);
    std::vector<std::string> cls;
    for (auto& s : pg::builtin_scans()) {
        cls.push_back(s.cls);
        CHECK_NOTHROW(prism::Regex(s.pattern));
        CHECK(std::string(s.message).size() > 0);
    }
    CHECK(cls == std::vector<std::string>{"VCS-CONFLICT-MARKER", "SECRET-PRIVATE-KEY",
                                          "SECRET-AWS-KEY", "SECRET-GITHUB-TOKEN",
                                          "SECRET-SLACK-TOKEN", "SECRET-GOOGLE-API-KEY",
                                          "SECRET-STRIPE-KEY"});
    CHECK(std::string(pg::ALLOW_MARKER) == "prism:allow");
    // Every class the stage emits is a taxonomy class.
    std::set<std::string> ids;
    for (auto& c : prism::taxonomy_classes()) ids.insert(c.id);
    for (auto& c : std::vector<std::string>{"SYNTAX-ERROR", "TYPE-ERROR", "LANG-LINT"}) cls.push_back(c);
    for (auto& c : cls) CHECK_MESSAGE(ids.contains(c), c);
    CHECK(prism::is_known_source("a.PY"));
    CHECK(prism::is_known_source("x/.env"));
    CHECK(prism::is_known_source("k.ini"));
    CHECK_FALSE(prism::is_known_source("README"));
}

// ---- built-in scans ---------------------------------------------------------

TEST_CASE("polyglot builtin: conflict markers at their lines, secrets, allow marker") {
    {
        Tree t({{"a.c", "<<<<<<< HEAD\nint x;\n=======\nint y;\n>>>>>>> b\n"}});
        std::vector<int> lines;
        for (auto& f : t.run())
            if (f.cls == "VCS-CONFLICT-MARKER") {
                CHECK(f.status == prism::laws::FAILED);
                CHECK(f.stage == "polyglot");
                lines.push_back(f.line.value_or(0));
            }
        CHECK(lines == std::vector<int>{1, 5});
    }
    {
        Tree t({{"k.ini", "aws = AKIA" "ABCDEFGHIJKLMNOP\n"},
                {"id_rsa.conf", "-----BEGIN RSA " "PRIVATE KEY-----\n"},
                {"t.env", "TOKEN=ghp_" + std::string(36, 'a') + "\n"},
                {"s.cfg", "slack = xoxb-" "0123456789abc\n"},
                {"g.xml", "<k>AIza" + std::string(35, 'Q') + "</k>\n"},
                {"p.properties", "stripe=sk_live_" + std::string(24, 'z') + "\n"}});
        std::set<std::string> found;
        for (auto& f : t.run())
            if (f.status == prism::laws::FAILED) found.insert(f.cls);
        for (auto* c : {"SECRET-AWS-KEY", "SECRET-PRIVATE-KEY", "SECRET-GITHUB-TOKEN",
                        "SECRET-SLACK-TOKEN", "SECRET-GOOGLE-API-KEY", "SECRET-STRIPE-KEY"})
            CHECK_MESSAGE(found.contains(c), c);
    }
    {
        Tree t({{"k.py", "KEY = \"AKIA" "ABCDEFGHIJKLMNOP\"  # prism" ":allow\n"}});
        for (auto& f : t.run()) CHECK(f.cls.rfind("SECRET-", 0) != 0);
    }
}

TEST_CASE("polyglot builtin: nothing found is CLEAN and says it is not a proof") {
    auto out = pg::builtin_scan({}, ".");
    REQUIRE(out.size() == 1);
    CHECK(out[0].status == prism::laws::CLEAN);
    CHECK(out[0].message.find("not a proof") != std::string::npos);
    CHECK_FALSE(prism::laws::is_proof(out[0].status));
}

TEST_CASE("polyglot builtin: text sniff and skipped directories") {
    Tree t({{"a", "text\n"}, {"b.dat", std::string("x\0y", 3)}, {"sub/c.txt", "ok\n"}});
    std::ofstream(t.dir / "big.txt", std::ios::binary) << std::string(pg::MAX_FILE_BYTES + 1, 'a');
    std::vector<std::string> names;
    for (auto& p : prism::iter_text_files(t.dir)) names.push_back(p.lexically_relative(t.dir).generic_string());
    CHECK(names == std::vector<std::string>{"a", "sub/c.txt"});
    // A NUL within the first SNIFF_BYTES is binary; after it the file is text.
    t.put("late.txt", std::string(pg::SNIFF_BYTES, 'a') + std::string(1, '\0'));
    CHECK(prism::is_text_file(t.dir / "late.txt"));

    Tree s({{"node_modules/x.js", "<<<<<<< a\n"}, {"src/y.py", "x = 1\n"}, {"build-x/z.sh", "echo\n"}});
    names.clear();
    for (auto& p : prism::iter_polyglot_sources(s.dir)) names.push_back(p.lexically_relative(s.dir).generic_string());
    CHECK(names == std::vector<std::string>{"src/y.py"});
    for (auto& f : s.run()) CHECK(f.file.find("node_modules") == std::string::npos);
}

// ---- syntax: JSON and TOML in process, Python through python3 ---------------

TEST_CASE("polyglot syntax: broken python / json / toml are FAILED SYNTAX-ERROR with a line") {
    Tree t({{"bad.py", "def f(:\n    pass\n"},
            {"bad.json", "{\"a\": 1,}\n"},
            {"bad.toml", "a = \n"},
            {"ok.py", "x = 1\n"},
            {"ok.json", "{\"a\": [1, 2]}\n"},
            {"ok.toml", "[t]\na = 1\n"}});
    auto out = t.run();
    std::map<std::string, prism::Finding> syn;
    for (auto& f : out)
        if (f.cls == "SYNTAX-ERROR") syn[f.file] = f;
    const bool have_python = prism::default_config().which({"python3", "python"}).has_value();
    if (have_python) {
        REQUIRE(syn.contains("bad.py"));
        CHECK(syn["bad.py"].line == 1);
        CHECK(xo(syn["bad.py"], "check") == "python-syntax");
    } else {
        auto py = rows_of(out, "python-syntax");
        REQUIRE(py.size() == 1);
        CHECK(py[0].status == prism::laws::NOTRUN);
    }
    REQUIRE(syn.contains("bad.json"));
    CHECK(syn["bad.json"].line == 1);
    CHECK(xo(syn["bad.json"], "check") == "json-syntax");
    CHECK(xo(syn["bad.json"], "tool") == "prism-json");
    CHECK(syn["bad.json"].message.rfind("prism-json: JSON: ", 0) == 0);
    REQUIRE(syn.contains("bad.toml"));
    CHECK(syn["bad.toml"].line == 1);
    CHECK(xo(syn["bad.toml"], "col") == "5");
    CHECK(xo(syn["bad.toml"], "check") == "toml-syntax");
    for (auto* ok : {"ok.py", "ok.json", "ok.toml"}) CHECK_FALSE(syn.contains(ok));
    for (auto& [file, f] : syn) {
        CHECK(f.status == prism::laws::FAILED);
        CHECK(f.stage == "polyglot");
        CHECK(f.strength == prism::laws::STRENGTH_FINDS);
    }
}

TEST_CASE("polyglot syntax: all well-formed is UNKNOWN (not a proof); json/toml never NOTRUN") {
#ifndef _WIN32
    EnvGuard path("PATH", "/nonexistent-prism-path");
#endif
    Tree t({{"ok.json", "{}\n"}, {"ok.toml", "a = 1\n"}, {"b.json", "[]"}});
    auto out = t.run();
    for (auto* g : {"json-syntax", "toml-syntax"}) {
        auto rows = rows_of(out, g);
        REQUIRE_MESSAGE(rows.size() == 1, g);
        CHECK(rows[0].status == prism::laws::UNKNOWN);
        CHECK(rows[0].message.find("no diagnostics (not a proof)") != std::string::npos);
        CHECK_FALSE(prism::laws::is_proof(rows[0].status));
    }
    CHECK(rows_of(out, "json-syntax")[0].message == "prism-json: 2 json file(s), no diagnostics (not a proof)");
}

TEST_CASE("json_syntax: RFC 8259 with line and column") {
    for (const char* ok : {"{}", "[1, 2.5e3, \"x\", true, false, null]", "\xEF\xBB\xBF{\"a\": 1}",
                           "{\"a\": {\"b\": []}, \"\\u00e9\": \"\\ud83d\\ude00\"}", "  42  ", "\"s\""})
        CHECK_MESSAGE(!pg::json_syntax(ok), std::string(ok));
    struct Bad {
        const char* text;
        int line;
    };
    for (auto b : {Bad{"{\"a\": 1,}\n", 1}, Bad{"{\n  \"a\": 1\n  \"b\": 2\n}\n", 3}, Bad{"", 1},
                   Bad{"NaN", 1}, Bad{"[1] x", 1}, Bad{"\"\xff\"", 1}, Bad{"{'a': 1}", 1},
                   Bad{"[1,\n2,\n]", 3}, Bad{"\"\\ud800\"", 1}}) {
        auto e = pg::json_syntax(b.text);
        REQUIRE_MESSAGE(e, std::string(b.text));
        CHECK_MESSAGE(e->line == b.line, std::string(b.text) << " -> " << e->msg);
        CHECK(e->msg.rfind("JSON: ", 0) == 0);
        CHECK(e->msg.find("json.exception") == std::string::npos);
    }
}

TEST_CASE("toml_syntax: TOML 1.0 documents that parse") {
    for (const char* ok : {
             "",
             "# only a comment\n",
             "a = 1\n",
             "a = \"x\" # c\r\nb = 'y'\r\n",
             "\xEF\xBB\xBF" "a = 1\n",
             "[t]\nx = 1\n[t.u]\ny = 2\n",
             "[[a]]\nb = 1\n[[a]]\nb = 2\n[a.c]\nd = 3\n",
             "a.b.c = 1\na.b.d = 2\n",
             "x = [1, 2,\n  3, # c\n]\ny = []\nz = [[1], ['a'], {k = 1}]\n",
             "t = { a = 1, b.c = 2 }\ne = {}\n",
             "d = 1979-05-27T07:32:00Z\ne = 1979-05-27 07:32:00.999-07:00\nf = 1979-05-27\n"
             "g = 07:32:00\nh = 1979-05-27t07:32:00\nleap = 2000-02-29\n",
             "n = [0x1F, 0o7, 0b1, 1_000, -0, +1.5e-3, inf, -nan, +inf, 6.626e-34, 1e06, 0.0, 3E+2]\n",
             "s = \"\"\"\nline \\\n    more\"\"\"\"\nq = \"\"\"two \"\" quotes\"\"\"\n",
             "l = '''a''b'''\nm = '''\nx''''\n",
             "\"quoted key\" = 1\n'lit' = 2\n\"\" = 3\n",
             "[a.b]\n[a]\nc = 1\n",
             "[fruit]\napple.color = 'red'\n[fruit.apple.texture]\nsmooth = true\n",
             "k = \"\\u00e9\\U0001F600\\t\\\\\"\n",
             "[ a . b ]\n[ \"x y\" . 'z' ]\n",
             "a = true\nb = false\n",
             "[[fruits]]\n[fruits.physical]\nc = 1\n[[fruits]]\n[fruits.physical]\nc = 2\n",
             "str = \"caf\xC3\xA9\"\n",
         })
        CHECK_MESSAGE(!pg::toml_syntax(ok), std::string(ok));
}

TEST_CASE("toml_syntax: syntax and redefinition errors, with line and column") {
    struct Bad {
        const char* text;
        int line;
    };
    for (auto b : {
             Bad{"a = \n", 1},
             Bad{"a = 1\na = 2\n", 2},
             Bad{"[t]\n[t]\n", 2},
             Bad{"a.b = 1\n[a.b]\n", 2},
             Bad{"[fruit]\napple.color = 'red'\n[fruit.apple]\n", 3},
             Bad{"a = {b = 1}\na.c = 2\n", 2},
             Bad{"t = {a = 1,}\n", 1},
             Bad{"t = {a = 1\n}\n", 1},
             Bad{"x = [1 2]\n", 1},
             Bad{"a = 01\n", 1},
             Bad{"a = 1__0\n", 1},
             Bad{"a = 0x\n", 1},
             Bad{"a = 1.\n", 1},
             Bad{"a = .5\n", 1},
             Bad{"a = +0x1\n", 1},
             Bad{"a = 1e\n", 1},
             Bad{"d = 1979-13-01\n", 1},
             Bad{"d = 1979-02-30\n", 1},
             Bad{"d = 1900-02-29\n", 1},
             Bad{"t = 25:00:00\n", 1},
             Bad{"t = 07:32\n", 1},
             Bad{"s = \"abc\n", 1},
             Bad{"s = \"\\q\"\n", 1},
             Bad{"s = \"\\uD800\"\n", 1},
             Bad{"a = 1 b = 2\n", 1},
             Bad{"[[a]]\n[a]\n", 2},
             Bad{"[a]\n[[a]]\n", 2},
             Bad{"a = []\n[[a]]\n", 2},
             Bad{"x = 1\n\xff = 1\n", 2},
             Bad{"[a.b.c]\nz = 9\n[a]\nb.c.t = 1\n", 4},
             Bad{"a = \"\"\"x\"\"\"\"\"\"\n", 1},
             Bad{"a = 1 # \x01\n", 1},
             Bad{"a = \"x\rb\"\n", 1},
             Bad{"= 1\n", 1},
             Bad{"[a\n", 1},
             Bad{"a = truex\n", 1},
             Bad{"\"\"\"k\"\"\" = 1\n", 1},
             Bad{"a = \"\"\"never closed\n", 1},
             Bad{"a = [1,\n2\n", 3},
             Bad{"a = 1\r", 1},
         }) {
        auto e = pg::toml_syntax(b.text);
        REQUIRE_MESSAGE(e, std::string(b.text));
        CHECK_MESSAGE(e->line == b.line, std::string(b.text) << " -> " << e->msg);
        CHECK(e->col >= 1);
        CHECK(e->msg.rfind("TOML: ", 0) == 0);
    }
    // Deep nesting is an error, not a stack overflow.
    auto deep = pg::toml_syntax("a = " + std::string(100000, '[') + std::string(100000, ']') + "\n");
    REQUIRE(deep);
    CHECK(deep->msg.find("nested") != std::string::npos);
    // Columns count characters, not bytes.
    auto col = pg::toml_syntax("s = \"\xC3\xA9\xC3\xA9\" x\n");
    REQUIRE(col);
    CHECK(col->col == 10);
}

// ---- tools that are missing, absent languages, empty trees -------------------

#ifndef _WIN32
TEST_CASE("polyglot: every language without its tools is NOTRUN with install, never quiet") {
    Tree t({{"a.js", "let x = 1;\n"}, {"b.ts", "let y: number = 1;\n"}, {"c.sh", "echo hi\n"},
            {"d.go", "package main\n"}, {"e.rb", "x = 1\n"}, {"f.php", "<?php echo 1;\n"},
            {"g.pl", "print 1;\n"}, {"h.lua", "print(1)\n"}, {"i.yml", "a: 1\n"},
            {"j.py", "x = 1\n"}, {"k/Cargo.toml", "[package]\n"}, {"k/src/main.rs", "fn main(){}\n"},
            {"l.json", "{}\n"}});
    std::vector<prism::Finding> out;
    {
        EnvGuard path("PATH", "/nonexistent-prism-path");
        out = t.run();
    }
    for (auto& c : pg::checks()) {
        auto rows = rows_of(out, c.group);
        REQUIRE_MESSAGE(!rows.empty(), c.group << " wrote nothing (quiet skip)");
        if (c.tools[0].native) {
            for (auto& r : rows) CHECK_MESSAGE(r.status != prism::laws::NOTRUN, c.group);
            continue;
        }
        for (auto& r : rows) {
            CHECK_MESSAGE(r.status == prism::laws::NOTRUN, c.group);
            CHECK_MESSAGE(!xo(r, "install").empty(), c.group);
            CHECK(r.status != prism::laws::CLEAN);
        }
    }
    auto py = rows_of(out, "python-syntax");
    CHECK(py[0].message == "python-syntax: python not found for 1 python file(s)");
    CHECK(xo(py[0], "install") == "install Python 3 (python3 on PATH)");
}

TEST_CASE("polyglot: a language that is absent writes nothing; an empty tree is UNKNOWN") {
    {
        Tree t({{"a.c", "int main(void){return 0;}\n"}});
        EnvGuard path("PATH", "/nonexistent-prism-path");
        for (auto& f : t.run()) CHECK(xo(f, "check").empty());
    }
    Tree e;
    auto out = e.run();
    REQUIRE(out.size() == 1);
    CHECK(out[0].status == prism::laws::UNKNOWN);
    CHECK(out[0].message == "no source files in scope");
}

// ---- Law 9: perl -c, cargo clippy and eslint run project code ---------------

TEST_CASE("polyglot: exec tools are NOTRUN without --allow-exec and run with it") {
    Tree t({{"a.pl", "print 1;\n"}, {"k/Cargo.toml", "[package]\n"}, {"k/src/main.rs", "fn main(){}\n"},
            {"a.js", "let x = 1;\n"}});
    Tree bin;
    auto log = bin.dir / "argv.log";
    std::map<std::string, fs::path> tools{{"perl", fake_tool(bin.dir, "perl", log)},
                                          {"cargo-clippy", fake_tool(bin.dir, "cargo", log)},
                                          {"eslint", fake_tool(bin.dir, "eslint", log)}};
    auto out = t.run(false, tools);
    for (auto [group, tool] : std::vector<std::pair<std::string, std::string>>{
             {"perl-syntax", "perl"}, {"rust-lint", "cargo-clippy"}, {"javascript-lint", "eslint"}}) {
        auto rows = rows_of(out, group);
        REQUIRE_MESSAGE(rows.size() == 1, group);
        CHECK(rows[0].status == prism::laws::NOTRUN);
        CHECK(xo(rows[0], "reason") == prism::sandbox::EXEC_REASON);
        CHECK(xo(rows[0], "install").find("--allow-exec") != std::string::npos);
        CHECK(rows[0].message == group + " (" + tool + "): executes code from the scanned tree; "
                                             "re-run with --allow-exec (only on code you trust)");
    }
    const auto held = slurp(log);
    CHECK(held.find("clippy") == std::string::npos);
    CHECK(held.find("unix") == std::string::npos);
    CHECK(held.find("-c ") == std::string::npos);

    out = t.run(true, tools);
    for (auto& f : out) CHECK(xo(f, "reason") != prism::sandbox::EXEC_REASON);
    const auto ran = slurp(log);
    CHECK(ran.find("clippy") != std::string::npos);
    CHECK(ran.find("--format unix") != std::string::npos);
    CHECK(ran.find("-c ") != std::string::npos);
    auto clippy = rows_of(out, "rust-lint");
    REQUIRE(clippy.size() == 1);
    CHECK(clippy[0].status == prism::laws::UNKNOWN);
}

// ---- tool resolution ------------------------------------------------------------

TEST_CASE("polyglot resolve: --tool first (with ~ expanded), then PATH; native rows need none") {
    Tree home;
    auto log = home.dir / "log";
    auto fake = fake_tool(home.dir, "my-ruff", log);
    EnvGuard h("HOME", home.dir.string());
    auto cfg = prism::default_config();
    cfg.tools["ruff"] = "~/my-ruff";
    auto got = pg::resolve(tool_named("ruff"), cfg);
    REQUIRE(got);
    CHECK(*got == fake);
    cfg.tools["ruff"] = "~/missing-ruff";
    {
        EnvGuard path("PATH", "/nonexistent-prism-path");
        CHECK_FALSE(pg::resolve(tool_named("ruff"), cfg));
        CHECK_FALSE(pg::resolve(tool_named("prism-json"), cfg));
    }
    // An explicit binary under an exe name counts too (--tool python3=...).
    cfg.tools.clear();
    cfg.tools["python3"] = fake;
    auto py = pg::resolve(tool_named("python"), cfg);
    REQUIRE(py);
    CHECK(*py == fake);
}

// ---- output that parses to nothing -----------------------------------------------

TEST_CASE("polyglot: unparsed output is ERROR, benign output is UNKNOWN, syntax rows parse") {
    Tree t({{"a.py", "x = 1\n"}});
    Tree bin;
    auto log = bin.dir / "log";
    auto rows_with = [&](const std::string& out, int rc) {
        auto fake = fake_tool(bin.dir, "ruff", log, out, rc);
        EnvGuard path("PATH", "/nonexistent-prism-path");
        return rows_of(t.run(false, {{"ruff", fake}}), "python-lint");
    };
    auto bad = rows_with("ruff: something new happened", 1);
    REQUIRE(bad.size() == 1);
    CHECK(bad[0].status == prism::laws::ERROR);
    CHECK(bad[0].message == "ruff: output not understood: ruff: something new happened");
    auto ok = rows_with("All checks passed!", 0);
    REQUIRE(ok.size() == 1);
    CHECK(ok[0].status == prism::laws::UNKNOWN);
    CHECK(ok[0].message.find("no diagnostics (not a proof)") != std::string::npos);
    // A crashed tool (exit outside ok_rcs) is ERROR.
    auto crash = rows_with("", 3);
    REQUIRE(crash.size() == 1);
    CHECK(crash[0].status == prism::laws::ERROR);
    CHECK(crash[0].message.rfind("ruff exit 3", 0) == 0);

    auto& ruby = tool_named("ruby");
    CHECK(pg::unexplained_output(ruby, "Syntax OK\n\n") == "");
    CHECK(pg::unexplained_output(ruby, "Syntax OK\nweird\n") == "weird");
    // A line the pattern matches but parse_output drops (a note) is understood.
    const pg::Check* sc_check = nullptr;
    auto& sc = tool_named("shellcheck", &sc_check);
    CHECK(pg::parse_output(sc, "/r/a.sh:2:6: note: quote it [SC2086]\n", testdata(), {}, *sc_check).empty());
    CHECK(pg::unexplained_output(sc, "/r/a.sh:2:6: note: quote it [SC2086]\n") == "");
    auto& py = tool_named("python");
    CHECK(pg::unexplained_output(py, "PRISM-OK\t/x/a.py\n") == "");
}
#endif

// ---- parsers: real output shapes without the tools ------------------------------

TEST_CASE("polyglot parsers: ruff, mypy, node, tsc") {
    auto ruff = parse("ruff", "/r/a.py:1:8: F401 [*] `os` imported but unused\n");
    REQUIRE(ruff.size() == 1);
    CHECK(ruff[0].file == "a.py");
    CHECK(ruff[0].line == 1);
    CHECK(xo(ruff[0], "rule") == "F401");
    CHECK(xo(ruff[0], "col") == "8");
    CHECK(ruff[0].message == "ruff: `os` imported but unused");
    CHECK(ruff[0].cls == "LANG-LINT");
    CHECK(xo(ruff[0], "check") == "python-lint");
    CHECK(xo(ruff[0], "language") == "python");

    // ruff >= 0.5 names syntax errors `invalid-syntax`; older prints SyntaxError.
    auto named = parse("ruff", "/r/b.py:1:7: invalid-syntax: Expected a parameter\n"
                               "/r/c.py:2:1: SyntaxError: Expected an expression\n");
    REQUIRE(named.size() == 2);
    CHECK(named[0].file == "b.py");
    CHECK(xo(named[0], "rule") == "invalid-syntax");
    CHECK(named[0].message == "ruff: Expected a parameter");
    CHECK(named[1].file == "c.py");
    CHECK(named[1].line == 2);
    CHECK(xo(named[1], "rule") == "SyntaxError");

    auto mypy = parse("mypy", "/r/a.py:2:10: error: Incompatible types  [assignment]\n"
                              "/r/a.py:2:10: note: see docs\n");
    REQUIRE(mypy.size() == 1);
    CHECK(mypy[0].cls == "TYPE-ERROR");
    CHECK(xo(mypy[0], "rule") == "assignment");

    auto node = parse("node", "/r/a.js:3\n  let = ;\n      ^\n\nSyntaxError: Unexpected token ';'\n");
    REQUIRE(node.size() == 1);
    CHECK(node[0].file == "a.js");
    CHECK(node[0].line == 3);
    CHECK(node[0].message.find("SyntaxError") != std::string::npos);
    CHECK(node[0].cls == "SYNTAX-ERROR");

    auto tsc = parse("tsc", "/r/t.ts(1,7): error TS2322: Type 'string' is not assignable.\n");
    REQUIRE(tsc.size() == 1);
    CHECK(tsc[0].line == 1);
    CHECK(xo(tsc[0], "rule") == "TS2322");
    CHECK(tsc[0].cls == "TYPE-ERROR");

    auto py = parse("python", "PRISM-SYNTAX\t/r/p.py\t1\t7\tSyntaxError: invalid syntax\nPRISM-OK\t/r/q.py\n");
    REQUIRE(py.size() == 1);
    CHECK(py[0].file == "p.py");
    CHECK(py[0].cls == "SYNTAX-ERROR");
    CHECK(xo(py[0], "col") == "7");
}

TEST_CASE("polyglot parsers: bash, php, perl, ruby") {
    auto bash = parse("bash", "/r/a.sh: line 4: syntax error near `fi'\n");
    REQUIRE(bash.size() == 1);
    CHECK(bash[0].line == 4);
    auto php = parse("php", "PHP Parse error:  syntax error, unexpected ';' in /r/a.php on line 2\n");
    REQUIRE(php.size() == 1);
    CHECK(php[0].file == "a.php");
    CHECK(php[0].line == 2);
    auto perl = parse("perl", "syntax error at /r/a.pl line 1, near \"= ;\"\n");
    REQUIRE(perl.size() == 1);
    CHECK(perl[0].file == "a.pl");
    CHECK(perl[0].line == 1);
    auto rb = parse("ruby", "/usr/bin/ruby: /r/a.rb:1: syntax error, unexpected end-of-input\n");
    REQUIRE(rb.size() == 1);
    CHECK(rb[0].file == "a.rb");
    CHECK(rb[0].line == 1);
    CHECK(rb[0].cls == "SYNTAX-ERROR");
    auto warn = parse("ruby", "/r/a.rb:3: warning: assigned but unused variable - y\n");
    REQUIRE(warn.size() == 1);
    CHECK(warn[0].cls == "LANG-LINT");  // a warning from a syntax check is a lint
}

TEST_CASE("polyglot parsers: shellcheck, gofmt, cargo clippy (cwd-relative), yamllint, eslint config") {
    auto sc = parse("shellcheck", "/r/a.sh:2:6: warning: foo is unused [SC2034]\n");
    REQUIRE(sc.size() == 1);
    CHECK(sc[0].line == 2);
    CHECK(xo(sc[0], "severity") == "warning");
    auto go = parse("gofmt", "/r/a.go:2:11: expected ')', found '{'\n");
    REQUIRE(go.size() == 1);
    CHECK(go[0].file == "a.go");
    CHECK(go[0].cls == "SYNTAX-ERROR");
    const pg::Check* cc = nullptr;
    auto& clippy = tool_named("cargo-clippy", &cc);
    auto cl = pg::parse_output(clippy, "src/main.rs:1:17: warning: unused variable: `x`\n", testdata(),
                               testdata() / "k", *cc);
    REQUIRE(cl.size() == 1);
    CHECK(cl[0].file == "k/src/main.rs");
    auto ym = parse("yamllint", "/r/a.yml:1:9: [error] syntax error: expected ',' (syntax)\n");
    REQUIRE(ym.size() == 1);
    CHECK(ym[0].line == 1);
    CHECK(xo(ym[0], "rule") == "syntax");
    auto& eslint = tool_named("eslint");
    CHECK(prism::Regex("(?i)" + eslint.unconfigured)
              .search("ESLint couldn't find an eslint.config.(js|mjs|cjs) file."));
}

// ---- real tools on this machine ---------------------------------------------------
// A well-formed file must come back UNKNOWN (ran, nothing to say), never ERROR
// "output not understood" (a benign line missing from Tool.benign); a broken
// file must be FAILED with a file (the regex matches). Tools not installed
// here are covered by the NOTRUN case above.

TEST_CASE("polyglot: each installed tool understands its own output") {
    struct Real {
        const char* group;
        const char* exe;
        std::map<std::string, std::string> ok, bad;
    };
    const std::vector<Real> real = {
        {"python-syntax", "python3", {{"ok.py", "x = 1\n"}}, {{"bad.py", "def f(:\n    pass\n"}}},
        {"python-lint", "ruff", {{"ok.py", "x = 1\n"}}, {{"bad.py", "def f(:\n    pass\n"}}},
        {"python-types", "mypy", {{"ok.py", "x: int = 1\n"}}, {{"t.py", "x: int = \"s\"\n"}}},
        {"javascript-syntax", "node", {{"ok.js", "let x = 1;\n"}}, {{"bad.js", "let = ;\n"}}},
        {"typescript-types", "tsc", {{"ok.ts", "let y: number = 1;\n"}}, {{"bad.ts", "let y: number = 's';\n"}}},
        {"shell-syntax", "bash", {{"ok.sh", "echo hi\n"}}, {{"bad.sh", "if then\nfi\n"}}},
        {"shell-lint", "shellcheck", {{"ok.sh", "#!/bin/sh\necho hi\n"}}, {{"bad.sh", "#!/bin/sh\nx=1\n"}}},
        {"go-syntax", "gofmt", {{"ok.go", "package main\nfunc main(){}\n"}},
         {{"bad.go", "package main\nfunc main( {\n"}}},
        {"ruby-syntax", "ruby", {{"ok.rb", "puts 1\n"}}, {{"bad.rb", "def (\n"}}},
        {"php-syntax", "php", {{"ok.php", "<?php echo 1;\n"}}, {{"bad.php", "<?php echo ;\n"}}},
        {"perl-syntax", "perl", {{"ok.pl", "print 1;\n"}}, {{"bad.pl", "my $x = ;\n"}}},
        {"yaml-lint", "yamllint", {{"ok.yml", "---\na: 1\n"}}, {{"bad.yml", "---\na: [1\n"}}},
    };
    int ran = 0;
    for (auto& r : real) {
        if (!prism::default_config().which({r.exe})) continue;
        ++ran;
        CAPTURE(r.group);
        Tree good(r.ok);
        auto g = rows_of(good.run(true), r.group);
        REQUIRE(g.size() == 1);
        CHECK_MESSAGE(g[0].status == prism::laws::UNKNOWN, g[0].message);
        Tree broken(r.bad);
        auto b = rows_of(broken.run(true), r.group);
        bool failed_with_file = false;
        for (auto& f : b)
            if (f.status == prism::laws::FAILED && !f.file.empty()) failed_with_file = true;
        CHECK_MESSAGE(failed_with_file, (b.empty() ? std::string("no rows") : b[0].status + " " + b[0].message));
    }
    MESSAGE(ran << " polyglot tools installed here");
}

TEST_CASE("polyglot: a syntax error does not blind the type checker to other files") {
    if (!prism::default_config().which({"mypy"})) {
        MESSAGE("mypy not installed: the NOTRUN case covers python-types");
        return;
    }
    Tree t({{"bad.py", "def f(:\n    pass\n"}, {"typed.py", "x: int = \"s\"\n"}});
    auto out = t.run();
    std::vector<std::string> types;
    std::set<std::string> syntax;
    for (auto& f : out) {
        if (f.cls == "TYPE-ERROR") types.push_back(f.file);
        if (f.cls == "SYNTAX-ERROR") syntax.insert(f.file);
    }
    CHECK(types == std::vector<std::string>{"typed.py"});
    CHECK(syntax.contains("bad.py"));
}
