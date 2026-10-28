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
// hits counts element operator+ / operator- calls.
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
};

int ValarrayTouch::hits = 0;

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
