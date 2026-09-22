"""Emit a compiling C++23 checkers_cxx.cpp from prism/checkers.py regexes."""
from __future__ import annotations

import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
src = (ROOT / "prism" / "checkers.py").read_text(encoding="utf-8")


def unraw(lit: str) -> str:
    lit = lit.strip()
    if lit.startswith("r"):
        lit = lit[1:]
    if lit.startswith("'''") or lit.startswith('"""'):
        return lit[3:-3]
    if len(lit) >= 2 and lit[0] in "\"'":
        return lit[1:-1]
    return lit


def cpp_raw(s: str) -> str:
    # Use R"py(...)py" so the pattern can contain quotes and backslashes.
    if ")py" in s:
        return 'R"cxx(' + s + ')cxx"'
    return 'R"py(' + s + ')py"'


# Collect re.compile assignments of the form NAME = re.compile( r"..." [, flags] )
assigns: dict[str, tuple[str, bool]] = {}
pat = re.compile(
    r"^([A-Za-z_]\w*)\s*=\s*re\.compile\(\s*(r?(?:'''[\s\S]*?'''|\"\"\"[\s\S]*?\"\"\"|'(?:\\.|[^'])*'|\"(?:\\.|[^\"])*\"))\s*(?:,\s*re\.(M|S|I|X))*",
    re.M,
)
for m in pat.finditer(src):
    name, lit = m.group(1), m.group(2)
    flags = m.group(0)
    multiline = "re.M" in flags
    try:
        assigns[name] = (unraw(lit), multiline)
    except Exception:
        pass

# Concatenated adjacent string literals inside compile() for a few _CXX_* names.
# Fallback: also catch NAME = re.compile(\n    r"..." \n    r"..." \n)
pat2 = re.compile(
    r"^(_CXX_\w+|_STD_MOVE|_NEW_ASSIGN|_DELETE|_CATCH_ALL_BLOCK|_THROW_NEW)\s*=\s*re\.compile\(\s*((?:r?(?:'''[\s\S]*?'''|\"\"\"[\s\S]*?\"\"\"|'(?:\\.|[^'])*'|\"(?:\\.|[^\"])*\")\s*)+)\)",
    re.M,
)
for m in pat2.finditer(src):
    name = m.group(1)
    parts = re.findall(
        r"r?(?:'''[\s\S]*?'''|\"\"\"[\s\S]*?\"\"\"|'(?:\\.|[^'])*'|\"(?:\\.|[^\"])*\")",
        m.group(2),
    )
    body = "".join(unraw(p) for p in parts)
    multiline = "re.M" in m.group(0)
    assigns[name] = (body, multiline)

# token_subscript_unguarded call sites
calls = re.findall(
    r"_cxx_token_subscript_unguarded\(\s*lines,\s*rel,\s*funcs,\s*out,\s*"
    r"(\w+),\s*\"([^\"]+)\",\s*\"([^\"]+)\"",
    src,
)

# Presence-style: cls from Finding + first _CXX_ / regex search in that function.
# We emit a second table from taxonomy CXX ids that have a matching _CXX_* regex.

out = []
out.append('#include "prism/checkers.hpp"')
out.append('#include "prism/cparse.hpp"')
out.append("")
out.append("#include <cctype>")
out.append("#include <map>")
out.append("#include <optional>")
out.append("#include <string>")
out.append("#include <string_view>")
out.append("#include <unordered_map>")
out.append("#include <unordered_set>")
out.append("#include <vector>")
out.append("")
out.append("namespace prism {")
out.append("namespace {")
out.append("")
out.append("std::vector<std::string> split_lines(std::string_view s) {")
out.append("    std::vector<std::string> out;")
out.append("    std::size_t i = 0;")
out.append("    while (i <= s.size()) {")
out.append("        auto n = s.find('\\n', i);")
out.append("        if (n == std::string_view::npos) {")
out.append("            out.emplace_back(s.substr(i));")
out.append("            break;")
out.append("        }")
out.append("        auto line = s.substr(i, n - i);")
out.append("        if (!line.empty() && line.back() == '\\r') line.remove_suffix(1);")
out.append("        out.emplace_back(line);")
out.append("        i = n + 1;")
out.append("        if (i == s.size()) { out.emplace_back(); break; }")
out.append("    }")
out.append("    return out;")
out.append("}")
out.append("")
out.append("std::string re_escape(std::string_view s) {")
out.append('    static const std::string special = R"(.^$*+?{}[]\\\\|())";')
out.append("    std::string o;")
out.append("    for (unsigned char c : s) {")
out.append("        if (special.find(static_cast<char>(c)) != std::string::npos) o.push_back('\\\\');")
out.append("        o.push_back(static_cast<char>(c));")
out.append("    }")
out.append("    return o;")
out.append("}")
out.append("")
out.append("bool idx_guarded(std::string_view idx, std::string_view body) {")
out.append("    auto v = re_escape(idx);")
out.append("    if (Regex(v + R\"(\\s*(?:<|<=|>|>=|==)\\s*)\").search(body)) return true;")
out.append("    if (Regex(R\"((?:<|<=|>|>=|==)\\s*)\" + v).search(body)) return true;")
out.append("    if (Regex(R\"(\\b(?:size|length|empty|count|contains|find)\\s*\\()\").search(body) &&")
out.append("        Regex(std::string(R\"(\\b)\") + v + R\"(\\b)\").search(body))")
out.append("        return true;")
out.append("    return false;")
out.append("}")
out.append("")
out.append("bool toarr_idx_bad(std::string idx, std::string_view body) {")
out.append("    while (!idx.empty() && std::isspace(static_cast<unsigned char>(idx.front()))) idx.erase(idx.begin());")
out.append("    while (!idx.empty() && std::isspace(static_cast<unsigned char>(idx.back()))) idx.pop_back();")
out.append("    if (idx.empty()) return false;")
out.append("    bool digits = true;")
out.append("    for (char c : idx) if (c < '0' || c > '9') digits = false;")
out.append("    if (digits) return std::stoi(idx) >= 4;")
out.append("    return !idx_guarded(idx, body);")
out.append("}")
out.append("")
out.append("bool move_use_after(std::string_view name, std::string_view ln) {")
out.append("    static Regex mv(R\"(std\\s*::\\s*move\\s*\\()\");")
out.append("    if (mv.search(ln)) return false;")
out.append("    auto v = re_escape(name);")
out.append("    return Regex(std::string(R\"((?<![A-Za-z0-9_]))\") + v + R\"((?![A-Za-z0-9_]))\").search(ln);")
out.append("}")
out.append("")
out.append("void token_sub(const std::vector<std::string>& lines, std::string_view rel,")
out.append("              const std::vector<FunctionInfo>& funcs, std::vector<Finding>& out,")
out.append("              const Regex& token, std::string_view cls, std::string_view message) {")
out.append("    static Regex sub(R\"(\\b(?P<cont>[A-Za-z_]\\w*)\\s*\\[\\s*(?P<idx>[^\\]]+)\\s*\\])\");")
out.append("    for (auto& fn : funcs) {")
out.append("        if (!token.search(fn.body)) continue;")
out.append("        auto body_lines = split_lines(fn.body);")
out.append("        for (int i = 0; i < static_cast<int>(body_lines.size()); ++i) {")
out.append("            for (auto& m : sub.finditer(body_lines[static_cast<std::size_t>(i)])) {")
out.append("                if (!toarr_idx_bad(m.named(\"idx\"), fn.body)) continue;")
out.append("                lint_add(out, rel, fn.name, fn.span.first + i, cls, message, lines);")
out.append("                return;")
out.append("            }")
out.append("        }")
out.append("    }")
out.append("}")
out.append("")
out.append("void scan_tok(const std::vector<std::string>& lines, std::string_view rel,")
out.append("              const std::vector<FunctionInfo>& funcs, std::vector<Finding>& out,")
out.append("              const Regex& token, std::string_view cls, std::string_view message,")
out.append("              const Regex* silence = nullptr) {")
out.append("    for (auto& fn : funcs) {")
out.append("        if (!token.search(fn.body)) continue;")
out.append("        if (silence && silence->search(fn.body)) continue;")
out.append("        auto body_lines = split_lines(fn.body);")
out.append("        for (int i = 0; i < static_cast<int>(body_lines.size()); ++i) {")
out.append("            if (!token.search(body_lines[static_cast<std::size_t>(i)])) continue;")
out.append("            lint_add(out, rel, fn.name, fn.span.first + i, cls, message, lines);")
out.append("            return;")
out.append("        }")
out.append("    }")
out.append("}")
out.append("")

# new/delete, use after move, vector index
out.append("void cxx_new_delete(const std::vector<std::string>& lines, std::string_view rel,")
out.append("                    const std::vector<FunctionInfo>& funcs, std::vector<Finding>& out) {")
out.append("    static Regex nw(R\"(\\b(?P<var>[A-Za-z_]\\w*)\\s*=\\s*new\\s+(?P<rest>[^;]+))\");")
out.append("    static Regex dl(R\"(\\bdelete\\s*(\\[\\s*\\])?\\s*(?P<var>[A-Za-z_]\\w*)\\b)\");")
out.append("    for (auto& fn : funcs) {")
out.append("        auto chunk = split_lines(fn.body);")
out.append("        std::map<std::string, bool> kind;")
out.append("        for (auto& ln : chunk) {")
out.append("            auto m = nw.search_match(ln);")
out.append("            if (!m) continue;")
out.append("            auto rest = m->named(\"rest\");")
out.append("            auto par = rest.find('(');")
out.append("            auto head = par == std::string::npos ? rest : rest.substr(0, par);")
out.append("            kind[m->named(\"var\")] = head.find('[') != std::string::npos;")
out.append("        }")
out.append("        for (int i = 0; i < static_cast<int>(chunk.size()); ++i) {")
out.append("            auto m = dl.search_match(chunk[static_cast<std::size_t>(i)]);")
out.append("            if (!m) continue;")
out.append("            auto var = m->named(\"var\");")
out.append("            auto it = kind.find(var);")
out.append("            if (it == kind.end()) continue;")
out.append("            bool del_arr = m->group(1).size() > 0;")
out.append("            if (it->second == del_arr) continue;")
out.append("            lint_add(out, rel, fn.name, fn.span.first + i, \"MEM-NEW-DELETE\",")
out.append("                     var + \" new/delete [] mismatch\", lines);")
out.append("            return;")
out.append("        }")
out.append("    }")
out.append("}")
out.append("")
out.append("void cxx_use_after_move(const std::vector<std::string>& lines, std::string_view rel,")
out.append("                        const std::vector<FunctionInfo>& funcs, std::vector<Finding>& out) {")
out.append("    static Regex mv(R\"(std\\s*::\\s*move\\s*\\(\\s*(?P<name>[A-Za-z_]\\w*)\\s*\\))\");")
out.append("    for (auto& fn : funcs) {")
out.append("        auto body_lines = split_lines(fn.body);")
out.append("        std::map<std::string, int> last;")
out.append("        for (int i = 0; i < static_cast<int>(body_lines.size()); ++i)")
out.append("            for (auto& m : mv.finditer(body_lines[static_cast<std::size_t>(i)]))")
out.append("                last[m.named(\"name\")] = i;")
out.append("        for (auto& [name, mi] : last) {")
out.append("            for (int j = mi + 1; j < static_cast<int>(body_lines.size()); ++j) {")
out.append("                if (!move_use_after(name, body_lines[static_cast<std::size_t>(j)])) continue;")
out.append("                lint_add(out, rel, fn.name, fn.span.first + j, \"CXX-USE-AFTER-MOVE\",")
out.append("                         name + \" used after std::move\", lines);")
out.append("                goto next_fn;")
out.append("            }")
out.append("        }")
out.append("    next_fn:;")
out.append("    }")
out.append("}")
out.append("")
out.append("void cxx_vector_index(const std::vector<std::string>& lines, std::string_view rel,")
out.append("                      const std::vector<FunctionInfo>& funcs, std::vector<Finding>& out) {")
out.append("    static Regex decl(R\"(\\b(?:std\\s*::\\s*)?(?:vector\\s*<[^>;]+>|(?:basic_)?string)\\s+(?P<name>[A-Za-z_]\\w*)\\b)\");")
out.append("    static Regex sub(R\"(\\b(?P<cont>[A-Za-z_]\\w*)\\s*\\[\\s*(?P<idx>[^\\]]+)\\s*\\])\");")
out.append("    for (auto& fn : funcs) {")
out.append("        std::unordered_set<std::string> names;")
out.append("        for (auto& m : decl.finditer(fn.body)) names.insert(m.named(\"name\"));")
out.append("        if (names.empty()) continue;")
out.append("        auto body_lines = split_lines(fn.body);")
out.append("        for (int i = 0; i < static_cast<int>(body_lines.size()); ++i) {")
out.append("            for (auto& m : sub.finditer(body_lines[static_cast<std::size_t>(i)])) {")
out.append("                auto cont = m.named(\"cont\");")
out.append("                if (!names.contains(cont)) continue;")
out.append("                if (idx_guarded(m.named(\"idx\"), fn.body) &&")
out.append("                    Regex(re_escape(cont) + R\"(\\s*\\.\\s*(?:size|length)\\s*\\()\").search(fn.body))")
out.append("                    continue;")
out.append("                if (!toarr_idx_bad(m.named(\"idx\"), fn.body) &&")
out.append("                    Regex(re_escape(cont) + R\"(\\s*\\.\\s*(?:size|length)\\s*\\()\").search(fn.body))")
out.append("                    continue;")
out.append("                if (Regex(re_escape(cont) + R\"(\\s*\\.\\s*(?:size|length)\\s*\\()\").search(fn.body))")
out.append("                    continue;")
out.append("                lint_add(out, rel, fn.name, fn.span.first + i, \"CXX-VECTOR-INDEX\",")
out.append("                         cont + \"[] without a size()/length() guard\", lines);")
out.append("                return;")
out.append("            }")
out.append("        }")
out.append("    }")
out.append("}")
out.append("")

# Emit token regex statics used by token_sub
needed = set()
for var, cls, msg in calls:
    needed.add(var)

# Also emit a handful of presence regexes
presence = [
    ("_THROW_NEW", "CXX-THROW-NEW", "throw new allocates the exception with new", None),
    ("_CATCH_ALL_BLOCK", "CXX-CATCH-ALL", "empty catch (...) swallows exceptions", r"\bthrow\s*;"),
    ("_STD_MOVE", None, None, None),  # used by use-after-move already
]

for name, (pat_s, ml) in sorted(assigns.items()):
    if name not in needed and name not in {"_THROW_NEW", "_CATCH_ALL_BLOCK", "_CXX_AUTO_PTR_USE",
                                          "_AUTO_PTR_USE"}:
        if not name.startswith("_CXX_") and name not in {"_THROW_NEW", "_CATCH_ALL_BLOCK"}:
            continue
        # keep presence tokens for CXX taxonomy
        if name not in needed and not name.startswith("_CXX_"):
            continue

# We'll emit regexes for token_sub vars and a compact presence table of _CXX_* that
# are NOT used by token_sub, using scan_tok(token, cls from name).

emitted = set()
for var, cls, msg in calls:
    if var not in assigns:
        continue
    pat_s, ml = assigns[var]
    cpp_name = "rx" + var  # _CXX_TAKE -> rx_CXX_TAKE
    if cpp_name in emitted:
        continue
    emitted.add(cpp_name)
    ml_arg = ", true" if ml else ""
    out.append(f"const Regex {cpp_name}{{{cpp_raw(pat_s)}{ml_arg}}};")

for extra in ("_THROW_NEW", "_CATCH_ALL_BLOCK"):
    if extra in assigns:
        pat_s, ml = assigns[extra]
        ml_arg = ", true" if ml else ""
        out.append(f"const Regex rx{extra}{{{cpp_raw(pat_s)}{ml_arg}}};")

# Presence regexes: map _CXX_FOO to CXX-FOO
# Skip ones already in token_sub
token_vars = {v for v, _, _ in calls}

presence_skip = token_vars | {"_CXX_SUBSCRIPT", "_CXX_TYPE", "_CXX_VIRTUAL", "_CXX_CALL",
                              "_CXX_INHERIT", "_CXX_LOCAL_OBJ", "_CXX_PTR_DECL", "_CXX_CLASS_NAME"}

# Emit remaining _CXX_* as rx and scan with derived class id
for name, (pat_s, ml) in sorted(assigns.items()):
    if not name.startswith("_CXX_"):
        continue
    if name in token_vars:
        continue
    cpp_name = "rx" + name
    if cpp_name in emitted:
        continue
    emitted.add(cpp_name)
    ml_arg = ", true" if ml else ""
    out.append(f"const Regex {cpp_name}{{{cpp_raw(pat_s)}{ml_arg}}};")

out.append("")
out.append("struct TokRule { const Regex* re; const char* cls; const char* msg; };")
out.append("const TokRule kTokenSub[] = {")
for var, cls, msg in calls:
    if var not in assigns:
        continue
    out.append(f'    {{&rx{var}, "{cls}", "{msg}"}},')
out.append("};")
out.append("")

# Presence: derive CLS from name _CXX_FOO_BAR -> CXX-FOO-BAR
out.append("struct PresRule { const Regex* re; const char* cls; const char* msg; const char* silence; };")
out.append("const PresRule kPresent[] = {")
for name, (pat_s, ml) in sorted(assigns.items()):
    if not name.startswith("_CXX_"):
        continue
    if name in token_vars:
        continue
    # skip helper regexes that aren't class tokens
    skip_suffix = ("_DECL", "_BIND", "_CALL", "_TOKEN", "_FN", "_ASSIGN", "_CTOR", "_GET",
                   "_TEST", "_MUTATE", "_VIEW", "_RET", "_LINE", "_ANY", "_LOCAL", "_MEMBER",
                   "_AUTO", "_ZERO", "_CATCH", "_WAITER", "_XFORM", "_ORDER", "_LOOKUP",
                   "_STARTED", "_DIRECT")
    # Actually DECL regexes ARE used as tokens in many lints (optional_decl etc.)
    cls = name[1:].replace("_", "-")  # CXX-TAKE
    msg = cls.replace("-", " ").lower()
    out.append(f'    {{&rx{name}, "{cls}", "{msg}", nullptr}},')
out.append("};")
out.append("")

out.append("}  // namespace")
out.append("")
out.append("void checkers_cxx(const std::vector<std::string>& lines, std::string_view rel,")
out.append("                  const std::vector<FunctionInfo>& funcs, std::string_view stripped,")
out.append("                  std::vector<Finding>& out) {")
out.append("    (void)stripped;")
out.append("    cxx_new_delete(lines, rel, funcs, out);")
out.append("    cxx_use_after_move(lines, rel, funcs, out);")
out.append("    cxx_vector_index(lines, rel, funcs, out);")
out.append("    static Regex throw_new_re = rx_THROW_NEW;")
out.append("    scan_tok(lines, rel, funcs, out, rx_THROW_NEW, \"CXX-THROW-NEW\",")
out.append("             \"throw new allocates the exception with new\");")
out.append("    static Regex rethrow(R\"(\\bthrow\\s*;)\");")
out.append("    scan_tok(lines, rel, funcs, out, rx_CATCH_ALL_BLOCK, \"CXX-CATCH-ALL\",")
out.append("             \"empty catch (...) swallows exceptions\", &rethrow);")
out.append("    for (auto& r : kTokenSub)")
out.append("        token_sub(lines, rel, funcs, out, *r.re, r.cls, r.msg);")
out.append("    for (auto& r : kPresent)")
out.append("        scan_tok(lines, rel, funcs, out, *r.re, r.cls, r.msg);")
out.append("}")
out.append("")
out.append("}  // namespace prism")
out.append("")

path = ROOT / "src" / "prism" / "checkers_cxx.cpp"
text = "\n".join(out)
path.write_text(text, encoding="utf-8")
print("wrote", path, "lines", text.count("\n") + 1, "token_sub", len(calls), "regexes", len(emitted))
