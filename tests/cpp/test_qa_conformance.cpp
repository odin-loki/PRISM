// prism-qa conformance / soundness (src/tools/qa): the shape of the
// conformance suite and the rules of the scorer, without depending on the
// engine's current numbers (the conformance workflow scores the engine).
//
// Covers: the YAML subset the task files use, task discovery, the in-house /
// SV-COMP / concurrency / ESBMC / libc-model corpora, ESBMC test conversion,
// classify (property scope, expect_class, Law 6), counterexample parsing,
// signatures, the certified-mode summary, the release gate against a stand-in
// engine (tests/cpp/fake_prism.cpp), the process-tree timeout, the Python-
// compatible random generator behind the soundness campaign's programs, and a
// sample of the sanitizer label self-check.

#include <doctest/doctest.h>

#include "gen_random.hpp"
#include "support/cli.hpp"
#include "support/engine.hpp"
#include "support/fetch.hpp"
#include "support/proctree.hpp"
#include "support/pyrandom.hpp"
#include "support/replay.hpp"
#include "support/report.hpp"
#include "support/task.hpp"
#include "support/yaml_subset.hpp"

#include "prism/ai.hpp"
#include "prism/regex.hpp"

#include <algorithm>
#include <chrono>
#include <csignal>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <sys/stat.h>
#include <unistd.h>

namespace {

namespace fs = std::filesystem;
namespace qa = prism::qa;
using qa::ojson;
using i128 = __int128;

const std::set<std::string> kProperties = {"no-overflow", "no-div0",       "no-shift-ub", "no-oob",
                                           "no-null-deref", "memsafety", "no-fp-cast",  "no-uncaught"};

fs::path suite() { return qa::suite_root(); }

std::string slurp(const fs::path& p) { return qa::read_text(p); }

void spit(const fs::path& p, const std::string& text) {
    fs::create_directories(p.parent_path());
    std::ofstream(p, std::ios::binary) << text;
}

bool ends_with(const std::string& s, const std::string& suf) {
    return s.size() >= suf.size() && s.compare(s.size() - suf.size(), suf.size(), suf) == 0;
}

struct QaTmp {
    fs::path dir;
    QaTmp() {
        std::string tmpl = (fs::temp_directory_path() / "prism-qa-test-XXXXXX").string();
        REQUIRE(::mkdtemp(tmpl.data()) != nullptr);
        dir = tmpl;
    }
    ~QaTmp() {
        std::error_code ec;
        fs::remove_all(dir, ec);
    }
};

std::vector<qa::Task> house_tasks() { return qa::discover({suite() / "prism", suite() / "sv-comp"}); }

qa::Task mk_task(const std::string& origin, const std::string& prop, std::vector<std::pair<std::string, bool>> exp) {
    qa::Task t;
    t.ident = "x.yml";
    t.yml = "x.yml";
    t.source = "x.c";
    t.origin = origin;
    t.category = "c";
    t.lang = "C";
    t.prop = prop;
    t.expected = std::move(exp);
    return t;
}

ojson finding(const std::string& status, const std::string& cls = "") {
    ojson f = ojson::object();
    f["status"] = status;
    if (!cls.empty()) f["cls"] = cls;
    return f;
}

ojson one(const ojson& f) { return ojson::array({f}); }

fs::path self_dir() {
    char buf[4096];
    ssize_t n = ::readlink("/proc/self/exe", buf, sizeof buf - 1);
    REQUIRE(n > 0);
    buf[n] = 0;
    return fs::path(buf).parent_path();
}

// a /bin/sh wrapper that runs fake_prism with fixed leading arguments
fs::path fake_engine(const fs::path& dir, const std::string& name, const std::string& lead) {
    fs::path fake = self_dir() / "fake_prism";
    REQUIRE_MESSAGE(fs::exists(fake), "fake_prism is built next to prism_tests");
    fs::path w = dir / name;
    spit(w, "#!/bin/sh\nexec '" + fake.string() + "' " + lead + " \"$@\"\n");
    ::chmod(w.c_str(), 0755);
    return w;
}

bool proc_gone(long pid) {
    for (int i = 0; i < 250; ++i) {
        if (::kill(static_cast<pid_t>(pid), 0) != 0 && errno == ESRCH) return true;
        std::string s = slurp("/proc/" + std::to_string(pid) + "/stat");
        auto rp = s.rfind(')');
        if (s.empty() || (rp != std::string::npos && rp + 2 < s.size() && s[rp + 2] == 'Z')) return true;
        ::usleep(20000);
    }
    return false;
}

std::string repr_rows(const std::vector<std::vector<i128>>& rows) {
    std::string s = "[";
    for (std::size_t i = 0; i < rows.size(); ++i) {
        if (i) s += ", ";
        s += "[";
        for (std::size_t k = 0; k < rows[i].size(); ++k) s += (k ? ", " : "") + qa::i128_str(rows[i][k]);
        s += "]";
    }
    return s + "]";
}

}  // namespace

// ---------------------------------------------------------------- YAML subset

TEST_CASE("qa yaml: the subset the task files use, typed like PyYAML") {
    auto n = qa::yaml::parse(
        "format_version: '2.0'\n"
        "# comment\n"
        "input_files: 'jain_6-2.c'\n"
        "properties:\n"
        "  - property_file: ../properties/no-overflow.prp\n"
        "    expected_verdict: false\n"
        "  - property_file: ../properties/coverage-branches.prp\n"
        "options:\n"
        "  language: C\n"
        "  data_model: ILP32\n"
        "expected:  # the verdicts\n"
        "  f_true: true\n"
        "  g: False\n"
        "witness:\n"
        "  g: [2147483647, -1, 0x10]\n"
        "  h: []\n"
        "sanitizer_blind:\n"
        "- g\n"
        "expect_class:\n"
        "  g: \"MEM-OOB-READ|MEM-OOB-WRITE\"\n"
        "esbmc_options: \"--unwind 12 \\\"q\\\"\"\n"
        "timeout: 900  # slow\n"
        "empty:\n");
    REQUIRE(n.is_map());
    CHECK(n.get("format_version")->quoted);
    CHECK(n.get("format_version")->py_str() == "2.0");
    CHECK(n.get("input_files")->text == "jain_6-2.c");
    const auto* props = n.get("properties");
    REQUIRE(props->is_seq());
    REQUIRE(props->seq.size() == 2);
    CHECK(props->seq[0].get("property_file")->text == "../properties/no-overflow.prp");
    CHECK(props->seq[0].get("expected_verdict")->as_bool() == false);
    CHECK_FALSE(props->seq[1].has("expected_verdict"));
    CHECK(n.get("options")->get("data_model")->text == "ILP32");
    CHECK(n.get("expected")->map.size() == 2);
    CHECK(n.get("expected")->get("f_true")->truthy_label());
    CHECK_FALSE(n.get("expected")->get("g")->truthy_label());
    const auto* w = n.get("witness")->get("g");
    REQUIRE(w->seq.size() == 3);
    CHECK(*w->seq[0].as_int() == 2147483647);
    CHECK(*w->seq[1].as_int() == -1);
    CHECK(*w->seq[2].as_int() == 16);
    CHECK(n.get("witness")->get("h")->seq.empty());
    CHECK(n.get("sanitizer_blind")->seq[0].text == "g");
    CHECK(n.get("expect_class")->get("g")->text == "MEM-OOB-READ|MEM-OOB-WRITE");
    CHECK(n.get("esbmc_options")->text == "--unwind 12 \"q\"");
    CHECK(n.get("timeout")->py_str() == "900");
    CHECK(n.get("empty")->is_null());
    CHECK(n.get("empty")->py_str() == "None");
    // PyYAML 1.1: yes/on are booleans, a quoted 'true' is the string "true"
    CHECK(qa::yaml::parse("a: yes\n").get("a")->truthy_label());
    CHECK(qa::yaml::parse("a: 'true'\n").get("a")->truthy_label());
    CHECK_FALSE(qa::yaml::parse("a: 'yes'\n").get("a")->truthy_label());
    CHECK_FALSE(qa::yaml::parse("a: 1\n").get("a")->truthy_label());
    CHECK(qa::yaml::parse("a: 010\n").get("a")->py_str() == "8");
    // a repeated key keeps its first position and its last value
    auto d = qa::yaml::parse("x: 1\ny: 2\nx: 3\n");
    REQUIRE(d.map.size() == 2);
    CHECK(d.map[0].first == "x");
    CHECK(d.map[0].second.text == "3");
}

TEST_CASE("qa yaml: constructs outside the subset are errors, never approximations") {
    for (const char* bad : {"a: |\n  text\n", "a: >\n  text\n", "a: &x 1\n", "a: *x\n", "a: !!str 1\n",
                            "a: {b: 1}\n", "a: [1, [2]]\n", "a: 'open\n", "a: b: c\n", "a:\n\tb: 1\n",
                            "a: 1\n---\nb: 2\n", "a: 1\n   b: 2\n"}) {
        CAPTURE(bad);
        CHECK_THROWS_AS(qa::yaml::parse(bad), qa::yaml::Error);
    }
    CHECK(qa::yaml::parse("a: {}\n").get("a")->is_map());
}

TEST_CASE("qa yaml: every task file of the suite loads") {
    int n = 0;
    for (const auto& yml : qa::rglob_yml(suite())) {
        CAPTURE(yml.string());
        CHECK_NOTHROW(qa::yaml::load_file(yml));
        ++n;
    }
    CHECK(n >= 600);
}

// ---------------------------------------------------------------- suite shape

TEST_CASE("qa conformance: in-house tasks come in labelled true/false pairs") {
    std::vector<qa::Task> tasks;
    for (auto& t : house_tasks())
        if (t.origin == "prism") tasks.push_back(t);
    CHECK(tasks.size() >= 120);
    std::set<std::string> stems;
    for (const auto& t : tasks) stems.insert(t.source.stem().string());
    long n_true = 0, n_false = 0;
    bool cxx = false, law6 = false;
    for (const auto& t : tasks) {
        CAPTURE(t.ident);
        CHECK(fs::exists(t.source));
        CHECK(kProperties.count(t.prop));
        CHECK_FALSE(t.expected.empty());
        const std::string text = slurp(t.source);
        for (const auto& [fn, exp] : t.expected) {
            CHECK(text.find(fn) != std::string::npos);
            if (!exp && !t.expect_status.count(fn)) CHECK_MESSAGE(t.witness.count(fn), "false function needs a witness");
            (exp ? n_true : n_false)++;
            if (t.expect_status.count(fn) && t.expect_status.at(fn) == "NEEDS-HARNESS") law6 = true;
        }
        // every feature comes as a true/false pair
        const std::string stem = t.source.stem().string();
        if (ends_with(stem, "_true")) CHECK(stems.count(stem.substr(0, stem.size() - 5) + "_false"));
        else if (ends_with(stem, "_false")) CHECK(stems.count(stem.substr(0, stem.size() - 6) + "_true"));
        if (t.lang == "C++") cxx = true;
    }
    CHECK(n_true == n_false);
    CHECK(cxx);
    CHECK(law6);
}

TEST_CASE("qa conformance: the SV-COMP subset is pinned") {
    std::vector<qa::Task> tasks;
    for (auto& t : house_tasks())
        if (t.origin == "sv-comp") tasks.push_back(t);
    CHECK(tasks.size() >= 40);
    std::set<bool> verdicts;
    for (const auto& t : tasks) {
        CAPTURE(t.ident);
        CHECK(t.prop == "no-overflow");
        REQUIRE(t.expected.size() == 1);
        CHECK(t.expected[0].first == "main");
        verdicts.insert(t.expected[0].second);
    }
    CHECK(verdicts == std::set<bool>{true, false});
    const std::string sources = slurp(suite() / "SOURCES.md");
    CHECK(sources.find("07b00127ac57f773f9de6eee95821b6947948dbe") != std::string::npos);
    CHECK(sources.find("Apache-2.0") != std::string::npos);
}

TEST_CASE("qa conformance: concurrency tasks are thread programs scored by conc only") {
    // roadmap 2.6: whole thread programs, true/false pairs, scored by conc only
    auto tasks = qa::discover({suite() / "concurrency"});
    CHECK(tasks.size() >= 20);
    std::set<std::string> stems, props;
    for (const auto& t : tasks) stems.insert(t.source.stem().string());
    for (const auto& t : tasks) {
        CAPTURE(t.ident);
        CHECK(t.origin == "concurrency");
        CHECK((t.prop == "norace" || t.prop == "noassert" || t.prop == "nodeadlock"));
        props.insert(t.prop);
        REQUIRE(t.expected.size() == 1);
        CHECK(t.expected[0].first == "main");
        std::string text = slurp(t.source);
        CHECK((text.find("pthread_create") != std::string::npos || text.find("thrd_create") != std::string::npos));
        const std::string stem = t.source.stem().string();
        const std::string twin = ends_with(stem, "_true") ? stem.substr(0, stem.size() - 5) + "_false"
                                                          : stem.substr(0, stem.size() - 6) + "_true";
        CHECK(stems.count(twin));
    }
    CHECK(props == std::set<std::string>{"norace", "noassert", "nodeadlock"});
    CHECK(qa::STAGE_TASK_ORIGINS.at("conc") == std::set<std::string>{"concurrency"});
    CHECK(qa::ORIGIN_STAGES.at("concurrency") == std::set<std::string>{"conc"});
    CHECK(std::find(qa::VERDICT_STAGES.begin(), qa::VERDICT_STAGES.end(), "conc") != qa::VERDICT_STAGES.end());
}

TEST_CASE("qa conformance: Juliet is pinned, not vendored") {
    const std::string sources = slurp(suite() / "SOURCES.md");
    CHECK(sources.find(qa::JULIET_SHA256) != std::string::npos);
    CHECK(sources.find(std::to_string(qa::JULIET_SIZE)) != std::string::npos);
    bool cwe = false;
    for (const auto& e : fs::recursive_directory_iterator(suite()))
        if (e.path().filename().string().rfind("CWE", 0) == 0) cwe = true;
    CHECK_MESSAGE(!cwe, "Juliet stays out of git");
}

TEST_CASE("qa conformance: Juliet test cases become tasks from their good/bad functions") {
    const std::string text =
        "void CWE190_x_01_bad()\n{\n}\nstatic void goodG2B()\n{\n}\nvoid CWE190_x_01_good()\n{\n}\n"
        "static void helper(int a)\n{\n}\n";
    auto y = qa::juliet_task("CWE190_x__int_01.c", text, "CWE190");
    REQUIRE(y);
    CHECK(*y ==
          "format_version: 1\ninput_files: CWE190_x__int_01.c\nlanguage: C\nproperty: no-overflow\n"
          "origin: juliet-1.3\nexpected:\n  CWE190_x_01_bad: false\n  goodG2B: true\n  CWE190_x_01_good: true\n");
    CHECK_FALSE(qa::juliet_task("a.c", "static void helper(int a)\n{\n}\n", "CWE369"));
    CHECK(qa::latin1_to_utf8("caf\xe9") == "caf\xc3\xa9");
    // the archive must match its pin before anything is extracted
    QaTmp tmp;
    spit(tmp.dir / "juliet-c-cplusplus-v1.3.zip", "not the archive");
    qa::JulietSource src;
    src.size = static_cast<long long>(fs::file_size(tmp.dir / "juliet-c-cplusplus-v1.3.zip"));
    CHECK_THROWS_WITH_AS(qa::fetch_juliet(tmp.dir, {"01"}, src), doctest::Contains("hash mismatch"),
                         std::runtime_error);
    CHECK_FALSE(fs::exists(tmp.dir / "juliet"));
}

// ---------------------------------------------------------------- ESBMC C++

TEST_CASE("qa conformance: the ESBMC C++ subset is pinned and small") {
    auto tasks = qa::discover({suite() / "esbmc-cpp"});
    CHECK(tasks.size() >= 40);
    CHECK_MESSAGE(tasks.size() <= 80, "the full ESBMC set stays out of git (--fetch-esbmc)");
    const prism::Regex foreign(qa::ESBMC_FOREIGN_TEXT);
    std::set<bool> verdicts;
    for (const auto& t : tasks) {
        CAPTURE(t.ident);
        CHECK(t.origin == "esbmc-cpp");
        CHECK(t.prop == "esbmc-cpp");
        REQUIRE(t.expected.size() == 1);
        CHECK(t.expected[0].first == "main");
        verdicts.insert(t.expected[0].second);
        CHECK(t.deterministic);
        CHECK(slurp(t.yml).find("esbmc@" + qa::ESBMC_COMMIT.substr(0, 12) + " regression/esbmc-cpp") !=
              std::string::npos);
        CHECK_FALSE(foreign.search(qa::utf8_clean(slurp(t.source))));
    }
    CHECK(verdicts == std::set<bool>{true, false});
    CHECK(slurp(suite() / "SOURCES.md").find(qa::ESBMC_COMMIT) != std::string::npos);
    CHECK(fs::exists(suite() / "esbmc-cpp" / "LICENSE.Apache-2.0.txt"));
    CHECK(qa::PROPERTY_SCOPED.count("esbmc-cpp"));
    CHECK(std::find(qa::DEFAULT_ROOTS.begin(), qa::DEFAULT_ROOTS.end(), "esbmc-cpp") != qa::DEFAULT_ROOTS.end());
}

TEST_CASE("qa conformance: ESBMC regression tests convert, or are skipped with the reason") {
    QaTmp tmp;
    const fs::path reg = tmp.dir / "regression";
    auto mk = [&](const std::string& name, const std::string& desc, const std::map<std::string, std::string>& files) {
        fs::path td = reg / "esbmc-cpp" / "vector" / name;
        spit(td / "test.desc", desc);
        for (const auto& [fn, text] : files) spit(td / fn, text);
        return td;
    };
    auto ok = mk("v1", "CORE\nmain.cpp\n--unwind 10 --no-unwinding-assertions\n^VERIFICATION SUCCESSFUL$\n",
                 {{"main.cpp", "#include <cassert>\nint main() { assert(1); }\n"}});
    auto [t, why] = qa::esbmc_convert(ok, reg);
    CHECK(why == "");
    REQUIRE(t);
    CHECK(t->expected);
    CHECK(t->label_bound == 10);
    CHECK(t->deterministic);
    CHECK(qa::esbmc_task_name(t->upstream) == std::pair<std::string, std::string>{"cpp03_vector", "v1"});
    auto bad = mk("v2", "CORE\nmain.cpp\n\n^VERIFICATION FAILED$\n",
                  {{"main.cpp", "int nondet_int();\nint main() { return 1 / nondet_int(); }\n"}});
    auto [t2, why2] = qa::esbmc_convert(bad, reg);
    REQUIRE(t2);
    CHECK_FALSE(t2->expected);
    CHECK_FALSE(t2->deterministic);
    struct Skip {
        std::string name, desc;
        std::map<std::string, std::string> files;
        std::string reason;
    };
    const std::vector<Skip> skips = {
        {"k", "KNOWNBUG\nmain.cpp\n\n^VERIFICATION FAILED$\n", {{"main.cpp", ""}}, "KNOWNBUG"},
        {"o", "CORE\nmain.cpp\n--overflow-check\n^VERIFICATION FAILED$\n", {{"main.cpp", ""}}, "--overflow-check"},
        // a reachable ERROR: label is the violation there; PRISM checks no labels
        {"e", "CORE\nmain.cpp\n--unwind 8 --error-label ERROR\n^VERIFICATION FAILED$\n", {{"main.cpp", ""}},
         "--error-label"},
        {"m", "CORE\nmain.cpp\n\n^VERIFICATION FAILED$\n", {{"main.cpp", ""}, {"a.h", ""}}, "several"},
        {"c", "CORE\nmain.cpp\n\n^VERIFICATION FAILED$\n^  String capacity exceeded$\n", {{"main.cpp", ""}},
         "operational model"},
        {"n", "CORE\nmain.cpp\n\n^EXIT=0$\n", {{"main.cpp", ""}}, "no single"},
    };
    for (const auto& s : skips) {
        CAPTURE(s.name);
        auto [ts, w] = qa::esbmc_convert(mk(s.name, s.desc, s.files), reg);
        CHECK_FALSE(ts);
        CHECK(w.find(s.reason) != std::string::npos);
    }
    // the task file written for a converted test
    const std::string y = qa::esbmc_task_yaml(*t, "v1.cpp");
    CHECK(y.find("upstream: esbmc@" + qa::ESBMC_COMMIT.substr(0, 12) + " regression/esbmc-cpp/vector/v1\n") !=
          std::string::npos);
    CHECK(y.find("esbmc_options: \"--unwind 10 --no-unwinding-assertions\"\n") != std::string::npos);
    CHECK(y.find("label_bound: 10\n") != std::string::npos);
    auto loaded = qa::yaml::parse(y);
    CHECK(loaded.get("expected")->get("main")->truthy_label());
}

TEST_CASE("qa conformance: ESBMC labels speak for ESBMC's default property set") {
    auto t = mk_task("esbmc-cpp", "esbmc-cpp", {{"main", true}});
    // ESBMC does not check signed overflow by default: another property
    CHECK(qa::classify(t, "main", one(finding("FAILED", "INT-SIGNED-OVF"))) == "failed-other-property");
    CHECK(qa::classify(t, "main", one(finding("FAILED", "FUNC-CONTRACT"))) == "false-alarm");
    t.expected[0].second = false;
    CHECK(qa::classify(t, "main", one(finding("PROVED"))) == "wrong-proof");
}

// ---------------------------------------------------------------- libc models

TEST_CASE("qa conformance: every libc model file has a contract harness") {
    auto tasks = qa::discover({suite() / "libc-models"});
    CHECK(tasks.size() >= 4);
    const fs::path models = qa::repo_root() / "src" / "prism" / "pir" / "models" / "libc";
    const prism::Regex inc_re(R"re(#include "\.\./\.\./\.\./src/prism/pir/models/libc/(\w+\.c)")re");
    std::set<std::string> included;
    for (const auto& t : tasks) {
        CAPTURE(t.ident);
        CHECK(t.origin == "libc-models");
        const std::string text = slurp(t.source);
        for (const auto& m : inc_re.finditer(text)) included.insert(m.group(1));
        std::set<bool> vals;
        std::set<std::string> false_fns;
        for (const auto& [fn, exp] : t.expected) {
            CHECK(text.find("int " + fn + "(") != std::string::npos);
            CHECK(ends_with(fn, "_true") == exp);
            vals.insert(exp);
            if (!exp) false_fns.insert(fn);
        }
        CHECK(vals == std::set<bool>{true, false});
        // every false harness names the violation it plants
        std::set<std::string> classed;
        for (const auto& [fn, _] : t.expect_class) classed.insert(fn);
        CHECK(classed == false_fns);
    }
    std::set<std::string> model_files;
    for (const auto& e : fs::directory_iterator(models))
        if (e.path().extension() == ".c") model_files.insert(e.path().filename().string());
    CHECK_MESSAGE(included == model_files, "every model file has a harness");
    CHECK(std::find(qa::DEFAULT_ROOTS.begin(), qa::DEFAULT_ROOTS.end(), "libc-models") != qa::DEFAULT_ROOTS.end());
}

TEST_CASE("qa conformance: every libc model function is called by a harness") {
    // roadmap 8.2: not only every model file, every model function has a
    // contract harness that calls it (the C++ allocation functions through
    // the new/delete expressions they implement)
    const fs::path models = qa::repo_root() / "src" / "prism" / "pir" / "models" / "libc";
    std::string harness_c;
    for (const auto& e : fs::directory_iterator(suite() / "libc-models")) {
        if (e.path().extension() != ".c") continue;
        std::istringstream in(slurp(e.path()));
        std::string ln;
        while (std::getline(in, ln))
            if (ln.rfind("#include", 0) != 0) harness_c += ln + "\n";
    }
    const std::string new_delete = slurp(suite() / "libc-models" / "new_delete.cpp");
    const std::map<std::string, std::string> cxx_forms = {
        {"_Znwm", "new int("},        {"_Znam", "new int["},
        {"_ZdlPv", "delete q;"},      {"_ZdaPv", "delete[] p;"},
        {"_ZdlPvm", "::operator delete(p, sizeof(int))"}, {"_ZdaPvm", "::operator delete[](q, 4)"}};
    const prism::Regex def_re(R"(^(?!static)[A-Za-z_][\w \*]*?\b(\w+)\([^;{]*\)\s*\{)", true);
    std::set<std::string> defined;
    for (const auto& e : fs::directory_iterator(models))
        if (e.path().extension() == ".c")
            for (const auto& m : def_re.finditer(slurp(e.path()))) defined.insert(m.group(1));
    CHECK(defined.size() >= 50);
    for (const auto& name : defined) {
        CAPTURE(name);
        if (name.rfind("_Z", 0) == 0) {
            REQUIRE(cxx_forms.count(name));
            CHECK(new_delete.find(cxx_forms.at(name)) != std::string::npos);
        } else {
            CHECK_MESSAGE(prism::Regex("\\b" + name + "\\(").search(harness_c), "model function has no contract harness");
        }
    }
}

TEST_CASE("qa conformance: every C++ library model header is embedded and exercised") {
    // every C++ library model header (src/prism/pir/models/cxx) is embedded
    // (CMake list) and exercised by a contract harness and in-house tasks
    const fs::path cxx = qa::repo_root() / "src" / "prism" / "pir" / "models" / "cxx";
    const std::string cmake = slurp(qa::repo_root() / "CMakeLists.txt");
    std::vector<std::string> files, headers;
    for (const auto& e : fs::recursive_directory_iterator(cxx))
        if (e.is_regular_file()) files.push_back(e.path().lexically_relative(cxx).generic_string());
    // public headers; prism_*.h are the models' internal headers and
    // bits/basic_string.tcc replaces the libstdc++ file <string> includes
    for (const auto& f : files)
        if (f.find('/') == std::string::npos && f.rfind("prism_", 0) != 0) headers.push_back(f);
    for (const char* h : {"vector", "map", "set"})
        CHECK(std::find(headers.begin(), headers.end(), h) != headers.end());
    CHECK(std::find(files.begin(), files.end(), "bits/basic_string.tcc") != files.end());
    std::string harnesses, tasks;
    for (const auto& e : fs::recursive_directory_iterator(suite() / "libc-models"))
        if (e.path().extension() == ".cpp") harnesses += slurp(e.path()) + "\n";
    for (const auto& e : fs::directory_iterator(suite() / "prism" / "cxx"))
        if (e.path().extension() == ".cpp") tasks += slurp(e.path()) + "\n";
    std::map<std::string, std::string> pub;
    for (const auto& h : headers) pub[h] = h;
    pub["bits/basic_string.tcc"] = "string";
    for (const auto& f : files) {
        CAPTURE(f);
        CHECK(cmake.find("src/prism/pir/models/cxx/" + f) != std::string::npos);
        if (pub.count(f)) {
            CHECK(harnesses.find("#include <" + pub[f] + ">") != std::string::npos);
            CHECK(tasks.find("#include <" + pub[f] + ">") != std::string::npos);
        } else {  // an internal header: some public model header includes it
            bool any = false;
            for (const auto& h : headers)
                if (slurp(cxx / h).find("#include <" + f + ">") != std::string::npos) any = true;
            CHECK(any);
        }
    }
}

TEST_CASE("qa conformance: expect_class requires the planted violation") {
    auto t = mk_task("libc-models", "libc-contract", {{"f", false}});
    t.expect_class["f"] = {"MEM-OOB-WRITE", "MEM-OOB-READ"};
    CHECK(qa::classify(t, "f", one(finding("FAILED", "MEM-OOB-WRITE"))) == "refuted");
    // refuted for another reason than the planted one: not a detection
    CHECK(qa::classify(t, "f", one(finding("FAILED", "UNINIT-READ"))) == "failed-other-property");
}

// ---------------------------------------------------------------- scorer rules

TEST_CASE("qa conformance: classify") {
    auto task = [](bool expected, const std::string& origin = "prism", const std::string& prop = "no-overflow",
                   const std::string& status = "") {
        auto t = mk_task(origin, prop, {{"f", expected}});
        if (!status.empty()) t.expect_status["f"] = status;
        return t;
    };
    const ojson fail = one(finding("FAILED", "INT-SIGNED-OVF"));
    CHECK(qa::PROOF == std::set<std::string>{"PROVED", "PROVED-UNBOUNDED", "PROVED-ASSUMING", "PROVED-CERTIFIED"});
    for (const auto& proof : qa::PROOF) {
        CAPTURE(proof);
        CHECK(qa::classify(task(false), "f", one(finding(proof))) == "wrong-proof");
        CHECK(qa::classify(task(true), "f", one(finding(proof))) == "proved");
    }
    CHECK(qa::classify(task(true), "f", fail) == "false-alarm");
    CHECK(qa::classify(task(false), "f", fail) == "refuted");
    // BOUNDED is never a proof (Law 2) and never a refutation
    CHECK(qa::classify(task(false), "f", one(finding("BOUNDED"))) == "bounded");
    CHECK(qa::classify(task(true), "f", one(finding("BOUNDED"))) == "bounded");
    CHECK(qa::classify(task(true), "f", ojson::array()) == "missing");
    // Law 6
    auto law = task(false, "prism", "no-overflow", "NEEDS-HARNESS");
    CHECK(qa::classify(law, "f", one(finding("NEEDS-HARNESS"))) == "law-ok");
    CHECK(qa::classify(law, "f", fail) == "law6-violation");
    CHECK(qa::classify(law, "f", one(finding("PROVED"))) == "wrong-proof");
    // property-scoped (SV-COMP) labels: another class is not a false alarm
    auto sv = task(true, "sv-comp");
    CHECK(qa::classify(sv, "f", one(finding("FAILED", "INT-SHIFT-UB"))) == "failed-other-property");
    CHECK(qa::classify(sv, "f", fail) == "false-alarm");
    // concurrency labels are property-scoped too; conc is BOUNDED on true tasks
    const ojson race = one(finding("FAILED", "CONC-DATA-RACE"));
    auto noassert_true = task(true, "concurrency", "noassert");
    CHECK(qa::classify(noassert_true, "f", race) == "failed-other-property");
    CHECK(qa::classify(noassert_true, "f", one(finding("BOUNDED"))) == "bounded");
    CHECK(qa::classify(task(false, "concurrency", "norace"), "f", race) == "refuted");
    CHECK(qa::classify(task(true, "concurrency", "norace"), "f", race) == "false-alarm");
    // a certified proof of a false function is a wrong proof like any other
    CHECK(qa::classify(task(false), "f", one(finding("PROVED-CERTIFIED"))) == "wrong-proof");
}

TEST_CASE("qa conformance: counterexamples parse in both notations") {
    using V = std::vector<std::pair<std::string, i128>>;
    CHECK(qa::parse_cex("a=2147418111, b=-3") == V{{"a", 2147418111}, {"b", -3}});
    CHECK(qa::parse_cex("a=#x0000000f, b=#b101") == V{{"a", 15}, {"b", 5}});
    CHECK(qa::parse_cex("ovf+=sat").empty());
    CHECK(qa::parse_cex("x=true, y=false, z=-0x10") == V{{"x", 1}, {"y", 0}, {"z", -16}});
    // replay reinterprets bit patterns in the parameter's C type
    CHECK(qa::parse_cex("a=#xffffffff") == V{{"a", 4294967295}});
}

TEST_CASE("qa conformance: function signatures and scalar ranges") {
    const std::string text = "[[nodiscard]] static long long g(unsigned short a, const int& b) {\n return 0; }\n";
    auto sig = qa::find_signature(text, "g");
    REQUIRE(sig);
    CHECK(sig->second == qa::Params{{"unsigned short", "a"}, {"const int&", "b"}});
    CHECK(qa::type_range("unsigned short") == std::pair<i128, i128>{0, 65535});
    CHECK(qa::type_range("const int&") == std::pair<i128, i128>{-(i128(1) << 31), (i128(1) << 31) - 1});
    CHECK_FALSE(qa::type_range("int *"));
    CHECK(qa::type_range("unsigned _BitInt(40)") == std::pair<i128, i128>{0, (i128(1) << 40) - 1});
    CHECK(qa::c_literal(-(i128(1) << 63), "long long") == "(long long)(-9223372036854775807LL - 1)");
    CHECK(qa::c_literal(i128(18446744073709551615ULL), "const size_t") == "(size_t)18446744073709551615ULL");
    CHECK(qa::c_literal(-5, "int") == "(int)(-5LL)");
    CHECK_FALSE(qa::find_signature("int f(int *p) {}", "g"));
    auto v = qa::find_signature("int f(void) { return 0; }", "f");
    REQUIRE(v);
    CHECK(v->second.empty());
}

TEST_CASE("qa conformance: metrics, gate line and markdown from rows") {
    auto row = [](const std::string& fn, bool exp, const std::string& outcome, bool replayed) {
        ojson r = ojson::object();
        r["task"] = "prism/x/t.yml";
        r["origin"] = "prism";
        r["category"] = "x";
        r["function"] = fn;
        r["expected"] = exp;
        r["property"] = "no-overflow";
        r["stage"] = "bmc";
        r["law_task"] = false;
        r["findings"] = ojson::array({finding(outcome == "refuted" ? "FAILED" : "PROVED", "INT-SIGNED-OVF")});
        r["seconds"] = 1.5;
        r["outcome"] = outcome;
        if (outcome == "refuted") {
            ojson rp = ojson::object();
            rp["replay"] = replayed ? "replayed" : "not-replayed";
            r["replay"] = rp;
        }
        return r;
    };
    std::vector<ojson> rows = {row("a", true, "proved", false), row("b", true, "no-answer", false),
                               row("c", false, "refuted", true), row("d", false, "refuted", false)};
    ojson m = qa::compute_metrics(rows, {"bmc"});
    const ojson& b = m["bmc"]["prism"];
    CHECK(b["functions"] == 4);
    CHECK(b["proved"] == 1);
    CHECK(b["refuted"] == 2);
    CHECK(b["refuted_replayed"] == 1);
    CHECK(b["completeness"].get<double>() == doctest::Approx(0.5));
    CHECK(b["detection"].get<double>() == doctest::Approx(0.5));
    CHECK(b["soundness_ok"] == true);
    CHECK(m["bmc"]["all"]["functions"] == 4);
    CHECK(qa::pct(1, 3) == "33.3%");
    CHECK(qa::pct(1, 0) == "n/a");
    CHECK(qa::py_round(2.0 / 3.0, 4) == 0.6667);
    ojson meta = ojson::object();
    meta["engine"] = "cpp";
    meta["command"] = {"/x/prism"};
    meta["stages"] = {"inventory", "bmc"};
    meta["tasks"] = 1;
    meta["functions"] = 4;
    meta["sandbox"] = "none (bwrap unavailable)";
    meta["wrong_proofs"] = 0;
    const std::string md = qa::markdown(m, rows, meta);
    CHECK(md.find("- **release gate: PASS** (0 wrong proof(s))") != std::string::npos);
    CHECK(md.find("| bmc | prism | 2 | 2 | **0** | 1/2 (50.0%) | 1/2 (50.0%) | 1 | 0 | 0 | 1 | 0/0 |") !=
          std::string::npos);
    CHECK(md.find("## Refuted but counterexample did not replay (1)") != std::string::npos);
    CHECK(md.find("  - replay: not-replayed") != std::string::npos);
    CHECK(md.find("| prism/x | 1/2 | 2/2 |") != std::string::npos);
}

TEST_CASE("qa conformance: certified summary counts loop-free true functions") {
    auto row = [](const std::string& fn, bool expected, const std::string& status, const char* loops,
                  const std::string& note = "") {
        ojson extra = ojson::object();
        if (loops) extra["loops"] = loops;
        if (fn == "v") extra["certificate_vcs"] = "0";
        if (fn == "a") extra["certificate_bitblast"] = "2/2 lean-proved";
        if (fn == "c") extra["certificate_bitblast"] = "0/1 lean-proved";
        if (!note.empty()) extra["certify_note"] = note;
        ojson f = ojson::object();
        f["status"] = status;
        f["extra"] = extra;
        ojson r = ojson::object();
        r["task"] = "t";
        r["function"] = fn;
        r["stage"] = qa::CERT_STAGE;
        r["expected"] = expected;
        r["law_task"] = false;
        r["findings"] = ojson::array({f});
        return r;
    };
    std::vector<ojson> rows = {row("a", true, "PROVED-CERTIFIED", "0"), row("b", true, "PROVED", "0", "VC x: why"),
                               // zero VCs: stays PROVED (a certificate that checks nothing is not one)
                               row("v", true, "PROVED", "0", "no verification conditions (nothing to certify)"),
                               row("c", true, "PROVED-CERTIFIED", "1"), row("d", true, "NEEDS-HARNESS", nullptr),
                               row("e", false, "FAILED", "0")};
    ojson s = qa::certified_summary(rows);
    CHECK(s["loop_free_true"] == 3);
    CHECK(s["loop_free_true_proved"] == 3);
    CHECK(s["loop_free_true_certified"] == 1);
    CHECK(s["loop_free_true_no_vcs"] == 1);
    CHECK(s["loop_free_true_certified_lean"] == 1);
    CHECK(s["loop_free_true_certified_z3"] == 0);
    CHECK(s["loop_free_true_certified_mixed"] == 0);
    CHECK(s["looped_true"] == 1);
    CHECK(s["looped_true_certified"] == 1);
    CHECK(s["not_encoded_true"] == 1);
    CHECK(s["wrong_certified"] == 0);
    ojson want = ojson::array();
    want.push_back({{"task", "t"}, {"function", "b"}, {"note", "VC x: why"}});
    CHECK(s["proved_not_certified"] == want);
    // false twin: PROVED-CERTIFIED on a false function is counted and a wrong proof
    rows.push_back(row("w", false, "PROVED-CERTIFIED", "0"));
    CHECK(qa::certified_summary(rows)["wrong_certified"] == 1);
}

// ---------------------------------------------------------------- release gate

TEST_CASE("qa conformance: a wrong proof fails the release gate") {
    QaTmp tmp;
    auto run = [&](const std::string& status) {
        fs::path fake = fake_engine(tmp.dir, "fake_" + status, status);
        fs::path out = tmp.dir / ("out_" + status);
        int rc = qa::conformance_main({"--prism", fake.string(), "--out", out.string(), "--filter",
                                       "prism/overflow/(add|neg)_", "--no-replay", "-j", "2"});
        return std::pair<int, ojson>{rc, ojson::parse(slurp(out / "metrics.json"))};
    };
    auto [rc, m] = run("PROVED");
    CHECK_MESSAGE(rc == 1, "a wrong proof must fail the release gate");
    CHECK(m["meta"]["wrong_proofs"] == 2);
    CHECK(m["meta"]["release_gate"] == "FAIL");
    CHECK(m["metrics"]["bmc"]["prism"]["completeness"].get<double>() == 1.0);
    std::tie(rc, m) = run("NEEDS-HARNESS");
    CHECK(rc == 0);
    CHECK(m["metrics"]["bmc"]["prism"]["completeness"].get<double>() == 0.0);
    std::tie(rc, m) = run("FAILED");
    CHECK(rc == 0);
    CHECK(m["metrics"]["bmc"]["prism"]["false_alarms"] == 2);
    CHECK(m["metrics"]["bmc"]["prism"]["detection"].get<double>() == 0.0);  // not replayed
    // an engine that cannot run is NOTRUN (exit 3), never a pass
    CHECK(qa::conformance_main({"--prism", (tmp.dir / "missing").string(), "--out", (tmp.dir / "o").string()}) == 3);
}

TEST_CASE("qa conformance: a timed-out PRISM run kills its solver in another process group") {
    QaTmp tmp;
    const fs::path pidfile = tmp.dir / "solver.pid";
    fs::path fake = fake_engine(tmp.dir, "fake_hang", "HANG '" + pidfile.string() + "'");
    spit(tmp.dir / "t.c", "int f(int x) { return x; }\n");
    auto task = mk_task("prism", "p", {{"f", true}});
    task.ident = "t";
    task.source = tmp.dir / "t.c";
    const auto t0 = std::chrono::steady_clock::now();
    auto res = qa::run_prism({fake.string()}, task, {"pir"}, tmp.dir / "work", 2.0, 0);
    CHECK(std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() < 30);
    REQUIRE(res.error);
    CHECK(*res.error == "TIMEOUT");
    long solver = std::atol(slurp(pidfile).c_str());
    REQUIRE(solver > 1);
    CHECK(proc_gone(solver));
}

TEST_CASE("qa proctree: output, exit status, environment, cwd and start failures") {
    QaTmp tmp;
    qa::RunOpts o;
    o.cwd = tmp.dir;
    o.env = {{"PRISM_QA_T", "v1"}};
    auto r = qa::run_tree({"sh", "-c", "echo out; echo err >&2; echo $PRISM_QA_T; pwd; exit 3"}, o);
    CHECK(r.rc == 3);
    CHECK_FALSE(r.timed_out);
    CHECK(r.out == "out\nv1\n" + fs::canonical(tmp.dir).string() + "\n");
    CHECK(r.err == "err\n");
    auto k = qa::run_tree({"sh", "-c", "kill -9 $$"});
    CHECK(k.rc == -9);
    auto missing = qa::run_tree({(tmp.dir / "no-such-binary").string()});
    CHECK(missing.start_failed);
    CHECK(qa::which("sh") != "");
    CHECK(qa::which("prism-qa-no-such-tool") == "");
    // a memory cap every child inherits
    qa::RunOpts lim;
    lim.mem_limit_mb = 64;
    auto big = qa::run_tree({"sh", "-c", "ulimit -v"}, lim);
    CHECK(big.out == "65536\n");
}

// ---------------------------------------------------------------- random programs

TEST_CASE("qa soundness: the random generator matches Python's random.Random") {
    qa::PyRandom r(7);
    CHECK(r.random() == 0.32383276483316237);
    CHECK(qa::i128_str(r.randint(-(i128(1) << 63), (i128(1) << 63) - 1)) == "-7486979218489765845");
    CHECK(qa::i128_str(r.randint(0, (i128(1) << 64) - 1)) == "3960482443532127989");
    CHECK(r.randint(-2000, 2000) == -1648);
    CHECK(r.choice(std::vector<int>{1, 2, 3, 4, 5}) == 4);
    CHECK(r.sample(std::vector<int>{0, 1, 2, 3, 4, 5, 6, 7}, 3) == std::vector<int>{6, 0, 1});
    CHECK(r.getrandbits(40) == 605979998169ULL);
    CHECK(qa::PyRandom((i128(1) << 40) + 5).randint(1, 100) == 65);
    CHECK(qa::PyRandom(-3).randint(1, 100) == 31);
}

TEST_CASE("qa soundness: a seed gives the program it gave before the port") {
    // sha256 of tools/csmith_soundness.py's gen_* output (Python 3.11) for the same seeds
    struct G {
        std::string (*fn)(long long, int);
        long long seed;
        const char* sha;
    };
    const std::vector<G> golden = {
        {qa::gen_inhouse, 1, "e8be46f9a9708b19e386c26df6dd96ea16f4c0635ab49c9aa826ac3507f882c5"},
        {qa::gen_inhouse, 42, "51214f8c7b39ba5e6edb25db3252ea5fb74553054ac74b522744a28773c06622"},
        {qa::gen_inhouse, 1234, "e159f748616ca879a8fdbbda6a98f02305d7029729c6c00a854e27178ebea57c"},
        {qa::gen_inhouse_ptr, 1, "45f57d20f83ad400207cc4f4095d785346646aec5650060466d356f025900bce"},
        {qa::gen_inhouse_ptr, 42, "d9ced85573aba5e79087d2eae5412ead9453774954c78b84fccd5ad84fe924f8"},
        {qa::gen_inhouse_ptr, 1234, "0d2323bb83970901a206a7318d07b30cac1eba83d387de9d6e703ddb5273bcde"},
        {qa::gen_inhouse_loop, 1, "33d19319aeab07f940b028627125eabf4696656955ddbf6f82101ce4b30c087e"},
        {qa::gen_inhouse_loop, 42, "0986e890575bbca9a7c1dc88ea26f2b0536fccfa4a42b5a5c6e8177bb0ebdd61"},
        {qa::gen_inhouse_loop, 1234, "ff08ba4f9137faa0b3c6bce2d77eeaee47e9eb332e2d207dd12844227fb283da"},
    };
    for (const auto& g : golden) {
        CAPTURE(g.seed);
        const std::string text = g.fn(g.seed, 3);
        CHECK(prism::ai::sha256_hex(text) == g.sha);
    }
    const std::string p = qa::gen_inhouse(1, 3);
    CHECK(p.rfind("/* PRISM random soundness program, in-house generator, seed 1 */\n", 0) == 0);
    CHECK(p.find("int rf1_2(") != std::string::npos);
}

TEST_CASE("qa conformance: the self-check input grid is the one it always was") {
    auto rows = qa::input_grid({{"int", "a"}, {"unsigned char", "b"}});
    CHECK(rows.size() == 2462);
    CHECK(repr_rows({rows.front()}) == "[[-2147483648, 0]]");
    CHECK(repr_rows({rows.back()}) == "[[20, 62]]");
    CHECK(repr_rows({rows[rows.size() - 2000]}) == "[[-1499591369, 0]]");
    auto wide = qa::input_grid({{"long long", "a"}, {"unsigned long", "b"}, {"bool", "c"}}, 5, 3);
    CHECK(wide.size() == 1457);
    CHECK(repr_rows(std::vector<std::vector<i128>>(wide.end() - 5, wide.end())) ==
          "[[-6817496105948636342, 0, 0], [1722, 0, 0], [-74, 251, 0], [1680490732736907042, 7875558642732048222, 1], "
          "[-7425657209472503229, 15776127480398349729, 1]]");
    CHECK(prism::ai::sha256_hex(repr_rows(qa::input_grid({{"int", "a"}, {"short", "b"}, {"unsigned", "c"}}))) ==
          "977709b7f9157789e82ce255022894766a69d5976f656f371b430d1eaf166085");
}

// ---------------------------------------------------------------- label self-check

TEST_CASE("qa conformance: a sample of the label self-check passes") {
    // the workflow runs all of it (prism-qa conformance --self-check)
    auto [cc, cxx] = qa::sanitizer_compilers();
    if (qa::which(cc).empty()) {
        WARN_MESSAGE(false, "no sanitizer-capable C compiler: label self-check sample NOTRUN");
        return;
    }
    std::vector<qa::Task> tasks;
    for (auto& t : house_tasks()) {
        if (t.origin != "prism") continue;
        std::string stem = t.source.stem().string();
        std::string head = stem.substr(0, stem.find('_'));
        if (head == "add" || head == "mod" || head == "shl" || head == "arr" || head == "ushort" || head == "div0")
            tasks.push_back(t);
    }
    REQUIRE_FALSE(tasks.empty());
    QaTmp tmp;
    const int jobs = std::max(1, static_cast<int>(std::thread::hardware_concurrency()) / 2);
    auto [recs, bad] = qa::self_check(tasks, tmp.dir, jobs);
    std::string fails;
    for (const auto& r : recs)
        if (r["check"] == "FAIL") fails += r.dump() + "\n";
    CHECK_MESSAGE(bad == 0, fails);
    // false twin: a witness that does not trip a sanitizer fails the check
    spit(tmp.dir / "s" / "x_false.c", "int x_false(int a) { return a + 1; }\n");
    auto t = mk_task("prism", "no-overflow", {{"x_false", false}});
    t.ident = "s/x_false.yml";
    t.source = tmp.dir / "s" / "x_false.c";
    t.witness["x_false"] = {1};
    auto [r2, bad2] = qa::self_check({t}, tmp.dir / "w", 1);
    CHECK(bad2 == 1);
    REQUIRE(r2.size() == 1);
    CHECK(r2[0]["outcome"] == "clean");
    t.witness["x_false"] = {2147483647};
    CHECK(qa::self_check({t}, tmp.dir / "w2", 1).second == 0);
}
