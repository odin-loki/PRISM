// The <stacktrace> model is one extensionless file. current() stores at most
// one return address. empty() is size() == 0 and does not read a frame.
// operator[] checks the index and traps instead of returning a frame.
// operator== of two empty traces is true and does not read a stored address.
// Different sizes are unequal and do not read past the shorter trace. The
// same size compares the stored addresses. operator!= is the negation.
// That comparison is not PROVED and not CLEAN.
// docs/finish-notes/stacktrace-equal.md
// CMake's prism_tests source list is explicit (not a glob).

#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>

#if defined(__GNUC__) || defined(__clang__)
#  if defined(_MSC_VER)
#    pragma warning(push)
#    pragma warning(disable : 4068)
#  endif
#  include "../../src/prism/pir/models/cxx/stacktrace"
#  if defined(_MSC_VER)
#    pragma warning(pop)
#  endif
#  define PRISM_CXX_STACKTRACE_COMPILED 1
#endif

namespace fs = std::filesystem;

namespace {

fs::path header_path() {
    return fs::path(PRISM_SOURCE_DIR) / "src" / "prism" / "pir" / "models" / "cxx" / "stacktrace";
}

std::string slurp(const fs::path& p) {
    std::ifstream in(p);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
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

}  // namespace

TEST_CASE("cxx stacktrace model: one return address, and an index past size traps") {
    const fs::path header = header_path();
    REQUIRE(fs::is_regular_file(header));
    CHECK_FALSE(fs::is_directory(header));
    const std::string src = slurp(header);
    REQUIRE_FALSE(src.empty());
    CHECK(src.find("_GLIBCXX_STACKTRACE") != std::string::npos);
    CHECK(src.find("#pragma GCC system_header") != std::string::npos);
    CHECK(src.find("namespace std") != std::string::npos);
    CHECK(src.find("class basic_stacktrace") != std::string::npos);
    CHECK(src.find("using stacktrace = basic_stacktrace;") != std::string::npos);
    CHECK(src.find("template <") == std::string::npos);
    CHECK(src.find("template<") == std::string::npos);
    CHECK(src.find("#include <memory>") == std::string::npos);
    CHECK(src.find("#include <string>") == std::string::npos);
    CHECK(src.find("#include <functional>") == std::string::npos);
    CHECK(src.find("description") == std::string::npos);
    CHECK(src.find("unknown") == std::string::npos);
    CHECK(src.find("main") == std::string::npos);
    CHECK(src.find("line 1") == std::string::npos);
    {
        std::istringstream lines(src);
        std::string line;
        while (std::getline(lines, line)) {
            std::size_t i = 0;
            while (i < line.size() && (line[i] == ' ' || line[i] == '\t')) ++i;
            CHECK(line.compare(i, 8, "#include") != 0);
        }
    }

    CHECK(src.find("extern \"C\" void __prism_check(int ok, const char* cls, const char* msg);") !=
          std::string::npos);
    CHECK(src.find("__prism_check(int ok, const char* cls, const char* msg) {") == std::string::npos);

    const auto current = body_of(src, "static basic_stacktrace current() noexcept");
    REQUIRE_FALSE(current.empty());
    CHECK(current.find("__builtin_return_address(0)") != std::string::npos);
    CHECK(current.find("__builtin_return_address(1)") == std::string::npos);
    CHECK(current.find("return basic_stacktrace();") != std::string::npos);
    CHECK(current.find("return basic_stacktrace(__ip, 1);") != std::string::npos);
    const auto recorded = current.find("return basic_stacktrace(__ip, 1);");
    const auto builtin = current.find("__builtin_return_address(0)");
    REQUIRE(builtin != std::string::npos);
    REQUIRE(recorded != std::string::npos);
    CHECK(builtin < recorded);

    CHECK(src.find("size_type size() const noexcept { return _M_size; }") != std::string::npos);
    CHECK(src.find("size_type _M_size = 0;") != std::string::npos);
    CHECK(src.find("basic_stacktrace() noexcept = default;") != std::string::npos);

    const auto empty_fn = body_of(src, "bool empty() const noexcept");
    REQUIRE_FALSE(empty_fn.empty());
    CHECK(empty_fn.find("return size() == 0;") != std::string::npos);
    CHECK(empty_fn.find("_M_frame") == std::string::npos);
    CHECK(empty_fn.find("_M_size") == std::string::npos);
    CHECK(empty_fn.find("stacktrace_entry") == std::string::npos);
    CHECK(empty_fn.find("native_handle") == std::string::npos);
    CHECK(empty_fn.find("__builtin") == std::string::npos);
    CHECK(empty_fn.find("basic_stacktrace") == std::string::npos);
    CHECK(empty_fn.find("[") == std::string::npos);

    const auto index = body_of(src, "const_reference operator[](size_type __n) const");
    REQUIRE_FALSE(index.empty());
    const auto check = index.find("__prism_check");
    const auto trap = index.find("__builtin_trap");
    const auto ret = index.find("return _M_frame;");
    REQUIRE(check != std::string::npos);
    REQUIRE(trap != std::string::npos);
    REQUIRE(ret != std::string::npos);
    CHECK(check < trap);
    CHECK(trap < ret);
    CHECK(index.find("__n < this->size()") != std::string::npos);
    CHECK(index.find("MEM-OOB-READ") != std::string::npos);
    CHECK(index.find("return stacktrace_entry") == std::string::npos);
    CHECK(index.find("return {}") == std::string::npos);
    CHECK(index.find("return nullptr") == std::string::npos);
    CHECK(index.find("native_handle") == std::string::npos);
}

TEST_CASE("cxx stacktrace model: equality does not read past the shorter trace") {
    const fs::path header = header_path();
    REQUIRE(fs::is_regular_file(header));
    const std::string src = slurp(header);
    REQUIRE_FALSE(src.empty());
    CHECK(src.find("description") == std::string::npos);
    CHECK(src.find("source_file") == std::string::npos);
    CHECK(src.find("source_line") == std::string::npos);
    CHECK(src.find("This is not PROVED and not CLEAN.") != std::string::npos);

    const auto current = body_of(src, "static basic_stacktrace current() noexcept");
    REQUIRE_FALSE(current.empty());
    CHECK(current.find("__builtin_return_address(0)") != std::string::npos);
    CHECK(current.find("__builtin_return_address(1)") == std::string::npos);
    CHECK(current.find("operator==") == std::string::npos);
    CHECK(current.find("operator!=") == std::string::npos);
    CHECK(current.find("native_handle") == std::string::npos);

    const auto empty_fn = body_of(src, "bool empty() const noexcept");
    REQUIRE_FALSE(empty_fn.empty());
    CHECK(empty_fn.find("return size() == 0;") != std::string::npos);
    CHECK(empty_fn.find("_M_frame") == std::string::npos);
    CHECK(empty_fn.find("operator==") == std::string::npos);
    CHECK(empty_fn.find("operator!=") == std::string::npos);

    CHECK(src.find("size_type size() const noexcept { return _M_size; }") != std::string::npos);
    const auto index = body_of(src, "const_reference operator[](size_type __n) const");
    REQUIRE_FALSE(index.empty());
    CHECK(index.find("operator==") == std::string::npos);
    CHECK(index.find("operator!=") == std::string::npos);
    CHECK(index.find("return _M_frame;") != std::string::npos);

    CHECK(src.find("friend constexpr bool operator==(const basic_stacktrace& __x, const basic_stacktrace& __y) "
                   "noexcept") != std::string::npos);
    CHECK(src.find("friend constexpr bool operator!=(const basic_stacktrace& __x, const basic_stacktrace& __y) "
                   "noexcept") != std::string::npos);

    const auto eq = body_of(src, "operator==(const basic_stacktrace& __x, const basic_stacktrace& __y)");
    REQUIRE_FALSE(eq.empty());
    const auto both_empty = eq.find("if (__x._M_size == 0 && __y._M_size == 0)");
    const auto empty_ret = eq.find("return true;");
    REQUIRE(both_empty != std::string::npos);
    REQUIRE(empty_ret != std::string::npos);
    CHECK(both_empty < empty_ret);
    const auto empty_prefix = eq.substr(0, empty_ret);
    CHECK(empty_prefix.find("_M_frame") == std::string::npos);
    CHECK(empty_prefix.find("native_handle") == std::string::npos);
    CHECK(empty_prefix.find("__builtin") == std::string::npos);
    CHECK(empty_prefix.find("operator[]") == std::string::npos);
    CHECK(empty_prefix.find("[") == std::string::npos);
    CHECK(empty_prefix.find("stacktrace_entry") == std::string::npos);

    const auto sizes = eq.find("if (__x._M_size != __y._M_size)");
    const auto size_ret = eq.find("return false;");
    REQUIRE(sizes != std::string::npos);
    REQUIRE(size_ret != std::string::npos);
    CHECK(empty_ret < sizes);
    CHECK(sizes < size_ret);
    const auto size_prefix = eq.substr(0, size_ret);
    CHECK(size_prefix.find("_M_frame") == std::string::npos);
    CHECK(size_prefix.find("native_handle") == std::string::npos);
    CHECK(size_prefix.find("__builtin") == std::string::npos);
    CHECK(size_prefix.find("operator[]") == std::string::npos);
    CHECK(size_prefix.find("[") == std::string::npos);

    const auto addrs = eq.find("return __x._M_frame.native_handle() == __y._M_frame.native_handle();");
    REQUIRE(addrs != std::string::npos);
    CHECK(size_ret < addrs);
    CHECK(eq.find("__builtin_return_address") == std::string::npos);
    CHECK(eq.find("__prism_check") == std::string::npos);
    CHECK(eq.find("__builtin_trap") == std::string::npos);
    CHECK(eq.find("stacktrace_entry") == std::string::npos);
    CHECK(eq.find("operator[]") == std::string::npos);
    CHECK(eq.find("basic_stacktrace(") == std::string::npos);
    CHECK(eq.find("return true;", empty_ret + 1) == std::string::npos);

    const auto ne = body_of(src, "operator!=(const basic_stacktrace& __x, const basic_stacktrace& __y)");
    REQUIRE_FALSE(ne.empty());
    CHECK(ne.find("return !(__x == __y);") != std::string::npos);
    CHECK(ne.find("_M_frame") == std::string::npos);
    CHECK(ne.find("native_handle") == std::string::npos);
    CHECK(ne.find("_M_size") == std::string::npos);
    CHECK(ne.find("__builtin") == std::string::npos);
    CHECK(ne.find("__prism_check") == std::string::npos);
    CHECK(ne.find("return true") == std::string::npos);
    CHECK(ne.find("return false") == std::string::npos);
    CHECK(ne.find("stacktrace_entry") == std::string::npos);

    const auto sig = src.find("friend constexpr bool operator==(const basic_stacktrace& __x, "
                               "const basic_stacktrace& __y) noexcept");
    REQUIRE(sig != std::string::npos);
    const auto note_from = src.rfind("Two empty traces compare equal.", sig);
    REQUIRE(note_from != std::string::npos);
    const auto note = src.substr(note_from, sig - note_from);
    CHECK(note.find("does not read a stored address") != std::string::npos);
    CHECK(note.find("do not read past the shorter trace") != std::string::npos);
    CHECK(note.find("stored addresses") != std::string::npos);
    CHECK(note.find("No entry is built") != std::string::npos);
    CHECK(note.find("This is not PROVED and not CLEAN.") != std::string::npos);
}

#if defined(PRISM_CXX_STACKTRACE_COMPILED)
static_assert(std::stacktrace{} == std::stacktrace{});
static_assert(!(std::stacktrace{} != std::stacktrace{}));

TEST_CASE("cxx stacktrace: a default trace is empty and current() keeps at most one frame") {
    std::stacktrace empty;
    CHECK(empty.size() == 0);
    CHECK(empty.empty());

    const std::stacktrace cur = std::stacktrace::current();
    const bool no_frame = cur.size() == 0;
    CHECK(cur.size() <= 1);
    CHECK(cur.empty() == no_frame);
#if defined(__has_builtin) && __has_builtin(__builtin_return_address)
    CHECK(cur.size() == 1);
    CHECK_FALSE(cur.empty());
    REQUIRE(cur.size() == 1);
    CHECK(cur[0].native_handle() != nullptr);
#elif defined(__GNUC__)
    CHECK(cur.size() == 1);
    CHECK_FALSE(cur.empty());
    REQUIRE(cur.size() == 1);
    CHECK(cur[0].native_handle() != nullptr);
#else
    CHECK(cur.size() == 0);
    CHECK(cur.empty());
#endif
}

TEST_CASE("cxx stacktrace: two empty traces compare equal") {
    constexpr std::stacktrace a;
    constexpr std::stacktrace b;
    CHECK(a.size() == 0);
    CHECK(b.size() == 0);
    CHECK(a.empty());
    CHECK(b.empty());
    CHECK(a == b);
    CHECK_FALSE(a != b);
    CHECK(a == a);
    CHECK_FALSE(a != a);

    const std::stacktrace cur = std::stacktrace::current();
    const std::stacktrace copy = cur;
    CHECK(cur.size() <= 1);
    CHECK(copy.size() == cur.size());
    CHECK(copy == cur);
    CHECK_FALSE(copy != cur);
    if (cur.size() == 0) {
        CHECK(cur.empty());
        CHECK(cur == a);
        CHECK_FALSE(cur != a);
    } else {
        CHECK(cur.size() == 1);
        CHECK_FALSE(cur.empty());
        CHECK_FALSE(cur == a);
        CHECK(cur != a);
        CHECK(a != cur);
        CHECK_FALSE(a == cur);
        REQUIRE(cur.size() == 1);
        CHECK(cur[0].native_handle() != nullptr);
        CHECK(copy[0].native_handle() == cur[0].native_handle());
    }

    const std::stacktrace left = std::stacktrace::current();
    const std::stacktrace right = std::stacktrace::current();
    CHECK(left.size() == right.size());
    CHECK(left.size() <= 1);
    if (left.size() == 0) {
        CHECK(left == right);
        CHECK_FALSE(left != right);
    } else {
        REQUIRE(left.size() == 1);
        REQUIRE(right.size() == 1);
        const bool same = left[0].native_handle() == right[0].native_handle();
        CHECK((left == right) == same);
        CHECK((right == left) == same);
        CHECK((left != right) == !same);
        CHECK((right != left) == !same);
    }
}
#endif
