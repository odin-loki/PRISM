// PRISM conformance task cxx/cxx_syncstream_true.cpp: expected true (no-oob)
// One sputc into a 4-byte caller buffer, then that byte is read back.
// libstdc++ has no caller-buffer constructor, so the label self-check
// (that header) does not invent a write.
#include <syncstream>
int cxx_syncstream_true(int n) {
#if defined(_PRISM_SYNCSTREAM_MODEL)
    (void)n;
    char buf[4] = {};
    std::basic_syncbuf b(buf, 4);
    b.sputc('A');
    return buf[0];
#else
    (void)n;
    return 0;
#endif
}
