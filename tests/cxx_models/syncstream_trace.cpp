// Differential trace of the partial <syncstream> model. Compiled with
// PRISM's model header (src/prism/pir/models/cxx/syncstream) first on the
// include path. emit and the syncstream class are not called: this header
// does not declare them.
//
// argv "full" is sputc on a size-0 buffer (the offset is already the size).
// argv "wrap" is a second sputc once the offset has reached the size.
// argv "null" is a null pointer with a non-zero size. Each must
// __prism_check and must not print a stored byte.
//
// argv "get" is three in-range sgetc calls: 0, then 255, then 'A'.
// argv "empty" is sgetc when the size is already 0.
// argv "null0" is sgetc on a null pointer whose size is 0.
// argv "past" is one in-range sgetc, then a second once the offset is the size.
// argv "forced" keeps going after the constructor check so a null pointer
// with a non-zero size (size forced to 0) reaches sgetc and does not read.
#include <syncstream>
#include <cstdio>
#include <cstdlib>
#include <cstring>

// A failing check aborts unless keep_going is set. A passing check returns.
static int keep_going = 0;

extern "C" void __prism_check(int ok, const char* cls, const char* msg) {
    if (!ok) {
        std::fprintf(stderr, "CHECK %s: %s\n", cls, msg);
        std::fflush(stderr);
        if (!keep_going) std::abort();
    }
}

static void show() {
    char buf[4] = {};
    {
        std::basic_syncbuf b(buf, 4);
        b.sputc('A');
        std::printf("byte %d\n", static_cast<int>(static_cast<unsigned char>(buf[0])));
    }
    std::printf("kept %d\n", static_cast<int>(static_cast<unsigned char>(buf[0])));
}

int main(int argc, char** argv) {
    if (argc > 1 && std::strcmp(argv[1], "full") == 0) {
        char buf[4] = {};
        std::basic_syncbuf b(buf, 0);
        b.sputc('A');
        std::printf("wrote %d\n", static_cast<int>(static_cast<unsigned char>(buf[0])));
        return 0;
    }
    if (argc > 1 && std::strcmp(argv[1], "wrap") == 0) {
        char buf[1] = {};
        std::basic_syncbuf b(buf, 1);
        b.sputc('A');
        b.sputc('B');
        std::printf("wrapped %d\n", static_cast<int>(static_cast<unsigned char>(buf[0])));
        return 0;
    }
    if (argc > 1 && std::strcmp(argv[1], "null") == 0) {
        std::basic_syncbuf b(nullptr, 1);
        std::printf("constructed\n");
        return 0;
    }
    if (argc > 1 && std::strcmp(argv[1], "get") == 0) {
        char buf[3] = {0, static_cast<char>(static_cast<unsigned char>(255)), 'A'};
        std::basic_syncbuf b(buf, 3);
        std::printf("got %d\n", b.sgetc());
        std::printf("got %d\n", b.sgetc());
        std::printf("got %d\n", b.sgetc());
        return 0;
    }
    if (argc > 1 && std::strcmp(argv[1], "empty") == 0) {
        char buf[1] = {'Z'};
        std::basic_syncbuf b(buf, 0);
        const int g = b.sgetc();
        std::printf("got %d\n", g);
        return 0;
    }
    if (argc > 1 && std::strcmp(argv[1], "null0") == 0) {
        std::basic_syncbuf b(nullptr, 0);
        const int g = b.sgetc();
        std::printf("read %d\n", g);
        return 0;
    }
    if (argc > 1 && std::strcmp(argv[1], "past") == 0) {
        char buf[1] = {'A'};
        std::basic_syncbuf b(buf, 1);
        const int first = b.sgetc();
        std::printf("once %d\n", first);
        std::fflush(stdout);
        const int g = b.sgetc();
        std::printf("past %d\n", g);
        return 0;
    }
    if (argc > 1 && std::strcmp(argv[1], "forced") == 0) {
        keep_going = 1;
        std::basic_syncbuf b(nullptr, 1);
        const int g = b.sgetc();
        std::printf("forced %d\n", g);
        return 0;
    }
    show();
    return 0;
}
