// stdio buffer models: a null fgets buffer, an overflowing fread, and an
// overflowing fwrite are checks. feof and ferror on a null FILE return 0.
// ftell on a null FILE returns -1. That is not a proof of the file position.
// Reads stdio.cpp as text. Does not compile the model.

#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>

namespace fs = std::filesystem;

namespace {

fs::path repo() { return fs::path(PRISM_SOURCE_DIR); }

std::string slurp(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    if (!in) return {};
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

// Third string literal of the __prism_check call that starts at `check`.
std::string check_message(std::string_view src, std::size_t check) {
    int quotes = 0;
    std::size_t start = 0;
    for (std::size_t i = check; i < src.size(); ++i) {
        if (src[i] != '"') continue;
        ++quotes;
        if (quotes == 3) start = i + 1;
        if (quotes == 4) return std::string(src.substr(start, i - start));
    }
    return {};
}

std::string function_body(std::string_view src, std::string_view sig, std::string_view next) {
    const auto begin = src.find(sig);
    if (begin == std::string_view::npos) return {};
    const auto end = src.find(next, begin + sig.size());
    if (end == std::string_view::npos) return {};
    return std::string(src.substr(begin, end - begin));
}

// Brace block that opens after `at`.
std::string brace_block_after(std::string_view src, std::size_t at) {
    const auto open = src.find('{', at);
    if (open == std::string_view::npos) return {};
    int depth = 0;
    for (std::size_t i = open; i < src.size(); ++i) {
        if (src[i] == '{')
            ++depth;
        else if (src[i] == '}') {
            --depth;
            if (depth == 0) return std::string(src.substr(open, i + 1 - open));
        }
    }
    return {};
}

}  // namespace

TEST_CASE("stdio buffer: fgets check names fgets") {
    const auto src = slurp(repo() / "src" / "prism" / "pir" / "models" / "libc" / "stdio.cpp");
    REQUIRE(!src.empty());
    const auto body = function_body(src, "char *fgets(", "size_t fread(");
    REQUIRE(!body.empty());
    const auto check = body.find("__prism_check(");
    REQUIRE(check != std::string::npos);
    CHECK(body.find("STR-NULL-ARG") != std::string::npos);
    const auto msg = check_message(body, check);
    CHECK(msg.find("fgets") != std::string::npos);
}

TEST_CASE("stdio buffer: fread overflow is a check and does not return 0") {
    const auto src = slurp(repo() / "src" / "prism" / "pir" / "models" / "libc" / "stdio.cpp");
    REQUIRE(!src.empty());
    const auto body = function_body(src, "size_t fread(", "size_t fwrite(");
    REQUIRE(!body.empty());
    const auto mul = body.find("__builtin_mul_overflow");
    REQUIRE(mul != std::string::npos);
    const auto arm = brace_block_after(body, mul);
    REQUIRE(!arm.empty());
    CHECK(arm.find("__prism_check(") != std::string::npos);
    CHECK(arm.find("MEM-OOB-WRITE") != std::string::npos || arm.find("MEM-OOB-READ") != std::string::npos);
    CHECK(arm.find("return 0") == std::string::npos);
}

TEST_CASE("stdio buffer: fwrite overflow does not read and is not a count") {
    const auto src = slurp(repo() / "src" / "prism" / "pir" / "models" / "libc" / "stdio.cpp");
    REQUIRE(!src.empty());
    const auto body = function_body(src, "size_t fwrite(", "int fgetc(");
    REQUIRE(!body.empty());
    const auto mul = body.find("__builtin_mul_overflow");
    REQUIRE(mul != std::string::npos);
    const auto arm = brace_block_after(body, mul);
    REQUIRE(!arm.empty());
    const auto check = arm.find("__prism_check(");
    REQUIRE(check != std::string::npos);
    CHECK(arm.find("MEM-OOB-READ") != std::string::npos);
    CHECK(check_message(arm, check) == "fwrite size * nmemb overflows");
    CHECK(arm.find("__prism_assume(0)") != std::string::npos);
    CHECK(arm.find("return 0") != std::string::npos);
    CHECK(arm.find("__prism_read_range") == std::string::npos);
    CHECK(arm.find("return k") == std::string::npos);

    const auto arm_at = body.find(arm, mul);
    REQUIRE(arm_at != std::string::npos);
    const auto cond = body.substr(mul, arm_at - mul);
    CHECK(cond.find("size == 0") == std::string::npos);
    CHECK(cond.find("nmemb == 0") == std::string::npos);
    const auto rest = body.substr(arm_at + arm.size());
    CHECK(rest.find("__prism_assume(0)") == std::string::npos);
    CHECK(rest.find("__prism_read_range(p, t)") != std::string::npos);
    CHECK(rest.find("return k") != std::string::npos);
}

TEST_CASE("stdio buffer: fopen message still names fopen") {
    const auto src = slurp(repo() / "src" / "prism" / "pir" / "models" / "libc" / "stdio.cpp");
    REQUIRE(!src.empty());
    const auto body = function_body(src, "FILE *fopen(", "int fclose(");
    REQUIRE(!body.empty());
    int found = 0;
    for (std::size_t at = 0;;) {
        const auto check = body.find("__prism_check(", at);
        if (check == std::string::npos) break;
        const auto msg = check_message(body, check);
        CHECK(msg.find("fopen") != std::string::npos);
        ++found;
        at = check + std::string("__prism_check(").size();
    }
    CHECK(found == 2);
}

TEST_CASE("stdio buffer: feof and ferror null file returns 0 and is not a proof") {
    const auto src = slurp(repo() / "src" / "prism" / "pir" / "models" / "libc" / "stdio.cpp");
    REQUIRE(!src.empty());

    const auto feof_at = src.find("int feof(");
    const auto ferror_at = src.find("int ferror(");
    REQUIRE(feof_at != std::string::npos);
    REQUIRE(ferror_at != std::string::npos);
    REQUIRE(feof_at < ferror_at);

    const auto note_at = src.rfind("/*", feof_at);
    REQUIRE(note_at != std::string::npos);
    const auto note = src.substr(note_at, feof_at - note_at);
    CHECK(note.find("not a proof that the stream is at end of file") != std::string::npos);
    CHECK(note.find("not a proof that the stream has an error") != std::string::npos);
    CHECK(note.find("not PROVED") != std::string::npos);
    CHECK(note.find("not CLEAN") != std::string::npos);

    const auto flag = function_body(src, "static int prism_file_flag(", "int feof(");
    REQUIRE(!flag.empty());
    const auto ok = flag.find("prism_file_ok(f)");
    REQUIRE(ok != std::string::npos);
    const auto null_if = flag.find("if (f == 0)", ok);
    REQUIRE(null_if != std::string::npos);
    const auto arm = brace_block_after(flag, null_if);
    REQUIRE(!arm.empty());
    CHECK(arm.find("__prism_assume(0)") != std::string::npos);
    CHECK(arm.find("return 0") != std::string::npos);
    CHECK(arm.find("return 1") == std::string::npos);
    CHECK(arm.find("__VERIFIER_nondet_int") == std::string::npos);
    CHECK(arm.find("__prism_havoc_bytes") == std::string::npos);
    CHECK(arm.find("__prism_read_range") == std::string::npos);

    const auto arm_at = flag.find(arm, null_if);
    REQUIRE(arm_at != std::string::npos);
    const auto rest = flag.substr(arm_at + arm.size());
    CHECK(rest.find("return __VERIFIER_nondet_int() ? 1 : 0") != std::string::npos);
    CHECK(rest.find("__prism_havoc_bytes") == std::string::npos);
    CHECK(rest.find("__prism_read_range") == std::string::npos);

    const auto feof_body = function_body(src, "int feof(", "int ferror(");
    const auto ferror_body = function_body(src, "int ferror(", "}  // extern");
    REQUIRE(!feof_body.empty());
    REQUIRE(!ferror_body.empty());
    CHECK(feof_body.find("return prism_file_flag(f)") != std::string::npos);
    CHECK(ferror_body.find("return prism_file_flag(f)") != std::string::npos);
    CHECK(feof_body.find("return 1") == std::string::npos);
    CHECK(ferror_body.find("return 1") == std::string::npos);
    CHECK(feof_body.find("__prism_havoc_bytes") == std::string::npos);
    CHECK(ferror_body.find("__prism_havoc_bytes") == std::string::npos);
}

TEST_CASE("stdio buffer: ftell null file returns -1 and is not a proof") {
    const auto src = slurp(repo() / "src" / "prism" / "pir" / "models" / "libc" / "stdio.cpp");
    REQUIRE(!src.empty());

    const auto ftell_at = src.find("long ftell(");
    const auto ferror_at = src.find("int ferror(");
    REQUIRE(ftell_at != std::string::npos);
    REQUIRE(ferror_at != std::string::npos);
    REQUIRE(ferror_at < ftell_at);

    const auto note_at = src.rfind("/*", ftell_at);
    REQUIRE(note_at != std::string::npos);
    const auto note = src.substr(note_at, ftell_at - note_at);
    CHECK(note.find("not a proof of the file position") != std::string::npos);
    CHECK(note.find("not PROVED") != std::string::npos);
    CHECK(note.find("not CLEAN") != std::string::npos);

    const auto body = function_body(src, "long ftell(", "}  // extern");
    REQUIRE(!body.empty());
    const auto ok = body.find("prism_file_ok(f)");
    REQUIRE(ok != std::string::npos);
    const auto null_if = body.find("if (f == 0)", ok);
    REQUIRE(null_if != std::string::npos);
    const auto arm = brace_block_after(body, null_if);
    REQUIRE(!arm.empty());
    CHECK(arm.find("__prism_assume(0)") != std::string::npos);
    CHECK(arm.find("return -1") != std::string::npos);
    CHECK(arm.find("return 0") == std::string::npos);
    CHECK(arm.find("__VERIFIER_nondet_long") == std::string::npos);
    CHECK(arm.find("__prism_havoc_bytes") == std::string::npos);
    CHECK(arm.find("__prism_read_range") == std::string::npos);

    const auto arm_at = body.find(arm, null_if);
    REQUIRE(arm_at != std::string::npos);
    const auto rest = body.substr(arm_at + arm.size());
    CHECK(rest.find("return __VERIFIER_nondet_long()") != std::string::npos);
    CHECK(rest.find("return 0") == std::string::npos);
    CHECK(rest.find("return -1") == std::string::npos);
    CHECK(rest.find("__prism_havoc_bytes") == std::string::npos);
    CHECK(rest.find("__prism_read_range") == std::string::npos);
    CHECK(body.find("fseek(") == std::string::npos);
    CHECK(body.find("rewind(") == std::string::npos);
    CHECK(body.find("snprintf(") == std::string::npos);
    CHECK(body.find("sprintf(") == std::string::npos);
    CHECK(body.find("printf(") == std::string::npos);
}
