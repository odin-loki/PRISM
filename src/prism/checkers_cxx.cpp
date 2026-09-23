#include "prism/checkers.hpp"
#include "prism/cparse.hpp"

#include <algorithm>
#include <cctype>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace prism {
namespace {

std::string re_escape(std::string_view s) {
    static constexpr std::string_view spec = ".^$*+?()[]{}\\|-";
    std::string o;
    o.reserve(s.size() * 2);
    for (unsigned char c : s) {
        if (spec.find(static_cast<char>(c)) != std::string_view::npos) o.push_back('\\');
        o.push_back(static_cast<char>(c));
    }
    return o;
}

std::vector<std::string> split_lines(std::string_view s) {
    std::vector<std::string> out;
    std::size_t i = 0;
    while (i < s.size()) {
        auto n = s.find('\n', i);
        if (n == std::string_view::npos) {
            auto line = s.substr(i);
            if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
            out.emplace_back(line);
            break;
        }
        auto line = s.substr(i, n - i);
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
        out.emplace_back(line);
        i = n + 1;
    }
    return out;
}

std::string strip(std::string s) {
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) s.erase(s.begin());
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.pop_back();
    return s;
}

std::string lower_copy(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string re_sub_pat(std::string_view pat, std::string_view repl, std::string_view s) {
    Regex re{std::string(pat)};
    std::string o;
    std::size_t pos = 0;
    for (auto& m : re.finditer(s)) {
        if (m.spans.empty()) continue;
        auto a = static_cast<std::size_t>(std::max(0, m.spans[0].first));
        auto b = static_cast<std::size_t>(std::max(0, m.spans[0].second));
        if (a < pos) continue;
        o.append(s.substr(pos, a - pos));
        o.append(repl);
        pos = b;
    }
    o.append(s.substr(pos));
    return o;
}

const std::set<std::string> KW = {
    "if", "for", "while", "switch", "return", "sizeof", "typeof",
    "else", "do", "case", "default",
};
const std::set<std::string> LAMBDA_SKIP = {
    "if", "for", "while", "switch", "return", "sizeof", "typeof",
    "else", "do", "case", "default", "this", "true", "false", "nullptr",
    "new", "delete", "mutable", "const", "int", "void", "char", "auto",
    "bool", "unsigned", "long", "short", "float", "double",
};
const std::set<std::string> NODISCARD_SKIP = {
    "if", "for", "while", "switch", "return", "sizeof", "typeof",
    "else", "do", "case", "default", "this", "true", "false", "nullptr",
    "new", "delete", "mutable", "const", "int", "void", "char", "auto",
    "bool", "unsigned", "long", "short", "float", "double", "class",
    "struct", "enum", "union", "static", "inline", "constexpr", "extern",
    "volatile", "signed", "wchar_t", "size_t", "nodiscard",
};
const std::set<std::string> THROW_SPEC_SKIP = {
    "if", "for", "while", "switch", "return", "sizeof", "typeof",
    "else", "do", "case", "default", "catch", "alignof", "decltype",
};
const std::set<std::string> PLACE_SKIP_PTR = {"nothrow"};

std::set<std::string> names_of(const Regex& re, std::string_view s, const char* grp = "name") {
    std::set<std::string> out;
    for (auto& m : re.finditer(s)) {
        auto n = m.named(grp);
        if (n.empty()) n = m.group(1);
        if (!n.empty()) out.insert(std::move(n));
    }
    return out;
}

std::vector<std::string> split_call_args(std::string_view inner) {
    std::vector<std::string> args;
    std::string cur;
    int depth = 0;
    bool in_str = false, esc = false;
    for (char ch : inner) {
        if (in_str) {
            cur.push_back(ch);
            if (esc) esc = false;
            else if (ch == '\\') esc = true;
            else if (ch == '"') in_str = false;
            continue;
        }
        if (ch == '"') { in_str = true; cur.push_back(ch); }
        else if (ch == '(') { ++depth; cur.push_back(ch); }
        else if (ch == ')') { --depth; cur.push_back(ch); }
        else if (ch == ',' && depth == 0) { args.push_back(strip(cur)); cur.clear(); }
        else cur.push_back(ch);
    }
    auto tail = strip(cur);
    if (!tail.empty()) args.push_back(std::move(tail));
    return args;
}

std::optional<std::vector<std::string>> find_call_args(std::string_view line, std::string_view fn) {
    auto m = re_search_match("\\b" + std::string(fn) + "\\s*\\(", line);
    if (!m || m->spans.empty()) return std::nullopt;
    auto start = static_cast<std::size_t>(m->spans[0].second);
    int depth = 1;
    std::size_t i = start;
    for (; i < line.size() && depth > 0; ++i) {
        if (line[i] == '(') ++depth;
        else if (line[i] == ')') --depth;
    }
    if (depth != 0) return std::nullopt;
    return split_call_args(line.substr(start, i - start - 1));
}

bool is_string_literal(std::string_view raw) {
    auto t = strip(std::string(raw));
    return !t.empty() && (t[0] == '"' || (t.size() >= 2 && t[0] == 'L' && t[1] == '"'));
}

bool null_test_for(std::string_view var, std::string_view text) {
    auto v = re_escape(var);
    if (re_search("\\bif\\s*\\(\\s*(?:" + v + "\\s*==\\s*(?:NULL|0|nullptr)|!\\s*" + v + "\\b|" + v + "\\s*\\))", text))
        return true;
    return re_search("\\(\\s*" + v + "\\s*=(?!=)[^;)]*?\\)\\s*==\\s*(?:NULL|nullptr|0)\\b", text);
}

bool optional_has_guard(std::string_view name, std::string_view body) {
    auto n = re_escape(name);
    if (re_search("\\b" + n + "\\s*\\.\\s*has_value\\s*\\(", body)) return true;
    if (re_search("\\bif\\s*\\(\\s*!?\\s*" + n + "\\s*\\)", body)) return true;
    return re_search("\\bif\\s*\\(\\s*" + n + "\\s*\\.\\s*operator\\s*bool", body);
}

bool optional_deref(std::string_view name, std::string_view ln) {
    auto n = re_escape(name);
    if (re_search("(?<!\\w)\\*\\s*" + n + "\\b", ln)) return true;
    return re_search("\\b" + n + "\\s*->", ln);
}

bool byteswap_idx_guarded(std::string_view idx, std::string_view body) {
    auto i = re_escape(strip(std::string(idx)));
    if (re_search("\\b" + i + "\\s*(?:>=|>|<|<=)\\s*\\d+", body)) return true;
    if (re_search("\\d+\\s*(?:>=|>|<|<=)\\s*" + i + "\\b", body)) return true;
    return re_search("\\b" + i + "\\s*(?:>=|>|<|<=)\\s*[A-Za-z_]\\w*\\s*\\.\\s*size\\s*\\(", body);
}

bool toarr_idx_bad(std::string_view idx, std::optional<int> size, std::string_view body) {
    auto s = strip(std::string(idx));
    bool digits = !s.empty();
    for (unsigned char c : s) if (!std::isdigit(c)) digits = false;
    if (digits) {
        int k = 0;
        try { k = std::stoi(s); } catch (...) { return true; }
        return size ? k >= *size : k >= 4;
    }
    return !byteswap_idx_guarded(s, body);
}

bool param_null_tested(std::string_view name, std::string_view body) {
    auto v = re_escape(name);
    return re_search("\\bif\\s*\\(\\s*" + v + "\\s*==\\s*(?:NULL|nullptr|0)\\b", body)
        || re_search("\\bif\\s*\\(\\s*(?:NULL|nullptr|0)\\s*==\\s*" + v + "\\b", body)
        || re_search("\\bif\\s*\\(\\s*" + v + "\\s*!=\\s*(?:NULL|nullptr|0)\\b", body)
        || re_search("\\bif\\s*\\(\\s*(?:NULL|nullptr|0)\\s*!=\\s*" + v + "\\b", body)
        || re_search("\\bif\\s*\\(\\s*!\\s*" + v + "\\b", body);
}

bool param_if_guard(std::string_view name, std::string_view body) {
    auto v = re_escape(name);
    if (re_search("\\bif\\s*\\(\\s*!\\s*" + v + "\\b", body)) return true;
    return re_search("\\bif\\s*\\(\\s*" + v + "\\b", body);
}

std::string cxx_class_name(std::string typ) {
    typ = re_sub_pat(R"(\b(?:const|volatile|struct|class)\b)", " ", typ);
    for (char& c : typ) if (c == '&' || c == '*') c = ' ';
    auto parts = split_lines(typ);
    (void)parts;
    std::string o, cur;
    for (unsigned char ch : typ) {
        if (std::isspace(ch)) {
            if (!cur.empty()) {
                if (!o.empty()) o += ' ';
                o += cur;
                cur.clear();
            }
        } else cur.push_back(static_cast<char>(ch));
    }
    if (!cur.empty()) {
        if (!o.empty()) o += ' ';
        o += cur;
    }
    return o;
}

std::string cxx_pointee(std::string typ) {
    typ = re_sub_pat(R"(\b(?:const|volatile)\b)", "", typ);
    typ = strip(re_sub_pat(R"(\s+)", " ", typ));
    auto star = typ.find('*');
    if (star != std::string::npos) typ = strip(typ.substr(0, star));
    else if (!typ.empty() && typ.back() == '&') typ.pop_back();
    return lower_copy(strip(typ));
}

bool allowed_reinterp_pointee(std::string_view typ) {
    auto p = cxx_pointee(std::string(typ));
    return p == "void" || p == "char" || p == "signed char" || p == "unsigned char";
}

bool is_ref_or_ptr_type(std::string_view typ) {
    auto t = re_sub_pat(R"(\b(?:const|volatile|restrict)\b)", "", std::string(typ));
    return t.find('*') != std::string::npos || t.find('&') != std::string::npos || t.find('[') != std::string::npos;
}

std::optional<std::string> first_ref_ptr_param(const FunctionInfo& fn) {
    for (auto& [typ, name] : fn.params)
        if (!name.empty() && is_ref_or_ptr_type(typ)) return name;
    return std::nullopt;
}

bool copy_from_param_line(std::string_view param, std::string_view ln) {
    auto p = re_escape(param);
    return re_search("\\b" + p + "\\s*\\.", ln) || re_search("\\b" + p + "\\s*->", ln);
}

bool has_self_assign_guard(std::string_view self_name, std::string_view body) {
    auto sn = re_escape(self_name);
    if (re_search("&\\s*" + sn + "\\s*==", body)) return true;
    if (re_search("==\\s*&\\s*" + sn + "\\b", body)) return true;
    if (re_search(R"(\bthis\s*==\s*&)", body)) return true;
    if (re_search(R"(&\s*\w+\s*==\s*this\b)", body)) return true;
    return re_search(R"(\bthis\s*!=)", body);
}

void report(std::vector<Finding>& out, std::string_view rel, std::string_view fn,
            int line, std::string_view cls, std::string msg,
            const std::vector<std::string>& lines) {
    lint_add(out, rel, std::string(fn), line, cls, msg, lines);
}

const Regex ONESIDED(R"re(\bif\s*\(\s*(?P<i>[A-Za-z_]\w*)\s*(?:>|>=)\s*(?P<n>[A-Za-z_]\w*)\s*\))re");
const Regex _ADDRESSOF_BIND(R"re(\b(?P<name>[A-Za-z_]\w*)\s*=\s*(?:std\s*::\s*)?\baddressof\s*\(\s*(?P<src>[A-Za-z_]\w*))re");
const Regex _ADDRESSOF_INLINE_RET(R"re(\breturn\s+(?:std\s*::\s*)?\baddressof\s*\()re");
const Regex _ADDRESSOF_IN_INDEX(R"re(\[\s*\*\s*(?:std\s*::\s*)?\baddressof\s*\()re");
const Regex _ADDSAT_ASSIGN(R"re(\b(?P<var>[A-Za-z_]\w*)\s*=\s*(?:\(\s*int\s*\))?\s*(?:std\s*::\s*)?(?:add_sat|sub_sat|mul_sat|div_sat|saturate_cast)\s*(?:<\s*[^>]*\s*>)?\s*\()re");
const Regex _ADDSAT_IN_INDEX(R"re(\[\s*(?:\(\s*int\s*\))?\s*(?:std\s*::\s*)?(?:add_sat|sub_sat|mul_sat|div_sat|saturate_cast)\s*(?:<\s*[^>]*\s*>)?\s*\()re");
const Regex _ANY_CAST_CALL(R"re((?:std\s*::\s*)?any_cast\s*<[^>]+>\s*\(\s*(?P<arg>[^)]+)\s*\))re");
const Regex _ASMALIGN_BIND(R"re(\b(?P<name>[A-Za-z_]\w*)\s*=\s*(?:std\s*::\s*)?\bassume_aligned\s*(?:<\s*[^>]*\s*>)?\s*\()re");
const Regex _ASMALIGN_INLINE_IDX(R"re(\b(?:std\s*::\s*)?\bassume_aligned\s*(?:<\s*[^>]*\s*>)?\s*\([^)]*\)\s*\[\s*(?P<idx>[^\]]+)\s*\])re");
const Regex _ASYNC_DISCARDED(R"re(^\s*(?:std\s*::\s*)?async\s*\([^;]*\)\s*;\s*$)re");
const Regex _AS_CONST_BIND(R"re(\b(?P<name>[A-Za-z_]\w*)\s*=\s*(?:std\s*::\s*)?\bas_const\s*\(\s*(?P<src>[A-Za-z_]\w*))re");
const Regex _AS_CONST_CAST_WRITE(R"re(const_cast\s*<[^>]+>\s*\(\s*(?P<name>[A-Za-z_]\w*)\s*\)\s*=)re");
const Regex _AUTO_PTR_USE(R"re(\b(?:std::)?auto_ptr\s*<)re");
const Regex _BCITER_BIND(R"re(\b(?P<name>[A-Za-z_]\w*)\s*=\s*(?:std\s*::\s*)?basic_const_iterator\b)re");
const Regex _BCITER_DECL(R"re((?:std\s*::\s*)?basic_const_iterator\s*<[^>]+>\s+(?P<name>[A-Za-z_]\w*))re");
const Regex _BIND_DISCARDED(R"re(^\s*bind\s*\([^;]*\)\s*;\s*$)re");
const Regex _BIND_TMP_LOCAL(R"re(const\s+(?:std\s*::\s*)?[A-Za-z_]\w*(?:\s*<[^>]*>)?\s*&\s*(?P<name>[A-Za-z_]\w*)\s*=\s*(?:std\s*::\s*)?[A-Za-z_]\w*\s*[\({])re");
const Regex _BITCEIL_ASSIGN(R"re(\b(?P<var>[A-Za-z_]\w*)\s*=\s*(?:std\s*::\s*)?(?:bit_ceil|bit_floor|has_single_bit|popcount)\s*\()re");
const Regex _BITCEIL_IN_INDEX(R"re(\[\s*(?:\(int\))?\s*(?:std\s*::\s*)?(?:bit_ceil|bit_floor|has_single_bit|popcount)\s*\()re");
const Regex _BITCEIL_ZERO(R"re(\b(?:std\s*::\s*)?bit_ceil\s*\(\s*0\s*\))re");
const Regex _BITWIDTH_ASSIGN(R"re(\b(?P<var>[A-Za-z_]\w*)\s*=\s*(?:\(\s*int\s*\))?\s*(?:std\s*::\s*)?bit_width\s*\()re");
const Regex _BITWIDTH_IN_INDEX(R"re(\[\s*(?:\(\s*int\s*\))?\s*(?:std\s*::\s*)?bit_width\s*\()re");
const Regex _BIT_CAST(R"re((?:std\s*::\s*)?bit_cast\s*<\s*(?P<tgt>[^>]+?)\s*>\s*\(\s*(?P<src>[^)]+?)\s*\))re");
const Regex _BYTESWAP_ASSIGN(R"re(\b(?P<var>[A-Za-z_]\w*)\s*=\s*(?:std\s*::\s*)?byteswap\s*\()re");
const Regex _BYTESWAP_IN_INDEX(R"re(\[\s*(?:std\s*::\s*)?byteswap\s*\()re");
const Regex _CATCH_ALL_BLOCK(R"re(\bcatch\s*\(\s*\.\.\.\s*\)\s*\{)re");
const Regex _CATCH_BLOCK(R"re(\bcatch\s*\(\s*(?P<clause>[^)]+)\)\s*\{)re");
const Regex _CATCH_CLAUSE(R"re(\bcatch\s*\(\s*(?P<clause>[^)]+)\))re");
const Regex _CHRONO_NOW(R"re((?:std\s*::\s*)?(?:chrono\s*::\s*)?(?:system_clock|steady_clock|high_resolution_clock)\s*::\s*now\s*\()re");
const Regex _CHRONO_PRNG(R"re(\b(?:srand|srandom|mt19937|mt19937_64)\b)re");
const Regex _CLAMP_ASSIGN(R"re(\b(?P<var>[A-Za-z_]\w*)\s*=\s*(?:\(\s*int\s*\))?\s*std\s*::\s*clamp\s*\()re");
const Regex _CLAMP_IN_INDEX(R"re(\[\s*(?:\(\s*int\s*\))?\s*std\s*::\s*clamp\s*\()re");
const Regex _CLASS_DEF(R"re(\b(?:class|struct)\s+(?P<name>[A-Za-z_]\w*)\b[^{;]*\{)re");
const Regex _CMPLESS_ASSIGN(R"re(\b(?P<var>[A-Za-z_]\w*)\s*=\s*(?:std\s*::\s*)?(?:cmp_less|in_range)\s*(?:<[^>;]*>)?\s*\()re");
const Regex _CMPLESS_IN_INDEX(R"re(\[\s*(?:std\s*::\s*)?(?:cmp_less|in_range)\s*(?:<[^>;]*>)?\s*\()re");
const Regex _CONSTRUCT_AT_CALL(R"re(\bconstruct_at\s*(?:<\s*[^>]*\s*>)?\s*\(\s*(?P<ptr>[A-Za-z_]\w*))re");
const Regex _CONST_CAST_WRITE(R"re(\*\s*const_cast\s*<\s*(?P<target>[^>]+?)\s*>\s*\(\s*&\s*(?P<name>[A-Za-z_]\w*)\s*\)\s*=)re");
const Regex _CONST_LOCAL(R"re(^\s*const\s+(?:unsigned\s+)?(?:char|short|int|long|float|double|bool)\s+(?P<name>[A-Za-z_]\w*)\s*[=;])re", true, false);
const Regex _CONST_OBJ_DECL(R"re((?m)(?:^|[;{(])\s*(?:(?:static|extern|auto|register)\s+)*const\s+(?:unsigned\s+|signed\s+)*(?:(?:std\s*::\s*)?[A-Za-z_]\w*(?:\s*<[^>]*>)?)\s+(?P<name>[A-Za-z_]\w*)\s*[=;])re", true, false);
const Regex _COUNTL_ASSIGN(R"re(\b(?P<var>[A-Za-z_]\w*)\s*=\s*(?:\(\s*int\s*\))?\s*(?:std\s*::\s*)?count[lr]_(?:zero|one)\s*\()re");
const Regex _COUNTL_IN_INDEX(R"re(\[\s*(?:\(\s*int\s*\))?\s*(?:std\s*::\s*)?count[lr]_(?:zero|one)\s*\()re");
const Regex _CUR_EXC_BIND(R"re(\b(?P<name>[A-Za-z_]\w*)\s*=\s*(?:std\s*::\s*)?current_exception\s*\()re");
const Regex _CUR_EXC_INLINE(R"re(\brethrow_exception\s*\(\s*(?:std\s*::\s*)?current_exception\s*\()re");
const Regex _CXX_ADDRESSOF(R"re(\baddressof\s*\()re");
const Regex _CXX_ADDSAT_TOKEN(R"re(\b(?:std\s*::\s*)?(?:add_sat|sub_sat|mul_sat|div_sat|saturate_cast)\s*(?:<\s*[^>]*\s*>)?\s*\()re");
const Regex _CXX_ADJACENT(R"re((?:(?:std\s*::\s*)?views\s*::\s*adjacent\b|\badjacent_transform\b|\badjacent_view\b))re");
const Regex _CXX_APPLY(R"re(\bstd\s*::\s*apply\s*\(\s*(?P<arg>[A-Za-z_]\w*))re");
const Regex _CXX_ARRAY_DECL(R"re((?:std\s*::\s*)?\barray\s*<\s*[^,>]+,\s*(?P<n>[^>]+)\s*>\s+(?P<name>[A-Za-z_]\w*)\b)re");
const Regex _CXX_ASSUME_ALIGNED(R"re(\bassume_aligned\s*(?:<\s*[^>]*\s*>)?\s*\()re");
const Regex _CXX_ASSUME_FALSE(R"re(\[\[\s*assume\s*\(\s*(?:false|0)\s*\)\s*\]\])re");
const Regex _CXX_AS_CONST(R"re(\bas_const\s*\()re");
const Regex _CXX_AS_RVALUE(R"re(\bas_rvalue(?:_view)?\b)re");
const Regex _CXX_ATOMIC_FENCE(R"re(\b(?:atomic_thread_fence|atomic_signal_fence)\s*\()re");
const Regex _CXX_ATOMIC_FLAG_DECL(R"re((?:std\s*::\s*)?atomic_flag\s+(?P<name>[A-Za-z_]\w*)\b)re");
const Regex _CXX_ATOMIC_REF_CTOR(R"re((?:std\s*::\s*)?atomic_ref\s*<[^>]+>\s+(?P<ref>[A-Za-z_]\w*)\s*[\({]\s*(?P<name>[A-Za-z_]\w*))re");
const Regex _CXX_AT_QUICK_EXIT(R"re(\bat_quick_exit\s*\()re");
const Regex _CXX_AUTO_LAMBDA(R"re(\bauto\s+(?P<name>[A-Za-z_]\w*)\s*=\s*\[(?P<capture>[^\]]*)\])re");
const Regex _CXX_BARRIER_DECL(R"re((?:std\s*::\s*)?\bbarrier(?:\s*<[^>]*>)?\s+(?P<name>[A-Za-z_]\w*))re");
const Regex _CXX_BASIC_CONST_ITER(R"re(\bbasic_const_iterator\b)re");
const Regex _CXX_BEGIN_CALL(R"re((?P<container>[A-Za-z_]\w*(?:\s*(?:->|\.)\s*[A-Za-z_]\w*)*)\s*\.\s*begin\s*\()re");
const Regex _CXX_BINSEM_ZERO(R"re((?:std\s*::\s*)?binary_semaphore\s+(?P<name>[A-Za-z_]\w*)\s*\(\s*0\s*\))re");
const Regex _CXX_BITCEIL_TOKEN(R"re(\b(?:std\s*::\s*)?(?:bit_ceil|bit_floor|has_single_bit|popcount)\s*\()re");
const Regex _CXX_BITSET_DECL(R"re((?:std\s*::\s*)?bitset\s*<\s*(?P<n>[^>]+)\s*>\s+(?P<name>[A-Za-z_]\w*))re");
const Regex _CXX_BITSET_TEST(R"re(\b(?P<name>[A-Za-z_]\w*)\s*\.\s*test\s*\(\s*(?P<idx>[A-Za-z_]\w*)\s*\))re");
const Regex _CXX_BITWIDTH_TOKEN(R"re(\b(?:std\s*::\s*)?bit_width\s*\()re");
const Regex _CXX_CALL(R"re((?:\b(?:this\s*->\s*)?([A-Za-z_]\w*)\s*\())re");
const Regex _CXX_CARTESIAN(R"re(\bcartesian_product\b)re");
const Regex _CXX_CHAR_BUF(R"re(\bchar\s+(?P<name>[A-Za-z_]\w*)\s*\[)re");
const Regex _CXX_CHUNK(R"re((?:(?:std\s*::\s*)?views\s*::\s*chunk\b|\bchunk_by\b|\bchunk_view\b))re");
const Regex _CXX_CLAMP_TOKEN(R"re(\bstd\s*::\s*clamp\s*\()re");
const Regex _CXX_CMPLESS_TOKEN(R"re(\b(?:std\s*::\s*)?(?:cmp_less|in_range)\s*(?:<[^>;]*>)?\s*\()re");
const Regex _CXX_CONSTRUCT_AT(R"re(\b(?:construct_at|destroy_at)\s*\()re");
const Regex _CXX_CONTAINER_MUTATE(R"re((?P<container>[A-Za-z_]\w*(?:\s*(?:->|\.)\s*[A-Za-z_]\w*)*)\s*\.(?:push_back|insert|erase|clear)\s*\()re");
const Regex _CXX_CONTRACT_FALSE(R"re(\bcontract_assert\s*\(\s*(?:false|0)\s*\)|(?:^|[;{])\s*(?:pre|post)\s*\(\s*(?:false|0)\s*\)|\[\[\s*(?:pre|post)\s*:\s*(?:false|0))re");
const Regex _CXX_COPYABLE_FN_DECL(R"re((?:std\s*::\s*)?copyable_function\s*<[^>]+>\s+(?P<name>[A-Za-z_]\w*)\b)re");
const Regex _CXX_CORO_HANDLE_DECL(R"re((?:std\s*::\s*)?coroutine_handle(?:\s*<[^>]*>)?\s+(?P<name>[A-Za-z_]\w*)\b)re");
const Regex _CXX_CORR_MEMBER(R"re(\bis_corresponding_member\b)re");
const Regex _CXX_COUNTED(R"re((?:(?:std\s*::\s*)?views\s*::\s*counted\b|\bcounted_view\b))re");
const Regex _CXX_COUNTED_ITER_CTOR(R"re((?:std\s*::\s*)?counted_iterator(?:\s*<[^>]+>)?\s+(?P<name>[A-Za-z_]\w*)\s*\(\s*(?P<src>[^,]+),\s*(?P<n>[A-Za-z_]\w*))re");
const Regex _CXX_COUNTL_TOKEN(R"re(\b(?:std\s*::\s*)?count[lr]_(?:zero|one)\s*\()re");
const Regex _CXX_CURRENT_EXC(R"re(\bcurrent_exception\s*\()re");
const Regex _CXX_CVANY_DECL(R"re((?:std\s*::\s*)?condition_variable_any\s+(?P<name>[A-Za-z_]\w*)\b)re");
const Regex _CXX_CV_WAITER(R"re(\bwait(?:_for|_until)?\s*\()re");
const Regex _CXX_CV_WAIT_BARE(R"re(\.\s*wait\s*\(\s*[A-Za-z_]\w*\s*\))re");
const Regex _CXX_DEQUE_DECL(R"re((?:std\s*::\s*)?deque\s*<[^>]+>\s+(?P<name>[A-Za-z_]\w*)\b)re");
const Regex _CXX_DESTROY_N(R"re(\bdestroy_n\s*\()re");
const Regex _CXX_DROP(R"re((?:(?:std\s*::\s*)?views\s*::\s*drop\b|\bdrop_view\b))re");
const Regex _CXX_DROP_WHILE(R"re(\bdrop_while(?:_view)?\b)re");
const Regex _CXX_ELEMENTS(R"re((?:(?:std\s*::\s*)?views\s*::\s*elements\b|\belements_view\b))re");
const Regex _CXX_ENDIAN(R"re(\bstd\s*::\s*endian\b)re");
const Regex _CXX_ENUMERATE(R"re((?:(?:std\s*::\s*)?views\s*::\s*enumerate\b|\benumerate_view\b))re");
const Regex _CXX_ERROR_CATEGORY(R"re(\berror_category\b)re");
const Regex _CXX_ERROR_CODE_DECL(R"re((?:std\s*::\s*)?error_code\s+(?P<name>[A-Za-z_]\w*)\b)re");
const Regex _CXX_EXCEPTION_PTR_DECL(R"re((?:std\s*::\s*)?exception_ptr\s+(?P<name>[A-Za-z_]\w*)\b)re");
const Regex _CXX_EXCHANGE_TOKEN(R"re(\bstd\s*::\s*exchange\s*\()re");
const Regex _CXX_EXCLUSIVE_SCAN(R"re(\bexclusive_scan\s*\()re");
const Regex _CXX_EXPECTED_DECL(R"re((?:std\s*::\s*)?expected\s*<[^>]+>\s+(?P<name>[A-Za-z_]\w*)\b)re");
const Regex _CXX_FENCE_ORDER(R"re(\b(?:atomic_thread_fence|atomic_signal_fence)\s*\([^)]*memory_order)re");
const Regex _CXX_FILTER(R"re((?:(?:std\s*::\s*)?views\s*::\s*filter\b|\bfilter_view\b))re");
const Regex _CXX_FLAT_MAP_DECL(R"re((?:std\s*::\s*)?(?:flat_map|(?P<map>map))\s*<[^>]+>\s+(?P<name>[A-Za-z_]\w*))re");
const Regex _CXX_FLAT_MULTIMAP_DECL(R"re((?:std\s*::\s*)?flat_multimap\s*<[^>]+>\s+(?P<name>[A-Za-z_]\w*))re");
const Regex _CXX_FLAT_MULTISET_DECL(R"re((?:std\s*::\s*)?flat_multiset\s*<[^>]+>\s+(?P<name>[A-Za-z_]\w*))re");
const Regex _CXX_FLAT_SET_DECL(R"re((?:std\s*::\s*)?flat_set\s*<[^>]+>\s+(?P<name>[A-Za-z_]\w*))re");
const Regex _CXX_FN_REF_RETURN(R"re(\breturn\s+(?:std\s*::\s*)?function_ref\s*<)re");
const Regex _CXX_FN_REF_TMP(R"re((?:std\s*::\s*)?function_ref\s*<[^>]+>\s+(?P<name>[A-Za-z_]\w*)\s*=\s*(?:\[|.+\())re");
const Regex _CXX_FORMAT_TO(R"re(\bformat_to\s*\(\s*(?P<dst>[A-Za-z_]\w*))re");
const Regex _CXX_FROM_RANGE(R"re(\bfrom_range\b)re");
const Regex _CXX_FSTREAM_DECL(R"re((?:std\s*::\s*)?(?:ifstream|ofstream|fstream)\s+(?P<name>[A-Za-z_]\w*)\b)re");
const Regex _CXX_FUNCTION_DECL(R"re((?:std\s*::\s*)?\bfunction\s*<[^>]+>\s+(?P<name>[A-Za-z_]\w*)\b)re");
const Regex _CXX_FUTURE_DECL(R"re((?:std\s*::\s*)?future\s*<[^>]+>\s+(?P<name>[A-Za-z_]\w*)\b)re");
const Regex _CXX_FWDLIKE_TOKEN(R"re(\b(?:std\s*::\s*)?forward_like\s*(?:<\s*[^>]*\s*>)?\s*\()re");
const Regex _CXX_FWD_LIST_DECL(R"re((?:std\s*::\s*)?forward_list\s*<[^>]+>\s+(?P<name>[A-Za-z_]\w*)\b)re");
const Regex _CXX_GCD_TOKEN(R"re(\bstd\s*::\s*gcd\s*\()re");
const Regex _CXX_GENERATOR_ASSIGN(R"re((?:std\s*::\s*)?generator\s*<[^>]+>\s+(?P<name>[A-Za-z_]\w*)\s*=(?!=))re");
const Regex _CXX_HIVE_DECL(R"re((?:std\s*::\s*)?hive\s*<[^>]+>\s+(?P<name>[A-Za-z_]\w*))re");
const Regex _CXX_HIVE_MUTATE(R"re((?P<container>[A-Za-z_]\w*(?:\s*(?:->|\.)\s*[A-Za-z_]\w*)*)\s*\.(?:insert|emplace|erase)\s*\()re");
const Regex _CXX_ICE_TOKEN(R"re(\bis_constant_evaluated\s*\()re");
const Regex _CXX_INCLUSIVE_SCAN(R"re(\binclusive_scan\s*\()re");
const Regex _CXX_INDIRECT_DECL(R"re((?:std\s*::\s*)?indirect\s*<[^>]+>\s+(?P<name>[A-Za-z_]\w*))re");
const Regex _CXX_INF_LOOP(R"re(\bwhile\s*\(\s*true\s*\)|\bfor\s*\(\s*;\s*;\s*\))re");
const Regex _CXX_INHERIT(R"re(\b(?:struct|class)\s+(?P<derived>[A-Za-z_]\w*)\s*:\s*(?:(?:public|protected|private|virtual)\s+)*(?P<base>[A-Za-z_]\w*))re");
const Regex _CXX_INPLACE_DECL(R"re((?:std\s*::\s*)?inplace_vector\s*<[^>]+>\s+(?P<name>[A-Za-z_]\w*))re");
const Regex _CXX_INVOKE(R"re(\bstd\s*::\s*invoke\s*\(\s*(?P<arg>[A-Za-z_]\w*))re");
const Regex _CXX_IN_RANGE_IF(R"re(\bif\s*\(\s*!?\s*(?:std\s*::\s*)?in_range\s*(?:<[^>;]*>)?\s*\()re");
const Regex _CXX_IOTA(R"re((?:(?:std\s*::\s*)?views\s*::\s*iota\b|\biota_view\b))re");
const Regex _CXX_JOIN_AUTO(R"re(\bauto\s+(?P<name>[A-Za-z_]\w*)\s*=\s*(?:std\s*::\s*)?views\s*::\s*join\s*\()re");
const Regex _CXX_JOIN_VIEW_DECL(R"re((?:std\s*::\s*(?:ranges\s*::\s*)?)?join_view(?:\s*<[^>]+>)?\s+(?P<name>[A-Za-z_]\w*)\b)re");
const Regex _CXX_JOIN_WITH(R"re(\bjoin_with(?:_view)?\b)re");
const Regex _CXX_JTHREAD_DECL(R"re((?:std\s*::\s*)?\bjthread\s+(?P<name>[A-Za-z_]\w*)\s*\()re");
const Regex _CXX_KEYS(R"re((?:(?:std\s*::\s*)?views\s*::\s*keys\b|\bkeys_view\b))re");
const Regex _CXX_KILLDEP(R"re(\bkill_dependency\s*\()re");
const Regex _CXX_LATCH_DECL(R"re((?:std\s*::\s*)?\blatch\s+(?P<name>[A-Za-z_]\w*)\s*\()re");
const Regex _CXX_LAYOUT_COMPATIBLE(R"re(\bis_layout_compatible\b)re");
const Regex _CXX_LCM_TOKEN(R"re(\bstd\s*::\s*lcm\s*\()re");
const Regex _CXX_LERP_FINITE(R"re(\b(?:isfinite|isnan|isinf)\s*\()re");
const Regex _CXX_LERP_TOKEN(R"re(\b(?:std\s*::\s*)?lerp\s*\()re");
const Regex _CXX_LIST_DECL(R"re((?:std\s*::\s*)?\blist\s*<[^>]+>\s+(?P<name>[A-Za-z_]\w*)\b)re");
const Regex _CXX_LOCAL_LINE(R"re(^\s*(?:(?:const|static|volatile)\s+)*(?:unsigned\s+)?(?:char|short|int|long|float|double|bool|size_t|auto|(?:std::)?(?:string|vector|map|set)\b|[A-Za-z_]\w*)\s+(?P<name>[A-Za-z_]\w*)\s*(?:=|;))re");
const Regex _CXX_LOCAL_OBJ(R"re(^\s*(?:const\s+)?(?P<ty>[A-Za-z_]\w*)\s+(?P<name>[A-Za-z_]\w*)\s*(?:=|;))re", true, false);
const Regex _CXX_LOCAL_STRING(R"re(^\s*(?P<static>static\s+)?(?:std::)?string\s+(?P<name>[A-Za-z_]\w*)\s*[;=])re", true, false);
const Regex _CXX_LOCK_RAII(R"re(\b(?:lock_guard|unique_lock|scoped_lock|shared_lock)\b)re");
const Regex _CXX_MAKE_EPTR(R"re(\bmake_exception_ptr\s*\()re");
const Regex _CXX_MAP_DECL(R"re((?:std\s*::\s*)?\bmap\s*<[^>]+>\s+(?P<name>[A-Za-z_]\w*)\b)re");
const Regex _CXX_MIDPOINT_TOKEN(R"re(\b(?:std\s*::\s*)?midpoint\s*\()re");
const Regex _CXX_MOF_DECL(R"re((?:std\s*::\s*)?move_only_function\s*<[^>]+>\s+(?P<name>[A-Za-z_]\w*)\b)re");
const Regex _CXX_MULTIMAP_DECL(R"re((?:std\s*::\s*)?\bmultimap\s*<[^>]+>\s+(?P<name>[A-Za-z_]\w*)\b)re");
const Regex _CXX_MULTISET_DECL(R"re((?:std\s*::\s*)?\bmultiset\s*<[^>]+>\s+(?P<name>[A-Za-z_]\w*)\b)re");
const Regex _CXX_NESTED_EXC(R"re(\b(?:nested_exception|throw_with_nested|rethrow_if_nested)\b)re");
const Regex _CXX_NONTYPE(R"re(\bnontype\b)re");
const Regex _CXX_NOTIFY_EXIT(R"re(\bnotify_all_at_thread_exit\b)re");
const Regex _CXX_OPTIONAL_DECL(R"re((?:std\s*::\s*)?optional\s*<[^>]+>\s+(?P<name>[A-Za-z_]\w*)\b)re");
const Regex _CXX_OSYNC_DECL(R"re((?:std\s*::\s*)?(?:basic_)?osyncstream(?:\s*<[^>]+>)?\s+(?P<name>[A-Za-z_]\w*)\s*[\({])re");
const Regex _CXX_PACKAGED_DECL(R"re((?:std\s*::\s*)?packaged_task\s*<[^>]+>\s+(?P<name>[A-Za-z_]\w*)\b)re");
const Regex _CXX_PQUEUE_DECL(R"re((?:std\s*::\s*)?priority_queue\s*<[^>]+>\s+(?P<name>[A-Za-z_]\w*)\b)re");
const Regex _CXX_PROMISE_DECL(R"re((?:std\s*::\s*)?promise\s*<[^>]+>\s+(?P<name>[A-Za-z_]\w*)\b)re");
const Regex _CXX_PTR_DECL(R"re(\b(?:const\s+)?(?P<cls>[A-Za-z_]\w*)\s*(?P<ptr>(?:\s*\*+\s*)+)(?P<name>[A-Za-z_]\w*)\b)re");
const Regex _CXX_PTR_DECL_ANY(R"re((?P<typ>(?:(?:unsigned|signed)\s+)?(?:char|short|int|long|float|double|void|std::byte|[A-Za-z_]\w*)\s*(?:\s*const\s*)?\*+)\s*(?P<name>[A-Za-z_]\w*)\b)re");
const Regex _CXX_PTR_DECL_LINE(R"re(^\s*(?:(?:const|volatile)\s+)*(?P<typ>(?:(?:unsigned|signed)\s+)?(?:char|short|int|long|float|double|void|std::byte|[A-Za-z_]\w*)\s*(?:\s*const\s*)?\*+)\s*(?P<name>[A-Za-z_]\w*)\s*=)re", true, false);
const Regex _CXX_PTR_INTERCONV(R"re(\bis_pointer_interconvertible_(?:with_class|base_of)\b)re");
const Regex _CXX_QUEUE_DECL(R"re((?:std\s*::\s*)?\bqueue\s*<[^>]+>\s+(?P<name>[A-Za-z_]\w*)\b)re");
const Regex _CXX_QUICK_EXIT(R"re(\bquick_exit\s*\()re");
const Regex _CXX_RANGES_TO(R"re(\branges\s*::\s*to\s*(?:<|\())re");
const Regex _CXX_RECURSIVE_MUTEX_DECL(R"re((?:std\s*::\s*)?recursive_mutex\s+(?P<name>[A-Za-z_]\w*)\b)re");
const Regex _CXX_RECURSIVE_TIMED_MUTEX_DECL(R"re((?:std\s*::\s*)?recursive_timed_mutex\s+(?P<name>[A-Za-z_]\w*)\b)re");
const Regex _CXX_REDUCE_TOKEN(R"re(\bstd\s*::\s*reduce\s*\()re");
const Regex _CXX_REFLECT_DEFINE(R"re(\bdefine_(?:aggregate|class)\s*\()re");
const Regex _CXX_REFLECT_META(R"re(\^\^|\bstd\s*::\s*meta\s*::)re");
const Regex _CXX_REFWRAP_BIND(R"re(\b(?P<w>[A-Za-z_]\w*)\s*=\s*(?:std\s*::\s*)?(?:cref|ref)\s*\(\s*(?P<arg>[A-Za-z_]\w*))re");
const Regex _CXX_REFWRAP_RETURN(R"re(\breturn\s+(?:std\s*::\s*)?(?:cref|ref)\s*\(\s*(?P<arg>[A-Za-z_]\w*))re");
const Regex _CXX_REFWRAP_TOKEN(R"re((?:\breference_wrapper\b|\bstd\s*::\s*(?:cref|ref)\s*\())re");
const Regex _CXX_REPEAT(R"re((?:(?:std\s*::\s*)?views\s*::\s*repeat\b|\brepeat_view\b))re");
const Regex _CXX_RETHROW_EXC(R"re(\brethrow_exception\s*\()re");
const Regex _CXX_RETURN_ATOMIC_REF(R"re(\breturn\s+(?:std\s*::\s*)?atomic_ref\s*<[^>]+>\s*\(\s*(?P<name>[A-Za-z_]\w*))re");
const Regex _CXX_RETURN_LAMBDA(R"re(\breturn\s*\[(?P<capture>[^\]]*)\])re");
const Regex _CXX_RETURN_MDSPAN(R"re(\breturn\s+(?:std\s*::\s*)?mdspan\s*<[^>]+>\s*\(\s*(?P<name>[A-Za-z_]\w*))re");
const Regex _CXX_RETURN_NAME(R"re(\breturn\s+(?P<name>[A-Za-z_]\w*)\s*;)re");
const Regex _CXX_RETURN_SPAN(R"re(\breturn\s+(?:std\s*::\s*)?\bspan\s*<[^>]+>\s*\(\s*(?P<name>[A-Za-z_]\w*))re");
const Regex _CXX_RETURN_U8VIEW(R"re(\breturn\s+(?:std\s*::\s*)?u8string_view\s*\(\s*(?P<name>[A-Za-z_]\w*)\s*\))re");
const Regex _CXX_RETURN_VIEW_OF(R"re(\breturn\s+(?:std::)?(?:string_view|span)\s*\(\s*(?P<name>[A-Za-z_]\w*)\s*\)\s*;)re");
const Regex _CXX_RETURN_WVIEW(R"re(\breturn\s+(?:std\s*::\s*)?wstring_view\s*\(\s*(?P<name>[A-Za-z_]\w*)\s*\))re");
const Regex _CXX_REVERSE_VIEW(R"re((?:(?:std\s*::\s*)?views\s*::\s*reverse\b|\breverse_view\b))re");
const Regex _CXX_ROTL_TOKEN(R"re(\b(?:std\s*::\s*)?rot[lr]\s*\()re");
const Regex _CXX_SCALAR_LOCAL(R"re(^\s*(?:(?:const|volatile)\s+)*(?P<typ>(?:(?:unsigned|signed)\s+)?(?:char|short|int|long|float|double|bool|[A-Za-z_]\w*))\s+(?P<name>[A-Za-z_]\w*)\s*[=;])re", true, false);
const Regex _CXX_SCALAR_MEMBER(R"re(\b(?:int|short|long|char|bool|float|double)\s+(?P<name>[A-Za-z_]\w*)\s*;)re");
const Regex _CXX_SCOPED_ENUM(R"re(\bis_scoped_enum\b)re");
const Regex _CXX_SEM_DECL(R"re((?:std\s*::\s*)?counting_semaphore(?:\s*<[^>]+>)?\s+(?P<name>[A-Za-z_]\w*))re");
const Regex _CXX_SET_DECL(R"re((?:std\s*::\s*)?\bset\s*<[^>]+>\s+(?P<name>[A-Za-z_]\w*)\b)re");
const Regex _CXX_SET_TERMINATE(R"re(\b(?:set_terminate|get_terminate)\s*\()re");
const Regex _CXX_SHARED_LOCK_DECL(R"re((?:std\s*::\s*)?shared_lock\s*<[^>]+>\s+(?P<name>[A-Za-z_]\w*)\b)re");
const Regex _CXX_SHARED_MUTEX_DECL(R"re((?:std\s*::\s*)?shared_mutex\s+(?P<name>[A-Za-z_]\w*)\b)re");
const Regex _CXX_SHARED_TIMED_MUTEX_DECL(R"re((?:std\s*::\s*)?shared_timed_mutex\s+(?P<name>[A-Za-z_]\w*)\b)re");
const Regex _CXX_SIMD_DECL(R"re((?:(?:std\s*::\s*)?(?:experimental\s*::\s*)?)?simd\s*<[^>]+>\s+(?P<name>[A-Za-z_]\w*))re");
const Regex _CXX_SLIDE(R"re((?:(?:std\s*::\s*)?views\s*::\s*slide\b|\bslide_view\b))re");
const Regex _CXX_SPANSTREAM_DECL(R"re((?:std\s*::\s*)?(?:basic_)?(?:i|o)?spanstream\s+(?P<name>[A-Za-z_]\w*))re");
const Regex _CXX_SSTREAM_CSTR_TMP(R"re((?P<ss>[A-Za-z_]\w*)\s*\.\s*str\s*\(\s*\)\s*\.\s*c_str\s*\()re");
const Regex _CXX_SSTREAM_DECL(R"re((?:std\s*::\s*)?(?:basic_)?(?:string|ostring|istring)stream\s+(?P<name>[A-Za-z_]\w*))re");
const Regex _CXX_SSTREAM_VIEW(R"re((?:std\s*::\s*)?string_view\s+(?P<v>[A-Za-z_]\w*)\s*=\s*(?P<ss>[A-Za-z_]\w*)\s*\.\s*str\s*\()re");
const Regex _CXX_STACK_DECL(R"re((?:std\s*::\s*)?\bstack\s*<[^>]+>\s+(?P<name>[A-Za-z_]\w*)\b)re");
const Regex _CXX_STD_UNREACHABLE(R"re(\bstd\s*::\s*unreachable\s*\()re");
const Regex _CXX_STOP_TOKEN(R"re((?:std\s*::\s*)?stop_token\b)re");
const Regex _CXX_STRIDE(R"re((?:(?:std\s*::\s*)?views\s*::\s*stride\b|\bstride_view\b))re");
const Regex _CXX_SUBSCRIPT(R"re(\b(?P<cont>[A-Za-z_]\w*)\s*\[\s*(?P<idx>[^\]]+)\s*\])re");
const Regex _CXX_SYNCBUF_DECL(R"re((?:std\s*::\s*)?(?:basic_)?syncbuf(?:\s*<[^>]+>)?\s+(?P<name>[A-Za-z_]\w*)\s*[\({])re");
const Regex _CXX_SYSTEM_ERROR_CATCH(R"re(\bcatch\s*\(\s*(?:const\s+)?(?:std\s*::\s*)?system_error\s*[&*]?\s*(?P<name>[A-Za-z_]\w*))re");
const Regex _CXX_SYSTEM_ERROR_DECL(R"re((?:std\s*::\s*)?system_error\s+(?P<name>[A-Za-z_]\w*)\b)re");
const Regex _CXX_TAKE(R"re((?:(?:std\s*::\s*)?views\s*::\s*take\b|\btake_view\b))re");
const Regex _CXX_TAKE_WHILE(R"re(\btake_while(?:_view)?\b)re");
const Regex _CXX_TASK_STARTED(R"re((?:std\s*::\s*(?:execution\s*::\s*)?)?task\s*<[^>]+>\s+(?P<name>[A-Za-z_]\w*)\s*=(?!=))re");
const Regex _CXX_THREAD_DECL(R"re((?:std\s*::\s*)?\bthread\s+(?P<name>[A-Za-z_]\w*)\s*\()re");
const Regex _CXX_THROW_SPEC(R"re((?P<name>[A-Za-z_]\w*)\s*\([^;{}]*\)\s*throw\s*\([^;{}]*\)\s*[{;])re");
const Regex _CXX_TIMED_MUTEX_DECL(R"re((?:std\s*::\s*)?\btimed_mutex\s+(?P<name>[A-Za-z_]\w*)\b)re");
const Regex _CXX_TO_ADDRESS(R"re(\bto_address\s*\()re");
const Regex _CXX_TO_ARRAY(R"re(\bto_array\s*\()re");
const Regex _CXX_TO_UNDERLYING(R"re((?:std\s*::\s*)?to_underlying\s*\(\s*(?P<arg>[A-Za-z_]\w*)\s*\))re");
const Regex _CXX_TRANSFORM_REDUCE(R"re(\btransform_reduce\s*\()re");
const Regex _CXX_TRANSFORM_SCAN(R"re(\b(?:transform_inclusive_scan|transform_exclusive_scan)\s*\()re");
const Regex _CXX_TRANSFORM_VIEW(R"re((?:(?:std\s*::\s*)?views\s*::\s*transform\b|\btransform_view\b))re");
const Regex _CXX_TRED_TOKEN(R"re(\b(?:std\s*::\s*)?transform_reduce\s*\()re");
const Regex _CXX_TUPLE_DECL(R"re((?:std\s*::\s*)?tuple\s*<[^>]+>\s+(?P<name>[A-Za-z_]\w*)\b)re");
const Regex _CXX_TUPLE_GET(R"re((?:std\s*::\s*)?\bget\s*<)re");
const Regex _CXX_TYPE(R"re(\b(?:struct|class)\s+([A-Za-z_]\w*)\b)re");
const Regex _CXX_TYPE_IDENTITY(R"re(\btype_identity(?:_t)?\s*<)re");
const Regex _CXX_TZDB_BIND(R"re(\b(?:auto(?:\s*[&*])?|(?:const\s+)?(?:std\s*::\s*chrono\s*::\s*)?time_zone\s*\*|tzdb\s*&)\s+(?P<name>[A-Za-z_]\w*)\s*=[^;]*(?:current_zone|locate_zone|get_tzdb)\s*\()re");
const Regex _CXX_TZDB_CALL(R"re(\b(?:current_zone|locate_zone|get_tzdb)\s*\()re");
const Regex _CXX_U8STRING_DECL(R"re(^\s*(?P<static>static\s+)?(?:std\s*::\s*)?u8string\s+(?P<name>[A-Za-z_]\w*))re", true, false);
const Regex _CXX_U8STRING_VIEW_BIND(R"re((?:std\s*::\s*)?u8string_view\s+(?P<name>[A-Za-z_]\w*)\s*=\s*(?P<src>[A-Za-z_]\w*))re");
const Regex _CXX_UMAP_DECL(R"re((?:std\s*::\s*)?unordered_map\s*<[^>]+>\s+(?P<name>[A-Za-z_]\w*)\b)re");
const Regex _CXX_UMMAP_DECL(R"re((?:std\s*::\s*)?unordered_multimap\s*<[^>]+>\s+(?P<name>[A-Za-z_]\w*)\b)re");
const Regex _CXX_UMSET_DECL(R"re((?:std\s*::\s*)?unordered_multiset\s*<[^>]+>\s+(?P<name>[A-Za-z_]\w*)\b)re");
const Regex _CXX_UNCAUGHT(R"re(\buncaught_exceptions\b)re");
const Regex _CXX_UNEXPECTED_DECL(R"re((?:std\s*::\s*)?unexpected\s*<[^>]+>\s+(?P<name>[A-Za-z_]\w*)\b)re");
const Regex _CXX_UNINITIALIZED_COPY(R"re(\b(?:uninitialized_copy|uninitialized_move)\s*\()re");
const Regex _CXX_UNINITIALIZED_FILL(R"re(\b(?:uninitialized_fill|uninitialized_default_construct)\s*\()re");
const Regex _CXX_UNINITIALIZED_VALUE(R"re(\buninitialized_value_construct\s*\()re");
const Regex _CXX_USET_DECL(R"re((?:std\s*::\s*)?unordered_set\s*<[^>]+>\s+(?P<name>[A-Za-z_]\w*)\b)re");
const Regex _CXX_VALARRAY_DECL(R"re((?:std\s*::\s*)?valarray\s*<[^>]+>\s+(?P<name>[A-Za-z_]\w*)\b)re");
const Regex _CXX_VALUES(R"re((?:(?:std\s*::\s*)?views\s*::\s*values\b|\bvalues_view\b))re");
const Regex _CXX_VARIANT_GET(R"re((?:std\s*::\s*)?\bget\s*<\s*(?P<ty>[^>]+)\s*>\s*\(\s*(?P<var>[A-Za-z_]\w*)\s*\))re");
const Regex _CXX_VEC_STR_DECL(R"re(\b(?:std\s*::\s*)?(?:vector\s*<[^>;]+>|(?:basic_)?string)\s+(?P<name>[A-Za-z_]\w*)\b)re");
const Regex _CXX_VIEWS_JOIN(R"re((?:(?:std\s*::\s*)?views\s*::\s*join\b|\bjoin_view\b))re");
const Regex _CXX_VIEWS_ZIP(R"re((?:(?:std\s*::\s*)?views\s*::\s*zip\b|\bzip_view\b))re");
const Regex _CXX_VIEW_RET(R"re(\b(?:string_view|span)\b)re");
const Regex _CXX_VIRTUAL(R"re(\bvirtual\b[^{;]*?\b([A-Za-z_]\w*)\s*\()re");
const Regex _CXX_WCONVERT_XFORM(R"re(\.\s*(?:to_bytes|from_bytes)\s*\()re");
const Regex _CXX_WEAK_LOCK(R"re((?P<locked>[A-Za-z_]\w*)\s*=\s*(?P<weak>[A-Za-z_]\w*)\s*\.\s*lock\s*\()re");
const Regex _CXX_WEAK_PTR_DECL(R"re((?:std\s*::\s*)?weak_ptr\s*<[^>]+>\s+(?P<name>[A-Za-z_]\w*)\b)re");
const Regex _CXX_WSTRING_CONVERT(R"re(\bwstring_convert\b)re");
const Regex _CXX_WSTRING_DECL(R"re(^\s*(?P<static>static\s+)?(?:std\s*::\s*)?wstring\s+(?P<name>[A-Za-z_]\w*))re", true, false);
const Regex _CXX_WSTRING_VIEW_BIND(R"re((?:std\s*::\s*)?wstring_view\s+(?P<name>[A-Za-z_]\w*)\s*=\s*(?P<src>[A-Za-z_]\w*))re");
const Regex _CXX_ZIP_AUTO(R"re(\bauto\s+(?P<name>[A-Za-z_]\w*)\s*=\s*(?:std\s*::\s*)?views\s*::\s*zip\s*\()re");
const Regex _CXX_ZIP_TRANSFORM(R"re(\bzip_transform(?:_view)?\b)re");
const Regex _CXX_ZIP_VIEW_DECL(R"re((?:std\s*::\s*(?:ranges\s*::\s*)?)?zip_view(?:\s*<[^>]+>)?\s+(?P<name>[A-Za-z_]\w*)\b)re");
const Regex _CXX_ZONED_TIME(R"re(\bzoned_time\b)re");
const Regex _CXX_ZONE_LOOKUP(R"re(\b(?:locate_zone|current_zone)\b)re");
const Regex _C_ARRAY_SIZE(R"re(\b(?:(?:unsigned|signed|const|volatile)\s+)*(?:int|char|short|long|float|double)\s+(?P<name>[A-Za-z_]\w*)\s*\[\s*(?P<n>\d+)\s*\])re");
const Regex _DELETE(R"re(\bdelete\s*(\[\s*\])?\s*(?P<var>[A-Za-z_]\w*)\b)re");
const Regex _DELETE_THIS(R"re(\bdelete\s*(?:\[\s*\])?\s*this\b)re");
const Regex _DESTROY_AT_CALL(R"re(\bdestroy_at\s*(?:<\s*[^>]*\s*>)?\s*\(\s*(?P<ptr>[A-Za-z_]\w*))re");
const Regex _DESTROY_N_CALL(R"re(\bdestroy_n\s*(?:<\s*[^>]*\s*>)?\s*\(\s*(?P<ptr>[A-Za-z_]\w*)\s*,\s*(?P<n>[^,\)]+))re");
const Regex _DTOR_HEAD(R"re((?m)^[ \t]*(?:[A-Za-z_]\w*::)?~(?P<name>[A-Za-z_]\w*)\s*\([^;{}]*\)[^{;]*\{)re", true, false);
const Regex _DYN_CAST_ASSIGN(R"re((?P<var>[A-Za-z_]\w*)\s*=\s*[^;]*\bdynamic_cast\s*<)re");
const Regex _EMBED_DIR(R"re(#\s*embed\b\s*(?P<arg>.*)$)re");
const Regex _ENABLE_SHARED_FROM(R"re(\benable_shared_from_this\b)re");
const Regex _ENDIAN_ASSIGN(R"re(\b(?:auto|int|unsigned|(?:std\s*::\s*)?endian)\s+(?P<var>[A-Za-z_]\w*)\s*=[^;]*\bstd\s*::\s*endian\b)re");
const Regex _ENDIAN_IN_INDEX(R"re(\[\s*[^\]]*\bstd\s*::\s*endian\b)re");
const Regex _EXCHANGE_ASSIGN(R"re(\b(?P<var>[A-Za-z_]\w*)\s*=\s*std\s*::\s*exchange\s*\(\s*(?P<obj>[A-Za-z_]\w*))re");
const Regex _EXCHANGE_CALL(R"re(\bstd\s*::\s*exchange\s*\(\s*(?P<obj>[A-Za-z_]\w*))re");
const Regex _EXCHANGE_IN_INDEX(R"re(\[\s*std\s*::\s*exchange\s*\()re");
const Regex _EXPECTED_ERROR_CALL(R"re(\b(?P<name>[A-Za-z_]\w*)\s*\.\s*error\s*\()re");
const Regex _EXSCAN_CALL(R"re(\bexclusive_scan\s*\(\s*(?P<src>[A-Za-z_]\w*)\s*,[^,]+,\s*(?P<dst>[A-Za-z_]\w*))re");
const Regex _FROM_CHARS_OUT(R"re((?:std\s*::\s*)?from_chars\s*\(\s*[^,]+,\s*[^,]+,\s*(?P<var>[A-Za-z_]\w*))re");
const Regex _FS_REMOVE(R"re((?:std\s*::\s*)?filesystem\s*::\s*remove(?:_all)?\s*\(\s*(?P<p>[A-Za-z_]\w*)\s*\))re");
const Regex _FS_USE_AFTER(R"re((?:std\s*::\s*)?filesystem\s*::\s*(?:exists|file_size|is_regular_file|is_directory|last_write_time|status|canonical|equivalent)\s*\(\s*(?P<p>[A-Za-z_]\w*)\s*\))re");
const Regex _FUTURE_GET(R"re(\b(?P<name>[A-Za-z_]\w*)\s*\.\s*get\s*\()re");
const Regex _FWDLIKE_ASSIGN(R"re(\b(?P<var>[A-Za-z_]\w*)\s*=\s*(?:\(\s*int\s*\))?\s*(?:std\s*::\s*)?forward_like\s*(?:<\s*[^>]*\s*>)?\s*\()re");
const Regex _FWDLIKE_IN_INDEX(R"re(\[\s*(?:\(\s*int\s*\))?\s*(?:std\s*::\s*)?forward_like\s*(?:<\s*[^>]*\s*>)?\s*\()re");
const Regex _FWDLIKE_SRC(R"re(\b(?:std\s*::\s*)?forward_like\s*(?:<\s*[^>]*\s*>)?\s*\(\s*(?P<src>[A-Za-z_]\w*))re");
const Regex _FWD_PUSH_ARG(R"re(\.\s*push_back\s*\(\s*(?P<arg>[A-Za-z_]\w*)\s*\))re");
const Regex _GCD_ASSIGN(R"re(\b(?P<var>[A-Za-z_]\w*)\s*=\s*(?:\(\s*int\s*\))?\s*std\s*::\s*gcd\s*\()re");
const Regex _GCD_IN_INDEX(R"re(\[\s*(?:\(\s*int\s*\))?\s*std\s*::\s*gcd\s*\()re");
const Regex _GET_TERM_BIND(R"re(\b(?P<name>[A-Za-z_]\w*)\s*=\s*(?:std\s*::\s*)?get_terminate\s*\()re");
const Regex _HAZARD_DECL(R"re((?:std\s*::\s*)?\bhazard_pointer\b)re");
const Regex _INIT_LIST_TMP_BEGIN(R"re((?:std\s*::\s*)?initializer_list\s*<[^>]+>\s*\{[^;]*\}\s*\.\s*begin\s*\()re");
const Regex _INIT_MODULE_DISCARDED(R"re(^\s*\b(?:init_module|finit_module|delete_module)\s*\([^;]*\)\s*;\s*$)re");
const Regex _INSCAN_CALL(R"re(\binclusive_scan\s*\(\s*(?P<src>[A-Za-z_]\w*)\s*,[^,]+,\s*(?P<dst>[A-Za-z_]\w*))re");
const Regex _KILLDEP_ASSIGN(R"re(\b(?P<var>[A-Za-z_]\w*)\s*=\s*(?:std\s*::\s*)?kill_dependency\s*\()re");
const Regex _KILLDEP_IN_INDEX(R"re(\[\s*(?:std\s*::\s*)?kill_dependency\s*\()re");
const Regex _LCM_ASSIGN(R"re(\b(?P<var>[A-Za-z_]\w*)\s*=\s*(?:\(\s*int\s*\))?\s*std\s*::\s*lcm\s*\()re");
const Regex _LCM_IN_INDEX(R"re(\[\s*(?:\(\s*int\s*\))?\s*std\s*::\s*lcm\s*\()re");
const Regex _LERP_ASSIGN(R"re(\b(?P<var>[A-Za-z_]\w*)\s*=\s*(?:\(\s*int\s*\))?\s*(?:std\s*::\s*)?lerp\s*\()re");
const Regex _LERP_IN_INDEX(R"re(\[\s*(?:\(\s*int\s*\))?\s*(?:std\s*::\s*)?lerp\s*\()re");
const Regex _LINALG_MATRIX_DECL(R"re((?:std\s*::\s*)?linalg\s*::\s*matrix\s*<[^>]+>\s+(?P<name>[A-Za-z_]\w*))re");
const Regex _LINALG_SCALED(R"re((?:std\s*::\s*)?linalg\s*::\s*(?:scaled|copied)\s*\()re");
const Regex _LOCAL_CHAR_ARRAY(R"re(^\s*(?P<static>static\s+)?(?:const\s+)?char\s+(?P<name>[A-Za-z_]\w*)\s*\[\s*(?P<size>\d+)\s*\]\s*;)re", true, false);
const Regex _LOCAL_DECL(R"re(^\s*(?P<static>static\s+)?(?:const\s+|volatile\s+)?(?:struct\s+\w+|union\s+\w+|enum\s+\w+|(?:unsigned\s+)?(?:char|short|int|long|float|double)|size_t|ssize_t|ptrdiff_t|int\d+_t|uint\d+_t|wchar_t)\s+(?P<name>[A-Za-z_]\w*)(?P<array>\s*\[[^\]]+\])?\s*;)re", true, false);
const Regex _LOCAL_PTR_UNINIT(R"re(^\s*(?:static\s+)?(?:const\s+|volatile\s+)?(?:struct\s+\w+|union\s+\w+|enum\s+\w+|(?:unsigned\s+)?(?:char|short|int|long|float|double)|void|size_t|ssize_t|ptrdiff_t|int\d+_t|uint\d+_t)\s*\*\s*([A-Za-z_]\w*)\s*;)re", true, false);
const Regex _LOCAL_STRUCT_DECL(R"re(^\s*(?P<static>static\s+)?struct\s+\w+\s+(?P<name>[A-Za-z_]\w*)\s*(?P<zero>=\s*\{0\})?\s*;)re", true, false);
const Regex _LOCAL_UNINIT(R"re(^\s*(?:const\s+|volatile\s+)?(?:struct\s+\w+|union\s+\w+|enum\s+\w+|(?:unsigned\s+)?(?:char|short|int|long|float|double)|size_t|ssize_t|ptrdiff_t|int\d+_t|uint\d+_t)\s+([A-Za-z_]\w*)\s*;)re", true, false);
const Regex _LOCAL_WCHAR_ARRAY(R"re(^\s*(?P<static>static\s+)?(?:const\s+)?wchar_t\s+(?P<name>[A-Za-z_]\w*)\s*\[\s*(?P<size>\d+)\s*\]\s*;)re", true, false);
const Regex _MAKE_EPTR_BIND(R"re(\b(?P<name>[A-Za-z_]\w*)\s*=\s*(?:std\s*::\s*)?make_exception_ptr\s*\()re");
const Regex _MAKE_EPTR_INLINE(R"re(\brethrow_exception\s*\(\s*(?:std\s*::\s*)?make_exception_ptr\s*\()re");
const Regex _MIDPOINT_ASSIGN(R"re(\b(?P<var>[A-Za-z_]\w*)\s*=\s*(?:\(\s*int\s*\))?\s*(?:std\s*::\s*)?midpoint\s*\()re");
const Regex _MIDPOINT_IN_INDEX(R"re(\[\s*(?:\(\s*int\s*\))?\s*(?:std\s*::\s*)?midpoint\s*\()re");
const Regex _NEW_ASSIGN(R"re(\b(?P<var>[A-Za-z_]\w*)\s*=\s*new\s+(?P<rest>[^;]+))re");
const Regex _NODISCARD_ATTR(R"re(\[\[\s*nodiscard\s*\]\])re");
const Regex _NODISCARD_DISCARDED(R"re(^\s*(?:std\s*::\s*)?(?P<name>[A-Za-z_]\w*)\s*\([^;]*\)\s*;\s*$)re");
const Regex _NODISCARD_NAME(R"re(\b([A-Za-z_]\w*)\s*\()re");
const Regex _OP_BOOL(R"re(\b(?P<ex>explicit\s+)?operator\s+bool\s*\()re");
const Regex _OUT_PTR_CALL(R"re((?:std\s*::\s*)?(?:inout_ptr|out_ptr)\s*(?:<[^>]+>)?\s*\(\s*(?P<ptr>[A-Za-z_]\w*))re");
const Regex _PACK_INT_MEMBER_ADDR(R"re(\b(?:unsigned\s+)?(?:int|short|long)\s*\*\s*(?P<ptr>[A-Za-z_]\w*)\s*=\s*&\s*(?P<obj>[A-Za-z_]\w*)\s*\.\s*(?P<mem>[A-Za-z_]\w*))re");
const Regex _PACK_PRAGMA_1(R"re(#\s*pragma\s+pack\s*\(\s*(?:push\s*,\s*)?1\s*\))re");
const Regex _PLACE_BUF_DECL(R"re((?:unsigned\s+char|(?:std\s*::\s*)?byte)\s+(?P<name>[A-Za-z_]\w*)\s*\[)re");
const Regex _PLACE_NEW(R"re(\bnew\s*\(\s*(?P<ptr>[A-Za-z_]\w*)\s*\))re");
const Regex _PLACE_PTR_DECL(R"re((?P<typ>unsigned\s+char|(?:std\s*::\s*)?byte|void|[A-Za-z_]\w*)\s*\*+\s*(?P<name>[A-Za-z_]\w*)\b)re");
const Regex _PTR_MEMBER(R"re((?:[A-Za-z_]\w*|char|int|void|short|long)\s*\*+\s*(?P<name>[A-Za-z_]\w*)\s*;)re");
const Regex _RANGES_MATERIALIZE(R"re((?:std\s*::\s*)?(?:vector|string)\s*(?:<[^>]+>)?\s+[A-Za-z_]\w*\s*\([^;]*\.begin\s*\()re");
const Regex _RANGES_TMP_PIPE(R"re((?:std\s*::\s*)?(?:string|vector\s*<[^>]+>)\s*[\({][^;]*\|\s*(?:std\s*::\s*)?(?:ranges\s*::\s*)?views\s*::)re");
const Regex _RANGES_TO_BIND(R"re(\b(?P<name>[A-Za-z_]\w*)\s*=\s*(?:std\s*::\s*)?ranges\s*::\s*to\b)re");
const Regex _RCU_OBJ_DECL(R"re((?:std\s*::\s*)?rcu_obj\s*<[^>]+>\s+(?P<name>[A-Za-z_]\w*))re");
const Regex _REDUCE_ASSIGN(R"re(\b(?P<var>[A-Za-z_]\w*)\s*=\s*(?:\(\s*int\s*\))?\s*std\s*::\s*reduce\s*\()re");
const Regex _REDUCE_IN_INDEX(R"re(\[\s*(?:\(\s*int\s*\))?\s*std\s*::\s*reduce\s*\()re");
const Regex _REF_RET_FN(R"re((?m)^[ \t]*(?:(?:static|inline|constexpr|extern|const)\s+)*(?:const\s+)?(?:std\s*::\s*)?[A-Za-z_]\w*(?:\s*<[^>]*>)?\s*&(?!&)\s*(?P<name>[A-Za-z_]\w*)\s*\((?P<params>[^;{}]*?)\)[^{]*\{)re", true, false);
const Regex _REGEX_CTOR(R"re((?:std\s*::\s*)?\bregex\s+(?P<name>[A-Za-z_]\w*)\s*[\({]\s*(?P<pat>[^)}]+))re");
const Regex _REINTERPRET_CAST(R"re(reinterpret_cast\s*<\s*(?P<target>[^>]+?)\s*>\s*\(\s*(?P<src>[^)]+?)\s*\))re");
const Regex _RETURN_ADDR(R"re(\breturn\s*\(*\s*&\s*\(*\s*([A-Za-z_]\w*)\s*\)*\s*;)re");
const Regex _RETURN_CTOR(R"re(\breturn\s+(?:std\s*::\s*)?[A-Za-z_]\w*(?:\s*<[^>]*>)?\s*[\({])re");
const Regex _RETURN_VAR(R"re(\breturn\s+\(?\s*(?!\*&)([A-Za-z_]\w*)\s*\)?\s*;)re");
const Regex _ROTL_ASSIGN(R"re(\b(?P<var>[A-Za-z_]\w*)\s*=\s*(?:std\s*::\s*)?rot[lr]\s*\()re");
const Regex _ROTL_IN_INDEX(R"re(\[\s*(?:std\s*::\s*)?rot[lr]\s*\()re");
const Regex _SET_TERM_CALL(R"re(\bset_terminate\s*\()re");
const Regex _SHALLOW_PTR_ASSIGN(R"re(\b(?P<dst>[A-Za-z_]\w*)\s*(?:\.|->)\s*(?P<field>[A-Za-z_]\w*)\s*=\s*(?P<src>[A-Za-z_]\w*)\s*(?:\.|->)\s*(?P=field)\b)re");
const Regex _SHARED_FROM_THIS(R"re(\bshared_from_this\s*\()re");
const Regex _SHARED_PTR_DECL(R"re(\b(?:std::)?shared_ptr\s*<[^>]+>\s+(?P<name>[A-Za-z_]\w*)\b)re");
const Regex _SPACESHIP_DEFAULT(R"re(operator\s*<=>\s*\([^)]*\)[^{;]*=\s*default)re");
const Regex _STACKTRACE_ASSIGN(R"re((?P<name>[A-Za-z_]\w*)\s*=\s*(?:std\s*::\s*)?stacktrace\s*::\s*current\s*\()re");
const Regex _STACKTRACE_CURRENT(R"re((?:std\s*::\s*)?stacktrace\s*::\s*current\s*\()re");
const Regex _STACKTRACE_DIRECT_ZERO(R"re((?:std\s*::\s*)?stacktrace\s*::\s*current\s*\(\s*\)\s*(?:\[\s*0\s*\]|\.\s*at\s*\(\s*0\s*\)))re");
const Regex _STD_BIND_CALL(R"re(\bstd::bind\s*\()re");
const Regex _STD_FMT_CALL(R"re(\bstd\s*::\s*(?P<fn>format|print|println)\s*\()re");
const Regex _STD_MOVE(R"re(std\s*::\s*move\s*\(\s*(?P<name>[A-Za-z_]\w*)\s*\))re");
const Regex _STR_CSTR_GET(R"re((?P<raw>[A-Za-z_]\w*)\s*=\s*(?P<s>[A-Za-z_]\w*)\s*\.\s*(?:c_str|data)\s*\(\s*\))re");
const Regex _STR_MUTATE(R"re((?P<s>[A-Za-z_]\w*)\s*(?:\+=|\.\s*(?:clear|append|assign)\s*\())re");
const Regex _STR_OWN_DECL(R"re(\b(?:std::)?string\s+(?P<name>[A-Za-z_]\w*)\b)re");
const Regex _SYNCFS_DISCARDED(R"re(^\s*\bsyncfs\s*\([^;]*\)\s*;\s*$)re");
const Regex _SYNC_FILE_RANGE_DISCARDED(R"re(^\s*\bsync_file_range\s*\([^;]*\)\s*;\s*$)re");
const Regex _SYNC_WAIT_DISCARDED(R"re(^\s*(?:std\s*::\s*(?:execution\s*::\s*)?)?sync_wait\s*\([^;]*\)\s*;\s*$)re");
const Regex _TEXT_ENC_CTOR(R"re((?:std\s*::\s*)?\btext_encoding\s+(?P<name>[A-Za-z_]\w*)\s*[\({]\s*(?P<arg>[^)}]+))re");
const Regex _THROW_IDENT(R"re(\bthrow\s+(?P<ident>[A-Za-z_]\w*)\s*;)re");
const Regex _THROW_NEW(R"re(\bthrow\s+new\b)re");
const Regex _TO_ADDR_BIND(R"re(\b(?P<name>[A-Za-z_]\w*)\s*=\s*(?:std\s*::\s*)?to_address\s*\(\s*(?P<src>[A-Za-z_]\w*))re");
const Regex _TO_ADDR_INLINE_IDX(R"re(\b(?:std\s*::\s*)?to_address\s*\([^)]*\)\s*\[\s*(?P<idx>[^\]]+)\s*\])re");
const Regex _TO_ARRAY_BIND(R"re(\b(?P<name>[A-Za-z_]\w*)\s*=\s*(?:std\s*::\s*)?to_array\s*\(\s*(?P<src>[A-Za-z_]\w*))re");
const Regex _TO_ARRAY_INLINE_IDX(R"re(\b(?:std\s*::\s*)?to_array\s*\([^)]*\)\s*\[\s*(?P<idx>[^\]]+)\s*\])re");
const Regex _TO_CHARS_BUF(R"re((?:std\s*::\s*)?\bto_chars\s*\(\s*(?P<buf>[A-Za-z_]\w*))re");
const Regex _TRED_ASSIGN(R"re(\b(?P<var>[A-Za-z_]\w*)\s*=\s*(?:\(\s*int\s*\))?\s*(?:std\s*::\s*)?transform_reduce\s*\()re");
const Regex _TRED_IN_INDEX(R"re(\[\s*(?:\(\s*int\s*\))?\s*(?:std\s*::\s*)?transform_reduce\s*\()re");
const Regex _TSCAN_CALL(R"re(\b(?:transform_inclusive_scan|transform_exclusive_scan)\s*\(\s*(?P<src>[A-Za-z_]\w*)\s*,[^,]+,\s*(?P<dst>[A-Za-z_]\w*))re");
const Regex _TYPEIDENT_ALIAS(R"re(\busing\s+(?P<alias>[A-Za-z_]\w*)\s*=\s*(?:typename\s+)?(?:std\s*::\s*)?type_identity)re");
const Regex _TYPEIDENT_DIRECT_PTR(R"re((?:std\s*::\s*)?type_identity(?:_t)?\s*<[^>;]+>(?:\s*::\s*type)?\s*\*\s*(?P<ptr>[A-Za-z_]\w*))re");
const Regex _UICOPY_CALL(R"re(\b(?:uninitialized_copy|uninitialized_move)\s*\(\s*(?P<src>[A-Za-z_]\w*)\s*,[^,]+,\s*(?P<dst>[A-Za-z_]\w*))re");
const Regex _UIFILL_CALL(R"re(\b(?:uninitialized_fill|uninitialized_default_construct)\s*\(\s*(?P<dst>[A-Za-z_]\w*))re");
const Regex _UNCAUGHT_BOOL_IF(R"re(\bif\s*\(\s*(?:std\s*::\s*)?uncaught_exceptions\s*\(\s*\)\s*\))re");
const Regex _UNCAUGHT_SAVED(R"re(\b(?:std\s*::\s*)?uncaught_exceptions\s*\(\s*\)\s*(?:==|!=|<|>|<=|>=)|(?:==|!=|<|>|<=|>=)\s*(?:std\s*::\s*)?uncaught_exceptions\s*\(\s*\))re");
const Regex _UNIQUE_NULL(R"re((?P<up>[A-Za-z_]\w*)\s*=\s*(?:nullptr|NULL|0)\s*;)re");
const Regex _UNIQUE_PTR_DECL(R"re(\b(?:std::)?unique_ptr\s*<[^>]+>\s+(?P<name>[A-Za-z_]\w*)\b)re");
const Regex _UNIQUE_RAW_GET(R"re((?P<raw>[A-Za-z_]\w*)\s*=\s*(?P<up>[A-Za-z_]\w*)\s*\.\s*get\s*\(\s*\))re");
const Regex _UNIQUE_RELEASE(R"re((?P<up>[A-Za-z_]\w*)\s*\.\s*release\s*\()re");
const Regex _UNIQUE_RESET(R"re((?P<up>[A-Za-z_]\w*)\s*\.\s*reset\s*\()re");
const Regex _UVALUE_CALL(R"re(\buninitialized_value_construct\s*\(\s*(?P<dst>[A-Za-z_]\w*))re");
const Regex _VOL_CAST_WRITE(R"re(\*\s*\(\s*(?:const\s+)?(?:(?:unsigned|signed)\s+)?(?:int|short|long|char|bool|void|float|double)\s*\*\s*\)\s*&\s*(?P<name>[A-Za-z_]\w*)\s*=)re");
const Regex _VOL_LOCAL(R"re((?:^|[;{(])\s*(?:(?:static|extern|auto|register|const)\s+)*volatile\s+(?:(?:unsigned|signed)\s+)?(?:int|short|long|char|bool|float|double)\s+(?P<name>[A-Za-z_]\w*))re", true, false);
std::optional<Match> match_at_start_line(const Regex& re, std::string_view s) {
    auto m = re.search_match(s);
    if (!m || m->spans.empty() || m->spans[0].first != 0) return std::nullopt;
    return m;
}

std::pair<std::set<std::string>, std::set<std::string>> locals_in_fn(const FunctionInfo& fn) {
    std::set<std::string> params;
    for (auto& [typ, name] : fn.params)
        if (!name.empty()) params.insert(name);
    std::set<std::string> scalars, arrays;
    for (auto& m : _LOCAL_DECL.finditer(fn.body)) {
        if (!m.named("static").empty()) continue;
        auto name = m.named("name");
        if (params.count(name) || KW.count(name)) continue;
        if (!m.named("array").empty()) arrays.insert(name);
        else scalars.insert(name);
    }
    return {scalars, arrays};
}

std::set<std::string> cxx_local_strings(const FunctionInfo& fn) {
    std::set<std::string> params;
    for (auto& [typ, name] : fn.params)
        if (!name.empty()) params.insert(name);
    std::set<std::string> names;
    for (auto& m : _CXX_LOCAL_STRING.finditer(fn.body)) {
        if (!m.named("static").empty()) continue;
        auto name = m.named("name");
        if (params.count(name) || KW.count(name)) continue;
        names.insert(name);
    }
    return names;
}

std::set<std::string> cxx_local_wstrings(const FunctionInfo& fn) {
    std::set<std::string> params;
    for (auto& [typ, name] : fn.params)
        if (!name.empty()) params.insert(name);
    std::set<std::string> names;
    for (auto& m : _CXX_WSTRING_DECL.finditer(fn.body)) {
        if (!m.named("static").empty()) continue;
        auto name = m.named("name");
        if (params.count(name) || KW.count(name)) continue;
        names.insert(name);
    }
    return names;
}

std::set<std::string> cxx_local_u8strings(const FunctionInfo& fn) {
    std::set<std::string> params;
    for (auto& [typ, name] : fn.params)
        if (!name.empty()) params.insert(name);
    std::set<std::string> names;
    for (auto& m : _CXX_U8STRING_DECL.finditer(fn.body)) {
        if (!m.named("static").empty()) continue;
        auto name = m.named("name");
        if (params.count(name) || KW.count(name)) continue;
        names.insert(name);
    }
    return names;
}

bool move_use_after(std::string_view name, std::string_view ln) {
    auto stripped = re_sub_pat(R"(std\s*::\s*move\s*\(\s*[A-Za-z_]\w*\s*\))", "", ln);
    auto v = re_escape(name);
    if (re_search("\\b" + v + "\\.", stripped)) return true;
    if (re_search("\\b" + v + "\\s*\\(", stripped)) return true;
    return re_search("\\b" + v + "\\s*=(?!=)", stripped);
}

void cxx_index_token_scan(const std::vector<std::string>& lines, std::string_view rel,
                          const std::vector<FunctionInfo>& funcs, std::vector<Finding>& out,
                          const Regex& token_re, const Regex& in_index_re, const Regex& assign_re,
                          std::string_view cls, std::string_view message) {
    for (auto& fn : funcs) {
        if (!token_re.search(fn.body)) continue;
        std::set<std::string> assigned;
        auto start = fn.span.first;
        auto body_lines = split_lines(fn.body);
        for (int i = 0; i < static_cast<int>(body_lines.size()); ++i) {
            auto& ln = body_lines[static_cast<std::size_t>(i)];
            for (auto& m : assign_re.finditer(ln)) assigned.insert(m.named("var"));
            if (in_index_re.search(ln)) {
                report(out, rel, fn.name, start + i, cls, std::string(message), lines);
                return;
            }
            for (auto& m : _CXX_SUBSCRIPT.finditer(ln)) {
                auto idx = strip(m.named("idx"));
                if (!assigned.count(idx)) continue;
                if (byteswap_idx_guarded(idx, fn.body)) continue;
                report(out, rel, fn.name, start + i, cls,
                       m.named("cont") + "[" + idx + "] indexes with " + std::string(cls)
                           + " without a range check",
                       lines);
                return;
            }
        }
    }
}

void cxx_token_subscript_unguarded(const std::vector<std::string>& lines, std::string_view rel,
                                   const std::vector<FunctionInfo>& funcs, std::vector<Finding>& out,
                                   const Regex& token_re, std::string_view cls, std::string_view message) {
    for (auto& fn : funcs) {
        if (!token_re.search(fn.body)) continue;
        auto start = fn.span.first;
        auto body_lines = split_lines(fn.body);
        for (int i = 0; i < static_cast<int>(body_lines.size()); ++i) {
            auto& ln = body_lines[static_cast<std::size_t>(i)];
            for (auto& m : _CXX_SUBSCRIPT.finditer(ln)) {
                if (!toarr_idx_bad(strip(m.named("idx")), std::nullopt, fn.body)) continue;
                report(out, rel, fn.name, start + i, cls, std::string(message), lines);
                return;
            }
        }
    }
}

void cxx_find_no_end(const std::vector<std::string>& lines, std::string_view rel,
                     const std::vector<FunctionInfo>& funcs, std::vector<Finding>& out,
                     const Regex& decl, std::string_view cls, std::string_view msg_suffix) {
    for (auto& fn : funcs) {
        auto names = names_of(decl, fn.body);
        if (names.empty()) continue;
        auto start = fn.span.first;
        auto body_lines = split_lines(fn.body);
        for (auto& name : names) {
            if (re_search("\\b" + re_escape(name) + "\\s*\\.\\s*end\\s*\\(", fn.body)) continue;
            auto n = re_escape(name);
            for (int i = 0; i < static_cast<int>(body_lines.size()); ++i) {
                auto& ln = body_lines[static_cast<std::size_t>(i)];
                if (!(re_search("\\*\\s*" + n + "\\s*\\.\\s*find\\s*\\(", ln)
                      || re_search("\\b" + n + "\\s*\\.\\s*find\\s*\\(", ln)))
                    continue;
                report(out, rel, fn.name, start + i, cls, name + std::string(msg_suffix), lines);
                return;
            }
        }
    }
}

void cxx_at_no_contains(const std::vector<std::string>& lines, std::string_view rel,
                        const std::vector<FunctionInfo>& funcs, std::vector<Finding>& out,
                        const Regex& decl, std::string_view cls, std::string_view msg_suffix) {
    for (auto& fn : funcs) {
        auto names = names_of(decl, fn.body);
        if (names.empty()) continue;
        auto start = fn.span.first;
        auto body_lines = split_lines(fn.body);
        for (auto& name : names) {
            auto n = re_escape(name);
            if (re_search("\\b" + n + "\\s*\\.\\s*(?:contains|count|find)\\s*\\(", fn.body)) continue;
            for (int i = 0; i < static_cast<int>(body_lines.size()); ++i) {
                auto& ln = body_lines[static_cast<std::size_t>(i)];
                if (!re_search("\\b" + n + "\\s*\\.\\s*at\\s*\\(", ln)) continue;
                report(out, rel, fn.name, start + i, cls, name + std::string(msg_suffix), lines);
                return;
            }
        }
    }
}

void cxx_method_no_empty(const std::vector<std::string>& lines, std::string_view rel,
                         const std::vector<FunctionInfo>& funcs, std::vector<Finding>& out,
                         const Regex& decl, std::string_view methods, std::string_view cls,
                         std::string_view msg_suffix) {
    for (auto& fn : funcs) {
        auto names = names_of(decl, fn.body);
        if (names.empty()) continue;
        auto start = fn.span.first;
        auto body_lines = split_lines(fn.body);
        for (auto& name : names) {
            auto n = re_escape(name);
            if (re_search("\\b" + n + "\\s*\\.\\s*empty\\s*\\(", fn.body)) continue;
            for (int i = 0; i < static_cast<int>(body_lines.size()); ++i) {
                auto& ln = body_lines[static_cast<std::size_t>(i)];
                if (!re_search("\\b" + n + "\\s*\\.\\s*(?:" + std::string(methods) + ")\\s*\\(", ln)) continue;
                report(out, rel, fn.name, start + i, cls, name + std::string(msg_suffix), lines);
                return;
            }
        }
    }
}

void cxx_first_token(const std::vector<std::string>& lines, std::string_view rel,
                     const std::vector<FunctionInfo>& funcs, std::vector<Finding>& out,
                     const Regex& token, std::string_view cls, std::string_view message) {
    for (auto& fn : funcs) {
        auto start = fn.span.first;
        auto body_lines = split_lines(fn.body);
        for (int i = 0; i < static_cast<int>(body_lines.size()); ++i) {
            if (!token.search(body_lines[static_cast<std::size_t>(i)])) continue;
            report(out, rel, fn.name, start + i, cls, std::string(message), lines);
            return;
        }
    }
}

void cxx_token_unless(const std::vector<std::string>& lines, std::string_view rel,
                      const std::vector<FunctionInfo>& funcs, std::vector<Finding>& out,
                      const Regex& token, std::string_view unless_pat, std::string_view cls,
                      std::string_view message) {
    for (auto& fn : funcs) {
        if (!token.search(fn.body)) continue;
        if (re_search(unless_pat, fn.body)) continue;
        auto start = fn.span.first;
        auto body_lines = split_lines(fn.body);
        for (int i = 0; i < static_cast<int>(body_lines.size()); ++i) {
            if (!token.search(body_lines[static_cast<std::size_t>(i)])) continue;
            report(out, rel, fn.name, start + i, cls, std::string(message), lines);
            return;
        }
    }
}

void cxx_sub_no_size(const std::vector<std::string>& lines, std::string_view rel,
                     const std::vector<FunctionInfo>& funcs, std::vector<Finding>& out,
                     const Regex& decl, std::string_view cls, std::string_view msg_mid) {
    for (auto& fn : funcs) {
        auto names = names_of(decl, fn.body);
        if (names.empty()) continue;
        auto start = fn.span.first;
        auto body_lines = split_lines(fn.body);
        for (int i = 0; i < static_cast<int>(body_lines.size()); ++i) {
            auto& ln = body_lines[static_cast<std::size_t>(i)];
            for (auto& m : _CXX_SUBSCRIPT.finditer(ln)) {
                auto cont = m.named("cont");
                if (!names.count(cont)) continue;
                auto idx = strip(m.named("idx"));
                auto c = re_escape(cont), iv = re_escape(idx);
                if (re_search("\\bif\\s*\\(\\s*" + iv + "\\s*<\\s*" + c + "\\s*\\.\\s*(?:size|length)\\s*\\(\\s*\\)", fn.body))
                    continue;
                if (re_search("\\bif\\s*\\(\\s*" + c + "\\s*\\.\\s*(?:size|length)\\s*\\(\\s*\\)\\s*>\\s*" + iv, fn.body))
                    continue;
                report(out, rel, fn.name, start + i, cls,
                       cont + "[" + idx + "]" + std::string(msg_mid), lines);
                return;
            }
        }
    }
}

// Python-parity helpers for the checkers below (prism/checkers.py twins).
bool rx_search(const std::string& pat, std::string_view s) { return re_search(pat, s); }

std::optional<Match> rx_match_start(const std::string& pat, std::string_view s) {
    auto m = re_search_match(pat, s);
    if (!m || m->spans.empty() || m->spans[0].first != 0) return std::nullopt;
    return m;
}


bool all_digits(std::string_view s) {
    if (s.empty()) return false;
    for (unsigned char ch : s)
        if (!std::isdigit(ch)) return false;
    return true;
}

// `*p`, `p[` or `p->` on the line.
bool ptr_deref_on(std::string_view ptr, std::string_view ln) {
    auto v = re_escape(ptr);
    return rx_search("(?:\\*\\s*" + v + "\\b|" + v + "\\s*\\[|" + v + "\\s*->)", ln);
}


// File-scope `int x;` / `unsigned x;` names, sorted (prism/checkers.py
// _file_scope_int_globals; checkers_core.cpp keeps its own copy).
std::set<std::string> file_int_globals(const std::vector<std::string>& lines) {
    static const Regex ig(R"(^int\s+(?P<name>[A-Za-z_]\w*)\s*;)");
    static const Regex ug(R"(^unsigned\s+(?P<name>[A-Za-z_]\w*)\s*;)");
    int depth = 0;
    std::set<std::string> g;
    for (auto& line : lines) {
        auto s = strip(line);
        auto delta = static_cast<int>(std::count(s.begin(), s.end(), '{') - std::count(s.begin(), s.end(), '}'));
        if (s.empty() || s[0] == '#') {
            depth += delta;
            continue;
        }
        if (depth == 0) {
            if (auto m = match_at_start_line(ig, line)) g.insert(m->named("name"));
            else if (auto m2 = match_at_start_line(ug, line)) g.insert(m2->named("name"));
        }
        depth += delta;
    }
    return g;
}


// True if placement storage is an unsigned char/byte buffer or void*; false
// if `name` is a typed object pointer; nullopt if unknown.
std::optional<bool> place_storage_ok(const std::string& name, const FunctionInfo& fn) {
    for (auto& [typ, pname] : fn.params) {
        if (pname != name) continue;
        if (typ.find('*') == std::string::npos) return std::nullopt;
        return allowed_reinterp_pointee(typ);
    }
    for (auto& m : _PLACE_BUF_DECL.finditer(fn.body))
        if (m.named("name") == name) return true;
    std::optional<std::string> last_typ;
    for (auto& m : _PLACE_PTR_DECL.finditer(fn.body))
        if (m.named("name") == name) last_typ = m.named("typ");
    if (!last_typ) return std::nullopt;
    auto t = lower_copy(strip(re_sub_pat(R"(\s+)", " ", *last_typ)));
    t = re_sub_pat(R"(std::)", "", t);
    return t == "unsigned char" || t == "byte" || t == "void";
}

std::set<std::string> cxx_local_scalar_names(const FunctionInfo& fn) {
    static const Regex decl(
        R"(\b(?:(?:unsigned|signed|const|volatile)\s+)*(?:int|short|long|char|bool|float|double)\s+(?P<name>[A-Za-z_]\w*)\b)");
    std::set<std::string> params, names;
    for (auto& [typ, name] : fn.params)
        if (!name.empty()) params.insert(name);
    for (auto& m : decl.finditer(fn.body))
        if (!params.count(m.named("name"))) names.insert(m.named("name"));
    return names;
}


// Report the first line where a name recorded by `rec_re` (group `grp`, last
// line seen) is used again, not on a line that `rec_re` also matches.
bool report_use_after(const std::vector<std::string>& lines, std::string_view rel, const FunctionInfo& fn,
                      const Regex& rec_re, const char* grp, std::string_view cls, const std::string& what,
                      std::vector<Finding>& out) {
    auto bl = split_lines(fn.body);
    std::vector<std::pair<std::string, int>> recs;  // insertion order, last line
    for (int i = 0; i < static_cast<int>(bl.size()); ++i)
        for (auto& m : rec_re.finditer(bl[static_cast<std::size_t>(i)])) {
            auto v = m.named(grp);
            auto it = std::find_if(recs.begin(), recs.end(), [&](auto& p) { return p.first == v; });
            if (it == recs.end()) recs.emplace_back(v, i);
            else it->second = i;
        }
    if (recs.empty()) return false;
    for (int i = 0; i < static_cast<int>(bl.size()); ++i) {
        auto& ln = bl[static_cast<std::size_t>(i)];
        for (auto& [var, prev] : recs) {
            if (i <= prev) continue;
            if (!rx_search("\\b" + re_escape(var) + "\\b", ln)) continue;
            if (rec_re.search(ln)) continue;
            report(out, rel, fn.name, fn.span.first + i, cls, var + what, lines);
            return true;
        }
    }
    return false;
}


// Ordered name -> value map with Python dict semantics (first insertion
// keeps its position, the last assignment wins).
using OrderedBinds = std::vector<std::pair<std::string, std::string>>;

void bind_set(OrderedBinds& b, const std::string& k, const std::string& v) {
    for (auto& [kk, vv] : b)
        if (kk == k) {
            vv = v;
            return;
        }
    b.emplace_back(k, v);
}

const std::string* bind_get(const OrderedBinds& b, const std::string& k) {
    for (auto& [kk, vv] : b)
        if (kk == k) return &vv;
    return nullptr;
}

std::map<std::string, int> c_array_sizes(std::string_view body) {
    std::map<std::string, int> sizes;
    for (auto& m : _C_ARRAY_SIZE.finditer(body)) {
        try {
            sizes[m.named("name")] = std::stoi(m.named("n"));
        } catch (...) {
        }
    }
    return sizes;
}

std::optional<int> size_of(const std::map<std::string, int>& sizes, const std::string& k) {
    auto it = sizes.find(k);
    if (it == sizes.end()) return std::nullopt;
    return it->second;
}

// Return borrows a local (w/u8)string: prism/checkers.py
// _cxx_wstring_view / _cxx_u8string_view.
void view_of_local_return(const std::vector<std::string>& lines, std::string_view rel,
                          const std::vector<FunctionInfo>& funcs, std::vector<Finding>& out,
                          const char* token_pat, const char* view_word_pat,
                          const std::function<std::set<std::string>(const FunctionInfo&)>& locals_of,
                          const Regex& view_bind, const Regex& return_view, std::string_view cls,
                          const std::string& what) {
    for (auto& fn : funcs) {
        if (!rx_search(token_pat, fn.signature + "\n" + fn.return_type + "\n" + fn.body)) continue;
        auto local = locals_of(fn);
        if (local.empty()) continue;
        std::set<std::string> view_from_local;
        for (auto& m : view_bind.finditer(fn.body))
            if (local.count(m.named("src"))) view_from_local.insert(m.named("name"));
        bool ret_is_view = rx_search(view_word_pat, fn.return_type);
        auto start = fn.span.first;
        auto bl = split_lines(fn.body);
        for (int i = 0; i < static_cast<int>(bl.size()); ++i) {
            auto& ln = bl[static_cast<std::size_t>(i)];
            std::string hit;
            if (auto sm = _RETURN_VAR.search_match(ln)) {
                auto name = sm->group(1);
                if (view_from_local.count(name)) hit = name;
                else if (local.count(name) && ret_is_view) hit = name;
            }
            if (hit.empty())
                if (auto sm = return_view.search_match(ln); sm && local.count(sm->named("name")))
                    hit = sm->named("name");
            if (hit.empty()) continue;
            report(out, rel, fn.name, start + i, cls, what + hit, lines);
            return;
        }
    }
}


// Lambda body after a capture list ending at `capture_end`, if any.
std::optional<std::string> lambda_body_from(std::string_view text, std::size_t capture_end) {
    static const Regex head(R"(\s*(?:\([^;{}]*\))?\s*(?:mutable\s*)?\{)");
    auto rest = text.substr(capture_end);
    auto m = head.search_match(rest);
    if (!m || m->spans[0].first != 0) return std::nullopt;
    auto brace = static_cast<int>(capture_end) + m->spans[0].second - 1;
    auto close = match_brace(text, brace);
    if (close < 0) return std::nullopt;
    return std::string(text.substr(static_cast<std::size_t>(brace + 1),
                                   static_cast<std::size_t>(close - brace - 1)));
}

std::string lambda_capture_kind(const std::string& capture) {
    auto cap = re_sub_pat(R"(\s+)", "", capture);
    if (cap == "this" || cap.starts_with("this,")) return "this";
    if (cap == "=" || cap.starts_with("=,")) return "eq";
    return "other";
}

std::set<std::string> lambda_idents(std::string_view body) {
    static const Regex ident(R"(\b([A-Za-z_]\w*)\b(?!\s*\())");
    std::set<std::string> names;
    for (auto& m : ident.finditer(body))
        if (!LAMBDA_SKIP.count(m.group(1))) names.insert(m.group(1));
    return names;
}

bool this_capture_hit(const std::string& capture, const std::optional<std::string>& lam,
                      const std::set<std::string>& locals_ok, const std::optional<std::set<std::string>>& members) {
    auto kind = lambda_capture_kind(capture);
    if (kind == "this") return true;
    if (kind != "eq" || !lam) return false;
    if (rx_search(R"(\bthis\b)", *lam)) return true;
    if (!members) return false;
    for (auto& u : lambda_idents(*lam))
        if (!locals_ok.count(u) && members->count(u)) return true;
    return false;
}

std::set<std::string> class_member_names(std::string_view cbody) {
    std::set<std::string> names;
    for (auto& m : _PTR_MEMBER.finditer(cbody)) names.insert(m.named("name"));
    for (auto& m : _CXX_SCALAR_MEMBER.finditer(cbody)) names.insert(m.named("name"));
    return names;
}

// Member names when fn is defined inside a class/struct, else nullopt.
std::optional<std::set<std::string>> fn_class_members(const FunctionInfo& fn, std::string_view stripped) {
    for (auto& m : _CLASS_DEF.finditer(stripped)) {
        int brace = m.spans[0].second - 1;
        int close = match_brace(stripped, brace);
        if (close < 0) continue;
        int start_line = static_cast<int>(std::count(stripped.begin(), stripped.begin() + brace, '\n')) + 1;
        int end_line = static_cast<int>(std::count(stripped.begin(), stripped.begin() + close, '\n')) + 1;
        if (!(start_line <= fn.line && fn.line <= end_line)) continue;
        return class_member_names(stripped.substr(static_cast<std::size_t>(brace + 1),
                                                  static_cast<std::size_t>(close - brace - 1)));
    }
    return std::nullopt;
}


void _cxx_new_delete(const std::vector<std::string>& lines, std::string_view rel,
                     const std::vector<FunctionInfo>& funcs, std::vector<Finding>& out) {
    for (auto& fn : funcs) {
        auto start = fn.span.first;
        auto end = fn.span.second;
        if (start <= 0 || end < start) continue;
        std::vector<std::string> chunk;
        for (int i = start - 1; i < end && i < static_cast<int>(lines.size()); ++i)
            chunk.push_back(lines[static_cast<std::size_t>(i)]);
        std::map<std::string, bool> new_kind;
        for (auto& ln : chunk) {
            auto m = _NEW_ASSIGN.search_match(ln);
            if (!m) continue;
            auto rest = m->named("rest");
            auto par = rest.find('(');
            auto head = par == std::string::npos ? rest : rest.substr(0, par);
            new_kind[m->named("var")] = head.find('[') != std::string::npos;
        }
        std::set<std::pair<std::string, int>> seen;
        for (int i = 0; i < static_cast<int>(chunk.size()); ++i) {
            auto m = _DELETE.search_match(chunk[static_cast<std::size_t>(i)]);
            if (!m) continue;
            auto var = m->named("var");
            auto it = new_kind.find(var);
            if (it == new_kind.end()) continue;
            bool is_array = it->second;
            bool del_array = !m->group(1).empty();
            if (is_array == del_array) continue;
            if (!seen.insert({var, i}).second) continue;
            auto want = is_array ? "delete[]" : "delete";
            auto got = del_array ? "delete[]" : "delete";
            report(out, rel, fn.name, start + i, "MEM-NEW-DELETE",
                   var + " allocated with new" + std::string(is_array ? "[]" : "")
                       + " but freed with " + got + "; expected " + want,
                   lines);
        }
    }
}

void _cxx_use_after_move(const std::vector<std::string>& lines, std::string_view rel,
                         const std::vector<FunctionInfo>& funcs, std::vector<Finding>& out) {
    for (auto& fn : funcs) {
        auto body_lines = split_lines(fn.body);
        std::map<std::string, int> last_move;
        for (int i = 0; i < static_cast<int>(body_lines.size()); ++i)
            for (auto& m : _STD_MOVE.finditer(body_lines[static_cast<std::size_t>(i)]))
                last_move[m.named("name")] = i;
        for (auto& [name, move_i] : last_move) {
            for (int j = move_i + 1; j < static_cast<int>(body_lines.size()); ++j) {
                if (!move_use_after(name, body_lines[static_cast<std::size_t>(j)])) continue;
                report(out, rel, fn.name, fn.span.first + j, "CXX-USE-AFTER-MOVE",
                       name + " used after std::move", lines);
                break;
            }
        }
    }
}

void _cxx_self_assign(const std::vector<std::string>& lines, std::string_view rel,
                      const std::vector<FunctionInfo>& funcs, std::vector<Finding>& out) {
    for (auto& fn : funcs) {
        auto body_lines = split_lines(fn.body);
        auto self_param = first_ref_ptr_param(fn);
        if (!self_param) continue;
        std::vector<std::string> copy_sources;
        for (auto& [typ, name] : fn.params) {
            if (name.empty() || name == *self_param) continue;
            bool hit = false;
            for (auto& ln : body_lines)
                if (copy_from_param_line(name, ln)) { hit = true; break; }
            if (hit) copy_sources.push_back(name);
        }
        if (copy_sources.empty()) continue;
        if (has_self_assign_guard(*self_param, fn.body)) continue;
        int delete_i = -1;
        for (int i = 0; i < static_cast<int>(body_lines.size()); ++i)
            if (_DELETE.search(body_lines[static_cast<std::size_t>(i)])) { delete_i = i; break; }
        if (delete_i < 0) continue;
        int copy_i = -1;
        for (int i = delete_i + 1; i < static_cast<int>(body_lines.size()); ++i) {
            for (auto& p : copy_sources)
                if (copy_from_param_line(p, body_lines[static_cast<std::size_t>(i)])) {
                    copy_i = i;
                    break;
                }
            if (copy_i >= 0) break;
        }
        if (copy_i < 0) continue;
        report(out, rel, fn.name, fn.span.first + delete_i, "CXX-SELF-ASSIGN",
               "delete before copy from other without self-assignment guard", lines);
    }
}

void _cxx_dangling_ref(const std::vector<std::string>& lines, std::string_view rel,
                       const std::vector<FunctionInfo>& funcs, std::vector<Finding>& out) {
    for (auto& fn : funcs) {
        if (!_CXX_VIEW_RET.search(fn.return_type)) continue;
        auto local_str = cxx_local_strings(fn);
        if (local_str.empty()) continue;
        auto body_lines = split_lines(fn.body);
        auto start = fn.span.first;
        for (int i = 0; i < static_cast<int>(body_lines.size()); ++i) {
            auto& ln = body_lines[static_cast<std::size_t>(i)];
            std::string hit;
            if (auto m = _RETURN_VAR.search_match(ln); m && local_str.count(m->group(1)))
                hit = m->group(1);
            else if (auto m2 = _CXX_RETURN_VIEW_OF.search_match(ln); m2 && local_str.count(m2->named("name")))
                hit = m2->named("name");
            if (hit.empty()) continue;
            report(out, rel, fn.name, start + i, "CXX-DANGLING-REF",
                   "return borrows local string " + hit, lines);
            break;
        }
    }
}

void _cxx_iter_invalid(const std::vector<std::string>& lines, std::string_view rel,
                       const std::vector<FunctionInfo>& funcs, std::vector<Finding>& out) {
    for (auto& fn : funcs) {
        auto hive_names = names_of(_CXX_HIVE_DECL, fn.body);
        for (auto& [typ, pname] : fn.params)
            if (!pname.empty() && re_search(R"(\bhive\s*<)", typ)) hive_names.insert(pname);
        auto body_lines = split_lines(fn.body);
        auto start = fn.span.first;
        std::set<std::string> begin_containers;
        for (int i = 0; i < static_cast<int>(body_lines.size()); ++i) {
            auto& ln = body_lines[static_cast<std::size_t>(i)];
            if (ln.find('=') != std::string::npos && ln.find(".begin(") != std::string::npos)
                for (auto& m : _CXX_BEGIN_CALL.finditer(ln))
                    begin_containers.insert(re_sub_pat(R"(\s+)", "", m.named("container")));
            for (auto& m : _CXX_CONTAINER_MUTATE.finditer(ln)) {
                auto cont = re_sub_pat(R"(\s+)", "", m.named("container"));
                if (!begin_containers.count(cont) || hive_names.count(cont)) continue;
                report(out, rel, fn.name, start + i, "CXX-ITERATOR-INVALID",
                       cont + " mutated after .begin() iterator taken", lines);
                return;
            }
        }
    }
}

void _cxx_virtual_in_ctor(std::string_view text, const std::vector<std::string>& lines,
                          std::string_view rel, const std::vector<FunctionInfo>& funcs,
                          std::vector<Finding>& out) {
    if (std::string(text).find("virtual") == std::string::npos) return;
    std::set<std::string> virtuals;
    for (auto& m : _CXX_VIRTUAL.finditer(text)) virtuals.insert(m.group(1));
    if (virtuals.empty()) return;
    std::set<std::string> type_names;
    for (auto& m : _CXX_TYPE.finditer(text)) type_names.insert(m.group(1));
    std::set<std::string> reported;
    for (auto& fn : funcs) {
        bool is_ctor = type_names.count(fn.name) || fn.name.find("::") != std::string::npos;
        bool fallback = lower_copy(fn.name).find("ctor") != std::string::npos;
        if (!is_ctor && !fallback) continue;
        std::set<std::string> calls;
        for (auto& m : _CXX_CALL.finditer(fn.body)) calls.insert(m.group(1));
        std::string callee;
        for (auto& c : calls)
            if (virtuals.count(c)) { callee = c; break; }
        if (callee.empty() || reported.count(fn.name)) continue;
        reported.insert(fn.name);
        auto start = fn.span.first;
        int line = start;
        auto body_lines = split_lines(fn.body);
        for (int i = 0; i < static_cast<int>(body_lines.size()); ++i)
            if (re_search("(?:\\b(?:this\\s*->\\s*)?" + re_escape(callee) + "\\s*\\()", body_lines[static_cast<std::size_t>(i)])) {
                line = start + i;
                break;
            }
        report(out, rel, fn.name, line, "CXX-VIRTUAL-IN-CTOR",
               "constructor calls virtual " + callee + "()", lines);
    }
}

void _cxx_exception_leak(const std::vector<std::string>& lines, std::string_view rel,
                         const std::vector<FunctionInfo>& funcs, std::vector<Finding>& out) {
    for (auto& fn : funcs) {
        auto body_lines = split_lines(fn.body);
        std::set<std::string> reported;
        for (int i = 0; i < static_cast<int>(body_lines.size()); ++i) {
            auto m = _NEW_ASSIGN.search_match(body_lines[static_cast<std::size_t>(i)]);
            if (!m) continue;
            auto var = m->named("var");
            if (reported.count(var)) continue;
            if (re_search(R"(\b(?:unique_ptr|shared_ptr)\b)", body_lines[static_cast<std::size_t>(i)])) continue;
            for (int j = i + 1; j < static_cast<int>(body_lines.size()); ++j) {
                auto& later = body_lines[static_cast<std::size_t>(j)];
                if (re_search("\\bdelete\\s*(?:\\[\\s*\\])?\\s*" + re_escape(var) + "\\b", later)) break;
                if (re_search(R"(\b(?:unique_ptr|shared_ptr)\b)", later)) break;
                bool hit = false;
                for (auto& cm : _CXX_CALL.finditer(later)) {
                    auto callee = cm.group(1);
                    if (KW.count(callee) || callee == "delete") continue;
                    reported.insert(var);
                    report(out, rel, fn.name, fn.span.first + j, "CXX-EXCEPTION-LEAK",
                           var + " allocated with new but call precedes delete; may leak on throw",
                           lines);
                    hit = true;
                    break;
                }
                if (hit) break;
            }
        }
    }
}

void _cxx_throw_destructor(std::string_view stripped, const std::vector<std::string>& lines,
                           std::string_view rel, std::vector<Finding>& out) {
    std::set<std::string> reported;
    for (auto& m : _DTOR_HEAD.finditer(stripped)) {
        auto name = m.named("name");
        if (reported.count(name)) continue;
        int brace = m.spans.empty() ? -1 : m.spans[0].second - 1;
        // find '{' after match
        auto pos = stripped.find('{', static_cast<std::size_t>(m.spans[0].first));
        if (pos == std::string_view::npos) continue;
        brace = static_cast<int>(pos);
        auto close = match_brace(stripped, brace);
        if (close < 0) continue;
        auto body = stripped.substr(static_cast<std::size_t>(brace + 1),
                                    static_cast<std::size_t>(close - brace - 1));
        auto tm = re_search_match(R"(\bthrow\b)", body);
        if (!tm) continue;
        reported.insert(name);
        int line = 1 + static_cast<int>(std::count(stripped.begin(), stripped.begin() + brace + 1 + tm->spans[0].first, '\n'));
        report(out, rel, "~" + name, line, "CXX-THROW-DESTRUCTOR",
               "destructor ~" + name + "() throws", lines);
    }
}

void _cxx_throw_spec(std::string_view stripped, const std::vector<std::string>& lines,
                     std::string_view rel, std::vector<Finding>& out) {
    std::set<std::string> reported;
    for (auto& m : _CXX_THROW_SPEC.finditer(stripped)) {
        auto name = m.named("name");
        if (THROW_SPEC_SKIP.count(name) || reported.count(name)) continue;
        reported.insert(name);
        int line = 1 + static_cast<int>(std::count(stripped.begin(), stripped.begin() + m.spans[0].first, '\n'));
        report(out, rel, name, line, "CXX-THROW-SPEC",
               name + " uses a dynamic exception specification", lines);
    }
}

void _cxx_slicing(std::string_view stripped, const std::vector<std::string>& lines,
                  std::string_view rel, const std::vector<FunctionInfo>& funcs,
                  std::vector<Finding>& out) {
    std::map<std::string, std::set<std::string>> inherit;
    for (auto& m : _CXX_INHERIT.finditer(stripped))
        inherit[m.named("derived")].insert(m.named("base"));
    if (inherit.empty()) return;
    std::set<std::string> bases;
    for (auto& [d, bs] : inherit) bases.insert(bs.begin(), bs.end());
    std::map<std::string, std::vector<std::pair<int, std::string>>> slicers;
    for (auto& fn : funcs) {
        std::vector<std::pair<int, std::string>> hits;
        int i = 0;
        for (auto& [typ, name] : fn.params) {
            if (name.empty() || typ.find('*') != std::string::npos || typ.find('&') != std::string::npos) {
                ++i;
                continue;
            }
            auto t = cxx_class_name(typ);
            if (bases.count(t)) hits.push_back({i, t});
            ++i;
        }
        if (!hits.empty()) slicers[fn.name] = std::move(hits);
    }
    if (slicers.empty()) return;
    std::set<std::string> derived_names;
    for (auto& [d, _] : inherit) derived_names.insert(d);
    for (auto& fn : funcs) {
        std::map<std::string, std::string> typed;
        for (auto& [typ, name] : fn.params) {
            if (name.empty() || typ.find('*') != std::string::npos) continue;
            auto t = cxx_class_name(typ);
            if (derived_names.count(t)) typed[name] = t;
        }
        for (auto& m : _CXX_LOCAL_OBJ.finditer(fn.body))
            if (derived_names.count(m.named("ty"))) typed[m.named("name")] = m.named("ty");
        if (typed.empty()) continue;
        auto start = fn.span.first;
        std::set<std::pair<std::string, int>> reported;
        auto body_lines = split_lines(fn.body);
        for (int i = 0; i < static_cast<int>(body_lines.size()); ++i) {
            for (auto& [callee, positions] : slicers) {
                auto args = find_call_args(body_lines[static_cast<std::size_t>(i)], callee);
                if (!args) continue;
                for (auto& [idx, base] : positions) {
                    if (idx >= static_cast<int>(args->size())) continue;
                    auto arg = strip((*args)[static_cast<std::size_t>(idx)]);
                    auto it = typed.find(arg);
                    if (it == typed.end()) continue;
                    if (!reported.insert({callee, i}).second) continue;
                    report(out, rel, fn.name, start + i, "CXX-SLICING",
                           callee + "() takes " + base + " by value; " + arg + " is " + it->second,
                           lines);
                }
            }
        }
    }
}

void _cxx_delete_this(const std::vector<std::string>& lines, std::string_view rel,
                      const std::vector<FunctionInfo>& funcs, std::vector<Finding>& out) {
    cxx_first_token(lines, rel, funcs, out, _DELETE_THIS, "CXX-DELETE-THIS",
                    "delete this while the member function still runs");
}

void _cxx_catch_by_value(const std::vector<std::string>& lines, std::string_view rel,
                         const std::vector<FunctionInfo>& funcs, std::vector<Finding>& out) {
    for (auto& fn : funcs) {
        auto start = fn.span.first;
        std::set<int> reported;
        auto body_lines = split_lines(fn.body);
        for (int i = 0; i < static_cast<int>(body_lines.size()); ++i) {
            for (auto& m : _CATCH_CLAUSE.finditer(body_lines[static_cast<std::size_t>(i)])) {
                auto clause = strip(m.named("clause"));
                if (clause.empty() || clause == "..." || clause.find('&') != std::string::npos
                    || clause.find('*') != std::string::npos)
                    continue;
                if (!reported.insert(i).second) continue;
                report(out, rel, fn.name, start + i, "CXX-CATCH-BY-VALUE",
                       "catch (" + clause + ") catches by value", lines);
            }
        }
    }
}

void _cxx_throw_noexcept(const std::vector<std::string>& lines, std::string_view rel,
                         const std::vector<FunctionInfo>& funcs, std::vector<Finding>& out) {
    for (auto& fn : funcs) {
        auto m = re_search_match(R"(\)([^({]*))", fn.signature);
        if (!m) continue;
        auto tail = m->group(1);
        if (!re_search(R"(\bnoexcept\b)", tail) && !re_search(R"(throw\s*\(\s*\))", tail)) continue;
        if (!re_search(R"(\bthrow\b)", fn.body)) continue;
        auto start = fn.span.first;
        int line = start;
        auto body_lines = split_lines(fn.body);
        for (int i = 0; i < static_cast<int>(body_lines.size()); ++i)
            if (re_search(R"(\bthrow\b)", body_lines[static_cast<std::size_t>(i)])) {
                line = start + i;
                break;
            }
        report(out, rel, fn.name, line, "CXX-THROW-NOEXCEPT",
               fn.name + " is noexcept but contains throw", lines);
    }
}

void _cxx_missing_virtual_dtor(std::string_view stripped, const std::vector<std::string>& lines,
                               std::string_view rel, const std::vector<FunctionInfo>& funcs,
                               std::vector<Finding>& out) {
    std::set<std::string> bad;
    for (auto& m : _CLASS_DEF.finditer(stripped)) {
        auto name = m.named("name");
        auto pos = stripped.find('{', static_cast<std::size_t>(m.spans[0].first));
        if (pos == std::string_view::npos) continue;
        auto close = match_brace(stripped, static_cast<int>(pos));
        if (close < 0) continue;
        auto body = std::string(stripped.substr(pos + 1, static_cast<std::size_t>(close) - pos - 1));
        bool has_virt_dtor = re_search("\\bvirtual\\s+~(?:" + re_escape(name) + ")\\b", body);
        bool has_virt_method = false;
        for (auto& vm : Regex(R"(\bvirtual\b)").finditer(body)) {
            auto chunk = body.substr(static_cast<std::size_t>(vm.spans[0].first), 120);
            if (re_search(R"(\bvirtual\s+~)", chunk)) continue;
            if (chunk.find('(') != std::string::npos) { has_virt_method = true; break; }
        }
        if (has_virt_method && !has_virt_dtor) bad.insert(name);
    }
    if (bad.empty()) return;
    for (auto& fn : funcs) {
        std::map<std::string, std::string> ptr_types;
        for (auto& [typ, pname] : fn.params) {
            if (pname.empty() || typ.find('*') == std::string::npos) continue;
            auto cls = cxx_class_name(typ);
            if (!cls.empty()) {
                auto sp = cls.rfind(' ');
                ptr_types[pname] = sp == std::string::npos ? cls : cls.substr(sp + 1);
            }
        }
        for (auto& m : _CXX_PTR_DECL.finditer(fn.body)) ptr_types[m.named("name")] = m.named("cls");
        if (ptr_types.empty()) continue;
        auto start = fn.span.first;
        std::set<std::string> reported;
        auto body_lines = split_lines(fn.body);
        for (int i = 0; i < static_cast<int>(body_lines.size()); ++i) {
            auto dm = _DELETE.search_match(body_lines[static_cast<std::size_t>(i)]);
            if (!dm) continue;
            auto var = dm->named("var");
            auto it = ptr_types.find(var);
            if (it == ptr_types.end() || !bad.count(it->second) || reported.count(var)) continue;
            reported.insert(var);
            report(out, rel, fn.name, start + i, "CXX-MISSING-VIRTUAL-DTOR",
                   "delete " + var + " (" + it->second + "*) but " + it->second
                       + " lacks virtual destructor",
                   lines);
        }
    }
}

void _cxx_auto_ptr(const std::vector<std::string>& lines, std::string_view rel,
                   const std::vector<FunctionInfo>& funcs, std::vector<Finding>& out) {
    cxx_first_token(lines, rel, funcs, out, _AUTO_PTR_USE, "CXX-AUTO-PTR",
                    "std::auto_ptr is deprecated; use unique_ptr");
}

void _cxx_throw_new(const std::vector<std::string>& lines, std::string_view rel,
                    const std::vector<FunctionInfo>& funcs, std::vector<Finding>& out) {
    cxx_first_token(lines, rel, funcs, out, _THROW_NEW, "CXX-THROW-NEW",
                    "throw new allocates the exception object");
}

void _cxx_catch_all(const std::vector<std::string>& lines, std::string_view rel,
                    const std::vector<FunctionInfo>& funcs, std::vector<Finding>& out) {
    for (auto& fn : funcs) {
        auto start = fn.span.first;
        std::size_t pos = 0;
        while (true) {
            auto m = _CATCH_ALL_BLOCK.search_match(fn.body, pos);
            if (!m) break;
            auto brace = static_cast<int>(m->spans[0].second) - 1;
            auto close = match_brace(fn.body, brace);
            if (close < 0) { pos = static_cast<std::size_t>(m->spans[0].second); continue; }
            auto inner = strip(std::string(fn.body.substr(static_cast<std::size_t>(brace + 1),
                                                          static_cast<std::size_t>(close - brace - 1))));
            if (!inner.empty()) { pos = static_cast<std::size_t>(close + 1); continue; }
            int line = start + static_cast<int>(std::count(fn.body.begin(), fn.body.begin() + m->spans[0].first, '\n'));
            report(out, rel, fn.name, line, "CXX-CATCH-ALL",
                   "catch (...) with an empty body swallows exceptions", lines);
            return;
        }
    }
}

void _cxx_const_cast(const std::vector<std::string>& lines, std::string_view rel,
                     const std::vector<FunctionInfo>& funcs, std::vector<Finding>& out) {
    for (auto& fn : funcs) {
        std::set<std::string> const_objs;
        for (auto& [typ, name] : fn.params)
            if (!name.empty() && re_search(R"(\bconst\b)", typ)) const_objs.insert(name);
        for (auto& m : _CONST_LOCAL.finditer(fn.body)) const_objs.insert(m.named("name"));
        if (const_objs.empty()) continue;
        auto start = fn.span.first;
        auto body_lines = split_lines(fn.body);
        for (int i = 0; i < static_cast<int>(body_lines.size()); ++i) {
            auto m = _CONST_CAST_WRITE.search_match(body_lines[static_cast<std::size_t>(i)]);
            if (!m) continue;
            auto name = m->named("name");
            if (!const_objs.count(name) || re_search(R"(\bconst\b)", m->named("target"))) continue;
            report(out, rel, fn.name, start + i, "CXX-CONST-CAST",
                   "write through const_cast of const object " + name, lines);
            break;
        }
    }
}

void _cxx_dynamic_cast_null(const std::vector<std::string>& lines, std::string_view rel,
                            const std::vector<FunctionInfo>& funcs, std::vector<Finding>& out) {
    for (auto& fn : funcs) {
        auto body_lines = split_lines(fn.body);
        auto start = fn.span.first;
        for (int i = 0; i < static_cast<int>(body_lines.size()); ++i) {
            auto m = _DYN_CAST_ASSIGN.search_match(body_lines[static_cast<std::size_t>(i)]);
            if (!m) continue;
            auto var = m->named("var");
            int use_j = -1;
            for (int j = i + 1; j < static_cast<int>(body_lines.size()); ++j) {
                auto& ln = body_lines[static_cast<std::size_t>(j)];
                if (null_test_for(var, ln)) continue;
                auto v = re_escape(var);
                if (re_search("\\b" + v + "\\s*->", ln) || re_search("\\*\\s*" + v + "\\b", ln)
                    || re_search("\\b" + v + "\\s*\\[", ln) || re_search("\\([^)]*\\b" + v + "\\b", ln)) {
                    use_j = j;
                    break;
                }
            }
            if (use_j < 0) continue;
            std::string between;
            for (int k = i; k < use_j; ++k) {
                if (!between.empty()) between += '\n';
                between += body_lines[static_cast<std::size_t>(k)];
            }
            if (null_test_for(var, between)) continue;
            report(out, rel, fn.name, start + use_j, "CXX-DYNAMIC-CAST-NULL",
                   var + " from dynamic_cast used without a null test", lines);
            break;
        }
    }
}

void _cxx_reinterpret(const std::vector<std::string>& lines, std::string_view rel,
                      const std::vector<FunctionInfo>& funcs, std::vector<Finding>& out) {
    for (auto& fn : funcs) {
        std::map<std::string, std::string> ptr_decls, scalar_decls;
        for (auto& [typ, name] : fn.params) {
            if (name.empty()) continue;
            if (typ.find('*') != std::string::npos) ptr_decls[name] = typ;
            else if (typ.find('&') == std::string::npos) scalar_decls[name] = typ;
        }
        for (auto& m : _CXX_PTR_DECL_LINE.finditer(fn.body))
            ptr_decls[m.named("name")] = strip(m.named("typ"));
        for (auto& m : _CXX_SCALAR_LOCAL.finditer(fn.body))
            scalar_decls[m.named("name")] = strip(m.named("typ"));
        auto start = fn.span.first;
        auto body_lines = split_lines(fn.body);
        for (int i = 0; i < static_cast<int>(body_lines.size()); ++i) {
            for (auto& m : _REINTERPRET_CAST.finditer(body_lines[static_cast<std::size_t>(i)])) {
                auto target = strip(m.named("target"));
                if (allowed_reinterp_pointee(target)) continue;
                auto src = strip(m.named("src"));
                std::optional<std::string> src_typ;
                if (auto am = re_search_match(R"(&\s*([A-Za-z_]\w*))", src)) {
                    auto name = am->group(1);
                    if (scalar_decls.count(name)) src_typ = scalar_decls[name] + " *";
                } else if (auto nm = re_search_match(R"(([A-Za-z_]\w*))", src)) {
                    auto name = nm->group(1);
                    if (ptr_decls.count(name)) src_typ = ptr_decls[name];
                }
                if (!src_typ || allowed_reinterp_pointee(*src_typ)) continue;
                if (cxx_pointee(target) == cxx_pointee(*src_typ)) continue;
                report(out, rel, fn.name, start + i, "CXX-REINTERPRET",
                       "reinterpret_cast type-puns between object pointers", lines);
                break;
            }
        }
    }
}

void _cxx_std_thread(const std::vector<std::string>& lines, std::string_view rel,
                     const std::vector<FunctionInfo>& funcs, std::vector<Finding>& out) {
    for (auto& fn : funcs) {
        auto start = fn.span.first;
        auto body_lines = split_lines(fn.body);
        for (int i = 0; i < static_cast<int>(body_lines.size()); ++i) {
            auto m = _CXX_THREAD_DECL.search_match(body_lines[static_cast<std::size_t>(i)]);
            if (!m) continue;
            auto name = m->named("name");
            if (re_search("\\b" + re_escape(name) + "\\s*\\.\\s*(?:join|detach)\\s*\\(", fn.body))
                continue;
            report(out, rel, fn.name, start + i, "CXX-STD-THREAD",
                   name + " is neither join()'d nor detach()'d", lines);
            return;
        }
    }
}

void _cxx_optional_null(const std::vector<std::string>& lines, std::string_view rel,
                        const std::vector<FunctionInfo>& funcs, std::vector<Finding>& out) {
    for (auto& fn : funcs) {
        auto names = names_of(_CXX_OPTIONAL_DECL, fn.body);
        if (names.empty()) continue;
        auto start = fn.span.first;
        auto body_lines = split_lines(fn.body);
        for (int i = 0; i < static_cast<int>(body_lines.size()); ++i) {
            for (auto& name : names) {
                if (optional_has_guard(name, fn.body) || !optional_deref(name, body_lines[static_cast<std::size_t>(i)]))
                    continue;
                report(out, rel, fn.name, start + i, "CXX-OPTIONAL-NULL",
                       name + " dereferenced without has_value()", lines);
                return;
            }
        }
    }
}

void _cxx_expected_null(const std::vector<std::string>& lines, std::string_view rel,
                        const std::vector<FunctionInfo>& funcs, std::vector<Finding>& out) {
    for (auto& fn : funcs) {
        auto names = names_of(_CXX_EXPECTED_DECL, fn.body);
        if (names.empty()) continue;
        auto start = fn.span.first;
        auto body_lines = split_lines(fn.body);
        for (int i = 0; i < static_cast<int>(body_lines.size()); ++i) {
            for (auto& name : names) {
                if (optional_has_guard(name, fn.body) || !optional_deref(name, body_lines[static_cast<std::size_t>(i)]))
                    continue;
                report(out, rel, fn.name, start + i, "CXX-EXPECTED-NULL",
                       name + " dereferenced without has_value()", lines);
                return;
            }
        }
    }
}

void _cxx_vector_index(const std::vector<std::string>& lines, std::string_view rel,
                       const std::vector<FunctionInfo>& funcs, std::vector<Finding>& out) {
    for (auto& fn : funcs) {
        std::set<std::string> containers;
        for (auto& [typ, pname] : fn.params)
            if (!pname.empty() && re_search(R"(\b(?:vector\s*<|string)\b)", typ)) containers.insert(pname);
        for (auto& m : _CXX_VEC_STR_DECL.finditer(fn.body)) containers.insert(m.named("name"));
        if (containers.empty()) continue;
        auto start = fn.span.first;
        auto body_lines = split_lines(fn.body);
        for (int i = 0; i < static_cast<int>(body_lines.size()); ++i) {
            for (auto& m : _CXX_SUBSCRIPT.finditer(body_lines[static_cast<std::size_t>(i)])) {
                auto cont = m.named("cont");
                if (!containers.count(cont)) continue;
                auto idx = m.named("idx");
                auto c = re_escape(cont), iv = re_escape(strip(idx));
                if (re_search("\\bif\\s*\\(\\s*" + iv + "\\s*<\\s*" + c + "\\s*\\.\\s*(?:size|length)\\s*\\(\\s*\\)", fn.body))
                    continue;
                if (re_search("\\bif\\s*\\(\\s*" + c + "\\s*\\.\\s*(?:size|length)\\s*\\(\\s*\\)\\s*>\\s*" + iv, fn.body))
                    continue;
                report(out, rel, fn.name, start + i, "CXX-VECTOR-INDEX",
                       cont + "[" + strip(idx) + "] without a size()/length() guard", lines);
                return;
            }
        }
    }
}

void _cxx_variant_get(const std::vector<std::string>& lines, std::string_view rel,
                      const std::vector<FunctionInfo>& funcs, std::vector<Finding>& out) {
    for (auto& fn : funcs) {
        if (re_search(R"(\bemplace\s*<)", fn.body) || re_search(R"(\bvalueless_by_exception\s*\()", fn.body))
            continue;
        auto start = fn.span.first;
        auto body_lines = split_lines(fn.body);
        for (int i = 0; i < static_cast<int>(body_lines.size()); ++i) {
            if (re_search(R"(\bany_cast\s*<)", body_lines[static_cast<std::size_t>(i)])) continue;
            for (auto& m : _CXX_VARIANT_GET.finditer(body_lines[static_cast<std::size_t>(i)])) {
                auto var = m.named("var");
                auto ty = re_escape(strip(m.named("ty")));
                auto v = re_escape(var);
                if (re_search("\\bholds_alternative\\s*<\\s*" + ty + "\\s*>\\s*\\(\\s*" + v + "\\s*\\)", fn.body))
                    continue;
                if (re_search("\\bholds_alternative\\s*<[^>]+>\\s*\\(\\s*" + v + "\\s*\\)", fn.body))
                    continue;
                report(out, rel, fn.name, start + i, "CXX-VARIANT-GET",
                       "std::get without holds_alternative on " + var, lines);
                return;
            }
        }
    }
}

void _cxx_span_dangle(const std::vector<std::string>& lines, std::string_view rel,
                      const std::vector<FunctionInfo>& funcs, std::vector<Finding>& out) {
    for (auto& fn : funcs) {
        auto [sc, arrays] = locals_in_fn(fn);
        auto locals_ok = arrays;
        auto ls = cxx_local_strings(fn);
        locals_ok.insert(ls.begin(), ls.end());
        if (locals_ok.empty()) continue;
        auto start = fn.span.first;
        auto body_lines = split_lines(fn.body);
        for (int i = 0; i < static_cast<int>(body_lines.size()); ++i) {
            auto& ln = body_lines[static_cast<std::size_t>(i)];
            if (ln.find("span") == std::string::npos && fn.return_type.find("span") == std::string::npos)
                continue;
            auto m = _CXX_RETURN_SPAN.search_match(ln);
            if (!m || !locals_ok.count(m->named("name"))) continue;
            report(out, rel, fn.name, start + i, "CXX-SPAN-DANGLE",
                   "return span constructed from local " + m->named("name"), lines);
            return;
        }
    }
}

void _cxx_std_async(const std::vector<std::string>& lines, std::string_view rel,
                    const std::vector<FunctionInfo>& funcs, std::vector<Finding>& out) {
    for (auto& fn : funcs) {
        auto start = fn.span.first;
        auto body_lines = split_lines(fn.body);
        for (int i = 0; i < static_cast<int>(body_lines.size()); ++i) {
            if (!_ASYNC_DISCARDED.match_line(body_lines[static_cast<std::size_t>(i)])) continue;
            report(out, rel, fn.name, start + i, "CXX-STD-ASYNC", "std::async result is discarded", lines);
            return;
        }
    }
}

void _cxx_std_bind(const std::vector<std::string>& lines, std::string_view rel,
                   const std::vector<FunctionInfo>& funcs, std::vector<Finding>& out) {
    cxx_first_token(lines, rel, funcs, out, _STD_BIND_CALL, "CXX-STD-BIND", "std::bind(); prefer a lambda");
}

void _cxx_assume(const std::vector<std::string>& lines, std::string_view rel,
                 const std::vector<FunctionInfo>& funcs, std::vector<Finding>& out) {
    for (auto& fn : funcs) {
        if (!re_search(R"(\breturn\b)", fn.body)) continue;
        cxx_first_token(lines, rel, funcs, out, _CXX_ASSUME_FALSE, "CXX-ASSUME",
                        "[[assume(false)]] / [[assume(0)]] is an unreachable lie");
        break;
    }
}

void _cxx_chrono_seed(const std::vector<std::string>& lines, std::string_view rel,
                      const std::vector<FunctionInfo>& funcs, std::vector<Finding>& out) {
    for (auto& fn : funcs) {
        if (!_CHRONO_NOW.search(fn.body) || !_CHRONO_PRNG.search(fn.body)) continue;
        auto start = fn.span.first;
        auto body_lines = split_lines(fn.body);
        for (int i = 0; i < static_cast<int>(body_lines.size()); ++i)
            if (_CHRONO_NOW.search(body_lines[static_cast<std::size_t>(i)])
                || _CHRONO_PRNG.search(body_lines[static_cast<std::size_t>(i)])) {
                report(out, rel, fn.name, start + i, "CXX-CHRONO-SEED",
                       "PRNG seeded from chrono clock::now()", lines);
                return;
            }
    }
}

void _cxx_regex(const std::vector<std::string>& lines, std::string_view rel,
                const std::vector<FunctionInfo>& funcs, std::vector<Finding>& out) {
    for (auto& fn : funcs) {
        auto start = fn.span.first;
        auto body_lines = split_lines(fn.body);
        for (int i = 0; i < static_cast<int>(body_lines.size()); ++i) {
            auto m = _REGEX_CTOR.search_match(body_lines[static_cast<std::size_t>(i)]);
            if (!m) continue;
            if (is_string_literal(m->named("pat"))) continue;
            report(out, rel, fn.name, start + i, "CXX-REGEX",
                   "std::regex pattern is not a string literal", lines);
            return;
        }
    }
}

void _cxx_enable_shared(const std::vector<std::string>& lines, std::string_view rel,
                        const std::vector<FunctionInfo>& funcs, std::vector<Finding>& out) {
    cxx_token_unless(lines, rel, funcs, out, _SHARED_FROM_THIS, R"(\benable_shared_from_this\b)",
                     "CXX-ENABLE-SHARED", "shared_from_this() without enable_shared_from_this");
}

void _cxx_move_const(const std::vector<std::string>& lines, std::string_view rel,
                     const std::vector<FunctionInfo>& funcs, std::vector<Finding>& out) {
    for (auto& fn : funcs) {
        std::set<std::string> consts;
        for (auto& [typ, name] : fn.params) {
            if (name.empty() || !re_search(R"(\bconst\b)", typ)) continue;
            auto stripped_ty = re_sub_pat(R"(\b(?:const|volatile)\b)", "", typ);
            if (stripped_ty.find('*') != std::string::npos) continue;
            consts.insert(name);
        }
        for (auto& m : _CONST_OBJ_DECL.finditer(fn.body)) consts.insert(m.named("name"));
        for (auto& m : _CONST_LOCAL.finditer(fn.body)) consts.insert(m.named("name"));
        if (consts.empty()) continue;
        auto start = fn.span.first;
        auto body_lines = split_lines(fn.body);
        for (int i = 0; i < static_cast<int>(body_lines.size()); ++i) {
            for (auto& m : _STD_MOVE.finditer(body_lines[static_cast<std::size_t>(i)])) {
                if (!consts.count(m.named("name"))) continue;
                report(out, rel, fn.name, start + i, "CXX-MOVE-CONST",
                       "std::move of const object " + m.named("name"), lines);
                return;
            }
        }
    }
}

void _cxx_optional_value(const std::vector<std::string>& lines, std::string_view rel,
                         const std::vector<FunctionInfo>& funcs, std::vector<Finding>& out) {
    for (auto& fn : funcs) {
        auto names = names_of(_CXX_OPTIONAL_DECL, fn.body);
        auto exp = names_of(_CXX_EXPECTED_DECL, fn.body);
        names.insert(exp.begin(), exp.end());
        if (names.empty()) continue;
        auto start = fn.span.first;
        auto body_lines = split_lines(fn.body);
        for (int i = 0; i < static_cast<int>(body_lines.size()); ++i) {
            for (auto& name : names) {
                if (optional_has_guard(name, fn.body)) continue;
                if (!re_search("\\b" + re_escape(name) + "\\s*\\.\\s*value\\s*\\(", body_lines[static_cast<std::size_t>(i)]))
                    continue;
                report(out, rel, fn.name, start + i, "CXX-OPTIONAL-VALUE",
                       name + ".value() without has_value()", lines);
                return;
            }
        }
    }
}

void _cxx_spaceship_default(std::string_view stripped, const std::vector<std::string>& lines,
                            std::string_view rel, const std::vector<FunctionInfo>& funcs,
                            std::vector<Finding>& out) {
    std::set<std::string> reported;
    for (auto& m : _CLASS_DEF.finditer(stripped)) {
        auto cls = m.named("name");
        auto pos = stripped.find('{', static_cast<std::size_t>(m.spans[0].first));
        if (pos == std::string_view::npos) continue;
        auto close = match_brace(stripped, static_cast<int>(pos));
        if (close < 0) continue;
        auto body = stripped.substr(pos + 1, static_cast<std::size_t>(close) - pos - 1);
        if (!_PTR_MEMBER.search(body)) continue;
        auto sm = _SPACESHIP_DEFAULT.search_match(body);
        if (!sm) continue;
        int op_line = 1 + static_cast<int>(std::count(stripped.begin(), stripped.begin() + static_cast<std::ptrdiff_t>(pos) + 1 + sm->spans[0].first, '\n'));
        std::string hit_fn;
        int hit_line = op_line;
        for (auto& fn : funcs) {
            if (!re_search("\\b" + re_escape(cls) + "\\b", fn.body)) continue;
            hit_fn = fn.name;
            auto start = fn.span.first;
            auto bl = split_lines(fn.body);
            for (int i = 0; i < static_cast<int>(bl.size()); ++i)
                if (re_search("\\b" + re_escape(cls) + "\\b", bl[static_cast<std::size_t>(i)])) {
                    hit_line = start + i;
                    break;
                }
            break;
        }
        if (hit_fn.empty()) {
            for (auto& fn : funcs)
                if (fn.line >= op_line) { hit_fn = fn.name; break; }
        }
        if (hit_fn.empty()) hit_fn = cls;
        if (reported.count(hit_fn)) continue;
        reported.insert(hit_fn);
        report(out, rel, hit_fn, hit_line, "CXX-SPACESHIP-DEFAULT",
               cls + " defaulted operator<=> shallow-compares a pointer member", lines);
    }
}

void _cxx_std_format(const std::vector<std::string>& lines, std::string_view rel,
                     const std::vector<FunctionInfo>& funcs, std::vector<Finding>& out) {
    for (auto& fn : funcs) {
        auto start = fn.span.first;
        auto body_lines = split_lines(fn.body);
        for (int i = 0; i < static_cast<int>(body_lines.size()); ++i) {
            auto& ln = body_lines[static_cast<std::size_t>(i)];
            for (auto& m : _STD_FMT_CALL.finditer(ln)) {
                auto args = find_call_args(ln, m.named("fn"));
                if (!args || args->empty()) continue;
                if (is_string_literal((*args)[0])) continue;
                report(out, rel, fn.name, start + i, "CXX-STD-FORMAT",
                       "std::" + m.named("fn") + "() format argument is not a string literal", lines);
                return;
            }
        }
    }
}

void _cxx_lambda_dangle(const std::vector<std::string>& lines, std::string_view rel,
                        const std::vector<FunctionInfo>& funcs, std::vector<Finding>& out) {
    for (auto& fn : funcs) {
        std::set<std::string> params;
        for (auto& [typ, name] : fn.params)
            if (!name.empty()) params.insert(name);
        auto body_lines = split_lines(fn.body);
        auto start = fn.span.first;
        std::set<std::string> locals_so_far;
        std::map<std::string, std::set<std::string>> returned_lambdas;
        for (int i = 0; i < static_cast<int>(body_lines.size()); ++i) {
            auto& ln = body_lines[static_cast<std::size_t>(i)];
            if (auto m = match_at_start_line(_CXX_LOCAL_LINE, ln)) {
                auto name = m->named("name");
                if (!params.count(name) && !KW.count(name)) locals_so_far.insert(name);
            }
            if (auto m = _CXX_AUTO_LAMBDA.search_match(ln)) {
                auto cap = re_sub_pat(R"(\s+)", "", m->named("capture"));
                if (cap.find('&') != std::string::npos) returned_lambdas[m->named("name")] = {"ref"};
            }
            if (auto m = _CXX_RETURN_LAMBDA.search_match(ln)) {
                auto cap = re_sub_pat(R"(\s+)", "", m->named("capture"));
                if (cap.find('&') != std::string::npos) {
                    report(out, rel, fn.name, start + i, "CXX-LAMBDA-DANGLE",
                           "returned lambda captures local by reference", lines);
                    break;
                }
            }
            if (auto m = _CXX_RETURN_NAME.search_match(ln); m && returned_lambdas.count(m->named("name"))) {
                report(out, rel, fn.name, start + i, "CXX-LAMBDA-DANGLE",
                       "returned lambda captures local by reference", lines);
                break;
            }
        }
    }
}

void _cxx_unique_reset(const std::vector<std::string>& lines, std::string_view rel,
                       const std::vector<FunctionInfo>& funcs, std::vector<Finding>& out) {
    for (auto& fn : funcs) {
        auto body_lines = split_lines(fn.body);
        auto start = fn.span.first;
        std::set<std::string> unique_ptrs;
        std::map<std::string, std::string> raw_owner;
        std::map<std::string, int> reset_at;
        for (int i = 0; i < static_cast<int>(body_lines.size()); ++i) {
            auto& ln = body_lines[static_cast<std::size_t>(i)];
            for (auto& m : _UNIQUE_PTR_DECL.finditer(ln)) unique_ptrs.insert(m.named("name"));
            if (auto m = _UNIQUE_RAW_GET.search_match(ln); m && unique_ptrs.count(m->named("up")))
                raw_owner[m->named("raw")] = m->named("up");
            for (auto& m : _UNIQUE_RESET.finditer(ln))
                if (unique_ptrs.count(m.named("up"))) reset_at.emplace(m.named("up"), i);
            if (auto m = _UNIQUE_NULL.search_match(ln); m && unique_ptrs.count(m->named("up")))
                reset_at.emplace(m->named("up"), i);
            for (auto& [raw, up] : raw_owner) {
                auto it = reset_at.find(up);
                if (it == reset_at.end() || i <= it->second) continue;
                if (_UNIQUE_RAW_GET.search(ln)) continue;
                auto v = re_escape(raw);
                if (!(re_search("(?<!\\w)\\*\\s*" + v + "\\b", ln) || re_search("\\b" + v + "\\s*->", ln)
                      || re_search("\\b" + v + "\\s*\\[", ln) || re_search("\\([^)]*\\b" + v + "\\b[^)]*\\)", ln)))
                    continue;
                report(out, rel, fn.name, start + i, "CXX-UNIQUE-RESET",
                       raw + " from " + up + ".get() used after reset", lines);
                return;
            }
        }
    }
}

void _cxx_shared_get(const std::vector<std::string>& lines, std::string_view rel,
                     const std::vector<FunctionInfo>& funcs, std::vector<Finding>& out) {
    for (auto& fn : funcs) {
        auto body_lines = split_lines(fn.body);
        auto start = fn.span.first;
        std::set<std::string> shared_ptrs;
        std::map<std::string, std::string> raw_owner;
        std::map<std::string, int> reset_at;
        for (int i = 0; i < static_cast<int>(body_lines.size()); ++i) {
            auto& ln = body_lines[static_cast<std::size_t>(i)];
            for (auto& m : _SHARED_PTR_DECL.finditer(ln)) shared_ptrs.insert(m.named("name"));
            if (auto m = _UNIQUE_RAW_GET.search_match(ln); m && shared_ptrs.count(m->named("up")))
                raw_owner[m->named("raw")] = m->named("up");
            for (auto& m : _UNIQUE_RESET.finditer(ln))
                if (shared_ptrs.count(m.named("up"))) reset_at.emplace(m.named("up"), i);
            if (auto m = _UNIQUE_NULL.search_match(ln); m && shared_ptrs.count(m->named("up")))
                reset_at.emplace(m->named("up"), i);
            for (auto& [raw, up] : raw_owner) {
                auto it = reset_at.find(up);
                if (it == reset_at.end() || i <= it->second) continue;
                auto v = re_escape(raw);
                if (!(re_search("(?<!\\w)\\*\\s*" + v + "\\b", ln) || re_search("\\b" + v + "\\s*->", ln)
                      || re_search("\\b" + v + "\\s*\\[", ln)))
                    continue;
                report(out, rel, fn.name, start + i, "CXX-SHARED-PTR-GET",
                       raw + " from " + up + ".get() used after reset", lines);
                return;
            }
        }
    }
}

void _cxx_string_data(const std::vector<std::string>& lines, std::string_view rel,
                      const std::vector<FunctionInfo>& funcs, std::vector<Finding>& out) {
    for (auto& fn : funcs) {
        auto body_lines = split_lines(fn.body);
        auto start = fn.span.first;
        std::set<std::string> strings;
        std::map<std::string, std::string> raw_owner;
        std::map<std::string, int> mutate_at;
        for (int i = 0; i < static_cast<int>(body_lines.size()); ++i) {
            auto& ln = body_lines[static_cast<std::size_t>(i)];
            for (auto& m : _STR_OWN_DECL.finditer(ln)) strings.insert(m.named("name"));
            if (auto m = _STR_CSTR_GET.search_match(ln); m && strings.count(m->named("s")))
                raw_owner[m->named("raw")] = m->named("s");
            for (auto& m : _STR_MUTATE.finditer(ln))
                if (strings.count(m.named("s"))) mutate_at.emplace(m.named("s"), i);
            for (auto& [raw, s] : raw_owner) {
                auto it = mutate_at.find(s);
                if (it == mutate_at.end() || i <= it->second) continue;
                auto v = re_escape(raw);
                if (!(re_search("(?<!\\w)\\*\\s*" + v + "\\b", ln) || re_search("\\b" + v + "\\s*\\[", ln)))
                    continue;
                report(out, rel, fn.name, start + i, "CXX-STRING-DATA",
                       raw + " from " + s + ".c_str()/data() used after mutation", lines);
                return;
            }
        }
    }
}

void _cxx_unique_release(const std::vector<std::string>& lines, std::string_view rel,
                         const std::vector<FunctionInfo>& funcs, std::vector<Finding>& out) {
    for (auto& fn : funcs) {
        auto ups = names_of(_UNIQUE_PTR_DECL, fn.body);
        if (ups.empty()) continue;
        auto start = fn.span.first;
        auto body_lines = split_lines(fn.body);
        for (auto& up : ups) {
            if (!re_search("\\b" + re_escape(up) + "\\s*\\.\\s*release\\s*\\(", fn.body)) continue;
            for (int i = 0; i < static_cast<int>(body_lines.size()); ++i) {
                if (!re_search("\\b" + re_escape(up) + "\\s*\\.\\s*get\\s*\\(", body_lines[static_cast<std::size_t>(i)]))
                    continue;
                report(out, rel, fn.name, start + i, "CXX-UNIQUE-RELEASE",
                       up + ".get() used after release()", lines);
                return;
            }
        }
    }
}

void _cxx_fwd_ref(const std::vector<std::string>& lines, std::string_view rel,
                  const std::vector<FunctionInfo>& funcs, std::vector<Finding>& out) {
    for (auto& fn : funcs) {
        std::set<std::string> rrefs;
        for (auto& [typ, name] : fn.params)
            if (!name.empty() && typ.find("&&") != std::string::npos) rrefs.insert(name);
        if (rrefs.empty()) continue;
        auto start = fn.span.first;
        auto body_lines = split_lines(fn.body);
        for (int i = 0; i < static_cast<int>(body_lines.size()); ++i) {
            auto m = _FWD_PUSH_ARG.search_match(body_lines[static_cast<std::size_t>(i)]);
            if (!m || !rrefs.count(m->named("arg"))) continue;
            auto v = re_escape(m->named("arg"));
            if (re_search("\\bstd\\s*::\\s*(?:move|forward)\\s*(?:<[^>]*>)?\\s*\\(\\s*" + v + "\\s*\\)", fn.body))
                continue;
            report(out, rel, fn.name, start + i, "CXX-FORWARDING-REF",
                   m->named("arg") + " is T&& but push_back copies it without std::forward/std::move",
                   lines);
            break;
        }
    }
}

void _cxx_bit_cast(const std::vector<std::string>& lines, std::string_view rel,
                   const std::vector<FunctionInfo>& funcs, std::vector<Finding>& out) {
    for (auto& fn : funcs) {
        auto start = fn.span.first;
        auto body_lines = split_lines(fn.body);
        for (int i = 0; i < static_cast<int>(body_lines.size()); ++i) {
            for (auto& m : _BIT_CAST.finditer(body_lines[static_cast<std::size_t>(i)])) {
                auto tgt = strip(m.named("tgt"));
                bool tgt_obj = tgt.find('*') != std::string::npos && !allowed_reinterp_pointee(tgt);
                if (!tgt_obj && m.named("src").find('*') == std::string::npos
                    && m.named("src").find('&') == std::string::npos)
                    continue;
                report(out, rel, fn.name, start + i, "CXX-BIT-CAST",
                       "bit_cast type-puns an object pointer", lines);
                return;
            }
        }
    }
}

// placement new (p) where p is a typed object pointer, not a byte buffer.
void _cxx_placement_new(const std::vector<std::string>& lines, std::string_view rel,
                        const std::vector<FunctionInfo>& funcs, std::vector<Finding>& out) {
    for (auto& fn : funcs) {
        auto bl = split_lines(fn.body);
        auto start = fn.span.first;
        for (int i = 0; i < static_cast<int>(bl.size()); ++i) {
            auto m = _PLACE_NEW.search_match(bl[static_cast<std::size_t>(i)]);
            if (!m) continue;
            auto ptr = m->named("ptr");
            if (PLACE_SKIP_PTR.count(ptr)) continue;
            auto ok = place_storage_ok(ptr, fn);
            if (!ok || *ok) continue;
            report(out, rel, fn.name, start + i, "CXX-PLACEMENT-NEW",
                   "placement new into typed object pointer " + ptr, lines);
            return;
        }
    }
}

void _cxx_throw_copy(const std::vector<std::string>& lines, std::string_view rel,
                     const std::vector<FunctionInfo>& funcs, std::vector<Finding>& out) {
    for (auto& fn : funcs) {
        std::size_t pos = 0;
        auto start = fn.span.first;
        while (true) {
            auto m = _CATCH_BLOCK.search_match(fn.body, pos);
            if (!m) break;
            auto clause = strip(m->named("clause"));
            std::string name;
            if (clause.find("...") == std::string::npos
                && (clause.find('&') != std::string::npos || clause.find('*') != std::string::npos)) {
                if (auto nm = re_search_match(R"(([A-Za-z_]\w*)\s*$)", clause)) name = nm->group(1);
            }
            auto brace = static_cast<int>(m->spans[0].second) - 1;
            auto close = match_brace(fn.body, brace);
            if (close < 0) { pos = static_cast<std::size_t>(m->spans[0].second); continue; }
            if (!name.empty()) {
                auto catch_body = fn.body.substr(static_cast<std::size_t>(brace + 1),
                                                 static_cast<std::size_t>(close - brace - 1));
                int catch_start = start + static_cast<int>(std::count(fn.body.begin(), fn.body.begin() + brace + 1, '\n'));
                auto cbl = split_lines(catch_body);
                for (int i = 0; i < static_cast<int>(cbl.size()); ++i) {
                    auto tm = _THROW_IDENT.search_match(cbl[static_cast<std::size_t>(i)]);
                    if (tm && tm->named("ident") == name) {
                        report(out, rel, fn.name, catch_start + i, "CXX-THROW-COPY",
                               "throw " + name + " copies caught exception; use throw;", lines);
                        break;
                    }
                }
            }
            pos = static_cast<std::size_t>(close + 1);
        }
    }
}

void _cxx_volatile_cast(const std::vector<std::string>& lines, std::string_view rel,
                        const std::vector<FunctionInfo>& funcs, std::vector<Finding>& out) {
    for (auto& fn : funcs) {
        auto vols = names_of(_VOL_LOCAL, fn.body);
        if (vols.empty()) continue;
        auto start = fn.span.first;
        auto body_lines = split_lines(fn.body);
        for (int i = 0; i < static_cast<int>(body_lines.size()); ++i) {
            auto m = _VOL_CAST_WRITE.search_match(body_lines[static_cast<std::size_t>(i)]);
            if (!m || !vols.count(m->named("name"))) continue;
            report(out, rel, fn.name, start + i, "CXX-VOLATILE-CAST",
                   "write through C-style cast of volatile " + m->named("name"), lines);
            return;
        }
    }
}

void _cxx_copy_assign_ptr(const std::vector<std::string>& lines, std::string_view rel,
                          const std::vector<FunctionInfo>& funcs, std::vector<Finding>& out) {
    std::set<std::string> members;
    std::string blob;
    for (auto& ln : lines) {
        if (!blob.empty()) blob += '\n';
        blob += ln;
    }
    for (auto& m : _CLASS_DEF.finditer(blob)) {
        auto pos = blob.find('{', static_cast<std::size_t>(m.spans[0].first));
        if (pos == std::string::npos) continue;
        auto close = match_brace(blob, static_cast<int>(pos));
        if (close < 0) continue;
        auto body = blob.substr(pos + 1, static_cast<std::size_t>(close) - pos - 1);
        for (auto& pm : _PTR_MEMBER.finditer(body)) members.insert(pm.named("name"));
    }
    if (members.empty()) return;
    for (auto& fn : funcs) {
        auto self = first_ref_ptr_param(fn);
        if (has_self_assign_guard(self.value_or(""), fn.body)) continue;
        auto start = fn.span.first;
        auto body_lines = split_lines(fn.body);
        for (int i = 0; i < static_cast<int>(body_lines.size()); ++i) {
            auto& ln = body_lines[static_cast<std::size_t>(i)];
            if (re_search(R"(\b(?:unique_ptr|shared_ptr|make_unique)\b)", ln) || re_search(R"(\bnew\b)", ln))
                continue;
            auto m = _SHALLOW_PTR_ASSIGN.search_match(ln);
            if (!m || !members.count(m->named("field"))) continue;
            report(out, rel, fn.name, start + i, "CXX-COPY-ASSIGN-PTR",
                   m->named("dst") + "." + m->named("field") + " = " + m->named("src") + "."
                       + m->named("field") + " shallow-copies a raw pointer member",
                   lines);
            return;
        }
    }
}

void _cxx_uninit_member(std::string_view stripped, const std::vector<std::string>& lines,
                        std::string_view rel, const std::vector<FunctionInfo>& funcs,
                        std::vector<Finding>& out) {
    std::set<std::string> reported;
    for (auto& m : _CLASS_DEF.finditer(stripped)) {
        auto cls = m.named("name");
        auto pos = stripped.find('{', static_cast<std::size_t>(m.spans[0].first));
        if (pos == std::string_view::npos) continue;
        auto close = match_brace(stripped, static_cast<int>(pos));
        if (close < 0) continue;
        auto body = std::string(stripped.substr(pos + 1, static_cast<std::size_t>(close) - pos - 1));
        std::vector<std::string> members;
        for (auto& mm : _CXX_SCALAR_MEMBER.finditer(body)) members.push_back(mm.named("name"));
        if (members.empty()) continue;
        auto ctor = re_search_match("\\b" + re_escape(cls) + "\\s*\\(\\s*\\)\\s*(?::(?P<init>[^{]*))?\\s*\\{", body);
        if (!ctor) continue;
        auto init = ctor->named("init");
        std::vector<std::string> missing;
        for (auto& mem : members)
            if (!re_search("\\b" + re_escape(mem) + "\\s*\\(", init)) missing.push_back(mem);
        if (missing.empty()) continue;
        std::string mems;
        for (std::size_t i = 0; i < missing.size(); ++i) {
            if (i) mems += ", ";
            mems += missing[i];
        }
        int ctor_line = 1 + static_cast<int>(std::count(stripped.begin(), stripped.begin() + static_cast<std::ptrdiff_t>(pos) + 1 + ctor->spans[0].first, '\n'));
        std::string hit_fn;
        int hit_line = ctor_line;
        for (auto& fn : funcs) {
            if (!re_search("\\b" + re_escape(cls) + "\\s+[A-Za-z_]\\w*", fn.body)) continue;
            auto start = fn.span.first;
            auto bl = split_lines(fn.body);
            for (int i = 0; i < static_cast<int>(bl.size()); ++i) {
                bool hit = false;
                for (auto& mem : missing)
                    if (re_search("\\.\\s*" + re_escape(mem) + "\\b", bl[static_cast<std::size_t>(i)])) {
                        hit_fn = fn.name;
                        hit_line = start + i;
                        hit = true;
                        break;
                    }
                if (hit) break;
            }
            if (hit_fn.empty()) hit_fn = fn.name;
            if (!hit_fn.empty()) break;
        }
        if (hit_fn.empty()) {
            for (auto& fn : funcs)
                if (fn.line >= ctor_line) { hit_fn = fn.name; break; }
        }
        if (hit_fn.empty() || reported.count(hit_fn)) continue;
        reported.insert(hit_fn);
        report(out, rel, hit_fn, hit_line, "CXX-UNINIT-MEMBER",
               cls + "() leaves scalar member " + mems + " uninitialised", lines);
    }
}

void _cxx_explicit_ctor(std::string_view stripped, const std::vector<std::string>& lines,
                        std::string_view rel, const std::vector<FunctionInfo>& funcs,
                        std::vector<Finding>& out) {
    std::set<std::string> bad;
    for (auto& m : _CLASS_DEF.finditer(stripped)) {
        auto name = m.named("name");
        auto pos = stripped.find('{', static_cast<std::size_t>(m.spans[0].first));
        if (pos == std::string_view::npos) continue;
        auto close = match_brace(stripped, static_cast<int>(pos));
        if (close < 0) continue;
        auto body = stripped.substr(pos + 1, static_cast<std::size_t>(close) - pos - 1);
        bool implicit = false;
        for (auto& om : _OP_BOOL.finditer(body)) {
            if (!om.named("ex").empty()) { implicit = false; break; }
            implicit = true;
        }
        if (implicit) bad.insert(name);
    }
    if (bad.empty()) return;
    for (auto& fn : funcs) {
        std::string hit_cls;
        for (auto& c : bad)
            if (re_search("\\b" + re_escape(c) + "\\b", fn.body)) { hit_cls = c; break; }
        if (hit_cls.empty()) continue;
        auto start = fn.span.first;
        int hit_i = 0;
        auto bl = split_lines(fn.body);
        for (int i = 0; i < static_cast<int>(bl.size()); ++i)
            if (re_search("\\b" + re_escape(hit_cls) + "\\b", bl[static_cast<std::size_t>(i)])) {
                hit_i = i;
                break;
            }
        report(out, rel, fn.name, start + hit_i, "CXX-EXPLICIT-CTOR",
               hit_cls + " has operator bool() without explicit", lines);
    }
}

void _cxx_bind_tmp(std::string_view stripped, const std::vector<std::string>& lines,
                   std::string_view rel, const std::vector<FunctionInfo>& funcs,
                   std::vector<Finding>& out) {
    std::set<std::string> reported;
    for (auto& m : _REF_RET_FN.finditer(stripped)) {
        auto name = m.named("name");
        auto pos = stripped.find('{', static_cast<std::size_t>(m.spans[0].first));
        if (pos == std::string_view::npos) continue;
        auto close = match_brace(stripped, static_cast<int>(pos));
        if (close < 0) continue;
        auto body = stripped.substr(pos + 1, static_cast<std::size_t>(close) - pos - 1);
        int hit_line = 0;
        std::string msg;
        if (auto rm = _RETURN_CTOR.search_match(body)) {
            hit_line = 1 + static_cast<int>(std::count(stripped.begin(), stripped.begin() + static_cast<std::ptrdiff_t>(pos) + 1 + rm->spans[0].first, '\n'));
            msg = "return of a temporary bound to a reference";
        } else if (auto bm = _BIND_TMP_LOCAL.search_match(body)) {
            auto nm = bm->named("name");
            if (re_search("\\breturn\\s+" + re_escape(nm) + "\\s*;", body)) {
                hit_line = 1 + static_cast<int>(std::count(stripped.begin(), stripped.begin() + static_cast<std::ptrdiff_t>(pos) + 1 + bm->spans[0].first, '\n'));
                msg = "return of " + nm + " bound to a temporary";
            }
        }
        if (!hit_line || reported.count(name)) continue;
        reported.insert(name);
        report(out, rel, name, hit_line, "CXX-BIND-TMP", msg, lines);
    }
    for (auto& fn : funcs) {
        if (reported.count(fn.name)) continue;
        auto bm = _BIND_TMP_LOCAL.search_match(fn.body);
        if (!bm) continue;
        auto nm = bm->named("name");
        if (!re_search("\\breturn\\s+" + re_escape(nm) + "\\s*;", fn.body)) continue;
        int line = fn.span.first + static_cast<int>(std::count(fn.body.begin(), fn.body.begin() + bm->spans[0].first, '\n'));
        reported.insert(fn.name);
        report(out, rel, fn.name, line, "CXX-BIND-TMP", "return of " + nm + " bound to a temporary", lines);
    }
}

// Returned lambda with [this] or [=] capturing a class member. Distinct from
// CXX-LAMBDA-DANGLE (`[&]` / explicit `&local`).
void _cxx_this_capture(std::string_view stripped, const std::vector<std::string>& lines,
                       std::string_view rel, const std::vector<FunctionInfo>& funcs,
                       std::vector<Finding>& out) {
    std::set<std::string> reported;
    for (auto& fn : funcs) {
        auto& body = fn.body;
        if (body.find('[') == std::string::npos ||
            (!_CXX_RETURN_LAMBDA.search(body) && !_CXX_AUTO_LAMBDA.search(body)))
            continue;
        std::set<std::string> params;
        for (auto& [typ, name] : fn.params)
            if (!name.empty()) params.insert(name);
        auto members = fn_class_members(fn, stripped);
        std::set<std::string> locals_ok = params;
        for (auto& ln : split_lines(body)) {
            auto mloc = match_at_start_line(_CXX_LOCAL_LINE, ln);
            if (!mloc) continue;
            auto name = mloc->named("name");
            if (!params.count(name) && !KW.count(name)) locals_ok.insert(name);
        }
        std::map<std::string, std::pair<std::string, std::string>> stored;
        for (auto& m : _CXX_AUTO_LAMBDA.finditer(body))
            stored[m.named("name")] = {m.named("capture"),
                                       lambda_body_from(body, static_cast<std::size_t>(m.spans[0].second))
                                           .value_or("")};
        std::optional<int> hit_off;
        for (auto& m : _CXX_RETURN_LAMBDA.finditer(body)) {
            auto lam = lambda_body_from(body, static_cast<std::size_t>(m.spans[0].second));
            if (this_capture_hit(m.named("capture"), lam, locals_ok, members)) {
                hit_off = m.spans[0].first;
                break;
            }
        }
        if (!hit_off) {
            for (auto& m : _CXX_RETURN_NAME.finditer(body)) {
                auto it = stored.find(m.named("name"));
                if (it == stored.end()) continue;
                if (this_capture_hit(it->second.first, it->second.second, locals_ok, members)) {
                    hit_off = m.spans[0].first;
                    break;
                }
            }
        }
        if (!hit_off) continue;
        int line = fn.span.first +
                   static_cast<int>(std::count(body.begin(), body.begin() + *hit_off, '\n'));
        reported.insert(fn.name);
        report(out, rel, fn.name, line, "CXX-THIS-CAPTURE",
               "returned lambda captures this by [=] or [this]", lines);
    }

    static const Regex fn_head(R"(\b(?P<name>[A-Za-z_]\w*)\s*\([^;{}]*\)[^{]*\{)");
    for (auto& cm : _CLASS_DEF.finditer(stripped)) {
        auto cls = cm.named("name");
        int brace = cm.spans[0].second - 1;
        int close = match_brace(stripped, brace);
        if (close < 0) continue;
        auto cbody = stripped.substr(static_cast<std::size_t>(brace + 1),
                                     static_cast<std::size_t>(close - brace - 1));
        auto members = class_member_names(cbody);
        for (auto& m : _CXX_RETURN_LAMBDA.finditer(cbody)) {
            auto lam = lambda_body_from(cbody, static_cast<std::size_t>(m.spans[0].second));
            if (!this_capture_hit(m.named("capture"), lam, {}, members)) continue;
            std::string hit_fn = cls;
            for (auto& hm : fn_head.finditer(cbody.substr(0, static_cast<std::size_t>(m.spans[0].first))))
                hit_fn = hm.named("name");
            if (reported.count(hit_fn)) continue;
            int line = static_cast<int>(std::count(stripped.begin(),
                                                   stripped.begin() + brace + 1 + m.spans[0].first, '\n')) + 1;
            reported.insert(hit_fn);
            report(out, rel, hit_fn, line, "CXX-THIS-CAPTURE",
                   "returned lambda captures this by [=] or [this]", lines);
        }
    }
}

void _cxx_future_get(const std::vector<std::string>& lines, std::string_view rel,
                     const std::vector<FunctionInfo>& funcs, std::vector<Finding>& out) {
    for (auto& fn : funcs) {
        if (re_search(R"(\bget_future\s*\()", fn.body) || re_search(R"(\b(?:std\s*::\s*)?promise\s*<)", fn.body))
            continue;
        auto names = names_of(_CXX_FUTURE_DECL, fn.body);
        if (names.empty()) continue;
        auto start = fn.span.first;
        auto bl = split_lines(fn.body);
        for (int i = 0; i < static_cast<int>(bl.size()); ++i) {
            for (auto& m : _FUTURE_GET.finditer(bl[static_cast<std::size_t>(i)])) {
                auto name = m.named("name");
                if (!names.count(name)) continue;
                auto n = re_escape(name);
                if (re_search("\\b" + n + "\\s*=(?!=)", fn.body) || re_search("\\b" + n + "\\s*\\.\\s*valid\\s*\\(", fn.body))
                    continue;
                report(out, rel, fn.name, start + i, "CXX-FUTURE-GET",
                       name + ".get() without a prior future assignment", lines);
                return;
            }
        }
    }
}

void _cxx_function_null(const std::vector<std::string>& lines, std::string_view rel,
                        const std::vector<FunctionInfo>& funcs, std::vector<Finding>& out) {
    for (auto& fn : funcs) {
        auto names = names_of(_CXX_FUNCTION_DECL, fn.body);
        if (names.empty()) continue;
        auto start = fn.span.first;
        auto bl = split_lines(fn.body);
        for (int i = 0; i < static_cast<int>(bl.size()); ++i) {
            for (auto& name : names) {
                if (optional_has_guard(name, fn.body)) continue;
                if (_CXX_FUNCTION_DECL.search(bl[static_cast<std::size_t>(i)])) continue;
                if (!re_search("\\b" + re_escape(name) + "\\s*\\(", bl[static_cast<std::size_t>(i)])) continue;
                report(out, rel, fn.name, start + i, "CXX-STD-FUNCTION-NULL",
                       name + " invoked without a truth test", lines);
                return;
            }
        }
    }
}

void _cxx_nodiscard(const std::vector<std::string>& lines, std::string_view rel,
                    const std::vector<FunctionInfo>& funcs, std::vector<Finding>& out) {
    std::string blob;
    for (auto& ln : lines) { if (!blob.empty()) blob += '\n'; blob += ln; }
    std::set<std::string> callees;
    for (auto& m : _NODISCARD_ATTR.finditer(blob)) {
        auto after = blob.substr(static_cast<std::size_t>(m.spans[0].second), 160);
        for (auto& nm : _NODISCARD_NAME.finditer(after)) {
            auto ident = nm.group(1);
            if (NODISCARD_SKIP.count(ident)) continue;
            callees.insert(ident);
            break;
        }
    }
    if (callees.empty()) return;
    for (auto& fn : funcs) {
        auto start = fn.span.first;
        auto bl = split_lines(fn.body);
        for (int i = 0; i < static_cast<int>(bl.size()); ++i) {
            auto m = match_at_start_line(_NODISCARD_DISCARDED, bl[static_cast<std::size_t>(i)]);
            if (!m) continue;
            auto name = m->named("name");
            if (!callees.count(name)) continue;
            report(out, rel, fn.name, start + i, "CXX-NODISCARD",
                   "[[nodiscard]] result of " + name + "() is discarded", lines);
            return;
        }
    }
}

void _cxx_std_jthread(const std::vector<std::string>& lines, std::string_view rel,
                      const std::vector<FunctionInfo>& funcs, std::vector<Finding>& out) {
    for (auto& fn : funcs) {
        if (re_search(R"(\brequest_stop\s*\()", fn.body)) continue;
        auto start = fn.span.first;
        auto bl = split_lines(fn.body);
        for (int i = 0; i < static_cast<int>(bl.size()); ++i) {
            auto m = _CXX_JTHREAD_DECL.search_match(bl[static_cast<std::size_t>(i)]);
            if (!m) continue;
            report(out, rel, fn.name, start + i, "CXX-STD-JTHREAD",
                   m->named("name") + " is constructed without request_stop()", lines);
            return;
        }
    }
}

void _cxx_mdspan_dangle(const std::vector<std::string>& lines, std::string_view rel,
                        const std::vector<FunctionInfo>& funcs, std::vector<Finding>& out) {
    for (auto& fn : funcs) {
        auto [sc, arrays] = locals_in_fn(fn);
        auto locals_ok = arrays;
        auto ls = cxx_local_strings(fn);
        locals_ok.insert(ls.begin(), ls.end());
        if (locals_ok.empty()) continue;
        auto start = fn.span.first;
        auto bl = split_lines(fn.body);
        for (int i = 0; i < static_cast<int>(bl.size()); ++i) {
            if (bl[static_cast<std::size_t>(i)].find("mdspan") == std::string::npos
                && fn.return_type.find("mdspan") == std::string::npos)
                continue;
            auto m = _CXX_RETURN_MDSPAN.search_match(bl[static_cast<std::size_t>(i)]);
            if (!m || !locals_ok.count(m->named("name"))) continue;
            report(out, rel, fn.name, start + i, "CXX-MDSPAN-DANGLE",
                   "return mdspan constructed from local " + m->named("name"), lines);
            return;
        }
    }
}

void _cxx_atomic_ref(const std::vector<std::string>& lines, std::string_view rel,
                     const std::vector<FunctionInfo>& funcs, std::vector<Finding>& out) {
    for (auto& fn : funcs) {
        std::set<std::string> params;
        for (auto& [typ, name] : fn.params)
            if (!name.empty()) params.insert(name);
        auto [scalars, arrays] = locals_in_fn(fn);
        auto locals_ok = scalars;
        locals_ok.insert(arrays.begin(), arrays.end());
        for (auto& m : _CXX_SCALAR_LOCAL.finditer(fn.body))
            if (!params.count(m.named("name"))) locals_ok.insert(m.named("name"));
        for (auto& p : params) locals_ok.erase(p);
        auto start = fn.span.first;
        auto bl = split_lines(fn.body);
        std::map<std::string, std::string> ref_from_local;
        for (int i = 0; i < static_cast<int>(bl.size()); ++i) {
            auto& ln = bl[static_cast<std::size_t>(i)];
            for (auto& m : _CXX_ATOMIC_REF_CTOR.finditer(ln))
                if (locals_ok.count(m.named("name"))) ref_from_local[m.named("ref")] = m.named("name");
            if (auto m = _CXX_RETURN_ATOMIC_REF.search_match(ln); m && locals_ok.count(m->named("name"))) {
                report(out, rel, fn.name, start + i, "CXX-ATOMIC-REF",
                       "return atomic_ref constructed from local " + m->named("name"), lines);
                return;
            }
            if (auto m = _CXX_RETURN_NAME.search_match(ln); m && ref_from_local.count(m->named("name"))) {
                report(out, rel, fn.name, start + i, "CXX-ATOMIC-REF",
                       "return atomic_ref of local " + ref_from_local[m->named("name")], lines);
                return;
            }
        }
    }
}

void _cxx_condition_wait(const std::vector<std::string>& lines, std::string_view rel,
                         const std::vector<FunctionInfo>& funcs, std::vector<Finding>& out) {
    for (auto& fn : funcs) {
        if (!re_search(R"(\b(?:std\s*::\s*)?condition_variable(?:_any)?\b)", fn.body)) continue;
        auto start = fn.span.first;
        auto bl = split_lines(fn.body);
        for (int i = 0; i < static_cast<int>(bl.size()); ++i) {
            if (!_CXX_CV_WAIT_BARE.search(bl[static_cast<std::size_t>(i)])) continue;
            report(out, rel, fn.name, start + i, "CXX-CONDITION-WAIT",
                   "cv.wait(lk) without a predicate lambda", lines);
            return;
        }
    }
}

void _cxx_shared_mutex(const std::vector<std::string>& lines, std::string_view rel,
                       const std::vector<FunctionInfo>& funcs, std::vector<Finding>& out) {
    for (auto& fn : funcs) {
        auto names = names_of(_CXX_SHARED_MUTEX_DECL, fn.body);
        auto start = fn.span.first;
        auto bl = split_lines(fn.body);
        for (auto& name : names) {
            auto n = re_escape(name);
            if (re_search("\\b" + n + "\\s*\\.\\s*unlock", fn.body) || _CXX_LOCK_RAII.search(fn.body))
                continue;
            for (int i = 0; i < static_cast<int>(bl.size()); ++i)
                if (re_search("\\b" + n + "\\s*\\.\\s*lock\\s*\\(", bl[static_cast<std::size_t>(i)])) {
                    report(out, rel, fn.name, start + i, "CXX-SHARED-MUTEX",
                           name + ".lock() without unlock or lock_guard", lines);
                    return;
                }
        }
    }
}

// any_cast<T>(v) by value without type()/has_value/try or pointer form.
void _cxx_any_cast(const std::vector<std::string>& lines, std::string_view rel,
                   const std::vector<FunctionInfo>& funcs, std::vector<Finding>& out) {
    for (auto& fn : funcs) {
        if (rx_search(R"(\btry\b)", fn.body) && rx_search(R"(\bcatch\b)", fn.body)) continue;
        auto start = fn.span.first;
        auto bl = split_lines(fn.body);
        for (int i = 0; i < static_cast<int>(bl.size()); ++i) {
            auto& ln = bl[static_cast<std::size_t>(i)];
            if (rx_search(R"(\(\s*void\s*\)\s*(?:std\s*::\s*)?any_cast\s*<)", ln)) continue;
            for (auto& m : _ANY_CAST_CALL.finditer(ln)) {
                auto arg = strip(m.named("arg"));
                if (!arg.empty() && arg[0] == '&') continue;
                if (auto var = rx_match_start(R"(^([A-Za-z_]\w*)\s*$)", arg)) {
                    auto v = re_escape(var->group(1));
                    if (rx_search("\\b" + v + "\\s*\\.\\s*(?:type|has_value)\\s*\\(", fn.body)) continue;
                }
                report(out, rel, fn.name, start + i, "CXX-ANY-CAST",
                       "any_cast by value without type() or pointer form", lines);
                return;
            }
        }
    }
}

void _cxx_filesystem(const std::vector<std::string>& lines, std::string_view rel,
                     const std::vector<FunctionInfo>& funcs, std::vector<Finding>& out) {
    for (auto& fn : funcs) {
        auto start = fn.span.first;
        auto bl = split_lines(fn.body);
        std::set<std::string> removed;
        for (int i = 0; i < static_cast<int>(bl.size()); ++i) {
            for (auto& m : _FS_REMOVE.finditer(bl[static_cast<std::size_t>(i)]))
                removed.insert(m.named("p"));
            for (auto& m : _FS_USE_AFTER.finditer(bl[static_cast<std::size_t>(i)])) {
                if (!removed.count(m.named("p"))) continue;
                report(out, rel, fn.name, start + i, "CXX-FILESYSTEM",
                       "filesystem::remove(" + m.named("p") + ") then use of the same path", lines);
                return;
            }
        }
    }
}

void _cxx_latch(const std::vector<std::string>& lines, std::string_view rel,
                const std::vector<FunctionInfo>& funcs, std::vector<Finding>& out) {
    for (auto& fn : funcs) {
        auto names = names_of(_CXX_LATCH_DECL, fn.body);
        auto start = fn.span.first;
        auto bl = split_lines(fn.body);
        for (auto& name : names) {
            auto n = re_escape(name);
            if (re_search("\\b" + n + "\\s*\\.\\s*count_down\\s*\\(", fn.body)) continue;
            for (int i = 0; i < static_cast<int>(bl.size()); ++i)
                if (re_search("\\b" + n + "\\s*\\.\\s*wait\\s*\\(", bl[static_cast<std::size_t>(i)])) {
                    report(out, rel, fn.name, start + i, "CXX-LATCH",
                           name + ".wait() without count_down()", lines);
                    return;
                }
        }
    }
}

// from_chars out-parameter used without checking r.ec / errc / ptr.
void _cxx_from_chars(const std::vector<std::string>& lines, std::string_view rel,
                     const std::vector<FunctionInfo>& funcs, std::vector<Finding>& out) {
    for (auto& fn : funcs) {
        if (rx_search(R"(\.(?:ec|ptr)\b|\berrc\b)", fn.body)) continue;
        auto start = fn.span.first;
        auto bl = split_lines(fn.body);
        std::vector<std::pair<std::string, int>> outs;  // insertion order, last line
        for (int i = 0; i < static_cast<int>(bl.size()); ++i)
            for (auto& m : _FROM_CHARS_OUT.finditer(bl[static_cast<std::size_t>(i)])) {
                auto var = m.named("var");
                auto it = std::find_if(outs.begin(), outs.end(), [&](auto& p) { return p.first == var; });
                if (it == outs.end()) outs.emplace_back(var, i);
                else it->second = i;
            }
        if (outs.empty()) continue;
        for (int i = 0; i < static_cast<int>(bl.size()); ++i) {
            auto& ln = bl[static_cast<std::size_t>(i)];
            for (auto& [var, prev] : outs) {
                if (i <= prev) continue;
                if (!rx_search("\\b" + re_escape(var) + "\\b", ln)) continue;
                if (_FROM_CHARS_OUT.search(ln)) continue;
                report(out, rel, fn.name, start + i, "CXX-FROM-CHARS",
                       var + " used after from_chars without checking ec", lines);
                return;
            }
        }
    }
}

// to_chars buffer used without checking r.ec / errc / ptr (CWE-252).
void _cxx_to_chars(const std::vector<std::string>& lines, std::string_view rel,
                   const std::vector<FunctionInfo>& funcs, std::vector<Finding>& out) {
    for (auto& fn : funcs) {
        if (rx_search(R"(\.(?:ec|ptr)\b|\berrc\b)", fn.body)) continue;
        if (report_use_after(lines, rel, fn, _TO_CHARS_BUF, "buf", "CXX-TO-CHARS",
                             " used after to_chars without checking ec", out))
            return;
    }
}

void _cxx_init_list_dangle(const std::vector<std::string>& lines, std::string_view rel,
                           const std::vector<FunctionInfo>& funcs, std::vector<Finding>& out) {
    cxx_first_token(lines, rel, funcs, out, _INIT_LIST_TMP_BEGIN, "CXX-INIT-LIST-DANGLE",
                    "pointer from a temporary initializer_list.begin()");
}

// stop_token in while(true)/for(;;) without stop_requested() (CWE-833).
void _cxx_stop_token(const std::vector<std::string>& lines, std::string_view rel,
                     const std::vector<FunctionInfo>& funcs, std::vector<Finding>& out) {
    for (auto& fn : funcs) {
        if (!_CXX_STOP_TOKEN.search(fn.signature + "\n" + fn.body)) continue;
        if (rx_search(R"(\bstop_requested\s*\()", fn.body)) continue;
        auto start = fn.span.first;
        auto bl = split_lines(fn.body);
        for (int i = 0; i < static_cast<int>(bl.size()); ++i) {
            if (!_CXX_INF_LOOP.search(bl[static_cast<std::size_t>(i)])) continue;
            report(out, rel, fn.name, start + i, "CXX-STOP-TOKEN",
                   "stop_token used in an infinite loop without stop_requested()", lines);
            return;
        }
    }
}

void _cxx_flat_map(const std::vector<std::string>& lines, std::string_view rel,
                   const std::vector<FunctionInfo>& funcs, std::vector<Finding>& out) {
    for (auto& fn : funcs) {
        std::set<std::string> names;
        for (auto& m : _CXX_FLAT_MAP_DECL.finditer(fn.body))
            if (m.named("map").empty()) names.insert(m.named("name"));
        if (names.empty()) continue;
        auto start = fn.span.first;
        auto bl = split_lines(fn.body);
        for (auto& name : names) {
            auto n = re_escape(name);
            if (re_search("\\b" + n + "\\s*\\.\\s*(?:contains|count|find)\\s*\\(", fn.body)) continue;
            for (int i = 0; i < static_cast<int>(bl.size()); ++i)
                if (re_search("\\b" + n + "\\s*\\.\\s*at\\s*\\(", bl[static_cast<std::size_t>(i)])) {
                    report(out, rel, fn.name, start + i, "CXX-FLAT-MAP",
                           name + ".at() without contains()/count()/find()", lines);
                    return;
                }
        }
    }
}

void _cxx_semaphore(const std::vector<std::string>& lines, std::string_view rel,
                    const std::vector<FunctionInfo>& funcs, std::vector<Finding>& out) {
    for (auto& fn : funcs) {
        auto names = names_of(_CXX_SEM_DECL, fn.body);
        auto start = fn.span.first;
        auto bl = split_lines(fn.body);
        for (auto& name : names) {
            auto n = re_escape(name);
            if (re_search("\\b" + n + "\\s*\\.\\s*release\\s*\\(", fn.body)) continue;
            for (int i = 0; i < static_cast<int>(bl.size()); ++i)
                if (re_search("\\b" + n + "\\s*\\.\\s*acquire\\s*\\(", bl[static_cast<std::size_t>(i)])) {
                    report(out, rel, fn.name, start + i, "CXX-SEMAPHORE",
                           name + ".acquire() without release()", lines);
                    return;
                }
        }
    }
}

// stacktrace::current() [0]/at(0) without empty/size check (CWE-125).
void _cxx_stacktrace(const std::vector<std::string>& lines, std::string_view rel,
                     const std::vector<FunctionInfo>& funcs, std::vector<Finding>& out) {
    for (auto& fn : funcs) {
        if (!_STACKTRACE_CURRENT.search(fn.body)) continue;
        auto start = fn.span.first;
        auto assigned = names_of(_STACKTRACE_ASSIGN, fn.body);
        auto bl = split_lines(fn.body);
        for (int i = 0; i < static_cast<int>(bl.size()); ++i) {
            auto& ln = bl[static_cast<std::size_t>(i)];
            if (_STACKTRACE_DIRECT_ZERO.search(ln)) {
                if (rx_search(R"(\.(?:empty|size)\s*\()", fn.body)) continue;
                report(out, rel, fn.name, start + i, "CXX-STACKTRACE",
                       "stacktrace::current()[0] without empty()/size()", lines);
                return;
            }
            for (auto& name : assigned) {
                auto n = re_escape(name);
                if (rx_search("\\b" + n + "\\s*\\.\\s*(?:empty|size)\\s*\\(", fn.body) ||
                    rx_search("\\bif\\s*\\(\\s*!\\s*" + n + "\\b", fn.body))
                    continue;
                if (!(rx_search("\\b" + n + "\\s*\\[\\s*0\\s*\\]", ln) ||
                      rx_search("\\b" + n + "\\s*\\.\\s*at\\s*\\(\\s*0\\s*\\)", ln)))
                    continue;
                report(out, rel, fn.name, start + i, "CXX-STACKTRACE",
                       name + "[0] from stacktrace::current() without empty()/size()", lines);
                return;
            }
        }
    }
}

void _cxx_pack_pragma(const std::vector<std::string>& lines, std::string_view rel,
                      const std::vector<FunctionInfo>& funcs, std::vector<Finding>& out) {
    std::string blob;
    for (auto& ln : lines) { if (!blob.empty()) blob += '\n'; blob += ln; }
    if (!_PACK_PRAGMA_1.search(blob)) return;
    for (auto& fn : funcs) {
        auto start = fn.span.first;
        auto bl = split_lines(fn.body);
        for (int i = 0; i < static_cast<int>(bl.size()); ++i) {
            if (!_PACK_INT_MEMBER_ADDR.search(bl[static_cast<std::size_t>(i)])) continue;
            report(out, rel, fn.name, start + i, "CXX-PACK-PRAGMA",
                   "address of a non-char member of a #pragma pack(1) struct", lines);
            return;
        }
    }
}

void _cxx_function_ref(const std::vector<std::string>& lines, std::string_view rel,
                       const std::vector<FunctionInfo>& funcs, std::vector<Finding>& out) {
    for (auto& fn : funcs) {
        if (_CXX_FN_REF_RETURN.search(fn.body) || _CXX_FN_REF_TMP.search(fn.body)) {
            auto start = fn.span.first;
            auto bl = split_lines(fn.body);
            for (int i = 0; i < static_cast<int>(bl.size()); ++i)
                if (_CXX_FN_REF_RETURN.search(bl[static_cast<std::size_t>(i)])
                    || _CXX_FN_REF_TMP.search(bl[static_cast<std::size_t>(i)])) {
                    report(out, rel, fn.name, start + i, "CXX-FUNCTION-REF",
                           "function_ref bound to a temporary then returned", lines);
                    return;
                }
        }
    }
}

void _cxx_move_only_function(const std::vector<std::string>& lines, std::string_view rel,
                             const std::vector<FunctionInfo>& funcs, std::vector<Finding>& out) {
    for (auto& fn : funcs) {
        auto names = names_of(_CXX_MOF_DECL, fn.body);
        auto start = fn.span.first;
        auto bl = split_lines(fn.body);
        for (auto& name : names) {
            if (optional_has_guard(name, fn.body)) continue;
            for (int i = 0; i < static_cast<int>(bl.size()); ++i)
                if (re_search("\\b" + re_escape(name) + "\\s*\\(", bl[static_cast<std::size_t>(i)])
                    && !_CXX_MOF_DECL.search(bl[static_cast<std::size_t>(i)])) {
                    report(out, rel, fn.name, start + i, "CXX-MOVE-ONLY-FUNCTION",
                           name + " invoked without a truth test", lines);
                    return;
                }
        }
    }
}

void _cxx_ranges_dangle(const std::vector<std::string>& lines, std::string_view rel,
                        const std::vector<FunctionInfo>& funcs, std::vector<Finding>& out) {
    cxx_first_token(lines, rel, funcs, out, _RANGES_TMP_PIPE, "CXX-RANGES-DANGLE",
                    "views pipeline over a temporary range");
}

void _cxx_inplace_vector(const std::vector<std::string>& lines, std::string_view rel,
                         const std::vector<FunctionInfo>& funcs, std::vector<Finding>& out) {
    for (auto& fn : funcs) {
        auto names = names_of(_CXX_INPLACE_DECL, fn.body);
        auto start = fn.span.first;
        auto bl = split_lines(fn.body);
        for (auto& name : names) {
            auto n = re_escape(name);
            if (re_search("\\b" + n + "\\s*\\.\\s*(?:size|capacity|try_push_back)\\s*\\(", fn.body))
                continue;
            for (int i = 0; i < static_cast<int>(bl.size()); ++i)
                if (re_search("\\b" + n + "\\s*\\.\\s*push_back\\s*\\(", bl[static_cast<std::size_t>(i)])) {
                    report(out, rel, fn.name, start + i, "CXX-INPLACE-VECTOR",
                           name + ".push_back without size()/capacity()/try_push_back", lines);
                    return;
                }
        }
    }
}

void _cxx_flat_set(const std::vector<std::string>& lines, std::string_view rel,
                   const std::vector<FunctionInfo>& funcs, std::vector<Finding>& out) {
    cxx_find_no_end(lines, rel, funcs, out, _CXX_FLAT_SET_DECL, "CXX-FLAT-SET",
                    ".find() dereferenced without != end()");
}

void _cxx_copyable_function(const std::vector<std::string>& lines, std::string_view rel,
                            const std::vector<FunctionInfo>& funcs, std::vector<Finding>& out) {
    for (auto& fn : funcs) {
        auto names = names_of(_CXX_COPYABLE_FN_DECL, fn.body);
        auto start = fn.span.first;
        auto bl = split_lines(fn.body);
        for (auto& name : names) {
            if (optional_has_guard(name, fn.body)) continue;
            for (int i = 0; i < static_cast<int>(bl.size()); ++i)
                if (re_search("\\b" + re_escape(name) + "\\s*\\(", bl[static_cast<std::size_t>(i)])) {
                    report(out, rel, fn.name, start + i, "CXX-COPYABLE-FUNCTION",
                           name + " invoked without a truth test", lines);
                    return;
                }
        }
    }
}

void _cxx_hive(const std::vector<std::string>& lines, std::string_view rel,
               const std::vector<FunctionInfo>& funcs, std::vector<Finding>& out) {
    for (auto& fn : funcs) {
        auto names = names_of(_CXX_HIVE_DECL, fn.body);
        for (auto& [typ, pname] : fn.params)
            if (!pname.empty() && re_search(R"(\bhive\s*<)", typ)) names.insert(pname);
        auto start = fn.span.first;
        auto bl = split_lines(fn.body);
        std::set<std::string> begins;
        for (int i = 0; i < static_cast<int>(bl.size()); ++i) {
            auto& ln = bl[static_cast<std::size_t>(i)];
            if (ln.find(".begin(") != std::string::npos)
                for (auto& m : _CXX_BEGIN_CALL.finditer(ln))
                    if (names.count(re_sub_pat(R"(\s+)", "", m.named("container"))))
                        begins.insert(re_sub_pat(R"(\s+)", "", m.named("container")));
            for (auto& m : _CXX_HIVE_MUTATE.finditer(ln)) {
                auto cont = re_sub_pat(R"(\s+)", "", m.named("container"));
                if (!begins.count(cont)) continue;
                report(out, rel, fn.name, start + i, "CXX-HIVE",
                       cont + " mutated after .begin() iterator taken", lines);
                return;
            }
        }
    }
}

void _cxx_bitset_index(const std::vector<std::string>& lines, std::string_view rel,
                       const std::vector<FunctionInfo>& funcs, std::vector<Finding>& out) {
    for (auto& fn : funcs) {
        auto names = names_of(_CXX_BITSET_DECL, fn.body);
        auto start = fn.span.first;
        auto bl = split_lines(fn.body);
        for (int i = 0; i < static_cast<int>(bl.size()); ++i) {
            auto& ln = bl[static_cast<std::size_t>(i)];
            if (auto m = _CXX_BITSET_TEST.search_match(ln); m && names.count(m->named("name"))) {
                auto idx = m->named("idx");
                if (byteswap_idx_guarded(idx, fn.body)) continue;
                report(out, rel, fn.name, start + i, "CXX-BITSET-INDEX",
                       m->named("name") + ".test/" + m->named("name") + "[" + idx + "] without a size guard",
                       lines);
                return;
            }
            for (auto& m : _CXX_SUBSCRIPT.finditer(ln))
                if (names.count(m.named("cont")) && toarr_idx_bad(strip(m.named("idx")), std::nullopt, fn.body)) {
                    report(out, rel, fn.name, start + i, "CXX-BITSET-INDEX",
                           m.named("cont") + "[" + strip(m.named("idx")) + "] without a size guard", lines);
                    return;
                }
        }
    }
}

void _cxx_sstream_view(const std::vector<std::string>& lines, std::string_view rel,
                       const std::vector<FunctionInfo>& funcs, std::vector<Finding>& out) {
    for (auto& fn : funcs) {
        auto names = names_of(_CXX_SSTREAM_DECL, fn.body);
        if (names.empty()) continue;
        auto start = fn.span.first;
        auto bl = split_lines(fn.body);
        for (int i = 0; i < static_cast<int>(bl.size()); ++i) {
            auto& ln = bl[static_cast<std::size_t>(i)];
            if (auto m = _CXX_SSTREAM_VIEW.search_match(ln); m && names.count(m->named("ss"))) {
                report(out, rel, fn.name, start + i, "CXX-SSTREAM-VIEW",
                       "string_view of " + m->named("ss") + ".str() dangles", lines);
                return;
            }
            if (auto m = _CXX_SSTREAM_CSTR_TMP.search_match(ln); m && names.count(m->named("ss"))) {
                report(out, rel, fn.name, start + i, "CXX-SSTREAM-VIEW",
                       m->named("ss") + ".str().c_str() of a temporary", lines);
                return;
            }
        }
    }
}

void _cxx_indirect(const std::vector<std::string>& lines, std::string_view rel,
                   const std::vector<FunctionInfo>& funcs, std::vector<Finding>& out) {
    for (auto& fn : funcs) {
        auto names = names_of(_CXX_INDIRECT_DECL, fn.body);
        auto start = fn.span.first;
        auto bl = split_lines(fn.body);
        for (auto& name : names) {
            if (re_search("\\b" + re_escape(name) + "\\s*\\.\\s*(?:has_value|operator bool)", fn.body)
                || optional_has_guard(name, fn.body))
                continue;
            for (int i = 0; i < static_cast<int>(bl.size()); ++i)
                if (optional_deref(name, bl[static_cast<std::size_t>(i)])) {
                    report(out, rel, fn.name, start + i, "CXX-INDIRECT",
                           name + " dereferenced while empty", lines);
                    return;
                }
        }
    }
}

// hazard_pointer in scope but atomic load dereferenced without protect.
void _cxx_hazard_pointer(const std::vector<std::string>& lines, std::string_view rel,
                         const std::vector<FunctionInfo>& funcs, std::vector<Finding>& out) {
    for (auto& fn : funcs) {
        if (!_HAZARD_DECL.search(fn.body)) continue;
        if (rx_search(R"(\bprotect\s*\(|\bhazard_pointer_for\s*\()", fn.body)) continue;
        if (!rx_search(R"(\.load\s*\()", fn.body)) continue;
        auto start = fn.span.first;
        auto bl = split_lines(fn.body);
        for (int i = 0; i < static_cast<int>(bl.size()); ++i) {
            if (bl[static_cast<std::size_t>(i)].find("->") == std::string::npos) continue;
            report(out, rel, fn.name, start + i, "CXX-HAZARD-POINTER",
                   "atomic load used without hazard_pointer::protect()", lines);
            return;
        }
    }
}

void _cxx_text_encoding(const std::vector<std::string>& lines, std::string_view rel,
                        const std::vector<FunctionInfo>& funcs, std::vector<Finding>& out) {
    for (auto& fn : funcs) {
        auto start = fn.span.first;
        auto bl = split_lines(fn.body);
        for (int i = 0; i < static_cast<int>(bl.size()); ++i) {
            auto m = _TEXT_ENC_CTOR.search_match(bl[static_cast<std::size_t>(i)]);
            if (!m || is_string_literal(m->named("arg"))) continue;
            report(out, rel, fn.name, start + i, "CXX-TEXT-ENCODING",
                   "std::text_encoding name is not a string literal", lines);
            return;
        }
    }
}

void _cxx_expected_error(const std::vector<std::string>& lines, std::string_view rel,
                         const std::vector<FunctionInfo>& funcs, std::vector<Finding>& out) {
    for (auto& fn : funcs) {
        auto names = names_of(_CXX_EXPECTED_DECL, fn.body);
        auto start = fn.span.first;
        auto bl = split_lines(fn.body);
        for (auto& name : names) {
            if (re_search("\\bif\\s*\\(\\s*!\\s*" + re_escape(name) + "\\b", fn.body)) continue;
            for (int i = 0; i < static_cast<int>(bl.size()); ++i)
                if (re_search("\\b" + re_escape(name) + "\\s*\\.\\s*error\\s*\\(", bl[static_cast<std::size_t>(i)])) {
                    report(out, rel, fn.name, start + i, "CXX-EXPECTED-ERROR",
                           name + ".error() without a prior !" + name + " check", lines);
                    return;
                }
        }
    }
}

void _cxx_variant_valueless(const std::vector<std::string>& lines, std::string_view rel,
                            const std::vector<FunctionInfo>& funcs, std::vector<Finding>& out) {
    for (auto& fn : funcs) {
        if (!re_search(R"(\bemplace\s*<)", fn.body)) continue;
        if (re_search(R"(\bvalueless_by_exception\s*\()", fn.body)) continue;
        if (re_search(R"(\bholds_alternative\s*<)", fn.body)) continue;
        auto start = fn.span.first;
        auto bl = split_lines(fn.body);
        for (int i = 0; i < static_cast<int>(bl.size()); ++i)
            if (_CXX_VARIANT_GET.search(bl[static_cast<std::size_t>(i)])) {
                report(out, rel, fn.name, start + i, "CXX-VARIANT-VALUELSS",
                       "std::get after emplace without valueless_by_exception", lines);
                return;
            }
    }
}

void _cxx_simd_index(const std::vector<std::string>& lines, std::string_view rel,
                     const std::vector<FunctionInfo>& funcs, std::vector<Finding>& out) {
    cxx_sub_no_size(lines, rel, funcs, out, _CXX_SIMD_DECL, "CXX-SIMD-INDEX",
                    " without a size() guard");
}

// rcu_obj update / retire without rcu_synchronize() (CWE-416).
void _cxx_rcu(const std::vector<std::string>& lines, std::string_view rel,
              const std::vector<FunctionInfo>& funcs, std::vector<Finding>& out) {
    for (auto& fn : funcs) {
        if (rx_search(R"(\brcu_synchronize\s*\()", fn.body)) continue;
        auto names = names_of(_RCU_OBJ_DECL, fn.body);
        if (names.empty() && !rx_search(R"(\bstd\s*::\s*rcu\b)", fn.body)) continue;
        auto start = fn.span.first;
        auto bl = split_lines(fn.body);
        for (int i = 0; i < static_cast<int>(bl.size()); ++i) {
            auto& ln = bl[static_cast<std::size_t>(i)];
            bool hit = rx_search(R"(\bretire\s*\()", ln);
            for (auto& name : names)
                if (rx_search("\\b" + re_escape(name) + "\\s*=(?!=)", ln)) {
                    hit = true;
                    break;
                }
            if (!hit) continue;
            report(out, rel, fn.name, start + i, "CXX-RCU",
                   "rcu_obj update/retire without rcu_synchronize()", lines);
            return;
        }
    }
}

// linalg matrix(i,j) / scaled return without extents (CWE-125).
void _cxx_linalg(const std::vector<std::string>& lines, std::string_view rel,
                 const std::vector<FunctionInfo>& funcs, std::vector<Finding>& out) {
    for (auto& fn : funcs) {
        if (!rx_search(R"((?:std\s*::\s*)?linalg\s*::)", fn.body)) continue;
        if (rx_search(R"(\b(?:extents|extent|size)\s*\()", fn.body)) continue;
        auto names = names_of(_LINALG_MATRIX_DECL, fn.body);
        auto start = fn.span.first;
        auto bl = split_lines(fn.body);
        if (_LINALG_SCALED.search(fn.body) && rx_search(R"(\breturn\b)", fn.body)) {
            int hit_i = 0;
            for (int i = 0; i < static_cast<int>(bl.size()); ++i) {
                auto& ln = bl[static_cast<std::size_t>(i)];
                if (_LINALG_SCALED.search(ln) || rx_search(R"(\breturn\b)", ln)) {
                    hit_i = i;
                    break;
                }
            }
            report(out, rel, fn.name, start + hit_i, "CXX-LINALG",
                   "linalg scaled/copied result used without extents", lines);
            return;
        }
        for (int i = 0; i < static_cast<int>(bl.size()); ++i) {
            for (auto& name : names) {
                if (!rx_search("\\b" + re_escape(name) + "\\s*\\(\\s*[^,\\)]+\\s*,", bl[static_cast<std::size_t>(i)]))
                    continue;
                report(out, rel, fn.name, start + i, "CXX-LINALG",
                       name + "(i, j) without an extents guard", lines);
                return;
            }
        }
    }
}

void _cxx_sync_wait(const std::vector<std::string>& lines, std::string_view rel,
                    const std::vector<FunctionInfo>& funcs, std::vector<Finding>& out) {
    for (auto& fn : funcs) {
        auto start = fn.span.first;
        auto bl = split_lines(fn.body);
        for (int i = 0; i < static_cast<int>(bl.size()); ++i)
            if (_SYNC_WAIT_DISCARDED.match_line(bl[static_cast<std::size_t>(i)])) {
                report(out, rel, fn.name, start + i, "CXX-SYNC-WAIT",
                       "std::execution::sync_wait result is discarded", lines);
                return;
            }
    }
}

void _cxx_embed(const std::vector<std::string>& lines, std::string_view rel,
                const std::vector<FunctionInfo>& funcs, std::vector<Finding>& out) {
    for (int i = 0; i < static_cast<int>(lines.size()); ++i) {
        auto m = _EMBED_DIR.search_match(lines[static_cast<std::size_t>(i)]);
        if (!m) continue;
        if (is_string_literal(m->named("arg"))) continue;
        std::optional<std::string> fn;
        for (auto& f : funcs)
            if (f.span.first <= i + 1 && i + 1 <= f.span.second) fn = f.name;
        lint_add(out, rel, fn, i + 1, "CXX-EMBED", "#embed path is not a string literal", lines);
    }
}

void _cxx_contracts(const std::vector<std::string>& lines, std::string_view rel,
                    const std::vector<FunctionInfo>& funcs, std::vector<Finding>& out) {
    cxx_first_token(lines, rel, funcs, out, _CXX_CONTRACT_FALSE, "CXX-CONTRACTS",
                    "contract_assert(false)/pre(false) is an unreachable lie");
}

// ^^ / std::meta:: with define_aggregate/define_class and no namespace.
void _cxx_reflection(const std::vector<std::string>& lines, std::string_view rel,
                     const std::vector<FunctionInfo>& funcs, std::vector<Finding>& out) {
    for (auto& fn : funcs) {
        if (!_CXX_REFLECT_META.search(fn.body)) continue;
        if (!_CXX_REFLECT_DEFINE.search(fn.body)) continue;
        if (rx_search(R"(\bnamespace\s+[A-Za-z_])", fn.body)) continue;
        int hit_i = 0;
        auto bl = split_lines(fn.body);
        for (int i = 0; i < static_cast<int>(bl.size()); ++i) {
            auto& ln = bl[static_cast<std::size_t>(i)];
            if (_CXX_REFLECT_DEFINE.search(ln) || _CXX_REFLECT_META.search(ln)) {
                hit_i = i;
                break;
            }
        }
        report(out, rel, fn.name, fn.span.first + hit_i, "CXX-REFLECTION",
               "define_aggregate/define_class without a named namespace", lines);
    }
}

// out_ptr/inout_ptr fill then unique_ptr/shared_ptr used without a null check.
void _cxx_out_ptr(const std::vector<std::string>& lines, std::string_view rel,
                  const std::vector<FunctionInfo>& funcs, std::vector<Finding>& out) {
    for (auto& fn : funcs) {
        if (!rx_search(R"(\b(?:inout_ptr|out_ptr)\s*(?:<|\())", fn.body)) continue;
        auto names = names_of(_UNIQUE_PTR_DECL, fn.body);
        for (auto& n : names_of(_SHARED_PTR_DECL, fn.body)) names.insert(n);
        if (names.empty()) continue;
        auto bl = split_lines(fn.body);
        auto start = fn.span.first;
        std::vector<std::pair<std::string, int>> filled;  // insertion order, last line
        for (int i = 0; i < static_cast<int>(bl.size()); ++i)
            for (auto& m : _OUT_PTR_CALL.finditer(bl[static_cast<std::size_t>(i)])) {
                auto ptr = m.named("ptr");
                auto it = std::find_if(filled.begin(), filled.end(), [&](auto& p) { return p.first == ptr; });
                if (it == filled.end()) filled.emplace_back(ptr, i);
                else it->second = i;
            }
        if (filled.empty()) continue;
        for (int i = 0; i < static_cast<int>(bl.size()); ++i) {
            auto& ln = bl[static_cast<std::size_t>(i)];
            for (auto& [name, prev] : filled) {
                if (i <= prev || !names.count(name)) continue;
                if (optional_has_guard(name, fn.body)) continue;
                auto n = re_escape(name);
                if (!(rx_search("(?<!\\w)\\*\\s*" + n + "\\b", ln) || rx_search("\\b" + n + "\\s*->", ln) ||
                      rx_search("\\b" + n + "\\s*\\.\\s*get\\s*\\(", ln)))
                    continue;
                report(out, rel, fn.name, start + i, "CXX-OUT-PTR",
                       name + " used after out_ptr without a null check", lines);
                return;
            }
        }
    }
}

void _cxx_flat_multimap(const std::vector<std::string>& lines, std::string_view rel,
                        const std::vector<FunctionInfo>& funcs, std::vector<Finding>& out) {
    cxx_find_no_end(lines, rel, funcs, out, _CXX_FLAT_MULTIMAP_DECL, "CXX-FLAT-MULTIMAP",
                    ".find() dereferenced without != end()");
}
void _cxx_flat_multiset(const std::vector<std::string>& lines, std::string_view rel,
                        const std::vector<FunctionInfo>& funcs, std::vector<Finding>& out) {
    cxx_find_no_end(lines, rel, funcs, out, _CXX_FLAT_MULTISET_DECL, "CXX-FLAT-MULTISET",
                    ".find() dereferenced without != end()");
}

void _cxx_spanstream(const std::vector<std::string>& lines, std::string_view rel,
                     const std::vector<FunctionInfo>& funcs, std::vector<Finding>& out) {
    for (auto& fn : funcs) {
        auto names = names_of(_CXX_SPANSTREAM_DECL, fn.body);
        auto start = fn.span.first;
        auto bl = split_lines(fn.body);
        for (int i = 0; i < static_cast<int>(bl.size()); ++i) {
            for (auto& name : names) {
                auto n = re_escape(name);
                if (re_search("\\breturn\\s+" + n + "\\s*\\.\\s*span\\s*\\(\\s*\\)\\s*\\.\\s*data\\s*\\(", bl[static_cast<std::size_t>(i)])
                    || re_search("(?:std\\s*::\\s*)?string_view\\b[^;]*" + n + "\\s*\\.\\s*span\\s*\\(", bl[static_cast<std::size_t>(i)])) {
                    report(out, rel, fn.name, start + i, "CXX-SPANSTREAM",
                           name + ".span() view/data outlives the stream", lines);
                    return;
                }
            }
        }
    }
}

void _cxx_barrier(const std::vector<std::string>& lines, std::string_view rel,
                  const std::vector<FunctionInfo>& funcs, std::vector<Finding>& out) {
    for (auto& fn : funcs) {
        auto names = names_of(_CXX_BARRIER_DECL, fn.body);
        auto start = fn.span.first;
        auto bl = split_lines(fn.body);
        for (auto& name : names) {
            auto n = re_escape(name);
            if (re_search("\\b" + n + "\\s*\\.\\s*arrive_and_wait\\s*\\(", fn.body)) continue;
            if (re_search("\\b" + n + "\\s*\\.\\s*wait\\s*\\(", fn.body)) continue;
            for (int i = 0; i < static_cast<int>(bl.size()); ++i)
                if (re_search("\\b" + n + "\\s*\\.\\s*arrive\\s*\\(", bl[static_cast<std::size_t>(i)])) {
                    report(out, rel, fn.name, start + i, "CXX-BARRIER",
                           name + ".arrive() without wait()/arrive_and_wait()", lines);
                    return;
                }
        }
    }
}

void _cxx_task(const std::vector<std::string>& lines, std::string_view rel,
               const std::vector<FunctionInfo>& funcs, std::vector<Finding>& out) {
    cxx_token_unless(lines, rel, funcs, out, _CXX_TASK_STARTED, R"(\bsync_wait\s*\(|\bco_await\b)",
                     "CXX-TASK", "std::task started without sync_wait/co_await");
}

void _cxx_generator(const std::vector<std::string>& lines, std::string_view rel,
                    const std::vector<FunctionInfo>& funcs, std::vector<Finding>& out) {
    for (auto& fn : funcs) {
        if (!re_search(R"(\bgenerator\s*<)", fn.body) && !re_search(R"(\bco_yield\b)", fn.body)) continue;
        auto [sc, arrays] = locals_in_fn(fn);
        if (arrays.empty() && !re_search(R"(generator\s*<[^>]*&)", fn.body)) continue;
        auto start = fn.span.first;
        auto bl = split_lines(fn.body);
        for (int i = 0; i < static_cast<int>(bl.size()); ++i)
            if (re_search(R"(\bco_yield\b)", bl[static_cast<std::size_t>(i)])) {
                report(out, rel, fn.name, start + i, "CXX-GENERATOR",
                       "std::generator yields a pointer/reference to a local", lines);
                return;
            }
    }
}

void _cxx_generator_discard(const std::vector<std::string>& lines, std::string_view rel,
                            const std::vector<FunctionInfo>& funcs, std::vector<Finding>& out) {
    for (auto& fn : funcs) {
        if (re_search(R"(\bfor\s*\()", fn.body)) continue;
        cxx_first_token(lines, rel, funcs, out, _CXX_GENERATOR_ASSIGN, "CXX-GENERATOR-DISCARD",
                        "std::generator constructed and not iterated");
        break;
    }
}

void _cxx_osyncstream(const std::vector<std::string>& lines, std::string_view rel,
                      const std::vector<FunctionInfo>& funcs, std::vector<Finding>& out) {
    for (auto& fn : funcs) {
        auto start = fn.span.first;
        auto bl = split_lines(fn.body);
        for (int i = 0; i < static_cast<int>(bl.size()); ++i) {
            auto m = _CXX_OSYNC_DECL.search_match(bl[static_cast<std::size_t>(i)]);
            if (!m) continue;
            if (re_search("\\b" + re_escape(m->named("name")) + "\\s*\\.\\s*emit\\s*\\(", fn.body)) continue;
            report(out, rel, fn.name, start + i, "CXX-OSYNCSTREAM",
                   m->named("name") + " destroyed without emit()", lines);
            return;
        }
    }
}

void _cxx_syncbuf(const std::vector<std::string>& lines, std::string_view rel,
                  const std::vector<FunctionInfo>& funcs, std::vector<Finding>& out) {
    for (auto& fn : funcs) {
        auto start = fn.span.first;
        auto bl = split_lines(fn.body);
        for (int i = 0; i < static_cast<int>(bl.size()); ++i) {
            auto m = _CXX_SYNCBUF_DECL.search_match(bl[static_cast<std::size_t>(i)]);
            if (!m) continue;
            if (re_search("\\b" + re_escape(m->named("name")) + "\\s*\\.\\s*emit\\s*\\(", fn.body)) continue;
            report(out, rel, fn.name, start + i, "CXX-SYNCBUF",
                   m->named("name") + " destroyed without emit()", lines);
            return;
        }
    }
}

void _cxx_packaged_task(const std::vector<std::string>& lines, std::string_view rel,
                        const std::vector<FunctionInfo>& funcs, std::vector<Finding>& out) {
    for (auto& fn : funcs) {
        auto names = names_of(_CXX_PACKAGED_DECL, fn.body);
        auto start = fn.span.first;
        auto bl = split_lines(fn.body);
        for (auto& name : names) {
            if (optional_has_guard(name, fn.body)) continue;
            for (int i = 0; i < static_cast<int>(bl.size()); ++i)
                if (re_search("\\b" + re_escape(name) + "\\s*\\(", bl[static_cast<std::size_t>(i)])
                    && !_CXX_PACKAGED_DECL.search(bl[static_cast<std::size_t>(i)])) {
                    report(out, rel, fn.name, start + i, "CXX-PACKAGED-TASK",
                           name + " invoked without a truth test", lines);
                    return;
                }
        }
    }
}

void _cxx_counted_iterator(const std::vector<std::string>& lines, std::string_view rel,
                           const std::vector<FunctionInfo>& funcs, std::vector<Finding>& out) {
    for (auto& fn : funcs) {
        auto start = fn.span.first;
        auto bl = split_lines(fn.body);
        for (int i = 0; i < static_cast<int>(bl.size()); ++i) {
            auto m = _CXX_COUNTED_ITER_CTOR.search_match(bl[static_cast<std::size_t>(i)]);
            if (!m) continue;
            if (byteswap_idx_guarded(m->named("n"), fn.body)) continue;
            report(out, rel, fn.name, start + i, "CXX-COUNTED-ITERATOR",
                   "counted_iterator count " + m->named("n") + " has no remaining-size guard", lines);
            return;
        }
    }
}

// promise.get_future() then .get() without set_value (CWE-394).
void _cxx_promise(const std::vector<std::string>& lines, std::string_view rel,
                  const std::vector<FunctionInfo>& funcs, std::vector<Finding>& out) {
    for (auto& fn : funcs) {
        if (names_of(_CXX_PROMISE_DECL, fn.body).empty()) continue;
        if (!rx_search(R"(\bget_future\s*\()", fn.body)) continue;
        if (rx_search(R"(\bset_value(?:_at_thread_exit)?\s*\()", fn.body)) continue;
        if (rx_search(R"(\bset_exception(?:_at_thread_exit)?\s*\()", fn.body)) continue;
        auto start = fn.span.first;
        auto bl = split_lines(fn.body);
        for (int i = 0; i < static_cast<int>(bl.size()); ++i) {
            auto& ln = bl[static_cast<std::size_t>(i)];
            if (rx_search(R"(\.\s*get_future\s*\()", ln)) continue;
            if (!rx_search(R"(\.\s*get\s*\()", ln)) continue;
            report(out, rel, fn.name, start + i, "CXX-PROMISE",
                   "promise get_future used without set_value/set_exception", lines);
            return;
        }
    }
}

void _cxx_weak_ptr(const std::vector<std::string>& lines, std::string_view rel,
                   const std::vector<FunctionInfo>& funcs, std::vector<Finding>& out) {
    for (auto& fn : funcs) {
        auto names = names_of(_CXX_WEAK_PTR_DECL, fn.body);
        auto start = fn.span.first;
        auto bl = split_lines(fn.body);
        for (int i = 0; i < static_cast<int>(bl.size()); ++i) {
            auto m = _CXX_WEAK_LOCK.search_match(bl[static_cast<std::size_t>(i)]);
            if (!m || !names.count(m->named("weak"))) continue;
            if (re_search("\\b" + re_escape(m->named("weak")) + "\\s*\\.\\s*expired\\s*\\(", fn.body)
                || optional_has_guard(m->named("locked"), fn.body))
                continue;
            report(out, rel, fn.name, start + i, "CXX-WEAK-PTR",
                   m->named("name").empty() ? (m->named("locked") + ".lock() used without expired() or a truth test")
                                            : (m->named("locked") + ".lock() used without expired() or a truth test"),
                   lines);
            return;
        }
    }
}

void _cxx_exception_ptr(const std::vector<std::string>& lines, std::string_view rel,
                        const std::vector<FunctionInfo>& funcs, std::vector<Finding>& out) {
    for (auto& fn : funcs) {
        auto names = names_of(_CXX_EXCEPTION_PTR_DECL, fn.body);
        auto start = fn.span.first;
        auto bl = split_lines(fn.body);
        for (auto& name : names) {
            if (param_null_tested(name, fn.body) || optional_has_guard(name, fn.body)) continue;
            for (int i = 0; i < static_cast<int>(bl.size()); ++i)
                if (re_search("\\brethrow_exception\\s*\\(\\s*" + re_escape(name), bl[static_cast<std::size_t>(i)])) {
                    report(out, rel, fn.name, start + i, "CXX-EXCEPTION-PTR",
                           "rethrow_exception(" + name + ") without a nullptr test", lines);
                    return;
                }
        }
    }
}

void _cxx_coro_handle(const std::vector<std::string>& lines, std::string_view rel,
                      const std::vector<FunctionInfo>& funcs, std::vector<Finding>& out) {
    for (auto& fn : funcs) {
        auto names = names_of(_CXX_CORO_HANDLE_DECL, fn.body);
        auto start = fn.span.first;
        auto bl = split_lines(fn.body);
        for (auto& name : names) {
            if (re_search("\\b" + re_escape(name) + "\\s*\\.\\s*done\\s*\\(", fn.body)) continue;
            for (int i = 0; i < static_cast<int>(bl.size()); ++i)
                if (re_search("\\b" + re_escape(name) + "\\s*\\.\\s*resume\\s*\\(", bl[static_cast<std::size_t>(i)])) {
                    report(out, rel, fn.name, start + i, "CXX-CORO-HANDLE",
                           name + ".resume() without done()", lines);
                    return;
                }
        }
    }
}

void _cxx_valarray(const std::vector<std::string>& lines, std::string_view rel,
                   const std::vector<FunctionInfo>& funcs, std::vector<Finding>& out) {
    cxx_sub_no_size(lines, rel, funcs, out, _CXX_VALARRAY_DECL, "CXX-VALARRAY",
                    " without a size() guard");
}

void _cxx_to_underlying(const std::vector<std::string>& lines, std::string_view rel,
                        const std::vector<FunctionInfo>& funcs, std::vector<Finding>& out) {
    for (auto& fn : funcs) {
        auto start = fn.span.first;
        auto bl = split_lines(fn.body);
        for (int i = 0; i < static_cast<int>(bl.size()); ++i) {
            auto m = _CXX_TO_UNDERLYING.search_match(bl[static_cast<std::size_t>(i)]);
            if (!m) continue;
            if (re_search(R"(\bstatic_cast\s*<)", fn.body) && re_search(R"(\benum\b)", fn.body)) continue;
            report(out, rel, fn.name, start + i, "CXX-TO-UNDERLYING",
                   "to_underlying(" + m->named("arg") + ") without an enum range check", lines);
            return;
        }
    }
}

void _cxx_unexpected(const std::vector<std::string>& lines, std::string_view rel,
                     const std::vector<FunctionInfo>& funcs, std::vector<Finding>& out) {
    for (auto& fn : funcs) {
        auto start = fn.span.first;
        auto bl = split_lines(fn.body);
        for (int i = 0; i < static_cast<int>(bl.size()); ++i) {
            auto m = _CXX_UNEXPECTED_DECL.search_match(bl[static_cast<std::size_t>(i)]);
            if (!m) continue;
            if (re_search("\\breturn\\s+" + re_escape(m->named("name")), fn.body)) continue;
            report(out, rel, fn.name, start + i, "CXX-UNEXPECTED",
                   m->named("name") + " unexpected< constructed and discarded", lines);
            return;
        }
    }
}

// std::get on tuple without tuple_size / structured binding (CWE-125).
void _cxx_tuple_get(const std::vector<std::string>& lines, std::string_view rel,
                    const std::vector<FunctionInfo>& funcs, std::vector<Finding>& out) {
    for (auto& fn : funcs) {
        auto& body = fn.body;
        bool has_tuple = body.find("tuple") != std::string::npos;
        if (body.find("variant") != std::string::npos && !has_tuple) continue;
        if (!has_tuple) continue;
        if (!(rx_search(R"(std\s*::\s*get)", body) || rx_search(R"(get\s*<)", body))) continue;
        if (!_CXX_TUPLE_DECL.search(body)) continue;
        if (rx_search(R"(\btuple_size\b)", body)) continue;
        if (rx_search(R"(\bauto\s*\[)", body)) continue;
        auto start = fn.span.first;
        auto bl = split_lines(body);
        for (int i = 0; i < static_cast<int>(bl.size()); ++i) {
            if (!_CXX_TUPLE_GET.search(bl[static_cast<std::size_t>(i)])) continue;
            report(out, rel, fn.name, start + i, "CXX-TUPLE-GET",
                   "std::get on tuple without tuple_size/index guard", lines);
            return;
        }
    }
}

void _cxx_deque_index(const std::vector<std::string>& lines, std::string_view rel,
                      const std::vector<FunctionInfo>& funcs, std::vector<Finding>& out) {
    cxx_sub_no_size(lines, rel, funcs, out, _CXX_DEQUE_DECL, "CXX-DEQUE-INDEX",
                    " without a size() guard");
}
void _cxx_array_index(const std::vector<std::string>& lines, std::string_view rel,
                      const std::vector<FunctionInfo>& funcs, std::vector<Finding>& out) {
    cxx_sub_no_size(lines, rel, funcs, out, _CXX_ARRAY_DECL, "CXX-ARRAY-INDEX",
                    " without a size() guard");
}
void _cxx_forward_list(const std::vector<std::string>& lines, std::string_view rel,
                       const std::vector<FunctionInfo>& funcs, std::vector<Finding>& out) {
    cxx_method_no_empty(lines, rel, funcs, out, _CXX_FWD_LIST_DECL, "front", "CXX-FORWARD-LIST",
                        ".front() without empty()");
}
void _cxx_list_front(const std::vector<std::string>& lines, std::string_view rel,
                     const std::vector<FunctionInfo>& funcs, std::vector<Finding>& out) {
    cxx_method_no_empty(lines, rel, funcs, out, _CXX_LIST_DECL, "front|back", "CXX-LIST-FRONT",
                        ".front()/.back() without empty()");
}
void _cxx_map_at(const std::vector<std::string>& lines, std::string_view rel,
                 const std::vector<FunctionInfo>& funcs, std::vector<Finding>& out) {
    cxx_at_no_contains(lines, rel, funcs, out, _CXX_MAP_DECL, "CXX-MAP-AT",
                       ".at() without count()/find()/contains()");
}
void _cxx_unordered_at(const std::vector<std::string>& lines, std::string_view rel,
                       const std::vector<FunctionInfo>& funcs, std::vector<Finding>& out) {
    cxx_at_no_contains(lines, rel, funcs, out, _CXX_UMAP_DECL, "CXX-UNORDERED-AT",
                       ".at() without count()/find()/contains()");
}
void _cxx_set_find(const std::vector<std::string>& lines, std::string_view rel,
                   const std::vector<FunctionInfo>& funcs, std::vector<Finding>& out) {
    cxx_find_no_end(lines, rel, funcs, out, _CXX_SET_DECL, "CXX-SET-FIND",
                    ".find() dereferenced without != end()");
}
void _cxx_unordered_set(const std::vector<std::string>& lines, std::string_view rel,
                        const std::vector<FunctionInfo>& funcs, std::vector<Finding>& out) {
    cxx_find_no_end(lines, rel, funcs, out, _CXX_USET_DECL, "CXX-UNORDERED-SET",
                    ".find() dereferenced without != end()");
}
void _cxx_multimap_find(const std::vector<std::string>& lines, std::string_view rel,
                        const std::vector<FunctionInfo>& funcs, std::vector<Finding>& out) {
    cxx_find_no_end(lines, rel, funcs, out, _CXX_MULTIMAP_DECL, "CXX-MULTIMAP-FIND",
                    ".find() dereferenced without != end()");
}
void _cxx_multiset_find(const std::vector<std::string>& lines, std::string_view rel,
                        const std::vector<FunctionInfo>& funcs, std::vector<Finding>& out) {
    cxx_find_no_end(lines, rel, funcs, out, _CXX_MULTISET_DECL, "CXX-MULTISET-FIND",
                    ".find() dereferenced without != end()");
}
void _cxx_unordered_multimap(const std::vector<std::string>& lines, std::string_view rel,
                             const std::vector<FunctionInfo>& funcs, std::vector<Finding>& out) {
    cxx_find_no_end(lines, rel, funcs, out, _CXX_UMMAP_DECL, "CXX-UNORDERED-MULTIMAP",
                    ".find() dereferenced without != end()");
}
void _cxx_unordered_multiset(const std::vector<std::string>& lines, std::string_view rel,
                             const std::vector<FunctionInfo>& funcs, std::vector<Finding>& out) {
    cxx_find_no_end(lines, rel, funcs, out, _CXX_UMSET_DECL, "CXX-UNORDERED-MULTISET",
                    ".find() dereferenced without != end()");
}

void _cxx_queue_front(const std::vector<std::string>& lines, std::string_view rel,
                      const std::vector<FunctionInfo>& funcs, std::vector<Finding>& out) {
    for (auto& fn : funcs) {
        if (fn.body.find("dequeue") != std::string::npos) continue;
        if (fn.body.find("queue") == std::string::npos) continue;
        auto stripped = re_sub_pat("priority_queue", "", fn.body);
        if (!re_search(R"(\bqueue\s*<)", stripped)) continue;
        auto names = names_of(_CXX_QUEUE_DECL, stripped);
        auto start = fn.span.first;
        auto bl = split_lines(fn.body);
        for (auto& name : names) {
            if (re_search("\\b" + re_escape(name) + "\\s*\\.\\s*empty\\s*\\(", fn.body)) continue;
            for (int i = 0; i < static_cast<int>(bl.size()); ++i)
                if (re_search("\\b" + re_escape(name) + "\\s*\\.\\s*(?:front|pop)\\s*\\(", bl[static_cast<std::size_t>(i)])) {
                    report(out, rel, fn.name, start + i, "CXX-QUEUE-FRONT",
                           name + ".front()/.pop() without empty()", lines);
                    return;
                }
        }
    }
}

void _cxx_stack_top(const std::vector<std::string>& lines, std::string_view rel,
                    const std::vector<FunctionInfo>& funcs, std::vector<Finding>& out) {
    for (auto& fn : funcs) {
        if (fn.body.find("stacktrace") != std::string::npos
            && !re_search(R"(\b(?:std\s*::\s*)?stack\s*<)", fn.body))
            continue;
        cxx_method_no_empty(lines, rel, {fn}, out, _CXX_STACK_DECL, "top|pop", "CXX-STACK-TOP",
                            ".top()/.pop() without empty()");
    }
}

void _cxx_priority_queue(const std::vector<std::string>& lines, std::string_view rel,
                         const std::vector<FunctionInfo>& funcs, std::vector<Finding>& out) {
    cxx_method_no_empty(lines, rel, funcs, out, _CXX_PQUEUE_DECL, "top|pop", "CXX-PRIORITY-QUEUE",
                        ".top()/.pop() without empty()");
}

// wstring_view / wstring borrows local storage (CWE-416).
void _cxx_wstring_view(const std::vector<std::string>& lines, std::string_view rel,
                       const std::vector<FunctionInfo>& funcs, std::vector<Finding>& out) {
    view_of_local_return(lines, rel, funcs, out, R"(\bwstring_view\b|\bwstring\b)", R"(\bwstring_view\b)",
                         cxx_local_wstrings, _CXX_WSTRING_VIEW_BIND, _CXX_RETURN_WVIEW,
                         "CXX-WSTRING-VIEW", "return borrows local wstring ");
}

// u8string_view / u8string borrows local storage (CWE-416).
void _cxx_u8string_view(const std::vector<std::string>& lines, std::string_view rel,
                        const std::vector<FunctionInfo>& funcs, std::vector<Finding>& out) {
    view_of_local_return(lines, rel, funcs, out, R"(\bu8string_view\b|\bu8string\b)", R"(\bu8string_view\b)",
                         cxx_local_u8strings, _CXX_U8STRING_VIEW_BIND, _CXX_RETURN_U8VIEW,
                         "CXX-U8STRING-VIEW", "return borrows local u8string ");
}

void _cxx_binary_semaphore(const std::vector<std::string>& lines, std::string_view rel,
                           const std::vector<FunctionInfo>& funcs, std::vector<Finding>& out) {
    for (auto& fn : funcs) {
        auto start = fn.span.first;
        auto bl = split_lines(fn.body);
        for (int i = 0; i < static_cast<int>(bl.size()); ++i) {
            auto m = _CXX_BINSEM_ZERO.search_match(bl[static_cast<std::size_t>(i)]);
            if (!m) continue;
            auto n = re_escape(m->named("name"));
            if (re_search("\\b" + n + "\\s*\\.\\s*release\\s*\\(", fn.body)) continue;
            for (int j = 0; j < static_cast<int>(bl.size()); ++j)
                if (re_search("\\b" + n + "\\s*\\.\\s*acquire\\s*\\(", bl[static_cast<std::size_t>(j)])) {
                    report(out, rel, fn.name, start + j, "CXX-BINARY-SEMAPHORE",
                           m->named("name") + ".acquire() on binary_semaphore(0) without release()",
                           lines);
                    return;
                }
        }
    }
}

void _cxx_error_code(const std::vector<std::string>& lines, std::string_view rel,
                     const std::vector<FunctionInfo>& funcs, std::vector<Finding>& out) {
    for (auto& fn : funcs) {
        auto names = names_of(_CXX_ERROR_CODE_DECL, fn.body);
        auto start = fn.span.first;
        auto bl = split_lines(fn.body);
        for (auto& name : names) {
            if (optional_has_guard(name, fn.body)
                || re_search("\\b" + re_escape(name) + "\\s*\\.\\s*value\\s*\\(", fn.body))
                continue;
            for (int i = 0; i < static_cast<int>(bl.size()); ++i)
                if (re_search("\\b" + re_escape(name) + "\\b", bl[static_cast<std::size_t>(i)])
                    && !_CXX_ERROR_CODE_DECL.search(bl[static_cast<std::size_t>(i)])) {
                    report(out, rel, fn.name, start + i, "CXX-ERROR-CODE",
                           name + " used without if(" + name + ") or .value()", lines);
                    return;
                }
        }
    }
}

void _cxx_byteswap(const std::vector<std::string>& lines, std::string_view rel,
                   const std::vector<FunctionInfo>& funcs, std::vector<Finding>& out) {
    cxx_index_token_scan(lines, rel, funcs, out, Regex(R"(\bbyteswap\s*\()"), _BYTESWAP_IN_INDEX,
                         _BYTESWAP_ASSIGN, "CXX-BYTESWAP",
                         "byteswap() result used as an array index without a range check");
}

void _cxx_pmr(const std::vector<std::string>& lines, std::string_view rel,
              const std::vector<FunctionInfo>& funcs, std::vector<Finding>& out) {
    for (auto& fn : funcs) {
        if (fn.body.find("pmr::") == std::string::npos && fn.body.find("std::pmr") == std::string::npos)
            continue;
        if (re_search(R"(\.\s*size\s*\()", fn.body)) continue;
        auto start = fn.span.first;
        auto bl = split_lines(fn.body);
        for (int i = 0; i < static_cast<int>(bl.size()); ++i)
            if (_CXX_SUBSCRIPT.search(bl[static_cast<std::size_t>(i)])) {
                report(out, rel, fn.name, start + i, "CXX-PMR",
                       "pmr container operator[] without size() guard", lines);
                return;
            }
    }
}

void _cxx_shared_lock(const std::vector<std::string>& lines, std::string_view rel,
                      const std::vector<FunctionInfo>& funcs, std::vector<Finding>& out) {
    for (auto& fn : funcs) {
        auto names = names_of(_CXX_SHARED_LOCK_DECL, fn.body);
        auto start = fn.span.first;
        auto bl = split_lines(fn.body);
        for (auto& name : names) {
            if (re_search("\\b" + re_escape(name) + "\\s*\\.\\s*owns_lock\\s*\\(", fn.body)
                || optional_has_guard(name, fn.body))
                continue;
            for (int i = 0; i < static_cast<int>(bl.size()); ++i)
                if (re_search("\\b" + re_escape(name) + "\\b", bl[static_cast<std::size_t>(i)])
                    && !_CXX_SHARED_LOCK_DECL.search(bl[static_cast<std::size_t>(i)])) {
                    report(out, rel, fn.name, start + i, "CXX-SHARED-LOCK",
                           name + " used without owns_lock() or if (" + name + ")", lines);
                    return;
                }
        }
    }
}

void _cxx_atomic_flag(const std::vector<std::string>& lines, std::string_view rel,
                      const std::vector<FunctionInfo>& funcs, std::vector<Finding>& out) {
    for (auto& fn : funcs) {
        auto names = names_of(_CXX_ATOMIC_FLAG_DECL, fn.body);
        auto start = fn.span.first;
        auto bl = split_lines(fn.body);
        for (auto& name : names) {
            if (re_search("\\b" + re_escape(name) + "\\s*\\.\\s*clear\\s*\\(", fn.body)) continue;
            for (int i = 0; i < static_cast<int>(bl.size()); ++i)
                if (re_search("\\b" + re_escape(name) + "\\s*\\.\\s*test_and_set\\s*\\(", bl[static_cast<std::size_t>(i)])) {
                    report(out, rel, fn.name, start + i, "CXX-ATOMIC-FLAG",
                           name + ".test_and_set() without a prior clear()", lines);
                    return;
                }
        }
    }
}

void _cxx_condvar_any(const std::vector<std::string>& lines, std::string_view rel,
                      const std::vector<FunctionInfo>& funcs, std::vector<Finding>& out) {
    for (auto& fn : funcs) {
        auto names = names_of(_CXX_CVANY_DECL, fn.body);
        auto start = fn.span.first;
        auto bl = split_lines(fn.body);
        for (auto& name : names) {
            for (int i = 0; i < static_cast<int>(bl.size()); ++i)
                if (re_search("\\b" + re_escape(name) + "\\s*\\.\\s*wait", bl[static_cast<std::size_t>(i)])
                    && !re_search(R"(,\s*\[)", bl[static_cast<std::size_t>(i)])) {
                    report(out, rel, fn.name, start + i, "CXX-CONDVAR-ANY",
                           name + ".wait() without a lock / predicate", lines);
                    return;
                }
        }
    }
}

void _cxx_recursive_mutex(const std::vector<std::string>& lines, std::string_view rel,
                          const std::vector<FunctionInfo>& funcs, std::vector<Finding>& out) {
    for (auto& fn : funcs) {
        auto names = names_of(_CXX_RECURSIVE_MUTEX_DECL, fn.body);
        auto start = fn.span.first;
        auto bl = split_lines(fn.body);
        for (auto& name : names) {
            if (re_search("\\b" + re_escape(name) + "\\s*\\.\\s*unlock", fn.body) || _CXX_LOCK_RAII.search(fn.body))
                continue;
            for (int i = 0; i < static_cast<int>(bl.size()); ++i)
                if (re_search("\\b" + re_escape(name) + "\\s*\\.\\s*lock\\s*\\(", bl[static_cast<std::size_t>(i)])) {
                    report(out, rel, fn.name, start + i, "CXX-RECURSIVE-MUTEX",
                           name + ".lock() without unlock or lock_guard", lines);
                    return;
                }
        }
    }
}
void _cxx_timed_mutex(const std::vector<std::string>& lines, std::string_view rel,
                      const std::vector<FunctionInfo>& funcs, std::vector<Finding>& out) {
    for (auto& fn : funcs) {
        auto names = names_of(_CXX_TIMED_MUTEX_DECL, fn.body);
        auto start = fn.span.first;
        auto bl = split_lines(fn.body);
        for (auto& name : names) {
            auto n = re_escape(name);
            bool unlocked = re_search("\\b" + n + "\\s*\\.\\s*unlock", fn.body) || _CXX_LOCK_RAII.search(fn.body);
            for (int i = 0; i < static_cast<int>(bl.size()); ++i) {
                auto& ln = bl[static_cast<std::size_t>(i)];
                bool try_disc = re_search("\\b" + n + "\\s*\\.\\s*try_lock", ln)
                    && !re_search(R"(\bif\s*\()", ln);
                bool lock_bare = re_search("\\b" + n + "\\s*\\.\\s*lock\\s*\\(", ln) && !unlocked;
                if (!try_disc && !lock_bare) continue;
                report(out, rel, fn.name, start + i, "CXX-TIMED-MUTEX",
                       name + " try_lock/lock discarded or lock without unlock", lines);
                return;
            }
        }
    }
}

// ifstream/ofstream used without is_open() / truth test (CWE-252).
void _cxx_fstream(const std::vector<std::string>& lines, std::string_view rel,
                  const std::vector<FunctionInfo>& funcs, std::vector<Finding>& out) {
    for (auto& fn : funcs) {
        if (!rx_search(R"(\b(?:ifstream|ofstream|fstream)\b)", fn.body)) continue;
        auto names = names_of(_CXX_FSTREAM_DECL, fn.body);
        if (names.empty()) continue;
        auto start = fn.span.first;
        auto bl = split_lines(fn.body);
        for (auto& name : names) {
            auto n = re_escape(name);
            if (rx_search("\\b" + n + "\\s*\\.\\s*is_open\\s*\\(", fn.body)) continue;
            if (rx_search("\\bif\\s*\\(\\s*!?" + n + "\\b", fn.body)) continue;
            Regex use("\\b" + n + "\\s*\\.\\s*(?:get|read|write|getline|put|peek)\\s*\\(");
            Regex shift("\\b" + n + "\\s*(?:<<|>>)");
            for (int i = 0; i < static_cast<int>(bl.size()); ++i) {
                auto& ln = bl[static_cast<std::size_t>(i)];
                if (!(use.search(ln) || shift.search(ln))) continue;
                report(out, rel, fn.name, start + i, "CXX-FSTREAM",
                       name + " used without is_open() or a truth test", lines);
                return;
            }
        }
    }
}

void _cxx_this_thread(const std::vector<std::string>& lines, std::string_view rel,
                      const std::vector<FunctionInfo>& funcs, std::vector<Finding>& out) {
    for (auto& fn : funcs) {
        if (!re_search(R"(\bthis_thread\s*::\s*sleep_for\s*\()", fn.body)) continue;
        if (re_search(R"(\b(?:mutex|condition_variable|atomic|latch|barrier)\b)", fn.body)) continue;
        auto start = fn.span.first;
        auto bl = split_lines(fn.body);
        for (int i = 0; i < static_cast<int>(bl.size()); ++i)
            if (re_search(R"(\bsleep_for\s*\()", bl[static_cast<std::size_t>(i)])) {
                report(out, rel, fn.name, start + i, "CXX-THIS-THREAD",
                       "this_thread::sleep_for used as the only sync", lines);
                return;
            }
    }
}

void _cxx_call_once(const std::vector<std::string>& lines, std::string_view rel,
                    const std::vector<FunctionInfo>& funcs, std::vector<Finding>& out) {
    for (auto& fn : funcs) {
        if (!re_search(R"(\bcall_once\s*\()", fn.body)) continue;
        if (re_search(R"(\bonce_flag\b)", fn.body)) continue;
        auto start = fn.span.first;
        auto bl = split_lines(fn.body);
        for (int i = 0; i < static_cast<int>(bl.size()); ++i)
            if (re_search(R"(\bcall_once\s*\()", bl[static_cast<std::size_t>(i)])) {
                report(out, rel, fn.name, start + i, "CXX-CALL-ONCE",
                       "std::call_once invoked without an once_flag", lines);
                return;
            }
    }
}

void _cxx_shared_timed_mutex(const std::vector<std::string>& lines, std::string_view rel,
                             const std::vector<FunctionInfo>& funcs, std::vector<Finding>& out) {
    for (auto& fn : funcs) {
        auto names = names_of(_CXX_SHARED_TIMED_MUTEX_DECL, fn.body);
        auto start = fn.span.first;
        auto bl = split_lines(fn.body);
        for (auto& name : names) {
            auto n = re_escape(name);
            if (re_search("\\b" + n + "\\s*\\.\\s*unlock", fn.body) || _CXX_LOCK_RAII.search(fn.body))
                continue;
            for (int i = 0; i < static_cast<int>(bl.size()); ++i)
                if (re_search("\\b" + n + "\\s*\\.\\s*(?:lock|lock_shared)\\s*\\(", bl[static_cast<std::size_t>(i)])) {
                    report(out, rel, fn.name, start + i, "CXX-SHARED-TIMED-MUTEX",
                           name + ".lock/lock_shared without unlock", lines);
                    return;
                }
        }
    }
}

void _cxx_recursive_timed_mutex(const std::vector<std::string>& lines, std::string_view rel,
                                const std::vector<FunctionInfo>& funcs, std::vector<Finding>& out) {
    for (auto& fn : funcs) {
        auto names = names_of(_CXX_RECURSIVE_TIMED_MUTEX_DECL, fn.body);
        auto start = fn.span.first;
        auto bl = split_lines(fn.body);
        for (auto& name : names) {
            if (re_search("\\b" + re_escape(name) + "\\s*\\.\\s*unlock", fn.body) || _CXX_LOCK_RAII.search(fn.body))
                continue;
            for (int i = 0; i < static_cast<int>(bl.size()); ++i)
                if (re_search("\\b" + re_escape(name) + "\\s*\\.\\s*lock\\s*\\(", bl[static_cast<std::size_t>(i)])) {
                    report(out, rel, fn.name, start + i, "CXX-RECURSIVE-TIMED-MUTEX",
                           name + ".lock() without unlock", lines);
                    return;
                }
        }
    }
}

void _cxx_system_error(const std::vector<std::string>& lines, std::string_view rel,
                       const std::vector<FunctionInfo>& funcs, std::vector<Finding>& out) {
    for (auto& fn : funcs) {
        if (!_CXX_SYSTEM_ERROR_DECL.search(fn.body) && !_CXX_SYSTEM_ERROR_CATCH.search(fn.body)) continue;
        if (re_search(R"(\.code\s*\()", fn.body)) continue;
        auto start = fn.span.first;
        auto bl = split_lines(fn.body);
        for (int i = 0; i < static_cast<int>(bl.size()); ++i)
            if (_CXX_SYSTEM_ERROR_DECL.search(bl[static_cast<std::size_t>(i)])
                || _CXX_SYSTEM_ERROR_CATCH.search(bl[static_cast<std::size_t>(i)])) {
                report(out, rel, fn.name, start + i, "CXX-SYSTEM-ERROR",
                       "std::system_error used without a .code() check", lines);
                return;
            }
    }
}

void _cxx_chrono_tzdb(const std::vector<std::string>& lines, std::string_view rel,
                      const std::vector<FunctionInfo>& funcs, std::vector<Finding>& out) {
    for (auto& fn : funcs) {
        if (!_CXX_TZDB_CALL.search(fn.body)) continue;
        auto names = names_of(_CXX_TZDB_BIND, fn.body);
        bool guarded = !names.empty();
        for (auto& n : names)
            if (!param_if_guard(n, fn.body) && !param_null_tested(n, fn.body)) guarded = false;
        if (guarded) continue;
        auto start = fn.span.first;
        auto bl = split_lines(fn.body);
        for (int i = 0; i < static_cast<int>(bl.size()); ++i)
            if (_CXX_TZDB_CALL.search(bl[static_cast<std::size_t>(i)])) {
                report(out, rel, fn.name, start + i, "CXX-CHRONO-TZDB",
                       "current_zone/tzdb used without a null/locate check", lines);
                return;
            }
    }
}

void _cxx_ranges_zip(const std::vector<std::string>& lines, std::string_view rel,
                     const std::vector<FunctionInfo>& funcs, std::vector<Finding>& out) {
    for (auto& fn : funcs) {
        if (!_CXX_VIEWS_ZIP.search(fn.body)) continue;
        auto names = names_of(_CXX_ZIP_AUTO, fn.body);
        auto more = names_of(_CXX_ZIP_VIEW_DECL, fn.body);
        names.insert(more.begin(), more.end());
        auto start = fn.span.first;
        auto bl = split_lines(fn.body);
        for (int i = 0; i < static_cast<int>(bl.size()); ++i)
            for (auto& m : _CXX_SUBSCRIPT.finditer(bl[static_cast<std::size_t>(i)])) {
                if (!names.empty() && !names.count(m.named("cont"))) continue;
                if (!toarr_idx_bad(strip(m.named("idx")), std::nullopt, fn.body)) continue;
                report(out, rel, fn.name, start + i, "CXX-RANGES-ZIP",
                       m.named("cont") + " indexed/iterated without a size guard", lines);
                return;
            }
    }
}

void _cxx_ranges_join(const std::vector<std::string>& lines, std::string_view rel,
                      const std::vector<FunctionInfo>& funcs, std::vector<Finding>& out) {
    for (auto& fn : funcs) {
        if (!_CXX_VIEWS_JOIN.search(fn.body)) continue;
        auto names = names_of(_CXX_JOIN_AUTO, fn.body);
        auto more = names_of(_CXX_JOIN_VIEW_DECL, fn.body);
        names.insert(more.begin(), more.end());
        auto start = fn.span.first;
        auto bl = split_lines(fn.body);
        for (int i = 0; i < static_cast<int>(bl.size()); ++i)
            for (auto& m : _CXX_SUBSCRIPT.finditer(bl[static_cast<std::size_t>(i)])) {
                if (!names.empty() && !names.count(m.named("cont"))) continue;
                if (!toarr_idx_bad(strip(m.named("idx")), std::nullopt, fn.body)) continue;
                report(out, rel, fn.name, start + i, "CXX-RANGES-JOIN",
                       m.named("cont") + " indexed/iterated without a size guard", lines);
                return;
            }
    }
}

void _cxx_format_to(const std::vector<std::string>& lines, std::string_view rel,
                    const std::vector<FunctionInfo>& funcs, std::vector<Finding>& out) {
    for (auto& fn : funcs) {
        auto bufs = names_of(_CXX_CHAR_BUF, fn.body);
        auto start = fn.span.first;
        auto bl = split_lines(fn.body);
        for (int i = 0; i < static_cast<int>(bl.size()); ++i) {
            auto m = _CXX_FORMAT_TO.search_match(bl[static_cast<std::size_t>(i)]);
            if (!m) continue;
            if (!bufs.count(m->named("dst"))) continue;
            if (re_search(R"(\bformat_to_n\s*\()", fn.body)) continue;
            report(out, rel, fn.name, start + i, "CXX-FORMAT-TO",
                   "format_to into a raw char buffer without format_to_n", lines);
            return;
        }
    }
}

// error_category used without == / .name() / .message() (CWE-252).
void _cxx_error_category(const std::vector<std::string>& lines, std::string_view rel,
                         const std::vector<FunctionInfo>& funcs, std::vector<Finding>& out) {
    for (auto& fn : funcs) {
        if (!_CXX_ERROR_CATEGORY.search(fn.body)) continue;
        if (fn.body.find("==") != std::string::npos) continue;
        if (rx_search(R"(\.\s*(?:name|message)\s*\()", fn.body)) continue;
        auto start = fn.span.first;
        auto bl = split_lines(fn.body);
        for (int i = 0; i < static_cast<int>(bl.size()); ++i) {
            if (!_CXX_ERROR_CATEGORY.search(bl[static_cast<std::size_t>(i)])) continue;
            report(out, rel, fn.name, start + i, "CXX-ERROR-CATEGORY",
                   "error_category used without generic/system compare or .message()", lines);
            return;
        }
    }
}

void _cxx_nested_exception(const std::vector<std::string>& lines, std::string_view rel,
                           const std::vector<FunctionInfo>& funcs, std::vector<Finding>& out) {
    cxx_token_unless(lines, rel, funcs, out, _CXX_NESTED_EXC, R"(\bcurrent_exception\s*\()",
                     "CXX-NESTED-EXCEPTION",
                     "throw_with_nested / nested_exception without current_exception");
}

// atomic_*_fence without memory_order, or as only sync (CWE-362).
void _cxx_atomic_fence(const std::vector<std::string>& lines, std::string_view rel,
                       const std::vector<FunctionInfo>& funcs, std::vector<Finding>& out) {
    auto globals_ = file_int_globals(lines);
    for (auto& fn : funcs) {
        if (!_CXX_ATOMIC_FENCE.search(fn.body)) continue;
        bool missing_order = !_CXX_FENCE_ORDER.search(fn.body);
        bool has_atomic_obj = rx_search(R"(\batomic\s*<)", fn.body);
        auto stores = globals_;
        static const Regex statics(R"(\bstatic\s+int\s+([A-Za-z_]\w*)\b)");
        for (auto& m : statics.finditer(fn.body)) stores.insert(m.group(1));
        bool has_plain_store = false;
        for (auto& g : stores)
            if (rx_search("\\b" + re_escape(g) + "\\s*=", fn.body)) has_plain_store = true;
        if (!missing_order && (has_atomic_obj || !has_plain_store)) continue;
        auto start = fn.span.first;
        auto bl = split_lines(fn.body);
        for (int i = 0; i < static_cast<int>(bl.size()); ++i) {
            if (!_CXX_ATOMIC_FENCE.search(bl[static_cast<std::size_t>(i)])) continue;
            report(out, rel, fn.name, start + i, "CXX-ATOMIC-FENCE",
                   "atomic_thread_fence without memory_order or as only sync around a store", lines);
            return;
        }
    }
}

void _cxx_notify_thread_exit(const std::vector<std::string>& lines, std::string_view rel,
                             const std::vector<FunctionInfo>& funcs, std::vector<Finding>& out) {
    cxx_token_unless(lines, rel, funcs, out, _CXX_NOTIFY_EXIT, R"(\bwait(?:_for|_until)?\s*\()",
                     "CXX-NOTIFY-THREAD-EXIT",
                     "notify_all_at_thread_exit without a waiting thread");
}

// wstring_convert without .converted() / .empty() (CWE-416/252).
void _cxx_wstring_convert(const std::vector<std::string>& lines, std::string_view rel,
                          const std::vector<FunctionInfo>& funcs, std::vector<Finding>& out) {
    for (auto& fn : funcs) {
        if (!_CXX_WSTRING_CONVERT.search(fn.body)) continue;
        if (!_CXX_WCONVERT_XFORM.search(fn.body)) continue;
        if (rx_search(R"(\.\s*converted\s*\()", fn.body)) continue;
        if (rx_search(R"(\.\s*empty\s*\()", fn.body)) continue;
        auto start = fn.span.first;
        auto bl = split_lines(fn.body);
        for (int i = 0; i < static_cast<int>(bl.size()); ++i) {
            auto& ln = bl[static_cast<std::size_t>(i)];
            if (!(_CXX_WCONVERT_XFORM.search(ln) || _CXX_WSTRING_CONVERT.search(ln))) continue;
            report(out, rel, fn.name, start + i, "CXX-WSTRING-CONVERT",
                   "wstring_convert without .converted() or dangling local converter", lines);
            return;
        }
    }
}

void _cxx_invoke(const std::vector<std::string>& lines, std::string_view rel,
                 const std::vector<FunctionInfo>& funcs, std::vector<Finding>& out) {
    for (auto& fn : funcs) {
        auto start = fn.span.first;
        auto bl = split_lines(fn.body);
        for (int i = 0; i < static_cast<int>(bl.size()); ++i) {
            auto m = _CXX_INVOKE.search_match(bl[static_cast<std::size_t>(i)]);
            if (!m) continue;
            if (optional_has_guard(m->named("arg"), fn.body)) continue;
            report(out, rel, fn.name, start + i, "CXX-INVOKE",
                   "std::invoke on a nullable callable without a truth test", lines);
            return;
        }
    }
}

void _cxx_apply(const std::vector<std::string>& lines, std::string_view rel,
                const std::vector<FunctionInfo>& funcs, std::vector<Finding>& out) {
    for (auto& fn : funcs) {
        auto start = fn.span.first;
        auto bl = split_lines(fn.body);
        for (int i = 0; i < static_cast<int>(bl.size()); ++i) {
            auto m = _CXX_APPLY.search_match(bl[static_cast<std::size_t>(i)]);
            if (!m) continue;
            if (optional_has_guard(m->named("arg"), fn.body)) continue;
            report(out, rel, fn.name, start + i, "CXX-APPLY",
                   "std::apply on a nullable callable or tuple without a guard", lines);
            return;
        }
    }
}

// std::ref/cref of a local that is returned (CWE-416).
void _cxx_reference_wrapper(const std::vector<std::string>& lines, std::string_view rel,
                            const std::vector<FunctionInfo>& funcs, std::vector<Finding>& out) {
    for (auto& fn : funcs) {
        if (!_CXX_REFWRAP_TOKEN.search(fn.body)) continue;
        auto locals_ = cxx_local_scalar_names(fn);
        std::set<std::string> params, wrapped;
        for (auto& [typ, name] : fn.params)
            if (!name.empty()) params.insert(name);
        for (auto& m : _CXX_REFWRAP_BIND.finditer(fn.body)) {
            auto arg = m.named("arg");
            if (locals_.count(arg) && !params.count(arg)) wrapped.insert(m.named("w"));
        }
        auto start = fn.span.first;
        auto bl = split_lines(fn.body);
        for (int i = 0; i < static_cast<int>(bl.size()); ++i) {
            auto& ln = bl[static_cast<std::size_t>(i)];
            bool hit = false;
            auto rm = _CXX_REFWRAP_RETURN.search_match(ln);
            if (rm && locals_.count(rm->named("arg")) && !params.count(rm->named("arg"))) {
                hit = true;
            } else if (auto rv = _RETURN_VAR.search_match(ln)) {
                if (wrapped.count(rv->group(1))) hit = true;
            }
            if (!hit) continue;
            report(out, rel, fn.name, start + i, "CXX-REFERENCE-WRAPPER",
                   "std::ref/cref/reference_wrapper wraps a local that is returned", lines);
            return;
        }
    }
}

void _cxx_endian(const std::vector<std::string>& lines, std::string_view rel,
                 const std::vector<FunctionInfo>& funcs, std::vector<Finding>& out) {
    cxx_index_token_scan(lines, rel, funcs, out, _CXX_ENDIAN, _ENDIAN_IN_INDEX, _ENDIAN_ASSIGN,
                         "CXX-ENDIAN", "std::endian used as an array index without a range check");
}

void _cxx_bit_ceil(const std::vector<std::string>& lines, std::string_view rel,
                   const std::vector<FunctionInfo>& funcs, std::vector<Finding>& out) {
    cxx_index_token_scan(lines, rel, funcs, out, _CXX_BITCEIL_TOKEN, _BITCEIL_IN_INDEX, _BITCEIL_ASSIGN,
                         "CXX-BIT-CEIL",
                         "bit_ceil/bit_floor/popcount used as an index without a range check");
}

void _cxx_uncaught_exceptions(const std::vector<std::string>& lines, std::string_view rel,
                              const std::vector<FunctionInfo>& funcs, std::vector<Finding>& out) {
    for (auto& fn : funcs) {
        if (!_CXX_UNCAUGHT.search(fn.body)) continue;
        if (_UNCAUGHT_SAVED.search(fn.body)) continue;
        auto start = fn.span.first;
        auto bl = split_lines(fn.body);
        for (int i = 0; i < static_cast<int>(bl.size()); ++i)
            if (_UNCAUGHT_BOOL_IF.search(bl[static_cast<std::size_t>(i)])) {
                report(out, rel, fn.name, start + i, "CXX-UNCAUGHT-EXCEPTIONS",
                       "std::uncaught_exceptions() used as a boolean", lines);
                break;  // one per function
            }
    }
}

void _cxx_quick_exit(const std::vector<std::string>& lines, std::string_view rel,
                     const std::vector<FunctionInfo>& funcs, std::vector<Finding>& out) {
    cxx_token_unless(lines, rel, funcs, out, _CXX_QUICK_EXIT, R"(\bat_quick_exit\s*\()",
                     "CXX-QUICK-EXIT", "std::quick_exit without a prior at_quick_exit");
}

// to_array result OOB-indexed, or source mutated while used (CWE-125/416).
void _cxx_to_array(const std::vector<std::string>& lines, std::string_view rel,
                   const std::vector<FunctionInfo>& funcs, std::vector<Finding>& out) {
    for (auto& fn : funcs) {
        if (!_CXX_TO_ARRAY.search(fn.body)) continue;
        auto sizes = c_array_sizes(fn.body);
        OrderedBinds binds;
        for (auto& m : _TO_ARRAY_BIND.finditer(fn.body)) bind_set(binds, m.named("name"), m.named("src"));
        std::set<std::string> srcs;
        for (auto& [k, v] : binds) srcs.insert(v);
        auto start = fn.span.first;
        auto bl = split_lines(fn.body);
        std::set<std::string> mutated;
        for (int i = 0; i < static_cast<int>(bl.size()); ++i) {
            auto& ln = bl[static_cast<std::size_t>(i)];
            for (auto& src : srcs) {
                auto s = re_escape(src);
                if (rx_search("\\b" + s + "\\s*\\[", ln) && rx_search("\\b" + s + "\\s*\\[[^\\]]+\\]\\s*=", ln))
                    mutated.insert(src);
                else if (rx_search("\\b" + s + "\\s*=(?!=)", ln) && !_TO_ARRAY_BIND.search(ln))
                    mutated.insert(src);
            }
            auto inl = _TO_ARRAY_INLINE_IDX.search_match(ln);
            if (inl && toarr_idx_bad(strip(inl->named("idx")), std::nullopt, fn.body)) {
                report(out, rel, fn.name, start + i, "CXX-TO-ARRAY",
                       "to_array result indexed without a size guard", lines);
                return;
            }
            for (auto& m : _CXX_SUBSCRIPT.finditer(ln)) {
                auto name = m.named("cont");
                auto src = bind_get(binds, name);
                if (!src) continue;
                auto idx = strip(m.named("idx"));
                if (!mutated.count(*src) && !toarr_idx_bad(idx, size_of(sizes, *src), fn.body)) continue;
                report(out, rel, fn.name, start + i, "CXX-TO-ARRAY",
                       name + " from to_array indexed without size or after the source was mutated", lines);
                return;
            }
        }
    }
}

void _cxx_zoned_time(const std::vector<std::string>& lines, std::string_view rel,
                     const std::vector<FunctionInfo>& funcs, std::vector<Finding>& out) {
    for (auto& fn : funcs) {
        if (!_CXX_ZONED_TIME.search(fn.body)) continue;
        if (_CXX_ZONE_LOOKUP.search(fn.body)) {
            auto names = names_of(_CXX_TZDB_BIND, fn.body);
            bool ok = !names.empty();
            for (auto& n : names)
                if (!param_if_guard(n, fn.body) && !param_null_tested(n, fn.body)) ok = false;
            if (ok) continue;
            if (names.empty() && re_search(R"(\bif\s*\(\s*!)", fn.body)) continue;
        }
        auto start = fn.span.first;
        auto bl = split_lines(fn.body);
        for (int i = 0; i < static_cast<int>(bl.size()); ++i)
            if (_CXX_ZONED_TIME.search(bl[static_cast<std::size_t>(i)])) {
                report(out, rel, fn.name, start + i, "CXX-ZONED-TIME",
                       "std::chrono::zoned_time constructed without a zone / locate_zone null check",
                       lines);
                return;
            }
    }
}

void _cxx_kill_dependency(const std::vector<std::string>& lines, std::string_view rel,
                          const std::vector<FunctionInfo>& funcs, std::vector<Finding>& out) {
    cxx_index_token_scan(lines, rel, funcs, out, _CXX_KILLDEP, _KILLDEP_IN_INDEX, _KILLDEP_ASSIGN,
                         "CXX-KILL-DEPENDENCY",
                         "std::kill_dependency used as only sync or as an unguarded index");
}

void _cxx_rotl(const std::vector<std::string>& lines, std::string_view rel,
               const std::vector<FunctionInfo>& funcs, std::vector<Finding>& out) {
    cxx_index_token_scan(lines, rel, funcs, out, _CXX_ROTL_TOKEN, _ROTL_IN_INDEX, _ROTL_ASSIGN,
                         "CXX-ROTL", "std::rotl/rotr result used as an array index without a range check");
}

void _cxx_current_exception(const std::vector<std::string>& lines, std::string_view rel,
                            const std::vector<FunctionInfo>& funcs, std::vector<Finding>& out) {
    for (auto& fn : funcs) {
        if (!_CXX_CURRENT_EXC.search(fn.body)) continue;
        if (re_search(R"(\bif\s*\()", fn.body) && re_search(R"(\bnullptr\b)", fn.body)) continue;
        auto start = fn.span.first;
        auto bl = split_lines(fn.body);
        for (int i = 0; i < static_cast<int>(bl.size()); ++i)
            if (_CUR_EXC_INLINE.search(bl[static_cast<std::size_t>(i)]) || _CXX_RETHROW_EXC.search(bl[static_cast<std::size_t>(i)])) {
                report(out, rel, fn.name, start + i, "CXX-CURRENT-EXCEPTION",
                       "std::current_exception() rethrown without a nullptr test", lines);
                return;
            }
    }
}

void _cxx_bit_width(const std::vector<std::string>& lines, std::string_view rel,
                    const std::vector<FunctionInfo>& funcs, std::vector<Finding>& out) {
    cxx_index_token_scan(lines, rel, funcs, out, _CXX_BITWIDTH_TOKEN, _BITWIDTH_IN_INDEX, _BITWIDTH_ASSIGN,
                         "CXX-BIT-WIDTH",
                         "std::bit_width result used as an array index without a range check");
}

void _cxx_lerp(const std::vector<std::string>& lines, std::string_view rel,
               const std::vector<FunctionInfo>& funcs, std::vector<Finding>& out) {
    for (auto& fn : funcs) {
        if (!_CXX_LERP_TOKEN.search(fn.body)) continue;
        std::set<std::string> assigned;
        auto start = fn.span.first;
        auto bl = split_lines(fn.body);
        for (int i = 0; i < static_cast<int>(bl.size()); ++i) {
            for (auto& m : _LERP_ASSIGN.finditer(bl[static_cast<std::size_t>(i)])) assigned.insert(m.named("var"));
            bool hit = _LERP_IN_INDEX.search(bl[static_cast<std::size_t>(i)]);
            if (!hit) {
                for (auto& m : _CXX_SUBSCRIPT.finditer(bl[static_cast<std::size_t>(i)])) {
                    auto idx = strip(m.named("idx"));
                    if (!assigned.count(idx) || byteswap_idx_guarded(idx, fn.body)) continue;
                    hit = true;
                    break;
                }
            }
            if (!hit) continue;
            report(out, rel, fn.name, start + i, "CXX-LERP",
                   "std::lerp result used as an array index without a range check", lines);
            return;
        }
        if (_CXX_LERP_FINITE.search(fn.body)) continue;
        bool any_g = false;
        for (auto& v : assigned)
            if (byteswap_idx_guarded(v, fn.body)) any_g = true;
        if (any_g) continue;
        for (int i = 0; i < static_cast<int>(bl.size()); ++i)
            if (_CXX_LERP_TOKEN.search(bl[static_cast<std::size_t>(i)])) {
                report(out, rel, fn.name, start + i, "CXX-LERP", "std::lerp with no finite bounds", lines);
                return;
            }
    }
}

void _cxx_midpoint(const std::vector<std::string>& lines, std::string_view rel,
                   const std::vector<FunctionInfo>& funcs, std::vector<Finding>& out) {
    cxx_index_token_scan(lines, rel, funcs, out, _CXX_MIDPOINT_TOKEN, _MIDPOINT_IN_INDEX, _MIDPOINT_ASSIGN,
                         "CXX-MIDPOINT",
                         "std::midpoint result used as an array index without a range check");
}

void _cxx_cmp_less(const std::vector<std::string>& lines, std::string_view rel,
                   const std::vector<FunctionInfo>& funcs, std::vector<Finding>& out) {
    cxx_index_token_scan(lines, rel, funcs, out, _CXX_CMPLESS_TOKEN, _CMPLESS_IN_INDEX, _CMPLESS_ASSIGN,
                         "CXX-CMP-LESS",
                         "std::cmp_less/in_range result used as an array index without a range check");
}

void _cxx_countl_zero(const std::vector<std::string>& lines, std::string_view rel,
                      const std::vector<FunctionInfo>& funcs, std::vector<Finding>& out) {
    cxx_index_token_scan(lines, rel, funcs, out, _CXX_COUNTL_TOKEN, _COUNTL_IN_INDEX, _COUNTL_ASSIGN,
                         "CXX-COUNTL-ZERO",
                         "std::countl_zero/countr result used as an array index without a range check");
}

void _cxx_unreachable(const std::vector<std::string>& lines, std::string_view rel,
                      const std::vector<FunctionInfo>& funcs, std::vector<Finding>& out) {
    for (auto& fn : funcs) {
        if (!_CXX_STD_UNREACHABLE.search(fn.body)) continue;
        if (!re_search(R"(\breturn\b)", fn.body)) continue;
        cxx_first_token(lines, rel, funcs, out, _CXX_STD_UNREACHABLE, "CXX-UNREACHABLE",
                        "std::unreachable() used as a recoverable path");
        break;
    }
}

void _cxx_gcd(const std::vector<std::string>& lines, std::string_view rel,
              const std::vector<FunctionInfo>& funcs, std::vector<Finding>& out) {
    cxx_index_token_scan(lines, rel, funcs, out, _CXX_GCD_TOKEN, _GCD_IN_INDEX, _GCD_ASSIGN,
                         "CXX-GCD", "std::gcd result used as an array index without a range check");
}
void _cxx_lcm(const std::vector<std::string>& lines, std::string_view rel,
              const std::vector<FunctionInfo>& funcs, std::vector<Finding>& out) {
    cxx_index_token_scan(lines, rel, funcs, out, _CXX_LCM_TOKEN, _LCM_IN_INDEX, _LCM_ASSIGN,
                         "CXX-LCM", "std::lcm result used as an array index without a range check");
}
void _cxx_clamp(const std::vector<std::string>& lines, std::string_view rel,
                const std::vector<FunctionInfo>& funcs, std::vector<Finding>& out) {
    cxx_index_token_scan(lines, rel, funcs, out, _CXX_CLAMP_TOKEN, _CLAMP_IN_INDEX, _CLAMP_ASSIGN,
                         "CXX-CLAMP", "std::clamp result used as an array index without a range check");
}
// std::exchange result or object used as an unguarded index (CWE-672/125).
void _cxx_exchange(const std::vector<std::string>& lines, std::string_view rel,
                   const std::vector<FunctionInfo>& funcs, std::vector<Finding>& out) {
    for (auto& fn : funcs) {
        if (!_CXX_EXCHANGE_TOKEN.search(fn.body)) continue;
        std::set<std::string> assigned, exchanged;
        auto start = fn.span.first;
        auto bl = split_lines(fn.body);
        for (int i = 0; i < static_cast<int>(bl.size()); ++i) {
            auto& ln = bl[static_cast<std::size_t>(i)];
            for (auto& m : _EXCHANGE_ASSIGN.finditer(ln)) {
                assigned.insert(m.named("var"));
                exchanged.insert(m.named("obj"));
            }
            for (auto& m : _EXCHANGE_CALL.finditer(ln)) exchanged.insert(m.named("obj"));
            if (_EXCHANGE_IN_INDEX.search(ln)) {
                report(out, rel, fn.name, start + i, "CXX-EXCHANGE",
                       "std::exchange result used as an array index without a range check", lines);
                return;
            }
            for (auto& m : _CXX_SUBSCRIPT.finditer(ln)) {
                auto idx = strip(m.named("idx"));
                if (!assigned.count(idx) && !exchanged.count(idx)) continue;
                if (byteswap_idx_guarded(idx, fn.body)) continue;
                report(out, rel, fn.name, start + i, "CXX-EXCHANGE",
                       m.named("cont") + "[" + idx + "] indexes with std::exchange without a range check",
                       lines);
                return;
            }
        }
    }
}
void _cxx_transform_reduce(const std::vector<std::string>& lines, std::string_view rel,
                           const std::vector<FunctionInfo>& funcs, std::vector<Finding>& out) {
    cxx_index_token_scan(lines, rel, funcs, out, _CXX_TRED_TOKEN, _TRED_IN_INDEX, _TRED_ASSIGN,
                         "CXX-TRANSFORM-REDUCE",
                         "std::transform_reduce result used as an array index without a range check");
}
void _cxx_reduce(const std::vector<std::string>& lines, std::string_view rel,
                 const std::vector<FunctionInfo>& funcs, std::vector<Finding>& out) {
    cxx_index_token_scan(lines, rel, funcs, out, _CXX_REDUCE_TOKEN, _REDUCE_IN_INDEX, _REDUCE_ASSIGN,
                         "CXX-REDUCE", "std::reduce result used as an array index without a range check");
}
void _cxx_forward_like(const std::vector<std::string>& lines, std::string_view rel,
                       const std::vector<FunctionInfo>& funcs, std::vector<Finding>& out) {
    cxx_index_token_scan(lines, rel, funcs, out, _CXX_FWDLIKE_TOKEN, _FWDLIKE_IN_INDEX, _FWDLIKE_ASSIGN,
                         "CXX-FORWARD-LIKE",
                         "std::forward_like result used as an array index without a range check");
}
void _cxx_add_sat(const std::vector<std::string>& lines, std::string_view rel,
                  const std::vector<FunctionInfo>& funcs, std::vector<Finding>& out) {
    cxx_index_token_scan(lines, rel, funcs, out, _CXX_ADDSAT_TOKEN, _ADDSAT_IN_INDEX, _ADDSAT_ASSIGN,
                         "CXX-ADD-SAT", "std::add_sat result used as an array index without a range check");
}

// to_address result used after source reset, or indexed OOB (CWE-416/125).
void _cxx_to_address(const std::vector<std::string>& lines, std::string_view rel,
                     const std::vector<FunctionInfo>& funcs, std::vector<Finding>& out) {
    for (auto& fn : funcs) {
        if (!_CXX_TO_ADDRESS.search(fn.body)) continue;
        auto sizes = c_array_sizes(fn.body);
        OrderedBinds binds;
        for (auto& m : _TO_ADDR_BIND.finditer(fn.body)) bind_set(binds, m.named("name"), m.named("src"));
        std::set<std::string> srcs;
        for (auto& [k, v] : binds) srcs.insert(v);
        auto start = fn.span.first;
        auto bl = split_lines(fn.body);
        std::set<std::string> reset_srcs;
        for (int i = 0; i < static_cast<int>(bl.size()); ++i) {
            auto& ln = bl[static_cast<std::size_t>(i)];
            for (auto& src : srcs)
                if (rx_search("\\b" + re_escape(src) + "\\s*\\.\\s*(?:reset|release)\\s*\\(", ln))
                    reset_srcs.insert(src);
            auto inl = _TO_ADDR_INLINE_IDX.search_match(ln);
            if (inl && toarr_idx_bad(strip(inl->named("idx")), std::nullopt, fn.body)) {
                report(out, rel, fn.name, start + i, "CXX-TO-ADDRESS",
                       "to_address result indexed without a bound", lines);
                return;
            }
            for (auto& m : _CXX_SUBSCRIPT.finditer(ln)) {
                auto name = m.named("cont");
                auto src = bind_get(binds, name);
                if (!src) continue;
                auto idx = strip(m.named("idx"));
                if (!reset_srcs.count(*src) && !toarr_idx_bad(idx, size_of(sizes, *src), fn.body)) continue;
                report(out, rel, fn.name, start + i, "CXX-TO-ADDRESS",
                       name + " from to_address indexed without a bound or after the source was reset", lines);
                return;
            }
            for (auto& [raw, src] : binds) {
                if (!reset_srcs.count(src)) continue;
                auto r = re_escape(raw);
                if (!rx_search("(?:\\*\\s*" + r + "\\b|" + r + "\\s*->)", ln)) continue;
                report(out, rel, fn.name, start + i, "CXX-TO-ADDRESS",
                       raw + " from to_address used after " + src + " was reset or released", lines);
                return;
            }
        }
    }
}
// is_constant_evaluated false-path indexes without a bound (CWE-125).
void _cxx_is_constant_evaluated(const std::vector<std::string>& lines, std::string_view rel,
                                const std::vector<FunctionInfo>& funcs, std::vector<Finding>& out) {
    for (auto& fn : funcs) {
        if (!_CXX_ICE_TOKEN.search(fn.body)) continue;
        auto start = fn.span.first;
        auto bl = split_lines(fn.body);
        for (int i = 0; i < static_cast<int>(bl.size()); ++i) {
            for (auto& m : _CXX_SUBSCRIPT.finditer(bl[static_cast<std::size_t>(i)])) {
                auto idx = strip(m.named("idx"));
                if (byteswap_idx_guarded(idx, fn.body)) continue;
                bool digits = !idx.empty();
                for (unsigned char ch : idx)
                    if (!std::isdigit(ch)) digits = false;
                if (digits && idx.size() < 10 && std::stoi(idx) < 4) continue;
                report(out, rel, fn.name, start + i, "CXX-IS-CONSTANT-EVALUATED",
                       "is_constant_evaluated() runtime path indexes without a bound", lines);
                return;
            }
        }
    }
}
void _cxx_addressof(const std::vector<std::string>& lines, std::string_view rel,
                    const std::vector<FunctionInfo>& funcs, std::vector<Finding>& out) {
    cxx_token_subscript_unguarded(lines, rel, funcs, out, _CXX_ADDRESSOF, "CXX-ADDRESSOF",
                                  "std::addressof result used as an array index without a bound");
}
// assume_aligned result indexed without a bound (CWE-125).
void _cxx_assume_aligned(const std::vector<std::string>& lines, std::string_view rel,
                         const std::vector<FunctionInfo>& funcs, std::vector<Finding>& out) {
    for (auto& fn : funcs) {
        if (!_CXX_ASSUME_ALIGNED.search(fn.body)) continue;
        auto binds = names_of(_ASMALIGN_BIND, fn.body);
        auto start = fn.span.first;
        auto bl = split_lines(fn.body);
        for (int i = 0; i < static_cast<int>(bl.size()); ++i) {
            auto& ln = bl[static_cast<std::size_t>(i)];
            auto inl = _ASMALIGN_INLINE_IDX.search_match(ln);
            if (inl && toarr_idx_bad(strip(inl->named("idx")), std::nullopt, fn.body)) {
                report(out, rel, fn.name, start + i, "CXX-ASSUME-ALIGNED",
                       "assume_aligned result indexed without a bound", lines);
                return;
            }
            for (auto& m : _CXX_SUBSCRIPT.finditer(ln)) {
                if (!binds.count(m.named("cont"))) continue;
                if (!toarr_idx_bad(strip(m.named("idx")), std::nullopt, fn.body)) continue;
                report(out, rel, fn.name, start + i, "CXX-ASSUME-ALIGNED",
                       "assume_aligned result indexed without a bound", lines);
                return;
            }
        }
    }
}
void _cxx_as_const(const std::vector<std::string>& lines, std::string_view rel,
                   const std::vector<FunctionInfo>& funcs, std::vector<Finding>& out) {
    for (auto& fn : funcs) {
        if (!_CXX_AS_CONST.search(fn.body)) continue;
        auto start = fn.span.first;
        auto bl = split_lines(fn.body);
        for (int i = 0; i < static_cast<int>(bl.size()); ++i)
            if (_AS_CONST_CAST_WRITE.search(bl[static_cast<std::size_t>(i)])) {
                report(out, rel, fn.name, start + i, "CXX-AS-CONST",
                       "const_cast write through std::as_const view", lines);
                return;
            }
    }
}

// exclusive_scan dest OOB-indexed or overlapping source (CWE-125).
void _cxx_exclusive_scan(const std::vector<std::string>& lines, std::string_view rel,
                         const std::vector<FunctionInfo>& funcs, std::vector<Finding>& out) {
    for (auto& fn : funcs) {
        if (!_CXX_EXCLUSIVE_SCAN.search(fn.body)) continue;
        std::set<std::string> dests, overlap;
        for (auto& m : _EXSCAN_CALL.finditer(fn.body)) {
            dests.insert(m.named("dst"));
            if (m.named("src") == m.named("dst")) overlap.insert(m.named("dst"));
        }
        auto start = fn.span.first;
        auto bl = split_lines(fn.body);
        for (int i = 0; i < static_cast<int>(bl.size()); ++i) {
            auto& ln = bl[static_cast<std::size_t>(i)];
            if (_CXX_EXCLUSIVE_SCAN.search(ln) && !overlap.empty()) {
                report(out, rel, fn.name, start + i, "CXX-EXCLUSIVE-SCAN",
                       "exclusive_scan dest overlaps source", lines);
                return;
            }
            for (auto& m : _CXX_SUBSCRIPT.finditer(ln)) {
                if (!dests.count(m.named("cont"))) continue;
                if (!toarr_idx_bad(strip(m.named("idx")), std::nullopt, fn.body)) continue;
                report(out, rel, fn.name, start + i, "CXX-EXCLUSIVE-SCAN",
                       "exclusive_scan output indexed without a size guard", lines);
                return;
            }
        }
    }
}
// inclusive_scan dest OOB-indexed or overlapping source (CWE-125).
void _cxx_inclusive_scan(const std::vector<std::string>& lines, std::string_view rel,
                         const std::vector<FunctionInfo>& funcs, std::vector<Finding>& out) {
    for (auto& fn : funcs) {
        if (!_CXX_INCLUSIVE_SCAN.search(fn.body)) continue;
        std::set<std::string> dests, overlap;
        for (auto& m : _INSCAN_CALL.finditer(fn.body)) {
            dests.insert(m.named("dst"));
            if (m.named("src") == m.named("dst")) overlap.insert(m.named("dst"));
        }
        auto start = fn.span.first;
        auto bl = split_lines(fn.body);
        for (int i = 0; i < static_cast<int>(bl.size()); ++i) {
            auto& ln = bl[static_cast<std::size_t>(i)];
            if (_CXX_INCLUSIVE_SCAN.search(ln) && !overlap.empty()) {
                report(out, rel, fn.name, start + i, "CXX-INCLUSIVE-SCAN",
                       "inclusive_scan dest overlaps source", lines);
                return;
            }
            for (auto& m : _CXX_SUBSCRIPT.finditer(ln)) {
                if (!dests.count(m.named("cont"))) continue;
                if (!toarr_idx_bad(strip(m.named("idx")), std::nullopt, fn.body)) continue;
                report(out, rel, fn.name, start + i, "CXX-INCLUSIVE-SCAN",
                       "inclusive_scan output indexed without a size guard", lines);
                return;
            }
        }
    }
}
// uninitialized_copy/move dest OOB-indexed or overlapping (CWE-125/824).
void _cxx_uninitialized_copy(const std::vector<std::string>& lines, std::string_view rel,
                             const std::vector<FunctionInfo>& funcs, std::vector<Finding>& out) {
    for (auto& fn : funcs) {
        if (!_CXX_UNINITIALIZED_COPY.search(fn.body)) continue;
        std::set<std::string> dests, overlap;
        for (auto& m : _UICOPY_CALL.finditer(fn.body)) {
            dests.insert(m.named("dst"));
            if (m.named("src") == m.named("dst")) overlap.insert(m.named("dst"));
        }
        auto start = fn.span.first;
        auto bl = split_lines(fn.body);
        for (int i = 0; i < static_cast<int>(bl.size()); ++i) {
            auto& ln = bl[static_cast<std::size_t>(i)];
            if (_CXX_UNINITIALIZED_COPY.search(ln) && !overlap.empty()) {
                report(out, rel, fn.name, start + i, "CXX-UNINITIALIZED-COPY",
                       "uninitialized_copy/move dest overlaps source", lines);
                return;
            }
            for (auto& m : _CXX_SUBSCRIPT.finditer(ln)) {
                if (!dests.count(m.named("cont"))) continue;
                if (!toarr_idx_bad(strip(m.named("idx")), std::nullopt, fn.body)) continue;
                report(out, rel, fn.name, start + i, "CXX-UNINITIALIZED-COPY",
                       "uninitialized_copy/move dest indexed without a size guard", lines);
                return;
            }
        }
    }
}
// transform_*_scan dest indexed without a size guard (CWE-125).
void _cxx_transform_scan(const std::vector<std::string>& lines, std::string_view rel,
                         const std::vector<FunctionInfo>& funcs, std::vector<Finding>& out) {
    for (auto& fn : funcs) {
        if (!_CXX_TRANSFORM_SCAN.search(fn.body)) continue;
        std::set<std::string> dests;
        for (auto& m : _TSCAN_CALL.finditer(fn.body)) dests.insert(m.named("dst"));
        auto start = fn.span.first;
        auto bl = split_lines(fn.body);
        for (int i = 0; i < static_cast<int>(bl.size()); ++i) {
            for (auto& m : _CXX_SUBSCRIPT.finditer(bl[static_cast<std::size_t>(i)])) {
                if (!dests.count(m.named("cont"))) continue;
                if (!toarr_idx_bad(strip(m.named("idx")), std::nullopt, fn.body)) continue;
                report(out, rel, fn.name, start + i, "CXX-TRANSFORM-SCAN",
                       "transform_inclusive_scan/transform_exclusive_scan output indexed without a size guard",
                       lines);
                return;
            }
        }
    }
}

// make_exception_ptr rethrown without a nullptr test (CWE-476).
void _cxx_make_exception_ptr(const std::vector<std::string>& lines, std::string_view rel,
                             const std::vector<FunctionInfo>& funcs, std::vector<Finding>& out) {
    for (auto& fn : funcs) {
        if (!_CXX_MAKE_EPTR.search(fn.body)) continue;
        if (!_CXX_RETHROW_EXC.search(fn.body)) continue;
        auto bound = names_of(_MAKE_EPTR_BIND, fn.body);
        auto start = fn.span.first;
        auto bl = split_lines(fn.body);
        for (int i = 0; i < static_cast<int>(bl.size()); ++i) {
            auto& ln = bl[static_cast<std::size_t>(i)];
            if (_MAKE_EPTR_INLINE.search(ln)) {
                report(out, rel, fn.name, start + i, "CXX-MAKE-EXCEPTION-PTR",
                       "std::make_exception_ptr() rethrown without a nullptr test", lines);
                return;
            }
            for (auto& name : bound) {
                if (param_if_guard(name, fn.body) || param_null_tested(name, fn.body)) continue;
                if (!rx_search("(?:std\\s*::\\s*)?rethrow_exception\\s*\\(\\s*" + re_escape(name) + "\\b", ln))
                    continue;
                report(out, rel, fn.name, start + i, "CXX-MAKE-EXCEPTION-PTR",
                       "rethrow_exception(" + name + ") from make_exception_ptr without a nullptr test",
                       lines);
                return;
            }
        }
    }
}
void _cxx_set_terminate(const std::vector<std::string>& lines, std::string_view rel,
                        const std::vector<FunctionInfo>& funcs, std::vector<Finding>& out) {
    cxx_token_unless(lines, rel, funcs, out, _CXX_SET_TERMINATE, R"(\bget_terminate\s*\()",
                     "CXX-SET-TERMINATE",
                     "set_terminate/get_terminate without storing the previous handler");
}

// destroy_at then use, or construct_at dest indexed OOB (CWE-416/125).
void _cxx_construct_at(const std::vector<std::string>& lines, std::string_view rel,
                       const std::vector<FunctionInfo>& funcs, std::vector<Finding>& out) {
    for (auto& fn : funcs) {
        if (!_CXX_CONSTRUCT_AT.search(fn.body)) continue;
        std::set<std::string> dests, destroyed;
        auto start = fn.span.first;
        auto bl = split_lines(fn.body);
        for (int i = 0; i < static_cast<int>(bl.size()); ++i) {
            auto& ln = bl[static_cast<std::size_t>(i)];
            for (auto& m : _CONSTRUCT_AT_CALL.finditer(ln)) {
                dests.insert(m.named("ptr"));
                destroyed.erase(m.named("ptr"));
            }
            for (auto& m : _DESTROY_AT_CALL.finditer(ln)) destroyed.insert(m.named("ptr"));
            if (!_DESTROY_AT_CALL.search(ln)) {
                for (auto& ptr : destroyed) {
                    auto v = re_escape(ptr);
                    if (!rx_search("(?:\\*\\s*" + v + "\\b|" + v + "\\s*\\[|" + v + "\\s*->)", ln)) continue;
                    report(out, rel, fn.name, start + i, "CXX-CONSTRUCT-AT",
                           ptr + " used after destroy_at", lines);
                    return;
                }
            }
            for (auto& m : _CXX_SUBSCRIPT.finditer(ln)) {
                if (!dests.count(m.named("cont"))) continue;
                if (!toarr_idx_bad(strip(m.named("idx")), std::nullopt, fn.body)) continue;
                report(out, rel, fn.name, start + i, "CXX-CONSTRUCT-AT",
                       "construct_at result indexed without a size guard", lines);
                return;
            }
        }
    }
}

void _cxx_uninitialized_fill(const std::vector<std::string>& lines, std::string_view rel,
                             const std::vector<FunctionInfo>& funcs, std::vector<Finding>& out) {
    for (auto& fn : funcs) {
        if (!_CXX_UNINITIALIZED_FILL.search(fn.body)) continue;
        if (re_search(R"(\.size\s*\()", fn.body)) continue;
        auto start = fn.span.first;
        auto bl = split_lines(fn.body);
        for (int i = 0; i < static_cast<int>(bl.size()); ++i)
            if (_CXX_SUBSCRIPT.search(bl[static_cast<std::size_t>(i)])) {
                report(out, rel, fn.name, start + i, "CXX-UNINITIALIZED-FILL",
                       "uninitialized_fill dest indexed without a size guard", lines);
                return;
            }
    }
}

// destroy_n then use, or count is unchecked (CWE-416).
void _cxx_destroy_n(const std::vector<std::string>& lines, std::string_view rel,
                    const std::vector<FunctionInfo>& funcs, std::vector<Finding>& out) {
    for (auto& fn : funcs) {
        if (!_CXX_DESTROY_N.search(fn.body)) continue;
        std::set<std::string> destroyed;
        auto start = fn.span.first;
        auto bl = split_lines(fn.body);
        for (int i = 0; i < static_cast<int>(bl.size()); ++i) {
            auto& ln = bl[static_cast<std::size_t>(i)];
            for (auto& m : _DESTROY_N_CALL.finditer(ln)) {
                auto n = strip(m.named("n"));
                destroyed.insert(m.named("ptr"));
                if (!all_digits(n) && !byteswap_idx_guarded(n, fn.body)) {
                    report(out, rel, fn.name, start + i, "CXX-DESTROY-N",
                           "destroy_n count is unchecked", lines);
                    return;
                }
            }
            if (!_DESTROY_N_CALL.search(ln)) {
                for (auto& ptr : destroyed) {
                    if (!ptr_deref_on(ptr, ln)) continue;
                    report(out, rel, fn.name, start + i, "CXX-DESTROY-N",
                           ptr + " used after destroy_n", lines);
                    return;
                }
            }
        }
    }
}

// type_identity recast pointer indexed OOB (CWE-125).
void _cxx_type_identity(const std::vector<std::string>& lines, std::string_view rel,
                        const std::vector<FunctionInfo>& funcs, std::vector<Finding>& out) {
    for (auto& fn : funcs) {
        if (!_CXX_TYPE_IDENTITY.search(fn.body)) continue;
        std::set<std::string> dests;
        for (auto& m : _TYPEIDENT_DIRECT_PTR.finditer(fn.body)) dests.insert(m.named("ptr"));
        std::set<std::string> aliases;
        for (auto& m : _TYPEIDENT_ALIAS.finditer(fn.body)) aliases.insert(m.named("alias"));
        if (!aliases.empty()) {
            std::string al;
            for (auto& a : aliases) al += (al.empty() ? "" : "|") + re_escape(a);
            Regex alias_ptr("\\b(?:" + al + ")\\s*\\*\\s*(?P<ptr>[A-Za-z_]\\w*)");
            for (auto& m : alias_ptr.finditer(fn.body)) dests.insert(m.named("ptr"));
        }
        if (dests.empty()) continue;
        auto start = fn.span.first;
        auto bl = split_lines(fn.body);
        for (int i = 0; i < static_cast<int>(bl.size()); ++i) {
            for (auto& m : _CXX_SUBSCRIPT.finditer(bl[static_cast<std::size_t>(i)])) {
                if (!dests.count(m.named("cont"))) continue;
                if (!toarr_idx_bad(strip(m.named("idx")), std::nullopt, fn.body)) continue;
                report(out, rel, fn.name, start + i, "CXX-TYPE-IDENTITY",
                       "type_identity recast pointer indexed without a size guard", lines);
                return;
            }
        }
    }
}
void _cxx_nontype(const std::vector<std::string>& lines, std::string_view rel,
                  const std::vector<FunctionInfo>& funcs, std::vector<Finding>& out) {
    cxx_token_subscript_unguarded(lines, rel, funcs, out, _CXX_NONTYPE, "CXX-NONTYPE",
                                  "std::nontype used as an unguarded index");
}
void _cxx_layout_compatible(const std::vector<std::string>& lines, std::string_view rel,
                            const std::vector<FunctionInfo>& funcs, std::vector<Finding>& out) {
    cxx_token_subscript_unguarded(lines, rel, funcs, out, _CXX_LAYOUT_COMPATIBLE, "CXX-LAYOUT-COMPATIBLE",
                                  "is_layout_compatible recast pointer indexed without a size guard");
}
void _cxx_ptr_interconvertible(const std::vector<std::string>& lines, std::string_view rel,
                               const std::vector<FunctionInfo>& funcs, std::vector<Finding>& out) {
    cxx_token_subscript_unguarded(lines, rel, funcs, out, _CXX_PTR_INTERCONV, "CXX-PTR-INTERCONVERTIBLE",
                                  "pointer-interconvertible recast pointer indexed without a size guard");
}
// uninitialized_value_construct dest indexed OOB (CWE-125).
void _cxx_uninitialized_value(const std::vector<std::string>& lines, std::string_view rel,
                              const std::vector<FunctionInfo>& funcs, std::vector<Finding>& out) {
    for (auto& fn : funcs) {
        if (!_CXX_UNINITIALIZED_VALUE.search(fn.body)) continue;
        std::set<std::string> dests;
        for (auto& m : _UVALUE_CALL.finditer(fn.body)) dests.insert(m.named("dst"));
        auto start = fn.span.first;
        auto bl = split_lines(fn.body);
        for (int i = 0; i < static_cast<int>(bl.size()); ++i) {
            for (auto& m : _CXX_SUBSCRIPT.finditer(bl[static_cast<std::size_t>(i)])) {
                if (!dests.count(m.named("cont"))) continue;
                if (!toarr_idx_bad(strip(m.named("idx")), std::nullopt, fn.body)) continue;
                report(out, rel, fn.name, start + i, "CXX-UNINITIALIZED-VALUE",
                       "uninitialized_value_construct dest indexed without a size guard", lines);
                return;
            }
        }
    }
}
void _cxx_const_iterator(const std::vector<std::string>& lines, std::string_view rel,
                         const std::vector<FunctionInfo>& funcs, std::vector<Finding>& out) {
    cxx_token_subscript_unguarded(lines, rel, funcs, out, _CXX_BASIC_CONST_ITER, "CXX-CONST-ITERATOR",
                                  "basic_const_iterator indexed without a size guard");
}
void _cxx_corresponding_member(const std::vector<std::string>& lines, std::string_view rel,
                               const std::vector<FunctionInfo>& funcs, std::vector<Finding>& out) {
    cxx_token_subscript_unguarded(lines, rel, funcs, out, _CXX_CORR_MEMBER, "CXX-CORRESPONDING-MEMBER",
                                  "is_corresponding_member recast pointer indexed without a size guard");
}
void _cxx_ranges_to(const std::vector<std::string>& lines, std::string_view rel,
                    const std::vector<FunctionInfo>& funcs, std::vector<Finding>& out) {
    cxx_token_subscript_unguarded(lines, rel, funcs, out, _CXX_RANGES_TO, "CXX-RANGES-TO",
                                  "ranges::to container indexed without a size guard");
}

void _cxx_enumerate(const std::vector<std::string>& lines, std::string_view rel,
                    const std::vector<FunctionInfo>& funcs, std::vector<Finding>& out) {
    cxx_token_subscript_unguarded(lines, rel, funcs, out, _CXX_ENUMERATE, "CXX-ENUMERATE",
                                  "enumerate view indexed without a size guard");
}
void _cxx_cartesian_product(const std::vector<std::string>& lines, std::string_view rel,
                            const std::vector<FunctionInfo>& funcs, std::vector<Finding>& out) {
    cxx_token_subscript_unguarded(lines, rel, funcs, out, _CXX_CARTESIAN, "CXX-CARTESIAN-PRODUCT",
                                  "cartesian_product view indexed without a size guard");
}
void _cxx_chunk(const std::vector<std::string>& lines, std::string_view rel,
                const std::vector<FunctionInfo>& funcs, std::vector<Finding>& out) {
    cxx_token_subscript_unguarded(lines, rel, funcs, out, _CXX_CHUNK, "CXX-CHUNK",
                                  "chunk view indexed without a size guard");
}
void _cxx_slide(const std::vector<std::string>& lines, std::string_view rel,
                const std::vector<FunctionInfo>& funcs, std::vector<Finding>& out) {
    cxx_token_subscript_unguarded(lines, rel, funcs, out, _CXX_SLIDE, "CXX-SLIDE",
                                  "slide view indexed without a size guard");
}
void _cxx_adjacent(const std::vector<std::string>& lines, std::string_view rel,
                   const std::vector<FunctionInfo>& funcs, std::vector<Finding>& out) {
    cxx_token_subscript_unguarded(lines, rel, funcs, out, _CXX_ADJACENT, "CXX-ADJACENT",
                                  "adjacent view indexed without a size guard");
}
void _cxx_join_with(const std::vector<std::string>& lines, std::string_view rel,
                    const std::vector<FunctionInfo>& funcs, std::vector<Finding>& out) {
    cxx_token_subscript_unguarded(lines, rel, funcs, out, _CXX_JOIN_WITH, "CXX-JOIN-WITH",
                                  "join_with view indexed without a size guard");
}
void _cxx_zip_transform(const std::vector<std::string>& lines, std::string_view rel,
                        const std::vector<FunctionInfo>& funcs, std::vector<Finding>& out) {
    cxx_token_subscript_unguarded(lines, rel, funcs, out, _CXX_ZIP_TRANSFORM, "CXX-ZIP-TRANSFORM",
                                  "zip_transform view indexed without a size guard");
}
void _cxx_as_rvalue(const std::vector<std::string>& lines, std::string_view rel,
                    const std::vector<FunctionInfo>& funcs, std::vector<Finding>& out) {
    cxx_token_subscript_unguarded(lines, rel, funcs, out, _CXX_AS_RVALUE, "CXX-AS-RVALUE",
                                  "as_rvalue view indexed without a size guard");
}
void _cxx_from_range(const std::vector<std::string>& lines, std::string_view rel,
                     const std::vector<FunctionInfo>& funcs, std::vector<Finding>& out) {
    cxx_token_subscript_unguarded(lines, rel, funcs, out, _CXX_FROM_RANGE, "CXX-FROM-RANGE",
                                  "from_range container indexed without a size guard");
}
void _cxx_scoped_enum(const std::vector<std::string>& lines, std::string_view rel,
                      const std::vector<FunctionInfo>& funcs, std::vector<Finding>& out) {
    cxx_token_subscript_unguarded(lines, rel, funcs, out, _CXX_SCOPED_ENUM, "CXX-SCOPED-ENUM",
                                  "is_scoped_enum recast indexed without a size guard");
}
void _cxx_stride(const std::vector<std::string>& lines, std::string_view rel,
                 const std::vector<FunctionInfo>& funcs, std::vector<Finding>& out) {
    cxx_token_subscript_unguarded(lines, rel, funcs, out, _CXX_STRIDE, "CXX-STRIDE",
                                  "stride view indexed without a size guard");
}
void _cxx_repeat(const std::vector<std::string>& lines, std::string_view rel,
                 const std::vector<FunctionInfo>& funcs, std::vector<Finding>& out) {
    cxx_token_subscript_unguarded(lines, rel, funcs, out, _CXX_REPEAT, "CXX-REPEAT",
                                  "repeat view indexed without a size guard");
}
void _cxx_take(const std::vector<std::string>& lines, std::string_view rel,
               const std::vector<FunctionInfo>& funcs, std::vector<Finding>& out) {
    cxx_token_subscript_unguarded(lines, rel, funcs, out, _CXX_TAKE, "CXX-TAKE",
                                  "take view indexed without a size guard");
}
void _cxx_drop(const std::vector<std::string>& lines, std::string_view rel,
               const std::vector<FunctionInfo>& funcs, std::vector<Finding>& out) {
    cxx_token_subscript_unguarded(lines, rel, funcs, out, _CXX_DROP, "CXX-DROP",
                                  "drop view indexed without a size guard");
}
void _cxx_filter(const std::vector<std::string>& lines, std::string_view rel,
                 const std::vector<FunctionInfo>& funcs, std::vector<Finding>& out) {
    cxx_token_subscript_unguarded(lines, rel, funcs, out, _CXX_FILTER, "CXX-FILTER",
                                  "filter view indexed without a size guard");
}
void _cxx_transform_view(const std::vector<std::string>& lines, std::string_view rel,
                         const std::vector<FunctionInfo>& funcs, std::vector<Finding>& out) {
    cxx_token_subscript_unguarded(lines, rel, funcs, out, _CXX_TRANSFORM_VIEW, "CXX-TRANSFORM-VIEW",
                                  "transform view indexed without a size guard");
}
void _cxx_elements(const std::vector<std::string>& lines, std::string_view rel,
                   const std::vector<FunctionInfo>& funcs, std::vector<Finding>& out) {
    cxx_token_subscript_unguarded(lines, rel, funcs, out, _CXX_ELEMENTS, "CXX-ELEMENTS",
                                  "elements view indexed without a size guard");
}
void _cxx_iota(const std::vector<std::string>& lines, std::string_view rel,
               const std::vector<FunctionInfo>& funcs, std::vector<Finding>& out) {
    cxx_token_subscript_unguarded(lines, rel, funcs, out, _CXX_IOTA, "CXX-IOTA",
                                  "iota view indexed without a size guard");
}
void _cxx_take_while(const std::vector<std::string>& lines, std::string_view rel,
                     const std::vector<FunctionInfo>& funcs, std::vector<Finding>& out) {
    cxx_token_subscript_unguarded(lines, rel, funcs, out, _CXX_TAKE_WHILE, "CXX-TAKE-WHILE",
                                  "take_while view indexed without a size guard");
}
void _cxx_drop_while(const std::vector<std::string>& lines, std::string_view rel,
                     const std::vector<FunctionInfo>& funcs, std::vector<Finding>& out) {
    cxx_token_subscript_unguarded(lines, rel, funcs, out, _CXX_DROP_WHILE, "CXX-DROP-WHILE",
                                  "drop_while view indexed without a size guard");
}
void _cxx_keys(const std::vector<std::string>& lines, std::string_view rel,
               const std::vector<FunctionInfo>& funcs, std::vector<Finding>& out) {
    cxx_token_subscript_unguarded(lines, rel, funcs, out, _CXX_KEYS, "CXX-KEYS",
                                  "keys view indexed without a size guard");
}
void _cxx_values(const std::vector<std::string>& lines, std::string_view rel,
                 const std::vector<FunctionInfo>& funcs, std::vector<Finding>& out) {
    cxx_token_subscript_unguarded(lines, rel, funcs, out, _CXX_VALUES, "CXX-VALUES",
                                  "values view indexed without a size guard");
}
void _cxx_reverse_view(const std::vector<std::string>& lines, std::string_view rel,
                       const std::vector<FunctionInfo>& funcs, std::vector<Finding>& out) {
    cxx_token_subscript_unguarded(lines, rel, funcs, out, _CXX_REVERSE_VIEW, "CXX-REVERSE-VIEW",
                                  "reverse view indexed without a size guard");
}
void _cxx_counted(const std::vector<std::string>& lines, std::string_view rel,
                  const std::vector<FunctionInfo>& funcs, std::vector<Finding>& out) {
    cxx_token_subscript_unguarded(lines, rel, funcs, out, _CXX_COUNTED, "CXX-COUNTED",
                                  "counted view indexed without a size guard");
}

}  // namespace

void checkers_cxx(const std::vector<std::string>& lines, std::string_view rel,
                  const std::vector<FunctionInfo>& funcs, std::string_view stripped,
                  std::vector<Finding>& out) {
    _cxx_new_delete(lines, rel, funcs, out);
    _cxx_use_after_move(lines, rel, funcs, out);
    _cxx_self_assign(lines, rel, funcs, out);
    _cxx_dangling_ref(lines, rel, funcs, out);
    _cxx_iter_invalid(lines, rel, funcs, out);
    _cxx_virtual_in_ctor(stripped, lines, rel, funcs, out);
    _cxx_exception_leak(lines, rel, funcs, out);
    _cxx_throw_destructor(stripped, lines, rel, out);
    _cxx_throw_spec(stripped, lines, rel, out);
    _cxx_slicing(stripped, lines, rel, funcs, out);
    _cxx_delete_this(lines, rel, funcs, out);
    _cxx_catch_by_value(lines, rel, funcs, out);
    _cxx_throw_noexcept(lines, rel, funcs, out);
    _cxx_missing_virtual_dtor(stripped, lines, rel, funcs, out);
    _cxx_lambda_dangle(lines, rel, funcs, out);
    _cxx_unique_reset(lines, rel, funcs, out);
    _cxx_const_cast(lines, rel, funcs, out);
    _cxx_dynamic_cast_null(lines, rel, funcs, out);
    _cxx_reinterpret(lines, rel, funcs, out);
    _cxx_throw_copy(lines, rel, funcs, out);
    _cxx_bit_cast(lines, rel, funcs, out);
    _cxx_placement_new(lines, rel, funcs, out);
    _cxx_std_thread(lines, rel, funcs, out);
    _cxx_optional_null(lines, rel, funcs, out);
    _cxx_variant_get(lines, rel, funcs, out);
    _cxx_span_dangle(lines, rel, funcs, out);
    _cxx_vector_index(lines, rel, funcs, out);
    _cxx_catch_all(lines, rel, funcs, out);
    _cxx_throw_new(lines, rel, funcs, out);
    _cxx_uninit_member(stripped, lines, rel, funcs, out);
    _cxx_copy_assign_ptr(lines, rel, funcs, out);
    _cxx_volatile_cast(lines, rel, funcs, out);
    _cxx_shared_get(lines, rel, funcs, out);
    _cxx_auto_ptr(lines, rel, funcs, out);
    _cxx_string_data(lines, rel, funcs, out);
    _cxx_enable_shared(lines, rel, funcs, out);
    _cxx_fwd_ref(lines, rel, funcs, out);
    _cxx_explicit_ctor(stripped, lines, rel, funcs, out);
    _cxx_move_const(lines, rel, funcs, out);
    _cxx_bind_tmp(stripped, lines, rel, funcs, out);
    _cxx_expected_null(lines, rel, funcs, out);
    _cxx_std_format(lines, rel, funcs, out);
    _cxx_this_capture(stripped, lines, rel, funcs, out);
    _cxx_spaceship_default(stripped, lines, rel, funcs, out);
    _cxx_std_async(lines, rel, funcs, out);
    _cxx_future_get(lines, rel, funcs, out);
    _cxx_function_null(lines, rel, funcs, out);
    _cxx_nodiscard(lines, rel, funcs, out);
    _cxx_std_jthread(lines, rel, funcs, out);
    _cxx_mdspan_dangle(lines, rel, funcs, out);
    _cxx_atomic_ref(lines, rel, funcs, out);
    _cxx_condition_wait(lines, rel, funcs, out);
    _cxx_std_bind(lines, rel, funcs, out);
    _cxx_assume(lines, rel, funcs, out);
    _cxx_generator(lines, rel, funcs, out);
    _cxx_shared_mutex(lines, rel, funcs, out);
    _cxx_any_cast(lines, rel, funcs, out);
    _cxx_filesystem(lines, rel, funcs, out);
    _cxx_regex(lines, rel, funcs, out);
    _cxx_latch(lines, rel, funcs, out);
    _cxx_from_chars(lines, rel, funcs, out);
    _cxx_init_list_dangle(lines, rel, funcs, out);
    _cxx_stop_token(lines, rel, funcs, out);
    _cxx_flat_map(lines, rel, funcs, out);
    _cxx_semaphore(lines, rel, funcs, out);
    _cxx_stacktrace(lines, rel, funcs, out);
    _cxx_unique_release(lines, rel, funcs, out);
    _cxx_pack_pragma(lines, rel, funcs, out);
    _cxx_function_ref(lines, rel, funcs, out);
    _cxx_move_only_function(lines, rel, funcs, out);
    _cxx_ranges_dangle(lines, rel, funcs, out);
    _cxx_chrono_seed(lines, rel, funcs, out);
    _cxx_inplace_vector(lines, rel, funcs, out);
    _cxx_flat_set(lines, rel, funcs, out);
    _cxx_copyable_function(lines, rel, funcs, out);
    _cxx_hive(lines, rel, funcs, out);
    _cxx_bitset_index(lines, rel, funcs, out);
    _cxx_sstream_view(lines, rel, funcs, out);
    _cxx_optional_value(lines, rel, funcs, out);
    _cxx_indirect(lines, rel, funcs, out);
    _cxx_to_chars(lines, rel, funcs, out);
    _cxx_hazard_pointer(lines, rel, funcs, out);
    _cxx_text_encoding(lines, rel, funcs, out);
    _cxx_expected_error(lines, rel, funcs, out);
    _cxx_variant_valueless(lines, rel, funcs, out);
    _cxx_simd_index(lines, rel, funcs, out);
    _cxx_rcu(lines, rel, funcs, out);
    _cxx_linalg(lines, rel, funcs, out);
    _cxx_sync_wait(lines, rel, funcs, out);
    _cxx_embed(lines, rel, funcs, out);
    _cxx_contracts(lines, rel, funcs, out);
    _cxx_reflection(lines, rel, funcs, out);
    _cxx_out_ptr(lines, rel, funcs, out);
    _cxx_flat_multimap(lines, rel, funcs, out);
    _cxx_spanstream(lines, rel, funcs, out);
    _cxx_barrier(lines, rel, funcs, out);
    _cxx_task(lines, rel, funcs, out);
    _cxx_generator_discard(lines, rel, funcs, out);
    _cxx_osyncstream(lines, rel, funcs, out);
    _cxx_packaged_task(lines, rel, funcs, out);
    _cxx_flat_multiset(lines, rel, funcs, out);
    _cxx_syncbuf(lines, rel, funcs, out);
    _cxx_counted_iterator(lines, rel, funcs, out);
    _cxx_promise(lines, rel, funcs, out);
    _cxx_weak_ptr(lines, rel, funcs, out);
    _cxx_exception_ptr(lines, rel, funcs, out);
    _cxx_coro_handle(lines, rel, funcs, out);
    _cxx_valarray(lines, rel, funcs, out);
    _cxx_to_underlying(lines, rel, funcs, out);
    _cxx_unexpected(lines, rel, funcs, out);
    _cxx_tuple_get(lines, rel, funcs, out);
    _cxx_deque_index(lines, rel, funcs, out);
    _cxx_forward_list(lines, rel, funcs, out);
    _cxx_list_front(lines, rel, funcs, out);
    _cxx_map_at(lines, rel, funcs, out);
    _cxx_unordered_at(lines, rel, funcs, out);
    _cxx_set_find(lines, rel, funcs, out);
    _cxx_queue_front(lines, rel, funcs, out);
    _cxx_stack_top(lines, rel, funcs, out);
    _cxx_priority_queue(lines, rel, funcs, out);
    _cxx_array_index(lines, rel, funcs, out);
    _cxx_unordered_set(lines, rel, funcs, out);
    _cxx_wstring_view(lines, rel, funcs, out);
    _cxx_multimap_find(lines, rel, funcs, out);
    _cxx_multiset_find(lines, rel, funcs, out);
    _cxx_binary_semaphore(lines, rel, funcs, out);
    _cxx_error_code(lines, rel, funcs, out);
    _cxx_byteswap(lines, rel, funcs, out);
    _cxx_pmr(lines, rel, funcs, out);
    _cxx_u8string_view(lines, rel, funcs, out);
    _cxx_unordered_multimap(lines, rel, funcs, out);
    _cxx_unordered_multiset(lines, rel, funcs, out);
    _cxx_shared_lock(lines, rel, funcs, out);
    _cxx_atomic_flag(lines, rel, funcs, out);
    _cxx_condvar_any(lines, rel, funcs, out);
    _cxx_recursive_mutex(lines, rel, funcs, out);
    _cxx_timed_mutex(lines, rel, funcs, out);
    _cxx_fstream(lines, rel, funcs, out);
    _cxx_this_thread(lines, rel, funcs, out);
    _cxx_call_once(lines, rel, funcs, out);
    _cxx_shared_timed_mutex(lines, rel, funcs, out);
    _cxx_recursive_timed_mutex(lines, rel, funcs, out);
    _cxx_system_error(lines, rel, funcs, out);
    _cxx_chrono_tzdb(lines, rel, funcs, out);
    _cxx_ranges_zip(lines, rel, funcs, out);
    _cxx_format_to(lines, rel, funcs, out);
    _cxx_error_category(lines, rel, funcs, out);
    _cxx_nested_exception(lines, rel, funcs, out);
    _cxx_atomic_fence(lines, rel, funcs, out);
    _cxx_notify_thread_exit(lines, rel, funcs, out);
    _cxx_wstring_convert(lines, rel, funcs, out);
    _cxx_invoke(lines, rel, funcs, out);
    _cxx_apply(lines, rel, funcs, out);
    _cxx_reference_wrapper(lines, rel, funcs, out);
    _cxx_endian(lines, rel, funcs, out);
    _cxx_bit_ceil(lines, rel, funcs, out);
    _cxx_uncaught_exceptions(lines, rel, funcs, out);
    _cxx_ranges_join(lines, rel, funcs, out);
    _cxx_quick_exit(lines, rel, funcs, out);
    _cxx_to_array(lines, rel, funcs, out);
    _cxx_zoned_time(lines, rel, funcs, out);
    _cxx_kill_dependency(lines, rel, funcs, out);
    _cxx_rotl(lines, rel, funcs, out);
    _cxx_current_exception(lines, rel, funcs, out);
    _cxx_bit_width(lines, rel, funcs, out);
    _cxx_lerp(lines, rel, funcs, out);
    _cxx_midpoint(lines, rel, funcs, out);
    _cxx_cmp_less(lines, rel, funcs, out);
    _cxx_countl_zero(lines, rel, funcs, out);
    _cxx_unreachable(lines, rel, funcs, out);
    _cxx_gcd(lines, rel, funcs, out);
    _cxx_lcm(lines, rel, funcs, out);
    _cxx_clamp(lines, rel, funcs, out);
    _cxx_exchange(lines, rel, funcs, out);
    _cxx_to_address(lines, rel, funcs, out);
    _cxx_is_constant_evaluated(lines, rel, funcs, out);
    _cxx_addressof(lines, rel, funcs, out);
    _cxx_assume_aligned(lines, rel, funcs, out);
    _cxx_as_const(lines, rel, funcs, out);
    _cxx_exclusive_scan(lines, rel, funcs, out);
    _cxx_make_exception_ptr(lines, rel, funcs, out);
    _cxx_set_terminate(lines, rel, funcs, out);
    _cxx_inclusive_scan(lines, rel, funcs, out);
    _cxx_transform_reduce(lines, rel, funcs, out);
    _cxx_reduce(lines, rel, funcs, out);
    _cxx_uninitialized_copy(lines, rel, funcs, out);
    _cxx_construct_at(lines, rel, funcs, out);
    _cxx_forward_like(lines, rel, funcs, out);
    _cxx_uninitialized_fill(lines, rel, funcs, out);
    _cxx_destroy_n(lines, rel, funcs, out);
    _cxx_add_sat(lines, rel, funcs, out);
    _cxx_transform_scan(lines, rel, funcs, out);
    _cxx_type_identity(lines, rel, funcs, out);
    _cxx_nontype(lines, rel, funcs, out);
    _cxx_layout_compatible(lines, rel, funcs, out);
    _cxx_ptr_interconvertible(lines, rel, funcs, out);
    _cxx_uninitialized_value(lines, rel, funcs, out);
    _cxx_const_iterator(lines, rel, funcs, out);
    _cxx_corresponding_member(lines, rel, funcs, out);
    _cxx_ranges_to(lines, rel, funcs, out);
    _cxx_enumerate(lines, rel, funcs, out);
    _cxx_cartesian_product(lines, rel, funcs, out);
    _cxx_chunk(lines, rel, funcs, out);
    _cxx_slide(lines, rel, funcs, out);
    _cxx_adjacent(lines, rel, funcs, out);
    _cxx_join_with(lines, rel, funcs, out);
    _cxx_zip_transform(lines, rel, funcs, out);
    _cxx_as_rvalue(lines, rel, funcs, out);
    _cxx_from_range(lines, rel, funcs, out);
    _cxx_scoped_enum(lines, rel, funcs, out);
    _cxx_stride(lines, rel, funcs, out);
    _cxx_repeat(lines, rel, funcs, out);
    _cxx_take(lines, rel, funcs, out);
    _cxx_drop(lines, rel, funcs, out);
    _cxx_filter(lines, rel, funcs, out);
    _cxx_transform_view(lines, rel, funcs, out);
    _cxx_elements(lines, rel, funcs, out);
    _cxx_iota(lines, rel, funcs, out);
    _cxx_take_while(lines, rel, funcs, out);
    _cxx_drop_while(lines, rel, funcs, out);
    _cxx_keys(lines, rel, funcs, out);
    _cxx_values(lines, rel, funcs, out);
    _cxx_reverse_view(lines, rel, funcs, out);
    _cxx_counted(lines, rel, funcs, out);
}

}  // namespace prism
