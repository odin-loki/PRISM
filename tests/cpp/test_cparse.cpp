// C/C++ front end (src/prism/cparse.cpp), the report JSON form
// (src/prism/models.cpp), and the static scalar inliner (src/prism/inline.cpp).
// Ported from tests/test_false_positives.py and tests/test_inline.py.
#include <doctest/doctest.h>
#ifdef ERROR
#  undef ERROR
#endif

#include "prism/astlint.hpp"
#include "prism/config.hpp"
#include "prism/cparse.hpp"
#include "prism/journal.hpp"
#include "prism/laws.hpp"
#include "prism/models.hpp"
#include "prism/models_json.hpp"
#include "prism/stages.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace {

namespace fs = std::filesystem;
using Params = std::vector<std::pair<std::string, std::string>>;

fs::path repo_dir(const char* name) {
    return fs::path(PRISM_SOURCE_DIR) / name;
}

std::vector<std::string> names_of(const std::vector<prism::FunctionInfo>& fns) {
    std::vector<std::string> out;
    for (auto& f : fns) out.push_back(f.name);
    return out;
}

std::vector<std::string> names_in(const fs::path& p) {
    return names_of(prism::extract_functions(p, p.filename().string()));
}

const prism::FunctionInfo* find_fn(const std::vector<prism::FunctionInfo>& fns, const std::string& name) {
    for (auto& f : fns)
        if (f.name == name) return &f;
    return nullptr;
}

bool has_hit(const std::vector<prism::Finding>& fs, int line, const char* cls) {
    for (auto& f : fs)
        if (f.status == prism::laws::FAILED && f.line && *f.line == line && f.cls == cls) return true;
    return false;
}

// A scratch file under the process-private temp directory (test_main.cpp).
fs::path scratch_file(const std::string& name, const std::string& text) {
    auto dir = fs::temp_directory_path() / "prism_cparse";
    fs::create_directories(dir);
    auto p = dir / name;
    std::ofstream(p, std::ios::binary) << text;
    return p;
}

prism::FunctionInfo scalar_fn(const char* name, const char* sig, const char* body, bool is_static,
                              const char* kind = "SCALAR", const char* ret = "int") {
    prism::FunctionInfo f;
    f.file = "a.c";
    f.name = name;
    f.kind = kind;
    f.line = 1;
    f.signature = sig;
    f.params = {{"int", "x"}};
    f.return_type = ret;
    f.body = body;
    f.is_static = is_static;
    return f;
}

std::string no_spaces(std::string s) {
    std::erase(s, ' ');
    return s;
}

}  // namespace

// ---------------------------------------------------------------- params

TEST_CASE("cparse: one canonical parameter-type spelling") {
    auto fns = prism::extract_functions_from_text(
        "int f(const char*s, int *out, const std::vector< int > &v, std::map<int, int> m,\n"
        "      unsigned  long n, char **argv, void (*cb)(int, int), int d = 3, int e = (1 < 2)) {\n"
        "    return 0;\n}\n",
        "p.cpp");
    REQUIRE(fns.size() == 1);
    CHECK(fns[0].params == Params{{"char *", "s"},
                                  {"int *", "out"},
                                  {"std::vector<int> &", "v"},
                                  {"std::map<int,int>", "m"},
                                  {"unsigned long", "n"},
                                  {"char **", "argv"},
                                  {"void(*cb)(int,int)", ""},
                                  {"int", "d"},
                                  {"int", "e"}});
    // An array declarator has no trailing name; a lone K&R identifier keeps
    // its old reading (name and type both the word).
    auto arr = prism::extract_functions_from_text("int g(int a[4], restrict int *p) { return 0; }\n", "g.c");
    REQUIRE(arr.size() == 1);
    CHECK(arr[0].params == Params{{"int a[4]", ""}, {"int *", "p"}});
    auto knr = prism::extract_functions_from_text("int k(a, b)\n    int a; int b;\n{\n    return a + b;\n}\n", "k.c");
    REQUIRE(knr.size() == 1);
    CHECK(knr[0].params == Params{{"a", "a"}, {"b", "b"}});
}

TEST_CASE("cparse: qualifiers are words to the classifier") {
    auto fns = prism::extract_functions_from_text(
        "typedef int constant_t;\n"
        "int a(constant_t x) { return x; }\n"
        "int b(const int x) { return x; }\n"
        "int c(const volatile unsigned x) { return (int)x; }\n"
        "int d(const int *p) { return *p; }\n",
        "q.c");
    std::map<std::string, std::string> kinds;
    for (auto& f : fns) kinds[f.name] = f.kind;
    CHECK(kinds["a"] == "OTHER");  // a typedef the classifier does not know
    CHECK(kinds["b"] == "SCALAR");
    CHECK(kinds["c"] == "SCALAR");
    CHECK(kinds["d"] == "POINTER");
}

TEST_CASE("cparse: CXX-VECTOR-INDEX reads a const vector reference parameter") {
    auto dir = fs::temp_directory_path() / "prism_cparse_vec";
    fs::create_directories(dir);
    std::ofstream(dir / "v.cpp", std::ios::binary)
        << "#include <vector>\n"
           "int at(const std::vector<int>& v, int i) {\n"
           "    return v[i];\n"
           "}\n"
           "int guarded(const std::vector<int>& v, int i) {\n"
           "    if (i < v.size()) return v[i];\n"
           "    return 0;\n"
           "}\n";
    auto rows = prism::run_lints({dir / "v.cpp"}, dir);
    CHECK(has_hit(rows, 3, "CXX-VECTOR-INDEX"));
    CHECK_FALSE(has_hit(rows, 6, "CXX-VECTOR-INDEX"));
}

// ---------------------------------------------------------------- text

TEST_CASE("cparse: a comment is whitespace, length-preserving") {
    std::string src = "int/**/x = 1; y = /* c */ f(2);";
    auto s = prism::strip_comments_keep_lines(src);
    CHECK(s == "int    x = 1; y =         f(2);");
    CHECK(s.size() == src.size());
    auto multi = prism::strip_comments_keep_lines("a /* x\ny */ b // z\nc");
    CHECK(multi == "a     \n     b     \nc");
}

TEST_CASE("cparse: body_col survives a block comment (bmc nondet call-site column)") {
    std::string src = "int f(void) { /* c */ int x = __VERIFIER_nondet_int(); return 100 / x; }\n";
    auto fns = prism::extract_functions_from_text(src, "n.c");
    REQUIRE(fns.size() == 1);
    auto& fn = fns[0];
    auto at = fn.body.find("__VERIFIER_nondet_int");
    REQUIRE(at != std::string::npos);
    const int want_col = static_cast<int>(src.find("__VERIFIER_nondet_int")) + 1;
    CHECK(fn.body_line == 1);
    CHECK(fn.body_col + static_cast<int>(at) == want_col);
    auto rows = prism::run_bmc({fn}, 4);
    bool seen = false;
    for (auto& r : rows) {
        if (r.status != prism::laws::FAILED) continue;
        seen = true;
        CHECK(r.cls == "INT-DIV-ZERO");
        CHECK(r.extra["nondet_loc"].find("1:" + std::to_string(want_col)) != std::string::npos);
    }
    CHECK(seen);
}

TEST_CASE("cparse: match_brace refuses a negative index") {
    CHECK(prism::match_brace("{ }", -1) == -1);
    CHECK(prism::match_brace("{ }", 0) == 2);
}

TEST_CASE("cparse: digit separators are not character literals") {
    // Before: the `'` in 1'000 opened a literal that ran to the next `'`, so
    // the comment's `}` closed the function and `return x;` was lost.
    auto fns = prism::extract_functions_from_text(
        "int f(void) { int x = 1'000; /* } */ return x; }\n"
        "int g(int n) {\n    return 0x10'00 + 1'000 / n;\n}\n"
        "int h(int c) {\n    if (c == '0') return c + '\\'';\n    return u8'a' + L'}' + (c ? 1 : '2');\n}\n"
        "int after(void) {\n    return 1;\n}\n",
        "d.c");
    CHECK(names_of(fns) == std::vector<std::string>{"f", "g", "h", "after"});
    REQUIRE(fns.size() == 4);
    CHECK(fns[0].body.find("return x;") != std::string::npos);
    CHECK(fns[2].span == std::pair<int, int>{5, 8});  // L'}' does not end the body
    CHECK(prism::parse_gaps_from_text("int f(void) { int x = 1'000; /* } */ return x; }\n").empty());
    auto s = prism::strip_comments_keep_lines("int x = 1'000; /* } */ char c = '}';");
    CHECK(s == "int x = 1'000;         char c = '}';");
    // The bmc front end reads the separated number: 1'000 / n fails at n == 0.
    auto rows = prism::run_bmc({fns[1]}, 4);
    bool div0 = false;
    for (auto& r : rows) {
        INFO(r.status << " " << r.message);
        CHECK(r.status != prism::laws::ERROR);
        CHECK_FALSE(prism::laws::is_proof(r.status));
        if (r.status == prism::laws::FAILED && r.cls == "INT-DIV-ZERO") div0 = true;
    }
    CHECK(div0);
    // The interval stage too.
    bool idiv0 = false;
    for (auto& r : prism::run_interval({fns[1]}))
        if (r.status == prism::laws::FAILED && r.cls == "INT-DIV-ZERO") idiv0 = true;
    CHECK(idiv0);
}

TEST_CASE("cparse: invalid UTF-8 is scrubbed, byte for byte") {
    CHECK(prism::scrub_utf8("caf\xe9") == "caf\x7f");
    CHECK(prism::scrub_utf8("caf\xc3\xa9") == "caf\xc3\xa9");      // valid, kept
    CHECK(prism::scrub_utf8("\xed\xa0\x80") == "\x7f\x7f\x7f");              // a surrogate
    CHECK(prism::scrub_utf8("\xc0\xaf") == "\x7f\x7f");                   // overlong
    CHECK(prism::scrub_utf8("\xf0\x9f\x98\x80 ok") == "\xf0\x9f\x98\x80 ok");
    auto p = scratch_file("latin1.c",
                          "/* caf\xe9 */\nint f(int x) {\n    const char *s = \"na\xefve\";\n"
                          "    return x + s[0];\n}\n");
    auto fns = prism::extract_functions(p, "latin1.c");
    REQUIRE(fns.size() == 1);
    CHECK(fns[0].body.find("na\x7fve") != std::string::npos);
    // The report writer never throws on a stray byte in a message either.
    prism::RunReport rep;
    rep.root = "r";
    rep.functions = fns;
    prism::StageResult st;
    st.name = "lints";
    st.status = "ok";
    prism::Finding f;
    f.message = "snippet: na\xefve";
    st.findings.push_back(f);
    rep.stages.push_back(st);
    std::string out;
    REQUIRE_NOTHROW(out = rep.dumps());
    auto j = nlohmann::json::parse(out);
    CHECK(j["stages"][0]["findings"][0]["message"] == "snippet: na\xef\xbf\xbdve");
    CHECK(j["functions"][0]["body"].get<std::string>().find("na\x7fve") != std::string::npos);
}

// The scrub changes the program: a Latin-1 'é' (0xE9) in a character
// literal is -23 as signed char, its replacement 0x7F is 127. So no engine
// may model a body that holds one. Twin: the same function spelled with the
// escape '\xE9' (valid text, same value) is still refuted by bmc.
TEST_CASE("cparse: a scrubbed byte is never modelled (no wrong proof)") {
    const std::string raw = "int f(void) {\n    signed char c = '\xe9';\n    return 100 / (c + 23);\n}\n";
    auto p = scratch_file("latin1_div.c", raw);
    auto fns = prism::extract_functions(p, p.string());
    REQUIRE(fns.size() == 1);
    CHECK(fns[0].body.find(prism::SCRUBBED_BYTE) != std::string::npos);
    CHECK(prism::scrubbed_byte_reason(fns[0].body, "x").has_value());
    CHECK_FALSE(prism::scrubbed_byte_reason("return 'a';", "x").has_value());
    for (auto& r : prism::run_bmc(fns, 4)) {
        INFO(r.status << " " << r.message);
        CHECK(r.status == prism::laws::NEEDS_HARNESS);
        CHECK(r.message.find("UNENCODED: byte that is not UTF-8 text") != std::string::npos);
    }
    CHECK(prism::run_interval(fns).empty());
    for (auto& r : prism::run_concolic(fns, 8)) {
        INFO(r.status << " " << r.message);
        CHECK(r.status == prism::laws::NEEDS_HARNESS);
    }
    for (auto& r : prism::run_rapid(fns, 8)) CHECK(r.status == prism::laws::NEEDS_HARNESS);
    // Inlined into a caller the byte still stops the model.
    auto both = prism::extract_functions_from_text(
        "static int g(void) { signed char c = '\xe9'; return c; }\n"
        "int h(void) { return 100 / (g() + 23); }\n", "both.c");
    REQUIRE(both.size() == 2);
    for (auto& r : prism::run_bmc(both, 4)) {
        INFO(r.function.value_or("") << " " << r.status << " " << r.message);
        CHECK_FALSE(prism::laws::is_proof(r.status));
        CHECK(r.status != prism::laws::FAILED);
    }
    // A comment's byte is only blanked text: the function is modelled.
    auto cmt = prism::extract_functions_from_text("int k(int n) { /* caf\xe9 */ return 100 / n; }\n", "c.c");
    REQUIRE(cmt.size() == 1);
    CHECK(cmt[0].body.find(prism::SCRUBBED_BYTE) == std::string::npos);
    // False twin: the escape spelling is modelled and refuted.
    auto twin = prism::extract_functions_from_text(
        "int f(void) {\n    signed char c = '\\xE9';\n    return 100 / (c + 23);\n}\n", "twin.c");
    REQUIRE(twin.size() == 1);
    bool div0 = false;
    for (auto& r : prism::run_bmc(twin, 4)) {
        INFO(r.status << " " << r.message);
        CHECK_FALSE(prism::laws::is_proof(r.status));
        if (r.status == prism::laws::FAILED && r.cls == "INT-DIV-ZERO") div0 = true;
    }
    CHECK(div0);
}

// ---------------------------------------------------------------- report JSON

TEST_CASE("models: load is per field and tolerant") {
    auto p = scratch_file("report.json", R"({
        "root": "r", "started": null, "visibility": "0.5", "notes": ["a", 3],
        "functions": [{"name": "f", "line": "7", "span": [1, "2"], "static": 1,
                       "params": [["int", "x"]], "body_line": 3, "body_col": 9}],
        "stages": [{"name": "bmc", "status": "ok", "records": null, "findings": [
            {"stage": "bmc", "status": "FAILED", "line": "3", "function": ["g", "h"],
             "cls": "INT-DIV-ZERO", "extra": {"unwind": 8, "flag": true, "s": "t"}},
            {"stage": "bmc", "line": "x", "function": null}
        ]}]
    })");
    auto rep = prism::RunReport::load(p);
    REQUIRE(rep);
    CHECK(rep->root == "r");
    CHECK(rep->started == 0.0);
    CHECK(rep->visibility == 0.5);
    CHECK(rep->notes == std::vector<std::string>{"a", "3"});
    REQUIRE(rep->functions.size() == 1);
    auto& fn = rep->functions[0];
    CHECK(fn.name == "f");
    CHECK(fn.line == 7);
    CHECK(fn.kind == "OTHER");
    CHECK(fn.return_type == "int");
    CHECK(fn.is_static);
    CHECK(fn.span == std::pair<int, int>{1, 2});
    CHECK(fn.body_line == 3);
    CHECK(fn.body_col == 9);
    REQUIRE(rep->stages.size() == 1);
    auto& st = rep->stages[0];
    CHECK(st.records == 2);  // null records: the number of findings
    REQUIRE(st.findings.size() == 2);
    CHECK(st.findings[0].line == std::optional<int>(3));
    CHECK(st.findings[0].function == std::optional<std::string>("g"));
    CHECK(st.findings[0].extra["unwind"] == "8");
    CHECK(st.findings[0].extra["flag"] == "true");
    CHECK(st.findings[0].extra["s"] == "t");
    CHECK_FALSE(st.findings[1].line);
    CHECK_FALSE(st.findings[1].function);
    // Not JSON, or not an object: no report.
    CHECK_FALSE(prism::RunReport::load(scratch_file("bad.json", "{")));
    CHECK_FALSE(prism::RunReport::load(scratch_file("arr.json", "[1]")));
    // A record list holding a non-record is the wrong shape (F2), not a
    // report with that entry dropped; a mistyped field inside a record is
    // still read tolerantly (above).
    CHECK_FALSE(prism::RunReport::load(scratch_file("fn5.json", R"({"functions": [{"name": "f"}, 5]})")));
}

TEST_CASE("models: records round-trip through their JSON form") {
    prism::FunctionInfo fn;
    fn.file = "a.c";
    fn.name = "f";
    fn.kind = "SCALAR";
    fn.line = 2;
    fn.signature = "int f(int x)";
    fn.params = {{"int", "x"}};
    fn.body = " return x; ";
    fn.span = {2, 4};
    fn.body_line = 2;
    fn.body_col = 14;
    fn.cxx_std = 20;
    nlohmann::json j = fn;
    CHECK_FALSE(j.contains("cxx_std"));
    auto back = j.get<prism::FunctionInfo>();
    CHECK(back.name == fn.name);
    CHECK(back.params == fn.params);
    CHECK(back.span == fn.span);
    CHECK(back.body_line == 2);
    CHECK(back.body_col == 14);
    CHECK(back.cxx_std == 0);
    // report.json: the function shape without the body position.
    prism::RunReport rr;
    rr.functions = {fn};
    auto rj = nlohmann::json::parse(rr.dumps());
    CHECK(rj["functions"][0]["name"] == "f");
    CHECK_FALSE(rj["functions"][0].contains("body_line"));
    CHECK_FALSE(rj["functions"][0].contains("body_col"));

    prism::Finding f;
    f.stage = "bmc";
    f.status = prism::laws::FAILED;
    f.function = "f";
    f.line = 3;
    f.extra["unwind"] = "8";
    auto fb = nlohmann::json(f).get<prism::Finding>();
    CHECK(fb.function == f.function);
    CHECK(fb.line == f.line);
    CHECK(fb.extra == f.extra);

    // The journal keeps the body position (--resume re-reads functions.json).
    auto out = fs::temp_directory_path() / "prism_cparse_journal";
    prism::journal_reset(out);
    prism::journal_write_functions(out, {fn});
    auto fns = prism::journal_read_functions(out);
    REQUIRE(fns.size() == 1);
    CHECK(fns[0].body_line == 2);
    CHECK(fns[0].body_col == 14);
    prism::StageResult st;
    st.name = "bmc";
    st.status = "ok";
    st.findings.push_back(f);
    st.records = 1;
    prism::journal_append_stage(out, st);
    auto stages = prism::journal_read_stages(out);
    REQUIRE(stages.size() == 1);
    CHECK(stages[0].findings.size() == 1);
    CHECK(stages[0].findings[0].extra["unwind"] == "8");
}

// ---------------------------------------------------------------- head shapes

TEST_CASE("cparse: zlib macros between the type, `*` and the name") {
    auto src =
        "#define FAR\n#define ZEXPORT\n#define ZLIB_INTERNAL\n"
        "typedef unsigned z_crc_t;\ntypedef unsigned long DWORD;\n"
        "const z_crc_t FAR * ZEXPORT get_crc_table()\n{\n    return 0;\n}\n"
        "char ZLIB_INTERNAL *gz_strwinerror (error)\n    DWORD error;\n{\n"
        "    static char buf[8];\n    buf[0] = (char)error;\n    return buf;\n}\n"
        "int after(int q) {\n    return q;\n}\n";
    auto fns = prism::extract_functions_from_text(src, "z.c");
    CHECK(names_of(fns) == std::vector<std::string>{"get_crc_table", "gz_strwinerror", "after"});
    CHECK(prism::parse_gaps_from_text(src).empty());
    for (auto& f : fns) {
        if (f.name == "after") continue;
        CHECK(f.kind == "OTHER");  // behind a macro: not modelled as plain C
        CHECK(f.return_type.find('*') != std::string::npos);
    }
    // A real ALL_CAPS return type is still the return type.
    auto ty = prism::extract_functions_from_text("FILE * open_it(int x) {\n    return 0;\n}\n", "o.c");
    REQUIRE(ty.size() == 1);
    CHECK(ty[0].name == "open_it");
    CHECK(ty[0].return_type == "* FILE");
}

TEST_CASE("cparse: C++ shapes from cxxopts and Catch2") {
    // (1) statement-like macros with no `;` before a class
    auto m = "#define PUSH\n#define IGNORE_WARNING(x)\nPUSH\nIGNORE_WARNING(\"-Wshadow\")\n"
             "class Opt {\npublic:\n    Opt(int v) : m_v(v) {}\n    int get() const { return m_v; }\n"
             "private:\n    int m_v;\n};\n";
    CHECK(names_of(prism::extract_functions_from_text(m, "m.cpp")) ==
          std::vector<std::string>{"Opt::Opt", "Opt::get"});
    CHECK(prism::parse_gaps_from_text(m).empty());

    // (2) a braced base initializer: the body is the real body, exactly
    auto b = "struct Base {\n    Base(bool b, int r) : m_b(b), m_r(r) {}\n    bool m_b;\n    int m_r;\n};\n"
             "struct Bin : Base {\n    Bin(int r, int lhs) : Base{ true, r }, m_lhs(lhs) {\n"
             "        m_lhs += 1;\n    }\n    int m_lhs;\n};\n"
             "Bin::Bin(bool b) : Base{ b, 0 }, m_lhs{ 2 } {\n    m_lhs = 3;\n}\n";
    auto bf = prism::extract_functions_from_text(b, "b.cpp");
    CHECK(names_of(bf) == std::vector<std::string>{"Base::Base", "Bin::Bin", "Bin::Bin"});
    REQUIRE(bf.size() == 3);
    CHECK(no_spaces(bf[1].body) == "\nm_lhs+=1;\n");
    CHECK(bf[1].span == std::pair<int, int>{7, 9});
    CHECK(no_spaces(bf[2].body) == "\nm_lhs=3;\n");
    CHECK(prism::parse_gaps_from_text(b).empty());

    // (3) user-defined literal operators
    auto u = "struct StringRef { const char* p; unsigned long n; };\n"
             "constexpr StringRef operator \"\" _sr(const char* s, unsigned long n) noexcept {\n"
             "    return StringRef{s, n};\n}\n"
             "constexpr StringRef operator\"\"_sv(const char* s, unsigned long n) {\n    return StringRef{s, n};\n}\n";
    CHECK(names_of(prism::extract_functions_from_text(u, "u.cpp")) ==
          std::vector<std::string>{"operator\"\"_sr", "operator\"\"_sv"});
    CHECK(prism::parse_gaps_from_text(u).empty());

    // (4) parenthesised names
    auto pn = "struct L {\n    static constexpr int (min)() { return 0; }\n};\n"
              "static constexpr int (max)() {\n    return 1;\n}\n";
    CHECK(names_of(prism::extract_functions_from_text(pn, "p.cpp")) ==
          std::vector<std::string>{"L::min", "max"});
    CHECK(prism::parse_gaps_from_text(pn).empty());

    // (5) a language linkage, the name on the next line
    auto ex = "extern \"C\" int\nLLVMFuzzerTestOneInput(const unsigned char* d, unsigned long n) {\n"
              "    return 0;\n}\nextern \"C\" int wmain(int argc, wchar_t* argv[]) {\n    return 0;\n}\n";
    CHECK(names_of(prism::extract_functions_from_text(ex, "e.cpp")) ==
          std::vector<std::string>{"LLVMFuzzerTestOneInput", "wmain"});
    CHECK(prism::parse_gaps_from_text(ex).empty());

    // (6) default member brace initializers after a split declaration
    auto dm = "#include <vector>\nclass P {\npublic:\n    void\n    f();\nprivate:\n"
              "    std::vector<int> m_items{};\n    int m_n{0};\n};\nvoid P::f() {\n    m_n = 1;\n}\n";
    CHECK(names_of(prism::extract_functions_from_text(dm, "d.cpp")) == std::vector<std::string>{"P::f"});
    CHECK(prism::parse_gaps_from_text(dm).empty());

    // (7) a partial specialisation with a decltype template argument
    auto ps = "template <typename T, typename = void> struct is_range : std::false_type {};\n"
              "template <typename T>\n"
              "struct is_range<T, decltype(void(begin(std::declval<T>())))> : std::true_type {\n"
              "    int get() { return 1; }\n};\n";
    CHECK(names_of(prism::extract_functions_from_text(ps, "s.cpp")) == std::vector<std::string>{"is_range::get"});
    CHECK(prism::parse_gaps_from_text(ps).empty());

    // A K&R head the patterns cannot read is still a gap, never silent.
    auto knr = "#define zlib_internal\ntypedef unsigned long DWORD;\n"
               "char zlib_internal *strwinerror(error)\n    DWORD error;\n{\n    return 0;\n}\n"
               "int after_gap(int q) {\n    return q;\n}\n";
    CHECK(prism::parse_gaps_from_text(knr) ==
          std::vector<std::pair<int, std::string>>{{3, "char zlib_internal *strwinerror(error)"}});
    CHECK(names_of(prism::extract_functions_from_text(knr, "k.c")) == std::vector<std::string>{"after_gap"});
}

// ---------------------------------------------------------------- corpora
// tests/test_false_positives.py: what test_main.cpp's corpus cases do not
// already hold.

TEST_CASE("false positives: the guard return is not the leak") {
    auto tp = repo_dir("testdata_tp");
    auto hits = prism::run_lints({tp / "leaks.c"}, tp);
    CHECK(has_hit(hits, 12, "MEM-LEAK"));
    CHECK(has_hit(hits, 23, "RES-FD-LEAK"));
    CHECK_FALSE(has_hit(hits, 10, "MEM-LEAK"));
    CHECK_FALSE(has_hit(hits, 21, "RES-FD-LEAK"));
}

TEST_CASE("false positives: parser forms in the corpus") {
    auto fp = repo_dir("testdata_fp");
    CHECK(names_in(fp / "raii.cpp") == std::vector<std::string>{"add_locked", "make_squares", "greet"});
    for (auto& f : prism::extract_functions(fp / "methods.cpp"))
        if (f.name.find("::") != std::string::npos) CHECK(f.kind == "OTHER");
    std::map<std::string, std::string> kinds;
    for (auto& f : prism::extract_functions(fp / "namespaced.cpp")) kinds[f.name] = f.kind;
    CHECK(kinds["twice"] == "OTHER");  // trailing return type
    CHECK(kinds["clamp_to"] == "OTHER");
    // testdata_tp/knr_gap.c: the zlib gz_strwinerror shape is a function now.
    auto gap = repo_dir("testdata_tp") / "knr_gap.c";
    CHECK(prism::parse_gaps(gap).empty());
    CHECK(names_in(gap) == std::vector<std::string>{"strwinerror", "after_gap"});
    // Its unreadable twin stays a gap on disk too.
    auto unread = repo_dir("testdata_tp") / "knr_gap_unread.c";
    CHECK(prism::parse_gaps(unread) ==
          std::vector<std::pair<int, std::string>>{{8, "char zlib_internal *strwinerror(error)"}});
    CHECK(names_in(unread) == std::vector<std::string>{"after_gap"});
}

TEST_CASE("cparse: gap decisions after a macro line or a declaration") {
    // Macro lines cut to a paren-less head that reads as nothing: the uncut
    // head decides, a gap (it was one before the cut existed).
    CHECK(prism::parse_gaps_from_text("DEFINE_X(a)\nWITH_LOCK {\n    x = 1;\n}\n") ==
          std::vector<std::pair<int, std::string>>{{1, "DEFINE_X(a)"}});
    // A paren-less head after `;` (a macro-bodied definition) is still the
    // K&R fallback: the last `(`-segment is the gap.
    CHECK(prism::parse_gaps_from_text("void g(int);\nint x;\nBEGIN_FN {\n    x = 1;\n}\n") ==
          std::vector<std::pair<int, std::string>>{{1, "void g(int)"}});
    // A brace initializer after a declaration is not a gap: a member's
    // default, or a variable at file scope.
    CHECK(prism::parse_gaps_from_text("struct S {\n    void f(int);\n    std::vector<int> m{};\n"
                                      "    int n{3};\n};\n")
              .empty());
    CHECK(prism::parse_gaps_from_text("void f(int);\nstatic std::atomic<int> counter{0};\n").empty());
    // The macro-line cut still reads a declaration after it.
    auto cx = "CXXOPTS_DIAGNOSTIC_PUSH\nCXXOPTS_IGNORE_WARNING(\"-Wx\")\nclass A {\n    int g() { return 1; }\n};\n";
    CHECK(prism::parse_gaps_from_text(cx).empty());
    CHECK(names_of(prism::extract_functions_from_text(cx, "a.cpp")) == std::vector<std::string>{"A::g"});
}

TEST_CASE("false positives: the whole lints stage, AST layer too, is silent on the corpus") {
    auto root = repo_dir("testdata_fp");
    prism::Config cfg;
    cfg.root = root;
    for (auto& f : prism::astlint::run_lints_ast(prism::iter_sources(root), root, cfg)) {
        INFO(f.file << ":" << f.line.value_or(0) << " " << f.cls << " " << f.message);
        CHECK(f.status != prism::laws::FAILED);
    }
}

TEST_CASE("false positives: a C++ qualified name is unencoded, not ERROR") {
    // Catch2 isFalseTest: `flags & ResultDisposition::FalseTest` was concolic
    // ERROR "expected ) got :".
    auto fns = prism::extract_functions_from_text(
        "namespace RD { enum Flags { FalseTest = 4 }; }\n"
        "bool is_false(int flags) { return (flags & RD::FalseTest) != 0; }\n",
        "t.cpp");
    std::vector<std::string> st;
    for (auto& f : prism::run_concolic(fns, 4))
        if (f.function == std::optional<std::string>("is_false")) st.push_back(f.status);
    CHECK(st == std::vector<std::string>{std::string(prism::laws::NEEDS_HARNESS)});
}

// ---------------------------------------------------------------- inline
// tests/test_inline.py: the deterministic static scalar inliner.

TEST_CASE("inline: a static scalar callee is inlined into its caller") {
    auto p = repo_dir("testdata") / "inline_add.c";
    auto fns = prism::extract_functions(p, p.string());
    auto out = prism::inline_static(fns);
    auto* caller = find_fn(out, "caller");
    REQUIRE(caller);
    CHECK(caller->body.find("bump(") == std::string::npos);
    auto flat = no_spaces(caller->body);
    CHECK((flat.find("x+1") != std::string::npos || flat.find("_i0+1") != std::string::npos ||
           flat.find("_ret") != std::string::npos));
}

TEST_CASE("inline: pointer functions are unchanged and the input is not mutated") {
    auto p = repo_dir("testdata") / "ptr_copy.c";
    auto fns = prism::extract_functions(p, p.string());
    std::map<std::string, std::string> orig;
    for (auto& f : fns) orig[f.name] = f.body;
    for (auto& f : prism::inline_static(fns))
        if (f.kind == "POINTER") CHECK(f.body == orig[f.name]);

    auto q = repo_dir("testdata") / "inline_add.c";
    auto add = prism::extract_functions(q, q.string());
    std::vector<std::pair<std::string, std::string>> before, after;
    for (auto& f : add) before.emplace_back(f.name, f.body);
    (void)prism::inline_static(add);
    for (auto& f : add) after.emplace_back(f.name, f.body);
    CHECK(before == after);
}

TEST_CASE("inline: a non-static callee is not inlined") {
    auto out = prism::inline_static({scalar_fn("pub", "int pub(int x)", "return x + 1;", false),
                                     scalar_fn("caller", "int caller(int x)", "return pub(x);", false)});
    auto* caller = find_fn(out, "caller");
    REQUIRE(caller);
    CHECK(caller->body.find("pub(") != std::string::npos);
}

TEST_CASE("inline: one level, no transitive inlining") {
    auto out = prism::inline_static({scalar_fn("inner", "static int inner(int x)", "return x + 1;", true),
                                     scalar_fn("mid", "static int mid(int x)", "return inner(x);", true),
                                     scalar_fn("caller", "int caller(int x)", "return mid(x);", false)});
    auto* caller = find_fn(out, "caller");
    auto* mid = find_fn(out, "mid");
    REQUIRE(caller);
    REQUIRE(mid);
    CHECK(caller->body.find("mid(") != std::string::npos);
    CHECK(mid->body.find("inner(") == std::string::npos);
    auto flat = no_spaces(mid->body);
    CHECK((flat.find("x+1") != std::string::npos || flat.find("_i0+1") != std::string::npos ||
           flat.find("_ret") != std::string::npos));
}

TEST_CASE("inline: assignment, declaration-initialiser and void-helper forms") {
    auto bump = scalar_fn("bump", "static int bump(int x)", "return x + 1;", true);
    for (const char* body : {"int y; y = bump(x); return y;", "int y = bump(x); return y;"}) {
        INFO(body);
        auto out = prism::inline_static({bump, scalar_fn("caller", "int caller(int x)", body, false)});
        auto* caller = find_fn(out, "caller");
        REQUIRE(caller);
        CHECK(caller->body.find("bump(") == std::string::npos);
    }
    auto vbump = scalar_fn("bump", "static void bump(int x)", "(void)x;", true, "VOID", "void");
    auto out = prism::inline_static({vbump, scalar_fn("caller", "int caller(int x)", "bump(x); return x;", false)});
    auto* caller = find_fn(out, "caller");
    REQUIRE(caller);
    CHECK(caller->body.find("bump(") == std::string::npos);
}

TEST_CASE("inline: an inlined caller is not an unconstrained proof") {
    auto p = repo_dir("testdata") / "inline_add.c";
    auto fns = prism::extract_functions(p, p.string());
    std::map<std::string, prism::Finding> recs;
    for (auto& f : prism::run_bmc(fns, 8))
        if (f.function) recs[*f.function] = f;
    REQUIRE(recs.contains("caller"));
    CHECK(recs["caller"].status == prism::laws::FAILED);
    CHECK(recs["caller"].cls == "INT-SIGNED-OVF");
    // Without the callee to inline, the call is unmodelled: never a proof.
    auto* raw = find_fn(fns, "caller");
    REQUIRE(raw);
    auto alone = prism::run_bmc({*raw}, 8);
    REQUIRE(alone.size() == 1);
    INFO(alone[0].message);
    CHECK(alone[0].status == prism::laws::NEEDS_HARNESS);
    CHECK(alone[0].message.find("UNENCODED") != std::string::npos);
}
