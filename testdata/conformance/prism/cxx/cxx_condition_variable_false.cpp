// PRISM conformance task cxx/cxx_condition_variable_false.cpp: expected false (memsafety)
// the native handle of a deleted condition variable is read (use after free)
#include <condition_variable>
int cxx_condition_variable_false(int n) {
    std::condition_variable* cv = new std::condition_variable;
    cv->notify_all();
    delete cv;
    if (n != 1) return 0;
    return *reinterpret_cast<const unsigned char*>(cv->native_handle());
}
