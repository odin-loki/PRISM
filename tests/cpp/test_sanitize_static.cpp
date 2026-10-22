// Portable sanitize_detail helpers (no fake sh compiler). Full stage tests with
// FakeCc remain in test_sanitize.cpp (#ifndef _WIN32).
#include <doctest/doctest.h>

#include "../../src/prism/sanitize_detail.hpp"

namespace sd = prism::sanitize_detail;

TEST_CASE("sanitize: sanitizer report text") {
    CHECK(sd::hit("ERROR: AddressSanitizer: heap-buffer-overflow", 1));
    CHECK(sd::hit("AddressSanitizer: heap-use-after-free", 1));
    CHECK(sd::hit("x.c:1: runtime error: shift exponent 40", 1));
    CHECK_FALSE(sd::hit("all good", 0));
    CHECK(sd::hit("", -6));  // killed by a signal
}

TEST_CASE("sanitize: an unusable TSan runtime is recognised in output text") {
    const std::string mapping = "FATAL: ThreadSanitizer: unexpected memory mapping 0x7f00";
    CHECK(sd::runtime_unusable(mapping));
    CHECK_FALSE(sd::hit(mapping, -1));
}

TEST_CASE("sanitize: MinGW is recognised by install path") {
    CHECK(sd::is_mingw_cc("C:\\mingw64\\bin\\gcc.exe"));
    CHECK(sd::is_mingw_cc("C:\\msys64\\mingw64\\bin\\gcc.exe"));
}
