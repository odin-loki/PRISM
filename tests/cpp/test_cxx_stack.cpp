// std::stack::pop on an empty stack is a FUNC-CONTRACT check, then
// __builtin_trap, before any destroy or free. std::stack::top on an empty
// stack is the same check shape, then __builtin_trap, before a node is
// loaded. Neither path invents an element or returns T{}. The header is
// not included here: this binary already uses the standard library in
// other translation units. This is not PROVED and not CLEAN.

#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>

namespace fs = std::filesystem;

namespace {

fs::path repo() { return fs::path(PRISM_SOURCE_DIR); }

fs::path model_header() { return repo() / "src" / "prism" / "pir" / "models" / "cxx" / "stack"; }

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

void require_top(const std::string& src, std::string_view sig) {
    const auto body = body_of(src, sig);
    REQUIRE_FALSE(body.empty());
    const auto check = body.find("__prism_check(!this->empty() ? 1 : 0, \"FUNC-CONTRACT\",");
    const auto msg = body.find("std::stack::top on an empty stack");
    const auto trap = body.find("if (this->empty()) __builtin_trap();");
    const auto load = body.find("return *_M_top->_M_valptr();");
    REQUIRE(check != std::string::npos);
    REQUIRE(msg != std::string::npos);
    REQUIRE(trap != std::string::npos);
    REQUIRE(load != std::string::npos);
    CHECK(check < msg);
    CHECK(msg < trap);
    CHECK(trap < load);
    const auto before = body.substr(0, trap);
    CHECK(before.find("_M_top") == std::string::npos);
    CHECK(before.find("_M_valptr") == std::string::npos);
    CHECK(before.find("_M_pop") == std::string::npos);
    CHECK(before.find("_M_drop") == std::string::npos);
    CHECK(before.find("destroy_at") == std::string::npos);
    CHECK(before.find("deallocate") == std::string::npos);
    CHECK(before.find("return") == std::string::npos);
    CHECK(body.find("return T{}") == std::string::npos);
    CHECK(body.find("return {}") == std::string::npos);
    CHECK(body.find("T{}") == std::string::npos);
    CHECK(body.find("value_type{}") == std::string::npos);
    CHECK(body.find("value_type()") == std::string::npos);
    CHECK(body.find("_Tp{}") == std::string::npos);
    CHECK(body.find("_Tp()") == std::string::npos);
    CHECK(body.find("__glibcxx_assert") == std::string::npos);
    CHECK(body.find("__glibcxx_requires_nonempty") == std::string::npos);
    CHECK(body.find("std::stack::pop on an empty stack") == std::string::npos);
    CHECK(count_sub(body, "__builtin_trap()") == 1);
    CHECK(count_sub(body, "std::stack::top on an empty stack") == 1);
}

void require_untouched(const std::string& src, std::string_view sig) {
    const auto body = body_of(src, sig);
    REQUIRE_FALSE(body.empty());
    CHECK(body.find("__prism_check") == std::string::npos);
    CHECK(body.find("__builtin_trap") == std::string::npos);
    CHECK(body.find("std::stack::pop on an empty stack") == std::string::npos);
    CHECK(body.find("std::stack::top on an empty stack") == std::string::npos);
}

}  // namespace

TEST_CASE("cxx stack model: empty pop traps before destroy or free") {
    const fs::path header = model_header();
    REQUIRE(fs::is_regular_file(header));
    CHECK_FALSE(fs::is_directory(header));

    const std::string src = slurp(header);
    REQUIRE_FALSE(src.empty());
    CHECK(src.find("#ifndef _PRISM_MODEL_STACK") != std::string::npos);
    CHECK(src.find("#define _GLIBCXX_STACK 1") != std::string::npos);
    CHECK(src.find("#pragma GCC system_header") != std::string::npos);
    CHECK(src.find("not PROVED") != std::string::npos);
    CHECK(src.find("not CLEAN") != std::string::npos);
    CHECK(src.find("extern \"C\" void __prism_check(int, const char*, const char*);") != std::string::npos);
    CHECK(count_sub(src, "extern \"C\" void __prism_check(int, const char*, const char*);") == 1);
    CHECK(src.find("__prism_check(int, const char*, const char*) {") == std::string::npos);
    CHECK(count_sub(src, "__prism_check(!this->empty() ? 1 : 0, \"FUNC-CONTRACT\",") == 3);
    CHECK(count_sub(src, "__builtin_trap()") == 3);
    CHECK(count_sub(src, "std::stack::pop on an empty stack") == 2);
    CHECK(count_sub(src, "std::stack::top on an empty stack") == 3);
    CHECK(src.find("__glibcxx_requires_nonempty") == std::string::npos);

    const auto pop = body_of(src, "void pop()");
    REQUIRE_FALSE(pop.empty());
    const auto check = pop.find("__prism_check(!this->empty() ? 1 : 0, \"FUNC-CONTRACT\",");
    const auto msg = pop.find("std::stack::pop on an empty stack");
    const auto trap = pop.find("if (this->empty()) __builtin_trap();");
    const auto drop = pop.find("_M_pop();");
    REQUIRE(check != std::string::npos);
    REQUIRE(msg != std::string::npos);
    REQUIRE(trap != std::string::npos);
    REQUIRE(drop != std::string::npos);
    CHECK(check < msg);
    CHECK(msg < trap);
    CHECK(trap < drop);
    const auto before = pop.substr(0, trap);
    CHECK(before.find("_M_pop") == std::string::npos);
    CHECK(before.find("_M_drop") == std::string::npos);
    CHECK(before.find("_M_top") == std::string::npos);
    CHECK(before.find("destroy_at") == std::string::npos);
    CHECK(before.find("deallocate") == std::string::npos);
    CHECK(before.find("return") == std::string::npos);
    CHECK(pop.find("return") == std::string::npos);
    CHECK(pop.find("return T{}") == std::string::npos);
    CHECK(pop.find("return {}") == std::string::npos);
    CHECK(pop.find("T{}") == std::string::npos);
    CHECK(pop.find("value_type{}") == std::string::npos);
    CHECK(pop.find("value_type()") == std::string::npos);
    CHECK(pop.find("_Tp{}") == std::string::npos);
    CHECK(pop.find("_Tp()") == std::string::npos);
    CHECK(pop.find("__glibcxx_assert") == std::string::npos);
    CHECK(pop.find("__glibcxx_requires_nonempty") == std::string::npos);
    CHECK(pop.find("std::stack::top on an empty stack") == std::string::npos);
    CHECK(count_sub(pop, "__builtin_trap()") == 1);
    CHECK(count_sub(pop, "std::stack::pop on an empty stack") == 1);

    const auto mpop = body_of(src, "void _M_pop() noexcept");
    REQUIRE_FALSE(mpop.empty());
    CHECK(mpop.find("_M_drop(__n);") != std::string::npos);
    CHECK(mpop.find("__prism_check") == std::string::npos);
    CHECK(mpop.find("__builtin_trap") == std::string::npos);

    require_top(src, "[[nodiscard]] reference top()");
    require_top(src, "const_reference top() const");

    CHECK(body_of(src, "bool empty() const") == "{ return _M_top == nullptr; }");
    CHECK(body_of(src, "size_type size() const") == "{ return _M_size; }");
    CHECK(body_of(src, "void push(const value_type& __x)") == "{ emplace(__x); }");
    CHECK(body_of(src, "void push(value_type&& __x)") == "{ emplace(std::move(__x)); }");

    require_untouched(src, "bool empty() const");
    require_untouched(src, "size_type size() const");
    require_untouched(src, "void push(const value_type& __x)");
    require_untouched(src, "void push(value_type&& __x)");
    require_untouched(src, "decltype(auto) emplace(_Args&&... __args)");
    require_untouched(src, "void swap(stack& __s) noexcept");
}
