// Trace of std::basic_spanbuf. Includes <spanstream>. One character is written
// into the caller's buffer. After the spanbuf is destroyed that byte is still
// there: the model does not own it. A write at size is a check and is not
// this path.
#include <cstdio>
#include <cstdlib>
#include <spanstream>

// The model declares this PIR intrinsic and does not define it.
extern "C" void __prism_check(int ok, const char* cls, const char* msg) {
    if (!ok) {
        std::fprintf(stderr, "%s: %s\n", cls ? cls : "", msg ? msg : "");
        std::abort();
    }
}

int main() {
    char buf[4] = {};
    {
        std::basic_spanbuf<char> b(buf, 4);
        b.sputc('A');
    }
    std::printf("byte=%c\n", buf[0]);
    return 0;
}
