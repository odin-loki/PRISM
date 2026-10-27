// Names <syncstream> so the C++ model-header conformance case sees this include.
// The scored tasks are testdata/conformance/prism/cxx/cxx_syncstream_true.cpp
// (one character into a 4-byte caller buffer) and cxx_syncstream_false.cpp
// (sputc one past a size-0 buffer). emit and the syncstream class are not named.
// This file is not a scored task.
#include <syncstream>

int syncstream_putc_true(int n) {
#if defined(_PRISM_SYNCSTREAM_MODEL)
    (void)n;
    char buf[4] = {};
    std::basic_syncbuf b(buf, 4);
    b.sputc('A');
    return buf[0] == 'A' ? 0 : 1;
#else
    (void)n;
    return 0;
#endif
}
