// Trace of std::basic_spanbuf. Includes <spanstream>. One character is written
// into the caller's buffer. After the spanbuf is destroyed that byte is still
// there: the model does not own it. A write at size is a check and is not
// this path. A separate spanbuf reads the first byte and advances, then
// sungetc steps back onto that byte. A read at size, including size 0, is a
// check and is not this path. sungetc at offset 0 is a check and does not
// read before the buffer.
//
// argv "off0" is sungetc when the offset is still 0 on a 4-byte buffer.
// argv "empty" is sungetc when the size is 0.
// argv "null0" is sungetc on a null pointer whose size is 0.
// argv "hi" is sgetc of 255, then sungetc of that same byte.
// argv "twice" is one in-range sungetc, then a second once the offset is 0.
// argv "forced" keeps going after the constructor check so a null pointer
// with a non-zero size (the size is kept) reaches sungetc at offset 0 and
// does not read.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <spanstream>

// A failing check aborts unless keep_going is set. A passing check returns.
static int keep_going = 0;

// The model declares this PIR intrinsic and does not define it.
extern "C" void __prism_check(int ok, const char* cls, const char* msg) {
    if (!ok) {
        std::fprintf(stderr, "CHECK %s: %s\n", cls ? cls : "", msg ? msg : "");
        std::fflush(stderr);
        if (!keep_going) std::abort();
    }
}

int main(int argc, char** argv) {
    if (argc > 1 && std::strcmp(argv[1], "off0") == 0) {
        char buf[4] = {'A', 'B', 'C', 'D'};
        std::basic_spanbuf<char> b(buf, 4);
        const int g = b.sungetc();
        std::printf("back=%d\n", g);
        return 0;
    }
    if (argc > 1 && std::strcmp(argv[1], "empty") == 0) {
        char buf[4] = {'A', 'B', 'C', 'D'};
        std::basic_spanbuf<char> b(buf, 0);
        const int g = b.sungetc();
        std::printf("empty=%d\n", g);
        return 0;
    }
    if (argc > 1 && std::strcmp(argv[1], "null0") == 0) {
        std::basic_spanbuf<char> b(nullptr, 0);
        const int g = b.sungetc();
        std::printf("read=%d\n", g);
        return 0;
    }
    if (argc > 1 && std::strcmp(argv[1], "hi") == 0) {
        char buf[1] = {static_cast<char>(static_cast<unsigned char>(255))};
        std::basic_spanbuf<char> b(buf, 1);
        (void)b.sgetc();
        std::printf("hi=%d\n", b.sungetc());
        return 0;
    }
    if (argc > 1 && std::strcmp(argv[1], "twice") == 0) {
        char buf[1] = {'A'};
        std::basic_spanbuf<char> b(buf, 1);
        (void)b.sgetc();
        std::printf("once=%d\n", b.sungetc());
        std::fflush(stdout);
        const int g = b.sungetc();
        std::printf("again=%d\n", g);
        return 0;
    }
    if (argc > 1 && std::strcmp(argv[1], "forced") == 0) {
        keep_going = 1;
        std::basic_spanbuf<char> b(nullptr, 1);
        const int g = b.sungetc();
        std::printf("forced=%d size=%zu\n", g, b.size());
        return 0;
    }
    char buf[4] = {};
    {
        std::basic_spanbuf<char> b(buf, 4);
        b.sputc('A');
    }
    std::printf("byte=%c\n", buf[0]);
    char in[1] = {'B'};
    std::basic_spanbuf<char> r(in, 1);
    std::printf("got=%d\n", r.sgetc());
    std::printf("back=%d\n", r.sungetc());
    return 0;
}
