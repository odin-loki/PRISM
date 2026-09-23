#include "prism/shipdocs.hpp"

#include "prism/config.hpp"
#include "prism/verdict.hpp"

#include <cctype>
#include <fstream>

namespace prism {

namespace {
#include "prism/shipped_docs.inc"
}  // namespace

const std::vector<ShippedDoc>& shipped_docs() {
    static const std::vector<ShippedDoc> k{{TRUSTED_BASE_FILE, kDoc_TRUSTED_BASE}, {VERDICTS_FILE, kDoc_VERDICTS}};
    return k;
}

std::string_view trusted_base_text() { return kDoc_TRUSTED_BASE; }

std::string trusted_base_sha256() { return sha256_hex(kDoc_TRUSTED_BASE); }

std::string verdict_anchor(std::string_view status) {
    if (!verdict::parse(status)) return {};
    std::string a = "verdict-";
    for (char c : status) a += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return a;
}

void write_shipped_docs(const std::filesystem::path& dir) {
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    for (const auto& d : shipped_docs()) {
        std::ofstream out(dir / std::string(d.name), std::ios::binary);
        out << d.text;
    }
}

}  // namespace prism
