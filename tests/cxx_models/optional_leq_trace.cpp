// Differential trace of std::optional::operator<=
// (tests/cpp/test_cxx_optional.cpp): compiled once against libstdc++ and once
// with PRISM's model header (src/prism/pir/models/cxx/optional) first on the
// include path, both under ASan/UBSan with _GLIBCXX_ASSERTIONS. Every line
// must agree. Both headers call operator<= on the stored T, in stored order.
//
// Two disengaged optionals compare less-or-equal and do not read the inactive
// T. A disengaged left and an engaged right is true and does not read the
// disengaged storage. An engaged left and a disengaged right is false and
// does not read the disengaged storage. Two engaged optionals compare the
// stored values. The comparison does not return T{}. This is not PROVED and
// not CLEAN.
//
// operator==, operator<, operator>, swap, emplace, value, operator*, and
// operator-> are declared; this trace does not call them. operator>= and
// operator<=> are not called.
#include <cstdio>
#include <cstdlib>
#include <optional>

// Native stand-in for the PIR intrinsic. This trace does not call value().
extern "C" void __prism_check(int ok, const char* cls, const char* msg) {
    if (!ok) {
        std::fprintf(stderr, "CHECK %s: %s\n", cls, msg);
        std::fflush(stderr);
        std::abort();
    }
}

// No default constructor. operator< prints lt, operator<= prints le, and
// operator> prints gt. A read of an inactive Box shows up as one of those
// lines before any engaged pair. operator<= on optional must print le only.
struct Box {
    int v;
    Box() = delete;
    explicit Box(int x) : v(x) {}
    bool operator<(const Box& o) const {
        std::printf("lt %d %d\n", v, o.v);
        return v < o.v;
    }
    bool operator<=(const Box& o) const {
        std::printf("le %d %d\n", v, o.v);
        return v <= o.v;
    }
    bool operator>(const Box& o) const {
        std::printf("gt %d %d\n", v, o.v);
        return v > o.v;
    }
};

int main() {
    std::optional<Box> e1;
    std::optional<Box> e2;
    std::printf("ee le=%d\n", static_cast<int>(e1 <= e2));
    std::printf("ee rev=%d\n", static_cast<int>(e2 <= e1));
    std::optional<Box> one(Box(1));
    std::printf("de le=%d\n", static_cast<int>(e1 <= one));
    std::printf("ed le=%d\n", static_cast<int>(one <= e1));
    std::optional<Box> one_b(Box(1));
    std::printf("same le=%d\n", static_cast<int>(one <= one_b));
    std::printf("same rev=%d\n", static_cast<int>(one_b <= one));
    std::printf("self le=%d\n", static_cast<int>(one <= one));
    std::optional<Box> two(Box(2));
    std::printf("ord le=%d\n", static_cast<int>(one <= two));
    std::printf("ord rev=%d\n", static_cast<int>(two <= one));
    const std::optional<Box> cempty;
    const std::optional<Box> ctwo(Box(2));
    const std::optional<Box> ctwo_b(Box(2));
    std::printf("cde le=%d\n", static_cast<int>(cempty <= ctwo));
    std::printf("ced le=%d\n", static_cast<int>(ctwo <= cempty));
    std::printf("c22 le=%d\n", static_cast<int>(ctwo <= ctwo_b));
    std::printf("c22 rev=%d\n", static_cast<int>(ctwo_b <= ctwo));
    std::optional<Box> neg(Box(-1));
    std::printf("nde le=%d\n", static_cast<int>(e1 <= neg));
    std::printf("ned le=%d\n", static_cast<int>(neg <= e1));
    std::printf("ngt le=%d\n", static_cast<int>(one <= neg));
    std::printf("ngt rev=%d\n", static_cast<int>(neg <= one));
    std::optional<int> ie;
    std::optional<int> iz(0);
    std::optional<int> iz2(0);
    std::optional<int> i1(1);
    std::optional<int> ineg(-1);
    std::printf("iz le=%d\n", static_cast<int>(ie <= iz));
    std::printf("zi le=%d\n", static_cast<int>(iz <= ie));
    std::printf("zz le=%d\n", static_cast<int>(iz <= iz2));
    std::printf("zz rev=%d\n", static_cast<int>(iz2 <= iz));
    std::printf("01 le=%d\n", static_cast<int>(iz <= i1));
    std::printf("10 le=%d\n", static_cast<int>(i1 <= iz));
    std::printf("neg le=%d\n", static_cast<int>(ineg <= iz));
    std::printf("pos le=%d\n", static_cast<int>(iz <= ineg));
    std::printf("ii le=%d\n", static_cast<int>(ie <= ie));
    return 0;
}
