// PRISM conformance task cxx/cxx_semaphore_false.cpp: expected false (memsafety)
// release past the max template argument. try_acquire on a zero semaphore
// returns false and is not this task.
#include <semaphore>
int cxx_semaphore_false(int n) {
    std::counting_semaphore<1> s(1);
    if (n <= 0) return 0;
    s.release(n);
    return 0;
}
