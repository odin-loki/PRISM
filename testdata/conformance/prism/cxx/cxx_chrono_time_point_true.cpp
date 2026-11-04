// PRISM conformance task cxx/cxx_chrono_time_point_true.cpp: expected true (noassert)
#include <cassert>
#include <chrono>
int cxx_chrono_time_point_true() {
    auto a = std::chrono::steady_clock::now();
    auto b = std::chrono::steady_clock::now();
    assert(b >= a);  // steady_clock is monotonic
    return 0;
}
