// std::unique_ptr::operator* and operator-> on an empty unique_ptr are a
// PTR-NULL-DEREF check, then __builtin_trap, before the stored pointer is
// dereferenced. That path does not invent an element and does not return
// T{}. The header is not included here: this binary already uses the
// standard library in other translation units. This is not PROVED and not
// CLEAN.

#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>

namespace fs = std::filesystem;

namespace {

fs::path repo() { return fs::path(PRISM_SOURCE_DIR); }

fs::path model_header() { return repo() / "src" / "prism" / "pir" / "models" / "cxx" / "memory"; }

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

std::string body_of(std::string_view src, std::string_view sig, std::size_t nth = 0) {
    std::size_t from = 0;
    for (std::size_t n = 0;; ++n) {
        const auto at = src.find(sig, from);
        if (at == std::string_view::npos) return {};
        from = at + sig.size();
        if (n != nth) continue;
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
}

std::string strip_comments(const std::string& in) {
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
        out.push_back(in[i++]);
    }
    return out;
}

void require_empty_deref(const std::string& body, std::string_view msg, std::string_view after_trap) {
    const auto check = body.find("__prism_check(_M_ptr != nullptr, \"PTR-NULL-DEREF\",");
    const auto message = body.find(msg);
    const auto trap = body.find("if (_M_ptr == nullptr) __builtin_trap();");
    const auto use = body.find(after_trap);
    REQUIRE(check != std::string::npos);
    REQUIRE(message != std::string::npos);
    REQUIRE(trap != std::string::npos);
    REQUIRE(use != std::string::npos);
    CHECK(check < message);
    CHECK(message < trap);
    CHECK(trap < use);
    const auto before = body.substr(0, trap);
    CHECK(before.find("return") == std::string::npos);
    CHECK(before.find("*_M_ptr") == std::string::npos);
    CHECK(before.find("_M_ptr[") == std::string::npos);
    CHECK(body.find("return T{}") == std::string::npos);
    CHECK(body.find("return {}") == std::string::npos);
    CHECK(body.find("return element_type{}") == std::string::npos);
    CHECK(body.find("return element_type()") == std::string::npos);
    CHECK(body.find("return _Tp{}") == std::string::npos);
    CHECK(body.find("return _Tp()") == std::string::npos);
    CHECK(body.find("element_type{") == std::string::npos);
    CHECK(body.find("element_type(") == std::string::npos);
    CHECK(body.find("new _Tp") == std::string::npos);
    CHECK(body.find("new element_type") == std::string::npos);
    CHECK(count_sub(body, "__builtin_trap();") == 1);
    CHECK(count_sub(body, msg) == 1);
    CHECK(count_sub(body, "__prism_check") == 1);
}

}  // namespace

TEST_CASE("cxx memory model: empty unique_ptr operator* and operator-> trap before dereference") {
    const fs::path header = model_header();
    REQUIRE(fs::is_regular_file(header));
    CHECK_FALSE(fs::is_directory(header));

    const std::string src = slurp(header);
    REQUIRE_FALSE(src.empty());
    CHECK(src.find("#ifndef _GLIBCXX_MEMORY") != std::string::npos);
    CHECK(src.find("#define _GLIBCXX_MEMORY 1") != std::string::npos);
    CHECK(src.find("#pragma GCC system_header") != std::string::npos);
    CHECK(count_sub(src, "extern \"C\" void __prism_check(int __ok, const char* __cls, const char* __msg);") == 1);
    CHECK(src.find("__prism_check(int __ok, const char* __cls, const char* __msg) {") == std::string::npos);

    // The comment names <memory>. The header does not include it again.
    CHECK(count_sub(src, "#include <memory>") == 1);
    CHECK(strip_comments(src).find("#include <memory>") == std::string::npos);

    const auto note = src.find("An empty unique_ptr is a PTR-NULL-DEREF check, then __builtin_trap,");
    const auto star_sig = src.find("element_type& operator*() const");
    REQUIRE(note != std::string::npos);
    REQUIRE(star_sig != std::string::npos);
    CHECK(note < star_sig);
    const auto note_text = src.substr(note, star_sig - note);
    CHECK(note_text.find("before the stored pointer is dereferenced.") != std::string::npos);
    CHECK(note_text.find("does not invent") != std::string::npos);
    CHECK(note_text.find("does not return T{}.") != std::string::npos);
    CHECK(note_text.find("This is not PROVED and not CLEAN.") != std::string::npos);

    require_empty_deref(body_of(src, "element_type& operator*() const"),
                        "std::unique_ptr::operator* null pointer", "return *_M_ptr;");
    const auto arrow = body_of(src, "pointer operator->() const");
    require_empty_deref(arrow, "std::unique_ptr::operator-> null pointer", "return _M_ptr;");
    CHECK(arrow.find("*_M_ptr") == std::string::npos);
    CHECK(arrow.find("return *_M_ptr") == std::string::npos);

    CHECK(count_sub(src, "std::unique_ptr::operator* null pointer") == 1);
    CHECK(count_sub(src, "std::unique_ptr::operator-> null pointer") == 1);
    CHECK(src.find("unique_ptr::operator* on an empty unique_ptr") == std::string::npos);
    CHECK(src.find("unique_ptr::operator-> on an empty unique_ptr") == std::string::npos);
    CHECK(count_sub(src, "element_type& operator*() const") == 1);
    CHECK(count_sub(src, "pointer operator->() const") == 1);
    CHECK(count_sub(src, "if (_M_ptr == nullptr) __builtin_trap();") == 2);
    CHECK(count_sub(src, "__builtin_trap();") == 3);
    // Main's equality note does not carry the sentence; operator*/-> does.
    CHECK(count_sub(src, "This is not PROVED and not CLEAN.") == 1);

    const auto index = body_of(src, "element_type& operator[](size_t __i) const");
    REQUIRE_FALSE(index.empty());
    CHECK(index.find("__prism_check(_M_ptr != nullptr, \"PTR-NULL-DEREF\",") != std::string::npos);
    CHECK(index.find("unique_ptr::operator[] on an empty unique_ptr") != std::string::npos);
    CHECK(index.find("__builtin_trap") == std::string::npos);
    CHECK(index.find("return _M_ptr[__i];") != std::string::npos);

    for (std::size_t n = 0; n < 2; ++n) {
        const auto released = body_of(src, "pointer release() noexcept", n);
        const auto reset = body_of(src, "void reset(pointer __p = nullptr) noexcept", n);
        const auto swapped = body_of(src, "void swap(unique_ptr& __u) noexcept", n);
        REQUIRE_FALSE(released.empty());
        REQUIRE_FALSE(reset.empty());
        REQUIRE_FALSE(swapped.empty());
        CHECK(released.find("_M_ptr = nullptr;") != std::string::npos);
        CHECK(released.find("return __p;") != std::string::npos);
        CHECK(released.find("__prism_check") == std::string::npos);
        CHECK(released.find("__builtin_trap") == std::string::npos);
        CHECK(released.find("::operator delete(") == std::string::npos);
        CHECK(released.find("delete[]") == std::string::npos);
        CHECK(reset.find("_M_ptr = __p;") != std::string::npos);
        CHECK(reset.find("__prism_check") == std::string::npos);
        CHECK(reset.find("__builtin_trap") == std::string::npos);
        CHECK(swapped.find("__prism_check") == std::string::npos);
        CHECK(swapped.find("__builtin_trap") == std::string::npos);
        CHECK(swapped.find("std::swap") == std::string::npos);
    }
    CHECK(count_sub(src, "pointer get() const noexcept { return _M_ptr; }") == 2);
    // unique_ptr, unique_ptr<T[]>, and shared_ptr.
    CHECK(count_sub(src, "explicit operator bool() const noexcept { return _M_ptr != nullptr; }") == 3);
    CHECK(count_sub(src, "::operator delete(") == 1);
    CHECK(count_sub(src, "delete[]") == 1);

    const auto eq = body_of(src, "bool operator==(const unique_ptr<_Tp, _Dp>& __x, const unique_ptr<_Tp, _Dp>& __y)");
    REQUIRE_FALSE(eq.empty());
    // Main compares the stored pointers in one line; C++20 rewrites
    // operator!= from it, so there is no operator!= body.
    CHECK(eq.find("return __x.get() == __y.get();") != std::string::npos);
    CHECK(eq.find("__prism_check") == std::string::npos);
    CHECK(eq.find("__builtin_trap") == std::string::npos);
    CHECK(eq.find("*") == std::string::npos);
    CHECK(src.find("bool operator!=(const unique_ptr<_Tp, _Dp>& __x, const unique_ptr<_Tp, _Dp>& __y)") ==
          std::string::npos);

    const auto one = body_of(src, "make_unique(_Args&&... __args)");
    const auto arr = body_of(src, "make_unique(size_t __n)");
    REQUIRE_FALSE(one.empty());
    REQUIRE_FALSE(arr.empty());
    CHECK(one.find("return unique_ptr<_Tp>(new _Tp(static_cast<_Args&&>(__args)...));") != std::string::npos);
    CHECK(one.find("__prism_check") == std::string::npos);
    CHECK(one.find("__builtin_trap") == std::string::npos);
    CHECK(arr.find("__prism_check(0, \"MEM-OOB-WRITE\", \"make_unique array allocation byte count overflows\")") !=
          std::string::npos);
    CHECK(arr.find("__builtin_trap();") != std::string::npos);
    CHECK(arr.find("__builtin_trap();") < arr.find("new _Elem[__n]()"));
    CHECK(src.find("make_unique_for_overwrite(") == std::string::npos);

    CHECK(src.find("class shared_ptr") != std::string::npos);  // modelled on main
    CHECK(count_sub(src, "class unique_ptr<_Tp[], _Dp>") == 1);
    CHECK(src.find("shared_ptr") != std::string::npos);
}
