// Trace of std::condition_variable. Includes <condition_variable>.
// A default object is constructed, then notify_one and notify_all.
// Those calls set the model's flag. They do not run a waiter.
// wait, wait_for, and wait_until are not called: the model does not
// declare them. A blocking wait that returned immediately would be a
// fake wakeup.
// tests/cpp/test_cxx_condition_variable.cpp checks the model text; it
// does not shell out to this file.
//
// The model declares __prism_check and does not call it, so this trace
// does not define a body. There is no failing operation.
#include <condition_variable>
#include <cstdio>

int main() {
    std::condition_variable cv;
    cv.notify_one();
    cv.notify_all();
    std::printf("notified\n");
    return 0;
}
