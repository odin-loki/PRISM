// PRISM conformance task cxx/cxx_syncstream_false.cpp: expected false (no-oob)
// sputc one past a size-0 buffer. The model checks and does not store.
// libstdc++ has no caller-buffer constructor; that side returns without a write.
#include <syncstream>
int cxx_syncstream_false(int n) {
    if (n != 0) return 0;
#if defined(_PRISM_SYNCSTREAM_MODEL)
    char buf[4] = {};
    std::basic_syncbuf b(buf, 0);
    b.sputc('A');
    return buf[0];
#else
    return 0;
#endif
}
