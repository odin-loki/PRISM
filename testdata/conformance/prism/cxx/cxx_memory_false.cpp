// PRISM conformance task cxx/cxx_memory_false.cpp: expected false (memsafety)
// operator* on an empty unique_ptr (c == 0 leaves it empty)
#include <memory>
int cxx_memory_false(int c) {
    std::unique_ptr<int> p;
    if (c) p.reset(new int(c));
    return *p;
}
