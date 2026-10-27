// The <flat_set> model stores one heap node per key. These checks read the
// header text: a second insert of a key that is already present returns
// before any new node and before size grows, and erase is where the node
// is freed (the delete expression is in erase, not in a helper).

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

int count_sub(std::string_view s, std::string_view n) {
    int c = 0;
    for (std::size_t i = 0; (i = s.find(n, i)) != std::string_view::npos; i += n.size()) ++c;
    return c;
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

TEST_CASE("cxx flat_set model: duplicate insert does not grow size; erase deletes the node") {
    const auto src = slurp(repo() / "src" / "prism" / "pir" / "models" / "cxx" / "flat_set");
    REQUIRE(!src.empty());

    CHECK(src.find("#ifndef _GLIBCXX_FLAT_SET") != std::string::npos);
    CHECK(src.find("#pragma GCC system_header") != std::string::npos);
    CHECK(src.find("namespace std") != std::string::npos);
    CHECK(src.find("#include <set>") == std::string::npos);
    CHECK(src.find("#include <vector>") == std::string::npos);
    CHECK(src.find("#include <flat_map>") == std::string::npos);
    CHECK(src.find("#include <memory>") == std::string::npos);
    CHECK(src.find("#include <initializer_list>") == std::string::npos);
    CHECK(src.find("fail to compile") != std::string::npos);

    // A key that is already present returns before the allocation and before
    // size grows, and that return does not assign over the stored key.
    for (const char* sig : {"insert(const value_type& __x)", "insert(value_type&& __x)"}) {
        CAPTURE(sig);
        const auto ins = body_of(src, sig);
        REQUIRE(!ins.empty());
        const auto alloc = ins.find("new _Node");
        const auto grow = ins.find("++_M_size");
        REQUIRE(alloc != std::string::npos);
        REQUIRE(grow != std::string::npos);
        CHECK(alloc < grow);
        CHECK(count_sub(ins, "new _Node") == 1);
        CHECK(count_sub(ins, "++_M_size") == 1);
        const auto early = ins.substr(0, alloc);
        CHECK(early.find("return {iterator(__have), false};") != std::string::npos);
        CHECK(early.find("++_M_size") == std::string::npos);
        CHECK(early.find("new ") == std::string::npos);
        CHECK(early.find("_M_key =") == std::string::npos);
    }

    CHECK(src.find("size_type size() const noexcept { return _M_size; }") != std::string::npos);
    CHECK(src.find("iterator begin() const noexcept { return iterator(_M_head); }") != std::string::npos);
    CHECK(src.find("iterator end() const noexcept { return iterator(nullptr); }") != std::string::npos);

    const auto found = body_of(src, "_Node* _M_find_node(const key_type& __x) const");
    REQUIRE(!found.empty());
    CHECK(found.find("new ") == std::string::npos);
    CHECK(found.find("return nullptr;") != std::string::npos);
    CHECK(src.find("bool contains(const key_type& __x) const { return find(__x) != end(); }") != std::string::npos);

    // The free is in erase. A missing key returns before that delete.
    const auto er_key = body_of(src, "size_type erase(const key_type& __x)");
    const auto er_it = body_of(src, "iterator erase(iterator __pos)");
    REQUIRE(!er_key.empty());
    REQUIRE(!er_it.empty());
    CHECK(er_key.find("delete ") != std::string::npos);
    CHECK(er_it.find("delete ") != std::string::npos);
    CHECK(er_key.find("return 0;") != std::string::npos);
    CHECK(er_key.find("return 0;") < er_key.find("delete "));
    CHECK(er_key.find("return 0;") < er_key.find("--_M_size"));
    CHECK(count_sub(er_key, "delete ") == 1);
    CHECK(count_sub(er_it, "delete ") == 1);
    CHECK(count_sub(src, "delete ") == count_sub(er_key, "delete ") + count_sub(er_it, "delete "));
}
