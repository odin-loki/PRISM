// PRISM conformance task cxx/cxx_memory_true.cpp: expected true (memsafety)
// unique_ptr owns one object, the function reads it, the destructor frees it
#include <memory>
int cxx_memory_true(int c) {
    std::unique_ptr<int> p(new int(c));
    return *p;
}
