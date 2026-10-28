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
// argv "exact" is sputn of two characters into a 4-byte buffer.
// argv "zero" is sputn of count 0, including a null source: it returns 0
// and does not write.
// argv "full" fills one byte with sputc, then sputn of two characters
// returns 0 and leaves that byte.
// argv "neg" is a negative sputn count. argv "null" is a null source with
// a positive count. Neither writes.
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
    if (argc > 1 && std::strcmp(argv[1], "exact") == 0) {
        char buf[4] = {};
        char src[2] = {'A', 'B'};
        std::basic_spanbuf<char> b(buf, 4);
        const int n = static_cast<int>(b.sputn(src, 2));
        std::printf("exact=%d %c%c\n", n, buf[0], buf[1]);
        return 0;
    }
    if (argc > 1 && std::strcmp(argv[1], "zero") == 0) {
        char buf[4] = {'Z', 'Z', 'Z', 'Z'};
        std::basic_spanbuf<char> b(buf, 4);
        const int n = static_cast<int>(b.sputn(nullptr, 0));
        std::printf("zero=%d %c\n", n, buf[0]);
        return 0;
    }
    if (argc > 1 && std::strcmp(argv[1], "full") == 0) {
        char buf[1] = {'Q'};
        char src[2] = {'A', 'B'};
        std::basic_spanbuf<char> b(buf, 1);
        b.sputc('Z');
        const int n = static_cast<int>(b.sputn(src, 2));
        std::printf("full=%d %c\n", n, buf[0]);
        return 0;
    }
    if (argc > 1 && std::strcmp(argv[1], "neg") == 0) {
        char buf[4] = {'Z', 'Z', 'Z', 'Z'};
        char src[1] = {'A'};
        std::basic_spanbuf<char> b(buf, 4);
        const int n = static_cast<int>(b.sputn(src, -1));
        std::printf("neg=%d %c\n", n, buf[0]);
        return 0;
    }
    if (argc > 1 && std::strcmp(argv[1], "null") == 0) {
        char buf[4] = {'Z', 'Z', 'Z', 'Z'};
        std::basic_spanbuf<char> b(buf, 4);
        const int n = static_cast<int>(b.sputn(nullptr, 3));
        std::printf("null=%d %c\n", n, buf[0]);
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
    char out[4] = {'x', 'x', 'x', 'x'};
    char src[6] = {'A', 'B', 'C', 'D', 'E', 'F'};
    std::basic_spanbuf<char> w(out, 4);
    const int n = static_cast<int>(w.sputn(src, 6));
    std::printf("n=%d clip=%c%c%c%c\n", n, out[0], out[1], out[2], out[3]);
    return 0;
}
