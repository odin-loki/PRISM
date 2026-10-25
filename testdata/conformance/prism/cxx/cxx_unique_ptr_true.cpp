// PRISM conformance task cxx/cxx_unique_ptr_true.cpp: expected true (no-null-deref)
// std::unique_ptr dereferenced while null (guarded)
#include <memory>
int cxx_unique_ptr_true(int c) {
    std::unique_ptr<int> p(c ? new int(c & 7) : nullptr);
    return p ? *p : 0;
}
