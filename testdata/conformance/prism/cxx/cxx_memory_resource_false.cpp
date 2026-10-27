// PRISM conformance task cxx/cxx_memory_resource_false.cpp: expected false (no-oob)
// allocate more bytes than the caller buffer holds
#include <memory_resource>
int cxx_memory_resource_false(int c) {
    (void)c;
    unsigned char buf[8] = {};
    std::pmr::monotonic_buffer_resource mr(buf, 8);
    void* p = mr.allocate(9, 1);
    (void)p;
    return 0;
}
