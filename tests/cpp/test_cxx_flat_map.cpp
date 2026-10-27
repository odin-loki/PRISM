// The std::flat_map model header (src/prism/pir/models/cxx/flat_map) is a
// file. This test reads that file. It does not compile it and it does not
// run pir. A missing at() is a throw (or a __prism_check) and not a default
// mapped value. erase is described as freeing the node.

#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

namespace fs = std::filesystem;

namespace {

fs::path header() {
    return fs::path(PRISM_SOURCE_DIR) / "src" / "prism" / "pir" / "models" / "cxx" / "flat_map";
}

std::string slurp(const fs::path& p) {
    std::ifstream in(p);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

std::string slice_from(const std::string& src, const std::string& begin, const std::string& end) {
    const auto a = src.find(begin);
    if (a == std::string::npos) return {};
    const auto b = src.find(end, a + begin.size());
    if (b == std::string::npos) return {};
    return src.substr(a, b - a);
}

}  // namespace

TEST_CASE("cxx flat_map model: missing at throws and erase frees the node") {
    const fs::path path = header();
    REQUIRE(fs::is_regular_file(path));
    CHECK_FALSE(fs::is_directory(path));
    const std::string src = slurp(path);
    REQUIRE(src.find("class flat_map") != std::string::npos);
    CHECK(src.find("_GLIBCXX_FLAT_MAP") != std::string::npos);
    CHECK(src.find("#pragma GCC system_header") != std::string::npos);
    CHECK(src.find("namespace std") != std::string::npos);
    CHECK(src.find("#include <map>") == std::string::npos);
    CHECK(src.find("#include <vector>") == std::string::npos);
    CHECK(src.find("#include <memory>") == std::string::npos);
    CHECK(src.find("#include <initializer_list>") == std::string::npos);
    CHECK(src.find("fail to compile") != std::string::npos);
    CHECK(src.find("pir falls back") != std::string::npos);

    // Both at() overloads. The miss throws (or is a check) and the only
    // value returned is the mapped object stored in a node.
    const std::string at = slice_from(src, "mapped_type& at(const key_type&", "size_type erase(");
    REQUIRE_FALSE(at.empty());
    CHECK(at.find("at(const key_type& __k) const") != std::string::npos);
    const bool miss = at.find("__throw_out_of_range_fmt") != std::string::npos ||
                      at.find("__prism_check") != std::string::npos;
    CHECK(miss);
    CHECK(at.find("flat_map::at") != std::string::npos);
    CHECK(at.find("key is not present") != std::string::npos);
    CHECK(at.find("not a fabricated element") != std::string::npos);
    CHECK(at.find("return __n->_M_mapped") != std::string::npos);
    CHECK(at.find("return {}") == std::string::npos);
    CHECK(at.find("mapped_type{}") == std::string::npos);
    CHECK(at.find("_Tp{}") == std::string::npos);
    CHECK(at.find("return 0") == std::string::npos);
    CHECK(at.find("return mapped_type") == std::string::npos);

    // operator[] on a missing key inserts a node (the default mapped value
    // is stored there, not returned from at()).
    const std::string index = slice_from(src, "mapped_type& operator[](const key_type&", "mapped_type& at(");
    REQUIRE_FALSE(index.empty());
    CHECK(index.find("_M_link(_M_make(") != std::string::npos);
    CHECK(src.find("::operator new") != std::string::npos);

    // erase of a present key is described as freeing the node. A missing
    // key frees nothing (the delete is only on the found path, then return 0).
    const std::string er = slice_from(src, "size_type erase(const key_type&", "size_type size()");
    REQUIRE_FALSE(er.empty());
    CHECK(er.find("frees the node") != std::string::npos);
    CHECK(er.find("::operator delete") != std::string::npos);
    CHECK(er.find("frees nothing") != std::string::npos);
    CHECK(er.find("return 0") != std::string::npos);
    const auto freed = er.find("::operator delete");
    const auto missing = er.find("frees nothing");
    const auto ret0 = er.find("return 0");
    REQUIRE(freed != std::string::npos);
    REQUIRE(missing != std::string::npos);
    REQUIRE(ret0 != std::string::npos);
    CHECK(freed < missing);
    CHECK(er.find("::operator delete", freed + 1) == std::string::npos);

    CHECK(src.find("size() const noexcept { return _M_size; }") != std::string::npos);
}
