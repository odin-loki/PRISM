// Empty std::queue::pop is the FUNC-CONTRACT check already on _M_pop:
// __glibcxx_assert(!this->empty()), then the front node is loaded and
// destroyed. The assert does not return, so an empty pop does not destroy
// a node and does not return T{}. The header is read as text. It includes
// libstdc++ private headers, so this doctest does not include it.
// CMake's prism_tests source list is explicit (not a glob): this file is one
// line in _prism_test_sources.

#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>

namespace fs = std::filesystem;

namespace {

fs::path repo() { return fs::path(PRISM_SOURCE_DIR); }

fs::path model_header() { return repo() / "src" / "prism" / "pir" / "models" / "cxx" / "queue"; }

std::string slurp(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    if (!in) return {};
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
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

}  // namespace

TEST_CASE("cxx queue model: empty pop is a contract check then trap") {
    const fs::path header = model_header();
    REQUIRE(fs::is_regular_file(header));
    CHECK_FALSE(fs::is_directory(header));

    const std::string src = slurp(header);
    REQUIRE_FALSE(src.empty());
    CHECK(src.find("#ifndef _PRISM_MODEL_QUEUE") != std::string::npos);
    CHECK(src.find("#define _GLIBCXX_QUEUE 1") != std::string::npos);
    CHECK(src.find("#pragma GCC system_header") != std::string::npos);
    CHECK(src.find("FUNC-CONTRACT") != std::string::npos);

    const std::string code = strip_comments(src);
    CHECK(code.find("__glibcxx_assert(!this->empty())") != std::string::npos);
    CHECK(code.find("T{}") == std::string::npos);
    CHECK(code.find("value_type{}") == std::string::npos);
    CHECK(code.find("_Tp{}") == std::string::npos);

    const auto pop = body_of(src, "void pop() noexcept");
    REQUIRE_FALSE(pop.empty());
    CHECK(pop.find("_M_pop()") != std::string::npos);
    CHECK(pop.find("_M_drop") == std::string::npos);
    CHECK(pop.find("_M_head") == std::string::npos);
    CHECK(pop.find("_M_valptr") == std::string::npos);
    CHECK(pop.find("destroy_at") == std::string::npos);
    CHECK(pop.find("deallocate") == std::string::npos);
    CHECK(pop.find("return") == std::string::npos);

    // The check is the first statement. The front node is named after it,
    // and the node is destroyed after that load.
    const auto mpop = body_of(src, "void _M_pop() noexcept");
    REQUIRE_FALSE(mpop.empty());
    const auto check = mpop.find("__glibcxx_assert(!this->empty())");
    const auto load = mpop.find("_Node* __n = _M_head;");
    const auto drop = mpop.find("_M_drop(");
    REQUIRE(check != std::string::npos);
    REQUIRE(load != std::string::npos);
    REQUIRE(drop != std::string::npos);
    CHECK(check < load);
    CHECK(load < drop);
    const auto before = mpop.substr(0, load);
    CHECK(before.find("_M_valptr") == std::string::npos);
    CHECK(before.find("_M_drop") == std::string::npos);
    CHECK(before.find("destroy_at") == std::string::npos);
    CHECK(before.find("deallocate") == std::string::npos);
    CHECK(mpop.find("T{}") == std::string::npos);
    CHECK(mpop.find("value_type{}") == std::string::npos);
    CHECK(mpop.find("_Tp{}") == std::string::npos);
    CHECK(mpop.find("_Tp()") == std::string::npos);

    CHECK(body_of(src, "bool empty() const noexcept") == "{ return _M_head == nullptr; }");
    CHECK(body_of(src, "size_type size() const noexcept") == "{ return _M_size; }");
    CHECK(body_of(src, "void push(const value_type& __x)") == "{ _M_push(__x); }");
    CHECK(body_of(src, "void push(value_type&& __x)") == "{ _M_push(std::move(__x)); }");

    const auto emplace = body_of(src, "reference emplace(_Args&&... __args)");
    REQUIRE_FALSE(emplace.empty());
    CHECK(emplace.find("_M_push(std::forward<_Args>(__args)...)") != std::string::npos);
    CHECK(emplace.find("return *_M_tail->_M_valptr();") != std::string::npos);
    CHECK(emplace.find("__glibcxx_assert") == std::string::npos);

    const auto swap = body_of(src, "void swap(queue& __q) noexcept");
    REQUIRE_FALSE(swap.empty());
    CHECK(swap.find("std::swap(_M_head, __q._M_head);") != std::string::npos);
    CHECK(swap.find("std::swap(_M_tail, __q._M_tail);") != std::string::npos);
    CHECK(swap.find("std::swap(_M_size, __q._M_size);") != std::string::npos);
    CHECK(swap.find("__glibcxx_assert") == std::string::npos);

    const fs::path note = repo() / "docs" / "finish-notes" / "queue-pop.md";
    const std::string note_txt = slurp(note);
    REQUIRE_FALSE(note_txt.empty());
    CHECK(note_txt.find("FUNC-CONTRACT") != std::string::npos);
    CHECK(note_txt.find("__glibcxx_assert") != std::string::npos);
    CHECK(note_txt.find("empty") != std::string::npos);
}
