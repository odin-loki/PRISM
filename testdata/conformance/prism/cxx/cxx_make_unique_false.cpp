// PRISM conformance task cxx/cxx_make_unique_false.cpp: expected false (memsafety)
// make_unique<int[]>(SIZE_MAX) does not fit in the byte count. A non-zero
// input takes that path. The model checks MEM-OOB-WRITE and traps before
// the allocation. It does not allocate a wrapped count and does not return
// a[0] of one.
#include <cstdint>
#include <memory>
int cxx_make_unique_false(int c) {
    if (c == 0) return 0;
    auto a = std::make_unique<int[]>(SIZE_MAX);
    return a[0];
}
