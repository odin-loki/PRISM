// PRISM conformance task cxx/cxx_shared_mutex_true.cpp: expected true (memsafety)
// try_lock_shared once; a second try_lock (exclusive) is false; then unlock_shared
#include <shared_mutex>
int cxx_shared_mutex_true(int c) {
    std::shared_mutex m;
    if (!m.try_lock_shared()) return 1;
    if (m.try_lock()) return 1;
    m.unlock_shared();
    return c;
}
