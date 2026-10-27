// Differential trace of the partial <syncstream> model. Compiled with
// PRISM's model header (src/prism/pir/models/cxx/syncstream) first on the
// include path. emit and the syncstream class are not called: this header
// does not declare them.
//
// argv "full" is sputc on a size-0 buffer (the offset is already the size).
// argv "wrap" is a second sputc once the offset has reached the size.
// argv "null" is a null pointer with a non-zero size. Each must
// __prism_check and must not print a stored byte.
#include <syncstream>
#include <cstdio>
#include <cstdlib>
#include <cstring>

// Native stand-in for the PIR intrinsic. A failing check aborts before the
// caller can print. A passing check returns.
extern "C" void __prism_check(int ok, const char* cls, const char* msg) {
    if (!ok) {
        std::fprintf(stderr, "CHECK %s: %s\n", cls, msg);
        std::fflush(stderr);
        std::abort();
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
    show();
    return 0;
}
