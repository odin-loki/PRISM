// PRISM conformance task cxx/cxx_mutex_true.cpp: expected true (memsafety)
// try_lock once; the second try_lock is false. Then unlock the one acquisition.
#include <mutex>
int cxx_mutex_true(int) {
    std::mutex m;
    if (!m.try_lock()) return 1;
    const bool second = m.try_lock();
    m.unlock();
    return second ? 1 : 0;
}
