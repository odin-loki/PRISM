// Direct checks of prism::cxx_model::ostream_buf. The header is included by
// path. It is not a substitute for <iostream>.

#include <doctest/doctest.h>

#include "../../src/prism/pir/models/cxx/iostream/prism_ostream.hpp"

#include <cstring>
#include <limits>
#include <utility>

#if defined(_GLIBCXX_IOSTREAM) || defined(_GLIBCXX_OSTREAM) || defined(_GLIBCXX_SSTREAM)
#error prism ostream model must not mask libstdc++ iostream headers
#endif

using prism::cxx_model::ostream_buf;

static_assert(std::numeric_limits<int>::max() == 2147483647,
              "the decimal checks below expect a 32-bit int");

namespace {

bool same(const ostream_buf& os, const char* text) {
    const std::size_t n = std::strlen(text);
    if (os.size() != n) return false;
    if (os.data()[n] != '\0') return false;
    for (std::size_t i = 0; i < n; ++i)
        if (os.data()[i] != text[i]) return false;
    return true;
}

}  // namespace

TEST_CASE("cxx ostream buffer records decimal integers and strings") {
    ostream_buf os;
    os << 42 << ":" << -7 << ":" << "ok";
    CHECK(same(os, "42:-7:ok"));
    CHECK_FALSE(os.failed());
    CHECK(static_cast<bool>(os));
    CHECK(os.capacity() == ostream_buf::local_capacity);

    ostream_buf zero;
    zero << 0;
    CHECK(same(zero, "0"));

    ostream_buf lo;
    lo << std::numeric_limits<int>::min();
    CHECK(same(lo, "-2147483648"));

    ostream_buf hi;
    hi << std::numeric_limits<int>::max();
    CHECK(same(hi, "2147483647"));

    ostream_buf empty_str;
    empty_str << "";
    CHECK(same(empty_str, ""));
    CHECK_FALSE(empty_str.failed());
}

TEST_CASE("cxx ostream buffer grows like the string model and keeps a trailing NUL") {
    ostream_buf os;
    const char text[] = "0123456789abcdefGHIJ";  // 20 characters, past the local buffer
    os.write(text, 20);
    CHECK(same(os, text));
    // 20 > 15 and 20 < 2*15, so the capacity becomes 30 (basic_string _M_create).
    CHECK(os.capacity() == 30);
    os << 5;
    CHECK(same(os, "0123456789abcdefGHIJ5"));
    CHECK(os.capacity() == 30);

    // A request of exactly twice the old capacity stays at that size
    // (_M_create grows only when the request is strictly below 2 * old).
    ostream_buf edge;
    char thirty[30];
    for (int i = 0; i < 30; ++i) thirty[i] = 'a';
    edge.write(thirty, 30);
    CHECK(edge.size() == 30);
    CHECK(edge.capacity() == 30);
    CHECK(edge.data()[30] == '\0');
    edge.write("b", 1);
    CHECK(edge.size() == 31);
    CHECK(edge.capacity() == 60);
    CHECK(edge.data()[30] == 'b');
    CHECK(edge.data()[31] == '\0');
}

TEST_CASE("cxx ostream buffer does not record inserts once it has failed") {
    ostream_buf os;
    os << "keep";
    os.set_bad();
    CHECK(os.failed());
    CHECK_FALSE(static_cast<bool>(os));
    os << 99 << "more";
    os.write("zzz", 3);
    CHECK(same(os, "keep"));

    ostream_buf fresh;
    fresh.set_bad();
    fresh << 1;
    CHECK(fresh.size() == 0);
    CHECK(fresh.failed());

    ostream_buf nil;
    nil << "hi";
    nil << static_cast<const char*>(nullptr);
    CHECK(nil.failed());
    CHECK(same(nil, "hi"));
    nil << "there";
    CHECK(same(nil, "hi"));

    ostream_buf counted;
    counted.write(nullptr, 0);
    CHECK_FALSE(counted.failed());
    CHECK(counted.size() == 0);
    counted << "ok";
    counted.write(nullptr, 4);
    CHECK(counted.failed());
    CHECK(same(counted, "ok"));

    ostream_buf huge;
    huge << "ok";
    char one = 'q';
    huge.write(&one, static_cast<std::size_t>(-1));
    CHECK(huge.failed());
    CHECK(same(huge, "ok"));
}

TEST_CASE("cxx ostream buffer move leaves a usable empty source") {
    ostream_buf src;
    src.write("0123456789abcdef", 16);
    CHECK(src.capacity() == 30);
    ostream_buf dst(std::move(src));
    CHECK(same(dst, "0123456789abcdef"));
    CHECK(dst.capacity() == 30);
    ostream_buf assigned;
    assigned << "zz";
    assigned = std::move(dst);
    CHECK(same(assigned, "0123456789abcdef"));
    CHECK(assigned.capacity() == 30);
    CHECK(dst.size() == 0);
    CHECK(same(dst, ""));
    CHECK(src.size() == 0);
    CHECK_FALSE(src.failed());
    CHECK(same(src, ""));
    src << 3;
    CHECK(same(src, "3"));

    ostream_buf bad;
    bad << "x";
    bad.set_bad();
    ostream_buf moved_bad(std::move(bad));
    CHECK(moved_bad.failed());
    CHECK(same(moved_bad, "x"));
    moved_bad << "y";
    CHECK(same(moved_bad, "x"));
    CHECK_FALSE(bad.failed());
    CHECK(same(bad, ""));
}
