// PRISM conformance task cxx/cxx_initializer_list_true.cpp: expected true (memsafety)
// Walks size() elements of the compiler-built array. Does not read end().
#include <initializer_list>
int cxx_initializer_list_true(int c) {
    if (c < 0) return 0;
    std::initializer_list<int> xs{1, 2, 3, 4};
    int s = 0;
    auto it = xs.begin();
    for (decltype(xs.size()) i = 0; i < xs.size(); ++i) {
        s += *it;
        ++it;
    }
    return s;
}
