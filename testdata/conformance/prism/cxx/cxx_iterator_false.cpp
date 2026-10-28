// PRISM conformance task cxx/cxx_iterator_false.cpp: expected false (no-overflow).
// distance of two char* whose element count does not fit in ptrdiff_t.
// A non-zero input takes that path. The model checks FUNC-CONTRACT and does
// not return the wrapped value. This is not a proof that the two pointers
// address the same array.
#include <cstdint>
#include <iterator>
int cxx_iterator_false(int c) {
    if (c == 0) return 0;
    volatile std::uintptr_t lo = 0;
    volatile std::uintptr_t hi = static_cast<std::uintptr_t>(PTRDIFF_MAX) + static_cast<std::uintptr_t>(1);
    char* first = reinterpret_cast<char*>(static_cast<std::uintptr_t>(lo));
    char* last = reinterpret_cast<char*>(static_cast<std::uintptr_t>(hi));
    return static_cast<int>(std::distance(first, last));
}
