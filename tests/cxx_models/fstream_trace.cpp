// Trace of std::ifstream. `#include <fstream>` is PRISM's model when its
// directory is first on the include path, and libstdc++ otherwise.
//
// A default stream is not open. argv "read", "get", and "getref" call read
// or get on that stream. The model must __prism_check and must not print a
// byte or a character. The default path prints the open flag.
#include <fstream>
#include <cstdio>
#include <cstdlib>
#include <cstring>

// The model declares this PIR intrinsic and does not define it. A failing
// check aborts before the caller can print. A passing check returns.
extern "C" void __prism_check(int ok, const char* cls, const char* msg) {
    if (!ok) {
        std::fprintf(stderr, "CHECK %s: %s\n", cls ? cls : "", msg ? msg : "");
        std::fflush(stderr);
        std::abort();
    }
}

int main(int argc, char** argv) {
    std::ifstream in;
    if (argc > 1 && std::strcmp(argv[1], "read") == 0) {
        char buf[4] = {'w', 'x', 'y', 'z'};
        in.read(buf, 4);
        std::printf("byte %c%c%c%c\n", buf[0], buf[1], buf[2], buf[3]);
        return 0;
    }
    if (argc > 1 && std::strcmp(argv[1], "get") == 0) {
        const int c = in.get();
        std::printf("got %d\n", c);
        return 0;
    }
    if (argc > 1 && std::strcmp(argv[1], "getref") == 0) {
        char c = 'Q';
        in.get(c);
        std::printf("ref %c\n", c);
        return 0;
    }
    const int open = in.is_open() ? 1 : 0;
    std::printf("open %d\n", open);
    return open;
}
