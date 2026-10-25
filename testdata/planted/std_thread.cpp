#include <thread>

void worker_fn(void) {
}

int thread_dtor_bad(void) {
    std::thread t(worker_fn);
    return 1;
}

int thread_join_ok(void) {
    std::thread t(worker_fn);
    t.join();
    return 0;
}
