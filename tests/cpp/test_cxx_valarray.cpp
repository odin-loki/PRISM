// The <valarray> model is a partial owning buffer. operator[] checks the
// index before it returns an element. A default valarray has size 0.
// sum() of an empty valarray is 0 and does not load the buffer. A total
// that does not fit is a check and a trap, not a wrapped result.
//
// tests/cpp/test_cxx_list.cpp compiles a separate program that includes
// <list>. This header has no libstdc++ private headers, so the same kind of
// include compiles in this doctest when the model file is named by path
// (`#include <valarray>` here would be the platform header). The operator[]
// check is also read from the header text.

#include <doctest/doctest.h>

#if defined(_MSC_VER)
#  pragma warning(push)
#  pragma warning(disable : 4068)  // #pragma GCC system_header in the model
#endif
#include "../../src/prism/pir/models/cxx/valarray"
#if defined(_MSC_VER)
#  pragma warning(pop)
#endif

#include <filesystem>
#include <fstream>
#include <limits>
#include <sstream>
#include <string>
#include <string_view>

namespace fs = std::filesystem;

namespace {

fs::path repo() { return fs::path(PRISM_SOURCE_DIR); }

fs::path header_path() { return repo() / "src" / "prism" / "pir" / "models" / "cxx" / "valarray"; }

std::string slurp(const fs::path& p) {
    std::ifstream in(p);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
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
            if (src[i] == '{')
                ++depth;
            else if (src[i] == '}') {
                if (--depth == 0) return std::string(src.substr(brace, i - brace + 1));
            }
        }
        return {};
    }
}

}  // namespace

TEST_CASE("cxx valarray model: operator[] checks the index and a default valarray has size 0") {
    const fs::path header = header_path();
    REQUIRE(fs::is_regular_file(header));
    CHECK_FALSE(fs::is_directory(header));
    const std::string src = slurp(header);
    REQUIRE(src.find("class valarray") != std::string::npos);
    CHECK(src.find("#include <vector>") == std::string::npos);
    CHECK(src.find("#include <memory>") == std::string::npos);
    CHECK(src.find("#include <initializer_list>") == std::string::npos);
    CHECK(src.find("::operator new") != std::string::npos);
    CHECK(src.find("::operator delete(") != std::string::npos);
    CHECK(src.find("delete[]") == std::string::npos);

    for (std::size_t n = 0; n < 2; ++n) {
        const auto body = body_of(src, "operator[](size_type __n)", n);
        REQUIRE(!body.empty());
        const auto check = body.find("__prism_check");
        const auto ret = body.find("return _M_data[__n];");
        REQUIRE(check != std::string::npos);
        REQUIRE(ret != std::string::npos);
        CHECK(check < ret);
        CHECK(body.find("__n < this->size()") != std::string::npos);
        CHECK(body.find("return _Tp") == std::string::npos);
        CHECK(body.find("return {}") == std::string::npos);
        CHECK(body.find("return value_type") == std::string::npos);
    }

    CHECK(src.find("size_type size() const noexcept { return _M_size; }") != std::string::npos);
    CHECK(src.find("size_type _M_size = 0;") != std::string::npos);
    CHECK(src.find("valarray() noexcept = default;") != std::string::npos);

    const auto resize = body_of(src, "void resize(size_type __n, const value_type& __c = value_type())");
    const auto release = body_of(src, "void _M_release() noexcept");
    REQUIRE(!resize.empty());
    REQUIRE(!release.empty());
    const auto drop = resize.find("_M_release()");
    const auto store = resize.find("_M_data = __fresh;");
    REQUIRE(drop != std::string::npos);
    REQUIRE(store != std::string::npos);
    CHECK(drop < store);
    CHECK(release.find("_M_data = nullptr;") != std::string::npos);
    CHECK(release.find("_M_data = nullptr;") < release.find("::operator delete(__old);"));
    CHECK(release.find("__old[__i].~_Tp();") != std::string::npos);

    std::valarray<int> empty;
    CHECK(empty.size() == 0);
    empty.resize(1, 4);
    CHECK(empty.size() == 1);
    empty.resize(3, 8);
    CHECK(empty.size() == 3);
    empty.resize(1, 2);
    CHECK(empty.size() == 1);
    empty.resize(0);
    CHECK(empty.size() == 0);
    // Default storage is a null buffer. A load here would fault.
    CHECK(empty.sum() == 0);
}

TEST_CASE("cxx valarray model: sum of an empty valarray is 0 and a total that does not fit traps") {
    const std::string src = slurp(header_path());
    REQUIRE(src.find("class valarray") != std::string::npos);
    CHECK(src.find("#include <vector>") == std::string::npos);
    CHECK(src.find("#include <memory>") == std::string::npos);
    CHECK(src.find("::operator new(__n * sizeof(value_type))") != std::string::npos);
    CHECK(src.find("valarray allocation byte count overflows") != std::string::npos);
    CHECK(src.find("operator+") == std::string::npos);
    CHECK(src.find("slice_array") == std::string::npos);
    CHECK(src.find("gslice_array") == std::string::npos);
    CHECK(src.find("value_type min(") == std::string::npos);
    CHECK(src.find("value_type max(") == std::string::npos);
    CHECK(src.find("shift(") == std::string::npos);
    CHECK(src.find("cshift(") == std::string::npos);
    CHECK(src.find("apply(") == std::string::npos);

    const auto sum = body_of(src, "value_type sum() const");
    REQUIRE(!sum.empty());
    const auto zero = sum.find("if (_M_size == 0) return value_type(0);");
    const auto load = sum.find("_M_data");
    REQUIRE(zero != std::string::npos);
    REQUIRE(load != std::string::npos);
    CHECK(zero < load);
    CHECK(sum.find("return value_type{}") == std::string::npos);

    const auto add = body_of(src, "static value_type _S_add_checked(value_type __a, value_type __b)");
    REQUIRE(!add.empty());
    const auto ovf = add.find("__prism_check(__ok, \"INT-SIGNED-OVF\", \"valarray::sum does not fit in value_type\")");
    const auto contract =
        add.find("__prism_check(__ok, \"FUNC-CONTRACT\", \"valarray::sum does not fit in value_type\")");
    const auto trap = add.find("__builtin_trap()");
    const auto plus = add.find("__r += __b;");
    REQUIRE(ovf != std::string::npos);
    REQUIRE(contract != std::string::npos);
    REQUIRE(trap != std::string::npos);
    REQUIRE(plus != std::string::npos);
    CHECK(ovf < trap);
    CHECK(contract < trap);
    CHECK(trap < plus);
    CHECK(add.find("return __a + __b") == std::string::npos);
    CHECK(add.find("return __s") == std::string::npos);
    const auto fits = body_of(src, "static int _S_sum_fits(");
    REQUIRE(!fits.empty());
    CHECK(fits.find("return __s") == std::string::npos);
    CHECK(fits.find("return __a + __b") == std::string::npos);
    CHECK(fits.find("return __wide") == std::string::npos);

    for (std::size_t n = 0; n < 2; ++n) {
        const auto body = body_of(src, "operator[](size_type __n)", n);
        REQUIRE(!body.empty());
        CHECK(body.find("MEM-OOB-READ") != std::string::npos);
        CHECK(body.find("return _Tp") == std::string::npos);
        CHECK(body.find("return {}") == std::string::npos);
        CHECK(body.find("return value_type") == std::string::npos);
    }

    std::valarray<int> empty;
    CHECK(empty.sum() == 0);
    std::valarray<int> one;
    one.resize(1, 7);
    CHECK(one.sum() == 7);
    std::valarray<int> mix;
    mix.resize(3, 0);
    mix[0] = 1;
    mix[1] = 2;
    mix[2] = 4;
    CHECK(mix.sum() == 7);
    std::valarray<int> neg;
    neg.resize(2, -4);
    CHECK(neg.sum() == -8);

    std::valarray<int> edge;
    edge.resize(2, 0);
    edge[0] = std::numeric_limits<int>::max();
    edge[1] = -1;
    CHECK(edge.sum() == std::numeric_limits<int>::max() - 1);
    edge[1] = 0;
    CHECK(edge.sum() == std::numeric_limits<int>::max());
    edge[0] = std::numeric_limits<int>::min();
    edge[1] = 1;
    CHECK(edge.sum() == std::numeric_limits<int>::min() + 1);
    edge[1] = 0;
    CHECK(edge.sum() == std::numeric_limits<int>::min());
    edge[0] = 0;
    edge[1] = std::numeric_limits<int>::min();
    CHECK(edge.sum() == std::numeric_limits<int>::min());
    edge[1] = std::numeric_limits<int>::max();
    CHECK(edge.sum() == std::numeric_limits<int>::max());
    std::valarray<long long> ll;
    ll.resize(2, 0);
    ll[0] = 0;
    ll[1] = std::numeric_limits<long long>::min();
    CHECK(ll.sum() == std::numeric_limits<long long>::min());
    ll[0] = std::numeric_limits<long long>::max();
    ll[1] = -1;
    CHECK(ll.sum() == std::numeric_limits<long long>::max() - 1);

    std::valarray<unsigned> uempty;
    CHECK(uempty.sum() == 0u);
    std::valarray<unsigned> u;
    u.resize(2, 0);
    u[0] = std::numeric_limits<unsigned>::max();
    u[1] = 0;
    CHECK(u.sum() == std::numeric_limits<unsigned>::max());

    std::valarray<double> dempty;
    CHECK(dempty.sum() == 0.0);
    std::valarray<double> d;
    d.resize(2, 0.0);
    d[0] = 1.5;
    d[1] = -0.5;
    CHECK(d.sum() == 1.0);
}
