// std::forward_list::pop_front on an empty list is a FUNC-CONTRACT check,
// then __builtin_trap, before any destroy or free. That path does not invent
// an element and does not return a value-initialized T. The header is not
// included here: this binary already uses the standard library in other
// translation units. This is not PROVED and not CLEAN. One line in
// _prism_test_sources.

#include <doctest/doctest.h>

#include <cctype>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>

namespace fs = std::filesystem;

namespace {

fs::path repo() { return fs::path(PRISM_SOURCE_DIR); }

fs::path model_header() { return repo() / "src" / "prism" / "pir" / "models" / "cxx" / "forward_list"; }

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

std::string strip_comments_and_strings(const std::string& in) {
    std::string out;
    out.reserve(in.size());
    const std::size_t n = in.size();
    for (std::size_t i = 0; i < n;) {
        if (in[i] == '/' && i + 1 < n && in[i + 1] == '/') {
            i += 2;
            while (i < n && in[i] != '\n') ++i;
            continue;
        }
        if (in[i] == '/' && i + 1 < n && in[i + 1] == '*') {
            i += 2;
            while (i + 1 < n && !(in[i] == '*' && in[i + 1] == '/')) ++i;
            if (i + 1 < n) i += 2;
            else i = n;
            out.push_back(' ');
            continue;
        }
        if (in[i] == '"') {
            out.push_back('"');
            ++i;
            while (i < n && in[i] != '"') {
                if (in[i] == '\\' && i + 1 < n) i += 2;
                else ++i;
            }
            if (i < n) ++i;
            out.push_back('"');
            continue;
        }
        out.push_back(in[i++]);
    }
    return out;
}

bool has_ident(std::string_view s, std::string_view id) {
    for (std::size_t i = 0; (i = s.find(id, i)) != std::string_view::npos; i += id.size()) {
        const bool left = i == 0 || !(std::isalnum(static_cast<unsigned char>(s[i - 1])) || s[i - 1] == '_');
        const std::size_t j = i + id.size();
        const bool right = j >= s.size() || !(std::isalnum(static_cast<unsigned char>(s[j])) || s[j] == '_');
        if (left && right) return true;
    }
    return false;
}

void require_front(const std::string& src, std::string_view sig) {
    const auto body = body_of(src, sig);
    REQUIRE_FALSE(body.empty());
    const auto check = body.find("__prism_check(!this->empty() ? 1 : 0, \"FUNC-CONTRACT\",");
    const auto msg = body.find("std::forward_list::front on an empty list");
    const auto trap = body.find("if (this->empty()) __builtin_trap();");
    const auto load = body.find("return *_S_val(_M_head._M_next);");
    REQUIRE(check != std::string::npos);
    REQUIRE(msg != std::string::npos);
    REQUIRE(trap != std::string::npos);
    REQUIRE(load != std::string::npos);
    CHECK(check < msg);
    CHECK(msg < trap);
    CHECK(trap < load);
    CHECK(body.find("std::forward_list::pop_front on an empty list") == std::string::npos);
    CHECK(body.find("__glibcxx_assert") == std::string::npos);
}

void require_no_invented_element(std::string_view body) {
    CHECK(body.find("return") == std::string::npos);
    CHECK(body.find("return T{}") == std::string::npos);
    CHECK(body.find("return {}") == std::string::npos);
    CHECK(body.find("return value_type{}") == std::string::npos);
    CHECK(body.find("return value_type()") == std::string::npos);
    CHECK(body.find("return _Tp{}") == std::string::npos);
    CHECK(body.find("return _Tp()") == std::string::npos);
    CHECK(body.find("T{}") == std::string::npos);
    CHECK(body.find("value_type{}") == std::string::npos);
    CHECK(body.find("value_type()") == std::string::npos);
    CHECK(body.find("_Tp{}") == std::string::npos);
    CHECK(body.find("_Tp()") == std::string::npos);
    CHECK(body.find("_M_create") == std::string::npos);
    CHECK(body.find("emplace") == std::string::npos);
    CHECK(body.find("construct_at") == std::string::npos);
    CHECK(body.find("push_front") == std::string::npos);
}

}  // namespace

TEST_CASE("cxx forward_list model: empty pop_front traps before destroy or free") {
    const fs::path header = model_header();
    REQUIRE(fs::is_regular_file(header));
    CHECK_FALSE(fs::is_directory(header));

    const std::string src = slurp(header);
    REQUIRE_FALSE(src.empty());
    CHECK(src.find("#ifndef _PRISM_MODEL_FORWARD_LIST") != std::string::npos);
    CHECK(src.find("#define _GLIBCXX_FORWARD_LIST 1") != std::string::npos);
    CHECK(src.find("#pragma GCC system_header") != std::string::npos);
    CHECK(src.find("extern \"C\" void __prism_check(int, const char*, const char*);") != std::string::npos);
    CHECK(count_sub(src, "extern \"C\" void __prism_check(int, const char*, const char*);") == 1);
    CHECK(src.find("__prism_check(int, const char*, const char*) {") == std::string::npos);
    CHECK(count_sub(src, "#include <memory>") == 1);
    CHECK(count_sub(src, "__prism_check(!this->empty() ? 1 : 0, \"FUNC-CONTRACT\",") == 3);
    CHECK(count_sub(src, "__builtin_trap()") == 3);

    require_front(src, "reference front()");
    require_front(src, "const_reference front() const");

    const auto pop = body_of(src, "void pop_front()");
    REQUIRE_FALSE(pop.empty());
    const auto check = pop.find("__prism_check(!this->empty() ? 1 : 0, \"FUNC-CONTRACT\",");
    const auto msg = pop.find("std::forward_list::pop_front on an empty list");
    const auto trap = pop.find("if (this->empty()) __builtin_trap();");
    const auto erase = pop.find("_M_erase_after(&_M_head);");
    REQUIRE(check != std::string::npos);
    REQUIRE(msg != std::string::npos);
    REQUIRE(trap != std::string::npos);
    REQUIRE(erase != std::string::npos);
    CHECK(check < msg);
    CHECK(msg < trap);
    CHECK(trap < erase);
    CHECK(count_sub(pop, "__builtin_trap()") == 1);
    CHECK(count_sub(pop, "std::forward_list::pop_front on an empty list") == 1);
    CHECK(count_sub(pop, "__prism_check") == 1);
    CHECK(pop.find("__glibcxx_assert") == std::string::npos);
    CHECK(pop.find("_S_val") == std::string::npos);
    CHECK(pop.find("std::forward_list::front on an empty list") == std::string::npos);

    const auto before = pop.substr(0, trap);
    CHECK(before.find("destroy_at") == std::string::npos);
    CHECK(before.find("deallocate") == std::string::npos);
    CHECK(before.find("operator delete") == std::string::npos);
    CHECK(before.find("free(") == std::string::npos);
    CHECK(before.find("_Prism_fwd_drop") == std::string::npos);
    CHECK(before.find("_M_erase_after") == std::string::npos);
    CHECK(before.find("malloc") == std::string::npos);
    require_no_invented_element(before);
    require_no_invented_element(pop);

    const auto drop = body_of(src, "void _Prism_fwd_drop(_Prism_fwd_link* __b)");
    REQUIRE_FALSE(drop.empty());
    CHECK(drop.find("std::destroy_at") != std::string::npos);
    CHECK(drop.find("deallocate") != std::string::npos);
    CHECK(pop.find("destroy_at") == std::string::npos);
    CHECK(pop.find("deallocate") == std::string::npos);

    const auto sig = src.find("void pop_front()");
    REQUIRE(sig != std::string::npos);
    const auto note = src.substr(sig > 240 ? sig - 240 : 0, sig > 240 ? 240 : sig);
    CHECK(note.find("not PROVED") != std::string::npos);
    CHECK(note.find("not CLEAN") != std::string::npos);

    const std::string code = strip_comments_and_strings(src);
    CHECK_FALSE(has_ident(code, "splice_after"));
    CHECK_FALSE(has_ident(code, "merge"));
}
