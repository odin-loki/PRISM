// std::queue::operator< compares stored elements from front to back.
// Two empty queues compare not-less and do not load a node. An empty
// queue is less than a non-empty queue, and that path does not load the
// empty queue. The walk stops at the first deciding element and does not
// read past the shorter queue. The header is read as text here, and
// tests/cxx_models/queue_less_trace.cpp is compiled twice when clang++ can
// host libstdc++ with ASan/UBSan. Stdout must match.
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

fs::path model_header() { return repo() / "src" / "prism" / "pir" / "models" / "cxx" / "queue"; }

fs::path model_dir() {
    static const fs::path d = [] {
        fs::path dir = fs::temp_directory_path() / ("prism_queue_model_" + std::to_string(std::random_device{}()));
        fs::create_directories(dir);
        fs::copy_file(model_header(), dir / "queue", fs::copy_options::overwrite_existing);
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

struct LessCtx {
    fs::path tmp;
    std::string cxx;
    bool ok = false;
};

LessCtx& ctx() {
    static LessCtx c;
    return c;
}

const std::vector<std::string>& flags() {
    static const std::vector<std::string> f{"-std=c++23", "-D_GLIBCXX_ASSERTIONS", "-fsanitize=address,undefined",
                                             "-fno-sanitize-recover=all", "-g", "-O1", "-w"};
    return f;
}

bool probe_clang_asan(const std::string& cxx, const fs::path& tmp) {
    return cxxh::probe_clang_asan(cxx, tmp,
                                  "#include <queue>\n"
                                  "int main() { std::queue<int> a, b; b.push(1); return (a < b) ? 0 : 1; }\n",
                                  flags());
}

fs::path build_prog(const std::string& cxx, const fs::path& tmp, bool with_models) {
    return cxxh::build_prog(cxx, tmp, cxxh::trace_src("queue_less_trace.cpp"), "queue_less", flags(), with_models);
}

}  // namespace

TEST_CASE("cxx queue model: operator< walks front to back") {
    const fs::path header = model_header();
    REQUIRE(fs::is_regular_file(header));
    CHECK_FALSE(fs::is_directory(header));

    const std::string src = slurp(header);
    REQUIRE_FALSE(src.empty());
    CHECK(src.find("Two empty queues compare not-less and do not load a node.") != std::string::npos);
    CHECK(src.find("#include <deque>") == std::string::npos);
    CHECK(src.find("#include <vector>") == std::string::npos);
    CHECK(src.find("#include <list>") == std::string::npos);
    CHECK(src.find("lexicographical_compare") == std::string::npos);

    const auto stripped = strip_comments(src);
    CHECK(stripped.find("operator>") == std::string::npos);
    CHECK(stripped.find("operator<=") == std::string::npos);
    CHECK(stripped.find("operator>=") == std::string::npos);
    CHECK(stripped.find("operator<=>") == std::string::npos);
    CHECK(stripped.find("operator==") == std::string::npos);
    CHECK(stripped.find("operator!=") == std::string::npos);
    CHECK(count_sub(stripped, "bool operator<") == 2);
    CHECK(stripped.find("friend bool operator<(const queue<_Up, _Seq>& __x, const queue<_Up, _Seq>& __y);") !=
          std::string::npos);

    const std::string sig = "bool operator<(const queue<_Tp, _Sequence>& __x, const queue<_Tp, _Sequence>& __y)";
    const auto less = body_of(src, sig);
    REQUIRE_FALSE(less.empty());
    const auto early = less.find("if (__x.empty() || __y.empty())");
    const auto early_ret = less.find("return __x.empty() && !__y.empty();");
    const auto head = less.find("const _Node* __a = __x._M_head;");
    const auto loop = less.find("while (__a != nullptr && __b != nullptr)");
    const auto less_el = less.find("if (__lv < __rv) return true;");
    const auto greater_el = less.find("if (__rv < __lv) return false;");
    const auto step = less.find("__a = __a->_M_next;");
    const auto rest = less.find("return __a == nullptr && __b != nullptr;");
    REQUIRE(early != std::string::npos);
    REQUIRE(early_ret != std::string::npos);
    REQUIRE(head != std::string::npos);
    REQUIRE(loop != std::string::npos);
    REQUIRE(less_el != std::string::npos);
    REQUIRE(greater_el != std::string::npos);
    REQUIRE(step != std::string::npos);
    REQUIRE(rest != std::string::npos);
    CHECK(early < early_ret);
    CHECK(early_ret < head);
    CHECK(head < loop);
    CHECK(loop < less_el);
    CHECK(less_el < greater_el);
    CHECK(greater_el < step);
    CHECK(step < rest);
    CHECK(less.find("front(") == std::string::npos);
    CHECK(less.find("back(") == std::string::npos);
    CHECK(less.find("pop(") == std::string::npos);
    CHECK(less.find("push(") == std::string::npos);
    CHECK(less.find("emplace") == std::string::npos);
    CHECK(less.find("_M_pop") == std::string::npos);
    CHECK(less.find("_M_drop") == std::string::npos);
    CHECK(less.find("_M_tail") == std::string::npos);
    CHECK(less.find("T{}") == std::string::npos);
    CHECK(less.find("value_type{}") == std::string::npos);
    CHECK(less.find("_Tp{}") == std::string::npos);
    CHECK(less.find("_Tp()") == std::string::npos);

    const std::string cmake = slurp(repo() / "CMakeLists.txt");
    CHECK(count_sub(cmake, "tests/cpp/test_cxx_queue_less.cpp") == 1);

    const std::string note = slurp(repo() / "docs" / "finish-notes" / "queue-less.md");
    REQUIRE_FALSE(note.empty());
    CHECK(note.find("not-less") != std::string::npos);
    CHECK(note.find("front to back") != std::string::npos);
    CHECK(note.find("queue_less_trace.cpp") != std::string::npos);
}

TEST_CASE("cxx queue model: setup clang++ with ASan/UBSan and libstdc++") {
    auto& c = ctx();
    if (c.tmp.empty()) {
        c.tmp = fs::temp_directory_path() / "prism-cxxqueue-less-doctest";
        fs::remove_all(c.tmp);
        fs::create_directories(c.tmp);
        c.cxx = cxxh::pick_cxx([&](const std::string& cxx) { return probe_clang_asan(cxx, c.tmp); });
        c.ok = !c.cxx.empty();
    }
    if (!c.ok) prism_test::notrun("clang++ with libstdc++ and ASan/UBSan not usable here");
}

TEST_CASE("cxx queue model: operator< trace matches libstdc++") {
    auto& c = ctx();
    if (!c.ok) return prism_test::notrun(cxxh::kNoCompiler);
    const std::string deps_anchor = fwd((model_dir() / "queue").string());
    std::vector<std::string> argv{c.cxx};
    argv.insert(argv.end(), flags().begin(), flags().end());
    argv.push_back("-isystem");
    argv.push_back(model_dir().string());
    argv.push_back("-M");
    argv.push_back((repo() / "tests" / "cxx_models" / "queue_less_trace.cpp").string());
    std::string deps;
    REQUIRE(run_cmd(argv, &deps));
    CHECK(fwd(deps).find(deps_anchor) != std::string::npos);
    CHECK(deps.find("stl_queue.h") == std::string::npos);

    auto lib = build_prog(c.cxx, c.tmp, false);
    auto mod = build_prog(c.cxx, c.tmp, true);
    auto a = run_lines(lib);
    auto b = run_lines(mod);
    const std::vector<std::string> want{
        "ops front=1 back=2 size=2 empty=0",
        "ops pop front=2 back=2 size=1 empty=0",
        "lt empty=0 el=1 le=0 same=0 mid=1 head=1 tail=1 gt=0 early=0 one=1 oned=0 oneeq=0 self=0 prefix=1 longer=0 "
        "front=1 csame=0 cdiff=1",
        "ltm same=0 mid=1 head=1 tail=1 gt=0 early=0 zero=0 el=1 le=0 poison=1 pgt=0 prefix=0 short=1 front=1 never=0 "
        "nzero=0 calls same=6 mid=3 head=1 tail=5 gt=6 early=2 zero=0 el=0 le=0 poison=3 pread=0 pgt=2 pgread=0 "
        "prefix=2 pread=0 short=2 sread=0 front=1 fread=0 never=6 nzero=0 nolt=3 4 2 rec=1 1",
    };
    CHECK(a == b);
    CHECK(b == want);
}
