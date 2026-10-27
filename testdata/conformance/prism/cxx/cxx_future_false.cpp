// PRISM conformance task cxx/cxx_future_false.cpp: expected false (memsafety)
// std::future::get() on a default future (no shared state)
#include <future>
int cxx_future_false(int c) {
    std::future<int> f;
    if (c) return c;
    return f.get();
}
