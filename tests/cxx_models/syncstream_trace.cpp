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
//
// argv "back" is three in-range sgetc calls, then sungetc back over 'A',
// 255, and 0. argv "before" is sungetc when the offset is still 0.
// argv "unnull0" is sungetc on a null pointer whose size is 0.
// argv "step" is one sputc, then sungetc of that byte, then sungetc at 0.
// argv "unforced" keeps going after the constructor check so a null pointer
// with a non-zero size (size forced to 0) reaches sungetc and does not read.
// argv "stay" is sungetc at offset 0, then sputc: the offset is not
// decremented, so the write still lands in the caller buffer.
//
// argv "put" stores three characters and leaves the fourth byte untouched.
// The bytes are still in the caller buffer after the destructor. argv
// "zero" is a count of 0 on a null source: it returns 0 and does not read.
// argv "room" asks for 5 characters from a 2-byte source into a 2-byte
// buffer and stores 2. argv "noroom" is sputn after the offset is already
// the size, on a null source. argv "emptyn" is a positive count into a
// size-0 buffer, on a null source. argv "rest" is sputc of one byte, then
// sputn of the three that remain. argv "neg" is a negative count. argv
// "neggo" is that count after the check returns, so __builtin_trap runs
// and the buffer is still not written. argv
// "after" fills the buffer with sputn, then sputc: that write is a check,
// so the full count was not a flush that made room. argv "putforced"
// keeps going after the constructor check so a null pointer with a
// non-zero size (size forced to 0) reaches sputn and does not write.
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
    if (argc > 1 && std::strcmp(argv[1], "back") == 0) {
        char buf[3] = {0, static_cast<char>(static_cast<unsigned char>(255)), 'A'};
        std::basic_syncbuf b(buf, 3);
        b.sgetc();
        b.sgetc();
        b.sgetc();
        std::printf("back %d\n", b.sungetc());
        std::printf("back %d\n", b.sungetc());
        std::printf("back %d\n", b.sungetc());
        return 0;
    }
    if (argc > 1 && std::strcmp(argv[1], "before") == 0) {
        char buf[1] = {'Z'};
        std::basic_syncbuf b(buf, 1);
        const int g = b.sungetc();
        std::printf("before %d\n", g);
        return 0;
    }
    if (argc > 1 && std::strcmp(argv[1], "unnull0") == 0) {
        std::basic_syncbuf b(nullptr, 0);
        const int g = b.sungetc();
        std::printf("unread %d\n", g);
        return 0;
    }
    if (argc > 1 && std::strcmp(argv[1], "step") == 0) {
        char buf[1] = {};
        std::basic_syncbuf b(buf, 1);
        b.sputc('A');
        const int g = b.sungetc();
        std::printf("step %d\n", g);
        std::fflush(stdout);
        const int h = b.sungetc();
        std::printf("under %d\n", h);
        return 0;
    }
    if (argc > 1 && std::strcmp(argv[1], "unforced") == 0) {
        keep_going = 1;
        std::basic_syncbuf b(nullptr, 1);
        const int g = b.sungetc();
        std::printf("unforced %d\n", g);
        return 0;
    }
    if (argc > 1 && std::strcmp(argv[1], "stay") == 0) {
        keep_going = 1;
        char buf[1] = {};
        std::basic_syncbuf b(buf, 1);
        const int g = b.sungetc();
        b.sputc('A');
        std::printf("stayed %d %d\n", g, static_cast<int>(static_cast<unsigned char>(buf[0])));
        return 0;
    }
    if (argc > 1 && std::strcmp(argv[1], "put") == 0) {
        char buf[4] = {};
        {
            std::basic_syncbuf b(buf, 4);
            const char src[3] = {'A', 'b', 'C'};
            const auto n = b.sputn(src, 3);
            std::printf("put %d\n", static_cast<int>(n));
        }
        std::printf("keptn %d %d %d %d\n", static_cast<int>(static_cast<unsigned char>(buf[0])),
                    static_cast<int>(static_cast<unsigned char>(buf[1])),
                    static_cast<int>(static_cast<unsigned char>(buf[2])),
                    static_cast<int>(static_cast<unsigned char>(buf[3])));
        return 0;
    }
    if (argc > 1 && std::strcmp(argv[1], "zero") == 0) {
        char buf[1] = {'Z'};
        std::basic_syncbuf b(buf, 1);
        const auto n = b.sputn(nullptr, 0);
        std::printf("zero %d %d\n", static_cast<int>(n), static_cast<int>(static_cast<unsigned char>(buf[0])));
        return 0;
    }
    if (argc > 1 && std::strcmp(argv[1], "room") == 0) {
        char buf[2] = {};
        char src[2] = {'A', 'B'};
        std::basic_syncbuf b(buf, 2);
        const auto n = b.sputn(src, 5);
        std::printf("room %d %d %d\n", static_cast<int>(n), static_cast<int>(static_cast<unsigned char>(buf[0])),
                    static_cast<int>(static_cast<unsigned char>(buf[1])));
        return 0;
    }
    if (argc > 1 && std::strcmp(argv[1], "noroom") == 0) {
        char buf[1] = {};
        std::basic_syncbuf b(buf, 1);
        b.sputc('A');
        const auto n = b.sputn(nullptr, 4);
        std::printf("noroom %d %d\n", static_cast<int>(n), static_cast<int>(static_cast<unsigned char>(buf[0])));
        return 0;
    }
    if (argc > 1 && std::strcmp(argv[1], "emptyn") == 0) {
        char buf[1] = {'Z'};
        std::basic_syncbuf b(buf, 0);
        const auto n = b.sputn(nullptr, 2);
        std::printf("emptyn %d %d\n", static_cast<int>(n), static_cast<int>(static_cast<unsigned char>(buf[0])));
        return 0;
    }
    if (argc > 1 && std::strcmp(argv[1], "rest") == 0) {
        char buf[4] = {};
        const char src[3] = {'B', 'C', 'D'};
        std::basic_syncbuf b(buf, 4);
        b.sputc('A');
        const auto n = b.sputn(src, 3);
        std::printf("rest %d %d %d %d %d\n", static_cast<int>(n),
                    static_cast<int>(static_cast<unsigned char>(buf[0])),
                    static_cast<int>(static_cast<unsigned char>(buf[1])),
                    static_cast<int>(static_cast<unsigned char>(buf[2])),
                    static_cast<int>(static_cast<unsigned char>(buf[3])));
        return 0;
    }
    if (argc > 1 && std::strcmp(argv[1], "neg") == 0) {
        char buf[1] = {'Z'};
        const char src[1] = {'A'};
        std::basic_syncbuf b(buf, 1);
        b.sputn(src, -1);
        std::printf("stored %d\n", static_cast<int>(static_cast<unsigned char>(buf[0])));
        return 0;
    }
    if (argc > 1 && std::strcmp(argv[1], "neggo") == 0) {
        keep_going = 1;
        char buf[1] = {'Z'};
        const char src[1] = {'A'};
        std::basic_syncbuf b(buf, 1);
        b.sputn(src, -1);
        std::printf("stored %d\n", static_cast<int>(static_cast<unsigned char>(buf[0])));
        return 0;
    }
    if (argc > 1 && std::strcmp(argv[1], "after") == 0) {
        char buf[2] = {};
        const char src[2] = {'A', 'B'};
        std::basic_syncbuf b(buf, 2);
        const auto n = b.sputn(src, 2);
        std::printf("filled %d\n", static_cast<int>(n));
        std::fflush(stdout);
        b.sputc('C');
        std::printf("extra %d\n", static_cast<int>(static_cast<unsigned char>(buf[0])));
        return 0;
    }
    if (argc > 1 && std::strcmp(argv[1], "putforced") == 0) {
        keep_going = 1;
        const char src[1] = {'A'};
        std::basic_syncbuf b(nullptr, 1);
        const auto n = b.sputn(src, 1);
        std::printf("putforced %d\n", static_cast<int>(n));
        return 0;
    }
    show();
    return 0;
}
