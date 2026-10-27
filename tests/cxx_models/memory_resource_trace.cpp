// Trace of std::pmr::monotonic_buffer_resource over a caller buffer.
// Include <memory_resource> and use only the caller-buffer constructor,
// allocate, and deallocate. Compiled against libstdc++ and against
// src/prism/pir/models/cxx (that directory first on the include path), the
// lines should match: 4 bytes then 4 bytes from an 8-byte buffer, both
// pointers inside it, and the buffer still there after the resource dies.
// An allocate larger than the buffer is not in this trace. libstdc++ takes
// that from the upstream resource; the model checks instead.
// tests/cpp/test_cxx_memory_resource.cpp checks the model text; it does not
// shell out to this file.
#include <cstdio>
#include <cstdlib>
#include <memory_resource>

// The model declares this PIR intrinsic and does not define it. libstdc++
// <memory_resource> never calls it, so a successful run prints nothing from here.
extern "C" void __prism_check(int ok, const char* cls, const char* msg) {
    if (!ok) {
        std::fprintf(stderr, "%s: %s\n", cls, msg);
        std::abort();
    }
}

int main() {
    unsigned char buf[8] = {};
    int off0 = -1;
    int off1 = -1;
    int kept = -1;
    {
        std::pmr::monotonic_buffer_resource mr(buf, 8);
        auto* p = static_cast<unsigned char*>(mr.allocate(4, 1));
        p[0] = 7;
        auto* q = static_cast<unsigned char*>(mr.allocate(4, 1));
        q[0] = 8;
        off0 = static_cast<int>(p - buf);
        off1 = static_cast<int>(q - buf);
        const int inside0 = (p >= buf && p + 4 <= buf + 8) ? 1 : 0;
        const int inside1 = (q >= buf && q + 4 <= buf + 8) ? 1 : 0;
        std::printf("inside %d %d\n", inside0, inside1);
        mr.deallocate(p, 4, 1);
        kept = static_cast<int>(buf[0]);
    }
    std::printf("off %d %d\n", off0, off1);
    std::printf("kept %d\n", kept);
    std::printf("alive %d\n", static_cast<int>(buf[0]));
    return 0;
}
