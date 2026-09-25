"""Comment-stripping, function extraction, SCALAR/POINTER classification.

Types come from the source text, not from a goto model. That is weaker
than ParanoidBSD's classify.py (which reads CBMC's own symbols) and the
records say so: a mis-parsed declarator is OTHER, never SCALAR.
"""

from __future__ import annotations

from bisect import bisect_left
from collections.abc import Callable
from pathlib import Path
import re
from typing import NamedTuple

from prism import scope
from prism import laws
from prism.models import Finding, FunctionInfo

# .i / .ii are preprocessed C / C++ translation units (cc -E output).
C_EXTS = {".c", ".h", ".cc", ".cpp", ".cxx", ".hpp", ".hh", ".i", ".ii"}
TU_EXTS = {".c", ".cc", ".cpp", ".cxx", ".i", ".ii"}

def _is_pointer_type(typ: str) -> bool:
    return "*" in typ or "[" in typ


SCALAR_WORDS = {
    "void", "bool", "_bool",
    "char", "short", "int", "long",
    "float", "double",
    "signed", "unsigned",
    "size_t", "ssize_t", "ptrdiff_t", "intptr_t", "uintptr_t",
    "int8_t", "int16_t", "int32_t", "int64_t",
    "uint8_t", "uint16_t", "uint32_t", "uint64_t",
    "int_fast8_t", "int_fast16_t", "int_fast32_t", "int_fast64_t",
    "uint_fast8_t", "uint_fast16_t", "uint_fast32_t", "uint_fast64_t",
    "int_least8_t", "int_least16_t", "int_least32_t", "int_least64_t",
    "uint_least8_t", "uint_least16_t", "uint_least32_t", "uint_least64_t",
    "_bool", "wchar_t", "char16_t", "char32_t",
    "enum",
}

KW = {
    "if", "for", "while", "switch", "return", "sizeof", "typeof",
    "else", "do", "case", "default", "sizeof", "_Generic",
}

# Trailing declarator junk is not a parameter list. `throw()` must not
# be swallowed as params or `noexcept` functions are invisible. C++
# ref-qualifiers and a trailing return type (`-> int`) end here too.
_ATTR = (
    r"(?:"
    r"\s*__attribute__\s*\(\s*\([^;{}]*?\)\s*\)"
    r"|\s*noexcept(?:\s*\([^;{}]*?\))?"
    r"|\s*throw\s*\([^;{}]*?\)"
    r"|\s*(?:const|volatile|override|final)"
    r"|\s*&&?"
    r")*"
    r"(?:\s*->[^;{}]*)?"
)
# `<...>` nested three deep: `std::map<int, std::vector<int>>`.
_T0 = r"[^<>;{}()]*"
_T2 = r"<" + _T0 + r"(?:<" + _T0 + r">" + _T0 + r")*>"
_TMPL = r"<" + _T0 + r"(?:" + _T2 + _T0 + r")*>"
# Qualifier chain: `std::`, `W::`, `W<T>::`.
_QUAL = r"(?:[A-Za-z_]\w*\s*(?:" + _TMPL + r"\s*)?::\s*)*"
# A parameter list: no `)` escapes it, so `int m(void) BODY` never runs
# on into the next function's parameters. Two levels of nested parens
# cover `void (*cb)(int)`.
_P0 = r"[^;{}()]*"
_P2 = r"\(" + _P0 + r"(?:\(" + _P0 + r"\)" + _P0 + r")*\)"
_PARAMS = _P0 + r"(?:" + _P2 + _P0 + r")*"
# Leading attribute forms: GNU, C++11/C23, MSVC.
_PRE_ATTR = (
    r"(?:__attribute__\s*\(\s*\(" + _PARAMS + r"\)\s*\)"
    r"|\[\[[^\];{}]*\]\]"
    r"|__declspec\s*\([^;{}()]*\))"
)
_OPERATOR = (
    r"operator\s*(?:\(\s*\)|\[\s*\]|new(?:\s*\[\s*\])?|delete(?:\s*\[\s*\])?"
    r"|[^\s\w(){};]{1,3})"
)

FUNC_HEAD = re.compile(
    r"(?m)^[ \t]*"
    r"(?P<head>"
    r"(?P<tmpl>template[ \t]*" + _TMPL + r"[ \t]*)?"
    r"(?P<mods>(?:(?:static|inline|extern|constexpr|consteval|virtual|"
    r"explicit|friend|unsigned|signed|const|volatile|restrict|"
    r"_Noreturn|__inline|__inline__|__forceinline|thread_local|"
    r"__extension__)\s+|" + _PRE_ATTR + r"[ \t]*"
    # A leading export macro before a lowercase type: `JSMN_API int f(`.
    r"|[A-Z_][A-Z0-9_]*[ \t]+(?=[a-z]))*)"
    r"(?P<ret>(?:(?:struct|enum|union|class|typename)\s+)?"
    r"(?:long\s+long|long\s+(?:int|double)\b|short\s+int\b"
    r"|[A-Za-z_]\w*(?:\s*" + _TMPL + r")?"
    r"(?:\s*::\s*[A-Za-z_]\w*(?:\s*" + _TMPL + r")?)*))"
    r"(?P<stars>(?:\s*(?:[*&]|\b(?:const|volatile)\b))+\s*|\s+)"
    # A calling-convention / export macro: `Z3_ast Z3_API Z3_mk_add(`.
    r"(?P<cc>(?:[A-Z_][A-Z0-9_]*|__\w+)[ \t]+)?"
    r"(?P<name>" + _QUAL + r"(?:" + _OPERATOR + r"|[A-Za-z_]\w*))\s*"
    r"\((?P<params>" + _PARAMS + r")\)"
    r"(?P<knr>(?:\s*(?:register\s+)?[A-Za-z_][\w \t\n*,\[\]]*;)*)"
    r"(?P<attrs>" + _ATTR + r")"
    r"\s*)"
    # A function-try-block: `void f(int &x) try { ... } catch (...) { ... }`.
    r"(?P<ftry>try\s*)?"
    r"\{",
)
_KNR_PARAMS = re.compile(r"\s*[A-Za-z_]\w*(?:\s*,\s*[A-Za-z_]\w*)*\s*")

# The declarators below are matched by the scope scan against the text
# before an unattributed `{` (from the previous `;`, `{` or `}`).

# `int (*get_handler(int sig))(int)`: a function returning a function
# pointer.
_FUNC_PTR_DECL = re.compile(
    r"\s*(?P<mods>(?:(?:static|inline|extern|const|unsigned|signed)\s+)*)"
    r"(?P<ret>(?:(?:struct|enum|union)\s+)?[A-Za-z_]\w*)\s*(?P<stars>\**)\s*"
    r"\(\s*\*\s*(?P<name>[A-Za-z_]\w*)\s*\((?P<params>" + _PARAMS + r")\)\s*\)"
    r"\s*\((?P<fparams>" + _PARAMS + r")\)\s*\Z"
)
# Out-of-line constructor / destructor: `W::W(int a) : v(a)`, `W::~W()`.
_CTOR_DECL = re.compile(
    r"\s*(?:template\s*" + _TMPL + r"\s*)?(?:(?:inline|constexpr)\s+)*"
    r"(?P<name>(?:[A-Za-z_]\w*\s*(?:" + _TMPL + r"\s*)?::\s*)+~?[A-Za-z_]\w*)\s*"
    r"\((?P<params>" + _PARAMS + r")\)" + _ATTR +
    r"(?:\s*:(?!:)[^;{}]*)?\s*\Z"
)
# In-class definition FUNC_HEAD cannot see: a constructor, destructor,
# conversion operator, or a method not at the start of a line
# (`struct W { int go() { return 1; } };`).
_MEMBER_DECL = re.compile(
    r"\s*(?:template\s*" + _TMPL + r"\s*)?"
    r"(?:(?:inline|constexpr|consteval|explicit|virtual|static|friend)\s+"
    r"|" + _PRE_ATTR + r"\s*)*"
    r"(?P<ret>[A-Za-z_][\w:<>,\s*&]*?[\s*&])??"
    r"(?P<name>operator\s+(?:(?:const|volatile)\s+)*[A-Za-z_][\w:<>]*"
    r"(?:\s*(?:[*&]|\b(?:const|volatile)\b))*"
    r"|~?[A-Za-z_]\w*|" + _OPERATOR + r")\s*"
    r"\((?P<params>" + _PARAMS + r")\)" + _ATTR +
    r"(?:\s*:(?!:)[^;{}]*)?\s*\Z"
)

# Heap / local pointers: checking the function unguarded invents a buffer.
_LOCAL_PTR = re.compile(
    r"\b(?:struct\s+\w+|union\s+\w+|void|char|int|short|long|unsigned|signed"
    r"|size_t|uint\w*|int\w*|FILE|DIR)\s+\*\s*[A-Za-z_]"
    r"|\b(?:malloc|calloc|realloc|reallocarray|strdup|getenv|fopen"
    r"|popen|alloca|__builtin_alloca)\s*\("
)
# Non-static local array. `return buf` decays to a dangling pointer.
_LOCAL_ARRAY_DECL = re.compile(
    r"(?m)^[ \t]*(?P<static>static\s+)?"
    r"(?:const\s+|volatile\s+)*"
    r"(?:unsigned\s+|signed\s+|long\s+|short\s+)*"
    r"(?:struct\s+\w+|union\s+\w+|enum\s+\w+|"
    r"char|int|short|long|float|double|void|"
    r"size_t|ssize_t|ptrdiff_t|uint\w*|int\w*|wchar_t|_Bool|bool)\s+"
    r"(?P<name>[A-Za-z_]\w*)\s*\["
)
# `return buf;` / `return (buf);` and `return buf + 0` / `return buf + i`
# decay to a pointer. `return 0 + buf` / `return (0 + buf)` is the same
# decay with the array on the right of +. `return buf[0]` is a scalar
# element and does not match.
_RETURN_DECAY = re.compile(
    r"\breturn\s+\(*\s*([A-Za-z_]\w*)\s*\)*\s*(?:;|[+\-])"
)
_RETURN_DECAY_RHS = re.compile(
    r"\breturn\s+[^;]*[+\-]\s*\(*\s*([A-Za-z_]\w*)\s*\)*\s*;"
)


def body_returns_local_array(body: str) -> bool:
    """True for `return buf;` / `return (buf);` / `return buf + i`.

    Array decay to a dangling pointer is not modeled; `return buf[0]` is
    a scalar element and is not this case.
    """
    if not body:
        return False
    arrays = {
        m.group("name")
        for m in _LOCAL_ARRAY_DECL.finditer(body)
        if not m.group("static")
    }
    if not arrays:
        return False
    names = [m.group(1) for m in _RETURN_DECAY.finditer(body)]
    names.extend(m.group(1) for m in _RETURN_DECAY_RHS.finditer(body))
    return any(n in arrays for n in names)


def body_needs_pointer_harness(body: str) -> bool:
    """True when the body names a local pointer, heap object, or address-of."""
    if not body:
        return False
    if _LOCAL_PTR.search(body):
        return True
    # `return &x` / `return (&x)` invents a pointer BMC does not model;
    # not a frontend ERROR. `return x & y` does not match.
    if re.search(r"return\s*\(*\s*&", body):
        return True
    # `return buf` / `return buf + i` / `return 0 + buf` (local array decay).
    return body_returns_local_array(body)


# One token per comment / literal; everything between tokens is copied.
# Alternatives are tried in the same order the old character loop checked
# them (`/*`, `//`, `'`, `"`), so the output is identical to it.
_STRIP_TOKEN = re.compile(
    r"/\*(?P<block>.*?)(?:\*/|\Z)"
    r"|//[^\n]*"
    r"|'(?:\\.?|[^'\\])*'?"
    r'|"(?P<str>(?:\\.?|[^"\\])*)(?P<close>"?)',
    re.S,
)
_STRIP_ESC = re.compile(r"\\.?", re.S)


def _spaces_keep_newlines(s: str) -> str:
    if "\n" not in s:
        return " " * len(s)
    return "\n".join([" " * len(part) for part in s.split("\n")])


def _blank_escape(m: re.Match[str]) -> str:
    # An escape pair is two spaces, even `\\<newline>` (the old loop did so).
    return " " * len(m.group())


def strip_comments_keep_lines(text: str, *, blank_strings: bool = True) -> str:
    """Blank comments (and optionally string interiors). Line numbers stay put.

    `/*` and `*/` themselves are dropped; the comment interior becomes
    spaces with its newlines kept. `//` comments become spaces. Character
    literals are kept verbatim. With `blank_strings`, string interiors are
    spaces (escape pairs are two spaces), their quotes kept.
    """
    out: list[str] = []
    append = out.append
    pos = 0
    for m in _STRIP_TOKEN.finditer(text):
        s = m.start()
        if s > pos:
            append(text[pos:s])
        pos = m.end()
        tok = m.group()
        c = tok[0]
        if c == "/":
            if tok[1] == "*":
                append(_spaces_keep_newlines(m.group("block")))
            else:
                append(" " * len(tok))
        elif c == '"' and blank_strings:
            inner = m.group("str")
            if "\\" in inner:
                inner = _STRIP_ESC.sub(_blank_escape, inner)
            append('"')
            append(_spaces_keep_newlines(inner))
            append(m.group("close"))
        else:
            # Character literal, or a string kept verbatim.
            append(tok)
    if pos < len(text):
        append(text[pos:])
    return "".join(out)


_BRACE = re.compile(r"[{}]")
_CATCH = re.compile(r"\s*catch\s*\(")


def _extend_try(f: "_Found", try_pos: int, code: str, bodies: str,
                line_of: Callable[[int], int]) -> "_Found":
    """A function-try-block (`) try {`): the body is the try block and every
    handler after it, `try { ... } catch (...) { ... }`, as written."""
    close = f.close
    while True:
        m = _CATCH.match(code, close + 1)
        if m is None:
            break
        depth, pc = 0, -1
        for i in range(m.end() - 1, len(code)):
            c = code[i]
            if c == "(":
                depth += 1
            elif c == ")":
                depth -= 1
                if depth == 0:
                    pc = i
                    break
        if pc < 0:
            break
        k = pc + 1
        while k < len(code) and code[k].isspace():
            k += 1
        if k >= len(code) or code[k] != "{":
            break
        bc = _match_brace(code, k)
        if bc < 0:
            break
        close = bc
    f.fn.body = bodies[try_pos : close + 1]
    f.fn.span = (f.fn.span[0], line_of(close))
    return f._replace(close=close)


def _match_brace(text: str, open_idx: int) -> int:
    """Index of the `}` closing the `{` at open_idx, or -1."""
    if open_idx < 0:
        return _match_brace_slow(text, open_idx)
    depth = 0
    for m in _BRACE.finditer(text, open_idx):
        if m.group() == "{":
            depth += 1
        else:
            depth -= 1
            if depth == 0:
                return m.start()
    return -1


def _match_brace_slow(text: str, open_idx: int) -> int:
    depth = 0
    i = open_idx
    n = len(text)
    while i < n:
        c = text[i]
        if c == "{":
            depth += 1
        elif c == "}":
            depth -= 1
            if depth == 0:
                return i
        i += 1
    return -1


def _split_params(params: str) -> list[tuple[str, str]]:
    params = params.strip()
    if not params or params == "void":
        return []
    out: list[tuple[str, str]] = []
    for raw in params.split(","):
        raw = raw.strip()
        if not raw or raw == "...":
            continue
        if "=" in raw:
            raw = raw.split("=", 1)[0].strip()  # C++ default argument
        raw = re.sub(r"\b(const|volatile|restrict|register)\b", "", raw)
        raw = " ".join(raw.split())
        m = re.search(r"([A-Za-z_]\w*)\s*$", raw.replace("*", " * "))
        if not m:
            out.append((raw, ""))
            continue
        name = m.group(1)
        typ = raw[: m.start()].strip() or raw
        out.append((typ, name))
    return out


def _kind_of(ret: str, stars: str, params: list[tuple[str, str]]) -> str:
    if "*" in stars:
        # returning a pointer does not by itself make the *check* unsound;
        # the domain of the inputs does. Return type is recorded, not class.
        pass
    if not params:
        return "VOID"

    def param_ok(typ: str) -> str:
        t = typ.replace("const", " ").replace("volatile", " ")
        if _is_pointer_type(t):
            return "POINTER"
        words = re.findall(r"[A-Za-z_]\w*", t)
        if not words:
            return "OTHER"
        if any(w not in SCALAR_WORDS for w in words):
            # struct-by-value, typedefs we do not know: OTHER, not SCALAR
            if "struct" in words or "union" in words:
                return "OTHER"
            if len(words) == 1 and words[0] not in SCALAR_WORDS:
                return "OTHER"
            if any(w not in SCALAR_WORDS for w in words):
                return "OTHER"
        return "SCALAR"

    kinds = [param_ok(t) for t, _ in params]
    if any(k == "POINTER" for k in kinds):
        return "POINTER"
    if any(k == "OTHER" for k in kinds):
        return "OTHER"
    return "SCALAR"


def tu_is_empty(path: Path) -> bool:
    """True when a .c/.cc/.cpp/.cxx file parses to zero functions."""
    if path.suffix.lower() not in TU_EXTS:
        return False
    return not extract_functions(path)


def extract_functions(path: Path, rel: str | None = None) -> list[FunctionInfo]:
    text = path.read_text(encoding="utf-8", errors="replace")
    return extract_functions_from_text(text, rel or str(path))


def extract_functions_from_text(
    text: str, rel: str, stripped: str | None = None,
) -> list[FunctionInfo]:
    """extract_functions on text already read.

    `stripped` is `strip_comments_keep_lines(text)` when the caller has it.
    """
    return _parse_text(text, rel, stripped)[0]


def parse_gaps(path: Path) -> list[tuple[int, str]]:
    """Top-level brace-delimited code no parsed function owns (Law 7).

    Each gap is (line, first line of the text before its `{`). A gap is
    code the lints, BMC and every other per-function stage never saw.
    """
    text = path.read_text(encoding="utf-8", errors="replace")
    return _parse_text(text, str(path))[1]


def parse_gap_findings(path: Path, rel: str) -> list[Finding]:
    """parse_gaps as inventory records: NOTRUN, cls PARSE-GAP, one per gap."""
    return [
        Finding(
            stage="inventory", status=laws.NOTRUN, file=rel, function=None,
            line=line, cls="PARSE-GAP",
            message=f"line {line}: code in braces not attributed to any "
            f"function; not checked: {head}",
            strength=laws.STRENGTH_FINDS,
        )
        for line, head in parse_gaps(path)
    ]


def _collapse(s: str) -> str:
    return re.sub(r"\s+", " ", s).strip()


def _norm_name(s: str) -> str:
    """`W :: go` -> `W::go`, `operator <<` -> `operator<<`; `operator bool` kept."""
    s = re.sub(r"\s*::\s*", "::", _collapse(s))
    return re.sub(r"\boperator\s+(?=[^\w\s])", "operator", s)


# `virtual ~W() {` / `explicit W(int) {` in a class: the scope scan names
# these; FUNC_HEAD must not take the keyword for a return type.
_NOT_A_TYPE = {
    "virtual", "explicit", "friend", "typedef", "using", "new", "delete", "operator",
}
_CXX_MODS = re.compile(r"\b(?:virtual|explicit|friend|consteval)\b")


class _Found(NamedTuple):
    head_start: int
    close: int
    fn: FunctionInfo


def _parse_text(
    text: str, rel: str, stripped: str | None = None,
) -> tuple[list[FunctionInfo], list[tuple[int, str]]]:
    """(functions, gaps). Discovery runs on string-blanked text."""
    if stripped is None:
        stripped = strip_comments_keep_lines(text)
    bodies = strip_comments_keep_lines(text, blank_strings=False)
    # Character literals blanked too: `'}'` must not close a body.
    code = _unwrap_export_macros(_blank_char_literals(stripped))
    found: dict[int, _Found] = {}  # keyed by the body's `{`
    newlines = [m.start() for m in _NEWLINE.finditer(code)]

    def line_of(pos: int) -> int:
        return bisect_left(newlines, pos) + 1

    def add(head_start: int, brace: int, name: str, kind: str, signature: str,
            params: list[tuple[str, str]], return_type: str,
            static: bool) -> None:
        if brace in found:
            return
        close = _match_brace(code, brace)
        if close < 0:
            return
        line = line_of(head_start)
        found[brace] = _Found(head_start, close, FunctionInfo(
            file=rel,
            name=name,
            kind=kind,
            line=line,
            signature=_collapse(signature),
            params=params,
            return_type=return_type,
            static=static,
            # Discovery blanks strings so `"int foo("` is not a function.
            # Bodies keep literals so strcpy/snprintf oracles see the bytes.
            body=bodies[brace + 1 : close],
            span=(line, line_of(close)),
        ))

    pos = 0
    while True:
        m = FUNC_HEAD.search(code, pos)
        if m is None:
            break
        name = _norm_name(m.group("name"))
        ret = (m.group("ret") or "int").strip()
        knr = m.group("knr") or ""
        if (name.rsplit("::", 1)[-1] in KW or ret in KW or ret in _NOT_A_TYPE
                or (knr.strip() and (
                    not _KNR_PARAMS.fullmatch(m.group("params"))
                    or m.group("params").strip() == "void"))):
            # Not a definition. Resume on the next line: this match may
            # have run over a real head.
            nl = code.find("\n", m.start() + 1)
            if nl < 0:
                break
            pos = nl + 1
            continue
        params = _split_params(m.group("params"))
        stars = m.group("stars") or ""
        mods = m.group("mods") or ""
        attrs = m.group("attrs") or ""
        kind = _kind_of(ret, stars, params)
        if (
            "::" in name or "::" in ret or "<" in ret or ret == "auto"
            or name.startswith("operator") or m.group("tmpl")
            or "&" in stars or "&" in attrs or "->" in attrs
            or _CXX_MODS.search(mods) or knr.strip() or m.group("cc")
        ):
            # A method, template, K&R, calling-convention or C++-typed
            # definition: BMC must not model it as a plain C function.
            kind = "OTHER"
        add(m.start("head"), m.end() - 1, name, kind, m.group("head"), params,
            (stars.strip() + " " + ret).strip(), "static" in mods)
        pos = m.end()
        if m.group("ftry") and m.end() - 1 in found:
            found[m.end() - 1] = _extend_try(found[m.end() - 1], m.start("ftry"), code, bodies, line_of)
            pos = found[m.end() - 1].close + 1

    gaps = _scope_scan(code, found, add, line_of)
    out = [f.fn for _, f in sorted(found.items(), key=lambda kv: (kv[1].head_start, kv[0]))]
    return out, gaps


# --- scope scan ------------------------------------------------------------
#
# Walks file scope and namespace, extern "C" and class bodies. Every `{`
# there is a function body FUNC_HEAD found, a container, an initializer
# or type body, a definition only the declarator regexes above recognize,
# or a gap: code no per-function stage sees (Law 7).

_PP_LINE = re.compile(r"(?m)^[ \t]*#(?:[^\n]*\\\n)*[^\n]*")
_SCOPE_TOKEN = re.compile(r"[{};]")
_ACCESS_LABEL = re.compile(
    r"\s*(?:(?:public|private|protected)(?:\s+(?:slots|Q_SLOTS))?"
    r"|signals|Q_SIGNALS)\s*:(?!:)"
)
_NAMESPACE_HEAD = re.compile(r"(?:^|\s)namespace\b")
_EXTERN_HEAD = re.compile(r"\s*extern\s*\"[^\"]*\"\s*\Z")
_TYPE_HEAD = re.compile(
    r"\s*(?:template\s*" + _TMPL + r"\s*)?(?:typedef\s+)?"
    r"(?:(?P<enum>enum)|class|struct|union)\b"
)
_CLASS_NAME = re.compile(
    r"\s*(?:template\s*" + _TMPL + r"\s*)?(?:typedef\s+)?(?:class|struct|union)\s+"
    r"(?:" + _PRE_ATTR + r"\s*|alignas\s*\([^;{}()]*\)\s*)*"
    r"(?P<name>[A-Za-z_]\w*)"
)
_PRE_ATTR_RE = re.compile(_PRE_ATTR + r"|alignas\s*\([^;{}()]*\)")
_OPERATOR_SYM = re.compile(r"\boperator\s*[^\s\w(]{1,3}")
_PAREN_GROUP = re.compile(r"\([^()]*\)")


def _head_shape(head: str) -> str:
    """`head` with `operator=` names and parenthesized text collapsed.

    `=` left in the shape is an initializer, not a default argument.
    """
    s = _OPERATOR_SYM.sub("operator", head) if "operator" in head else head
    while "(" in s:
        t = _PAREN_GROUP.sub("", s)
        if t == s:
            break
        s = t
    return s.replace(")", "") + ("(" if "(" in head else "")


_NEWLINE = re.compile(r"\n")
_CHAR_LITERAL = re.compile(r"'(?:\\.|[^'\\\n])*'")


# `CJSON_PUBLIC(cJSON *) cJSON_Parse(const char *v) {`: an ALL_CAPS
# function-like export macro wrapping the return type (cJSON, libpng,
# zlib-style APIs). Discovery reads it as `cJSON * cJSON_Parse(...)`; the
# macro name and its parentheses become spaces, so offsets and lines hold.
_EXPORT_MACRO = re.compile(
    r"(?m)^([ \t]*(?:(?:static|extern|inline)[ \t]+)*)"
    r"([A-Z_][A-Z0-9_]*[ \t]*\()([^();{}\n]*)\)"
    r"(?=[ \t]*\**[ \t]*[A-Za-z_]\w*[ \t]*\()"
)


def _unwrap_export_macros(text: str) -> str:
    if "(" not in text:
        return text
    return _EXPORT_MACRO.sub(
        lambda m: m.group(1) + " " * len(m.group(2)) + m.group(3) + " ", text)


def _blank_char_literals(text: str) -> str:
    """Character literal interiors as spaces, quotes kept (`'}'` -> `' '`)."""
    if "'" not in text:
        return text
    return _CHAR_LITERAL.sub(lambda m: "'" + " " * (len(m.group()) - 2) + "'", text)


def _scope_scan(
    stripped: str,
    found: dict[int, _Found],
    add: Callable[..., None],
    line_of: Callable[[int], int],
) -> list[tuple[int, str]]:
    """Attribute container-level `{`; qualify in-class methods; list gaps."""
    text = stripped
    if "#" in text:
        text = _PP_LINE.sub(lambda m: _spaces_keep_newlines(m.group()), text)
    gaps: list[tuple[int, str]] = []
    # Open containers: (is_class, qualified class name or "").
    stack: list[tuple[bool, str]] = []
    head_start = 0
    pos = 0
    while True:
        m = _SCOPE_TOKEN.search(text, pos)
        if m is None:
            break
        c = m.group()
        brace = m.start()
        pos = m.end()
        if c == ";":
            head_start = pos
            continue
        if c == "}":
            if stack:
                stack.pop()
            head_start = pos
            continue
        in_class, qual = stack[-1] if stack else (False, "")
        head = text[head_start:brace]
        lab = _ACCESS_LABEL.match(head)
        if lab:
            head = head[lab.end():]
        lead = len(head) - len(head.lstrip())
        start = brace - len(head) + lead
        head_start = pos
        shape = _head_shape(head)
        hit = found.get(brace)
        if hit is None and "(" in shape and "=" not in shape:
            hit = _scan_definition(head, start, brace, qual, in_class, add, found)
        if hit is not None:
            if in_class:
                if qual and "::" not in hit.fn.name:
                    hit.fn.name = f"{qual}::{hit.fn.name}"
                hit.fn.kind = "OTHER"
            pos = head_start = hit.close + 1
            continue
        if _EXTERN_HEAD.match(head) or (
            _NAMESPACE_HEAD.search(head) and "(" not in head
        ):
            stack.append((False, qual))
            continue
        tm = _TYPE_HEAD.match(head)
        is_type = tm is not None and "=" not in shape and (
            bool(tm.group("enum")) or "(" not in _PRE_ATTR_RE.sub("", head)
        )
        if is_type and tm and not tm.group("enum"):
            cm = _CLASS_NAME.match(head)
            cname = cm.group("name") if cm else ""
            if cname and qual:
                cname = f"{qual}::{cname}"
            stack.append((True, cname))
            continue
        if not is_type and "(" in shape and "=" not in shape:
            line = line_of(start)
            gaps.append((line, head.strip().split("\n", 1)[0].strip()[:80]))
        # Initializer, enum or brace-init body, or a gap: skip it whole.
        close = _match_brace(text, brace)
        if close < 0:
            break
        pos = head_start = close + 1
    return gaps


def _scan_definition(
    head: str, start: int, brace: int, qual: str,
    in_class: bool, add: Callable[..., None], found: dict[int, _Found],
) -> _Found | None:
    """A definition FUNC_HEAD missed, recognized from its declarator."""
    m = _FUNC_PTR_DECL.match(head)
    if m and m.group("name") not in KW and m.group("ret") not in KW:
        ret = m.group("ret").strip()
        stars = m.group("stars") or ""
        add(start, brace, m.group("name"), "OTHER", head,
            _split_params(m.group("params")),
            f"{ret} {stars}(*)({_collapse(m.group('fparams'))})",
            "static" in (m.group("mods") or ""))
        return found.get(brace)
    m = _CTOR_DECL.match(head)
    if m:
        name = _norm_name(m.group("name"))
        parts = [re.sub(r"<.*$", "", p) for p in name.split("::")]
        if parts[-1].startswith("~") or parts[-1] == parts[-2]:
            add(start, brace, name, "OTHER", head,
                _split_params(m.group("params")), "", False)
            return found.get(brace)
    if in_class:
        m = _MEMBER_DECL.match(head)
        if m and m.group("name") not in KW:
            name = _norm_name(m.group("name"))
            add(start, brace, f"{qual}::{name}" if qual else name, "OTHER",
                head, _split_params(m.group("params")),
                _collapse(m.group("ret") or ""), False)
            return found.get(brace)
    # `TEST(Suite, Name) {` (Unity fixture, GoogleTest) and other ALL_CAPS
    # macros that define a function: the body is code, checked by the
    # lints; OTHER, so no stage models it as a plain C function.
    m = _MACRO_DEF.match(head)
    if m and not in_class:
        name = "_".join(
            [m.group("mac")] + [a for a in re.split(r"[^A-Za-z0-9_]+", m.group("args")) if a])
        add(start, brace, name, "OTHER", head, [], "void", False)
        return found.get(brace)
    return None


_MACRO_DEF = re.compile(r"\s*(?P<mac>[A-Z_][A-Z0-9_]*)\s*\((?P<args>[^()]*)\)\s*\Z")


def iter_sources(root: Path) -> list[Path]:
    files: list[Path] = []
    if root.is_file():
        return [root]
    for p in root.rglob("*"):
        if scope.skipped_path(p, root):
            continue
        if p.suffix.lower() in C_EXTS and p.is_file():
            files.append(p)
    return sorted(files)
