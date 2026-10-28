// The std::flat_map model header (src/prism/pir/models/cxx/flat_map) is a
// file. This test reads that file. It does not compile it and it does not
// run pir. A missing at() is a throw (or a __prism_check) and not a default
// mapped value. erase is described as freeing the node. contains is a
// lookup: a missing key is false, a present key is true, and that body
// does not allocate, insert, or change size. try_emplace on a present key
// returns false before it allocates or overwrites the mapped object. On a
// missing key the mapped object is constructed from the arguments in the
// new node, and that construction is not reached until the key is absent.

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
    CHECK(src.find("#include <set>") == std::string::npos);
    CHECK(src.find("#include <vector>") == std::string::npos);
    CHECK(src.find("#include <memory>") == std::string::npos);
    CHECK(src.find("#include <initializer_list>") == std::string::npos);
    CHECK(src.find("fail to compile") != std::string::npos);
    CHECK(src.find("pir falls back") != std::string::npos);

    // Both at() overloads. The miss throws (or is a check) and the only
    // value returned is the mapped object stored in a node.
    const std::string at = slice_from(src, "mapped_type& at(const key_type&", "bool contains(");
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
    CHECK(index.find("mapped_type{}") != std::string::npos);
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

    // contains used to be listed with the names that are not declared.
    CHECK(src.find("find, contains, count") == std::string::npos);
    CHECK(src.find("find, count, clear") != std::string::npos);

    // Lookup only. The const finder returns nullptr for a miss and does not
    // allocate, insert, or touch size. contains is that result as a bool.
    const std::string finder =
        slice_from(src, "const _Node* _M_find(const key_type& __k) const noexcept {", "template <typename _Uk");
    REQUIRE_FALSE(finder.empty());
    CHECK(finder.find("return nullptr") != std::string::npos);
    CHECK(finder.find("::operator new") == std::string::npos);
    CHECK(finder.find("_M_make") == std::string::npos);
    CHECK(finder.find("_M_link") == std::string::npos);
    CHECK(finder.find("_M_size") == std::string::npos);

    const std::string has =
        "bool contains(const key_type& __k) const { return _M_find(__k) != nullptr; }";
    const auto has_at = src.find(has);
    REQUIRE(has_at != std::string::npos);
    const auto note_at = has_at > 240 ? has_at - 240 : 0;
    const std::string note = src.substr(note_at, has_at - note_at);
    CHECK(note.find("Missing key returns false") != std::string::npos);
    CHECK(note.find("present key returns true") != std::string::npos);
    CHECK(note.find("allocates, inserts a node, or changes size") != std::string::npos);
    CHECK(note.find("::operator new") == std::string::npos);
    CHECK(note.find("_M_make") == std::string::npos);
    CHECK(note.find("_M_link") == std::string::npos);
    CHECK(note.find("_M_size") == std::string::npos);

    // try_emplace used to be listed with the names that are not declared.
    CHECK(src.find("insert ranges, try_emplace,") == std::string::npos);
    CHECK(src.find("try_emplace, erase(iterator)") == std::string::npos);

    // Present key returns false before any allocation. The mapped object is
    // not built on that path, and the stored object is not assigned.
    const std::string te =
        slice_from(src, "bool try_emplace(const key_type&", "bool try_emplace(key_type&&");
    REQUIRE_FALSE(te.empty());
    CHECK(te.find("if (_M_find(__k) != nullptr) return false;") != std::string::npos);
    CHECK(te.find("mapped_type{}") == std::string::npos);
    CHECK(te.find("mapped_type(") == std::string::npos);
    CHECK(te.find("::operator new") == std::string::npos);
    CHECK(te.find("_M_mapped") == std::string::npos);
    const auto te_miss = te.find("return false");
    const auto te_make = te.find("_M_emplace_args");
    REQUIRE(te_miss != std::string::npos);
    REQUIRE(te_make != std::string::npos);
    CHECK(te_miss < te_make);
    CHECK(te.find("return true") != std::string::npos);

    const std::string te_rv = slice_from(src, "bool try_emplace(key_type&&", "size_type erase(");
    REQUIRE_FALSE(te_rv.empty());
    CHECK(te_rv.find("if (_M_find(__k) != nullptr) return false;") != std::string::npos);
    CHECK(te_rv.find("mapped_type{}") == std::string::npos);
    CHECK(te_rv.find("mapped_type(") == std::string::npos);
    CHECK(te_rv.find("::operator new") == std::string::npos);
    CHECK(te_rv.find("_M_mapped") == std::string::npos);
    const auto rv_miss = te_rv.find("return false");
    const auto rv_make = te_rv.find("_M_emplace_args");
    REQUIRE(rv_miss != std::string::npos);
    REQUIRE(rv_make != std::string::npos);
    CHECK(rv_miss < rv_make);

    const auto te_fn = src.find("bool try_emplace(const key_type&");
    REQUIRE(te_fn != std::string::npos);
    const auto te_note_at = te_fn > 320 ? te_fn - 320 : 0;
    const std::string te_note = src.substr(te_note_at, te_fn - te_note_at);
    CHECK(te_note.find("return false") != std::string::npos);
    CHECK(te_note.find("Do not allocate") != std::string::npos);
    CHECK(te_note.find("do not overwrite") != std::string::npos);
    CHECK(te_note.find("known to be absent") != std::string::npos);

    // The new node constructs the mapped member from the argument pack.
    // There is no mapped temporary before that member.
    const std::string built =
        slice_from(src, "_Node* _M_emplace_args(", "_Node* _M_link(");
    REQUIRE_FALSE(built.empty());
    CHECK(built.find("::operator new") != std::string::npos);
    CHECK(built.find("mapped_type{}") == std::string::npos);
    CHECK(built.find("mapped_type(") == std::string::npos);
    CHECK(built.find("_Node{nullptr, static_cast<_Uk&&>(__k), static_cast<_Args&&>(__args)...}") !=
          std::string::npos);
    const std::string ctor =
        slice_from(src, "_Node(_Node* __next, _Uk&& __k, _Args&&... __args)", "_Node* _M_head");
    REQUIRE_FALSE(ctor.empty());
    CHECK(ctor.find("_M_mapped(static_cast<_Args&&>(__args)...)") != std::string::npos);
}
