// The <fstream> model is a partial basic_ifstream for char. These checks
// read the header text: a default stream is not open, and read or get on
// that stream is a FUNC-CONTRACT check that does not write a byte and does
// not return a fabricated character. tellg on that stream is the same class
// of check, then __builtin_trap. The return type is long. That path does
// not return 0 and does not return a file position. The header is not
// included here: this binary already uses the standard fstream in other
// translation units.

#include <doctest/doctest.h>

#include <cctype>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>

namespace fs = std::filesystem;

namespace {

fs::path repo() { return fs::path(PRISM_SOURCE_DIR); }

fs::path model_header() { return repo() / "src" / "prism" / "pir" / "models" / "cxx" / "fstream"; }

std::string slurp(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    if (!in) return {};
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

int count_sub(std::string_view s, std::string_view n) {
    int c = 0;
    for (std::size_t i = 0; (i = s.find(n, i)) != std::string_view::npos; i += n.size()) ++c;
    return c;
}

std::string body_of(std::string_view src, std::string_view sig) {
    const auto at = src.find(sig);
    if (at == std::string_view::npos) return {};
    const auto brace = src.find('{', at);
    if (brace == std::string_view::npos) return {};
    int depth = 0;
    for (std::size_t i = brace; i < src.size(); ++i) {
        if (src[i] == '{')
            ++depth;
        else if (src[i] == '}') {
            if (--depth == 0) return std::string(src.substr(brace, i - brace + 1));
        }
    }
    return {};
}

// Drop // and /* */ comments and the contents of string literals, so a name
// that is only documented or only mentioned in a check message is not a
// declaration.
std::string strip_comments_and_strings(const std::string& in) {
    std::string out;
    out.reserve(in.size());
    const std::size_t n = in.size();
    for (std::size_t i = 0; i < n;) {
        if (in[i] == '/' && i + 1 < n && in[i + 1] == '/') {
            i += 2;
            while (i < n && in[i] != '\n') ++i;
            continue;
        }
        if (in[i] == '/' && i + 1 < n && in[i + 1] == '*') {
            i += 2;
            while (i + 1 < n && !(in[i] == '*' && in[i + 1] == '/')) ++i;
            if (i + 1 < n) i += 2;
            else i = n;
            out.push_back(' ');
            continue;
        }
        if (in[i] == '"') {
            out.push_back('"');
            ++i;
            while (i < n && in[i] != '"') {
                if (in[i] == '\\' && i + 1 < n) i += 2;
                else ++i;
            }
            if (i < n) ++i;
            out.push_back('"');
            continue;
        }
        out.push_back(in[i++]);
    }
    return out;
}

bool has_ident(std::string_view s, std::string_view id) {
    for (std::size_t i = 0; (i = s.find(id, i)) != std::string_view::npos; i += id.size()) {
        const bool left = i == 0 || !(std::isalnum(static_cast<unsigned char>(s[i - 1])) || s[i - 1] == '_');
        const std::size_t j = i + id.size();
        const bool right = j >= s.size() || !(std::isalnum(static_cast<unsigned char>(s[j])) || s[j] == '_');
        if (left && right) return true;
    }
    return false;
}

}  // namespace

TEST_CASE("cxx fstream model: a closed ifstream read is a check and writes nothing") {
    const fs::path header = model_header();
    REQUIRE(fs::is_regular_file(header));
    CHECK_FALSE(fs::is_directory(header));

    const std::string src = slurp(header);
    REQUIRE_FALSE(src.empty());
    CHECK(src.find("#ifndef _GLIBCXX_FSTREAM") != std::string::npos);
    CHECK(src.find("#define _GLIBCXX_FSTREAM 1") != std::string::npos);
    CHECK(src.find("#pragma GCC system_header") != std::string::npos);
    CHECK(src.find("namespace std {") != std::string::npos);
    CHECK(src.find("using ifstream = basic_ifstream<char>;") != std::string::npos);
    CHECK(src.find("is_same_v<_CharT, char>") != std::string::npos);
    CHECK(src.find("bool _M_is_open = false;") != std::string::npos);
    CHECK(src.find("return _M_is_open;") != std::string::npos);
    CHECK(src.find("_M_is_open = true") == std::string::npos);
    CHECK(src.find("~basic_ifstream() = default;") != std::string::npos);
    CHECK(src.find("extern \"C\" void __prism_check(int, const char*, const char*);") != std::string::npos);
    CHECK(count_sub(src, "extern \"C\" void __prism_check(int, const char*, const char*);") == 1);
    CHECK(src.find("__prism_check(int, const char*, const char*) {") == std::string::npos);

    for (const char* inc : {"<string>", "<filesystem>", "<memory>", "<mutex>", "<iostream>", "<istream>", "<ostream>"}) {
        CAPTURE(inc);
        CHECK(src.find(std::string("#include ") + inc) == std::string::npos);
    }
    CHECK(src.find("::operator new") == std::string::npos);
    CHECK(src.find("::operator delete") == std::string::npos);
    CHECK(src.find("malloc") == std::string::npos);
    CHECK(src.find("free(") == std::string::npos);

    CHECK(src.find("fake file") != std::string::npos);
    CHECK(src.find("fails to compile") != std::string::npos);
    CHECK(src.find("not a proof that a file exists") != std::string::npos);

    const std::string code = strip_comments_and_strings(src);
    for (const char* name : {"open", "close", "ofstream", "fstream", "basic_ofstream", "basic_fstream", "basic_filebuf",
                             "filebuf", "wifstream", "exists", "peek", "getline", "ignore"}) {
        CAPTURE(name);
        CHECK_FALSE(has_ident(code, name));
    }
    CHECK(has_ident(code, "ifstream"));
    CHECK(has_ident(code, "is_open"));
    CHECK(has_ident(code, "basic_ifstream"));

    const auto read = body_of(src, "basic_ifstream& read(");
    REQUIRE_FALSE(read.empty());
    CHECK(read.find("__prism_check(_M_is_open ? 1 : 0, \"FUNC-CONTRACT\",") != std::string::npos);
    CHECK(read.find("std::basic_ifstream::read on a stream that is not open") != std::string::npos);
    CHECK(read.find("__s[") == std::string::npos);
    CHECK(read.find("*__s") == std::string::npos);
    CHECK(read.find("return -1") == std::string::npos);

    const auto get = body_of(src, "int get()");
    REQUIRE_FALSE(get.empty());
    CHECK(get.find("__prism_check(_M_is_open ? 1 : 0, \"FUNC-CONTRACT\",") != std::string::npos);
    CHECK(get.find("std::basic_ifstream::get on a stream that is not open") != std::string::npos);
    CHECK(get.find("return -1;") != std::string::npos);
    CHECK(get.find("return '") == std::string::npos);
    CHECK(get.find("return 0") == std::string::npos);

    const auto get_ref = body_of(src, "basic_ifstream& get(char_type& __c)");
    REQUIRE_FALSE(get_ref.empty());
    CHECK(get_ref.find("__prism_check(_M_is_open ? 1 : 0, \"FUNC-CONTRACT\",") != std::string::npos);
    CHECK(get_ref.find("__c =") == std::string::npos);
    CHECK(get_ref.find("= __c") == std::string::npos);

    const auto get_buf = body_of(src, "basic_ifstream& get(char_type* __s, std::ptrdiff_t __n)");
    REQUIRE_FALSE(get_buf.empty());
    CHECK(get_buf.find("__prism_check(_M_is_open ? 1 : 0, \"FUNC-CONTRACT\",") != std::string::npos);
    CHECK(get_buf.find("__s[") == std::string::npos);
    CHECK(get_buf.find("*__s") == std::string::npos);
    CHECK(get_buf.find("'\\0'") == std::string::npos);

    const auto get_delim = body_of(src, "char_type __delim)");
    REQUIRE_FALSE(get_delim.empty());
    CHECK(get_delim.find("__prism_check(_M_is_open ? 1 : 0, \"FUNC-CONTRACT\",") != std::string::npos);
    CHECK(get_delim.find("__s[") == std::string::npos);
    CHECK(get_delim.find("*__s") == std::string::npos);

    const auto is_open = body_of(src, "bool is_open() const noexcept");
    CHECK(is_open == "{ return _M_is_open; }");

    CHECK(src.find("long tellg()") != std::string::npos);
    CHECK(has_ident(code, "tellg"));
    const auto tellg = body_of(src, "long tellg()");
    REQUIRE_FALSE(tellg.empty());
    const auto tellg_check = tellg.find("__prism_check(_M_is_open ? 1 : 0, \"FUNC-CONTRACT\",");
    const auto tellg_trap = tellg.find("__builtin_trap()");
    REQUIRE(tellg_check != std::string::npos);
    REQUIRE(tellg_trap != std::string::npos);
    CHECK(tellg_check < tellg_trap);
    CHECK(tellg.find("std::basic_ifstream::tellg on a stream that is not open") != std::string::npos);
    CHECK(tellg.find("return") == std::string::npos);
    CHECK(tellg.find("return 0") == std::string::npos);
    CHECK(tellg.find("pos_type") == std::string::npos);
    CHECK(tellg.find("streampos") == std::string::npos);
    CHECK(tellg.find("streamoff") == std::string::npos);
    CHECK(tellg.find("fpos") == std::string::npos);
    CHECK(tellg.find("_M_is_open =") == std::string::npos);
    CHECK(count_sub(src, "std::basic_ifstream::tellg on a stream that is not open") == 1);
    CHECK(count_sub(src, "__builtin_trap()") == 1);

    CHECK(count_sub(src, "__prism_check(_M_is_open ? 1 : 0, \"FUNC-CONTRACT\",") == 6);
}
