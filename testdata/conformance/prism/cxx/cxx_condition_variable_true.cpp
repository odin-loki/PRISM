// PRISM conformance task cxx/cxx_condition_variable_true.cpp: expected true
// (memsafety). notify_one and notify_all set the flag in the object. There
// is no heap and no thread. wait is not called. A blocking wait that
// returned immediately would be a fake wakeup.
#include <condition_variable>
int cxx_condition_variable_true(int) {
    std::condition_variable cv;
    cv.notify_one();
    cv.notify_all();
    return 1;
}
