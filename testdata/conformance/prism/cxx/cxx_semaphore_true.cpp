// PRISM conformance task cxx/cxx_semaphore_true.cpp: expected true (memsafety)
// Empty try_acquire is false. release adds one token and try_acquire takes it.
// A second try_acquire on the empty semaphore is false. That result is defined.
#include <semaphore>
int cxx_semaphore_true(int c) {
    (void)c;
    std::counting_semaphore<1> s(0);
    if (s.try_acquire()) return 1;
    s.release();
    if (!s.try_acquire()) return 1;
    if (s.try_acquire()) return 1;
    return 0;
}
