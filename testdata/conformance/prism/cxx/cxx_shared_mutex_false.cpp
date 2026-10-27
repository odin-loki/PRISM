// PRISM conformance task cxx/cxx_shared_mutex_false.cpp: expected false (memsafety)
// unlock_shared when the shared count is 0 (c == 0)
#include <shared_mutex>
int cxx_shared_mutex_false(int c) {
    std::shared_mutex m;
    if (c) return 0;
    m.unlock_shared();
    return 0;
}
