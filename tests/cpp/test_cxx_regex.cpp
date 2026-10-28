// The <regex> model owns a copy of the pattern characters. An empty pattern
// stays empty. empty() is size() == 0 and does not decide a match.
// mark_count() returns 0 and does not scan the pattern for a match.
// A null const char* is a FUNC-CONTRACT check and is not stored
// as a match-all pattern. flags() returns the stored unsigned. It does not
// read the pattern to decide a match, and 0 is not a claim that the pattern
// matches. basic_regex is this regex. basic_regex::swap exchanges the
// pattern pointer, the length, and the flags. It does not read a character.
// Two empty patterns stay empty. Self-swap does not free the pattern.
// basic_regex operator== and operator!= compare two patterns. Two empty
// patterns compare equal and do not read a character. A null pattern
// pointer with length 0 is empty and is not loaded. Different lengths
// compare unequal before a character is read. The same length compares
// the stored characters. operator!= is the negation. This is not PROVED
// and not CLEAN.
// regex_match, regex_search, and regex_replace are not declared:
// a true return would be a fake match.
//
// CMake's prism_tests source list is explicit (not a glob): this file is one
// line in _prism_test_sources. The trace is tests/cxx_models/regex_trace.cpp.
// The header is not included here: this binary already uses the standard
// std::regex in other translation units.

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

fs::path model_header() { return repo() / "src" / "prism" / "pir" / "models" / "cxx" / "regex"; }

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

TEST_CASE("cxx regex model: owns the pattern, does not match, null is not a match-all") {
    const fs::path header = model_header();
    REQUIRE(fs::is_regular_file(header));
    CHECK_FALSE(fs::is_directory(header));

    const std::string src = slurp(header);
    REQUIRE_FALSE(src.empty());
    CHECK(src.find("_GLIBCXX_REGEX") != std::string::npos);
    CHECK(src.find("#pragma GCC system_header") != std::string::npos);
    CHECK(src.find("namespace std {") != std::string::npos);
    CHECK(src.find("class regex {") != std::string::npos);
    CHECK(src.find("This header is not a proof that a string matches.") != std::string::npos);
    CHECK(src.find("extern \"C\" void __prism_check(int, const char*, const char*);") != std::string::npos);
    CHECK(count_sub(src, "extern \"C\" void __prism_check(int, const char*, const char*);") == 1);
    CHECK(src.find("__prism_check(int, const char*, const char*) {") == std::string::npos);
    CHECK(src.find("unsigned long __prism_obj_size(const void*);") != std::string::npos);
    CHECK(src.find("__prism_obj_size(const void*) {") == std::string::npos);
    // the matcher works on std::string subjects (regex_match/regex_search)
    CHECK(src.find("#include <string>") != std::string::npos);
    CHECK(src.find("#include <string_view>") == std::string::npos);
    CHECK(src.find("#include <memory>") == std::string::npos);
    CHECK(src.find("#include <locale>") == std::string::npos);

    CHECK(src.find("regex_match") != std::string::npos);
    CHECK(src.find("regex_search") != std::string::npos);
    CHECK(src.find("regex_replace") != std::string::npos);
    // pattern syntax the matcher does not model is undecided, never a fake match
    CHECK(src.find("__prism_incomplete(") != std::string::npos);
    CHECK(src.find("empty match-all") != std::string::npos);
    CHECK(src.find("stays empty") != std::string::npos);
    const std::string code = strip_comments(src);
    for (const char* name : {"regex_replace", "wregex"}) {
        CAPTURE(name);
        CHECK_FALSE(has_ident(code, name));
    }
    CHECK(has_ident(code, "basic_regex"));
    CHECK(code.find("\".*\"") == std::string::npos);
    for (const char* name : {"regex_match", "regex_search", "match_results"}) {
        CAPTURE(name);
        CHECK(has_ident(code, name));  // modelled by the iterative matcher
    }
    CHECK(src.find("regex_replace(") == std::string::npos);

    CHECK(src.find("regex() noexcept : _M_str(nullptr), _M_len(0)") != std::string::npos);
    CHECK(src.find("return _M_len;") != std::string::npos);
    CHECK(src.find("return _M_str;") != std::string::npos);

    const std::string dtor = body_of(src, "~regex()");
    REQUIRE_FALSE(dtor.empty());
    CHECK(dtor.find("_M_free()") != std::string::npos);
    const std::string free_body = body_of(src, "void regex::_M_free() noexcept");
    REQUIRE_FALSE(free_body.empty());
    CHECK(count_sub(free_body, "::operator delete(") == 1);
    CHECK(free_body.find("_M_str != nullptr") != std::string::npos);
    CHECK(count_sub(code, "::operator delete(") == 1);

    const std::string moved = body_of(src, "regex(regex&& __o) noexcept");
    REQUIRE_FALSE(moved.empty());
    CHECK(moved.find("__o._M_str = nullptr") != std::string::npos);
    CHECK(moved.find("__o._M_len = 0") != std::string::npos);
    CHECK(moved.find("_M_free") == std::string::npos);
    CHECK(moved.find("delete") == std::string::npos);
    CHECK(src.find("regex(regex&& __o) noexcept : _M_str(__o._M_str), _M_len(__o._M_len)") != std::string::npos);

    const std::string move_as = body_of(src, "operator=(regex&& __o) noexcept");
    REQUIRE_FALSE(move_as.empty());
    const auto free_at = move_as.find("_M_free()");
    const auto take = move_as.find("_M_str = __o._M_str;");
    const auto clear = move_as.find("__o._M_str = nullptr;");
    REQUIRE(free_at != std::string::npos);
    REQUIRE(take != std::string::npos);
    REQUIRE(clear != std::string::npos);
    CHECK(free_at < take);
    CHECK(take < clear);
    CHECK(move_as.find("this != &__o") != std::string::npos);

    // every failure is reported in place (no _M_fail helper: a call to a
    // noreturn helper hid the trap from the encoder): __prism_check, then trap
    CHECK(src.find("_M_fail") == std::string::npos);
    for (std::size_t at = src.find("{ __prism_check(0, \""); at != std::string::npos;
         at = src.find("{ __prism_check(0, \"", at + 1)) {
        const auto end = src.find('}', at);
        REQUIRE(end != std::string::npos);
        CHECK(src.substr(at, end - at).find("); __builtin_trap(); ") != std::string::npos);
    }

    const std::string from = body_of(src, "void regex::_M_from_cstr(const char* __s)");
    REQUIRE_FALSE(from.empty());
    const auto null_fail = from.find("__prism_check(0, \"FUNC-CONTRACT\", \"std::regex constructed from a null pointer\")");
    const auto oob_fail =
        from.find("__prism_check(0, \"MEM-OOB-READ\", \"std::regex C string is not terminated within its object\")");
    const auto own = from.find("_M_own(");
    REQUIRE(null_fail != std::string::npos);
    REQUIRE(oob_fail != std::string::npos);
    REQUIRE(own != std::string::npos);
    CHECK(null_fail < oob_fail);
    CHECK(oob_fail < own);
    CHECK(from.find("if (__s == nullptr) { __prism_check(0, \"FUNC-CONTRACT\", \"std::regex constructed from a null pointer\"); __builtin_trap(); }") !=
          std::string::npos);
    CHECK(from.find("if (__n >= __bytes) { __prism_check(0, \"MEM-OOB-READ\", \"std::regex C string is not terminated "
                    "within its object\"); __builtin_trap(); }") != std::string::npos);
    CHECK(from.find("if (__n != 0) _M_own(__s, static_cast<std::size_t>(__n));") != std::string::npos);
    CHECK(from.find("__prism_obj_size(__s)") != std::string::npos);

    const std::string arr = body_of(src, "regex(const char (&__s)[_Nm])");
    REQUIRE_FALSE(arr.empty());
    CHECK(arr.find("while (__n < _Nm && __s[__n] != '\\0') ++__n;") != std::string::npos);
    CHECK(arr.find("if (__n != 0) _M_own(__s, __n);") != std::string::npos);
    CHECK(arr.find("\".*\"") == std::string::npos);

    CHECK(src.find("It does not read the pattern to decide a match") != std::string::npos);
    CHECK(src.find("0 is not a claim that the pattern matches") != std::string::npos);
    CHECK(src.find("not PROVED and not CLEAN") != std::string::npos);
    CHECK(src.find("unsigned _M_flags;") != std::string::npos);
    CHECK(count_sub(src, "_M_flags(0)") == 3);
    CHECK(src.find("regex() noexcept : _M_str(nullptr), _M_len(0), _M_flags(0)") != std::string::npos);
    CHECK(src.find("explicit regex(const char* __s) : _M_str(nullptr), _M_len(0), _M_flags(0)") != std::string::npos);
    CHECK(src.find("explicit regex(const char (&__s)[_Nm]) : _M_str(nullptr), _M_len(0), _M_flags(0)") !=
          std::string::npos);
    CHECK(src.find("regex(const regex& __o) : _M_str(nullptr), _M_len(0), _M_flags(__o._M_flags)") != std::string::npos);
    CHECK(src.find("regex(regex&& __o) noexcept : _M_str(__o._M_str), _M_len(__o._M_len), _M_flags(__o._M_flags)") !=
          std::string::npos);
    CHECK(src.find("_M_flags(__f)") == std::string::npos);

    const std::string flags_fn = body_of(src, "unsigned flags() const noexcept");
    REQUIRE_FALSE(flags_fn.empty());
    CHECK(flags_fn.find("return _M_flags;") != std::string::npos);
    CHECK(flags_fn.find("_M_str") == std::string::npos);
    CHECK(flags_fn.find("_M_len") == std::string::npos);
    CHECK(flags_fn.find("_M_own") == std::string::npos);
    CHECK(flags_fn.find("_M_from_cstr") == std::string::npos);
    CHECK(flags_fn.find("delete") == std::string::npos);

    const std::string flagged = body_of(src, "regex(const char* __s, unsigned __f)");
    REQUIRE_FALSE(flagged.empty());
    CHECK(src.find("regex(const char* __s, unsigned __f) : _M_str(nullptr), _M_len(0) {") != std::string::npos);
    const auto flag_null =
        flagged.find("if (__s == nullptr) { __prism_check(0, \"FUNC-CONTRACT\", \"std::regex constructed from a null pointer\"); __builtin_trap(); }");
    const auto flag_store = flagged.find("_M_flags = __f;");
    const auto flag_from = flagged.find("_M_from_cstr(__s);");
    REQUIRE(flag_null != std::string::npos);
    REQUIRE(flag_store != std::string::npos);
    REQUIRE(flag_from != std::string::npos);
    CHECK(flag_null < flag_store);
    CHECK(flag_store < flag_from);
    CHECK(flagged.find("_M_flags(0)") == std::string::npos);
    CHECK(flagged.find("_M_str") == std::string::npos);
    CHECK(flagged.find("_M_own") == std::string::npos);
    CHECK(flagged.find("delete") == std::string::npos);
    CHECK(flagged.find("\".*\"") == std::string::npos);
    CHECK(from.find("_M_flags") == std::string::npos);

    CHECK(moved.find("__o._M_flags = 0") != std::string::npos);
    const auto move_copy_flag = move_as.find("_M_flags = __o._M_flags;");
    const auto move_clear_flag = move_as.find("__o._M_flags = 0;");
    REQUIRE(move_copy_flag != std::string::npos);
    REQUIRE(move_clear_flag != std::string::npos);
    CHECK(take < move_copy_flag);
    CHECK(move_copy_flag < clear);
    CHECK(clear < move_clear_flag);

    const std::string swap_body = body_of(src, "void regex::_M_swap(regex& __o) noexcept");
    REQUIRE_FALSE(swap_body.empty());
    CHECK(swap_body.find("_M_flags = __o._M_flags;") != std::string::npos);
    CHECK(swap_body.find("__o._M_flags = __f;") != std::string::npos);
    CHECK(swap_body.find("delete") == std::string::npos);
    CHECK(free_body.find("_M_flags") == std::string::npos);
    const std::string own_body = body_of(src, "void regex::_M_own(const char* __s, std::size_t __n)");
    REQUIRE_FALSE(own_body.empty());
    CHECK(own_body.find("_M_flags") == std::string::npos);

    const std::string empty_fn = body_of(src, "bool empty() const noexcept");
    REQUIRE_FALSE(empty_fn.empty());
    CHECK(empty_fn.find("return size() == 0;") != std::string::npos);
    CHECK(empty_fn.find("_M_str") == std::string::npos);
    CHECK(empty_fn.find("_M_len") == std::string::npos);
    CHECK(empty_fn.find("_M_flags") == std::string::npos);
    CHECK(empty_fn.find("_M_own") == std::string::npos);
    CHECK(empty_fn.find("_M_from_cstr") == std::string::npos);
    CHECK(empty_fn.find("__prism_obj_size") == std::string::npos);
    CHECK(empty_fn.find("delete") == std::string::npos);
    CHECK(src.find("empty() is size() == 0") != std::string::npos);
    CHECK(src.find("It does not decide a match") != std::string::npos);

    const std::string marks = body_of(src, "unsigned mark_count() const noexcept");
    REQUIRE_FALSE(marks.empty());
    CHECK(marks.find("return 0;") != std::string::npos);
    CHECK(marks.find("_M_str") == std::string::npos);
    CHECK(marks.find("_M_len") == std::string::npos);
    CHECK(marks.find("_M_flags") == std::string::npos);
    CHECK(marks.find("_M_own") == std::string::npos);
    CHECK(marks.find("_M_from_cstr") == std::string::npos);
    CHECK(marks.find("__prism_obj_size") == std::string::npos);
    CHECK(marks.find("delete") == std::string::npos);
    CHECK(src.find("does not scan the pattern for a match") != std::string::npos);
    CHECK(count_sub(code, "::operator delete(") == 1);
    CHECK(from.find("FUNC-CONTRACT") != std::string::npos);
    CHECK(flags_fn.find("return _M_flags;") != std::string::npos);

    CHECK(src.find("using basic_regex = regex;") != std::string::npos);
    CHECK(src.find("void swap(regex& __o) noexcept;") != std::string::npos);
    CHECK(src.find("void basic_regex::swap(basic_regex& __o) noexcept") != std::string::npos);
    const std::string swap_pub = body_of(src, "void basic_regex::swap(basic_regex& __o) noexcept");
    REQUIRE_FALSE(swap_pub.empty());
    const auto swap_self = swap_pub.find("if (this == &__o) return;");
    const auto swap_call = swap_pub.find("_M_swap(__o);");
    REQUIRE(swap_self != std::string::npos);
    REQUIRE(swap_call != std::string::npos);
    CHECK(swap_self < swap_call);
    CHECK(swap_pub.find("delete") == std::string::npos);
    CHECK(swap_pub.find("_M_free") == std::string::npos);
    CHECK(swap_pub.find("_M_own") == std::string::npos);
    CHECK(swap_pub.find("_M_from_cstr") == std::string::npos);
    CHECK(swap_pub.find("__prism_obj_size") == std::string::npos);
    CHECK(swap_pub.find("_M_str[") == std::string::npos);
    CHECK(swap_pub.find("while") == std::string::npos);
    CHECK(swap_body.find("delete") == std::string::npos);
    CHECK(swap_body.find("_M_str[") == std::string::npos);
    CHECK(swap_body.find("while") == std::string::npos);
    const std::string swap_free = body_of(src, "void swap(basic_regex& __a, basic_regex& __b) noexcept");
    REQUIRE_FALSE(swap_free.empty());
    CHECK(swap_free.find("__a.swap(__b);") != std::string::npos);
    CHECK(swap_free.find("delete") == std::string::npos);
    CHECK(count_sub(code, "::operator delete(") == 1);
    CHECK(dtor.find("::operator delete(") == std::string::npos);
    CHECK(empty_fn.find("return size() == 0;") != std::string::npos);
    CHECK(marks.find("return 0;") != std::string::npos);
    CHECK(flags_fn.find("return _M_flags;") != std::string::npos);
}

TEST_CASE("cxx regex model: equality does not read a character of an empty pattern") {
    const fs::path header = model_header();
    REQUIRE(fs::is_regular_file(header));
    const std::string src = slurp(header);
    REQUIRE_FALSE(src.empty());
    const std::string code = strip_comments(src);
    CHECK(count_sub(code, "::operator delete(") == 1);
    for (const char* name : {"regex_match", "regex_search", "regex_replace"}) {
        CAPTURE(name);
        CHECK_FALSE(has_ident(code, name));
    }

    CHECK(src.find("bool operator==(const regex& __y) const noexcept;") != std::string::npos);
    CHECK(src.find("bool operator!=(const regex& __y) const noexcept;") != std::string::npos);
    CHECK(src.find("inline bool basic_regex::operator==(const basic_regex& __y) const noexcept") != std::string::npos);
    CHECK(src.find("inline bool basic_regex::operator!=(const basic_regex& __y) const noexcept") != std::string::npos);

    const std::string eq = body_of(src, "basic_regex::operator==(const basic_regex& __y) const noexcept");
    REQUIRE_FALSE(eq.empty());
    const auto both_empty = eq.find("if (_M_len == 0 && __y._M_len == 0) return true;");
    REQUIRE(both_empty != std::string::npos);
    const auto empty_ret = eq.find("return true;");
    REQUIRE(empty_ret != std::string::npos);
    CHECK(both_empty == empty_ret - std::string("if (_M_len == 0 && __y._M_len == 0) ").size());
    const auto empty_prefix = eq.substr(0, empty_ret);
    CHECK(empty_prefix.find("_M_str") == std::string::npos);
    CHECK(empty_prefix.find("_M_flags") == std::string::npos);
    CHECK(empty_prefix.find("[") == std::string::npos);
    CHECK(empty_prefix.find("data()") == std::string::npos);

    const auto lens = eq.find("if (_M_len != __y._M_len) return false;");
    REQUIRE(lens != std::string::npos);
    const auto len_ret = eq.find("return false;");
    REQUIRE(len_ret != std::string::npos);
    CHECK(empty_ret < lens);
    CHECK(lens < len_ret);
    const auto len_prefix = eq.substr(0, len_ret);
    CHECK(len_prefix.find("_M_str") == std::string::npos);
    CHECK(len_prefix.find("_M_flags") == std::string::npos);
    CHECK(len_prefix.find("[") == std::string::npos);
    CHECK(len_prefix.find("data()") == std::string::npos);

    const auto chars = eq.find("if (_M_str[__i] != __y._M_str[__i]) return false;");
    const auto scan = eq.find("for (std::size_t __i = 0; __i < _M_len; ++__i)");
    REQUIRE(chars != std::string::npos);
    REQUIRE(scan != std::string::npos);
    CHECK(len_ret < scan);
    CHECK(scan < chars);
    CHECK(eq.find("_M_flags") == std::string::npos);
    CHECK(eq.find("delete") == std::string::npos);
    CHECK(eq.find("_M_free") == std::string::npos);
    CHECK(eq.find("_M_own") == std::string::npos);
    CHECK(eq.find("_M_from_cstr") == std::string::npos);
    CHECK(eq.find("__prism_check") == std::string::npos);
    CHECK(eq.find("__builtin_trap") == std::string::npos);
    CHECK(eq.find("__prism_obj_size") == std::string::npos);
    CHECK(eq.find("flags()") == std::string::npos);
    CHECK(eq.find("return true;", empty_ret + 1) != std::string::npos);

    const std::string ne = body_of(src, "basic_regex::operator!=(const basic_regex& __y) const noexcept");
    REQUIRE_FALSE(ne.empty());
    CHECK(ne.find("return !(*this == __y);") != std::string::npos);
    CHECK(ne.find("_M_str") == std::string::npos);
    CHECK(ne.find("_M_len") == std::string::npos);
    CHECK(ne.find("_M_flags") == std::string::npos);
    CHECK(ne.find("delete") == std::string::npos);
    CHECK(ne.find("_M_free") == std::string::npos);
    CHECK(ne.find("__prism_check") == std::string::npos);
    CHECK(ne.find("return true") == std::string::npos);
    CHECK(ne.find("return false") == std::string::npos);

    const auto sig = src.find("inline bool basic_regex::operator==(const basic_regex& __y) const noexcept");
    REQUIRE(sig != std::string::npos);
    const auto note_from = src.rfind("Two empty patterns compare equal", sig);
    REQUIRE(note_from != std::string::npos);
    const auto note = src.substr(note_from, sig - note_from);
    CHECK(note.find("do not read a character") != std::string::npos);
    CHECK(note.find("is not loaded") != std::string::npos);
    CHECK(note.find("before a character is read") != std::string::npos);
    CHECK(note.find("stored characters") != std::string::npos);
    CHECK(note.find("operator!= is the negation") != std::string::npos);
    CHECK(note.find("This is not PROVED and not CLEAN.") != std::string::npos);

    const std::string empty_fn = body_of(src, "bool empty() const noexcept");
    CHECK(empty_fn.find("return size() == 0;") != std::string::npos);
    CHECK(empty_fn.find("operator==") == std::string::npos);
    CHECK(empty_fn.find("operator!=") == std::string::npos);
    const std::string marks = body_of(src, "unsigned mark_count() const noexcept");
    CHECK(marks.find("return 0;") != std::string::npos);
    CHECK(marks.find("operator==") == std::string::npos);
    const std::string flags_fn = body_of(src, "unsigned flags() const noexcept");
    CHECK(flags_fn.find("return _M_flags;") != std::string::npos);
    CHECK(flags_fn.find("operator==") == std::string::npos);
    CHECK(src.find("[[nodiscard]] std::size_t size() const noexcept { return _M_len; }") != std::string::npos);
    const std::string swap_pub = body_of(src, "void basic_regex::swap(basic_regex& __o) noexcept");
    CHECK(swap_pub.find("if (this == &__o) return;") != std::string::npos);
    CHECK(swap_pub.find("operator==") == std::string::npos);
    CHECK(swap_pub.find("_M_str[") == std::string::npos);
    const std::string from = body_of(src, "void regex::_M_from_cstr(const char* __s)");
    CHECK(from.find("_M_fail(\"FUNC-CONTRACT\", \"std::regex constructed from a null pointer\")") != std::string::npos);
    const std::string flagged = body_of(src, "regex(const char* __s, unsigned __f)");
    const auto flag_null =
        flagged.find("if (__s == nullptr) _M_fail(\"FUNC-CONTRACT\", \"std::regex constructed from a null pointer\");");
    const auto flag_store = flagged.find("_M_flags = __f;");
    REQUIRE(flag_null != std::string::npos);
    REQUIRE(flag_store != std::string::npos);
    CHECK(flag_null < flag_store);
    CHECK(flagged.find("operator==") == std::string::npos);
    CHECK(count_sub(code, "::operator delete(") == 1);
}
