// The <memory> model is a partial unique_ptr plus make_unique. These checks
// read the header text: empty operator* is a __prism_check, reset frees the
// previous object once, move leaves the source empty without a second free,
// and make_unique<T[]> traps on a byte count that does not fit before new.

#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>

namespace fs = std::filesystem;

namespace {

fs::path repo() { return fs::path(PRISM_SOURCE_DIR); }

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

// Body of the nth function whose text contains `sig`, including braces.
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
            if (src[i] == '{') ++depth;
            else if (src[i] == '}') {
                if (--depth == 0) return std::string(src.substr(brace, i - brace + 1));
            }
        }
        return {};
    }
}

}  // namespace

TEST_CASE("cxx memory model: empty operator* checks, reset frees, move does not double-free") {
    const auto src = slurp(repo() / "src" / "prism" / "pir" / "models" / "cxx" / "memory");
    REQUIRE(!src.empty());

    const auto star = body_of(src, "element_type& operator*() const");
    REQUIRE(!star.empty());
    CHECK(star.find("__prism_check") != std::string::npos);
    CHECK(star.find("_M_ptr != nullptr") != std::string::npos);
    CHECK(star.find("return *_M_ptr") != std::string::npos);
    CHECK(star.find("element_type{") == std::string::npos);
    CHECK(star.find("element_type(") == std::string::npos);

    const auto arrow = body_of(src, "pointer operator->() const");
    REQUIRE(!arrow.empty());
    CHECK(arrow.find("__prism_check") != std::string::npos);
    CHECK(arrow.find("_M_ptr != nullptr") != std::string::npos);

    const auto obj_reset = body_of(src, "void reset(pointer __p = nullptr) noexcept", 0);
    const auto arr_reset = body_of(src, "void reset(pointer __p = nullptr) noexcept", 1);
    REQUIRE(!obj_reset.empty());
    REQUIRE(!arr_reset.empty());
    CHECK(obj_reset.find("_M_ptr = __p") != std::string::npos);
    CHECK(obj_reset.find("_M_ptr = __p") < obj_reset.find("::operator delete("));
    CHECK(obj_reset.find("__old != nullptr") != std::string::npos);
    CHECK(obj_reset.find("__old != nullptr") < obj_reset.find("::operator delete("));
    CHECK(count_sub(obj_reset, "::operator delete(") == 1);
    CHECK(obj_reset.find("delete[]") == std::string::npos);
    CHECK(arr_reset.find("_M_ptr = __p") != std::string::npos);
    CHECK(arr_reset.find("_M_ptr = __p") < arr_reset.find("delete[]"));
    CHECK(arr_reset.find("__old != nullptr") != std::string::npos);
    CHECK(arr_reset.find("__old != nullptr") < arr_reset.find("delete[]"));
    CHECK(count_sub(arr_reset, "delete[]") == 1);
    CHECK(arr_reset.find("::operator delete(") == std::string::npos);

    // One free in the whole header. Move and release do not add another.
    CHECK(count_sub(src, "::operator delete(") == 1);
    CHECK(count_sub(src, "delete[]") == 1);

    CHECK(count_sub(src, "unique_ptr(unique_ptr&& __u) noexcept : _M_ptr(__u.release()) {}") == 2);
    CHECK(count_sub(src, "~unique_ptr() noexcept { reset(); }") == 2);
    for (std::size_t n = 0; n < 2; ++n) {
        const auto moved = body_of(src, "unique_ptr& operator=(unique_ptr&& __u) noexcept", n);
        const auto released = body_of(src, "pointer release() noexcept", n);
        REQUIRE(!moved.empty());
        REQUIRE(!released.empty());
        CHECK(moved.find("reset(__u.release())") != std::string::npos);
        CHECK(moved.find("::operator delete(") == std::string::npos);
        CHECK(moved.find("delete[]") == std::string::npos);
        CHECK(released.find("_M_ptr = nullptr") != std::string::npos);
        CHECK(released.find("::operator delete(") == std::string::npos);
        CHECK(released.find("delete[]") == std::string::npos);
    }

    CHECK(count_sub(src, "pointer get() const noexcept { return _M_ptr; }") == 2);
    CHECK(count_sub(src, "unique_ptr() noexcept : _M_ptr(nullptr) {}") == 2);
    CHECK(src.find("class shared_ptr") == std::string::npos);
    CHECK(src.find("class weak_ptr") == std::string::npos);
    CHECK(src.find("class allocator") == std::string::npos);
    CHECK(src.find("pointer_traits") != std::string::npos);
    CHECK(src.find("shared_ptr") != std::string::npos);
    CHECK(src.find("uninitialized_") != std::string::npos);
}

TEST_CASE("cxx memory model: make_unique owns a new object, array overflow traps before new") {
    const auto src = slurp(repo() / "src" / "prism" / "pir" / "models" / "cxx" / "memory");
    REQUIRE(!src.empty());

    const auto one = body_of(src, "make_unique(_Args&&... __args)");
    REQUIRE(!one.empty());
    CHECK(one.find("return unique_ptr<_Tp>(new _Tp(static_cast<_Args&&>(__args)...))") != std::string::npos);
    CHECK(one.find("__prism_check") == std::string::npos);
    CHECK(one.find("__builtin_trap") == std::string::npos);
    CHECK(one.find("new _Tp[") == std::string::npos);
    CHECK(one.find("delete[]") == std::string::npos);
    CHECK(one.find("::operator delete(") == std::string::npos);
    CHECK(one.find("allocator") == std::string::npos);

    const auto arr = body_of(src, "make_unique(size_t __n)");
    REQUIRE(!arr.empty());
    const auto check = arr.find("__prism_check(0, \"MEM-OOB-WRITE\", \"make_unique array allocation byte count overflows\")");
    const auto trap = arr.find("__builtin_trap()");
    const auto neu = arr.find("new _Elem[__n]()");
    REQUIRE(check != std::string::npos);
    REQUIRE(trap != std::string::npos);
    REQUIRE(neu != std::string::npos);
    CHECK(check < neu);
    CHECK(trap < neu);
    CHECK(trap > check);
    CHECK(arr.find("sizeof(_Elem) == 0 || __n > (SIZE_MAX / sizeof(_Elem))") != std::string::npos);
    CHECK(arr.find("new _Elem[1]") == std::string::npos);
    CHECK(arr.find("__n = 1") == std::string::npos);
    CHECK(arr.find("__n *") == std::string::npos);
    CHECK(arr.find("delete[]") == std::string::npos);
    CHECK(arr.find("::operator delete(") == std::string::npos);
    CHECK(arr.find("allocator") == std::string::npos);

    CHECK(src.find("make_unique_for_overwrite(") == std::string::npos);
    CHECK(src.find("class shared_ptr") == std::string::npos);
    CHECK(src.find("shared_ptr") != std::string::npos);
}
