// no-false-twin: this harness labels positive contracts of a C++ header model only (no planted defect).
// Names <condition_variable> so the C++ model-header conformance case sees
// this include. notify_one and notify_all set the model's flag. They do
// not run a waiter. A default condition_variable has the flag false.
// wait, wait_for, and wait_until are not called. There is no false twin:
// this header has no failing operation, and a woken thread is not invented.
// No sidecar: a libc-models task must list both a true and a false function.
#include <condition_variable>

int condition_variable_notify_true(int) {
    std::condition_variable cv;
    cv.notify_one();
    cv.notify_all();
    return 0;
}
