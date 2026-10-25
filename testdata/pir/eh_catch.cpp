// PIR tasks: C++ exceptions as explicit edges (docs/PIR.md "Exceptions").
// invoke/landingpad/resume become jumps to the handler that catches the
// thrown type; an exception reaching a noexcept boundary is
// CXX-THROW-NOEXCEPT, one leaving main is CXX-UNCAUGHT (eh_uncaught.cpp).
#include <cassert>
#include <stdexcept>
#include <vector>

struct E {
    int code;
};
struct D : E {
    int extra;
};

static int thrower(int x) {
    if (x > 10) throw E{x};
    return x;
}

static int thrower_d(int x) {
    if (x > 10) throw D{{x}, 1};
    return x;
}

int eh_catch_ok(int x) {
    try {
        return thrower(x);
    } catch (const E&) {
        return -1;
    }
}

int eh_catch_bad(int x) {
    try {
        return thrower(x);
    } catch (const E& e) {
        return 100 / (e.code - 11);  // x == 11: division by zero in the handler
    }
}

int eh_noexcept_bad(int x) noexcept { return thrower(x); }

int eh_noexcept_ok(int x) noexcept {
    if (x > 10) return 0;
    return thrower(x);
}

int eh_wrong_type_bad(int x) noexcept {
    try {
        return thrower(x);
    } catch (int) {  // E is not an int: the exception reaches the noexcept boundary
        return 0;
    }
}

int eh_base_ok(int x) noexcept {
    try {
        return thrower_d(x);
    } catch (const E& e) {  // D derives from E: caught
        return e.code;
    }
}

struct Guard {
    int* p;
    ~Guard() { *p += 1; }
};

int eh_cleanup_ok(int x) {
    int n = 0;
    try {
        Guard g{&n};
        thrower(x);
    } catch (...) {
        assert(n == 1);  // the destructor ran during unwinding
        return 1;
    }
    assert(n == 1);
    return 0;
}

int eh_cleanup_bad(int x) {
    int n = 0;
    try {
        Guard g{&n};
        thrower(x);
    } catch (...) {
        assert(n == 0);  // wrong: the destructor already ran
        return 1;
    }
    return 0;
}

int eh_rethrow_ok(int x) {
    try {
        try {
            return thrower(x);
        } catch (const E&) {
            throw;  // rethrown to the outer handler
        }
    } catch (const E& e) {
        return e.code > 10 ? 1 : 0;
    }
}

int eh_std_ok(int i) {
    std::vector<int> v{1, 2, 3};
    try {
        return v.at(static_cast<unsigned>(i) & 7u);
    } catch (const std::out_of_range&) {
        return -1;
    }
}

int eh_std_bad(int i) noexcept {
    std::vector<int> v{1, 2, 3};
    return v.at(static_cast<unsigned>(i) & 3u);  // at(3) throws out of a noexcept function
}
