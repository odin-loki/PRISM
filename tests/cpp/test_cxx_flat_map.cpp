// The std::flat_map model header (src/prism/pir/models/cxx/flat_map) is a
// file. This test reads that file. It does not compile it and it does not
// run pir. A missing at() is a throw (or a __prism_check) and not a default
// mapped value. erase is described as freeing the node. contains is a
// lookup: a missing key is false, a present key is true, and that body
// does not allocate, insert, or change size. count returns 1 when the key
// is present and 0 when it is missing. That body is the existing find: it
// does not allocate, insert, or overwrite the mapped object, and a missing
// key does not construct a mapped value. try_emplace on a present key
// returns false before it allocates or overwrites the mapped object. On a
// missing key the mapped object is constructed from the arguments in the
// new node, and that construction is not reached until the key is absent.
// empty() is true when there are no nodes and false when there is at least
// one. That body reads the existing size. It does not allocate, insert,
// erase, or construct a mapped value, and it does not call operator[] or
// at(). The comment states that this is not PROVED and not CLEAN.
// clear() calls the existing _M_clear. It does not allocate, does not
// insert, and does not construct a mapped value. An empty map does not
// call ::operator delete. After clear(), empty() is true and size() is 0.
// The comment states that this is not PROVED and not CLEAN. _M_clear still
// destroys each node, releases it with ::operator delete, and sets
// _M_size to 0. That delete stays inside the walk of a non-null head.
// swap exchanges the node pointer and the size. Two empty maps swap to
// empty and do not load a node. That path returns before a node is
// loaded. Swap does not allocate, free, or construct a key. Self-swap
// returns before any free, so it does not free the nodes. The comment
// states that this is not PROVED and not CLEAN. operator< is
// lexicographical order of the stored pairs along the node chain. Two
// empty maps compare not-less and do not load a node. An empty map is
// less than a non-empty map, and that path does not load the empty map's
// node. The walk stops at the first deciding pair and does not read past
// the shorter map. The comment states that this is not PROVED and not CLEAN.
// operator> returns __y < __x, using the existing operator<. Two empty
// maps compare not-greater and do not load a node. A non-empty map is
// greater than an empty map, and that path does not load the empty map.
// It does not return T{}. The comment states that this is not PROVED and
// not CLEAN. operator== is container equality: two empty maps compare
// equal before a node is loaded, different sizes compare unequal before a
// node is loaded, and otherwise each pair of __x is looked up in __y with
// the existing _M_find (the chain is link order, not key order) and its
// key and mapped value are compared with ==. operator!= is not declared:
// a != b is the C++20 rewrite of !(a == b). The comment states that this
// is not PROVED and not CLEAN.

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
    CHECK(src.find("#include <vector>") != std::string::npos);  // extract()/replace() containers
    CHECK(src.find("#include <memory>") == std::string::npos);
    CHECK(src.find("#include <initializer_list>") != std::string::npos);  // initializer_list constructors
    CHECK(src.find("fails to compile") != std::string::npos);
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
    // count used to be listed with the names that are not declared.
    CHECK(src.find("find, count, clear") == std::string::npos);
    // clear used to be listed with the names that are not declared.
    CHECK(src.find("iterators, find, clear,") == std::string::npos);
    // swap used to be listed with the names that are not declared.
    CHECK(src.find("Not declared: insert, insert_range, emplace, swap") == std::string::npos);
    CHECK(src.find("Not declared: insert, insert_range,") != std::string::npos);
    CHECK(src.find("emplace and the rest;") != std::string::npos);

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

    // count is the existing finder as 1 or 0. The body does not allocate,
    // insert, overwrite the mapped object, or construct a mapped value.
    const std::string cnt =
        "size_type count(const key_type& __k) const { return _M_find(__k) != nullptr ? 1 : 0; }";
    const auto cnt_at = src.find(cnt);
    REQUIRE(cnt_at != std::string::npos);
    const std::string cnt_body =
        slice_from(src, "size_type count(const key_type&", "bool try_emplace(const key_type&");
    REQUIRE_FALSE(cnt_body.empty());
    CHECK(cnt_body.find("_M_find(__k)") != std::string::npos);
    CHECK(cnt_body.find("!= nullptr ? 1 : 0") != std::string::npos);
    CHECK(cnt_body.find("::operator new") == std::string::npos);
    CHECK(cnt_body.find("_M_make") == std::string::npos);
    CHECK(cnt_body.find("_M_link") == std::string::npos);
    CHECK(cnt_body.find("_M_emplace_args") == std::string::npos);
    CHECK(cnt_body.find("_M_size") == std::string::npos);
    CHECK(cnt_body.find("mapped_type{}") == std::string::npos);
    CHECK(cnt_body.find("mapped_type(") == std::string::npos);
    CHECK(cnt_body.find("_M_mapped") == std::string::npos);
    CHECK(cnt_body.find("return {}") == std::string::npos);
    const auto cnt_note_at = cnt_at > 280 ? cnt_at - 280 : 0;
    const std::string cnt_note = src.substr(cnt_note_at, cnt_at - cnt_note_at);
    CHECK(cnt_note.find("returns 0") != std::string::npos);
    CHECK(cnt_note.find("does not construct a mapped value") != std::string::npos);
    CHECK(cnt_note.find("returns 1") != std::string::npos);
    CHECK(cnt_note.find("existing _M_find") != std::string::npos);
    CHECK(cnt_note.find("does not allocate") != std::string::npos);
    CHECK(cnt_note.find("does not insert") != std::string::npos);
    CHECK(cnt_note.find("does not overwrite the mapped object") != std::string::npos);

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

    // empty() is the existing node count. No nodes is true. At least one
    // node is false. The body does not allocate, insert, erase, construct a
    // mapped value, or call operator[] or at().
    const std::string emp =
        "bool empty() const noexcept { return _M_size == 0; }";
    const auto emp_at = src.find(emp);
    REQUIRE(emp_at != std::string::npos);
    const std::string emp_body = slice_from(src, "bool empty() const noexcept", "\n");
    REQUIRE_FALSE(emp_body.empty());
    CHECK(emp_body.find("return _M_size == 0") != std::string::npos);
    CHECK(emp_body.find("::operator new") == std::string::npos);
    CHECK(emp_body.find("_M_make") == std::string::npos);
    CHECK(emp_body.find("_M_link") == std::string::npos);
    CHECK(emp_body.find("_M_emplace_args") == std::string::npos);
    CHECK(emp_body.find("::operator delete") == std::string::npos);
    CHECK(emp_body.find("mapped_type{}") == std::string::npos);
    CHECK(emp_body.find("mapped_type(") == std::string::npos);
    CHECK(emp_body.find("_M_mapped") == std::string::npos);
    CHECK(emp_body.find("operator[]") == std::string::npos);
    CHECK(emp_body.find("at(") == std::string::npos);
    CHECK(emp_body.find("erase(") == std::string::npos);
    CHECK(emp_body.find("try_emplace") == std::string::npos);
    CHECK(emp_body.find("contains(") == std::string::npos);
    const std::string emp_note = slice_from(src, "True when there are no nodes", "bool empty() const noexcept");
    REQUIRE_FALSE(emp_note.empty());
    CHECK(emp_note.find("False when there is at least one") != std::string::npos);
    CHECK(emp_note.find("existing size") != std::string::npos);
    CHECK(emp_note.find("does not allocate") != std::string::npos);
    CHECK(emp_note.find("insert") != std::string::npos);
    CHECK(emp_note.find("erase") != std::string::npos);
    CHECK(emp_note.find("construct a mapped value") != std::string::npos);
    CHECK(emp_note.find("operator[]") != std::string::npos);
    CHECK(emp_note.find("at()") != std::string::npos);
    CHECK(emp_note.find("This is not PROVED and not CLEAN.") != std::string::npos);
    CHECK(emp_note.find("::operator new") == std::string::npos);
    CHECK(emp_note.find("_M_make") == std::string::npos);
    CHECK(emp_note.find("_M_link") == std::string::npos);
    CHECK(emp_note.find("mapped_type{}") == std::string::npos);

    // clear() is the existing _M_clear. The body does not allocate, insert,
    // or construct a mapped value. An empty map does not reach
    // ::operator delete: that call stays inside the non-null head walk.
    // After clear(), _M_size is 0, so empty() is true and size() is 0.
    const std::string clr = "void clear() noexcept { _M_clear(); }";
    const auto clr_at = src.find(clr);
    REQUIRE(clr_at != std::string::npos);
    CHECK(src.find(clr, clr_at + 1) == std::string::npos);
    const auto clr_pub = src.rfind("public:", clr_at);
    const auto mclear_at = src.find("void _M_clear() noexcept");
    REQUIRE(clr_pub != std::string::npos);
    REQUIRE(mclear_at != std::string::npos);
    CHECK(mclear_at < clr_pub);
    CHECK(clr_pub < clr_at);
    const auto emp_fn = src.find("bool empty() const noexcept { return _M_size == 0; }");
    REQUIRE(emp_fn != std::string::npos);
    CHECK(clr_at < emp_fn);

    const std::string clr_body = slice_from(src, "void clear() noexcept", "True when there are no nodes");
    REQUIRE_FALSE(clr_body.empty());
    CHECK(clr_body.find("_M_clear()") != std::string::npos);
    CHECK(clr_body.find("::operator new") == std::string::npos);
    CHECK(clr_body.find("_M_make") == std::string::npos);
    CHECK(clr_body.find("_M_link") == std::string::npos);
    CHECK(clr_body.find("_M_emplace_args") == std::string::npos);
    CHECK(clr_body.find("::operator delete") == std::string::npos);
    CHECK(clr_body.find("mapped_type{}") == std::string::npos);
    CHECK(clr_body.find("mapped_type(") == std::string::npos);
    CHECK(clr_body.find("_M_mapped") == std::string::npos);
    CHECK(clr_body.find("operator[]") == std::string::npos);
    CHECK(clr_body.find("at(") == std::string::npos);
    CHECK(clr_body.find("erase(") == std::string::npos);
    CHECK(clr_body.find("try_emplace") == std::string::npos);
    CHECK(clr_body.find("contains(") == std::string::npos);
    CHECK(clr_body.find("while") == std::string::npos);

    const std::string clr_note = slice_from(src, "Calls the existing _M_clear.", "void clear() noexcept");
    REQUIRE_FALSE(clr_note.empty());
    CHECK(clr_note.find("does not allocate") != std::string::npos);
    CHECK(clr_note.find("does not insert") != std::string::npos);
    CHECK(clr_note.find("does not construct a mapped value") != std::string::npos);
    CHECK(clr_note.find("An empty map does not call ::operator delete") != std::string::npos);
    CHECK(clr_note.find("empty() is true") != std::string::npos);
    CHECK(clr_note.find("size() is 0") != std::string::npos);
    CHECK(clr_note.find("This is not PROVED and not CLEAN.") != std::string::npos);
    CHECK(clr_note.find("::operator new") == std::string::npos);
    CHECK(clr_note.find("_M_make") == std::string::npos);
    CHECK(clr_note.find("_M_link") == std::string::npos);
    CHECK(clr_note.find("mapped_type{}") == std::string::npos);

    // _M_clear's body is the walk that already existed. Each node is
    // destroyed and released, then the size is 0. The delete is not
    // reached when the head is null.
    const std::string mclear = slice_from(src, "void _M_clear() noexcept {", "flat_map() noexcept");
    REQUIRE_FALSE(mclear.empty());
    const auto walk = mclear.find("while (_M_head != nullptr)");
    const auto destroyed = mclear.find("__n->~_Node()");
    const auto deleted = mclear.find("::operator delete(__n)");
    const auto zero = mclear.find("_M_size = 0");
    REQUIRE(walk != std::string::npos);
    REQUIRE(destroyed != std::string::npos);
    REQUIRE(deleted != std::string::npos);
    REQUIRE(zero != std::string::npos);
    CHECK(walk < destroyed);
    CHECK(destroyed < deleted);
    CHECK(deleted < zero);
    CHECK(mclear.find("::operator delete", deleted + 1) == std::string::npos);
    CHECK(mclear.find("::operator new") == std::string::npos);
    CHECK(mclear.find("_M_make") == std::string::npos);
    CHECK(mclear.find("_M_link") == std::string::npos);
    CHECK(mclear.find("_M_emplace_args") == std::string::npos);
    CHECK(mclear.find("mapped_type{}") == std::string::npos);
    CHECK(mclear.find("mapped_type(") == std::string::npos);

    const std::string omitted = slice_from(src, "Not declared:", "a use of an omitted name");
    REQUIRE_FALSE(omitted.empty());
    CHECK(omitted.find("empty") == std::string::npos);
    CHECK(omitted.find("clear") == std::string::npos);
    CHECK(omitted.find("swap") == std::string::npos);
    CHECK(omitted.find("operator<") == std::string::npos);
}

TEST_CASE("cxx flat_map model: swap exchanges the node pointer and the size") {
    const fs::path path = header();
    REQUIRE(fs::is_regular_file(path));
    CHECK_FALSE(fs::is_directory(path));
    const std::string src = slurp(path);
    REQUIRE_FALSE(src.empty());

    const std::string sig = "void swap(flat_map& __o) noexcept";
    const auto swap_at = src.find(sig);
    REQUIRE(swap_at != std::string::npos);
    CHECK(src.find(sig, swap_at + sig.size()) == std::string::npos);

    const auto emp_fn = src.find("bool empty() const noexcept { return _M_size == 0; }");
    const auto session = src.find("Session 5:");
    const auto pub = src.rfind("public:", swap_at);
    REQUIRE(emp_fn != std::string::npos);
    REQUIRE(session != std::string::npos);
    REQUIRE(pub != std::string::npos);
    CHECK(pub < emp_fn);
    CHECK(emp_fn < swap_at);
    CHECK(swap_at < session);

    // Self-swap returns before any exchange. Two empty maps return before a
    // node is loaded. The exchange then writes the node pointer and the size
    // both ways. The body does not allocate, free, or construct a key.
    const std::string body = slice_from(src, sig, "Session 5:");
    REQUIRE_FALSE(body.empty());
    const auto self = body.find("if (this == &__o) return;");
    const auto self_ret = body.find("return;");
    REQUIRE(self != std::string::npos);
    REQUIRE(self_ret != std::string::npos);
    CHECK(self <= self_ret);
    const auto self_prefix = body.substr(0, self_ret);
    CHECK(self_prefix.find("::operator delete") == std::string::npos);
    CHECK(self_prefix.find("_M_clear") == std::string::npos);
    CHECK(self_prefix.find("->") == std::string::npos);
    CHECK(self_prefix.find("_M_head =") == std::string::npos);
    CHECK(self_prefix.find("_M_size =") == std::string::npos);

    const auto heads = body.find("if (_M_head == nullptr && __o._M_head == nullptr) return;");
    const auto empty_ret = body.find("return;", self_ret + 1);
    REQUIRE(heads != std::string::npos);
    REQUIRE(empty_ret != std::string::npos);
    CHECK(self_ret < heads);
    CHECK(heads < empty_ret);
    const auto empty_prefix = body.substr(0, empty_ret);
    CHECK(empty_prefix.find("->") == std::string::npos);
    CHECK(empty_prefix.find("_M_key") == std::string::npos);
    CHECK(empty_prefix.find("_M_mapped") == std::string::npos);
    CHECK(empty_prefix.find("_M_next") == std::string::npos);
    CHECK(empty_prefix.find("*") == std::string::npos);
    CHECK(empty_prefix.find("::operator delete") == std::string::npos);
    CHECK(empty_prefix.find("_M_clear") == std::string::npos);
    CHECK(empty_prefix.find("::operator new") == std::string::npos);

    const auto save_h = body.find("_Node* __h = _M_head;");
    const auto save_s = body.find("size_type __s = _M_size;");
    const auto set_h = body.find("_M_head = __o._M_head;");
    const auto set_s = body.find("_M_size = __o._M_size;");
    const auto back_h = body.find("__o._M_head = __h;");
    const auto back_s = body.find("__o._M_size = __s;");
    REQUIRE(save_h != std::string::npos);
    REQUIRE(save_s != std::string::npos);
    REQUIRE(set_h != std::string::npos);
    REQUIRE(set_s != std::string::npos);
    REQUIRE(back_h != std::string::npos);
    REQUIRE(back_s != std::string::npos);
    CHECK(empty_ret < save_h);
    CHECK(save_h < save_s);
    CHECK(save_s < set_h);
    CHECK(set_h < set_s);
    CHECK(set_s < back_h);
    CHECK(back_h < back_s);

    CHECK(body.find("->") == std::string::npos);
    CHECK(body.find("_M_next") == std::string::npos);
    CHECK(body.find("_M_key") == std::string::npos);
    CHECK(body.find("_M_mapped") == std::string::npos);
    CHECK(body.find("key_type") == std::string::npos);
    CHECK(body.find("mapped_type") == std::string::npos);
    CHECK(body.find("::operator new") == std::string::npos);
    CHECK(body.find("::operator delete") == std::string::npos);
    CHECK(body.find("_M_clear") == std::string::npos);
    CHECK(body.find("_M_make") == std::string::npos);
    CHECK(body.find("_M_link") == std::string::npos);
    CHECK(body.find("_M_emplace_args") == std::string::npos);
    CHECK(body.find("new ") == std::string::npos);
    CHECK(body.find("~") == std::string::npos);
    CHECK(body.find("operator[]") == std::string::npos);
    CHECK(body.find("at(") == std::string::npos);
    CHECK(body.find("erase") == std::string::npos);
    CHECK(body.find("contains") == std::string::npos);
    CHECK(body.find("try_emplace") == std::string::npos);
    CHECK(body.find("clear(") == std::string::npos);
    CHECK(body.find("empty(") == std::string::npos);
    CHECK(body.find("operator==") == std::string::npos);

    const auto note_from = src.rfind("Exchanges the node pointer", swap_at);
    REQUIRE(note_from != std::string::npos);
    const std::string note = src.substr(note_from, swap_at - note_from);
    CHECK(note.find("and the size") != std::string::npos);
    CHECK(note.find("Two empty maps swap to empty") != std::string::npos);
    CHECK(note.find("do not load a node") != std::string::npos);
    CHECK(note.find("does not allocate, free, or construct a key") != std::string::npos);
    CHECK(note.find("Self-swap does not free the nodes") != std::string::npos);
    CHECK(note.find("This is not PROVED and not CLEAN.") != std::string::npos);
    CHECK(note.find("::operator new") == std::string::npos);
    CHECK(note.find("::operator delete") == std::string::npos);
    CHECK(note.find("_M_make") == std::string::npos);
    CHECK(note.find("mapped_type{}") == std::string::npos);

    CHECK(src.find("mapped_type& operator[](const key_type& __k)") != std::string::npos);
    CHECK(src.find("mapped_type& at(const key_type& __k)") != std::string::npos);
    CHECK(src.find("bool contains(const key_type& __k) const { return _M_find(__k) != nullptr; }") !=
          std::string::npos);
    CHECK(src.find(
              "size_type count(const key_type& __k) const { return _M_find(__k) != nullptr ? 1 : 0; }") !=
          std::string::npos);
    CHECK(src.find("bool try_emplace(const key_type& __k, _Args&&... __args)") != std::string::npos);
    CHECK(src.find("size_type erase(const key_type& __k)") != std::string::npos);
    CHECK(src.find("size_type size() const noexcept { return _M_size; }") != std::string::npos);
    CHECK(src.find("void clear() noexcept { _M_clear(); }") != std::string::npos);
    CHECK(src.find("bool empty() const noexcept { return _M_size == 0; }") != std::string::npos);
}

TEST_CASE("cxx flat_map model: less compares stored keys and mapped values") {
    const fs::path path = header();
    REQUIRE(fs::is_regular_file(path));
    CHECK_FALSE(fs::is_directory(path));
    const std::string src = slurp(path);
    REQUIRE_FALSE(src.empty());

    CHECK(src.find("#include <memory>") == std::string::npos);
    CHECK(src.find("#include <algorithm>") == std::string::npos);
    CHECK(src.find("operator<=(const flat_map&") == std::string::npos);
    CHECK(src.find("operator>=(const flat_map&") == std::string::npos);
    CHECK(src.find("operator<=>") == std::string::npos);

    const std::string lt_sig = "friend bool operator<(const flat_map& __x, const flat_map& __y)";
    const auto lt_at = src.find(lt_sig);
    REQUIRE(lt_at != std::string::npos);
    CHECK(src.find(lt_sig, lt_at + lt_sig.size()) == std::string::npos);

    const auto swap_at = src.find("void swap(flat_map& __o) noexcept");
    REQUIRE(swap_at != std::string::npos);
    CHECK(swap_at < lt_at);

    // Two empty maps return false before a node is loaded. An empty map
    // is less than a non-empty map before the empty map's node is loaded.
    // The walk then compares stored keys and mapped values with < and does
    // not read a node past the shorter map. A deciding pair returns before
    // the next node. No T{} is returned.
    const std::string lt = slice_from(src, lt_sig, "Returns __y < __x, using the existing operator<.");
    REQUIRE_FALSE(lt.empty());
    const auto both = lt.find("if (__x._M_head == nullptr && __y._M_head == nullptr)");
    const auto both_ret = lt.find("return false;");
    REQUIRE(both != std::string::npos);
    REQUIRE(both_ret != std::string::npos);
    CHECK(both < both_ret);
    const auto empty_lt = lt.find("if (__x._M_head == nullptr && __y._M_head != nullptr)");
    const auto empty_ret = lt.find("return true;");
    REQUIRE(empty_lt != std::string::npos);
    REQUIRE(empty_ret != std::string::npos);
    CHECK(both_ret < empty_lt);
    CHECK(empty_lt < empty_ret);
    const auto filled = lt.find("if (__y._M_head == nullptr && __x._M_head != nullptr)");
    const auto filled_ret = lt.find("return false;", both_ret + 1);
    REQUIRE(filled != std::string::npos);
    REQUIRE(filled_ret != std::string::npos);
    CHECK(empty_ret < filled);
    CHECK(filled < filled_ret);

    const auto walk_at = lt.find("const _Node* __a = __x._M_head;");
    REQUIRE(walk_at != std::string::npos);
    CHECK(filled_ret < walk_at);
    const auto before_walk = lt.substr(0, walk_at);
    CHECK(before_walk.find("->") == std::string::npos);
    CHECK(before_walk.find("_M_key") == std::string::npos);
    CHECK(before_walk.find("_M_mapped") == std::string::npos);
    CHECK(before_walk.find("_M_next") == std::string::npos);
    CHECK(before_walk.find("*") == std::string::npos);
    CHECK(before_walk.find("_M_size") == std::string::npos);

    const auto walk = lt.find("while (__a != nullptr && __b != nullptr)");
    const auto klt = lt.find("__a->_M_key < __b->_M_key");
    const auto kgt = lt.find("__b->_M_key < __a->_M_key");
    const auto mlt = lt.find("__a->_M_mapped < __b->_M_mapped");
    const auto mgt = lt.find("__b->_M_mapped < __a->_M_mapped");
    const auto kret = lt.find("return true;", empty_ret + 1);
    const auto kfals = lt.find("return false;", filled_ret + 1);
    const auto mret = lt.find("return true;", kret + 1);
    const auto mfals = lt.find("return false;", kfals + 1);
    const auto next_a = lt.find("__a = __a->_M_next;");
    const auto next_b = lt.find("__b = __b->_M_next;");
    const auto done = lt.find("return __a == nullptr && __b != nullptr;");
    REQUIRE(walk != std::string::npos);
    REQUIRE(klt != std::string::npos);
    REQUIRE(kgt != std::string::npos);
    REQUIRE(mlt != std::string::npos);
    REQUIRE(mgt != std::string::npos);
    REQUIRE(kret != std::string::npos);
    REQUIRE(kfals != std::string::npos);
    REQUIRE(mret != std::string::npos);
    REQUIRE(mfals != std::string::npos);
    REQUIRE(next_a != std::string::npos);
    REQUIRE(next_b != std::string::npos);
    REQUIRE(done != std::string::npos);
    CHECK(walk_at < walk);
    CHECK(walk < klt);
    CHECK(klt < kret);
    CHECK(kret < kgt);
    CHECK(kgt < kfals);
    CHECK(kfals < mlt);
    CHECK(mlt < mret);
    CHECK(mret < mgt);
    CHECK(mgt < mfals);
    CHECK(mfals < next_a);
    CHECK(next_a < next_b);
    CHECK(next_b < done);
    CHECK(lt.find("_M_key ==") == std::string::npos);
    CHECK(lt.find("_M_mapped ==") == std::string::npos);
    CHECK(lt.find("_M_size") == std::string::npos);
    CHECK(lt.find("T{}") == std::string::npos);
    CHECK(lt.find("return {}") == std::string::npos);
    CHECK(lt.find("mapped_type{}") == std::string::npos);
    CHECK(lt.find("mapped_type(") == std::string::npos);
    CHECK(lt.find("key_type") == std::string::npos);
    CHECK(lt.find("mapped_type") == std::string::npos);
    CHECK(lt.find("new ") == std::string::npos);
    CHECK(lt.find("::operator new") == std::string::npos);
    CHECK(lt.find("::operator delete") == std::string::npos);
    CHECK(lt.find("_M_make") == std::string::npos);
    CHECK(lt.find("_M_link") == std::string::npos);
    CHECK(lt.find("_M_emplace_args") == std::string::npos);
    CHECK(lt.find("operator[]") == std::string::npos);
    CHECK(lt.find("at(") == std::string::npos);
    CHECK(lt.find("erase") == std::string::npos);
    CHECK(lt.find("contains") == std::string::npos);
    CHECK(lt.find("try_emplace") == std::string::npos);
    CHECK(lt.find("count") == std::string::npos);
    CHECK(lt.find("empty(") == std::string::npos);
    CHECK(lt.find("clear") == std::string::npos);
    CHECK(lt.find("swap") == std::string::npos);
    CHECK(lt.find("operator==") == std::string::npos);
    CHECK(lt.find("size()") == std::string::npos);

    const auto note_from = src.rfind("Two empty maps compare not-less", lt_at);
    REQUIRE(note_from != std::string::npos);
    const std::string note = src.substr(note_from, lt_at - note_from);
    CHECK(note.find("do not load a node") != std::string::npos);
    CHECK(note.find("An empty map is less than a non-empty map") != std::string::npos);
    CHECK(note.find("does not load the empty map's node") != std::string::npos);
    CHECK(note.find("lexicographically") != std::string::npos);
    CHECK(note.find("stored keys and mapped values") != std::string::npos);
    CHECK(note.find("do not read past the shorter map") != std::string::npos);
    CHECK(note.find("with <") != std::string::npos);
    CHECK(note.find("does not return T{}") != std::string::npos);
    CHECK(note.find("This is not PROVED and not CLEAN.") != std::string::npos);

    CHECK(src.find("mapped_type& operator[](const key_type& __k)") != std::string::npos);
    CHECK(src.find("mapped_type& at(const key_type& __k)") != std::string::npos);
    CHECK(src.find("bool contains(const key_type& __k) const { return _M_find(__k) != nullptr; }") !=
          std::string::npos);
    CHECK(src.find(
              "size_type count(const key_type& __k) const { return _M_find(__k) != nullptr ? 1 : 0; }") !=
          std::string::npos);
    CHECK(src.find("bool try_emplace(const key_type& __k, _Args&&... __args)") != std::string::npos);
    CHECK(src.find("bool try_emplace(key_type&& __k, _Args&&... __args)") != std::string::npos);
    CHECK(src.find("size_type erase(const key_type& __k)") != std::string::npos);
    CHECK(src.find("size_type size() const noexcept { return _M_size; }") != std::string::npos);
    CHECK(src.find("void clear() noexcept { _M_clear(); }") != std::string::npos);
    CHECK(src.find("bool empty() const noexcept { return _M_size == 0; }") != std::string::npos);
    CHECK(src.find("void swap(flat_map& __o) noexcept") != std::string::npos);
}

TEST_CASE("cxx flat_map model: greater is operator< with the arguments swapped") {
    const fs::path path = header();
    REQUIRE(fs::is_regular_file(path));
    CHECK_FALSE(fs::is_directory(path));
    const std::string src = slurp(path);
    REQUIRE_FALSE(src.empty());

    CHECK(src.find("#include <memory>") == std::string::npos);
    CHECK(src.find("#include <algorithm>") == std::string::npos);
    CHECK(src.find("operator<=") == std::string::npos);
    CHECK(src.find("operator>=") == std::string::npos);
    CHECK(src.find("operator<=>") == std::string::npos);

    const std::string gt_sig = "friend bool operator>(const flat_map& __x, const flat_map& __y)";
    const auto gt_at = src.find(gt_sig);
    REQUIRE(gt_at != std::string::npos);
    CHECK(src.find(gt_sig, gt_at + gt_sig.size()) == std::string::npos);

    const std::string lt_sig = "friend bool operator<(const flat_map& __x, const flat_map& __y)";
    const auto lt_at = src.find(lt_sig);
    const auto swap_at = src.find("void swap(flat_map& __o) noexcept");
    REQUIRE(lt_at != std::string::npos);
    REQUIRE(swap_at != std::string::npos);
    CHECK(swap_at < lt_at);
    CHECK(lt_at < gt_at);
    // Container operator== is declared once, before operator<. operator!=
    // is not declared: a != b is the C++20 rewrite of !(a == b).
    const auto eq_at = src.find("friend bool operator==(const flat_map& __x, const flat_map& __y)");
    REQUIRE(eq_at != std::string::npos);
    CHECK(src.find("operator==(const flat_map&", eq_at + std::string("friend bool operator==").size()) ==
          std::string::npos);
    CHECK(eq_at < lt_at);
    CHECK(src.find("operator!=(const flat_map&") == std::string::npos);

    // The body is the existing operator< with the arguments swapped. It does
    // not load a node itself and it does not return T{}.
    const std::string gt = slice_from(src, gt_sig, "};");
    REQUIRE_FALSE(gt.empty());
    const auto ret = gt.find("return __y < __x;");
    REQUIRE(ret != std::string::npos);
    CHECK(gt.find("return __y < __x;", ret + 1) == std::string::npos);
    CHECK(gt.find("return true") == std::string::npos);
    CHECK(gt.find("return false") == std::string::npos);
    CHECK(gt.find("return {}") == std::string::npos);
    CHECK(gt.find("T{}") == std::string::npos);
    CHECK(gt.find("mapped_type{}") == std::string::npos);
    CHECK(gt.find("mapped_type(") == std::string::npos);
    CHECK(gt.find("key_type") == std::string::npos);
    CHECK(gt.find("mapped_type") == std::string::npos);
    CHECK(gt.find("_M_") == std::string::npos);
    CHECK(gt.find("->") == std::string::npos);
    CHECK(gt.find("*") == std::string::npos);
    CHECK(gt.find("new ") == std::string::npos);
    CHECK(gt.find("::operator new") == std::string::npos);
    CHECK(gt.find("::operator delete") == std::string::npos);
    CHECK(gt.find("operator==") == std::string::npos);
    CHECK(gt.find("operator[]") == std::string::npos);
    CHECK(gt.find("at(") == std::string::npos);
    CHECK(gt.find("erase") == std::string::npos);
    CHECK(gt.find("contains") == std::string::npos);
    CHECK(gt.find("try_emplace") == std::string::npos);
    CHECK(gt.find("count") == std::string::npos);
    CHECK(gt.find("empty(") == std::string::npos);
    CHECK(gt.find("clear") == std::string::npos);
    CHECK(gt.find("swap") == std::string::npos);
    CHECK(gt.find("size()") == std::string::npos);

    // Two empty maps: operator< returns false before a node is loaded, so
    // __y < __x is not-greater and does not load a node. A non-empty map
    // compared with an empty map calls operator< with the empty map on the
    // left, which returns true before that empty map's node is loaded.
    const std::string lt = slice_from(src, lt_sig, "Returns __y < __x, using the existing operator<.");
    REQUIRE_FALSE(lt.empty());
    const auto both = lt.find("if (__x._M_head == nullptr && __y._M_head == nullptr)");
    const auto both_ret = lt.find("return false;");
    const auto empty_lt = lt.find("if (__x._M_head == nullptr && __y._M_head != nullptr)");
    const auto empty_ret = lt.find("return true;");
    const auto filled = lt.find("if (__y._M_head == nullptr && __x._M_head != nullptr)");
    const auto filled_ret = lt.find("return false;", both_ret + 1);
    const auto walk_at = lt.find("const _Node* __a = __x._M_head;");
    REQUIRE(both != std::string::npos);
    REQUIRE(both_ret != std::string::npos);
    REQUIRE(empty_lt != std::string::npos);
    REQUIRE(empty_ret != std::string::npos);
    REQUIRE(filled != std::string::npos);
    REQUIRE(filled_ret != std::string::npos);
    REQUIRE(walk_at != std::string::npos);
    CHECK(both < both_ret);
    CHECK(both_ret < empty_lt);
    CHECK(empty_lt < empty_ret);
    CHECK(empty_ret < filled);
    CHECK(filled < filled_ret);
    CHECK(filled_ret < walk_at);
    const auto before_walk = lt.substr(0, walk_at);
    CHECK(before_walk.find("->") == std::string::npos);
    CHECK(before_walk.find("_M_key") == std::string::npos);
    CHECK(before_walk.find("_M_mapped") == std::string::npos);
    CHECK(before_walk.find("_M_next") == std::string::npos);
    CHECK(before_walk.find("*") == std::string::npos);

    const auto note_from = src.rfind("Returns __y < __x, using the existing operator<.", gt_at);
    REQUIRE(note_from != std::string::npos);
    const std::string note = src.substr(note_from, gt_at - note_from);
    CHECK(note.find("Two empty maps compare not-greater") != std::string::npos);
    CHECK(note.find("do not load a node") != std::string::npos);
    CHECK(note.find("A non-empty map is greater than an empty map") != std::string::npos);
    CHECK(note.find("does not load the empty map") != std::string::npos);
    CHECK(note.find("does not return T{}") != std::string::npos);
    CHECK(note.find("This is not PROVED and not CLEAN.") != std::string::npos);

    CHECK(src.find("mapped_type& operator[](const key_type& __k)") != std::string::npos);
    CHECK(src.find("mapped_type& at(const key_type& __k)") != std::string::npos);
    CHECK(src.find("bool contains(const key_type& __k) const { return _M_find(__k) != nullptr; }") !=
          std::string::npos);
    CHECK(src.find(
              "size_type count(const key_type& __k) const { return _M_find(__k) != nullptr ? 1 : 0; }") !=
          std::string::npos);
    CHECK(src.find("bool try_emplace(const key_type& __k, _Args&&... __args)") != std::string::npos);
    CHECK(src.find("bool try_emplace(key_type&& __k, _Args&&... __args)") != std::string::npos);
    CHECK(src.find("size_type erase(const key_type& __k)") != std::string::npos);
    CHECK(src.find("size_type size() const noexcept { return _M_size; }") != std::string::npos);
    CHECK(src.find("void clear() noexcept { _M_clear(); }") != std::string::npos);
    CHECK(src.find("bool empty() const noexcept { return _M_size == 0; }") != std::string::npos);
    CHECK(src.find("void swap(flat_map& __o) noexcept") != std::string::npos);
    CHECK(src.find(lt_sig) != std::string::npos);
}

TEST_CASE("cxx flat_map model: equality looks each pair up so link order does not matter") {
    const fs::path path = header();
    REQUIRE(fs::is_regular_file(path));
    CHECK_FALSE(fs::is_directory(path));
    const std::string src = slurp(path);
    REQUIRE_FALSE(src.empty());

    CHECK(src.find("#include <memory>") == std::string::npos);
    CHECK(src.find("#include <algorithm>") == std::string::npos);
    CHECK(src.find("operator!=(const flat_map&") == std::string::npos);
    CHECK(src.find("operator<=>") == std::string::npos);

    const std::string eq_sig = "friend bool operator==(const flat_map& __x, const flat_map& __y)";
    const auto eq_at = src.find(eq_sig);
    REQUIRE(eq_at != std::string::npos);
    CHECK(src.find(eq_sig, eq_at + eq_sig.size()) == std::string::npos);
    const auto swap_at = src.find("void swap(flat_map& __o) noexcept");
    const auto lt_at = src.find("friend bool operator<(const flat_map& __x, const flat_map& __y)");
    REQUIRE(swap_at != std::string::npos);
    REQUIRE(lt_at != std::string::npos);
    CHECK(swap_at < eq_at);
    CHECK(eq_at < lt_at);

    const std::string eq = slice_from(src, eq_sig, "Two empty maps compare not-less");
    REQUIRE_FALSE(eq.empty());

    // Two empty maps return true before a node is loaded.
    const auto heads = eq.find("if (__x._M_head == nullptr && __y._M_head == nullptr)");
    const auto empty_ret = eq.find("return true;");
    REQUIRE(heads != std::string::npos);
    REQUIRE(empty_ret != std::string::npos);
    CHECK(heads < empty_ret);
    const auto empty_prefix = eq.substr(0, empty_ret);
    CHECK(empty_prefix.find("->") == std::string::npos);
    CHECK(empty_prefix.find("_M_key") == std::string::npos);
    CHECK(empty_prefix.find("_M_mapped") == std::string::npos);
    CHECK(empty_prefix.find("_M_next") == std::string::npos);
    CHECK(empty_prefix.find("_M_find") == std::string::npos);

    // Different sizes return false before a node is loaded.
    const auto sizes = eq.find("if (__x._M_size != __y._M_size)");
    const auto size_ret = eq.find("return false;");
    REQUIRE(sizes != std::string::npos);
    REQUIRE(size_ret != std::string::npos);
    CHECK(empty_ret < sizes);
    CHECK(sizes < size_ret);
    const auto size_prefix = eq.substr(0, size_ret);
    CHECK(size_prefix.find("->") == std::string::npos);
    CHECK(size_prefix.find("_M_key") == std::string::npos);
    CHECK(size_prefix.find("_M_mapped") == std::string::npos);
    CHECK(size_prefix.find("_M_next") == std::string::npos);
    CHECK(size_prefix.find("_M_find") == std::string::npos);

    // The walk is over __x only. Each pair is looked up in __y by key, not
    // paired with __y's node at the same position, because the chain is in
    // link order. A missing key returns false. The stored key and mapped
    // value are compared with ==, and a mismatch returns false.
    const auto walk = eq.find("for (const _Node* __a = __x._M_head; __a != nullptr; __a = __a->_M_next)");
    const auto look = eq.find("const _Node* __b = __y._M_find(__a->_M_key);");
    const auto miss = eq.find("if (__b == nullptr)");
    const auto miss_ret = eq.find("return false;", size_ret + 1);
    const auto keys = eq.find("__a->_M_key == __b->_M_key");
    const auto mapped = eq.find("__a->_M_mapped == __b->_M_mapped");
    const auto bad_ret = eq.find("return false;", miss_ret + 1);
    const auto done = eq.find("return true;", empty_ret + 1);
    REQUIRE(walk != std::string::npos);
    REQUIRE(look != std::string::npos);
    REQUIRE(miss != std::string::npos);
    REQUIRE(miss_ret != std::string::npos);
    REQUIRE(keys != std::string::npos);
    REQUIRE(mapped != std::string::npos);
    REQUIRE(bad_ret != std::string::npos);
    REQUIRE(done != std::string::npos);
    CHECK(size_ret < walk);
    CHECK(walk < look);
    CHECK(look < miss);
    CHECK(miss < miss_ret);
    CHECK(miss_ret < keys);
    CHECK(keys < mapped);
    CHECK(mapped < bad_ret);
    CHECK(bad_ret < done);
    CHECK(eq.find("return false;", bad_ret + 1) == std::string::npos);
    CHECK(eq.find("return true;", done + 1) == std::string::npos);
    CHECK(eq.find("__b = __b->_M_next") == std::string::npos);
    CHECK(eq.find("__y._M_head;") == std::string::npos);
    CHECK(eq.find("key_type") == std::string::npos);
    CHECK(eq.find("mapped_type") == std::string::npos);
    CHECK(eq.find("T{}") == std::string::npos);
    CHECK(eq.find("new ") == std::string::npos);
    CHECK(eq.find("_Node(") == std::string::npos);
    CHECK(eq.find("::operator new") == std::string::npos);
    CHECK(eq.find("::operator delete") == std::string::npos);
    CHECK(eq.find("_M_make") == std::string::npos);
    CHECK(eq.find("_M_link") == std::string::npos);
    CHECK(eq.find("_M_emplace_args") == std::string::npos);
    CHECK(eq.find("operator[]") == std::string::npos);
    CHECK(eq.find("at(") == std::string::npos);
    CHECK(eq.find("erase") == std::string::npos);
    CHECK(eq.find("contains") == std::string::npos);
    CHECK(eq.find("try_emplace") == std::string::npos);
    CHECK(eq.find("count") == std::string::npos);
    CHECK(eq.find("empty(") == std::string::npos);
    CHECK(eq.find("clear") == std::string::npos);
    CHECK(eq.find("insert") == std::string::npos);
    CHECK(eq.find("operator<") == std::string::npos);

    const auto note_from = src.rfind("Container equality, not iterator equality.", eq_at);
    REQUIRE(note_from != std::string::npos);
    const std::string note = src.substr(note_from, eq_at - note_from);
    CHECK(note.find("Two empty maps compare equal") != std::string::npos);
    CHECK(note.find("does not dereference a node") != std::string::npos);
    CHECK(note.find("does not read past the shorter map") != std::string::npos);
    CHECK(note.find("link order, not key order") != std::string::npos);
    CHECK(note.find("not paired by position") != std::string::npos);
    CHECK(note.find("_M_find") != std::string::npos);
    CHECK(note.find("mapped values are compared with ==") != std::string::npos);
    CHECK(note.find("inserted in a different order compare equal") != std::string::npos);
    CHECK(note.find("A missing key or a mismatch returns false") != std::string::npos);
    CHECK(note.find("does not construct a new key") != std::string::npos);
    CHECK(note.find("operator!= is not declared") != std::string::npos);
    CHECK(note.find("This is not PROVED and not CLEAN.") != std::string::npos);

    const std::string declared = slice_from(src, "Declared besides the above:", "Not declared:");
    REQUIRE_FALSE(declared.empty());
    CHECK(declared.find("operator==") != std::string::npos);
    CHECK(declared.find("Container equality and") == std::string::npos);

    CHECK(src.find("const _Node* _M_find(const key_type& __k) const noexcept {") != std::string::npos);
    CHECK(src.find("mapped_type& operator[](const key_type& __k)") != std::string::npos);
    CHECK(src.find("mapped_type& at(const key_type& __k)") != std::string::npos);
    CHECK(src.find("bool contains(const key_type& __k) const { return _M_find(__k) != nullptr; }") !=
          std::string::npos);
    CHECK(src.find("size_type erase(const key_type& __k)") != std::string::npos);
    CHECK(src.find("size_type size() const noexcept { return _M_size; }") != std::string::npos);
    CHECK(src.find("bool empty() const noexcept { return _M_size == 0; }") != std::string::npos);
}
