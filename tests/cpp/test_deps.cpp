// Supply chain (roadmap Part 1): third_party/MANIFEST.toml, prism-deps
// (fetch/verify/build, linked-tree digests, licence firewall, SBOM), tool
// SHA in findings, and the CI / Docker / notice files that run them.
#include "doctest/doctest.h"
#include "nlohmann/json.hpp"
#include "prism/config.hpp"
#include "prism/deps.hpp"
#include "prism/models.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <random>
#include <vector>

namespace {
namespace fs = std::filesystem;
namespace deps = prism::deps;

// __FILE__ is relative to the build directory under Ninja; also accept a
// run from the repository root (./build/prism_tests) or from build/.
fs::path repo_root() {
    const fs::path from_file = fs::path(__FILE__).parent_path().parent_path().parent_path();
    std::error_code ec;
    if (fs::exists(from_file / "third_party" / "MANIFEST.toml", ec)) return fs::absolute(from_file);
    for (fs::path d = fs::current_path(); !d.empty(); d = d.parent_path()) {
        if (fs::exists(d / "tests" / "cpp" / "test_deps.cpp", ec)) return d;
        if (d == d.parent_path()) break;
    }
    return from_file;
}

fs::path manifest_path() { return repo_root() / "third_party" / "MANIFEST.toml"; }

std::string slurp(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

void spit(const fs::path& p, const std::string& text) {
    fs::create_directories(p.parent_path());
    std::ofstream o(p, std::ios::binary | std::ios::trunc);
    o << text;
}

bool contains(const std::string& hay, std::string_view needle) { return hay.find(needle) != std::string::npos; }

struct TempDir {
    fs::path path;
    TempDir() {
        static int n = 0;
        static const auto run = std::random_device{}();
        path = fs::temp_directory_path() /
               ("prism-deps-test-" + std::to_string(run) + "-" + std::to_string(++n));
        fs::remove_all(path);
        fs::create_directories(path);
    }
    ~TempDir() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
};

// Swallows what prism-deps prints to std::cout / std::cerr in a test.
struct Quiet {
    std::ostringstream sink;
    std::streambuf* out = std::cout.rdbuf(sink.rdbuf());
    std::streambuf* err = std::cerr.rdbuf(sink.rdbuf());
    ~Quiet() {
        std::cout.rdbuf(out);
        std::cerr.rdbuf(err);
    }
};

// The CLI, fetcher and linked check with their progress output swallowed
// (doctest reports through std::cout, so only around the call itself).
int cli(const std::vector<std::string>& args) {
    Quiet q;
    return deps::deps_main(args);
}
fs::path quiet_fetch(const deps::Table& comp, const fs::path& base) {
    Quiet q;
    return deps::fetch_tool(comp, base);
}
int quiet_linked(const deps::Manifest& m) {
    Quiet q;
    return deps::run_linked(m, false);
}

// Scoped environment variable.
struct EnvGuard {
    std::string key, was;
    bool had = false;
    EnvGuard(std::string k, const std::string& value) : key(std::move(k)) {
        if (const char* v = std::getenv(key.c_str())) {
            had = true;
            was = v;
        }
        set(value);
    }
    ~EnvGuard() {
        if (had) set(was);
        else unset();
    }
    void set(const std::string& v) const {
#ifdef _WIN32
        _putenv_s(key.c_str(), v.c_str());
#else
        ::setenv(key.c_str(), v.c_str(), 1);
#endif
    }
    void unset() const {
#ifdef _WIN32
        _putenv_s(key.c_str(), "");
#else
        ::unsetenv(key.c_str());
#endif
    }
};

const deps::Table& row(const deps::Manifest& m, const std::string& name) {
    for (const auto& c : m.components())
        if (c.str("name") == name) return c;
    FAIL("no component " << name);
    throw;
}

const std::vector<std::string> kLinked = {"z3", "pcre2", "xsimd", "nlohmann_json", "doctest", "llama.cpp"};
const std::vector<std::string> kMined = {"AFLplusplus", "Frama-C", "FuSeBMC", "Fuzz4All", "cbmc",
                                         "coccinelle", "codeql", "cppcheck", "dafny", "esbmc",
                                         "infer", "klee", "rapidcheck", "semgrep", "strix"};

// ---- git upstream helpers ----------------------------------------------

std::string git(const std::vector<std::string>& args, const fs::path& cwd) {
    std::vector<std::string> argv{"git", "-c", "user.name=t", "-c", "user.email=t@t", "-c",
                                  "commit.gpgsign=false", "-c", "init.defaultBranch=main"};
    argv.insert(argv.end(), args.begin(), args.end());
    auto r = deps::run_process(argv, cwd);
    REQUIRE_MESSAGE(r.code == 0, "git " << args[0] << ": " << r.err);
    auto s = r.out;
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
    return s;
}

// A tiny upstream: returns (url, commit).
std::pair<std::string, std::string> local_tool_repo(const fs::path& td, const std::string* configure = nullptr) {
    auto repo = td / "upstream";
    fs::create_directories(repo);
    git({"init", "-q"}, repo);
    spit(repo / "README", "tool\n");
    if (configure) {
        spit(repo / "configure", *configure);
        fs::permissions(repo / "configure", fs::perms::owner_all | fs::perms::group_read | fs::perms::others_read);
        spit(repo / "makefile", "all:\n\t@true\n");
    }
    git({"add", "-A"}, repo);
    git({"commit", "-q", "-m", "x"}, repo);
    // uploadpack.allowReachableSHA1InWant lets `git fetch <url> <sha>` work locally.
    git({"config", "uploadpack.allowReachableSHA1InWant", "true"}, repo);
    return {"file://" + fs::absolute(repo).string(), git({"rev-parse", "HEAD"}, repo)};
}

deps::Table demo_row(const std::string& url, const std::string& commit, const std::string& sha) {
    deps::Table t;
    t.set("name", std::string("demo"))
        .set("kind", std::string("external"))
        .set("version", std::string("1"))
        .set("url", url)
        .set("commit", commit)
        .set("archive_sha256", sha)
        .set("spdx", std::string("MIT"))
        .set("bins", std::vector<std::string>{"demo"});
    deps::validate_component(t);
    return t;
}

// The archive hash computed independently of fetch_archive; also checks
// that a mismatching archive is refused and deleted.
std::string real_sha(const std::string& url, const std::string& commit) {
    TempDir td;
    auto tar = td.path / "a.tar";
    CHECK_THROWS_AS(deps::fetch_archive(demo_row(url, commit, std::string(64, '0')), tar), deps::HashMismatch);
    CHECK_MESSAGE(!fs::exists(tar), "a mismatching archive must be deleted");
    auto repo = td.path / "r";
    fs::create_directories(repo);
    git({"init", "-q", "--bare"}, repo);
    git({"fetch", "-q", "--depth", "1", url, commit}, repo);
    auto out = td.path / "b.tar";
    auto r = deps::run_process({"git", "archive", "--format=tar", "--prefix=demo-" + commit + "/", commit}, repo,
                               true, &out);
    REQUIRE(r.code == 0);
    return deps::sha256_file(out);
}

// ---- hand-made tar members for the extraction rules --------------------

void tar_member(std::string& out, const std::string& name, char type, const std::string& data = {},
                const std::string& link = {}, unsigned mode = 0644) {
    char h[512] = {};
    std::snprintf(h, 100, "%s", name.c_str());
    std::snprintf(h + 100, 8, "%07o", mode);
    std::snprintf(h + 108, 8, "%07o", 0);
    std::snprintf(h + 116, 8, "%07o", 0);
    std::snprintf(h + 124, 12, "%011o", static_cast<unsigned>(data.size()));
    std::snprintf(h + 136, 12, "%011o", 0);
    std::memset(h + 148, ' ', 8);
    h[156] = type;
    std::snprintf(h + 157, 100, "%s", link.c_str());
    std::memcpy(h + 257, "ustar\0" "00", 8);
    unsigned sum = 0;
    for (unsigned char c : h) sum += c;
    std::snprintf(h + 148, 8, "%06o", sum);
    h[155] = ' ';
    out.append(h, 512);
    out += data;
    out.append((512 - data.size() % 512) % 512, '\0');
}

std::string tar_end() { return std::string(1024, '\0'); }

}  // namespace

// ---- manifest ------------------------------------------------------------

TEST_CASE("deps: every manifest component is well formed") {
    auto m = deps::load_manifest(manifest_path());  // validates every row
    REQUIRE(m.components().size() > 20);
    for (const auto& c : m.components()) CHECK_NOTHROW(deps::validate_component(c));
    CHECK(m.root == fs::weakly_canonical(repo_root()));
}

TEST_CASE("deps: six linked libraries with tree digests") {
    auto m = deps::load_manifest(manifest_path());
    std::vector<std::string> linked;
    for (auto* c : deps::components(m, "linked")) linked.push_back(c->str("name"));
    auto want = kLinked;
    std::sort(linked.begin(), linked.end());
    std::sort(want.begin(), want.end());
    CHECK(linked == want);
    static const std::regex hex64("^[0-9a-f]{64}$");
    for (const auto& name : kLinked) {
        const auto& r = row(m, name);
        CHECK_MESSAGE(fs::is_directory(repo_root() / r.str("path")), name);
        CHECK_MESSAGE(std::regex_match(r.str("tree_sha256"), hex64), name);
        CHECK_MESSAGE(r.truthy("tag"), name);
    }
}

TEST_CASE("deps: external tools invoked by prism are pinned") {
    auto m = deps::load_manifest(manifest_path());
    std::set<std::string> want = {"esbmc", "cbmc", "klee", "cppcheck", "frama-c", "infer", "semgrep",
                                  "coccinelle", "aflplusplus", "dafny", "strix", "cadical", "kissat",
                                  "cake_lpr", "bitwuzla",
                                  // proof re-checkers run by CI (roadmap 8.5), not by the prism binary
                                  "lean4export", "nanoda"};
    std::set<std::string> external;
    for (auto* c : deps::components(m, "external")) external.insert(c->str("name"));
    CHECK(external == want);
    CHECK(row(m, "clang-tidy").str("kind") == "system");
    for (const char* name : {"cadical", "kissat", "cake_lpr", "bitwuzla", "lean4export", "nanoda"})
        CHECK_MESSAGE(row(m, name).truthy("recipe"), std::string(name) << " needs a build recipe");
    // A stage name finds its component too.
    CHECK(deps::find_component(m, "spatch").str("name") == "coccinelle");
    CHECK(deps::find_component(m, "afl-fuzz").str("name") == "aflplusplus");
    CHECK_THROWS_AS(deps::find_component(m, "no-such-tool"), deps::FetchError);
}

TEST_CASE("deps: the mined source trees are gone") {
    const auto root = repo_root();
    for (const auto& name : kMined) CHECK_MESSAGE(!fs::exists(root / "third_party" / name), name);
    if (deps::which("git")) {
        auto r = deps::run_process({"git", "ls-files", "third_party"}, root);
        std::istringstream lines(r.out);
        std::string line;
        while (std::getline(lines, line)) {
            auto a = line.find('/');
            if (a == std::string::npos) continue;
            auto b = line.find('/', a + 1);
            auto top = line.substr(a + 1, b == std::string::npos ? std::string::npos : b - a - 1);
            CHECK_MESSAGE(std::find(kMined.begin(), kMined.end(), top) == kMined.end(), line);
        }
    }
    CHECK_FALSE(fs::exists(root / "third_party" / "SOURCES.md"));
    CHECK_FALSE(fs::exists(root / "third_party" / "vendor.log"));
    // The provenance stays recorded in the manifest.
    CHECK(contains(slurp(manifest_path()), "vendor.log recorded clone URLs and timestamps"));
}

TEST_CASE("deps: linked trees match the manifest offline") {
    auto m = deps::load_manifest(manifest_path());
    for (const auto& name : kLinked) {
        auto errs = deps::check_linked(row(m, name), m.root);
        CHECK_MESSAGE(errs.empty(), name << ": " << (errs.empty() ? "" : errs[0]));
    }
    CHECK(quiet_linked(m) == 0);
}

TEST_CASE("deps: a modified linked tree fails closed") {
    auto m = deps::load_manifest(manifest_path());
    deps::Table r = row(m, "nlohmann_json");
    TempDir td;
    auto dst = td.path / r.str("path");
    fs::create_directories(dst.parent_path());
    fs::copy(repo_root() / r.str("path"), dst, fs::copy_options::recursive);
    CHECK(deps::check_linked(r, td.path).empty());
    // A stray .pyc / __pycache__ / .gguf is not part of the tree.
    spit(dst / "__pycache__" / "x.pyc", "junk");
    spit(dst / "vocab.gguf", "junk");
    CHECK(deps::check_linked(r, td.path).empty());
    {
        std::ofstream o(dst / "json.hpp", std::ios::app);
        o << "\n// tampered\n";
    }
    auto errs = deps::check_linked(r, td.path);
    CHECK(std::any_of(errs.begin(), errs.end(), [](auto& e) { return contains(e, "tree_sha256 mismatch"); }));
    deps::Table bumped = r;
    bumped.set("tree_version", std::string("9.9.9"));
    errs = deps::check_linked(bumped, td.path);
    CHECK(std::any_of(errs.begin(), errs.end(), [](auto& e) {
        return contains(e, "in-tree version '3.11.3' != manifest '9.9.9'");
    }));
    deps::Table undigested = r;
    undigested.erase("tree_sha256");
    errs = deps::check_linked(undigested, td.path);
    CHECK(std::any_of(errs.begin(), errs.end(), [](auto& e) { return contains(e, "no tree_sha256"); }));
    fs::remove_all(dst);
    errs = deps::check_linked(r, td.path);
    REQUIRE(errs.size() == 1);
    CHECK(errs[0] == "nlohmann_json: third_party/nlohmann missing");
}

TEST_CASE("deps: drift against the upstream tree is D / A / M, sorted by path") {
    TempDir td;
    spit(td.path / "ours" / "same.txt", "x");
    spit(td.path / "ours" / "changed.txt", "ours");
    spit(td.path / "ours" / "added.txt", "a");
    spit(td.path / "up" / "sub" / "same.txt", "x");
    spit(td.path / "up" / "sub" / "changed.txt", "theirs");
    spit(td.path / "up" / "sub" / "dropped.txt", "d");
    spit(td.path / "up" / "other.txt", "o");
    deps::Table c;
    c.set("name", std::string("t")).set("path", std::string("ours")).set("upstream_subdir", std::string("sub"));
    CHECK(deps::diff_against_upstream(c, td.path / "up", td.path) ==
          std::vector<std::string>{"A added.txt", "M changed.txt", "D dropped.txt"});
    c.set("upstream_files", std::vector<std::string>{"same.txt", "changed.txt"});
    CHECK(deps::diff_against_upstream(c, td.path / "up", td.path) == std::vector<std::string>{"M changed.txt"});
    std::vector<std::string> d = {"M b", "D a", "A b"};
    deps::sort_drift(d);
    CHECK(d == std::vector<std::string>{"D a", "A b", "M b"});
}

TEST_CASE("deps: tree digest is sha256 over sorted relpath NUL sha LF lines") {
    TempDir td;
    spit(td.path / "b" / "x", "2");
    spit(td.path / "a", "1");
    spit(td.path / ".git" / "HEAD", "ignored");
    std::string lines = "a" + std::string(1, '\0') + deps::sha256_hex("1") + "\n" + "b/x" + std::string(1, '\0') +
                        deps::sha256_hex("2") + "\n";
    CHECK(deps::tree_digest(td.path) == deps::sha256_hex(lines));
    CHECK(cli({"tree-digest", td.path.string()}) == 0);
    CHECK(cli({"tree-digest", (td.path / "missing").string()}) == 2);
}

TEST_CASE("deps: CMake builds the manifest pins with prism-deps") {
    auto cmake = slurp(repo_root() / "CMakeLists.txt");
    CHECK(contains(cmake, "third_party/MANIFEST.toml"));
    CHECK(contains(cmake, "manifest_pins.hpp"));
    CHECK(contains(cmake, "COMMAND prism-deps pins"));
    CHECK(contains(cmake, "CMAKE_CONFIGURE_DEPENDS"));
    CHECK(contains(cmake, "PRISM_DEPS_ONLY"));
    for (const char* bad : {"OneDrive", "/mnt/", "CMAKE_SUPPRESS_REGENERATION", "mined"})
        CHECK_MESSAGE(!contains(cmake, bad), std::string(bad));
    // The pins header prism-deps writes.
    TempDir td;
    auto out = td.path / "prism" / "manifest_pins.hpp";
    REQUIRE(cli({"pins", "--manifest", manifest_path().string(), "-o", out.string()}) == 0);
    auto hdr = slurp(out);
    CHECK(contains(hdr, "{\"esbmc\", \"" + *prism::pinned_commit("esbmc") + "\"},"));
    CHECK_FALSE(contains(hdr, "\"z3\""));
    CHECK(cli({"pins", "--manifest", manifest_path().string()}) == 2);  // needs -o
}

TEST_CASE("deps: the adapter stage table maps to pinned manifest components") {
    auto m = deps::load_manifest(manifest_path());
    auto cpp = slurp(repo_root() / "src" / "prism" / "config.cpp");
    auto a = cpp.find("static const char* vendor_dir_for(");
    auto b = cpp.find("std::string adapter_install(");
    REQUIRE(a != std::string::npos);
    REQUIRE(b != std::string::npos);
    auto body = cpp.substr(a, b - a);
    static const std::regex pair_re(R"re(\{"([^"]+)", "([^"]+)"\})re");
    int n = 0;
    for (std::sregex_iterator it(body.begin(), body.end(), pair_re), end; it != end; ++it, ++n) {
        const std::string stage = (*it)[1], comp = (*it)[2];
        const auto& r = row(m, comp);
        CHECK_MESSAGE(r.str("kind") == "external", stage);
        auto pin = prism::pinned_commit(comp);
        REQUIRE_MESSAGE(pin.has_value(), stage);
        CHECK_MESSAGE(*pin == r.str("commit"), stage);
        CHECK(prism::adapter_install(stage) == "prism-deps tool " + comp + " (pinned in third_party/MANIFEST.toml)");
    }
    CHECK(n >= 14);
    // Every external row is baked in; nothing else is.
    for (const auto& c : m.components()) {
        auto pin = prism::pinned_commit(c.str("name"));
        if (c.str("kind") == "external") CHECK_MESSAGE((pin && *pin == c.str("commit")), c.str("name"));
        else CHECK_MESSAGE(!pin, c.str("name"));
    }
}

// ---- TOML subset ---------------------------------------------------------

TEST_CASE("deps: TOML reader covers the manifest subset") {
    auto d = deps::parse_toml(
        "# comment\n"
        "schema = 1\n"
        "neg = -1_000\n"
        "flag = true\n"
        "\n"
        "[[component]]\n"
        "name = \"a\\\"b\\\\c\\u00e9\"  # trailing comment\n"
        "rx = 'x\\(\\d+\\)'\n"
        "empty = []\n"
        "one = [\"x\"]\n"
        "multi = [\n"
        "  \"D a\",  # why\n"
        "  'M b',\n"
        "]\n"
        "\r\n"
        "[[component]]\n"
        "name = \"second\"\n"
        "[[mined]]\n"
        "name = \"m\"\n");
    CHECK(d.root.str("schema") == "1");
    CHECK(std::get<std::int64_t>(*d.root.find("neg")) == -1000);
    CHECK(std::get<bool>(*d.root.find("flag")));
    const auto& comps = d.array("component");
    REQUIRE(comps.size() == 2);
    CHECK(comps[0].str("name") == "a\"b\\c\xc3\xa9");
    CHECK(comps[0].str("rx") == "x\\(\\d+\\)");
    CHECK(comps[0].has("empty"));
    CHECK_FALSE(comps[0].truthy("empty"));
    CHECK(comps[0].list("one") == std::vector<std::string>{"x"});
    CHECK(comps[0].list("multi") == std::vector<std::string>{"D a", "M b"});
    CHECK(comps[1].str("name") == "second");
    CHECK(d.array("mined").size() == 1);
    CHECK(d.array("absent").empty());
}

TEST_CASE("deps: TOML reader fails closed outside the subset") {
    for (const char* bad : {
             "[component]\nname = \"a\"\n",       // plain table
             "a.b = 1\n",                          // dotted key
             "a = 1.5\n",                          // float
             "a = 1979-05-27\n",                   // date
             "a = { b = 1 }\n",                    // inline table
             "a = \"\"\"x\"\"\"\n",                // multi-line string
             "a = [1, 2]\n",                       // non-string array
             "a = \"x\"\na = \"y\"\n",             // duplicate key
             "a = \"unterminated\n",               // unterminated string
             "a = \"x\" b\n",                      // text after value
             "a = \"\\q\"\n",                      // unknown escape
             "a = [\"x\"\n",                       // unterminated array
             "a = 0x10\n",                         // hex integer
             "= 1\n",                              // missing key
             "a\n",                                // missing '='
         })
        CHECK_THROWS_AS_MESSAGE(deps::parse_toml(bad), deps::TomlError, std::string(bad));
    TempDir td;
    spit(td.path / "third_party" / "MANIFEST.toml", "[[component]]\nname = 'x'\nkind = 1.0\n");
    CHECK(cli({"list", "--manifest", (td.path / "third_party" / "MANIFEST.toml").string()}) == 2);
}

TEST_CASE("deps: bad manifest rows are rejected") {
    deps::Table base;
    base.set("name", std::string("x"))
        .set("kind", std::string("external"))
        .set("version", std::string("1"))
        .set("url", std::string("u"))
        .set("spdx", std::string("MIT"))
        .set("commit", std::string(40, 'a'))
        .set("archive_sha256", std::string(64, 'b'));
    CHECK_NOTHROW(deps::validate_component(base));
    auto with = [&](const char* k, deps::Value v) {
        deps::Table t = base;
        t.set(k, std::move(v));
        return t;
    };
    for (const auto& bad : {with("commit", std::string("abc")), with("commit", std::string(40, 'A')),
                            with("archive_sha256", std::string("zz")), with("kind", std::string("vendored")),
                            with("spdx", std::string("")), with("name", std::string("")),
                            with("url", std::string(""))})
        CHECK_THROWS_AS(deps::validate_component(bad), deps::FetchError);
    deps::Table linked = with("kind", std::string("linked"));
    CHECK_THROWS_WITH_AS(deps::validate_component(linked), "manifest: x: linked component needs path",
                         deps::FetchError);
    // A system tool needs no commit.
    deps::Table sys = with("kind", std::string("system"));
    sys.erase("commit").erase("archive_sha256");
    CHECK_NOTHROW(deps::validate_component(sys));
    // Duplicate names are refused by load_manifest.
    TempDir td;
    auto man = td.path / "third_party" / "MANIFEST.toml";
    std::string rowtxt = "[[component]]\nname = \"x\"\nkind = \"system\"\nversion = \"1\"\nurl = \"u\"\nspdx = \"MIT\"\n";
    spit(man, rowtxt + rowtxt);
    CHECK_THROWS_WITH_AS(deps::load_manifest(man), "manifest: duplicate component 'x'", deps::FetchError);
}

// ---- version markers -----------------------------------------------------

TEST_CASE("deps: tree_version_regex is read with Python re.S semantics") {
    CHECK(deps::py_regex_to_ecma(R"(A (\d+).*?B)") == R"(A (\d+)[\s\S]*?B)");
    CHECK(deps::py_regex_to_ecma(R"([.]x\.)") == R"([.]x\.)");
    for (const char* bad : {"(?P<v>\\d+)", "(?i)x", "x$", "\\Ax", "(a)\\1"})
        CHECK_THROWS_AS_MESSAGE(deps::py_regex_to_ecma(bad), deps::FetchError, std::string(bad));
    TempDir td;
    spit(td.path / "lib" / "v.h", "#define MAJ 3\n// x\n#define MIN 11\n#define PAT 4\n");
    deps::Table c;
    c.set("name", std::string("lib"))
        .set("path", std::string("lib"))
        .set("tree_version_file", std::string("v.h"))
        .set("tree_version_regex", std::string(R"(MAJ (\d+).*?MIN (\d+)\s*#define PAT (\d+))"));
    CHECK(deps::tree_version(c, td.path) == std::optional<std::string>("3.11.4"));
    c.set("tree_version_regex", std::string(R"(MAJ (\d+)(x)?)"));  // unmatched group skipped
    CHECK(deps::tree_version(c, td.path) == std::optional<std::string>("3"));
    c.set("tree_version_regex", std::string("NOPE (\\d+)"));
    CHECK(deps::tree_version(c, td.path) == std::optional<std::string>(""));
    c.erase("tree_version_regex");
    CHECK_FALSE(deps::tree_version(c, td.path).has_value());
}

// ---- hashing -------------------------------------------------------------

TEST_CASE("deps: SHA-256, SHA-1 and UUIDv5 test vectors") {
    CHECK(deps::sha256_hex("abc") == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    CHECK(deps::sha256_hex(std::string(1000000, 'a')) ==
          "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
    auto hex = [](const std::string& b) {
        std::string out;
        for (unsigned char c : b) {
            const char* d = "0123456789abcdef";
            out += d[c >> 4];
            out += d[c & 15];
        }
        return out;
    };
    CHECK(hex(deps::sha1_bytes("abc")) == "a9993e364706816aba3e25717850c26c9cd0d89d");
    CHECK(hex(deps::sha1_bytes("")) == "da39a3ee5e6b4b0d3255bfef95601890afd80709");
    CHECK(hex(deps::sha1_bytes("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq")) ==
          "84983e441c3bd26ebaae4aa1f95129e5e54670f1");
    // uuid.uuid5(uuid.NAMESPACE_DNS, "python.org")
    CHECK(deps::uuid5("6ba7b810-9dad-11d1-80b4-00c04fd430c8", "python.org") ==
          "886313e1-3b8a-5372-9b90-0c9aee199e5d");
    TempDir td;
    spit(td.path / "f", "abc");
    CHECK(deps::sha256_file(td.path / "f") == deps::sha256_hex("abc"));
    CHECK_THROWS_AS(deps::sha256_file(td.path / "missing"), deps::FetchError);
}

#ifndef _WIN32  // POSIX modes, sh recipes and file:// upstreams
// ---- tar extraction ------------------------------------------------------

TEST_CASE("deps: tar extraction keeps modes and refuses escapes") {
    TempDir td;
    auto write_tar = [&](const std::string& bytes) {
        auto p = td.path / "t.tar";
        spit(p, bytes);
        return p;
    };
    std::string good;
    tar_member(good, "pax_global_header", 'g', "52 comment=0000000000000000000000000000000000000000\n");
    tar_member(good, "top/", '5');
    tar_member(good, "top/run.sh", '0', "#!/bin/sh\n", {}, 0775);
    tar_member(good, "top/sub/data.txt", '0', "data\n", {}, 0666);
    tar_member(good, "top/link", '2', {}, "sub/data.txt");
    good += tar_end();
    auto top = deps::extract_tar(write_tar(good), td.path / "ok");
    CHECK(top.filename() == "top");
    CHECK(slurp(top / "sub" / "data.txt") == "data\n");
    CHECK(slurp(top / "link") == "data\n");
    auto perms = fs::status(top / "run.sh").permissions();
    CHECK((perms & fs::perms::owner_exec) != fs::perms::none);
    CHECK((perms & fs::perms::others_write) == fs::perms::none);
    CHECK((fs::status(top / "sub" / "data.txt").permissions() & fs::perms::group_write) == fs::perms::none);

    auto refuses = [&](const std::string& why, std::string bytes) {
        bytes += tar_end();
        auto dest = td.path / ("bad-" + std::to_string(std::hash<std::string>{}(why)));
        CHECK_THROWS_AS_MESSAGE(deps::extract_tar(write_tar(bytes), dest), deps::FetchError, why);
        CHECK_MESSAGE(!fs::exists(td.path / "escaped"), why);
    };
    std::string t;
    tar_member(t, "top/../../escaped", '0', "x");
    refuses("dot-dot path", t);
    t.clear();
    tar_member(t, "/tmp/escaped-abs", '0', "x");
    refuses("absolute path", t);
    t.clear();
    tar_member(t, "top/l", '2', {}, "../../escaped");
    refuses("symlink out", t);
    t.clear();
    tar_member(t, "top/l", '2', {}, "/etc/passwd");
    refuses("absolute symlink", t);
    t.clear();
    tar_member(t, "top/", '5');
    tar_member(t, "top/dot", '2', {}, ".");
    tar_member(t, "top/dot/l", '2', {}, "../../escaped");
    refuses("symlink out through a link", t);
    t.clear();
    tar_member(t, "top/dev", '3');
    refuses("device node", t);
    t.clear();
    tar_member(t, "a/x", '0', "x");
    tar_member(t, "b/y", '0', "y");
    refuses("two top-level directories", t);
    t.clear();
    tar_member(t, "top/x", '0', "x");
    t[148] = '7';  // corrupt the checksum
    refuses("bad checksum", t);
    t.clear();
    tar_member(t, "top/x", '0', std::string(600, 'x'));
    t.resize(900);
    refuses("truncated member", t);
}

// ---- fetch / build (local git upstream) ----------------------------------

TEST_CASE("deps: a tampered archive hash fails closed and installs nothing") {
    if (!deps::which("git")) {
        MESSAGE("NOTRUN: git not on PATH");
        return;
    }
    TempDir td;
    auto [url, commit] = local_tool_repo(td.path);
    auto good = real_sha(url, commit);
    auto tampered = std::string(1, good[0] != 'f' ? 'f' : 'e') + good.substr(1);
    auto tools = td.path / "tools";
    try {
        quiet_fetch(demo_row(url, commit, tampered), tools);
        FAIL("fetch_tool accepted a tampered hash");
    } catch (const deps::HashMismatch& e) {
        CHECK(contains(e.what(), "fail closed"));
        CHECK(contains(e.what(), "expected " + tampered));
    }
    CHECK_FALSE(fs::exists(tools / "demo" / commit));
    if (fs::exists(tools / "demo"))
        CHECK_MESSAGE(fs::is_empty(tools / "demo"), "no staging dir may survive a failed fetch");
    // CLI: exit code 3 on a hash mismatch.
    auto man = td.path / "third_party" / "MANIFEST.toml";
    spit(man, "[[component]]\nname = \"demo\"\nkind = \"external\"\nversion = \"1\"\nurl = \"" + url +
                  "\"\ncommit = \"" + commit + "\"\narchive_sha256 = \"" + tampered + "\"\nspdx = \"MIT\"\n");
    CHECK(cli({"tool", "demo", "--manifest", man.string(), "--tools-dir", tools.string()}) == 3);
    CHECK_FALSE(fs::exists(tools / "demo" / commit));
    // An unknown name is exit 1, a missing name is usage (2).
    CHECK(cli({"tool", "nothere", "--manifest", man.string(), "--tools-dir", tools.string()}) == 1);
    CHECK(cli({"tool", "--manifest", man.string()}) == 2);
}

TEST_CASE("deps: a good hash installs the verified source and a stamp") {
    if (!deps::which("git")) {
        MESSAGE("NOTRUN: git not on PATH");
        return;
    }
    TempDir td;
    auto [url, commit] = local_tool_repo(td.path);
    auto good = real_sha(url, commit);
    auto tools = td.path / "tools";
    auto final_dir = quiet_fetch(demo_row(url, commit, good), tools);
    CHECK(final_dir == tools / "demo" / commit);
    CHECK(fs::is_regular_file(final_dir / "src" / "README"));
    auto stamp = nlohmann::ordered_json::parse(slurp(final_dir / deps::kStamp));
    CHECK(stamp["commit"] == commit);
    CHECK(stamp["archive_sha256"] == good);
    CHECK(stamp["built"] == false);  // no recipe: source only, never guessed
    CHECK(stamp["bins"].empty());
    std::vector<std::string> keys;
    for (auto& [k, _] : stamp.items()) keys.push_back(k);
    CHECK(keys == std::vector<std::string>{"name", "version", "url", "commit", "archive_sha256", "spdx", "built",
                                           "bins"});
    // A second run finds the stamp and does not fetch again.
    CHECK(quiet_fetch(demo_row(url, commit, good), tools) == final_dir);
    // Kinds other than external are refused.
    deps::Table sys = demo_row(url, commit, good);
    sys.set("kind", std::string("system")).set("install", std::string("apt install demo"));
    CHECK_THROWS_WITH_AS(quiet_fetch(sys, tools), "demo is a system tool: apt install demo", deps::FetchError);
}

TEST_CASE("deps: a recipe build installs bin/ and the tool identity is the commit") {
    if (!deps::which("git") || !deps::which("make") || !deps::which("sh")) {
        MESSAGE("NOTRUN: git, make or sh not on PATH");
        return;
    }
    const std::string configure =
        "#!/bin/sh\nmkdir -p build\nprintf '#!/bin/sh\\necho demo 1\\n' > build/demo\nchmod +x build/demo\n";
    TempDir td;
    auto [url, commit] = local_tool_repo(td.path, &configure);
    auto good = real_sha(url, commit);
    auto tools = td.path / "tools";
    auto comp = demo_row(url, commit, good);
    comp.set("recipe", std::string("configure-make")).set("recipe_out", std::vector<std::string>{"build/demo"});
    auto final_dir = quiet_fetch(comp, tools);
    auto exe = final_dir / "bin" / "demo";
    REQUIRE(fs::is_regular_file(exe));
    CHECK((fs::status(exe).permissions() & fs::perms::owner_exec) != fs::perms::none);
    CHECK(deps::run_process({exe.string()}, td.path).out == "demo 1\n");
    auto stamp = nlohmann::ordered_json::parse(slurp(final_dir / deps::kStamp));
    CHECK(stamp["built"] == true);
    CHECK(stamp["bins"] == nlohmann::ordered_json::array({"demo"}));
    EnvGuard env("PRISM_TOOLS_DIR", tools.string());
    CHECK(deps::tools_dir() == tools);
    CHECK(prism::tools_home() == tools);
    CHECK(prism::tool_identity(exe) == commit);
    // An unknown recipe and a missing output fail and leave nothing behind.
    auto tools2 = td.path / "tools2";
    auto odd = comp;
    odd.set("recipe", std::string("no-such-recipe"));
    CHECK_THROWS_AS(quiet_fetch(odd, tools2), deps::FetchError);
    auto missing = comp;
    missing.set("recipe_out", std::vector<std::string>{"build/not-built"});
    CHECK_THROWS_WITH_AS(quiet_fetch(missing, tools2), "demo: build did not produce build/not-built",
                         deps::FetchError);
    CHECK_FALSE(fs::exists(tools2 / "demo" / commit));
    if (fs::exists(tools2 / "demo")) CHECK(fs::is_empty(tools2 / "demo"));
}

TEST_CASE("deps: an unknown commit fails") {
    if (!deps::which("git")) {
        MESSAGE("NOTRUN: git not on PATH");
        return;
    }
    TempDir td;
    auto [url, commit] = local_tool_repo(td.path);
    (void)commit;
    CHECK_THROWS_AS(quiet_fetch(demo_row(url, std::string(40, '1'), std::string(64, '0')), td.path / "tools"),
                    deps::FetchError);
    CHECK_FALSE(fs::exists(td.path / "tools" / "demo" / std::string(40, '1')));
}

#endif

// ---- licence firewall ----------------------------------------------------

TEST_CASE("deps: the repository manifest passes the licence firewall") {
    CHECK(cli({"licence-check", "--manifest", manifest_path().string()}) == 0);
    auto m = deps::read_manifest(manifest_path());
    CHECK(deps::licence_problems(m.components(), m.root).empty());
}

TEST_CASE("deps: copyleft linked components fail the licence firewall") {
    auto one = [](const std::string& spdx, const std::string& kind = "linked") {
        deps::Table t;
        t.set("name", std::string("bad")).set("kind", kind).set("spdx", spdx);
        return std::vector<deps::Table>{t};
    };
    for (const char* spdx : {"GPL-3.0-or-later", "LGPL-2.1-only", "AGPL-3.0-or-later", "MPL-2.0",
                             "MIT AND GPL-2.0-only", "EPL-2.0", "NOASSERTION", "", "GPL-2.0-only OR LGPL-2.1-only",
                             "MPL-2.0 OR GPL-2.0-only", "(MIT OR Apache-2.0) AND GPL-3.0-only"})
        CHECK_MESSAGE(!deps::licence_problems(one(spdx), ".").empty(), std::string(spdx));
    CHECK(deps::licence_problems(one("MPL-2.0"), ".")[0] ==
          "bad: linked component is weak copyleft (MPL-2.0); needs review + kAllowWeakCopyleft "
          "(src/tools/prism_deps/licence.cpp)");
    CHECK(deps::licence_problems(one("GPL-3.0-only"), ".")[0] ==
          "bad: linked component is copyleft (GPL-3.0-only); run it as an external process instead");
    // A recorded licence file that is gone fails too.
    TempDir td;
    deps::Table t;
    t.set("name", std::string("lib"))
        .set("kind", std::string("linked"))
        .set("spdx", std::string("MIT"))
        .set("path", std::string("third_party/lib"))
        .set("licence_file", std::string("LICENSE (upstream copy)"));
    auto p = deps::licence_problems({t}, td.path);
    REQUIRE(p.size() == 1);
    CHECK(p[0] == "lib: licence file third_party/lib/LICENSE is missing");
    spit(td.path / "third_party" / "lib" / "LICENSE", "MIT\n");
    CHECK(deps::licence_problems({t}, td.path).empty());
    // The CLI exits 1 on a copyleft linked row.
    auto man = td.path / "third_party" / "MANIFEST.toml";
    spit(man, "[[component]]\nname = \"bad\"\nkind = \"linked\"\nspdx = \"GPL-2.0-only\"\n");
    CHECK(cli({"licence-check", "--manifest", man.string()}) == 1);
}

TEST_CASE("deps: permissive and external copyleft pass the licence firewall") {
    std::vector<deps::Table> ok;
    auto add = [&](const char* name, const char* kind, const char* spdx) {
        deps::Table t;
        t.set("name", std::string(name)).set("kind", std::string(kind)).set("spdx", std::string(spdx));
        ok.push_back(t);
    };
    add("a", "linked", "MIT");
    add("b", "linked", "BSD-3-Clause WITH PCRE2-exception");
    add("c", "linked", "MIT OR GPL-2.0-only");
    add("d", "linked", "Apache-2.0 WITH LLVM-exception");
    add("e", "external", "GPL-3.0-or-later");
    add("f", "external", "AGPL-3.0-or-later");
    add("g", "linked", "GPL-3.0-or-later WITH GCC-exception-3.1");
    add("h", "system", "GPL-3.0-only");
    CHECK(deps::licence_problems(ok, ".").empty());
}

TEST_CASE("deps: licence classification table") {
    struct Row {
        const char* id;
        const char* want;
    };
    for (const Row& r : std::initializer_list<Row>{
             {"GPL", "strong"},
             {"GPL-2.0-only", "strong"},
             {"GPLv2", "strong"},  // tag + "V" on the upper-cased id
             {"gpl-3.0", "strong"},
             {"LGPL-2.1-or-later", "strong"},
             {"AGPLv3", "strong"},
             {"SSPL-1.0", "strong"},
             {"EUPL-1.2", "strong"},
             {"OSL-3.0", "strong"},
             {"CPAL-1.0", "strong"},
             {"CC-BY-SA-4.0", "strong"},
             {"RPL-1.5", "strong"},
             {"QPL-1.0", "strong"},
             {"GPL-2.0-only WITH Autoconf-exception-2.0", "strong"},
             {"GPL-2.0-only WITH Classpath-exception-2.0", "permissive"},
             {"GPL-3.0-or-later WITH GCC-exception-3.1", "permissive"},
             {"Apache-2.0 WITH LLVM-exception", "permissive"},
             {"MPL-2.0", "weak"},
             {"mpl-1.1", "weak"},
             {"EPL-2.0", "weak"},
             {"CDDL-1.0", "weak"},
             {"CPL-1.0", "weak"},
             {"MS-RL", "weak"},
             {"APSL-2.0", "weak"},
             {"MPLv2", "permissive"},  // the "V" suffix rule is for strong tags only
             {"MS-PL", "permissive"},
             {"CC-BY-4.0", "permissive"},
             {"MIT", "permissive"},
             {"BSD-3-Clause", "permissive"},
             {"Apache-2.0", "permissive"},
             {"GPLX", "permissive"},
             {"  GPL-2.0  ", "strong"},
         })
        CHECK_MESSAGE(deps::classify(r.id) == r.want, std::string(r.id));
    CHECK(deps::spdx_ids("(MIT OR GPL-2.0-only) AND BSD-3-Clause") ==
          std::vector<std::string>{"MIT", "GPL-2.0-only", "BSD-3-Clause"});
    CHECK(deps::spdx_ids("Apache-2.0 WITH LLVM-exception") ==
          std::vector<std::string>{"Apache-2.0 WITH LLVM-exception"});
    CHECK(deps::spdx_ids("  ").empty());
}

// ---- SBOM ----------------------------------------------------------------

TEST_CASE("deps: CycloneDX 1.5 SBOM from the manifest") {
    auto bytes = slurp(manifest_path());
    auto bom = nlohmann::ordered_json::parse(deps::build_sbom(bytes, "1.2.3"));
    CHECK(bom["bomFormat"] == "CycloneDX");
    CHECK(bom["specVersion"] == "1.5");
    CHECK(bom["version"] == 1);
    static const std::regex serial("^urn:uuid:[0-9a-f-]{36}$");
    CHECK(std::regex_match(bom["serialNumber"].get<std::string>(), serial));
    CHECK(bom["serialNumber"] == "urn:uuid:" + deps::uuid5("5b0e7c52-3a4f-4c33-9d53-7072736d7362",
                                                          deps::sha256_hex(bytes) + "1.2.3"));
    std::map<std::string, nlohmann::ordered_json> refs;
    for (auto& c : bom["components"]) refs[c["bom-ref"].get<std::string>()] = c;
    auto z3 = refs.at("linked:z3");
    CHECK(z3["type"] == "library");
    CHECK(z3["scope"] == "required");
    CHECK(z3["licenses"] == nlohmann::ordered_json::parse(R"([{"license": {"id": "MIT"}}])"));
    CHECK(z3["purl"].get<std::string>().rfind("pkg:github/z3prover/z3@6f24123f", 0) == 0);
    CHECK(z3["hashes"][0]["alg"] == "SHA-256");
    CHECK(refs.at("linked:pcre2")["licenses"] ==
          nlohmann::ordered_json::parse(R"([{"expression": "BSD-3-Clause WITH PCRE2-exception"}])"));
    CHECK(refs.at("external:cppcheck")["scope"] == "optional");
    CHECK(refs.at("system:clang")["type"] == "application");
    CHECK_FALSE(refs.at("system:clang").contains("hashes"));
    CHECK_FALSE(refs.at("system:clang").contains("purl"));  // no commit
    CHECK(refs.at("external:cake_lpr")["properties"][0] ==
          nlohmann::ordered_json::parse(R"({"name": "prism:kind", "value": "external"})"));
    bool drift = false;
    for (auto& p : refs.at("linked:z3")["properties"])
        if (p["name"] == "prism:drift")
            drift = p["value"] == "D src/api/js/package-lock.json; D src/api/js/package.json; "
                                  "M src/math/simplex/model_based_opt.cpp";
    CHECK(drift);
    auto dep = bom["dependencies"][0];
    CHECK(dep["ref"] == "prism");
    std::vector<std::string> on = dep["dependsOn"];
    std::sort(on.begin(), on.end());
    std::vector<std::string> want;
    for (auto& n : kLinked) want.push_back("linked:" + n);
    std::sort(want.begin(), want.end());
    CHECK(on == want);
    CHECK(bom["metadata"]["component"]["version"] == "1.2.3");
    CHECK(bom["metadata"]["properties"][0]["value"] == deps::sha256_hex(bytes));
}

TEST_CASE("deps: SBOM is deterministic and dated by SOURCE_DATE_EPOCH only") {
    auto bytes = slurp(manifest_path());
    std::string a, b;
    {
        EnvGuard env("SOURCE_DATE_EPOCH", "1700000000");
        a = deps::build_sbom(bytes, "v1");
        b = deps::build_sbom(bytes, "v1");
    }
    CHECK(a == b);
    CHECK(contains(a, "\"timestamp\": \"2023-11-14T22:13:20Z\""));
    {
        EnvGuard env("SOURCE_DATE_EPOCH", "not-a-number");
        CHECK_FALSE(contains(deps::build_sbom(bytes, "v1"), "\"timestamp\""));
    }
    // json.dumps(indent=2) layout: two-space indent, ", " never inline, "[]" when empty.
    CHECK(a.rfind("{\n  \"bomFormat\": \"CycloneDX\",\n  \"specVersion\": \"1.5\",\n", 0) == 0);
    CHECK(a.back() == '\n');
    CHECK(deps::build_sbom(bytes, "v2") != a);
    auto tiny = deps::build_sbom("[[component]]\nname = \"n\"\nkind = \"system\"\nnote = 1\n", "\xc3\xa9");
    CHECK(contains(tiny, "\"version\": \"\\u00e9\""));  // ensure_ascii
    CHECK(contains(tiny, "\"dependsOn\": []"));
    TempDir td;
    auto out = td.path / "prism.cdx.json";
    CHECK(cli({"sbom", "--manifest", manifest_path().string(), "--version", "v9", "-o", out.string()}) ==
          0);
    CHECK(slurp(out) == deps::build_sbom(bytes, "v9"));
}

// ---- command line --------------------------------------------------------

TEST_CASE("deps: command line usage and bad manifests") {
    CHECK(cli({}) == 2);
    CHECK(cli({"frobnicate"}) == 2);
    CHECK(cli({"list", "--bogus"}) == 2);
    CHECK(cli({"list", "--refetch"}) == 2);
    CHECK(cli({"linked", "--no-build"}) == 2);
    CHECK(cli({"--help"}) == 0);
    TempDir td;
    CHECK(cli({"list", "--manifest", (td.path / "none.toml").string()}) == 2);
    auto man = td.path / "third_party" / "MANIFEST.toml";
    spit(man, "[[component]]\nname = \"x\"\nkind = \"vendored\"\n");
    CHECK(cli({"list", "--manifest", man.string()}) == 2);
    CHECK(cli({"linked", "--manifest", man.string()}) == 2);
    CHECK(cli({"list", "--manifest", manifest_path().string()}) == 0);
    auto found = deps::find_manifest(repo_root() / "src" / "prism");
    REQUIRE(found.has_value());
    CHECK(fs::equivalent(*found, manifest_path()));
    auto from_tmp = deps::find_manifest(man.parent_path() / "sub");  // walks up to td's manifest
    REQUIRE(from_tmp.has_value());
    CHECK(fs::equivalent(*from_tmp, man));
}

// ---- tool SHA in findings -------------------------------------------------

TEST_CASE("deps: an unpinned binary's identity is path and sha256, stamped on findings") {
    TempDir td;
    auto exe = td.path / "tool";
    spit(exe, "#!/bin/sh\nexit 0\n");
    EnvGuard env("PRISM_TOOLS_DIR", (td.path / "tools").string());
    auto ident = prism::tool_identity(exe);
    CHECK(ident == "path:" + fs::weakly_canonical(exe).string() + ";sha256:" + deps::sha256_hex("#!/bin/sh\nexit 0\n"));
    std::vector<prism::Finding> fs1(1);
    fs1[0].stage = "x";
    fs1[0].message = "m";
    prism::stamp_tool_sha(fs1, exe);
    CHECK(fs1[0].extra["tool_sha"] == ident);
    fs1[0].extra["tool_sha"] = "kept";
    prism::stamp_tool_sha(fs1, exe);  // an existing value is kept
    CHECK(fs1[0].extra["tool_sha"] == "kept");
}

TEST_CASE("deps: optional tools stamp every finding") {
    auto cpp = slurp(repo_root() / "src" / "prism" / "adapters.cpp");
    CHECK(contains(cpp, "stamp_tool_sha(more, *exe);"));
    for (const char* tool : {"cppcheck", "esbmc", "dafny"})
        CHECK_MESSAGE(contains(cpp, std::string("auto out = run_") + tool + "_unstamped(paths, cfg);"), std::string(tool));
}

// ---- CI, Docker, notices --------------------------------------------------

TEST_CASE("deps: CI runs the licence firewall, linked checks and SBOM with prism-deps") {
    const auto wf = repo_root() / ".github" / "workflows";
    if (!fs::is_directory(wf)) {  // the Docker build context leaves .github/ out
        MESSAGE("NOTRUN: .github/workflows is not in this tree");
        return;
    }
    auto ci = slurp(wf / "ci.yml");
    std::erase(ci, '\r');  // autocrlf checkouts
    for (const char* cmd : {"-DPRISM_DEPS_ONLY=ON", "--target prism-deps", "prism-deps licence-check",
                            "prism-deps linked\n", "prism-deps linked --refetch", "prism-deps sbom --version"})
        CHECK_MESSAGE(contains(ci, cmd), std::string(cmd));
    for (const char* gone : {"scripts/licence_check.py", "scripts/fetch_deps.py", "scripts/sbom.py"})
        CHECK_MESSAGE(!contains(ci, gone), std::string(gone));
    auto rel = slurp(wf / "release.yml");
    CHECK(contains(rel, "id-token: write"));
    CHECK(contains(rel, "sigstore/cosign-installer"));
    CHECK(contains(rel, "SOURCE_DATE_EPOCH"));
    CHECK(contains(rel, "prism-deps licence-check"));
    CHECK(contains(rel, "prism-deps linked"));
    auto recheck = slurp(wf / "proofs-recheck.yml");
    CHECK(contains(recheck, "prism-deps tool lean4export nanoda"));
    CHECK(contains(recheck, "src/tools/prism_deps/**"));
    CHECK_FALSE(contains(recheck, "fetch_deps"));
}

TEST_CASE("deps: the Docker build runs prism-deps before the full build") {
    // Docker: the firewall and the linked check run before the full build,
    // and the SBOM is written by prism-deps.
    auto docker = slurp(repo_root() / "Dockerfile");
    auto fire = docker.find("prism-deps licence-check");
    auto full = docker.find("cmake --build /build ");
    REQUIRE(fire != std::string::npos);
    REQUIRE(full != std::string::npos);
    CHECK(fire < full);
    CHECK(contains(docker, "prism-deps linked"));
    CHECK(contains(docker, "prism-deps sbom --version"));
    for (const char* gone : {"licence_check.py", "fetch_deps.py", "sbom.py"}) CHECK_MESSAGE(!contains(docker, gone), std::string(gone));
    for (const char* gone : {"fetch_deps.py", "licence_check.py", "sbom.py"})
        CHECK_MESSAGE(!fs::exists(repo_root() / "scripts" / gone), std::string(gone));
}

TEST_CASE("deps: self-check and SV-COMP workflow guards") {
    const auto wf = repo_root() / ".github" / "workflows";
    if (!fs::is_directory(wf)) {  // the Docker build context leaves .github/ out
        MESSAGE("NOTRUN: .github/workflows is not in this tree");
        return;
    }
    auto self = slurp(wf / "self-check.yml");
    bool pyyaml = false, pip = false;
    std::istringstream lines(self);
    std::string line;
    while (std::getline(lines, line))
        if (contains(line, "pip install")) {
            pip = true;
            std::string lower = line;
            for (char& c : lower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            pyyaml |= contains(lower, "pyyaml");
        }
    CHECK_MESSAGE(pip, "self-check.yml has no pip install line");
    CHECK(pyyaml);
    // The self-scan must match ci.yml's stage limit (the full tree exceeds the job timeout).
    CHECK(contains(self, "--stage inventory,lints,polyglot"));
    CHECK_MESSAGE(contains(slurp(repo_root() / "src" / "prism" / "cli_svcomp.cpp"), "svcomp pack"),
                  "SV-COMP submission packager must exist");
    CHECK(contains(slurp(wf / "ci.yml"), "prism svcomp pack"));
    CHECK_FALSE(fs::exists(repo_root() / "tools" / "svcomp" / "package_archive.py"));
    CHECK(contains(slurp(repo_root() / "src" / "prism" / "pir" / "stage.cpp"), "PRISM_FUNCTION_BUDGET"));
}

TEST_CASE("deps: notice, licence and history-rewrite script") {
    const auto root = repo_root();
    auto notice = slurp(root / "NOTICE");
    CHECK(contains(notice, "CC-BY-4.0"));
    CHECK(contains(notice, "Fuzz4All"));
    auto lic = slurp(root / "LICENSE");
    CHECK(contains(lic.substr(0, lic.find('\n')), "GNU AFFERO GENERAL PUBLIC LICENSE"));
    CHECK(contains(lic, "Version 3, 19 November 2007"));
    auto script = slurp(root / "scripts" / "rewrite_history.sh");
    for (const auto& name : kMined) CHECK_MESSAGE(contains(script, "third_party/" + name + "/"), name);
    CHECK(contains(script, "git filter-repo"));
    CHECK(contains(script, "I UNDERSTAND"));
    CHECK(fs::is_regular_file(root / "docs" / "SUPPLY_CHAIN.md"));
}

// docs/CPP_PORT_PLAN.md phase 5: tracked .py outside vendored third_party/ must
// stay on the allow-list until the Python engine and pytest suite are deleted.
TEST_CASE("deps: tracked .py files match the phase-5 allow-list") {
    // When true, only tools/svcomp/prism.py may appear outside third_party/.
    constexpr bool k_python_engine_deleted = false;

    auto norm_rel = [](std::string rel) {
        for (char& c : rel)
            if (c == '\\') c = '/';
        return rel;
    };
    auto allowed = [&](std::string_view rel) -> bool {
        if (rel.starts_with("third_party/")) return true;
        if (rel == "tools/svcomp/prism.py") return true;
        if (!k_python_engine_deleted) {
            for (const char* pre : {"prism/", "tests/", "scripts/", "tools/"})
                if (rel.starts_with(pre)) return true;
        }
        return false;
    };

    const auto root = repo_root();
    if (!deps::which("git")) {
        MESSAGE("NOTRUN: git not on PATH");
        return;
    }
    auto r = deps::run_process({"git", "ls-files", "--", "*.py"}, root);
    REQUIRE(r.code == 0);

    std::vector<std::string> forbidden;
    std::istringstream lines(r.out);
    std::string line;
    while (std::getline(lines, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) continue;
        const std::string rel = norm_rel(std::move(line));
        if (!allowed(rel)) forbidden.push_back(rel);
    }
    std::sort(forbidden.begin(), forbidden.end());
    for (const auto& path : forbidden) CHECK_MESSAGE(false, "tracked .py outside allow-list: " << path);

    if (k_python_engine_deleted) {
        std::vector<std::string> tracked_outside_vendor;
        std::istringstream again(r.out);
        while (std::getline(again, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.empty()) continue;
            const std::string rel = norm_rel(line);
            if (!rel.starts_with("third_party/")) tracked_outside_vendor.push_back(rel);
        }
        std::sort(tracked_outside_vendor.begin(), tracked_outside_vendor.end());
        CHECK(tracked_outside_vendor == std::vector<std::string>{"tools/svcomp/prism.py"});
    }
}
