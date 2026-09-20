"""Mechanical fixes for the Python-translated checkers_cxx.cpp."""
from __future__ import annotations

from pathlib import Path

p = Path(__file__).resolve().parents[1] / "src" / "prism" / "checkers_cxx.cpp"
s = p.read_text(encoding="utf-8")

HELPERS = r'''
int str_index(const std::string& s, std::string_view sub) {
    auto pos = s.find(sub);
    if (pos == std::string::npos) throw std::out_of_range("index");
    return static_cast<int>(pos);
}
int str_index(std::string_view s, std::string_view sub) {
    auto pos = s.find(sub);
    if (pos == std::string_view::npos) throw std::out_of_range("index");
    return static_cast<int>(pos);
}

template <class T>
std::vector<T> to_vector(const std::vector<T>& v) { return v; }
template <class K, class V>
std::vector<std::pair<K, V>> to_vector(const std::unordered_map<K, V>& m) {
    return {m.begin(), m.end()};
}
template <class K, class V>
std::vector<std::pair<K, V>> to_vector(const std::map<K, V>& m) {
    return {m.begin(), m.end()};
}

template <class M, class K, class V>
auto& map_setdefault(M& m, const K& k, V&& v) {
    auto [it, _] = m.try_emplace(k, std::forward<V>(v));
    return it->second;
}

template <class T>
std::unordered_set<T>& operator+=(std::unordered_set<T>& a, const std::unordered_set<T>& b) {
    a.insert(b.begin(), b.end());
    return a;
}
template <class T>
std::unordered_set<T>& operator+=(std::unordered_set<T>& a, std::unordered_set<T>&& b) {
    a.insert(b.begin(), b.end());
    return a;
}

inline bool operator!(const std::string& s) { return s.empty(); }
inline bool operator!(std::string_view s) { return s.empty(); }
inline bool operator!(std::nullopt_t) { return true; }

std::string py_or(const std::optional<std::string>& a, const char* b) {
    return a ? *a : std::string(b);
}
std::string py_or(const std::optional<std::string>& a, const std::string& b) {
    return a ? *a : b;
}
int py_or(const std::optional<int>& a, int b) { return a.value_or(b); }

bool operator==(const std::string& a, char b) { return a.size() == 1 && a[0] == b; }
bool operator!=(const std::string& a, char b) { return !(a == b); }

template <class T>
void set_discard(std::unordered_set<T>& s, const T& x) { s.erase(x); }

inline bool truthy(const std::optional<int>& x) { return x.has_value() && *x != 0; }
inline bool truthy(const std::optional<std::string>& x) { return x.has_value() && !x->empty(); }

std::string group_of(const std::optional<Match>& m, std::size_t i) {
    return m ? m->group(i) : "";
}
std::string group_of(const Match& m, std::size_t i) { return m.group(i); }

'''

needle = "inline int sub_of(int a, int b) { return a - b; }"
if "int str_index(" not in s:
    s = s.replace(needle, HELPERS + needle, 1)

if "#include <stdexcept>" not in s:
    s = s.replace('#include <format>', '#include <format>\n#include <map>\n#include <stdexcept>')

# string.index -> str_index
s = s.replace(".index(", "_INDEX_OPEN_")
# only the one known call was t.index(...)
s = s.replace("str_index(t, " , "str_index(t, ")  # noop
s = s.replace("(t)_INDEX_OPEN_", "str_index((t), ")
s = s.replace("t_INDEX_OPEN_", "str_index(t, ")
s = s.replace("_INDEX_OPEN_", ".find(")  # leftover: find is close enough if any remain

# keyword inline
s = s.replace("auto inline =", "auto inline_m =")
s = s.replace("bool(inline)", "bool(inline_m)")
s = s.replace("inline->", "inline_m->")
s = s.replace("inline_m_m", "inline_m")

s = s.replace(".discard(", ".erase(")

# bind_tmp: msg is a string, not optional<int>
s = s.replace("        std::optional<int> msg = std::nullopt;",
              "        std::optional<std::string> msg = std::nullopt;")

# undeclared body/bm/nm in second loop of _cxx_bind_tmp
s = s.replace(
    "        body = py_or(fn.body, R\"py()py\");\n        bm = rx_BIND_TMP_LOCAL.search_match(body);",
    "        auto body = py_or(fn.body, R\"py()py\");\n        auto bm = rx_BIND_TMP_LOCAL.search_match(body);",
    1,
)
s = s.replace(
    "        nm = bm->named(R\"py(name)py\");\n        if (!(truthy(Regex(std::format(R\"py(\\breturn\\s+{}\\s*;)py\", re_escape(nm))).search_match(body)))) {",
    "        auto nm = bm->named(R\"py(name)py\");\n        if (!(truthy(Regex(std::format(R\"py(\\breturn\\s+{}\\s*;)py\", re_escape(nm))).search_match(body)))) {",
    1,
)

# optional<Match>.group
s = s.replace("star.group(", "group_of(star, ")
# if that doubled a paren, group_of(star, 1) instead of star.group(1)
# group_of(star, 1)  -- the original was star.group(1) so we get group_of(star, 1) which is correct if we replaced "star.group(" with "group_of(star, "

p.write_text(s, encoding="utf-8")
print("patched", p, "bytes", p.stat().st_size)
