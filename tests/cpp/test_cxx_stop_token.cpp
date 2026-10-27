// The <stop_token> model is a partial stop_source and stop_token. These
// checks read the header text: the second request_stop returns false and
// does not clear the flag, a default token is not stop_requested, and the
// header does not define stop_callback.

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

// Body of the nth function whose text contains `sig`, including braces.
std::string body_of(std::string_view src, std::string_view sig, std::size_t nth = 0) {
    std::size_t from = 0;
    for (std::size_t n = 0;; ++n) {
        const auto at = src.find(sig, from);
        if (at == std::string_view::npos) return {};
        from = at + sig.size();
        if (n != nth) continue;
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
}

}  // namespace

TEST_CASE("cxx stop_token model: second request_stop is false, default token is not stopped, no callback") {
    const auto src = slurp(repo() / "src" / "prism" / "pir" / "models" / "cxx" / "stop_token");
    REQUIRE(!src.empty());

    CHECK(src.find("#ifndef _GLIBCXX_STOP_TOKEN") != std::string::npos);
    CHECK(src.find("#pragma GCC system_header") != std::string::npos);
    CHECK(src.find("namespace std {") != std::string::npos);
    CHECK(src.find("#include <atomic>") == std::string::npos);
    CHECK(src.find("#include <thread>") == std::string::npos);
    CHECK(src.find("#include <functional>") == std::string::npos);
    CHECK(src.find("stop_callback") == std::string::npos);

    // Move is omitted. A move falls back to the copy, so a moved-from source
    // is not a null flag that stop_possible() would still call true.
    CHECK(src.find("falls back") != std::string::npos);
    CHECK(src.find("stop_source&&") == std::string::npos);
    CHECK(src.find("stop_token&&") == std::string::npos);
    CHECK(src.find("bool stop_possible() const noexcept { return true; }") != std::string::npos);

    const auto req = body_of(src, "bool request_stop() noexcept");
    REQUIRE(!req.empty());
    const auto already = req.find("if (_M_stop) return false;");
    const auto arm = req.find("_M_stop = true;");
    const auto yes = req.find("return true;");
    CHECK(already != std::string::npos);
    CHECK(arm != std::string::npos);
    CHECK(yes != std::string::npos);
    CHECK(already < arm);
    CHECK(arm < yes);
    CHECK(req.find("_M_stop = false") == std::string::npos);

    // A default token stores a null pointer. stop_requested() is false unless
    // that pointer is set and the flag is set.
    CHECK(src.find("stop_token() noexcept : _M_flag(nullptr)") != std::string::npos);
    const auto tok = body_of(src, "bool stop_requested() const noexcept");
    REQUIRE(!tok.empty());
    CHECK(tok.find("return _M_flag != nullptr && *_M_flag;") != std::string::npos);
}
