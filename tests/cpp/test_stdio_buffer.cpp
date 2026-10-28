// stdio buffer models: a null fgets buffer, an overflowing fread, and an
// overflowing fwrite are checks. feof and ferror on a null FILE return 0.
// ftell on a null FILE returns -1. That is not a proof of the file position.
// fseek on a null FILE returns -1. That is not a proof the position changed.
// rewind on a null FILE assumes 0 and returns. That is not a success that
// means the position is 0. A live FILE does not store a position of 0 and
// is not a proof the stream is at the start.
// clearerr on a null FILE assumes 0 and returns. That does not claim the
// error flag is clear. A live FILE does not store a cleared error or eof
// flag and is not a proof those flags changed.
// ungetc on a null FILE is PTR-NULL-DEREF "ungetc null FILE" then
// __builtin_trap before any store. That path does not return the character.
// EOF returns EOF and does not store a character. Any other call returns
// prism_eof_or of the unsigned char value. It does not store a pushback
// buffer and does not make a later read return that character.
// Reads stdio.cpp as text. Does not compile the model.
// docs/finish-notes/ungetc.md

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
    CHECK((arm.find("MEM-OOB-WRITE") != std::string::npos || arm.find("MEM-OOB-READ") != std::string::npos));
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

    const auto body = function_body(src, "long ftell(", "int fseek(");
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

TEST_CASE("stdio buffer: fseek null file returns -1 and is not a proof") {
    const auto src = slurp(repo() / "src" / "prism" / "pir" / "models" / "libc" / "stdio.cpp");
    const auto body = function_body(src, "int fseek(", "void rewind(");
    REQUIRE(!body.empty());
    const auto note_at = src.rfind("/*", src.find("int fseek("));
    const auto note = src.substr(note_at, src.find("int fseek(") - note_at);
    CHECK(note.find("not a proof the position changed") != std::string::npos);
    CHECK(note.find("not PROVED") != std::string::npos);
    CHECK(note.find("not CLEAN") != std::string::npos);
    const auto null_if = body.find("if (f == 0)");
    REQUIRE(null_if != std::string::npos);
    const auto arm = brace_block_after(body, null_if);
    CHECK(arm.find("__prism_assume(0)") != std::string::npos);
    CHECK(arm.find("return -1") != std::string::npos);
    CHECK(arm.find("return 0") == std::string::npos);
    const auto rest = body.substr(body.find(arm, null_if) + arm.size());
    CHECK(rest.find("return __VERIFIER_nondet_int() ? -1 : 0") != std::string::npos);
    CHECK(rest.find("(void)off") != std::string::npos);
    CHECK(rest.find("(void)whence") != std::string::npos);
    CHECK(rest.find("__prism_havoc_bytes") == std::string::npos);
}

TEST_CASE("stdio buffer: rewind null file returns without a position of 0") {
    const auto src = slurp(repo() / "src" / "prism" / "pir" / "models" / "libc" / "stdio.cpp");
    const auto body = function_body(src, "void rewind(", "void clearerr(");
    REQUIRE(!body.empty());
    const auto note_at = src.rfind("/*", src.find("void rewind("));
    const auto note = src.substr(note_at, src.find("void rewind(") - note_at);
    CHECK(note.find("not a proof the stream is at the start") != std::string::npos);
    CHECK(note.find("not PROVED") != std::string::npos);
    CHECK(note.find("not CLEAN") != std::string::npos);
    CHECK(body.find("return 0") == std::string::npos);
    CHECK(body.find("= 0;") == std::string::npos);
    const auto null_if = body.find("if (f == 0)");
    REQUIRE(null_if != std::string::npos);
    const auto arm = brace_block_after(body, null_if);
    CHECK(arm.find("__prism_assume(0)") != std::string::npos);
    CHECK(arm.find("return;") != std::string::npos);
    CHECK(body.find("__prism_havoc_bytes") == std::string::npos);
}

TEST_CASE("stdio buffer: clearerr null file returns without clearing flags") {
    const auto src = slurp(repo() / "src" / "prism" / "pir" / "models" / "libc" / "stdio.cpp");
    const auto body = function_body(src, "void clearerr(", "int ungetc(");
    REQUIRE(!body.empty());
    const auto note_at = src.rfind("/*", src.find("void clearerr("));
    const auto note = src.substr(note_at, src.find("void clearerr(") - note_at);
    CHECK(note.find("not a proof those flags changed") != std::string::npos);
    CHECK(note.find("not PROVED") != std::string::npos);
    CHECK(note.find("not CLEAN") != std::string::npos);
    CHECK(body.find("return 0") == std::string::npos);
    CHECK(body.find("= 0;") == std::string::npos);
    const auto null_if = body.find("if (f == 0)");
    REQUIRE(null_if != std::string::npos);
    const auto arm = brace_block_after(body, null_if);
    CHECK(arm.find("__prism_assume(0)") != std::string::npos);
    CHECK(arm.find("return;") != std::string::npos);
    CHECK(body.find("feof(") == std::string::npos);
    CHECK(body.find("ferror(") == std::string::npos);
    CHECK(body.find("__prism_havoc_bytes") == std::string::npos);
    CHECK(body.find("fgetc(") == std::string::npos);
    CHECK(body.find("fopen(") == std::string::npos);
    CHECK(body.find("fclose(") == std::string::npos);
    CHECK(body.find("ungetc(") == std::string::npos);
    CHECK(body.find("snprintf(") == std::string::npos);
    CHECK(body.find("sprintf(") == std::string::npos);
    CHECK(body.find("printf(") == std::string::npos);
}

TEST_CASE("stdio buffer: ungetc null file traps and does not push a character back") {
    const auto src = slurp(repo() / "src" / "prism" / "pir" / "models" / "libc" / "stdio.cpp");
    REQUIRE(!src.empty());

    const auto ungetc_at = src.find("int ungetc(");
    const auto clearerr_at = src.find("void clearerr(");
    REQUIRE(ungetc_at != std::string::npos);
    REQUIRE(clearerr_at != std::string::npos);
    REQUIRE(clearerr_at < ungetc_at);

    const auto note_at = src.rfind("/*", ungetc_at);
    REQUIRE(note_at != std::string::npos);
    const auto note = src.substr(note_at, ungetc_at - note_at);
    CHECK(note.find("not a proof a character was pushed back") != std::string::npos);
    CHECK(note.find("does not store a pushback buffer") != std::string::npos);
    CHECK(note.find("A later read is not that character") != std::string::npos);
    CHECK(note.find("does not yield the character") != std::string::npos);
    CHECK(note.find("EOF yields EOF and stores nothing") != std::string::npos);
    CHECK(note.find("not PROVED") != std::string::npos);
    CHECK(note.find("not CLEAN") != std::string::npos);

    const auto body = function_body(src, "int ungetc(", "}  // extern");
    REQUIRE(!body.empty());
    const auto check = body.find("__prism_check(");
    REQUIRE(check != std::string::npos);
    CHECK(body.find("PTR-NULL-DEREF") != std::string::npos);
    CHECK(check_message(body, check) == "ungetc null FILE");

    const auto null_if = body.find("if (f == 0)", check);
    REQUIRE(null_if != std::string::npos);
    CHECK(check < null_if);
    const auto arm = brace_block_after(body, null_if);
    REQUIRE(!arm.empty());
    const auto trap = arm.find("__builtin_trap()");
    REQUIRE(trap != std::string::npos);
    CHECK(arm.find("return") == std::string::npos);
    CHECK(arm.find(" = ") == std::string::npos);
    CHECK(arm.find("c") == std::string::npos);
    CHECK(arm.find("prism_eof_or") == std::string::npos);

    const auto trap_at = body.find("__builtin_trap()");
    REQUIRE(trap_at != std::string::npos);
    const auto before = body.substr(0, trap_at);
    CHECK(before.find(" = ") == std::string::npos);
    CHECK(before.find("return") == std::string::npos);
    CHECK(before.find("prism_eof_or") == std::string::npos);
    CHECK(body.find(" = ") == std::string::npos);

    const auto eof_if = body.find("if (c == EOF)", trap_at);
    REQUIRE(eof_if != std::string::npos);
    CHECK(trap_at < eof_if);
    const auto eof_arm = brace_block_after(body, eof_if);
    REQUIRE(!eof_arm.empty());
    CHECK(eof_arm.find("return EOF;") != std::string::npos);
    CHECK(eof_arm.find(" = ") == std::string::npos);
    CHECK(eof_arm.find("c") == std::string::npos);
    CHECK(eof_arm.find("prism_eof_or") == std::string::npos);
    CHECK(eof_arm.find("__builtin_trap") == std::string::npos);
    CHECK(eof_arm.find("unsigned char") == std::string::npos);

    const auto eof_arm_at = body.find(eof_arm, eof_if);
    REQUIRE(eof_arm_at != std::string::npos);
    const auto rest = body.substr(eof_arm_at + eof_arm.size());
    CHECK(rest.find("return prism_eof_or((unsigned char)c);") != std::string::npos);
    CHECK(rest.find("return c") == std::string::npos);
    CHECK(rest.find("return EOF") == std::string::npos);
    CHECK(rest.find(" = ") == std::string::npos);
    CHECK(rest.find("__builtin_trap") == std::string::npos);
    CHECK(rest.find("[") == std::string::npos);
    CHECK(body.find("pushback") == std::string::npos);
    CHECK(body.find("[") == std::string::npos);
    CHECK(body.find("fgetc(") == std::string::npos);
    CHECK(body.find("fread(") == std::string::npos);
    CHECK(body.find("fwrite(") == std::string::npos);
    CHECK(body.find("fflush(") == std::string::npos);
    CHECK(body.find("fputc(") == std::string::npos);
    CHECK(body.find("putc(") == std::string::npos);
    CHECK(body.find("putchar(") == std::string::npos);
    CHECK(body.find("fseek(") == std::string::npos);
    CHECK(body.find("ftell(") == std::string::npos);
    CHECK(body.find("rewind(") == std::string::npos);
    CHECK(body.find("clearerr(") == std::string::npos);
    CHECK(body.find("feof(") == std::string::npos);
    CHECK(body.find("ferror(") == std::string::npos);
    CHECK(body.find("__prism_havoc_bytes") == std::string::npos);
    CHECK(body.find("__prism_read_range") == std::string::npos);
    CHECK(body.find("__prism_memset") == std::string::npos);
    CHECK(body.find("prism_file_ok") == std::string::npos);

    const auto fgetc_body = function_body(src, "int fgetc(", "int getc(");
    REQUIRE(!fgetc_body.empty());
    CHECK(fgetc_body.find("ungetc") == std::string::npos);
    CHECK(fgetc_body.find("int c = __VERIFIER_nondet_int();") != std::string::npos);
    CHECK(fgetc_body.find("return c;") != std::string::npos);
    CHECK(fgetc_body.find("pushback") == std::string::npos);

    const auto fflush_body = function_body(src, "int fflush(", "static int prism_file_flag(");
    REQUIRE(!fflush_body.empty());
    CHECK(fflush_body.find("if (f) prism_file_ok(f);") != std::string::npos);
    CHECK(fflush_body.find("return prism_eof_or(0);") != std::string::npos);
    CHECK(fflush_body.find("__builtin_trap") == std::string::npos);
    CHECK(fflush_body.find("ungetc(") == std::string::npos);

    const auto harness = slurp(repo() / "testdata" / "conformance" / "libc-models" / "stdio_contracts.cpp");
    REQUIRE(!harness.empty());
    CHECK(harness.find("ungetc(") != std::string::npos);
    CHECK(harness.find("fflush((FILE *)0)") != std::string::npos);
    CHECK(harness.find("pushed back") == std::string::npos);
}
