// PRISM conformance task cxx/cxx_mutex_false.cpp: expected false (memsafety)
// unlock on a mutex that was never locked (c == 0)
#include <mutex>
int cxx_mutex_false(int c) {
    std::mutex m;
    if (c) m.try_lock();
    m.unlock();
    return 0;
}
