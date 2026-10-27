// std::pmr::monotonic_buffer_resource over a caller buffer
// (src/prism/pir/models/cxx/memory_resource). The pir stage puts the model
// headers first on the include path, so <memory_resource> below is the model.
// polymorphic_allocator and the pool resources are not part of the model.
#include <memory_resource>

// 4 bytes from an 8-byte caller buffer. The pointer is inside that buffer.
int mbr_inside_true(int c) {
    unsigned char buf[8] = {};
    std::pmr::monotonic_buffer_resource mr(buf, 8);
    auto* p = static_cast<unsigned char*>(mr.allocate(4, 1));
    p[0] = static_cast<unsigned char>(c);
    p[3] = 1;
    return p[0] + p[3];
}

// deallocate does not free the caller buffer, and the destructor does not either.
int mbr_dtor_keeps_true(int c) {
    unsigned char buf[8] = {};
    {
        std::pmr::monotonic_buffer_resource mr(buf, 8);
        auto* p = static_cast<unsigned char*>(mr.allocate(4, 1));
        p[0] = static_cast<unsigned char>(c);
        mr.deallocate(p, 4, 1);
    }
    return buf[0];
}

// More bytes than the caller buffer holds.
int mbr_oversize_false(int c) {
    (void)c;
    unsigned char buf[8] = {};
    std::pmr::monotonic_buffer_resource mr(buf, 8);
    void* p = mr.allocate(9, 1);
    (void)p;
    return 0;
}

// A null upstream buffer with a non-zero size.
int mbr_null_size_false(int c) {
    (void)c;
    std::pmr::monotonic_buffer_resource mr(nullptr, 4);
    (void)mr;
    return 0;
}

// deallocate of a pointer this resource did not hand out.
int mbr_dealloc_foreign_false(int c) {
    (void)c;
    unsigned char buf[8] = {};
    unsigned char other = 0;
    std::pmr::monotonic_buffer_resource mr(buf, 8);
    (void)mr.allocate(4, 1);
    mr.deallocate(&other, 1, 1);
    return 0;
}
