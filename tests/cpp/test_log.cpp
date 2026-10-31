#include "prism/log.hpp"

#include <doctest/doctest.h>

#include <string>
#include <vector>

namespace {
std::vector<std::string> g_lines;
void capture(prism::log::Level l, std::string_view c, std::string_view m) {
    g_lines.push_back(std::to_string(static_cast<int>(l)) + "|" + std::string(c) + "|" + std::string(m));
}
}  // namespace

TEST_CASE("log: a replaced sink receives level, component and message; nullptr restores stderr") {
    g_lines.clear();
    auto old = prism::log::set_sink(&capture);
    prism::log::error("ask", "cannot read r.json");
    prism::log::trace("memtrace", "trip");
    prism::log::set_sink(old);
    REQUIRE(g_lines.size() == 2);
    CHECK(g_lines[0] == "3|ask|cannot read r.json");
    CHECK(g_lines[1] == "0|memtrace|trip");
    CHECK(prism::log::set_sink(nullptr) == old);
}
