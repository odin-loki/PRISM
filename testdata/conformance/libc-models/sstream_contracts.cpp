// std::ostringstream model (<sstream> in src/prism/pir/models/cxx/sstream).
#include <cassert>
#include <cstring>
#include <sstream>
#include <string>

int sstream_int_true(int x) {
    std::ostringstream os;
    os << x;
    assert(os.str() == std::to_string(x));
    return 0;
}

int sstream_chain_true(int a, int b) {
    std::ostringstream os;
    os << a << ':' << b;
    std::string want = std::to_string(a) + ':' + std::to_string(b);
    assert(os.str() == want);
    return 0;
}

int sstream_strbuf_true(int) {
    // a terminated string (an unguarded pointer parameter is a missing
    // precondition, NEEDS-HARNESS, not a defect)
    const char p[] = "abc";
    std::stringbuf buf;
    buf.sputn(p, static_cast<std::streamsize>(std::strlen(p)));
    assert(buf.str() == p);
    return 0;
}

int sstream_empty_false(int x) {
    std::ostringstream os;
    os << x;
    assert(os.str().empty());
    return 0;
}
