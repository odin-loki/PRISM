// Differential test of the std::initializer_list model header
// (src/prism/pir/models/cxx/initializer_list). Same shape as
// tests/cpp/test_cxx_list.cpp: the trace is compiled twice, once against
// libstdc++ and once with -isystem on the model directory so
// `#include <initializer_list>` is this header. CMake's prism_tests source
// list is explicit (not a glob).

#include <doctest/doctest.h>

#include "cxx_model_harness.hpp"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

namespace cxxh = prism_test::cxx;
using cxxh::fwd;
using cxxh::repo;
using cxxh::run_cmd;
using cxxh::run_lines;

fs::path model_header() { return repo() / "src" / "prism" / "pir" / "models" / "cxx" / "initializer_list"; }

struct IlCtx {
    fs::path tmp;
    std::string cxx;
    bool ok = false;
};

IlCtx& ctx() {
    static IlCtx c;
    return c;
}

bool probe_clang_asan(const std::string& cxx, const fs::path& tmp) {
    return cxxh::probe_clang_asan(cxx, tmp,
                                  "#include <initializer_list>\n"
                                  "int main() {\n"
                                  "  std::initializer_list<int> e;\n"
                                  "  return (e.size() == 0 && e.begin() == e.end()) ? 0 : 1;\n"
                                  "}\n",
                                  cxxh::asan_flags());
}

fs::path build_prog(const std::string& cxx, const fs::path& tmp, bool with_models) {
    return cxxh::build_prog(cxx, tmp, cxxh::trace_src("initializer_list_trace.cpp"), "ilist_trace", cxxh::asan_flags(), with_models);
}

// libstdc++ initializer_list has size() and no empty(), so this trace is
// compiled only with the model directory on the include path.
fs::path build_size_trace(const std::string& cxx, const fs::path& tmp) {
    const fs::path models = repo() / "src" / "prism" / "pir" / "models" / "cxx";
    const std::vector<std::string> flags{"-std=c++23", "-D_GLIBCXX_ASSERTIONS", "-fsanitize=address,undefined",
                                         "-fno-sanitize-recover=all", "-g", "-O1", "-w"};
    fs::path out = tmp / "ilist_size.model";
    std::vector<std::string> argv{cxx};
    argv.insert(argv.end(), flags.begin(), flags.end());
    argv.push_back("-isystem");
    argv.push_back(models.string());
    argv.push_back((repo() / "tests" / "cxx_models" / "initializer_list_size_trace.cpp").string());
    argv.push_back("-o");
    argv.push_back(out.string());
    REQUIRE(run_cmd(argv));
    return out;
}

bool has_line(const std::vector<std::string>& lines, const std::string& want) {
    for (const auto& line : lines)
        if (line == want) return true;
    return false;
}

std::string slurp(const fs::path& p) {
    std::ifstream in(p);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

std::string body_of(const std::string& src, const std::string& sig) {
    const auto at = src.find(sig);
    if (at == std::string::npos) return {};
    const auto brace = src.find('{', at);
    if (brace == std::string::npos) return {};
    int depth = 0;
    for (std::size_t i = brace; i < src.size(); ++i) {
        if (src[i] == '{')
            ++depth;
        else if (src[i] == '}') {
            if (--depth == 0) return src.substr(brace, i - brace + 1);
        }
    }
    return {};
}

std::string slice_between(const std::string& src, const std::string& a, const std::string& b) {
    const auto i = src.find(a);
    if (i == std::string::npos) return {};
    const auto j = src.find(b, i + a.size());
    if (j == std::string::npos) return {};
    return src.substr(i, j - i);
}

int count_of(const std::string& s, const std::string& needle) {
    int n = 0;
    for (std::size_t at = 0; (at = s.find(needle, at)) != std::string::npos; at += needle.size()) ++n;
    return n;
}

bool loads_element(const std::string& body) {
    return body.find("_M_array") != std::string::npos || body.find('*') != std::string::npos ||
           body.find('[') != std::string::npos || body.find("->") != std::string::npos ||
           body.find("begin") != std::string::npos || body.find("end") != std::string::npos;
}

}  // namespace

TEST_CASE("cxx initializer_list model: header is a file and the destructor does not free") {
    const fs::path header = model_header();
    REQUIRE(fs::is_regular_file(header));
    CHECK_FALSE(fs::is_directory(header));
    const std::string text = slurp(header);
    REQUIRE_FALSE(text.empty());
    CHECK(text.find("_GLIBCXX_INITIALIZER_LIST") != std::string::npos);
    CHECK(text.find("#pragma GCC system_header") != std::string::npos);
    CHECK(text.find("operator[]") == std::string::npos);
    CHECK(text.find("delete") == std::string::npos);
    CHECK(text.find("#include <vector") == std::string::npos);
    CHECK(text.find("#include <list") == std::string::npos);
    CHECK(text.find("#include <span") == std::string::npos);
    CHECK(text.find("#include <string") == std::string::npos);
    CHECK(text.find("#include <optional") == std::string::npos);
    CHECK(text.find("#include <memory") == std::string::npos);
    CHECK(text.find("#include <algorithm") == std::string::npos);
    const auto dtor = text.find("~initializer_list");
    REQUIRE(dtor != std::string::npos);
    const auto semi = text.find(';', dtor);
    REQUIRE(semi != std::string::npos);
    const std::string decl = text.substr(dtor, semi - dtor);
    CHECK(decl.find("= default") != std::string::npos);
}

TEST_CASE("cxx initializer_list model: setup clang++ with ASan/UBSan") {
    auto& c = ctx();
    if (c.tmp.empty()) {
        c.tmp = fs::temp_directory_path() / "prism-cxxilist-doctest";
        fs::remove_all(c.tmp);
        fs::create_directories(c.tmp);
        c.cxx = cxxh::pick_cxx([&](const std::string& cxx) { return probe_clang_asan(cxx, c.tmp); });
        c.ok = !c.cxx.empty();
    }
    if (!c.ok) prism_test::notrun("clang++ with ASan/UBSan and libstdc++ not usable here");
}

TEST_CASE("cxx initializer_list model: empty size is 0 and the destructor does not free") {
    auto& c = ctx();
    if (!c.ok) return prism_test::notrun(cxxh::kNoCompiler);
    const fs::path models = repo() / "src" / "prism" / "pir" / "models" / "cxx";
    const std::vector<std::string> flags{"-std=c++23", "-D_GLIBCXX_ASSERTIONS", "-fsanitize=address,undefined",
                                         "-fno-sanitize-recover=all", "-g", "-O1", "-w"};
    std::vector<std::string> deps_argv{c.cxx};
    deps_argv.insert(deps_argv.end(), flags.begin(), flags.end());
    deps_argv.push_back("-isystem");
    deps_argv.push_back(models.string());
    deps_argv.push_back("-M");
    deps_argv.push_back((repo() / "tests" / "cxx_models" / "initializer_list_trace.cpp").string());
    std::string deps;
    REQUIRE(run_cmd(deps_argv, &deps));
    CHECK(fwd(deps).find(fwd(model_header().string())) != std::string::npos);

    auto lib = build_prog(c.cxx, c.tmp, false);
    auto mod = build_prog(c.cxx, c.tmp, true);
    auto a = run_lines(lib);
    auto b = run_lines(mod);
    CHECK(a == b);
    CHECK(has_line(b, "empty size=0 eq=1 hits=0"));
    CHECK(has_line(b, "copy_dtors=0 copy_frees=0"));
    CHECK(has_line(b, "still=6"));
    CHECK(has_line(b, "walk=60"));
    CHECK(has_line(b, "empty_walk=0"));
}

TEST_CASE("cxx initializer_list model: size is the stored length and empty does not load") {
    const std::string text = slurp(model_header());
    REQUIRE_FALSE(text.empty());
    CHECK(text.find("operator[]") == std::string::npos);
    CHECK(text.find("#include <memory") == std::string::npos);
    CHECK(text.find("#include <algorithm") == std::string::npos);
    CHECK(text.find("constexpr initializer_list() noexcept : _M_array(nullptr), _M_len(0)") != std::string::npos);
    CHECK(count_of(text, "This is not PROVED and not CLEAN.") == 3);
    CHECK(count_of(text, "A zero-length list has size 0.") == 2);
    CHECK(count_of(text, "constexpr size_type size() const noexcept { return _M_len; }") == 1);
    CHECK(count_of(text, "constexpr bool empty() const noexcept { return size() == 0; }") == 1);

    const std::string size_note = slice_between(text, "Stored length.", "constexpr size_type size()");
    REQUIRE_FALSE(size_note.empty());
    CHECK(size_note.find("Does not load an element.") != std::string::npos);
    CHECK(size_note.find("A zero-length list has size 0.") != std::string::npos);
    CHECK(size_note.find("This is not PROVED and not CLEAN.") != std::string::npos);
    CHECK(size_note.find("_M_array") == std::string::npos);

    const std::string empty_note = slice_between(text, "size() == 0. Does not load an element.", "constexpr bool empty()");
    REQUIRE_FALSE(empty_note.empty());
    CHECK(empty_note.find("This is not PROVED and not CLEAN.") != std::string::npos);
    CHECK(empty_note.find("_M_array") == std::string::npos);
    CHECK(empty_note.find("_M_len") == std::string::npos);

    const std::string size_body = body_of(text, "constexpr size_type size() const noexcept");
    const std::string empty_body = body_of(text, "constexpr bool empty() const noexcept");
    REQUIRE_FALSE(size_body.empty());
    REQUIRE_FALSE(empty_body.empty());
    CHECK(size_body.find("return _M_len;") != std::string::npos);
    CHECK(empty_body.find("return size() == 0;") != std::string::npos);
    CHECK(empty_body.find("_M_len") == std::string::npos);
    CHECK_FALSE(loads_element(size_body));
    CHECK_FALSE(loads_element(empty_body));
}

TEST_CASE("cxx initializer_list model: zero length is empty and size does not load") {
    auto& c = ctx();
    if (!c.ok) return prism_test::notrun(cxxh::kNoCompiler);
    auto mod = build_size_trace(c.cxx, c.tmp);
    auto lines = run_lines(mod);
    CHECK(has_line(lines, "zero size=0 empty=1"));
    CHECK(has_line(lines, "braced size=0 empty=1"));
    CHECK(has_line(lines, "three size=3 empty=0"));
    CHECK(has_line(lines, "none size=0 empty=1 copies=0 reads=0"));
    CHECK(has_line(lines, "after size=3 empty=0 copies=0 reads=0"));
}
