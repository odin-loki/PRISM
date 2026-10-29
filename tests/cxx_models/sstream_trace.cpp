// Differential trace of std::ostringstream (tests/cpp/test_cxx_models.cpp):
// compiled once against libstdc++ and once with PRISM's model headers
// (src/prism/pir/models/cxx) first on the include path, both under
// ASan/UBSan with _GLIBCXX_ASSERTIONS; the two traces must be identical.
#include <cstdio>
#include <sstream>
#include <string>

static void show(const char* what, const std::string& s) {
    std::printf("%s: [%s] size=%zu\n", what, s.c_str(), s.size());
}

int main() {
    std::ostringstream empty;
    show("default", empty.str());

    std::ostringstream o;
    o << 0;
    show("int0", o.str());
    o << -1 << 42;
    show("int-chain", o.str());

    o.str("");
    show("cleared", o.str());
    o << "hello";
    show("cstr", o.str());
    o << " " << "world";
    show("cstr-chain", o.str());

    std::string s = "foo";
    o.str("");
    o << s << "bar";
    show("string", o.str());

    std::basic_stringbuf<char> buf(std::ios_base::out);
    buf.str("buf-set");
    show("stringbuf-str", buf.str());
    std::ostringstream via_buf;
    via_buf.rdbuf()->str("via-rdbuf");
    show("rdbuf-str", via_buf.str());

    std::ostringstream o2;
    o2 << 100 << " items";
    show("mixed", o2.str());

    std::ostringstream o3(std::string("seed"), std::ios_base::out);
    show("seed-before", o3.str());
    o3 << "tail";
    show("seed-after", o3.str());

    std::ostringstream moved;
    moved << 7;
    std::ostringstream dst(std::move(moved));
    show("move-dst", dst.str());
    show("move-src", moved.str());

    for (int i = 0; i < 8; ++i) {
        std::ostringstream line;
        line << "row=" << i << " val=" << (i * 10);
        std::printf("loop: %s\n", line.str().c_str());
    }

    return 0;
}
