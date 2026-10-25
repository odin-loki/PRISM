// PRISM conformance task cxx/cxx_unique_ptr_false.cpp: expected false (no-null-deref)
// std::unique_ptr dereferenced while null
#include <memory>
int cxx_unique_ptr_false(int c) {
    std::unique_ptr<int> p(c ? new int(c & 7) : nullptr);
    return *p;
}
