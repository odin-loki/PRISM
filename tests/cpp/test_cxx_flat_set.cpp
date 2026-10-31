// The <flat_set> model stores one heap node per key. These checks read the
// header text: a second insert of a key that is already present returns
// before any new node and before size grows, erase is where the node is
// freed (the delete expression is in erase, not in a helper), contains
// returns false for a missing key and true for a present key without
// allocating, and operator== of two flat_sets is container equality.

#include <doctest/doctest.h>

#include "prose_text.hpp"

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
    CHECK(count_sub(src, "flat_set iterator dereference at end") == 2);

    const auto found = body_of(src, "_Node* _M_find_node(const key_type& __x) const");
    REQUIRE(!found.empty());
    CHECK(found.find("new ") == std::string::npos);
    CHECK(found.find("return nullptr;") != std::string::npos);

    // A missing key returns false and a present key returns true. The body
    // does not allocate, and it does not go through end().
    const auto has = body_of(src, "bool contains(const key_type& __x) const");
    REQUIRE(!has.empty());
    CHECK(has.find("_M_find_node") != std::string::npos);
    CHECK(has.find("new ") == std::string::npos);
    CHECK(has.find("delete ") == std::string::npos);
    CHECK(has.find("_M_key =") == std::string::npos);
    CHECK(has.find("end()") == std::string::npos);
    const auto miss = has.find("return false;");
    const auto hit = has.find("return true;");
    REQUIRE(miss != std::string::npos);
    REQUIRE(hit != std::string::npos);
    CHECK(miss < hit);
    CHECK(has.substr(0, miss).find("new ") == std::string::npos);

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

TEST_CASE("cxx flat_set model: container equality does not read past the shorter set") {
    const auto src = prism_test::unwrap_prose(slurp(repo() / "src" / "prism" / "pir" / "models" / "cxx" / "flat_set"));
    REQUIRE(!src.empty());

    CHECK(src.find("#include <memory>") == std::string::npos);
    CHECK(src.find("#include <algorithm>") == std::string::npos);
    CHECK(src.find("This is not PROVED and not CLEAN.") != std::string::npos);

    // Iterator equality is still the two node pointers. Container equality
    // is a separate operator on flat_set.
    CHECK(count_sub(src, "operator==(const iterator&") == 1);
    CHECK(count_sub(src, "operator!=(const iterator&") == 0);
    const auto it_eq = body_of(src, "operator==(const iterator& __a, const iterator& __b)");
    REQUIRE(!it_eq.empty());
    CHECK(it_eq.find("return __a._M_node == __b._M_node;") != std::string::npos);
    CHECK(it_eq.find("_M_key") == std::string::npos);
    CHECK(it_eq.find("_M_size") == std::string::npos);

    CHECK(count_sub(src, "operator==(const flat_set&") == 1);
    CHECK(count_sub(src, "operator!=(const flat_set&") == 1);
    const auto eq = body_of(src, "operator==(const flat_set& __x, const flat_set& __y)");
    REQUIRE(!eq.empty());

    // Two empty sets return true before a node is dereferenced.
    const auto heads = eq.find("if (__x._M_head == nullptr && __y._M_head == nullptr)");
    const auto empty_ret = eq.find("return true;");
    REQUIRE(heads != std::string::npos);
    REQUIRE(empty_ret != std::string::npos);
    CHECK(heads < empty_ret);
    const auto empty_prefix = eq.substr(0, empty_ret);
    CHECK(empty_prefix.find("->") == std::string::npos);
    CHECK(empty_prefix.find("_M_key") == std::string::npos);
    CHECK(empty_prefix.find("_M_next") == std::string::npos);
    CHECK(empty_prefix.find("*") == std::string::npos);

    // Different sizes return false before the walk reads a key.
    const auto sizes = eq.find("if (__x._M_size != __y._M_size)");
    const auto size_ret = eq.find("return false;");
    REQUIRE(sizes != std::string::npos);
    REQUIRE(size_ret != std::string::npos);
    CHECK(empty_ret < sizes);
    CHECK(sizes < size_ret);
    const auto size_prefix = eq.substr(0, size_ret);
    CHECK(size_prefix.find("->") == std::string::npos);
    CHECK(size_prefix.find("_M_key") == std::string::npos);
    CHECK(size_prefix.find("_M_next") == std::string::npos);

    // Same size compares stored keys with ==. The walk stops when either
    // node is missing, so it does not read past the shorter set. A mismatch
    // returns false before the next node. No new key is constructed.
    const auto walk = eq.find("while (__a != nullptr && __b != nullptr)");
    const auto keys = eq.find("if (!(__a->_M_key == __b->_M_key))");
    const auto mismatch = eq.find("return false;", size_ret + 1);
    const auto next = eq.find("_M_next");
    const auto done = eq.find("return __a == nullptr && __b == nullptr;");
    REQUIRE(walk != std::string::npos);
    REQUIRE(keys != std::string::npos);
    REQUIRE(mismatch != std::string::npos);
    REQUIRE(next != std::string::npos);
    REQUIRE(done != std::string::npos);
    CHECK(size_ret < walk);
    CHECK(walk < keys);
    CHECK(keys < mismatch);
    CHECK(mismatch < next);
    CHECK(next < done);
    // The only true return is the empty path. A leftover node is not equal.
    CHECK(eq.find("return true;", empty_ret + 1) == std::string::npos);
    CHECK(eq.find("key_type") == std::string::npos);
    CHECK(eq.find("value_type") == std::string::npos);
    CHECK(eq.find("new ") == std::string::npos);
    CHECK(eq.find("_Node(") == std::string::npos);
    CHECK(eq.find("_M_comp") == std::string::npos);
    CHECK(eq.find("insert") == std::string::npos);
    CHECK(eq.find("erase") == std::string::npos);
    CHECK(eq.find("find(") == std::string::npos);
    CHECK(eq.find("contains") == std::string::npos);
    CHECK(eq.find("count") == std::string::npos);
    CHECK(eq.find("empty(") == std::string::npos);
    CHECK(eq.find("clear") == std::string::npos);
    CHECK(eq.find("swap") == std::string::npos);

    const auto ne = body_of(src, "operator!=(const flat_set& __x, const flat_set& __y)");
    REQUIRE(!ne.empty());
    CHECK(ne.find("return !(__x == __y);") != std::string::npos);
    CHECK(ne.find("_M_") == std::string::npos);
    CHECK(ne.find("return true") == std::string::npos);
    CHECK(ne.find("return false") == std::string::npos);
    CHECK(ne.find("new ") == std::string::npos);

    const auto sig = src.find("friend bool operator==(const flat_set& __x, const flat_set& __y)");
    REQUIRE(sig != std::string::npos);
    const auto note_from = src.rfind("Two empty sets compare equal.", sig);
    REQUIRE(note_from != std::string::npos);
    const auto note = src.substr(note_from, sig - note_from);
    CHECK(note.find("does not dereference a node") != std::string::npos);
    CHECK(note.find("does not read past the shorter set") != std::string::npos);
    CHECK(note.find("key equality") != std::string::npos);
    CHECK(note.find("does not construct a new key") != std::string::npos);
    CHECK(note.find("A mismatch returns false") != std::string::npos);
    CHECK(note.find("This is not PROVED and not CLEAN.") != std::string::npos);

    CHECK(src.find("pair<iterator, bool> insert(const value_type& __x)") != std::string::npos);
    CHECK(src.find("pair<iterator, bool> insert(value_type&& __x)") != std::string::npos);
    CHECK(src.find("size_type erase(const key_type& __x)") != std::string::npos);
    CHECK(src.find("iterator erase(iterator __pos)") != std::string::npos);
    CHECK(src.find("iterator find(const key_type& __x) const { return iterator(_M_find_node(__x)); }") !=
          std::string::npos);
    CHECK(src.find("bool contains(const key_type& __x) const") != std::string::npos);
    CHECK(src.find("size_type count(const key_type& __x) const { return contains(__x) ? 1 : 0; }") !=
          std::string::npos);
    CHECK(src.find("bool empty() const noexcept { return _M_head == nullptr; }") != std::string::npos);
    CHECK(src.find("void clear() noexcept") != std::string::npos);
    CHECK(src.find("void swap(flat_set& __o) noexcept") != std::string::npos);
}
