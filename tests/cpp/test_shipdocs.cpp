// Shipped verdict definitions (docs/VERDICTS.md via write_shipped_docs).
// Law 2: PROVED and BOUNDED are different anchors and different rows.
// Law 3: a fuzzer CLEAN is not a proof.

#include <doctest/doctest.h>
#ifdef ERROR
#  undef ERROR
#endif

#include "prism/shipdocs.hpp"

#include <cctype>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>

namespace {

namespace fs = std::filesystem;

struct TmpDir {
    fs::path dir;
    TmpDir() {
        static int seq = 0;
        dir = fs::temp_directory_path() / ("prism_shipdocs_" + std::to_string(++seq));
        std::error_code ec;
        fs::remove_all(dir, ec);
        fs::create_directories(dir);
    }
    ~TmpDir() {
        std::error_code ec;
        fs::remove_all(dir, ec);
    }
};

std::string slurp(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

std::string lower_copy(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

// Text of one verdict row: from its anchor through the character before the
// next anchor. `id="verdict-proved"` does not match `verdict-proved-certified`.
std::string row_after_anchor(const std::string& text, std::string_view id) {
    const std::string needle = std::string("id=\"") + std::string(id) + "\"";
    const auto at = text.find(needle);
    REQUIRE(at != std::string::npos);
    const auto start = at + needle.size();
    const auto next = text.find("<a id=\"", start);
    const auto end = next == std::string::npos ? text.size() : next;
    return text.substr(start, end - start);
}

}  // namespace

TEST_CASE("shipdocs: PROVED and BOUNDED anchors differ and fuzzer CLEAN is not a proof") {
    TmpDir tmp;
    prism::write_shipped_docs(tmp.dir);

    const auto path = tmp.dir / std::string(prism::VERDICTS_FILE);
    REQUIRE(fs::is_regular_file(path));
    const auto text = slurp(path);

    std::string_view embedded;
    for (const auto& doc : prism::shipped_docs())
        if (doc.name == prism::VERDICTS_FILE) embedded = doc.text;
    REQUIRE_FALSE(embedded.empty());
    CHECK(text == std::string(embedded));

    CHECK(text.find("id=\"verdict-proved\"") != std::string::npos);
    CHECK(text.find("id=\"verdict-bounded\"") != std::string::npos);

    const auto proved = row_after_anchor(text, "verdict-proved");
    const auto bounded = row_after_anchor(text, "verdict-bounded");
    CHECK(proved != bounded);
    CHECK(proved.find("PROVED") != std::string::npos);
    CHECK(bounded.find("BOUNDED") != std::string::npos);
    CHECK(proved.find("BOUNDED") == std::string::npos);
    CHECK(bounded.find("PROVED") == std::string::npos);

    const auto clean = lower_copy(row_after_anchor(text, "verdict-clean"));
    CHECK(clean.find("clean") != std::string::npos);
    CHECK(clean.find("fuzzer") != std::string::npos);
    CHECK(clean.find("not a proof") != std::string::npos);
}
