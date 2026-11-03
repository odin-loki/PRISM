// std::thread model (src/prism/pir/models/cxx/thread) against C++23
// [thread.thread.class]. Threads are explored by the bounded scheduler in
// bits/prism_sched.h; this is not a proof for unbounded preemptions.
#include <cassert>
#include <thread>

// join runs the thread to completion before returning
int thread_join_true(int c) {
    if (c == 0) return 0;
    int x = 0;
    std::thread t([&] { x = 1; });
    t.join();
    assert(x == 1);
    return 0;
}

// destroying a joinable thread calls std::terminate
int thread_joinable_false(int c) {
    if (c == 0) return 0;
    std::thread t([] {});
    return 0;
}
