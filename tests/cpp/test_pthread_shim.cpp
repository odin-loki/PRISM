// The Windows PIR <pthread.h> shim is declarations only. Read it as text.
// Do not compile the header: a body that returns success would be a fake lock.

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

bool is_ident(char c) {
    return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_';
}

bool is_space(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v'; }

// A line returns a value when `return` is followed by an operand. `return;` does not.
bool line_returns_value(std::string_view line) {
    constexpr std::string_view key = "return";
    for (std::size_t i = 0; i + key.size() <= line.size(); ++i) {
        if (line.compare(i, key.size(), key) != 0) continue;
        if (i > 0 && is_ident(line[i - 1])) continue;
        const auto end = i + key.size();
        if (end < line.size() && is_ident(line[end])) continue;
        auto j = end;
        while (j < line.size() && (line[j] == ' ' || line[j] == '\t' || line[j] == '\r')) ++j;
        if (j < line.size() && line[j] != ';') return true;
    }
    return false;
}

struct Decl {
    bool semi = false;
    bool brace = false;
};

// After the parameter list, ';' is a declaration and '{' is a brace body.
Decl classify(std::string_view text, std::string_view name) {
    Decl d;
    for (std::size_t i = 0; i + name.size() <= text.size(); ++i) {
        if (text.compare(i, name.size(), name) != 0) continue;
        if (i > 0 && is_ident(text[i - 1])) continue;
        const auto after = i + name.size();
        if (after < text.size() && is_ident(text[after])) continue;
        std::size_t j = after;
        while (j < text.size() && is_space(text[j])) ++j;
        if (j >= text.size() || text[j] != '(') continue;
        int depth = 0;
        for (; j < text.size(); ++j) {
            if (text[j] == '(') {
                ++depth;
            } else if (text[j] == ')') {
                --depth;
                if (depth == 0) {
                    ++j;
                    break;
                }
            }
        }
        while (j < text.size() && is_space(text[j])) ++j;
        if (j < text.size() && text[j] == ';') d.semi = true;
        if (j < text.size() && text[j] == '{') d.brace = true;
    }
    return d;
}

}  // namespace

TEST_CASE("pthread shim: declarations only, missing body is not success") {
    const auto src = slurp(repo() / "src" / "prism" / "pir" / "models" / "libc" / "shim" / "pthread.h");
    REQUIRE(!src.empty());

    CHECK(src.find("Declarations only") != std::string::npos);
    CHECK(src.find("not a concurrency proof") != std::string::npos);
    CHECK(src.find("unmodeled") != std::string::npos);
    CHECK(src.find("rather than success") != std::string::npos);
    CHECK(src.find("PROVED") == std::string::npos);
    CHECK(src.find("CLEAN") == std::string::npos);

    std::size_t i = 0;
    while (i < src.size()) {
        auto n = src.find('\n', i);
        if (n == std::string::npos) n = src.size();
        auto line = std::string_view(src).substr(i, n - i);
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
        INFO(std::string(line));
        CHECK_FALSE(line_returns_value(line));
        if (n == src.size()) break;
        i = n + 1;
    }

    CHECK(src.find("int pthread_mutex_lock(pthread_mutex_t* mutex);") != std::string::npos);
    CHECK(src.find("int pthread_mutex_unlock(pthread_mutex_t* mutex);") != std::string::npos);
    CHECK(src.find(
              "int pthread_create(pthread_t* thread, const void* attr, void* (*start)(void*), void* arg);") !=
          std::string::npos);

    for (const char* name : {"pthread_create", "pthread_join", "pthread_mutex_lock", "pthread_mutex_unlock",
                             "pthread_mutex_init", "pthread_mutex_destroy"}) {
        const auto decl = classify(src, name);
        CHECK(decl.semi);
        CHECK_FALSE(decl.brace);
    }
}
