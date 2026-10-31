// The <filesystem> model is a partial path. These checks read the header
// text: an empty path has size 0, its filename is empty, and its stem is
// empty. The header does not define exists. CMake's prism_tests source list
// is explicit (not a glob). The header is not included here: this binary
// already uses the standard std::filesystem in other translation units.

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

fs::path model_header() { return repo() / "src" / "prism" / "pir" / "models" / "cxx" / "filesystem"; }

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
        if (src[i] == '{') ++depth;
        else if (src[i] == '}') {
            if (--depth == 0) return std::string(src.substr(brace, i - brace + 1));
        }
    }
    return {};
}

// Drop // and /* */ comments so a name that is only documented is not a
// declaration. String literals are kept.
std::string strip_comments(const std::string& in) {
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
            out.push_back(in[i++]);
            while (i < n && in[i] != '"') {
                if (in[i] == '\\' && i + 1 < n) {
                    out.push_back(in[i++]);
                    out.push_back(in[i++]);
                } else {
                    out.push_back(in[i++]);
                }
            }
            if (i < n) out.push_back(in[i++]);
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

TEST_CASE("cxx filesystem model: empty filename is empty and exists is not defined") {
    const fs::path header = model_header();
    REQUIRE(fs::is_regular_file(header));
    CHECK_FALSE(fs::is_directory(header));

    const std::string src = slurp(header);
    REQUIRE_FALSE(src.empty());
    CHECK(src.find("_GLIBCXX_FILESYSTEM") != std::string::npos);
    CHECK(src.find("#pragma GCC system_header") != std::string::npos);
    CHECK(src.find("namespace std::filesystem") != std::string::npos);
    CHECK(src.find("#include <string>") != std::string::npos);  // path::string()
    CHECK(src.find("#include <string_view>") == std::string::npos);
    CHECK(src.find("#include <memory>") == std::string::npos);
    CHECK(src.find("#include <mutex>") == std::string::npos);

    // The omitted names are documented, and only there. A declaration would
    // survive comment stripping.
    CHECK(src.find("exists") != std::string::npos);
    CHECK(src.find("fails to compile") != std::string::npos);
    CHECK(src.find("fake file") != std::string::npos);
    const std::string code = strip_comments(src);
    for (const char* name : {"exists", "file_size", "status", "is_regular_file", "is_directory", "current_path",
                             "absolute", "lexically_normal"}) {
        CAPTURE(name);
        CHECK_FALSE(has_ident(code, name));
    }

    CHECK(src.find("path() noexcept : _M_str(nullptr), _M_len(0)") != std::string::npos);
    CHECK(src.find("return _M_len") != std::string::npos);

    const std::string filename = body_of(src, "path path::filename() const");
    REQUIRE_FALSE(filename.empty());
    CHECK(filename.find("if (_M_len == 0) return path();") != std::string::npos);
    CHECK(filename.find("\".\"") == std::string::npos);
    CHECK(filename.find("'.'") == std::string::npos);

    const std::string dtor = body_of(src, "~path()");
    REQUIRE_FALSE(dtor.empty());
    CHECK(dtor.find("_M_free()") != std::string::npos);
    const std::string free_body = body_of(src, "void path::_M_free() noexcept");
    REQUIRE_FALSE(free_body.empty());
    CHECK(count_sub(free_body, "::operator delete(") == 1);
    CHECK(free_body.find("_M_str != nullptr") != std::string::npos);
    CHECK(count_sub(code, "::operator delete(") == 1);

    const std::string moved = body_of(src, "path(path&& __o) noexcept");
    REQUIRE_FALSE(moved.empty());
    CHECK(moved.find("__o._M_str = nullptr") != std::string::npos);
    CHECK(moved.find("_M_free") == std::string::npos);
    CHECK(moved.find("delete") == std::string::npos);

    const std::string join = body_of(src, "path& path::operator/=(const path& __p)");
    REQUIRE_FALSE(join.empty());
    CHECK(join.find("'/'" ) != std::string::npos);
    CHECK(join.find("\"..\"") == std::string::npos);

    const std::string stem = body_of(src, "path path::stem() const");
    REQUIRE_FALSE(stem.empty());
    CHECK(stem.find("if (_M_len == 0) return path();") != std::string::npos);
    CHECK(stem.find("\".\"") == std::string::npos);
    CHECK(stem.find("'.'") == std::string::npos);
    CHECK(stem.find("filename()") != std::string::npos);
    CHECK(stem.find("extension()") != std::string::npos);
    CHECK(stem.find("::operator delete(") == std::string::npos);
}

TEST_CASE("cxx filesystem model: compare orders stored characters") {
    const std::string src = slurp(model_header());
    REQUIRE_FALSE(src.empty());
    const std::string code = strip_comments(src);
    CHECK(count_sub(code, "::operator delete(") == 1);
    CHECK(src.find("#include <string>") != std::string::npos);  // path::string()
    CHECK(src.find("#include <string_view>") == std::string::npos);
    CHECK(src.find("#include <memory>") == std::string::npos);

    const std::string cmp = body_of(src, "int path::compare(const path&");
    REQUIRE_FALSE(cmp.empty());
    CHECK(cmp.find("if (_M_len == 0 && __p._M_len == 0) return 0;") != std::string::npos);
    CHECK(cmp.find("_M_str") != std::string::npos);
    CHECK(cmp.find("return -1") != std::string::npos);
    CHECK(cmp.find("return 1") != std::string::npos);
    CHECK(cmp.find("::operator delete(") == std::string::npos);
    CHECK(cmp.find("__prism_check") == std::string::npos);
    CHECK(cmp.find("exists") == std::string::npos);
    CHECK(cmp.find("stat(") == std::string::npos);
    CHECK(cmp.find("\"..\"") == std::string::npos);
    CHECK(cmp.find("lexically_normal") == std::string::npos);

    // filename, stem, and the length checks stay as they were.
    const std::string filename = body_of(src, "path path::filename() const");
    CHECK(filename.find("if (_M_len == 0) return path();") != std::string::npos);
    const std::string stem = body_of(src, "path path::stem() const");
    CHECK(stem.find("if (_M_len == 0) return path();") != std::string::npos);
    CHECK(src.find("std::filesystem::path length does not fit") != std::string::npos);
    CHECK(src.find("__builtin_trap()") != std::string::npos);
}

TEST_CASE("cxx filesystem model: replace_extension rewrites stored characters") {
    const std::string src = slurp(model_header());
    REQUIRE_FALSE(src.empty());
    const std::string code = strip_comments(src);
    CHECK(count_sub(code, "::operator delete(") == 1);
    CHECK(src.find("#include <string>") != std::string::npos);  // path::string()
    CHECK(src.find("#include <string_view>") == std::string::npos);
    CHECK(src.find("#include <memory>") == std::string::npos);
    for (const char* name : {"exists", "lexically_normal", "lexically_relative"}) {
        CAPTURE(name);
        CHECK_FALSE(has_ident(code, name));
    }

    CHECK(src.find("path& replace_extension(const path& __rep = path());") != std::string::npos);
    const std::string repl = body_of(src, "path& path::replace_extension(");
    REQUIRE_FALSE(repl.empty());
    const auto early = repl.find("if (_M_len == 0) return *this;");
    REQUIRE(early != std::string::npos);
    const auto rep_at = repl.find("__rep");
    const auto str_at = repl.find("_M_str");
    REQUIRE(rep_at != std::string::npos);
    REQUIRE(str_at != std::string::npos);
    CHECK(early < rep_at);
    CHECK(early < str_at);
    CHECK(repl.find("extension()") != std::string::npos);
    CHECK(repl.find("\"..\"") != std::string::npos);
    CHECK(repl.find("!= '.'") != std::string::npos);
    CHECK(repl.find("__prism_check") != std::string::npos);
    CHECK(repl.find("__builtin_trap()") != std::string::npos);
    CHECK(repl.find("replace_extension length does not fit") != std::string::npos);
    CHECK(repl.find("const std::size_t __rn = __self ? _M_len :") != std::string::npos);
    CHECK(repl.find("::operator delete(") == std::string::npos);
    CHECK(repl.find("exists") == std::string::npos);
    CHECK(repl.find("lexically_normal") == std::string::npos);
    CHECK(repl.find("lexically_relative") == std::string::npos);
    CHECK(repl.find("stat(") == std::string::npos);

    // compare, stem, filename, extension, parent_path, and operator/ stay.
    const std::string cmp = body_of(src, "int path::compare(const path&");
    CHECK(cmp.find("if (_M_len == 0 && __p._M_len == 0) return 0;") != std::string::npos);
    const std::string stem = body_of(src, "path path::stem() const");
    CHECK(stem.find("if (_M_len == 0) return path();") != std::string::npos);
    const std::string filename = body_of(src, "path path::filename() const");
    CHECK(filename.find("if (_M_len == 0) return path();") != std::string::npos);
    const std::string ext = body_of(src, "path path::extension() const");
    CHECK(ext.find("if (__n == 2 && _M_str[__b] == '.' && _M_str[__b + 1] == '.') return path();") !=
          std::string::npos);
    const std::string parent = body_of(src, "path path::parent_path() const");
    CHECK(parent.find("if (_M_len == 0) return path();") != std::string::npos);
    const std::string join = body_of(src, "path& path::operator/=(const path& __p)");
    CHECK(join.find("'/'" ) != std::string::npos);
    const std::string slash = body_of(src, "path operator/(const path&");
    CHECK(slash.find("/=") != std::string::npos);
}
