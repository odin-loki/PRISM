// The <valarray> model is a partial owning buffer. operator[] checks the
// index before it returns an element. A default valarray has size 0.
// sum() of an empty valarray is 0 and does not load the buffer. A total
// that does not fit is a check and a trap, not a wrapped result.
// min() and max() of an empty valarray are a check and a trap, not a
// value-initialized T. A non-empty valarray returns a stored extreme.
// operator+ and operator- of two valarrays require equal sizes. Different
// sizes are a check and a trap before any load, and that path does not
// return a partial array. Two empty valarrays return an empty valarray.
// An element sum or difference that does not fit is a check and a trap,
// not a wrapped stored element and not a value-initialized T.
//
// tests/cpp/test_cxx_list.cpp compiles a separate program that includes
// <list>. This header has no libstdc++ private headers, so the same kind of
// include compiles in this doctest when the model file is named by path
// (`#include <valarray>` here would be the platform header). The operator[]
// check is also read from the header text.

#include <doctest/doctest.h>

#include "test_support.hpp"

#if defined(_MSC_VER)
#  pragma warning(push)
#  pragma warning(disable : 4068)  // #pragma GCC system_header in the model
#endif
#include "../../src/prism/pir/models/cxx/valarray"
#if defined(_MSC_VER)
#  pragma warning(pop)
#endif

#include <cmath>
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
    CHECK(src.find("#include <algorithm>") == std::string::npos);
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
    CHECK(prism_test::appears_before(release, "_M_data = nullptr;", "::operator delete(__old);"));
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
    CHECK(src.find("#include <algorithm>") == std::string::npos);
    CHECK(src.find("::operator new(__n * sizeof(value_type))") != std::string::npos);
    CHECK(src.find("valarray allocation byte count overflows") != std::string::npos);
    CHECK(src.find("operator+(const value_type&") == std::string::npos);
    CHECK(src.find("operator-(const value_type&") == std::string::npos);
    CHECK(src.find("slice_array") == std::string::npos);
    CHECK(src.find("gslice_array") == std::string::npos);
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

// Empty min/max is the check then the trap in the header text. Calling it
// here would abort the process. The trace covers that path.
namespace {

void require_extreme(const std::string& src, std::string_view sig, std::string_view check_call,
                     std::string_view compare) {
    const auto body = body_of(src, sig);
    REQUIRE(!body.empty());
    const auto check = body.find(check_call);
    const auto trap = body.find("if (_M_size == 0) __builtin_trap();");
    const auto load = body.find("_M_data");
    const auto ret = body.find("return __best;");
    REQUIRE(check != std::string::npos);
    REQUIRE(trap != std::string::npos);
    REQUIRE(load != std::string::npos);
    REQUIRE(ret != std::string::npos);
    CHECK(check < trap);
    CHECK(trap < load);
    CHECK(load < ret);
    CHECK(body.find(compare) != std::string::npos);
    CHECK(body.find("return {}") == std::string::npos);
    CHECK(body.find("return value_type{}") == std::string::npos);
    CHECK(body.find("return value_type()") == std::string::npos);
    CHECK(body.find("return value_type(0)") == std::string::npos);
    CHECK(body.find("return _Tp{}") == std::string::npos);
    CHECK(body.find("return _Tp()") == std::string::npos);
    CHECK(body.find("return T{}") == std::string::npos);
}

struct ValarrayTag {
    int key;
    int id;
    ValarrayTag() = delete;
    ValarrayTag(int k, int i) : key(k), id(i) {}
    bool operator<(const ValarrayTag& o) const { return key < o.key; }
};

// No default constructor. operator+ must not value-initialize an element.
// hits counts element operator+ / operator- / operator* / operator/ calls.
struct ValarrayTouch {
    int v;
    static int hits;
    ValarrayTouch() = delete;
    explicit ValarrayTouch(int x) : v(x) {}
    friend ValarrayTouch operator+(ValarrayTouch a, const ValarrayTouch& b) {
        ++hits;
        a.v += b.v;
        return a;
    }
    friend ValarrayTouch operator-(ValarrayTouch a, const ValarrayTouch& b) {
        ++hits;
        a.v -= b.v;
        return a;
    }
    friend ValarrayTouch operator*(ValarrayTouch a, const ValarrayTouch& b) {
        ++hits;
        a.v *= b.v;
        return a;
    }
    friend ValarrayTouch operator/(ValarrayTouch a, const ValarrayTouch& b) {
        ++hits;
        a.v /= b.v;
        return a;
    }
};

int ValarrayTouch::hits = 0;

// No default constructor. operator% prints nothing here; hits counts calls.
// A size mismatch must not call it. Empty operator% must still compile.
struct ValarrayModTouch {
    int v;
    static int hits;
    ValarrayModTouch() = delete;
    explicit ValarrayModTouch(int x) : v(x) {}
    friend ValarrayModTouch operator%(ValarrayModTouch a, const ValarrayModTouch& b) {
        ++hits;
        a.v %= b.v;
        return a;
    }
};

int ValarrayModTouch::hits = 0;

void require_no_fake_t(const std::string& body) {
    CHECK(body.find("return {}") == std::string::npos);
    CHECK(body.find("return T{}") == std::string::npos);
    CHECK(body.find("return value_type{}") == std::string::npos);
    CHECK(body.find("return value_type()") == std::string::npos);
    CHECK(body.find("return value_type(0)") == std::string::npos);
    CHECK(body.find("return _Tp{}") == std::string::npos);
    CHECK(body.find("return _Tp()") == std::string::npos);
}

void require_binop(const std::string& src, std::string_view sig, std::string_view size_msg, std::string_view elem_sig,
                   std::string_view ovf_msg, std::string_view contract_msg, std::string_view op_expr) {
    const auto body = body_of(src, sig);
    REQUIRE(!body.empty());
    const auto check = body.find(size_msg);
    const auto trap = body.find("if (__x._M_size != __y._M_size) __builtin_trap();");
    const auto empty = body.find("if (__x._M_size == 0) return valarray();");
    const auto load = body.find("_M_data");
    const auto placed = body.find("::new (static_cast<void*>(__r._M_data + __i)) _Tp(__e);");
    const auto sized = body.find("__r._M_size = __x._M_size;");
    const auto ret = body.find("return __r;");
    REQUIRE(check != std::string::npos);
    REQUIRE(trap != std::string::npos);
    REQUIRE(empty != std::string::npos);
    REQUIRE(load != std::string::npos);
    REQUIRE(placed != std::string::npos);
    REQUIRE(sized != std::string::npos);
    REQUIRE(ret != std::string::npos);
    CHECK(check < trap);
    CHECK(trap < empty);
    CHECK(empty < load);
    CHECK(body.substr(0, trap).find("_M_data") == std::string::npos);
    CHECK(body.substr(0, trap).find("return") == std::string::npos);
    CHECK(body.substr(trap, empty - trap).find("_M_data") == std::string::npos);
    CHECK(body.substr(trap, empty - trap).find("return") == std::string::npos);
    const auto loop = body.find("for (size_type __i = 0;");
    REQUIRE(loop != std::string::npos);
    CHECK(loop < sized);
    CHECK(placed < sized);
    CHECK(body.substr(loop, sized - loop).find("return") == std::string::npos);
    CHECK(sized < ret);
    CHECK(body.find("return __r;", ret + 1) == std::string::npos);
    require_no_fake_t(body);
    CHECK(body.find("resize(") == std::string::npos);
    CHECK(body.find("value_type()") == std::string::npos);

    const auto elem = body_of(src, elem_sig);
    REQUIRE(!elem.empty());
    const auto ovf = elem.find(ovf_msg);
    const auto contract = elem.find(contract_msg);
    const auto etrap = elem.find("if (!__ok) __builtin_trap();");
    const auto expr = elem.find(op_expr);
    const auto eret = elem.find("return __r;");
    REQUIRE(ovf != std::string::npos);
    REQUIRE(contract != std::string::npos);
    REQUIRE(etrap != std::string::npos);
    REQUIRE(expr != std::string::npos);
    REQUIRE(eret != std::string::npos);
    CHECK(ovf < etrap);
    CHECK(contract < etrap);
    CHECK(etrap < expr);
    CHECK(expr < eret);
    CHECK(elem.find("return __a + __b") == std::string::npos);
    CHECK(elem.find("return __a - __b") == std::string::npos);
    CHECK(elem.find("_M_data") == std::string::npos);
    CHECK(elem.find("::new") == std::string::npos);
    require_no_fake_t(elem);
}

}  // namespace

TEST_CASE("cxx valarray model: min and max return a stored extreme and an empty valarray traps") {
    const std::string src = slurp(header_path());
    REQUIRE(src.find("class valarray") != std::string::npos);
    CHECK(src.find("value_type min() const") != std::string::npos);
    CHECK(src.find("value_type max() const") != std::string::npos);
    CHECK(src.find("#include <vector>") == std::string::npos);
    CHECK(src.find("#include <memory>") == std::string::npos);
    CHECK(src.find("#include <algorithm>") == std::string::npos);
    CHECK(src.find("min_element") == std::string::npos);
    CHECK(src.find("max_element") == std::string::npos);
    CHECK(src.find("::operator new(__n * sizeof(value_type))") != std::string::npos);
    CHECK(src.find("valarray allocation byte count overflows") != std::string::npos);
    CHECK(src.find("operator+(const value_type&") == std::string::npos);
    CHECK(src.find("operator-(const value_type&") == std::string::npos);
    CHECK(src.find("slice_array") == std::string::npos);
    CHECK(src.find("gslice_array") == std::string::npos);
    CHECK(src.find("shift(") == std::string::npos);
    CHECK(src.find("cshift(") == std::string::npos);
    CHECK(src.find("apply(") == std::string::npos);

    require_extreme(src, "value_type min() const",
                    "__prism_check(_M_size > 0, \"FUNC-CONTRACT\", \"valarray::min of an empty valarray\")",
                    "_M_data[__i] < __best");
    require_extreme(src, "value_type max() const",
                    "__prism_check(_M_size > 0, \"FUNC-CONTRACT\", \"valarray::max of an empty valarray\")",
                    "__best < _M_data[__i]");

    const auto sum = body_of(src, "value_type sum() const");
    REQUIRE(!sum.empty());
    const auto zero = sum.find("if (_M_size == 0) return value_type(0);");
    const auto load = sum.find("_M_data");
    REQUIRE(zero != std::string::npos);
    REQUIRE(load != std::string::npos);
    CHECK(zero < load);

    for (std::size_t n = 0; n < 2; ++n) {
        const auto body = body_of(src, "operator[](size_type __n)", n);
        REQUIRE(!body.empty());
        CHECK(body.find("MEM-OOB-READ") != std::string::npos);
        CHECK(body.find("return _Tp") == std::string::npos);
        CHECK(body.find("return {}") == std::string::npos);
        CHECK(body.find("return value_type") == std::string::npos);
    }

    std::valarray<int> one;
    one.resize(1, 7);
    CHECK(one.min() == 7);
    CHECK(one.max() == 7);
    CHECK(one.sum() == 7);

    std::valarray<int> span;
    span.resize(4, 0);
    span[0] = 4;
    span[1] = -2;
    span[2] = 9;
    span[3] = 3;
    CHECK(span.min() == -2);
    CHECK(span.max() == 9);
    CHECK(span.sum() == 14);

    std::valarray<int> ties;
    ties.resize(3, 5);
    ties[2] = 1;
    CHECK(ties.min() == 1);
    CHECK(ties.max() == 5);

    std::valarray<int> ends;
    ends.resize(3, 0);
    ends[1] = 2;
    ends[2] = -4;
    CHECK(ends.min() == -4);
    CHECK(ends.max() == 2);

    std::valarray<int> lo;
    lo.resize(1, std::numeric_limits<int>::min());
    CHECK(lo.min() == std::numeric_limits<int>::min());
    CHECK(lo.max() == std::numeric_limits<int>::min());
    std::valarray<int> hi;
    hi.resize(2, 0);
    hi[0] = std::numeric_limits<int>::max();
    hi[1] = std::numeric_limits<int>::min();
    CHECK(hi.min() == std::numeric_limits<int>::min());
    CHECK(hi.max() == std::numeric_limits<int>::max());
    CHECK(hi.sum() == -1);

    std::valarray<long long> wide;
    wide.resize(2, 0);
    wide[0] = std::numeric_limits<long long>::max();
    wide[1] = std::numeric_limits<long long>::min();
    CHECK(wide.min() == std::numeric_limits<long long>::min());
    CHECK(wide.max() == std::numeric_limits<long long>::max());

    std::valarray<unsigned> u;
    u.resize(3, 1);
    u[1] = 0;
    u[2] = 9;
    CHECK(u.min() == 0u);
    CHECK(u.max() == 9u);

    std::valarray<double> d;
    d.resize(3, 1.5);
    d[1] = -0.25;
    CHECK(d.min() == -0.25);
    CHECK(d.max() == 1.5);

    std::valarray<bool> b;
    b.resize(2, true);
    b[1] = false;
    CHECK(b.min() == false);
    CHECK(b.max() == true);

    std::valarray<ValarrayTag> tags(ValarrayTag(2, 1), 3);
    tags[1] = ValarrayTag(2, 2);
    tags[2] = ValarrayTag(0, 3);
    const ValarrayTag tmin = tags.min();
    const ValarrayTag tmax = tags.max();
    CHECK(tmin.key == 0);
    CHECK(tmin.id == 3);
    CHECK(tmax.key == 2);
    CHECK((tmax.id == 1 || tmax.id == 2));
}

TEST_CASE("cxx valarray model: operator+ and operator- add equal sizes and trap otherwise") {
    const std::string src = slurp(header_path());
    REQUIRE(src.find("class valarray") != std::string::npos);
    CHECK(src.find("#include <vector>") == std::string::npos);
    CHECK(src.find("#include <memory>") == std::string::npos);
    CHECK(src.find("#include <algorithm>") == std::string::npos);
    CHECK(src.find("#include <initializer_list>") == std::string::npos);
    CHECK(src.find("slice_array") == std::string::npos);
    CHECK(src.find("gslice_array") == std::string::npos);
    CHECK(src.find("shift(") == std::string::npos);
    CHECK(src.find("cshift(") == std::string::npos);
    CHECK(src.find("apply(") == std::string::npos);
    CHECK(src.find("operator+() const") == std::string::npos);
    CHECK(src.find("operator-() const") == std::string::npos);
    CHECK(src.find("operator+(const value_type&") == std::string::npos);
    CHECK(src.find("operator-(const value_type&") == std::string::npos);
    CHECK(src.find("operator+(const _Tp&") == std::string::npos);
    CHECK(src.find("operator-(const _Tp&") == std::string::npos);

    const auto news = std::string("::operator new(__n * sizeof(value_type))");
    const auto at = src.find(news);
    REQUIRE(at != std::string::npos);
    CHECK(src.find(news, at + news.size()) == std::string::npos);
    CHECK(src.find("valarray allocation byte count overflows") != std::string::npos);

    const auto alloc = body_of(src, "static value_type* _S_alloc(size_type __n)");
    REQUIRE(!alloc.empty());
    CHECK(alloc.find(news) != std::string::npos);
    CHECK(alloc.find("valarray allocation byte count overflows") != std::string::npos);

    const auto sum = body_of(src, "value_type sum() const");
    REQUIRE(!sum.empty());
    const auto zero = sum.find("if (_M_size == 0) return value_type(0);");
    const auto sum_load = sum.find("_M_data");
    REQUIRE(zero != std::string::npos);
    REQUIRE(sum_load != std::string::npos);
    CHECK(zero < sum_load);
    const auto add = body_of(src, "static value_type _S_add_checked(value_type __a, value_type __b)");
    REQUIRE(!add.empty());
    const auto sum_ovf =
        add.find("__prism_check(__ok, \"INT-SIGNED-OVF\", \"valarray::sum does not fit in value_type\")");
    const auto sum_trap = add.find("if (!__ok) __builtin_trap();");
    const auto sum_plus = add.find("__r += __b;");
    REQUIRE(sum_ovf != std::string::npos);
    REQUIRE(sum_trap != std::string::npos);
    REQUIRE(sum_plus != std::string::npos);
    CHECK(sum_ovf < sum_trap);
    CHECK(sum_trap < sum_plus);

    require_binop(src, "operator+(const valarray& __x, const valarray& __y)", "valarray::operator+ sizes differ",
                  "static value_type _S_add_elem(value_type __a, value_type __b)",
                  "__prism_check(__ok, \"INT-SIGNED-OVF\", \"valarray::operator+ does not fit in value_type\")",
                  "__prism_check(__ok, \"FUNC-CONTRACT\", \"valarray::operator+ does not fit in value_type\")",
                  "__a + __b");
    require_binop(src, "operator-(const valarray& __x, const valarray& __y)", "valarray::operator- sizes differ",
                  "static value_type _S_sub_elem(value_type __a, value_type __b)",
                  "__prism_check(__ok, \"INT-SIGNED-OVF\", \"valarray::operator- does not fit in value_type\")",
                  "__prism_check(__ok, \"FUNC-CONTRACT\", \"valarray::operator- does not fit in value_type\")",
                  "__a - __b");

    const auto diff = body_of(src, "static int _S_diff_fits(");
    REQUIRE(!diff.empty());
    CHECK(diff.find("return __d") == std::string::npos);
    CHECK(diff.find("return __a - __b") == std::string::npos);
    CHECK(diff.find("return __a + __b") == std::string::npos);

    std::valarray<int> ea;
    std::valarray<int> eb;
    CHECK((ea + eb).size() == 0);
    CHECK((ea - eb).size() == 0);
    std::valarray<int> grown(4);
    grown.resize(0);
    CHECK((grown + eb).size() == 0);
    CHECK((grown - ea).size() == 0);

    std::valarray<int> lhs(3);
    lhs[0] = 1;
    lhs[1] = 2;
    lhs[2] = 4;
    std::valarray<int> rhs(3);
    rhs[0] = 10;
    rhs[1] = 20;
    rhs[2] = 30;
    const std::valarray<int> sm = lhs + rhs;
    const std::valarray<int> df = rhs - lhs;
    CHECK(sm.size() == 3);
    CHECK(sm[0] == 11);
    CHECK(sm[1] == 22);
    CHECK(sm[2] == 34);
    CHECK(df[0] == 9);
    CHECK(df[1] == 18);
    CHECK(df[2] == 26);
    CHECK(lhs[0] == 1);
    CHECK(lhs[2] == 4);
    CHECK(rhs[2] == 30);
    std::valarray<int> alias = lhs + lhs;
    CHECK(alias[0] == 2);
    CHECK(alias[1] == 4);
    CHECK(alias[2] == 8);
    alias[0] = 0;
    CHECK(lhs[0] == 1);
    const std::valarray<int> self_zero = lhs - lhs;
    CHECK(self_zero[0] == 0);
    CHECK(self_zero[1] == 0);
    CHECK(self_zero[2] == 0);

    const auto iv = [](int v) {
        std::valarray<int> a(1);
        a[0] = v;
        return a;
    };
    const int imax = std::numeric_limits<int>::max();
    const int imin = std::numeric_limits<int>::min();
    CHECK((iv(imax) + iv(-1))[0] == imax - 1);
    CHECK((iv(imax) + iv(imin))[0] == -1);
    CHECK((iv(imin) + iv(0))[0] == imin);
    CHECK((iv(imax) - iv(0))[0] == imax);
    CHECK((iv(imin) - iv(imin))[0] == 0);
    CHECK((iv(-1) - iv(imin))[0] == imax);
    CHECK((iv(imin) - iv(-1))[0] == imin + 1);
    CHECK((iv(0) - iv(0))[0] == 0);

    const auto uv = [](unsigned v) {
        std::valarray<unsigned> a(1);
        a[0] = v;
        return a;
    };
    const unsigned umax = std::numeric_limits<unsigned>::max();
    CHECK((uv(umax) + uv(0u))[0] == umax);
    CHECK((uv(umax) - uv(1u))[0] == umax - 1u);
    CHECK((uv(0u) - uv(0u))[0] == 0u);
    CHECK((uv(5u) - uv(3u))[0] == 2u);
    std::valarray<unsigned> ue;
    CHECK((ue + ue).size() == 0);
    CHECK((ue - ue).size() == 0);

    std::valarray<double> da(2);
    da[0] = 1.5;
    da[1] = -0.5;
    std::valarray<double> db(2);
    db[0] = 2.5;
    db[1] = -0.5;
    const std::valarray<double> ds = da + db;
    const std::valarray<double> dd = da - db;
    CHECK(ds[0] == 4.0);
    CHECK(ds[1] == -1.0);
    CHECK(dd[0] == -1.0);
    CHECK(dd[1] == 0.0);
    std::valarray<double> de;
    CHECK((de + de).size() == 0);
    CHECK((de - de).size() == 0);

    const auto lv = [](long long v) {
        std::valarray<long long> a(1);
        a[0] = v;
        return a;
    };
    const long long lmax = std::numeric_limits<long long>::max();
    const long long lmin = std::numeric_limits<long long>::min();
    CHECK((lv(lmax) + lv(-1))[0] == lmax - 1);
    CHECK((lv(lmin) - lv(-1))[0] == lmin + 1);
    CHECK((lv(lmin) - lv(lmin))[0] == 0);
    CHECK((lv(-1) - lv(lmin))[0] == lmax);

    std::valarray<bool> ba(2);
    ba[0] = true;
    ba[1] = false;
    std::valarray<bool> bb(2);
    bb[0] = true;
    bb[1] = true;
    const std::valarray<bool> bs = ba + bb;
    const std::valarray<bool> bd = ba - bb;
    CHECK(bs[0] == true);
    CHECK(bs[1] == true);
    CHECK(bd[0] == false);
    CHECK(bd[1] == true);
    CHECK(ba[1] == false);
    CHECK(bb[0] == true);

    ValarrayTouch::hits = 0;
    std::valarray<ValarrayTouch> te;
    std::valarray<ValarrayTouch> tf;
    const std::valarray<ValarrayTouch> tz = te + tf;
    const std::valarray<ValarrayTouch> tzm = te - tf;
    CHECK(tz.size() == 0);
    CHECK(tzm.size() == 0);
    CHECK(ValarrayTouch::hits == 0);

    std::valarray<ValarrayTouch> ta(ValarrayTouch(2), 2);
    ta[1] = ValarrayTouch(5);
    std::valarray<ValarrayTouch> tb(ValarrayTouch(3), 2);
    tb[1] = ValarrayTouch(1);
    ValarrayTouch::hits = 0;
    const std::valarray<ValarrayTouch> ts = ta + tb;
    CHECK(ValarrayTouch::hits == 2);
    CHECK(ts.size() == 2);
    CHECK(ts[0].v == 5);
    CHECK(ts[1].v == 6);
    CHECK(ta[0].v == 2);
    CHECK(tb[1].v == 1);
    ValarrayTouch::hits = 0;
    const std::valarray<ValarrayTouch> td = ta - tb;
    CHECK(ValarrayTouch::hits == 2);
    CHECK(td[0].v == -1);
    CHECK(td[1].v == 4);
}

TEST_CASE("cxx valarray model: operator* multiplies equal sizes and traps otherwise") {
    const std::string src = slurp(header_path());
    REQUIRE(src.find("class valarray") != std::string::npos);
    CHECK(src.find("#include <vector>") == std::string::npos);
    CHECK(src.find("#include <memory>") == std::string::npos);
    CHECK(src.find("#include <algorithm>") == std::string::npos);
    CHECK(src.find("#include <initializer_list>") == std::string::npos);
    CHECK(src.find("slice_array") == std::string::npos);
    CHECK(src.find("gslice_array") == std::string::npos);
    CHECK(src.find("shift(") == std::string::npos);
    CHECK(src.find("cshift(") == std::string::npos);
    CHECK(src.find("apply(") == std::string::npos);
    CHECK(src.find("operator*(const value_type&") == std::string::npos);
    CHECK(src.find("operator*(const _Tp&") == std::string::npos);
    CHECK(src.find("operator*(value_type") == std::string::npos);
    const auto star = src.find("friend valarray operator*(const valarray& __x, const valarray& __y)");
    REQUIRE(star != std::string::npos);
    CHECK(src.find("friend valarray operator*(const valarray& __x, const valarray& __y)", star + 1) == std::string::npos);
    CHECK(src.find("friend valarray operator+(const valarray& __x, const valarray& __y)") != std::string::npos);
    CHECK(src.find("friend valarray operator-(const valarray& __x, const valarray& __y)") != std::string::npos);
    CHECK(src.find("value_type sum() const") != std::string::npos);
    CHECK(src.find("value_type min() const") != std::string::npos);
    CHECK(src.find("value_type max() const") != std::string::npos);
    CHECK(src.find("valarray(valarray&& __o) noexcept : _M_data(__o._M_data), _M_size(__o._M_size)") !=
          std::string::npos);
    const auto moved = body_of(src, "valarray(valarray&& __o) noexcept");
    REQUIRE(!moved.empty());
    CHECK(moved.find("__o._M_data = nullptr;") != std::string::npos);
    CHECK(moved.find("__o._M_size = 0;") != std::string::npos);
    CHECK(prism_test::appears_before(moved, "__o._M_data = nullptr;", "}"));

    const auto news = std::string("::operator new(__n * sizeof(value_type))");
    const auto at = src.find(news);
    REQUIRE(at != std::string::npos);
    CHECK(src.find(news, at + news.size()) == std::string::npos);

    require_binop(src, "operator*(const valarray& __x, const valarray& __y)", "valarray::operator* sizes differ",
                  "static value_type _S_mul_elem(value_type __a, value_type __b)",
                  "__prism_check(__ok, \"INT-SIGNED-OVF\", \"valarray::operator* does not fit in value_type\")",
                  "__prism_check(__ok, \"FUNC-CONTRACT\", \"valarray::operator* does not fit in value_type\")",
                  "__a * __b");

    const auto elem = body_of(src, "static value_type _S_mul_elem(value_type __a, value_type __b)");
    REQUIRE(!elem.empty());
    const auto etrap = elem.find("if (!__ok) __builtin_trap();");
    REQUIRE(etrap != std::string::npos);
    const auto first_mul = elem.find("__a * __b");
    const auto second_mul = elem.find("__a * __b", first_mul + 1);
    REQUIRE(first_mul != std::string::npos);
    REQUIRE(second_mul != std::string::npos);
    CHECK(etrap < first_mul);
    CHECK(etrap < second_mul);
    CHECK(elem.find("__a * __b", second_mul + 1) == std::string::npos);
    CHECK(elem.find("return __a * __b") == std::string::npos);
    const auto nonint = elem.rfind("} else {");
    REQUIRE(nonint != std::string::npos);
    CHECK(nonint > etrap);
    CHECK(elem.find("__prism_check", nonint) == std::string::npos);
    CHECK(elem.find("__a * __b", nonint) != std::string::npos);
    CHECK(elem.find("is_same_v<value_type, bool>") != std::string::npos);

    const auto fits = body_of(src, "static int _S_prod_fits(");
    REQUIRE(!fits.empty());
    CHECK(fits.find("__a * __b") == std::string::npos);
    CHECK(fits.find("return __a * __b") == std::string::npos);
    CHECK(fits.find("return __p") == std::string::npos);
    CHECK(fits.find("__hi / __b") != std::string::npos);
    CHECK(fits.find("__lo / __a") != std::string::npos);
    CHECK(fits.find("__lo / __b") != std::string::npos);
    CHECK(fits.find("__hi / __a") != std::string::npos);
    CHECK(fits.find("__lo / -") == std::string::npos);

    std::valarray<int> ea;
    std::valarray<int> eb;
    CHECK((ea * eb).size() == 0);
    std::valarray<int> grown(4);
    grown.resize(0);
    CHECK((grown * eb).size() == 0);

    std::valarray<int> lhs(3);
    lhs[0] = 1;
    lhs[1] = 2;
    lhs[2] = 4;
    std::valarray<int> rhs(3);
    rhs[0] = 10;
    rhs[1] = 20;
    rhs[2] = 30;
    const std::valarray<int> prod = lhs * rhs;
    CHECK(prod.size() == 3);
    CHECK(prod[0] == 10);
    CHECK(prod[1] == 40);
    CHECK(prod[2] == 120);
    CHECK(lhs[0] == 1);
    CHECK(lhs[2] == 4);
    CHECK(rhs[2] == 30);
    std::valarray<int> alias = lhs * lhs;
    CHECK(alias[0] == 1);
    CHECK(alias[1] == 4);
    CHECK(alias[2] == 16);
    alias[0] = 0;
    CHECK(lhs[0] == 1);

    const auto iv = [](int v) {
        std::valarray<int> a(1);
        a[0] = v;
        return a;
    };
    const int imax = std::numeric_limits<int>::max();
    const int imin = std::numeric_limits<int>::min();
    CHECK((iv(imax) * iv(1))[0] == imax);
    CHECK((iv(imax) * iv(-1))[0] == -imax);
    CHECK((iv(imax) * iv(0))[0] == 0);
    CHECK((iv(imin) * iv(1))[0] == imin);
    CHECK((iv(imin) * iv(0))[0] == 0);
    CHECK((iv(0) * iv(imin))[0] == 0);
    CHECK((iv(-1) * iv(-1))[0] == 1);
    CHECK((iv(imax / 2) * iv(2))[0] == (imax / 2) * 2);
    CHECK((iv(2) * iv(imin / 2))[0] == imin);
    CHECK((iv(-46340) * iv(46340))[0] == -46340 * 46340);
    CHECK((iv(46340) * iv(46340))[0] == 46340 * 46340);
    CHECK((iv(-46340) * iv(-46340))[0] == 46340 * 46340);

    const auto uv = [](unsigned v) {
        std::valarray<unsigned> a(1);
        a[0] = v;
        return a;
    };
    const unsigned umax = std::numeric_limits<unsigned>::max();
    CHECK((uv(umax) * uv(1u))[0] == umax);
    CHECK((uv(umax) * uv(0u))[0] == 0u);
    CHECK((uv(0u) * uv(umax))[0] == 0u);
    CHECK((uv(umax / 2u) * uv(2u))[0] == (umax / 2u) * 2u);
    CHECK((uv(65535u) * uv(65535u))[0] == 65535u * 65535u);
    std::valarray<unsigned> ue;
    CHECK((ue * ue).size() == 0);

    const auto sc = [](signed char v) {
        std::valarray<signed char> a(1);
        a[0] = v;
        return a;
    };
    const signed char smax = std::numeric_limits<signed char>::max();
    const signed char smin = std::numeric_limits<signed char>::min();
    CHECK(static_cast<int>((sc(smax) * sc(static_cast<signed char>(1)))[0]) == static_cast<int>(smax));
    CHECK(static_cast<int>((sc(smax) * sc(static_cast<signed char>(-1)))[0]) == -static_cast<int>(smax));
    CHECK(static_cast<int>((sc(smin) * sc(static_cast<signed char>(1)))[0]) == static_cast<int>(smin));
    CHECK(static_cast<int>((sc(smin) * sc(static_cast<signed char>(0)))[0]) == 0);
    CHECK(static_cast<int>((sc(static_cast<signed char>(0)) * sc(smin))[0]) == 0);
    CHECK(static_cast<int>((sc(static_cast<signed char>(-1)) * sc(static_cast<signed char>(-1)))[0]) == 1);
    CHECK(static_cast<int>((sc(static_cast<signed char>(2)) * sc(static_cast<signed char>(smin / 2)))[0]) ==
          static_cast<int>(smin));
    CHECK(static_cast<int>((sc(static_cast<signed char>(smax / 2)) * sc(static_cast<signed char>(2)))[0]) ==
          static_cast<int>(smax / 2) * 2);

    const auto uc = [](unsigned char v) {
        std::valarray<unsigned char> a(1);
        a[0] = v;
        return a;
    };
    const unsigned char ucmax = std::numeric_limits<unsigned char>::max();
    CHECK(static_cast<unsigned>((uc(ucmax) * uc(static_cast<unsigned char>(1)))[0]) == ucmax);
    CHECK(static_cast<unsigned>((uc(ucmax) * uc(static_cast<unsigned char>(0)))[0]) == 0u);
    CHECK(static_cast<unsigned>((uc(static_cast<unsigned char>(15)) * uc(static_cast<unsigned char>(17)))[0]) == 255u);

    std::valarray<double> da(2);
    da[0] = 1.5;
    da[1] = -0.5;
    std::valarray<double> db(2);
    db[0] = 2.0;
    db[1] = -4.0;
    const std::valarray<double> dp = da * db;
    CHECK(dp[0] == 3.0);
    CHECK(dp[1] == 2.0);
    std::valarray<double> de;
    CHECK((de * de).size() == 0);
    std::valarray<double> big(1);
    big[0] = std::numeric_limits<double>::max();
    std::valarray<double> two(1);
    two[0] = 2.0;
    const std::valarray<double> huge = big * two;
    CHECK(huge[0] > std::numeric_limits<double>::max());
    CHECK(huge.size() == 1);

    const auto lv = [](long long v) {
        std::valarray<long long> a(1);
        a[0] = v;
        return a;
    };
    const long long lmax = std::numeric_limits<long long>::max();
    const long long lmin = std::numeric_limits<long long>::min();
    CHECK((lv(lmax) * lv(1))[0] == lmax);
    CHECK((lv(lmax) * lv(-1))[0] == -lmax);
    CHECK((lv(lmin) * lv(1))[0] == lmin);
    CHECK((lv(lmin) * lv(0))[0] == 0);
    CHECK((lv(2) * lv(lmin / 2))[0] == lmin);
    CHECK((lv(lmax / 2) * lv(2))[0] == (lmax / 2) * 2);

    std::valarray<bool> ba(3);
    ba[0] = true;
    ba[1] = true;
    ba[2] = false;
    std::valarray<bool> bb(3);
    bb[0] = true;
    bb[1] = false;
    bb[2] = false;
    const std::valarray<bool> bp = ba * bb;
    CHECK(bp[0] == true);
    CHECK(bp[1] == false);
    CHECK(bp[2] == false);
    CHECK(ba[1] == true);
    CHECK(bb[0] == true);
    std::valarray<bool> be;
    CHECK((be * be).size() == 0);

    ValarrayTouch::hits = 0;
    std::valarray<ValarrayTouch> te;
    std::valarray<ValarrayTouch> tf;
    const std::valarray<ValarrayTouch> tz = te * tf;
    CHECK(tz.size() == 0);
    CHECK(ValarrayTouch::hits == 0);

    std::valarray<ValarrayTouch> ta(ValarrayTouch(2), 2);
    ta[1] = ValarrayTouch(5);
    std::valarray<ValarrayTouch> tb(ValarrayTouch(3), 2);
    tb[1] = ValarrayTouch(4);
    ValarrayTouch::hits = 0;
    const std::valarray<ValarrayTouch> ts = ta * tb;
    CHECK(ValarrayTouch::hits == 2);
    CHECK(ts.size() == 2);
    CHECK(ts[0].v == 6);
    CHECK(ts[1].v == 20);
    CHECK(ta[0].v == 2);
    CHECK(tb[1].v == 4);
}

TEST_CASE("cxx valarray model: operator/ divides equal sizes and traps otherwise") {
    const std::string src = slurp(header_path());
    REQUIRE(src.find("class valarray") != std::string::npos);
    CHECK(src.find("#include <vector>") == std::string::npos);
    CHECK(src.find("#include <memory>") == std::string::npos);
    CHECK(src.find("#include <algorithm>") == std::string::npos);
    CHECK(src.find("#include <initializer_list>") == std::string::npos);
    CHECK(src.find("slice_array") == std::string::npos);
    CHECK(src.find("gslice_array") == std::string::npos);
    CHECK(src.find("shift(") == std::string::npos);
    CHECK(src.find("cshift(") == std::string::npos);
    CHECK(src.find("apply(") == std::string::npos);
    CHECK(src.find("operator/(const value_type&") == std::string::npos);
    CHECK(src.find("operator/(const _Tp&") == std::string::npos);
    CHECK(src.find("operator/(value_type") == std::string::npos);
    const auto slash = src.find("friend valarray operator/(const valarray& __x, const valarray& __y)");
    REQUIRE(slash != std::string::npos);
    CHECK(src.find("friend valarray operator/(const valarray& __x, const valarray& __y)", slash + 1) ==
          std::string::npos);
    CHECK(src.find("friend valarray operator*(const valarray& __x, const valarray& __y)") != std::string::npos);
    CHECK(src.find("friend valarray operator+(const valarray& __x, const valarray& __y)") != std::string::npos);
    CHECK(src.find("friend valarray operator-(const valarray& __x, const valarray& __y)") != std::string::npos);
    CHECK(src.find("static value_type _S_mul_elem(value_type __a, value_type __b)") != std::string::npos);
    CHECK(src.find("valarray::operator* sizes differ") != std::string::npos);
    CHECK(src.find("valarray::operator* does not fit in value_type") != std::string::npos);
    CHECK(src.find("value_type sum() const") != std::string::npos);
    CHECK(src.find("value_type min() const") != std::string::npos);
    CHECK(src.find("value_type max() const") != std::string::npos);
    CHECK(src.find("void resize(size_type __n, const value_type& __c = value_type())") != std::string::npos);
    CHECK(src.find("valarray(valarray&& __o) noexcept : _M_data(__o._M_data), _M_size(__o._M_size)") !=
          std::string::npos);

    const auto news = std::string("::operator new(__n * sizeof(value_type))");
    const auto at = src.find(news);
    REQUIRE(at != std::string::npos);
    CHECK(src.find(news, at + news.size()) == std::string::npos);
    CHECK(src.find("valarray allocation byte count overflows") != std::string::npos);

    require_binop(src, "operator/(const valarray& __x, const valarray& __y)", "valarray::operator/ sizes differ",
                  "static value_type _S_div_elem(value_type __a, value_type __b)",
                  "__prism_check(__ok, \"INT-SIGNED-OVF\", \"valarray::operator/ does not fit in value_type\")",
                  "__prism_check(__nz, \"FUNC-CONTRACT\", \"valarray::operator/ divisor is zero\")", "__a / __b");

    const auto body = body_of(src, "operator/(const valarray& __x, const valarray& __y)");
    REQUIRE(!body.empty());
    const auto strap = body.find("if (__x._M_size != __y._M_size) __builtin_trap();");
    const auto sempty = body.find("if (__x._M_size == 0) return valarray();");
    const auto salloc = body.find("_S_alloc(");
    const auto xload = body.find("__x._M_data");
    const auto yload = body.find("__y._M_data");
    const auto stored = body.find("::new (static_cast<void*>(__r._M_data + __i)) _Tp(__e);");
    REQUIRE(strap != std::string::npos);
    REQUIRE(sempty != std::string::npos);
    REQUIRE(salloc != std::string::npos);
    REQUIRE(xload != std::string::npos);
    REQUIRE(yload != std::string::npos);
    REQUIRE(stored != std::string::npos);
    CHECK(strap < sempty);
    CHECK(sempty < salloc);
    CHECK(salloc < xload);
    CHECK(salloc < yload);
    CHECK(xload < stored);
    CHECK(yload < stored);
    CHECK(body.find("::operator new") == std::string::npos);
    CHECK(body.find("__a / __b") == std::string::npos);
    CHECK(body.substr(0, strap).find("_M_data") == std::string::npos);
    CHECK(body.substr(0, sempty).find("__x._M_data") == std::string::npos);
    CHECK(body.substr(0, sempty).find("__y._M_data") == std::string::npos);

    const auto elem = body_of(src, "static value_type _S_div_elem(value_type __a, value_type __b)");
    REQUIRE(!elem.empty());
    const auto zcheck =
        elem.find("__prism_check(__nz, \"FUNC-CONTRACT\", \"valarray::operator/ divisor is zero\")");
    const auto ztrap = elem.find("if (!__nz) __builtin_trap();");
    const auto ocheck =
        elem.find("__prism_check(__ok, \"INT-SIGNED-OVF\", \"valarray::operator/ does not fit in value_type\")");
    const auto otrap = elem.find("if (!__ok) __builtin_trap();");
    const auto first = elem.find("__a / __b");
    REQUIRE(zcheck != std::string::npos);
    REQUIRE(ztrap != std::string::npos);
    REQUIRE(ocheck != std::string::npos);
    REQUIRE(otrap != std::string::npos);
    REQUIRE(first != std::string::npos);
    const auto second = elem.find("__a / __b", first + 1);
    REQUIRE(second != std::string::npos);
    CHECK(zcheck < ztrap);
    CHECK(ztrap < ocheck);
    CHECK(ocheck < otrap);
    CHECK(otrap < first);
    CHECK(otrap < second);
    CHECK(elem.find("__a / __b", second + 1) == std::string::npos);
    CHECK(elem.find("return __a / __b") == std::string::npos);
    CHECK(elem.find("return __a * __b") == std::string::npos);
    const auto nonint = elem.rfind("} else {");
    REQUIRE(nonint != std::string::npos);
    CHECK(nonint > otrap);
    CHECK(elem.find("__prism_check", nonint) == std::string::npos);
    CHECK(elem.find("__a / __b", nonint) != std::string::npos);
    CHECK(elem.find("::new") == std::string::npos);
    CHECK(elem.find("_M_data") == std::string::npos);

    const auto fits = body_of(src, "static int _S_quot_fits(");
    REQUIRE(!fits.empty());
    CHECK(fits.find("/") == std::string::npos);
    CHECK(fits.find("__a / __b") == std::string::npos);
    CHECK(fits.find("numeric_limits<value_type>::min()") != std::string::npos);
    CHECK(fits.find("static_cast<value_type>(-1)") != std::string::npos);
    CHECK(fits.find("return __q") == std::string::npos);
    CHECK(fits.find("return __a") == std::string::npos);
    CHECK(fits.find("return __b") == std::string::npos);

    std::valarray<int> ea;
    std::valarray<int> eb;
    CHECK((ea / eb).size() == 0);
    std::valarray<int> grown(4);
    grown.resize(0);
    CHECK((grown / eb).size() == 0);

    std::valarray<int> lhs(3);
    lhs[0] = 10;
    lhs[1] = 20;
    lhs[2] = 31;
    std::valarray<int> rhs(3);
    rhs[0] = 2;
    rhs[1] = 4;
    rhs[2] = 5;
    const std::valarray<int> quot = lhs / rhs;
    CHECK(quot.size() == 3);
    CHECK(quot[0] == 5);
    CHECK(quot[1] == 5);
    CHECK(quot[2] == 6);
    CHECK(lhs[0] == 10);
    CHECK(lhs[2] == 31);
    CHECK(rhs[2] == 5);
    std::valarray<int> alias = lhs / lhs;
    CHECK(alias[0] == 1);
    CHECK(alias[1] == 1);
    CHECK(alias[2] == 1);
    alias[0] = 0;
    CHECK(lhs[0] == 10);

    const auto iv = [](int v) {
        std::valarray<int> a(1);
        a[0] = v;
        return a;
    };
    const int imax = std::numeric_limits<int>::max();
    const int imin = std::numeric_limits<int>::min();
    CHECK((iv(8) / iv(3))[0] == 2);
    CHECK((iv(3) / iv(8))[0] == 0);
    CHECK((iv(7) / iv(2))[0] == 3);
    CHECK((iv(-7) / iv(2))[0] == -3);
    CHECK((iv(7) / iv(-2))[0] == -3);
    CHECK((iv(-7) / iv(-2))[0] == 3);
    CHECK((iv(imax) / iv(1))[0] == imax);
    CHECK((iv(imax) / iv(-1))[0] == -imax);
    CHECK((iv(imax) / iv(2))[0] == imax / 2);
    CHECK((iv(imin) / iv(1))[0] == imin);
    CHECK((iv(imin) / iv(2))[0] == imin / 2);
    CHECK((iv(imin) / iv(-2))[0] == imin / -2);
    CHECK((iv(0) / iv(1))[0] == 0);
    CHECK((iv(0) / iv(-1))[0] == 0);
    CHECK((iv(0) / iv(imin))[0] == 0);
    CHECK((iv(1) / iv(1))[0] == 1);
    CHECK((iv(-1) / iv(-1))[0] == 1);
    CHECK((iv(imin) / iv(imin))[0] == 1);
    CHECK((iv(imax) / iv(imax))[0] == 1);

    const auto uv = [](unsigned v) {
        std::valarray<unsigned> a(1);
        a[0] = v;
        return a;
    };
    const unsigned umax = std::numeric_limits<unsigned>::max();
    CHECK((uv(umax) / uv(1u))[0] == umax);
    CHECK((uv(umax) / uv(2u))[0] == umax / 2u);
    CHECK((uv(0u) / uv(1u))[0] == 0u);
    CHECK((uv(0u) / uv(umax))[0] == 0u);
    CHECK((uv(15u) / uv(4u))[0] == 3u);
    std::valarray<unsigned> ue;
    CHECK((ue / ue).size() == 0);

    const auto sc = [](signed char v) {
        std::valarray<signed char> a(1);
        a[0] = v;
        return a;
    };
    const signed char smax = std::numeric_limits<signed char>::max();
    const signed char smin = std::numeric_limits<signed char>::min();
    CHECK(static_cast<int>((sc(smax) / sc(static_cast<signed char>(1)))[0]) == static_cast<int>(smax));
    CHECK(static_cast<int>((sc(smax) / sc(static_cast<signed char>(-1)))[0]) == -static_cast<int>(smax));
    CHECK(static_cast<int>((sc(smin) / sc(static_cast<signed char>(1)))[0]) == static_cast<int>(smin));
    CHECK(static_cast<int>((sc(smin) / sc(static_cast<signed char>(2)))[0]) == static_cast<int>(smin) / 2);
    CHECK(static_cast<int>((sc(smin) / sc(static_cast<signed char>(-2)))[0]) == static_cast<int>(smin) / -2);
    CHECK(static_cast<int>((sc(static_cast<signed char>(0)) / sc(static_cast<signed char>(1)))[0]) == 0);
    CHECK(static_cast<int>((sc(static_cast<signed char>(-7)) / sc(static_cast<signed char>(2)))[0]) == -3);

    const auto uc = [](unsigned char v) {
        std::valarray<unsigned char> a(1);
        a[0] = v;
        return a;
    };
    const unsigned char ucmax = std::numeric_limits<unsigned char>::max();
    CHECK(static_cast<unsigned>((uc(ucmax) / uc(static_cast<unsigned char>(1)))[0]) == ucmax);
    CHECK(static_cast<unsigned>((uc(ucmax) / uc(static_cast<unsigned char>(5)))[0]) == 51u);
    CHECK(static_cast<unsigned>((uc(static_cast<unsigned char>(0)) / uc(static_cast<unsigned char>(1)))[0]) == 0u);
    CHECK(static_cast<unsigned>((uc(static_cast<unsigned char>(17)) / uc(static_cast<unsigned char>(3)))[0]) == 5u);

    std::valarray<double> da(2);
    da[0] = 3.0;
    da[1] = -4.0;
    std::valarray<double> db(2);
    db[0] = 2.0;
    db[1] = 2.0;
    const std::valarray<double> dq = da / db;
    CHECK(dq[0] == 1.5);
    CHECK(dq[1] == -2.0);
    std::valarray<double> de;
    CHECK((de / de).size() == 0);
    std::valarray<double> big(1);
    big[0] = std::numeric_limits<double>::max();
    std::valarray<double> half(1);
    half[0] = 0.5;
    const std::valarray<double> huge = big / half;
    CHECK(huge[0] > std::numeric_limits<double>::max());
    CHECK(huge.size() == 1);

    const auto lv = [](long long v) {
        std::valarray<long long> a(1);
        a[0] = v;
        return a;
    };
    const long long lmax = std::numeric_limits<long long>::max();
    const long long lmin = std::numeric_limits<long long>::min();
    CHECK((lv(lmax) / lv(1))[0] == lmax);
    CHECK((lv(lmax) / lv(-1))[0] == -lmax);
    CHECK((lv(lmin) / lv(1))[0] == lmin);
    CHECK((lv(lmin) / lv(2))[0] == lmin / 2);
    CHECK((lv(lmin) / lv(-2))[0] == lmin / -2);
    CHECK((lv(0) / lv(1))[0] == 0);
    CHECK((lv(-7) / lv(2))[0] == -3);

    std::valarray<bool> ba(3);
    ba[0] = true;
    ba[1] = false;
    ba[2] = true;
    std::valarray<bool> bb(3);
    bb[0] = true;
    bb[1] = true;
    bb[2] = true;
    const std::valarray<bool> bq = ba / bb;
    CHECK(bq[0] == true);
    CHECK(bq[1] == false);
    CHECK(bq[2] == true);
    CHECK(ba[1] == false);
    CHECK(bb[0] == true);
    std::valarray<bool> be;
    CHECK((be / be).size() == 0);

    ValarrayTouch::hits = 0;
    std::valarray<ValarrayTouch> te;
    std::valarray<ValarrayTouch> tf;
    const std::valarray<ValarrayTouch> tz = te / tf;
    CHECK(tz.size() == 0);
    CHECK(ValarrayTouch::hits == 0);

    std::valarray<ValarrayTouch> ta(ValarrayTouch(8), 2);
    ta[1] = ValarrayTouch(9);
    std::valarray<ValarrayTouch> tb(ValarrayTouch(2), 2);
    tb[1] = ValarrayTouch(4);
    ValarrayTouch::hits = 0;
    const std::valarray<ValarrayTouch> ts = ta / tb;
    CHECK(ValarrayTouch::hits == 2);
    CHECK(ts.size() == 2);
    CHECK(ts[0].v == 4);
    CHECK(ts[1].v == 2);
    CHECK(ta[0].v == 8);
    CHECK(tb[1].v == 4);
}

TEST_CASE("cxx valarray model: operator% remains of equal sizes and traps otherwise") {
    const std::string src = slurp(header_path());
    REQUIRE(src.find("class valarray") != std::string::npos);
    CHECK(src.find("This is not PROVED and not CLEAN.") != std::string::npos);
    CHECK(src.find("#include <vector>") == std::string::npos);
    CHECK(src.find("#include <memory>") == std::string::npos);
    CHECK(src.find("#include <algorithm>") == std::string::npos);
    CHECK(src.find("#include <initializer_list>") == std::string::npos);
    CHECK(src.find("#include <cmath>") == std::string::npos);
    CHECK(src.find("slice_array") == std::string::npos);
    CHECK(src.find("gslice_array") == std::string::npos);
    CHECK(src.find("shift(") == std::string::npos);
    CHECK(src.find("cshift(") == std::string::npos);
    CHECK(src.find("apply(") == std::string::npos);
    CHECK(src.find("operator%(const value_type&") == std::string::npos);
    CHECK(src.find("operator%(const _Tp&") == std::string::npos);
    CHECK(src.find("operator%(value_type") == std::string::npos);
    CHECK(src.find("operator%=(") == std::string::npos);
    const auto mod = src.find("friend valarray operator%(const valarray& __x, const valarray& __y)");
    REQUIRE(mod != std::string::npos);
    CHECK(src.find("friend valarray operator%(const valarray& __x, const valarray& __y)", mod + 1) ==
          std::string::npos);
    CHECK(src.find("friend valarray operator/(const valarray& __x, const valarray& __y)") != std::string::npos);
    CHECK(src.find("friend valarray operator*(const valarray& __x, const valarray& __y)") != std::string::npos);
    CHECK(src.find("friend valarray operator+(const valarray& __x, const valarray& __y)") != std::string::npos);
    CHECK(src.find("friend valarray operator-(const valarray& __x, const valarray& __y)") != std::string::npos);
    CHECK(src.find("static value_type _S_div_elem(value_type __a, value_type __b)") != std::string::npos);
    CHECK(src.find("valarray::operator/ sizes differ") != std::string::npos);
    CHECK(src.find("valarray::operator/ divisor is zero") != std::string::npos);
    CHECK(src.find("valarray::operator/ does not fit in value_type") != std::string::npos);
    CHECK(src.find("value_type sum() const") != std::string::npos);
    CHECK(src.find("value_type min() const") != std::string::npos);
    CHECK(src.find("value_type max() const") != std::string::npos);
    CHECK(src.find("void resize(size_type __n, const value_type& __c = value_type())") != std::string::npos);
    CHECK(src.find("valarray(valarray&& __o) noexcept : _M_data(__o._M_data), _M_size(__o._M_size)") !=
          std::string::npos);

    const auto news = std::string("::operator new(__n * sizeof(value_type))");
    const auto at = src.find(news);
    REQUIRE(at != std::string::npos);
    CHECK(src.find(news, at + news.size()) == std::string::npos);
    CHECK(src.find("valarray allocation byte count overflows") != std::string::npos);

    require_binop(src, "operator%(const valarray& __x, const valarray& __y)", "valarray::operator% sizes differ",
                  "static value_type _S_mod_elem(value_type __a, value_type __b)",
                  "__prism_check(__ok, \"INT-SIGNED-OVF\", \"valarray::operator% does not fit in value_type\")",
                  "__prism_check(__nz, \"FUNC-CONTRACT\", \"valarray::operator% divisor is zero\")", "__a % __b");

    const auto body = body_of(src, "operator%(const valarray& __x, const valarray& __y)");
    REQUIRE(!body.empty());
    const auto strap = body.find("if (__x._M_size != __y._M_size) __builtin_trap();");
    const auto sempty = body.find("if (__x._M_size == 0) return valarray();");
    const auto salloc = body.find("_S_alloc(");
    const auto xload = body.find("__x._M_data");
    const auto yload = body.find("__y._M_data");
    const auto stored = body.find("::new (static_cast<void*>(__r._M_data + __i)) _Tp(__e);");
    REQUIRE(strap != std::string::npos);
    REQUIRE(sempty != std::string::npos);
    REQUIRE(salloc != std::string::npos);
    REQUIRE(xload != std::string::npos);
    REQUIRE(yload != std::string::npos);
    REQUIRE(stored != std::string::npos);
    CHECK(strap < sempty);
    CHECK(sempty < salloc);
    CHECK(salloc < xload);
    CHECK(salloc < yload);
    CHECK(xload < stored);
    CHECK(yload < stored);
    CHECK(body.find("::operator new") == std::string::npos);
    CHECK(body.find("__a % __b") == std::string::npos);
    CHECK(body.substr(0, strap).find("_M_data") == std::string::npos);
    CHECK(body.substr(0, sempty).find("__x._M_data") == std::string::npos);
    CHECK(body.substr(0, sempty).find("__y._M_data") == std::string::npos);
    CHECK(body.substr(0, strap).find("return") == std::string::npos);

    const auto elem = body_of(src, "static value_type _S_mod_elem(value_type __a, value_type __b)");
    REQUIRE(!elem.empty());
    const auto zcheck =
        elem.find("__prism_check(__nz, \"FUNC-CONTRACT\", \"valarray::operator% divisor is zero\")");
    const auto ztrap = elem.find("if (!__nz) __builtin_trap();");
    const auto ocheck =
        elem.find("__prism_check(__ok, \"INT-SIGNED-OVF\", \"valarray::operator% does not fit in value_type\")");
    const auto otrap = elem.find("if (!__ok) __builtin_trap();");
    const auto first = elem.find("__a % __b");
    REQUIRE(zcheck != std::string::npos);
    REQUIRE(ztrap != std::string::npos);
    REQUIRE(ocheck != std::string::npos);
    REQUIRE(otrap != std::string::npos);
    REQUIRE(first != std::string::npos);
    const auto second = elem.find("__a % __b", first + 1);
    REQUIRE(second != std::string::npos);
    CHECK(zcheck < ztrap);
    CHECK(ztrap < ocheck);
    CHECK(ocheck < otrap);
    CHECK(otrap < first);
    CHECK(otrap < second);
    CHECK(elem.find("__a % __b", second + 1) == std::string::npos);
    CHECK(elem.find("return __a % __b") == std::string::npos);
    CHECK(elem.find("return __a / __b") == std::string::npos);
    const auto fcall = elem.find("_S_float_mod(");
    REQUIRE(fcall != std::string::npos);
    CHECK(ztrap < fcall);
    CHECK(otrap < fcall);
    CHECK(elem.find("__builtin_fmod", fcall) == std::string::npos);
    const auto nonint = elem.rfind("} else {");
    REQUIRE(nonint != std::string::npos);
    CHECK(nonint > otrap);
    CHECK(elem.find("__prism_check", nonint) == std::string::npos);
    CHECK(elem.find("__a % __b", nonint) != std::string::npos);
    CHECK(elem.find("_S_float_mod(", nonint) == std::string::npos);
    CHECK(elem.find("::new") == std::string::npos);
    CHECK(elem.find("_M_data") == std::string::npos);

    const auto fits = body_of(src, "static int _S_rem_fits(");
    REQUIRE(!fits.empty());
    CHECK(fits.find("%") == std::string::npos);
    CHECK(fits.find("/") == std::string::npos);
    CHECK(fits.find("__a % __b") == std::string::npos);
    CHECK(fits.find("numeric_limits<value_type>::min()") != std::string::npos);
    CHECK(fits.find("static_cast<value_type>(-1)") != std::string::npos);
    CHECK(fits.find("return __r") == std::string::npos);
    CHECK(fits.find("return __a") == std::string::npos);
    CHECK(fits.find("return __b") == std::string::npos);

    const auto frem = body_of(src, "static value_type _S_float_mod(value_type __a, value_type __b)");
    REQUIRE(!frem.empty());
    CHECK(frem.find("%") == std::string::npos);
    CHECK(frem.find("__prism_check") == std::string::npos);
    CHECK(frem.find("__builtin_trap") == std::string::npos);
    CHECK(frem.find("__builtin_fmodf") != std::string::npos);
    CHECK(frem.find("__builtin_fmodl") != std::string::npos);
    CHECK(frem.find("__builtin_fmod(") != std::string::npos);
    const auto ffirst = frem.find("__builtin_fmod");
    REQUIRE(ffirst != std::string::npos);
    CHECK(frem.find("return __r", ffirst) != std::string::npos);

    std::valarray<int> ea;
    std::valarray<int> eb;
    CHECK((ea % eb).size() == 0);
    std::valarray<int> grown(4);
    grown.resize(0);
    CHECK((grown % eb).size() == 0);

    std::valarray<int> lhs(3);
    lhs[0] = 10;
    lhs[1] = 20;
    lhs[2] = 31;
    std::valarray<int> rhs(3);
    rhs[0] = 3;
    rhs[1] = 6;
    rhs[2] = 5;
    const std::valarray<int> rem = lhs % rhs;
    CHECK(rem.size() == 3);
    CHECK(rem[0] == 1);
    CHECK(rem[1] == 2);
    CHECK(rem[2] == 1);
    CHECK(lhs[0] == 10);
    CHECK(lhs[2] == 31);
    CHECK(rhs[2] == 5);
    std::valarray<int> alias = lhs % lhs;
    CHECK(alias[0] == 0);
    CHECK(alias[1] == 0);
    CHECK(alias[2] == 0);
    alias[0] = 4;
    CHECK(lhs[0] == 10);

    const auto iv = [](int v) {
        std::valarray<int> a(1);
        a[0] = v;
        return a;
    };
    const int imax = std::numeric_limits<int>::max();
    const int imin = std::numeric_limits<int>::min();
    CHECK((iv(8) % iv(3))[0] == 2);
    CHECK((iv(3) % iv(8))[0] == 3);
    CHECK((iv(7) % iv(2))[0] == 1);
    CHECK((iv(-7) % iv(2))[0] == -1);
    CHECK((iv(7) % iv(-2))[0] == 1);
    CHECK((iv(-7) % iv(-2))[0] == -1);
    CHECK((iv(imax) % iv(1))[0] == 0);
    CHECK((iv(imax) % iv(-1))[0] == 0);
    CHECK((iv(imax) % iv(2))[0] == 1);
    CHECK((iv(imin) % iv(1))[0] == 0);
    CHECK((iv(imin) % iv(2))[0] == 0);
    CHECK((iv(imin) % iv(-2))[0] == 0);
    CHECK((iv(0) % iv(1))[0] == 0);
    CHECK((iv(0) % iv(-1))[0] == 0);
    CHECK((iv(0) % iv(imin))[0] == 0);
    CHECK((iv(1) % iv(1))[0] == 0);
    CHECK((iv(-1) % iv(-1))[0] == 0);
    CHECK((iv(imin) % iv(imin))[0] == 0);
    CHECK((iv(imax) % iv(imax))[0] == 0);

    const auto uv = [](unsigned v) {
        std::valarray<unsigned> a(1);
        a[0] = v;
        return a;
    };
    const unsigned umax = std::numeric_limits<unsigned>::max();
    CHECK((uv(umax) % uv(1u))[0] == 0u);
    CHECK((uv(umax) % uv(2u))[0] == 1u);
    CHECK((uv(0u) % uv(1u))[0] == 0u);
    CHECK((uv(0u) % uv(umax))[0] == 0u);
    CHECK((uv(15u) % uv(4u))[0] == 3u);
    std::valarray<unsigned> ue;
    CHECK((ue % ue).size() == 0);

    const auto sc = [](signed char v) {
        std::valarray<signed char> a(1);
        a[0] = v;
        return a;
    };
    const signed char smax = std::numeric_limits<signed char>::max();
    const signed char smin = std::numeric_limits<signed char>::min();
    CHECK(static_cast<int>((sc(smax) % sc(static_cast<signed char>(1)))[0]) == 0);
    CHECK(static_cast<int>((sc(smax) % sc(static_cast<signed char>(-1)))[0]) == 0);
    CHECK(static_cast<int>((sc(smin) % sc(static_cast<signed char>(1)))[0]) == 0);
    CHECK(static_cast<int>((sc(smin) % sc(static_cast<signed char>(2)))[0]) == 0);
    CHECK(static_cast<int>((sc(smin) % sc(static_cast<signed char>(-2)))[0]) == 0);
    CHECK(static_cast<int>((sc(static_cast<signed char>(0)) % sc(static_cast<signed char>(1)))[0]) == 0);
    CHECK(static_cast<int>((sc(static_cast<signed char>(-7)) % sc(static_cast<signed char>(2)))[0]) == -1);

    const auto uc = [](unsigned char v) {
        std::valarray<unsigned char> a(1);
        a[0] = v;
        return a;
    };
    const unsigned char ucmax = std::numeric_limits<unsigned char>::max();
    CHECK(static_cast<unsigned>((uc(ucmax) % uc(static_cast<unsigned char>(1)))[0]) == 0u);
    CHECK(static_cast<unsigned>((uc(ucmax) % uc(static_cast<unsigned char>(5)))[0]) == 0u);
    CHECK(static_cast<unsigned>((uc(static_cast<unsigned char>(0)) % uc(static_cast<unsigned char>(1)))[0]) == 0u);
    CHECK(static_cast<unsigned>((uc(static_cast<unsigned char>(17)) % uc(static_cast<unsigned char>(3)))[0]) == 2u);

    std::valarray<double> da(2);
    da[0] = 5.5;
    da[1] = -5.5;
    std::valarray<double> db(2);
    db[0] = 2.0;
    db[1] = 2.0;
    const std::valarray<double> dr = da % db;
    CHECK(dr[0] == 1.5);
    CHECK(dr[1] == -1.5);
    std::valarray<double> de;
    CHECK((de % de).size() == 0);
    std::valarray<double> neg0(1);
    neg0[0] = std::copysign(0.0, -1.0);
    std::valarray<double> two(1);
    two[0] = 2.0;
    const std::valarray<double> nrem = neg0 % two;
    CHECK(nrem[0] == 0.0);
    CHECK(std::signbit(nrem[0]));
    std::valarray<double> big(1);
    big[0] = std::numeric_limits<double>::max();
    std::valarray<double> half(1);
    half[0] = 0.5;
    const std::valarray<double> huge = big % half;
    CHECK(huge[0] == 0.0);
    CHECK(huge.size() == 1);
    std::valarray<float> fa(1);
    fa[0] = 5.5f;
    std::valarray<float> fb(1);
    fb[0] = 2.0f;
    CHECK((fa % fb)[0] == 1.5f);
    std::valarray<long double> la(1);
    la[0] = -5.5L;
    std::valarray<long double> lb(1);
    lb[0] = -2.0L;
    CHECK((la % lb)[0] == -1.5L);

    const auto lv = [](long long v) {
        std::valarray<long long> a(1);
        a[0] = v;
        return a;
    };
    const long long lmax = std::numeric_limits<long long>::max();
    const long long lmin = std::numeric_limits<long long>::min();
    CHECK((lv(lmax) % lv(1))[0] == 0);
    CHECK((lv(lmax) % lv(-1))[0] == 0);
    CHECK((lv(lmin) % lv(1))[0] == 0);
    CHECK((lv(lmin) % lv(2))[0] == 0);
    CHECK((lv(lmin) % lv(-2))[0] == 0);
    CHECK((lv(0) % lv(1))[0] == 0);
    CHECK((lv(-7) % lv(2))[0] == -1);

    std::valarray<bool> ba(3);
    ba[0] = true;
    ba[1] = false;
    ba[2] = true;
    std::valarray<bool> bb(3);
    bb[0] = true;
    bb[1] = true;
    bb[2] = true;
    const std::valarray<bool> br = ba % bb;
    CHECK(br[0] == false);
    CHECK(br[1] == false);
    CHECK(br[2] == false);
    CHECK(ba[1] == false);
    CHECK(bb[0] == true);
    std::valarray<bool> be;
    CHECK((be % be).size() == 0);

    ValarrayModTouch::hits = 0;
    std::valarray<ValarrayModTouch> te;
    std::valarray<ValarrayModTouch> tf;
    const std::valarray<ValarrayModTouch> tz = te % tf;
    CHECK(tz.size() == 0);
    CHECK(ValarrayModTouch::hits == 0);

    std::valarray<ValarrayModTouch> ta(ValarrayModTouch(8), 2);
    ta[1] = ValarrayModTouch(9);
    std::valarray<ValarrayModTouch> tb(ValarrayModTouch(3), 2);
    tb[1] = ValarrayModTouch(4);
    ValarrayModTouch::hits = 0;
    const std::valarray<ValarrayModTouch> ts = ta % tb;
    CHECK(ValarrayModTouch::hits == 2);
    CHECK(ts.size() == 2);
    CHECK(ts[0].v == 2);
    CHECK(ts[1].v == 1);
    CHECK(ta[0].v == 8);
    CHECK(tb[1].v == 4);
}
