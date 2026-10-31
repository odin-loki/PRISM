// no-false-twin: this harness labels positive contracts of a C++ header model only (no planted defect).
// Names <semaphore> so the C++ model-header conformance case sees this
// include. The scored tasks are
// testdata/conformance/prism/cxx/cxx_semaphore_true.cpp and
// cxx_semaphore_false.cpp.
// try_acquire on a zero semaphore returns false. That is defined, and it is
// not the false task. release past the max template argument is that task.
#include <semaphore>

int semaphore_acquire_released_true(int c) {
    (void)c;
    std::counting_semaphore<1> s(0);
    if (s.try_acquire()) return 1;
    s.release();
    if (!s.try_acquire()) return 1;
    if (s.try_acquire()) return 1;
    return 0;
}
