// Empty priority_queue::pop is __glibcxx_assert(!this->empty()) before the
// root is loaded and before any node is destroyed. operator< walks stored
// elements in heap-index order, skips an empty root, and treats two empty
// queues as not less. libstdc++ has no priority_queue comparison and no
// <priority_queue> header, so the less trace is compiled only against this
// model. The header is read as text: it includes libstdc++ private headers,
// so this doctest does not include it.
// CMake's prism_tests source list is explicit (not a glob): this file is one
// line in _prism_test_sources.

#include <doctest/doctest.h>

#include "cxx_model_harness.hpp"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <random>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace fs = std::filesystem;

namespace {

namespace cxxh = prism_test::cxx;
using cxxh::fwd;
using cxxh::repo;
using cxxh::run_cmd;
using cxxh::run_lines;

fs::path header_path() { return repo() / "src" / "prism" / "pir" / "models" / "cxx" / "priority_queue"; }

fs::path model_dir() {
    static const fs::path d = [] {
        fs::path dir =
            fs::temp_directory_path() / ("prism_pqueue_model_" + std::to_string(std::random_device{}()));
        fs::create_directories(dir);
        fs::copy_file(header_path(), dir / "priority_queue", fs::copy_options::overwrite_existing);
        return dir;
    }();
    return d;
}

std::string slurp(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    if (!in) return {};
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

int count_sub(std::string_view s, std::string_view n) {
    int c = 0;
    for (std::size_t i = 0; (i = s.find(n, i)) != std::string_view::npos; i += n.size()) ++c;
    return c;
}

std::string body_of(std::string_view src, std::string_view sig) {
    const auto at = src.find(sig);
    if (at == std::string_view::npos) return {};
    const auto brace = src.find('{', at);
    if (brace == std::string_view::npos) return {};
    int depth = 0;
    for (std::size_t i = brace; i < src.size(); ++i) {
        if (src[i] == '{')
            ++depth;
        else if (src[i] == '}') {
            if (--depth == 0) return std::string(src.substr(brace, i - brace + 1));
        }
    }
    return {};
}

std::string strip_comments(std::string_view src) {
    std::string out;
    out.reserve(src.size());
    for (std::size_t i = 0; i < src.size(); ++i) {
        if (i + 1 < src.size() && src[i] == '/' && src[i + 1] == '/') {
            while (i < src.size() && src[i] != '\n') ++i;
            if (i < src.size()) out.push_back('\n');
            continue;
        }
        if (i + 1 < src.size() && src[i] == '/' && src[i + 1] == '*') {
            i += 2;
            while (i + 1 < src.size() && !(src[i] == '*' && src[i + 1] == '/')) ++i;
            if (i + 1 < src.size()) ++i;
            continue;
        }
        out.push_back(src[i]);
    }
    return out;
}

struct PqCtx {
    fs::path tmp;
    std::string cxx;
    bool ok = false;
};

PqCtx& ctx() {
    static PqCtx c;
    return c;
}

const std::vector<std::string>& flags() {
    static const std::vector<std::string> f{"-std=c++23", "-D_GLIBCXX_ASSERTIONS", "-fsanitize=address,undefined",
                                             "-fno-sanitize-recover=all", "-g", "-O1", "-w"};
    return f;
}

bool probe_clang_asan(const std::string& cxx, const fs::path& tmp) {
    std::vector<std::string> with_models(flags());
    with_models.insert(with_models.end(), {"-isystem", model_dir().string()});
    return cxxh::probe_clang_asan(cxx, tmp,
                                  "#include <priority_queue>\n"
                                  "int main() { std::priority_queue<int> a, b; return (a < b) ? 1 : 0; }\n",
                                  with_models);
}

}  // namespace

TEST_CASE("cxx priority_queue model: empty pop checks then traps before destroy") {
    const fs::path header = header_path();
    REQUIRE(fs::is_regular_file(header));
    CHECK_FALSE(fs::is_directory(header));
    const std::string src = slurp(header);
    REQUIRE_FALSE(src.empty());
    CHECK(src.find("#include <queue>") == std::string::npos);
    CHECK(src.find("#include <vector>") == std::string::npos);
    CHECK(src.find("#include <deque>") == std::string::npos);
    CHECK(src.find("FUNC-CONTRACT") != std::string::npos);

    const auto pop = body_of(src, "void pop()");
    REQUIRE_FALSE(pop.empty());
    const auto check = pop.find("__glibcxx_assert(!this->empty())");
    const auto touch = pop.find("_M_heap[0]");
    const auto drop = pop.find("_M_drop(");
    REQUIRE(check != std::string::npos);
    REQUIRE(touch != std::string::npos);
    REQUIRE(drop != std::string::npos);
    CHECK(check < touch);
    CHECK(touch < drop);
    CHECK(pop.substr(0, touch).find("_M_heap") == std::string::npos);
    CHECK(pop.substr(0, touch).find("_M_drop") == std::string::npos);
    CHECK(pop.substr(0, touch).find("destroy_at") == std::string::npos);
    CHECK(pop.substr(0, touch).find("deallocate") == std::string::npos);
    CHECK(pop.find("T{}") == std::string::npos);
    CHECK(pop.find("value_type{}") == std::string::npos);
    CHECK(pop.find("_Tp{}") == std::string::npos);
    CHECK(pop.find("_Tp()") == std::string::npos);

    const auto top = body_of(src, "const_reference top() const");
    REQUIRE_FALSE(top.empty());
    const auto top_check = top.find("__glibcxx_assert(!this->empty())");
    const auto top_load = top.find("return *_M_heap[0]->_M_valptr();");
    REQUIRE(top_check != std::string::npos);
    REQUIRE(top_load != std::string::npos);
    CHECK(top_check < top_load);
    CHECK(top.substr(0, top_load).find("_M_heap") == std::string::npos);
    CHECK(top.find("T{}") == std::string::npos);

    const std::string note = slurp(repo() / "docs" / "finish-notes" / "priority-queue-pop.md");
    REQUIRE_FALSE(note.empty());
    CHECK(note.find("FUNC-CONTRACT") != std::string::npos);
    CHECK(note.find("__glibcxx_assert") != std::string::npos);
}

TEST_CASE("cxx priority_queue model: operator< walks storage and skips an empty root") {
    const fs::path header = header_path();
    REQUIRE(fs::is_regular_file(header));
    const std::string src = slurp(header);
    REQUIRE_FALSE(src.empty());
    CHECK(src.find("#include <algorithm>") == std::string::npos);
    CHECK(src.find("std::sort") == std::string::npos);
    CHECK(src.find("lexicographical_compare") == std::string::npos);
    CHECK(src.find("Two empty queues are not less and do not load the root.") != std::string::npos);

    const auto stripped = strip_comments(src);
    CHECK(stripped.find("operator>") == std::string::npos);
    CHECK(stripped.find("operator<=") == std::string::npos);
    CHECK(stripped.find("operator>=") == std::string::npos);
    CHECK(stripped.find("operator<=>") == std::string::npos);
    CHECK(stripped.find("operator==") == std::string::npos);
    CHECK(stripped.find("operator!=") == std::string::npos);
    CHECK(count_sub(stripped, "bool operator<") == 2);

    const auto sig = std::string("bool operator<(const priority_queue<_Tp, _Sequence, _Compare>& __x");
    const auto lt = body_of(src, sig);
    REQUIRE_FALSE(lt.empty());
    const auto heap = lt.find("_M_heap");
    REQUIRE(heap != std::string::npos);
    const auto pre = lt.substr(0, heap);
    CHECK(pre.find("_M_valptr") == std::string::npos);
    CHECK(pre.find("_M_comp") == std::string::npos);
    const auto both = pre.find("if (__x._M_size == 0 && __y._M_size == 0) return false;");
    const auto left = pre.find("if (__x._M_size == 0) return true;");
    const auto right = pre.find("if (__y._M_size == 0) return false;");
    REQUIRE(both != std::string::npos);
    REQUIRE(left != std::string::npos);
    REQUIRE(right != std::string::npos);
    CHECK(both < left);
    CHECK(left < right);
    CHECK(pre.find("T{}") == std::string::npos);
    CHECK(pre.find("value_type{}") == std::string::npos);
    CHECK(pre.find("_Tp{}") == std::string::npos);

    const auto bound = lt.find("const size_type __n = __x._M_size < __y._M_size ? __x._M_size : __y._M_size;");
    const auto loop = lt.find("for (size_type __i = 0; __i < __n; ++__i)");
    const auto less_xy = lt.find("*__x._M_heap[__i]->_M_valptr() < *__y._M_heap[__i]->_M_valptr()");
    const auto less_yx = lt.find("*__y._M_heap[__i]->_M_valptr() < *__x._M_heap[__i]->_M_valptr()");
    const auto tail = lt.find("return __x._M_size < __y._M_size;");
    REQUIRE(bound != std::string::npos);
    REQUIRE(loop != std::string::npos);
    REQUIRE(less_xy != std::string::npos);
    REQUIRE(less_yx != std::string::npos);
    REQUIRE(tail != std::string::npos);
    CHECK(right < bound);
    CHECK(bound < loop);
    CHECK(loop < heap);
    CHECK(less_xy < less_yx);
    CHECK(less_yx < tail);
    for (const char* bad : {"T{}", "value_type{}", "value_type()", "_Tp{}", "_Tp()", "std::sort", "_M_comp", "top()",
                            "pop()", "push(", "emplace(", "__builtin_trap"}) {
        CHECK(lt.find(bad) == std::string::npos);
    }

    const std::string cmake = slurp(repo() / "CMakeLists.txt");
    CHECK(count_sub(cmake, "tests/cpp/test_cxx_priority_queue.cpp") == 1);
    const std::string note = slurp(repo() / "docs" / "finish-notes" / "priority-queue-less.md");
    REQUIRE_FALSE(note.empty());
    CHECK(note.find("heap-index") != std::string::npos);
    CHECK(note.find("not less") != std::string::npos);
}

TEST_CASE("cxx priority_queue model: setup clang++ with ASan/UBSan") {
    auto& c = ctx();
    if (c.tmp.empty()) {
        c.tmp = fs::temp_directory_path() / "prism-cxxpqueue-doctest";
        fs::remove_all(c.tmp);
        fs::create_directories(c.tmp);
        c.cxx = cxxh::pick_cxx([&](const std::string& cxx) { return probe_clang_asan(cxx, c.tmp); });
        c.ok = !c.cxx.empty();
    }
    if (!c.ok) prism_test::notrun("clang++ with ASan/UBSan not usable here");
}

TEST_CASE("cxx priority_queue model: operator< trace skips an empty root") {
    auto& c = ctx();
    if (!c.ok) return prism_test::notrun(cxxh::kNoCompiler);
    const fs::path src = repo() / "tests" / "cxx_models" / "priority_queue_less_trace.cpp";
    std::vector<std::string> plain{c.cxx};
    plain.insert(plain.end(), flags().begin(), flags().end());
    plain.push_back(src.string());
    plain.push_back("-o");
    plain.push_back((c.tmp / "pq_less_system").string());
    CHECK_FALSE(run_cmd(plain));

    const std::string deps_anchor = fwd((model_dir() / "priority_queue").string());
    std::vector<std::string> deps_argv{c.cxx};
    deps_argv.insert(deps_argv.end(), flags().begin(), flags().end());
    deps_argv.push_back("-isystem");
    deps_argv.push_back(model_dir().string());
    deps_argv.push_back("-M");
    deps_argv.push_back(src.string());
    std::string deps;
    REQUIRE(run_cmd(deps_argv, &deps));
    CHECK(fwd(deps).find(deps_anchor) != std::string::npos);

    fs::path out = c.tmp / "pq_less.model";
    std::vector<std::string> build{c.cxx};
    build.insert(build.end(), flags().begin(), flags().end());
    build.push_back("-isystem");
    build.push_back(model_dir().string());
    build.push_back(src.string());
    build.push_back("-o");
    build.push_back(out.string());
    REQUIRE(run_cmd(build));
    const std::vector<std::string> want{
        "lt empty=0 el=1 le=0 head=1 hd=0 self=0 same=0 store=1 storer=0 short=1 long=0 minlt=1 minrev=0 cempty=0 "
        "moved=1 movedback=0 popped=0 plive=1 lpop=0 pcalls=0 lcalls=0 ppoison=0",
        "mem zero=0 el=1 le=0 head=1 gt=0 mid=1 short=1 calls zero=0 el=0 le=0 head=1 hpoison=0 gt=2 gpoison=0 mid=3 "
        "mpoison=0 short=4 spoison=0 nolt=0",
    };
    CHECK(run_lines(out) == want);
}
