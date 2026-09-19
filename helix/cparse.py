"""Comment-stripping, function extraction, SCALAR/POINTER classification.

Types come from the source text, not from a goto model. That is weaker
than ParanoidBSD's classify.py (which reads CBMC's own symbols) and the
records say so: a mis-parsed declarator is OTHER, never SCALAR.
"""

from __future__ import annotations

from pathlib import Path
import re

from helix.models import FunctionInfo

C_EXTS = {".c", ".h", ".cc", ".cpp", ".cxx", ".hpp", ".hh"}
TU_EXTS = {".c", ".cc", ".cpp", ".cxx"}

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
# be swallowed as params or `noexcept` functions are invisible.
_ATTR = (
    r"(?:"
    r"\s*__attribute__\s*\(\s*\([^;{}]*?\)\s*\)"
    r"|\s*noexcept(?:\s*\([^;{}]*?\))?"
    r"|\s*throw\s*\([^;{}]*?\)"
    r"|\s*(?:const|volatile|override|final)"
    r")*"
)

FUNC_HEAD = re.compile(
    r"(?m)^[ \t]*"
    r"(?P<head>"
    r"(?P<mods>(?:(?:static|inline|extern|constexpr|unsigned|signed|"
    r"const|volatile|restrict|_Noreturn)\s+)*)"
    r"(?P<ret>(?:(?:struct|enum|union)\s+)?(?:long\s+long|[A-Za-z_]\w*))"
    r"(?P<stars>(?:\s*\*+\s*|\s+))"
    r"(?P<name>[A-Za-z_]\w*)\s*"
    r"\((?P<params>[^;{}]*?)\)"
    r"(?P<attrs>" + _ATTR + r")"
    r"\s*)"
    r"\{",
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


def strip_comments_keep_lines(text: str, *, blank_strings: bool = True) -> str:
    """Blank comments (and optionally string interiors). Line numbers stay put."""
    out: list[str] = []
    i = 0
    n = len(text)
    in_block = False
    while i < n:
        if in_block:
            if text.startswith("*/", i):
                in_block = False
                i += 2
            else:
                out.append("\n" if text[i] == "\n" else " ")
                i += 1
            continue
        if text.startswith("/*", i):
            in_block = True
            i += 2
            continue
        if text.startswith("//", i):
            while i < n and text[i] != "\n":
                out.append(" ")
                i += 1
            continue
        c = text[i]
        if c == "'":
            # Keep character literals. Blanking them to `' '` made BMC
            # see an empty expression and report ERROR, not a finding.
            out.append(c)
            i += 1
            while i < n and text[i] != "'":
                if text[i] == "\\":
                    out.append(text[i])
                    i += 1
                    if i < n:
                        out.append(text[i])
                        i += 1
                    continue
                out.append(text[i])
                i += 1
            if i < n:
                out.append(text[i])
                i += 1
            continue
        if c == '"':
            q = c
            out.append(q)
            i += 1
            while i < n and text[i] != q:
                if text[i] == "\\":
                    if blank_strings:
                        out.append(" ")
                        i += 1
                        if i < n:
                            out.append(" ")
                            i += 1
                    else:
                        out.append(text[i])
                        i += 1
                        if i < n:
                            out.append(text[i])
                            i += 1
                    continue
                if blank_strings:
                    out.append("\n" if text[i] == "\n" else " ")
                else:
                    out.append(text[i])
                i += 1
            if i < n:
                out.append(text[i])
                i += 1
            continue
        out.append(c)
        i += 1
    return "".join(out)


def _match_brace(text: str, open_idx: int) -> int:
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
    stripped = strip_comments_keep_lines(text)
    bodies = strip_comments_keep_lines(text, blank_strings=False)
    rel = rel or str(path)
    out: list[FunctionInfo] = []
    for m in FUNC_HEAD.finditer(stripped):
        name = m.group("name")
        if name in KW or m.group("ret") in KW:
            continue
        brace = m.end() - 1
        close = _match_brace(stripped, brace)
        if close < 0:
            continue
        # Discovery blanks strings so `"int foo("` is not a function.
        # Bodies keep literals so strcpy/snprintf oracles see the bytes.
        body = bodies[brace + 1 : close]
        head = m.group("head")
        line = stripped[: m.start("head")].count("\n") + 1
        params = _split_params(m.group("params"))
        stars = m.group("stars") or ""
        ret = (m.group("ret") or "int").strip()
        mods = m.group("mods") or ""
        kind = _kind_of(ret, stars, params)
        end_line = stripped[: close].count("\n") + 1
        out.append(
            FunctionInfo(
                file=rel,
                name=name,
                kind=kind,
                line=line,
                signature=re.sub(r"\s+", " ", head).strip(),
                params=params,
                return_type=(stars.strip() + " " + ret).strip(),
                static="static" in mods,
                body=body,
                span=(line, end_line),
            )
        )
    return out


def iter_sources(root: Path) -> list[Path]:
    files: list[Path] = []
    if root.is_file():
        return [root]
    skip = {".git", "helix-out", "third_party", "build", "node_modules", "__pycache__"}
    for p in root.rglob("*"):
        if any(part in skip for part in p.parts):
            continue
        if p.suffix.lower() in C_EXTS and p.is_file():
            files.append(p)
    return sorted(files)
