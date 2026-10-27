// PRISM conformance task cxx/cxx_future_true.cpp: expected true (memsafety)
// promise::set_value stores one int; the associated future reads it once
#include <future>
int cxx_future_true(int c) {
    std::promise<int> p;
    std::future<int> f = p.get_future();
    p.set_value(c);
    return f.get();
}
