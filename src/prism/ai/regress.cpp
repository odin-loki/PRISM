// Regression test generation (roadmap 9.3).
//
// Every FAILED / CRASH finding with a concrete counterexample over a
// function's scalar parameters becomes a unit test in the project's own
// framework (GoogleTest, Catch2, CMake+ctest, pytest or a plain C runner).
// Each test calls the function with the counterexample arguments in a build
// with -fsanitize=undefined,address -fno-sanitize-recover=all, so it FAILS
// while the bug is present (the sanitizer aborts) and passes once it is fixed.
//
// No model is involved: this is deterministic. Tests are written under
// <out>/regression_tests/ and never into the user's tree unless the user asks
// (--write-tests DIR). Generating tests executes nothing; --run compiles and
// runs them now, which executes scanned code and so needs --allow-exec (Law 9).
// A finding that cannot become a test is listed as "unsupported" with the
// reason (Law 7: nothing is dropped quietly).

#include "prism/ai_assist.hpp"

#include "prism/cparse.hpp"
#include "prism/laws.hpp"
#include "prism/sandbox.hpp"

#include "../proc.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <map>
#include <set>
#include <sstream>

namespace prism::ai {
namespace fs = std::filesystem;

namespace {

std::string read_text(const fs::path& p, std::size_t cap = 1u << 20) {
    std::ifstream in(p, std::ios::binary);
    if (!in) return {};
    std::string s;
    s.resize(cap);
    in.read(s.data(), static_cast<std::streamsize>(cap));
    s.resize(static_cast<std::size_t>(in.gcount()));
    return s;
}

bool write_text(const fs::path& p, const std::string& s) {
    std::error_code ec;
    fs::create_directories(p.parent_path(), ec);
    std::ofstream o(p, std::ios::binary);
    o << s;
    return static_cast<bool>(o);
}

std::string lower(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string ident(const std::string& s) {
    std::string o;
    for (char c : s) o += std::isalnum(static_cast<unsigned char>(c)) ? c : '_';
    while (o.find("__") != std::string::npos) o.replace(o.find("__"), 2, "_");
    if (o.empty() || std::isdigit(static_cast<unsigned char>(o[0]))) o = "t_" + o;
    return o;
}

// ------------------------------------------------------------ scalar types
struct Scalar {
    int bits = 0;
    bool is_signed = true;
    bool is_bool = false;
};

std::string norm_type(std::string t) {
    for (const char* q : {"const", "volatile", "register", "static", "inline", "restrict", "__restrict", "struct"}) {
        std::string w = q;
        for (std::size_t p = 0; (p = t.find(w, p)) != std::string::npos;) {
            bool lb = p == 0 || !(std::isalnum(static_cast<unsigned char>(t[p - 1])) || t[p - 1] == '_');
            bool rb = p + w.size() >= t.size() ||
                      !(std::isalnum(static_cast<unsigned char>(t[p + w.size()])) || t[p + w.size()] == '_');
            if (lb && rb && w != "struct") t.replace(p, w.size(), " ");
            else p += w.size();
        }
    }
    std::istringstream is(t);
    std::string w, o;
    while (is >> w) o += (o.empty() ? "" : " ") + w;
    return o;
}

std::optional<Scalar> scalar_of(const std::string& type) {
    static const std::map<std::string, Scalar> table = {
        {"char", {8, true}}, {"signed char", {8, true}}, {"unsigned char", {8, false}},
        {"short", {16, true}}, {"short int", {16, true}}, {"signed short", {16, true}},
        {"unsigned short", {16, false}}, {"unsigned short int", {16, false}},
        {"int", {32, true}}, {"signed", {32, true}}, {"signed int", {32, true}},
        {"unsigned", {32, false}}, {"unsigned int", {32, false}},
        {"long", {64, true}}, {"long int", {64, true}}, {"signed long", {64, true}},
        {"unsigned long", {64, false}}, {"unsigned long int", {64, false}},
        {"long long", {64, true}}, {"long long int", {64, true}}, {"signed long long", {64, true}},
        {"unsigned long long", {64, false}}, {"unsigned long long int", {64, false}},
        {"_Bool", {1, false, true}}, {"bool", {1, false, true}},
        {"int8_t", {8, true}}, {"int16_t", {16, true}}, {"int32_t", {32, true}}, {"int64_t", {64, true}},
        {"uint8_t", {8, false}}, {"uint16_t", {16, false}}, {"uint32_t", {32, false}}, {"uint64_t", {64, false}},
        {"size_t", {64, false}}, {"ssize_t", {64, true}}, {"ptrdiff_t", {64, true}},
        {"intptr_t", {64, true}}, {"uintptr_t", {64, false}},
        {"std::int32_t", {32, true}}, {"std::uint32_t", {32, false}}, {"std::int64_t", {64, true}},
        {"std::uint64_t", {64, false}}, {"std::size_t", {64, false}}};
    auto it = table.find(norm_type(type));
    if (it == table.end()) return std::nullopt;
    return it->second;
}

// Raw cex value -> the integer it denotes (two's complement bit pattern for #x/#b).
std::optional<unsigned __int128> raw_bits(const std::string& raw, bool* negative_decimal, __int128* dec) {
    *negative_decimal = false;
    if (raw == "true") return 1;
    if (raw == "false") return 0;
    try {
        if (raw.starts_with("#x")) {
            if (raw.size() > 34) return std::nullopt;
            unsigned __int128 v = 0;
            for (std::size_t i = 2; i < raw.size(); ++i) {
                char c = static_cast<char>(std::tolower(static_cast<unsigned char>(raw[i])));
                int d = std::isdigit(static_cast<unsigned char>(c)) ? c - '0' : (c >= 'a' && c <= 'f') ? c - 'a' + 10 : -1;
                if (d < 0) return std::nullopt;
                v = v * 16 + static_cast<unsigned>(d);
            }
            return v;
        }
        if (raw.starts_with("#b")) {
            if (raw.size() > 130) return std::nullopt;
            unsigned __int128 v = 0;
            for (std::size_t i = 2; i < raw.size(); ++i) {
                if (raw[i] != '0' && raw[i] != '1') return std::nullopt;
                v = v * 2 + static_cast<unsigned>(raw[i] - '0');
            }
            return v;
        }
        std::string s = raw;
        bool neg = false;
        if (!s.empty() && (s[0] == '-' || s[0] == '+')) {
            neg = s[0] == '-';
            s = s.substr(1);
        }
        if (s.empty()) return std::nullopt;
        unsigned __int128 v = 0;
        int base = 10;
        std::size_t i = 0;
        if (s.size() > 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
            base = 16;
            i = 2;
        }
        for (; i < s.size(); ++i) {
            char c = static_cast<char>(std::tolower(static_cast<unsigned char>(s[i])));
            int d = std::isdigit(static_cast<unsigned char>(c)) ? c - '0'
                    : (base == 16 && c >= 'a' && c <= 'f') ? c - 'a' + 10 : -1;
            if (d < 0) return std::nullopt;
            v = v * static_cast<unsigned>(base) + static_cast<unsigned>(d);
            if (v >> 100) return std::nullopt;
        }
        *negative_decimal = neg;
        *dec = neg ? -static_cast<__int128>(v) : static_cast<__int128>(v);
        return v;
    } catch (...) {
        return std::nullopt;
    }
}

std::string i128_str(__int128 v) {
    if (v == 0) return "0";
    bool neg = v < 0;
    unsigned __int128 u = neg ? static_cast<unsigned __int128>(-(v + 1)) + 1 : static_cast<unsigned __int128>(v);
    std::string s;
    while (u) {
        s += static_cast<char>('0' + static_cast<int>(u % 10));
        u /= 10;
    }
    if (neg) s += '-';
    std::reverse(s.begin(), s.end());
    return s;
}

}  // namespace

std::optional<std::string> c_literal_for(const std::string& type, const std::string& raw) {
    auto sc = scalar_of(type);
    if (!sc) return std::nullopt;
    bool negdec = false;
    __int128 dec = 0;
    auto bits = raw_bits(raw, &negdec, &dec);
    if (!bits) return std::nullopt;
    const bool decimal = !(raw.starts_with("#x") || raw.starts_with("#b") || raw == "true" || raw == "false");
    const int w = sc->bits;
    const unsigned __int128 mask = w >= 128 ? ~static_cast<unsigned __int128>(0)
                                            : ((static_cast<unsigned __int128>(1) << w) - 1);
    // Reinterpret in the parameter's width: decimal values are wrapped into
    // range the same way tools/conformance.py replay() does.
    unsigned __int128 pat = decimal ? static_cast<unsigned __int128>(dec) & mask : (*bits & mask);
    __int128 value;
    if (sc->is_signed && w < 128 && (pat >> (w - 1)) & 1)
        value = static_cast<__int128>(pat) - (static_cast<__int128>(1) << w);
    else
        value = static_cast<__int128>(pat);
    std::string t = norm_type(type);
    if (sc->is_bool) return std::string(value ? "1" : "0");
    std::string lit;
    if (sc->is_signed && w == 64 && value == -(static_cast<__int128>(1) << 63))
        lit = "(-9223372036854775807LL - 1)";
    else if (value < 0)
        lit = "(" + i128_str(value) + "LL)";
    else if (!sc->is_signed && w == 64)
        lit = i128_str(value) + "ULL";
    else
        lit = i128_str(value) + "LL";
    return "(" + t + ")" + lit;
}

std::vector<std::pair<std::string, std::string>> parse_counterexample(const std::string& cex) {
    std::vector<std::pair<std::string, std::string>> out;
    std::size_t i = 0;
    auto is_id = [](char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_'; };
    while (i < cex.size()) {
        if (!is_id(cex[i]) || (i > 0 && is_id(cex[i - 1]))) {
            ++i;
            continue;
        }
        std::size_t b = i;
        while (i < cex.size() && is_id(cex[i])) ++i;
        std::string name = cex.substr(b, i - b);
        std::size_t j = i;
        while (j < cex.size() && cex[j] == ' ') ++j;
        if (j >= cex.size() || cex[j] != '=' || (j + 1 < cex.size() && cex[j + 1] == '=')) continue;
        ++j;
        while (j < cex.size() && cex[j] == ' ') ++j;
        std::size_t vb = j;
        if (j < cex.size() && (cex[j] == '-' || cex[j] == '+' || cex[j] == '#')) ++j;
        while (j < cex.size() && std::isalnum(static_cast<unsigned char>(cex[j]))) ++j;
        std::string val = cex.substr(vb, j - vb);
        bool neg = false;
        __int128 d = 0;
        if (!val.empty() && !std::isdigit(static_cast<unsigned char>(name[0])) && raw_bits(val, &neg, &d)) {
            bool dup = false;
            for (auto& [n, v] : out) dup = dup || n == name;
            if (!dup) out.emplace_back(name, val);
        }
        i = j;
    }
    return out;
}

// ------------------------------------------------------------ framework detection
std::string detect_framework(const fs::path& root, std::string* evidence) {
    auto say = [&](const std::string& fw, const fs::path& p) {
        if (evidence) {
            std::error_code ec;
            auto rel = fs::relative(p, root, ec);
            *evidence = ec || rel.empty() ? p.string() : rel.generic_string();
        }
        return fw;
    };
    std::error_code ec;
    if (fs::is_regular_file(root, ec)) {
        if (evidence) *evidence = "single file";
        return root.extension() == ".py" ? "pytest" : "plain";
    }
    std::optional<fs::path> gtest, catch2, ctest, pytest;
    std::size_t seen = 0;
    const std::set<std::string> skip_dirs = {".git", "build", "third_party", "node_modules", "prism-out",
                                             "prism-out-gui", ".venv", "venv", "__pycache__", "_deps"};
    fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, ec), end;
    for (; !ec && it != end; it.increment(ec)) {
        if (++seen > 20000) break;
        const auto& p = it->path();
        auto name = p.filename().string();
        if (it->is_directory(ec)) {
            if (skip_dirs.contains(name) || name.starts_with("cmake-build") || it.depth() >= 5)
                it.disable_recursion_pending();
            continue;
        }
        auto ext = lower(p.extension().string());
        bool cmake = name == "CMakeLists.txt" || ext == ".cmake";
        bool code = ext == ".c" || ext == ".cc" || ext == ".cpp" || ext == ".cxx" || ext == ".h" || ext == ".hpp";
        if (cmake || code || name == "meson.build" || name == "Makefile") {
            auto txt = read_text(p, 256 * 1024);
            if (!gtest && (txt.find("gtest/gtest.h") != std::string::npos || txt.find("GTest::") != std::string::npos ||
                           txt.find("find_package(GTest") != std::string::npos || txt.find("googletest") != std::string::npos))
                gtest = p;
            if (!catch2 && (txt.find("catch2/catch") != std::string::npos || txt.find("Catch2::") != std::string::npos ||
                            txt.find("find_package(Catch2") != std::string::npos || txt.find("\"catch.hpp\"") != std::string::npos))
                catch2 = p;
            if (!ctest && name == "CMakeLists.txt" && it.depth() <= 1 &&
                (txt.find("enable_testing") != std::string::npos || txt.find("add_test") != std::string::npos ||
                 txt.find("include(CTest)") != std::string::npos))
                ctest = p;
            if (!ctest && name == "CMakeLists.txt" && it.depth() == 0) ctest = p;
        }
        if (!pytest && it.depth() <= 1 &&
            (name == "pyproject.toml" || name == "setup.py" || name == "setup.cfg" || name == "pytest.ini" ||
             name == "conftest.py" || name == "tox.ini"))
            pytest = p;
    }
    if (gtest) return say("gtest", *gtest);
    if (catch2) return say("catch2", *catch2);
    if (ctest) return say("ctest", *ctest);
    if (pytest) return say("pytest", *pytest);
    if (evidence) *evidence = "no test framework found";
    return "plain";
}

namespace {

const std::string kSanFlags = "-O0 -g -fsanitize=undefined,address -fno-sanitize-recover=all -fno-omit-frame-pointer";
// Uninitialised reads are invisible to UBSan/ASan: MemorySanitizer (clang,
// cannot be combined with ASan) for those classes.
const std::string kMsanFlags =
    "-O0 -g -fsanitize=memory -fsanitize-memory-track-origins -fno-sanitize-recover=all -fno-omit-frame-pointer";

std::string flags_for(const std::string& cls) {
    std::string u = cls;
    for (auto& ch : u) ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
    return u.find("UNINIT") != std::string::npos ? kMsanFlags : kSanFlags;
}

bool is_cxx_file(const std::string& f) {
    auto e = lower(fs::path(f).extension().string());
    return e == ".cpp" || e == ".cc" || e == ".cxx" || e == ".c++" || e == ".hpp" || e == ".hh";
}

std::string cstr(const std::string& s) {
    std::string o = "\"";
    for (char c : s) {
        if (c == '"' || c == '\\') o += '\\';
        if (c == '\n') {
            o += "\\n";
            continue;
        }
        o += c;
    }
    return o + "\"";
}

std::string comment_safe(std::string s) {
    for (std::size_t p; (p = s.find("*/")) != std::string::npos;) s.replace(p, 2, "* /");
    for (auto& c : s)
        if (c == '\n' || c == '\r') c = ' ';
    return s;
}

struct Case {
    RegressTest t;
    fs::path source;              // absolute path of the user's source
    std::string include;          // spelling used in #include
    std::string call;             // "fn((int)(1LL), ...)"
    bool cxx = false;
    std::string cex;
    std::string stage, status;
};

fs::path resolve_source(const RunReport& report, const std::string& file) {
    fs::path p(file);
    std::error_code ec;
    if (p.is_absolute() && fs::is_regular_file(p, ec)) return p;
    fs::path root(report.root);
    if (fs::is_regular_file(root, ec)) {
        if (root.filename() == p.filename() || file.empty()) return root;
        root = root.parent_path();
    }
    if (fs::is_regular_file(root / p, ec)) return fs::weakly_canonical(root / p, ec);
    if (fs::is_regular_file(p, ec)) return fs::absolute(p);
    return {};
}

std::optional<FunctionInfo> find_fn(const RunReport& report, const fs::path& src, const Finding& f) {
    for (const auto& fn : report.functions) {
        if (fn.name != *f.function) continue;
        std::error_code ec;
        auto a = resolve_source(report, fn.file);
        if (!a.empty() && fs::equivalent(a, src, ec)) return fn;
    }
    for (auto& fn : extract_functions(src, f.file))
        if (fn.name == *f.function) return fn;
    return std::nullopt;
}

std::string case_source(const Case& c, bool with_main) {
    std::ostringstream o;
    o << "/* PRISM regression test " << c.t.name << " (roadmap 9.3), generated from " << c.t.finding_id << ".\n"
      << " * " << c.status << " " << comment_safe(c.stage) << " " << comment_safe(c.t.file) << " `"
      << comment_safe(c.t.function) << "` " << comment_safe(c.t.cls) << "\n"
      << " * counterexample: " << comment_safe(c.cex) << "\n"
      << " * Built with -fsanitize=undefined,address -fno-sanitize-recover=all: the call\n"
      << " * aborts (test FAILS) while the defect is present and returns (test passes) once fixed. */\n"
      << "#define main prism_regress_user_main_\n"
      << "#include " << cstr(c.include) << "\n"
      << "#undef main\n\n"
      << "static volatile int prism_regress_sink_;\n";
    if (c.cxx) o << "extern \"C++\" ";
    o << "void prism_regress_" << c.t.name << "(void) {\n    " << c.call << ";\n}\n";
    if (with_main) {
        o << "\n#ifndef PRISM_REGRESS_NO_MAIN\nint main(void) {\n    prism_regress_" << c.t.name
          << "();\n    return 0;\n}\n#endif\n";
    }
    return o.str();
}

std::string case_file(const Case& c) { return c.t.name + (c.cxx ? ".cpp" : ".c"); }

std::string sanitizer_env_cmake() {
    return "\"UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1;ASAN_OPTIONS=detect_leaks=0:abort_on_error=0\"";
}

void write_plain(const fs::path& dir, const std::vector<Case>& cases) {
    std::ostringstream sh;
    sh << "#!/bin/sh\n# PRISM regression tests (roadmap 9.3). Each test FAILS while its defect is present.\n"
       << "# CC / CXX select the compiler (default clang / clang++).\n"
       << "cd \"$(dirname \"$0\")\" || exit 2\nCC=${CC:-clang}\nCXX=${CXX:-clang++}\n"
       << "export UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 ASAN_OPTIONS=detect_leaks=0\n"
       << "mkdir -p bin\nfail=0\n";
    for (auto& c : cases) {
        sh << "if " << (c.cxx ? "$CXX" : "$CC") << " " << c.t.flags << " " << case_file(c) << " -o bin/" << c.t.name
           << (c.cxx ? "" : " -lm") << " 2>bin/" << c.t.name << ".build.log; then\n"
           << "  if ./bin/" << c.t.name << " >bin/" << c.t.name << ".log 2>&1; then echo \"PASS " << c.t.name
           << "\"; else echo \"FAIL " << c.t.name << " (" << comment_safe(c.t.cls) << ")\"; fail=1; fi\n"
           << "else echo \"FAIL " << c.t.name << " (does not compile, see bin/" << c.t.name << ".build.log)\"; fail=1; fi\n";
    }
    sh << "exit $fail\n";
    write_text(dir / "run_tests.sh", sh.str());
    std::error_code ec;
    fs::permissions(dir / "run_tests.sh", fs::perms::owner_exec | fs::perms::group_exec | fs::perms::others_exec,
                    fs::perm_options::add, ec);
}

void write_cmake(const fs::path& dir, const std::vector<Case>& cases, const std::string& fw) {
    std::ostringstream o;
    o << "# PRISM regression tests (roadmap 9.3), framework: " << fw << ".\n"
      << "# cmake -S . -B build && cmake --build build && ctest --test-dir build\n"
      << "# Each test FAILS while its defect is present (sanitizer abort) and passes once fixed.\n"
      << "cmake_minimum_required(VERSION 3.16)\nproject(prism_regression C CXX)\nenable_testing()\n"
      << "set(PRISM_SAN_FLAGS " << kSanFlags << ")\n"
      << "set(PRISM_MSAN_FLAGS " << kMsanFlags << ")  # uninitialised-read classes\n";
    if (fw == "gtest") o << "find_package(GTest REQUIRED)\n";
    if (fw == "catch2") o << "find_package(Catch2 3 REQUIRED)\n";
    for (auto& c : cases) {
        std::string exe = c.t.name;
        if (fw == "gtest") {
            o << "add_executable(" << exe << " " << exe << "_test.cpp " << case_file(c) << ")\n"
              << "set_source_files_properties(" << case_file(c) << " PROPERTIES COMPILE_DEFINITIONS PRISM_REGRESS_NO_MAIN)\n"
              << "target_link_libraries(" << exe << " PRIVATE GTest::gtest_main)\n";
        } else if (fw == "catch2") {
            o << "add_executable(" << exe << " " << exe << "_test.cpp " << case_file(c) << ")\n"
              << "set_source_files_properties(" << case_file(c) << " PROPERTIES COMPILE_DEFINITIONS PRISM_REGRESS_NO_MAIN)\n"
              << "target_link_libraries(" << exe << " PRIVATE Catch2::Catch2WithMain)\n";
        } else {
            o << "add_executable(" << exe << " " << case_file(c) << ")\n";
            if (!c.cxx) o << "target_link_libraries(" << exe << " PRIVATE m)\n";
        }
        const std::string fv = c.t.flags == kMsanFlags ? "${PRISM_MSAN_FLAGS}" : "${PRISM_SAN_FLAGS}";
        o << "target_compile_options(" << exe << " PRIVATE " << fv << ")\n"
          << "target_link_options(" << exe << " PRIVATE " << fv << ")\n"
          << "add_test(NAME " << exe << " COMMAND " << exe << ")\n"
          << "set_tests_properties(" << exe << " PROPERTIES ENVIRONMENT " << sanitizer_env_cmake() << ")\n";
        if (fw == "gtest") {
            std::ostringstream t;
            t << "// PRISM regression test (GoogleTest) for " << c.t.finding_id << ".\n"
              << "#include <gtest/gtest.h>\n#include <cstdlib>\n\nvoid prism_regress_" << c.t.name << "(void);\n\n"
              << "// The call runs in a death-test child: a sanitizer abort fails this test\n"
              << "// without taking the other tests down.\n"
              << "TEST(PrismRegression, " << c.t.name << ") {\n"
              << "    EXPECT_EXIT({ prism_regress_" << c.t.name << "(); std::exit(0); }, ::testing::ExitedWithCode(0), \".*\");\n}\n";
            write_text(dir / (exe + "_test.cpp"), t.str());
        } else if (fw == "catch2") {
            std::ostringstream t;
            t << "// PRISM regression test (Catch2) for " << c.t.finding_id << ".\n"
              << "#include <catch2/catch_test_macros.hpp>\n\nvoid prism_regress_" << c.t.name << "(void);\n\n"
              << "// A sanitizer abort ends this test binary with a failure.\n"
              << "TEST_CASE(\"" << c.t.name << "\", \"[prism][regression]\") {\n"
              << "    prism_regress_" << c.t.name << "();\n    SUCCEED();\n}\n";
            write_text(dir / (exe + "_test.cpp"), t.str());
        }
    }
    write_text(dir / "CMakeLists.txt", o.str());
}

void write_pytest(const fs::path& dir, const std::vector<Case>& cases) {
    std::ostringstream o;
    o << "\"\"\"PRISM regression tests (roadmap 9.3), pytest.\n\n"
      << "Each case compiles a C harness with -fsanitize=undefined,address and runs the\n"
      << "counterexample; the test FAILS while the defect is present and passes once fixed.\n"
      << "CC / CXX select the compiler (default clang / clang++).\n\"\"\"\n\n"
      << "import os\nimport shutil\nimport subprocess\nfrom pathlib import Path\n\nimport pytest\n\n"
      << "HERE = Path(__file__).resolve().parent\n"
      << "CASES = [\n";
    for (auto& c : cases)
        o << "    (" << cstr(c.t.name) << ", " << cstr(case_file(c)) << ", " << (c.cxx ? "True" : "False") << ", "
          << cstr(c.t.flags) << "),\n";
    o << "]\n\n\n"
      << "@pytest.mark.parametrize(\"name,src,cxx,flags\", CASES, ids=[c[0] for c in CASES])\n"
      << "def test_prism_regression(name, src, cxx, flags, tmp_path):\n"
      << "    cc = os.environ.get(\"CXX\" if cxx else \"CC\", \"clang++\" if cxx else \"clang\")\n"
      << "    if not shutil.which(cc):\n"
      << "        pytest.skip(f\"NOTRUN: compiler {cc} not found\")\n"
      << "    exe = tmp_path / name\n"
      << "    b = subprocess.run([cc, *flags.split(), str(HERE / src), \"-o\", str(exe)] + ([] if cxx else [\"-lm\"]),\n"
      << "                       capture_output=True, text=True)\n"
      << "    assert b.returncode == 0, b.stderr\n"
      << "    env = dict(os.environ, UBSAN_OPTIONS=\"halt_on_error=1:print_stacktrace=1\", ASAN_OPTIONS=\"detect_leaks=0\")\n"
      << "    r = subprocess.run([str(exe)], capture_output=True, text=True, env=env, timeout=60)\n"
      << "    assert r.returncode == 0 and \"runtime error\" not in r.stderr, r.stderr[-2000:]\n";
    write_text(dir / "test_prism_regression.py", o.str());
}

std::string probe_cc(const std::string& want, bool cxx) {
    if (!want.empty()) return want;
    return cxx ? "clang++" : "clang";
}

}  // namespace

RegressResult generate_regression_tests(const RunReport& report, const RegressOptions& opt) {
    RegressResult res;
    fs::path root(report.root);
    res.framework = opt.framework;
    if (opt.framework == "auto" || opt.framework.empty())
        res.framework = detect_framework(root, &res.framework_evidence);
    else
        res.framework_evidence = "requested";
    const fs::path dir = opt.out_dir;
    std::error_code ec;
    fs::create_directories(dir, ec);

    std::vector<Case> cases;
    std::set<std::string> seen_calls;
    std::map<std::string, int> name_count;
    for (const auto& ref : enumerate_findings(report)) {
        const Finding& f = *ref.f;
        if (f.status != laws::FAILED && f.status != laws::CRASH) continue;
        RegressTest t;
        t.finding_id = ref.id;
        t.file = f.file;
        t.function = f.function.value_or("");
        t.cls = f.cls;
        auto unsupported = [&](std::string why) {
            t.status = "unsupported";
            t.detail = std::move(why);
            res.tests.push_back(t);
        };
        if (f.counterexample.empty()) {
            unsupported("no concrete counterexample");
            continue;
        }
        if (!f.function || f.function->empty()) {
            unsupported("finding names no function");
            continue;
        }
        auto src = resolve_source(report, f.file);
        if (src.empty()) {
            unsupported("source file not found: " + f.file);
            continue;
        }
        auto fn = find_fn(report, src, f);
        if (!fn) {
            unsupported("function definition not found in " + f.file);
            continue;
        }
        auto vals = parse_counterexample(f.counterexample);
        std::vector<std::string> args, shown, missing;
        bool ok = true;
        for (const auto& [ptype, pname] : fn->params) {
            if (!scalar_of(ptype)) {
                unsupported("parameter '" + ptype + " " + pname + "' is not a scalar (a pointer/struct needs a harness)");
                ok = false;
                break;
            }
            std::string raw = "0";
            auto it = std::find_if(vals.begin(), vals.end(), [&](auto& kv) { return kv.first == pname; });
            if (it != vals.end()) raw = it->second;
            else missing.push_back(pname);
            auto lit = c_literal_for(ptype, raw);
            if (!lit) {
                unsupported("value '" + raw + "' of " + pname + " cannot be written as a " + ptype + " literal");
                ok = false;
                break;
            }
            args.push_back(*lit);
            shown.push_back(pname + "=" + *lit);
        }
        if (!ok) continue;
        if (!fn->params.empty() && missing.size() == fn->params.size()) {
            unsupported("counterexample names none of the parameters: " + f.counterexample);
            continue;
        }
        std::string call = *f.function + "(";
        for (std::size_t i = 0; i < args.size(); ++i) call += (i ? ", " : "") + args[i];
        call += ")";
        if (scalar_of(fn->return_type)) call = "if (" + call + ") prism_regress_sink_ = 1";  // a use (MSan)
        else if (fn->return_type != "void") call = "(void)" + call;
        auto key = src.string() + "|" + call;
        std::string base = ident(fs::path(f.file).stem().string() + "_" + *f.function);
        if (!seen_calls.insert(key).second) {
            t.status = "duplicate";
            t.detail = "same function and arguments as an earlier test";
            t.args = [&] {
                std::string s;
                for (auto& x : shown) s += (s.empty() ? "" : ", ") + x;
                return s;
            }();
            res.tests.push_back(t);
            continue;
        }
        int n = ++name_count[base];
        t.name = n == 1 ? base : base + "_" + std::to_string(n);
        for (auto& x : shown) t.args += (t.args.empty() ? "" : ", ") + x;
        if (!missing.empty()) {
            t.detail = "parameters absent from the counterexample (any value triggers it) set to 0:";
            for (auto& m : missing) t.detail += " " + m;
        }
        Case c;
        c.source = src;
        // Include spelling: relative to the output directory when it can be
        // (tests written into the user's tree stay relocatable with it).
        c.include = src.generic_string();
        if (opt.relative_includes) {
            auto rel = fs::relative(src, fs::absolute(dir), ec);
            if (!ec && !rel.empty()) c.include = rel.generic_string();
        }
        c.cxx = is_cxx_file(src.string());
        c.call = call;
        c.cex = f.counterexample;
        c.stage = f.stage;
        c.status = f.status;
        t.harness = t.name + (c.cxx ? ".cpp" : ".c");
        t.flags = flags_for(f.cls);
        t.status = "written";
        c.t = t;
        cases.push_back(c);
        res.tests.push_back(t);
    }

    for (auto& c : cases) write_text(dir / case_file(c), case_source(c, true));
    const std::string fw = res.framework;
    if (fw == "gtest" || fw == "catch2" || fw == "ctest") write_cmake(dir, cases, fw);
    else if (fw == "pytest") write_pytest(dir, cases);
    write_plain(dir, cases);  // always: the framework-free runner

    // --run: compile and run each case now (executes scanned code, Law 9).
    if (opt.run) {
        for (auto& t : res.tests) {
            if (t.status != "written") continue;
            if (!opt.allow_exec) {
                t.status = "NOTRUN";
                t.detail = sandbox::exec_message("regression test run");
                continue;
            }
            const bool cxx = is_cxx_file(t.harness);
            std::string cc = probe_cc(cxx ? opt.cxx : opt.cc, cxx);
            fs::path bin = dir / "bin";
            fs::create_directories(bin, ec);
            std::vector<std::string> argv{cc};
            std::istringstream fl(t.flags);
            for (std::string w; fl >> w;) argv.push_back(w);
            argv.push_back((dir / t.harness).string());
            argv.push_back("-o");
            argv.push_back((bin / t.name).string());
            if (!cxx) argv.push_back("-lm");
            auto b = detail::run_process(argv, 180.0);
            if (b.failed || b.rc != 0) {
                t.status = "compile-error";
                t.detail = b.failed ? cc + " not found" : b.text.substr(b.text.size() > 600 ? b.text.size() - 600 : 0);
                continue;
            }
            // halt_on_error so the first UB report ends the run with a failure.
            auto r = detail::run_process(
                sandbox::wrap_argv({"env", "UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1",
                                    "ASAN_OPTIONS=detect_leaks=0", (bin / t.name).string()},
                                   bin),
                opt.timeout_s);
            auto hit = r.text.find("runtime error:");
            if (hit == std::string::npos) hit = r.text.find("ERROR: AddressSanitizer");
            if (r.rc != 0 || r.timed_out || hit != std::string::npos) {
                t.status = "reproduces";
                t.detail = hit != std::string::npos ? r.text.substr(hit, r.text.find('\n', hit) - hit)
                                                    : (r.timed_out ? "timeout" : "exit " + std::to_string(r.rc));
            } else {
                t.status = "does-not-reproduce";
                t.detail = "the call returned without a sanitizer report (the defect may need a different build, "
                           "or is not observable by UBSan/ASan)";
            }
        }
    }

    nlohmann::json m;
    m["schema"] = 1;
    m["kind"] = "prism-regression-tests";
    m["framework"] = res.framework;
    m["framework_evidence"] = res.framework_evidence;
    m["sanitizer_flags"] = kSanFlags;
    m["msan_flags"] = kMsanFlags;
    m["note"] = "Each test fails while its defect is present and passes once it is fixed. "
                "Generated from report.json; no model involved; statuses in the report are unchanged.";
    m["tests"] = nlohmann::json::array();
    for (auto& t : res.tests)
        m["tests"].push_back({{"name", t.name}, {"finding", t.finding_id}, {"file", t.file},
                              {"function", t.function}, {"cls", t.cls}, {"args", t.args},
                              {"harness", t.harness}, {"flags", t.flags}, {"status", t.status},
                              {"detail", t.detail}});
    res.manifest = dir / "manifest.json";
    write_text(res.manifest, m.dump(2));
    return res;
}

int regress_main(int argc, char** argv) {
    fs::path report_path = "prism-out/report.json";
    fs::path write_dir;
    RegressOptions opt;
    bool report_given = false;
    for (int i = 0; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&]() -> std::string { return i + 1 < argc ? argv[++i] : ""; };
        if (a == "--report") {
            report_path = next();
            report_given = true;
        } else if (a == "--write-tests") write_dir = next();
        else if (a == "--framework") opt.framework = next();
        else if (a == "--run") opt.run = true;
        else if (a == "--allow-exec") opt.allow_exec = true;
        else if (a == "--cc") opt.cc = next();
        else if (a == "--cxx") opt.cxx = next();
        else if (a == "-h" || a == "--help") {
            std::cout << "prism regress [--report OUT/report.json] [--write-tests DIR] [--framework "
                         "auto|gtest|catch2|ctest|pytest|plain]\n"
                         "              [--run --allow-exec] [--cc CC] [--cxx CXX]\n"
                         "Turns every FAILED/CRASH finding with a concrete counterexample into a unit test\n"
                         "that fails while the defect is present (UBSan+ASan build). Tests go to\n"
                         "<report dir>/regression_tests/ unless --write-tests DIR. --run compiles and runs\n"
                         "them now (executes scanned code: needs --allow-exec, Law 9).\n";
            return 0;
        } else if (!a.starts_with("-") && !report_given) {
            fs::path p(a);
            report_path = fs::is_directory(p) ? p / "report.json" : p;
            report_given = true;
        }
    }
    auto report = RunReport::load(report_path);
    if (!report) {
        std::cerr << "ERROR regress: cannot read " << report_path.string() << "\n";
        return 2;
    }
    opt.relative_includes = !write_dir.empty();
    opt.out_dir = write_dir.empty() ? fs::absolute(report_path).parent_path() / "regression_tests" : fs::absolute(write_dir);
    if (opt.framework != "auto" && opt.framework != "gtest" && opt.framework != "catch2" &&
        opt.framework != "ctest" && opt.framework != "pytest" && opt.framework != "plain") {
        std::cerr << "--framework expects auto|gtest|catch2|ctest|pytest|plain\n";
        return 2;
    }
    auto res = generate_regression_tests(*report, opt);
    std::map<std::string, int> by;
    for (auto& t : res.tests) ++by[t.status];
    std::cout << "framework " << res.framework << " (" << res.framework_evidence << ")\n";
    std::cout << "tests " << opt.out_dir.string() << "  (manifest " << res.manifest.string() << ")\n";
    for (auto& [k, v] : by) std::cout << "  " << k << ": " << v << "\n";
    for (auto& t : res.tests)
        if (t.status != "written")
            std::cout << "  " << t.status << " " << t.finding_id << " " << t.function << ": " << t.detail << "\n";
    return 0;
}

}  // namespace prism::ai
