// PRISM conformance task cxx/cxx_memory_resource_true.cpp: expected true (no-oob)
// allocate 4 bytes from an 8-byte caller buffer; the pointer is inside that buffer
#include <memory_resource>
int cxx_memory_resource_true(int c) {
    unsigned char buf[8] = {};
    std::pmr::monotonic_buffer_resource mr(buf, 8);
    auto* p = static_cast<unsigned char*>(mr.allocate(4, 1));
    p[0] = static_cast<unsigned char>(c);
    p[3] = 1;
    return p[0] + p[3];
}
