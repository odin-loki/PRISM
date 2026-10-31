// PRISM conformance task cxx/cxx_chrono_time_point_false.cpp: expected false (no-overflow)
#include <chrono>
long long cxx_chrono_time_point_false() {
    auto t = std::chrono::system_clock::now();
    return (t + std::chrono::seconds(1)).time_since_epoch().count();  // now() may be max()
}
