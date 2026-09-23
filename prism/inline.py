"""One-level inliner for static scalar callees in the same translation unit."""

from __future__ import annotations

from dataclasses import replace
from pathlib import Path
import re

from prism.models import FunctionInfo

_KW = {
    "if", "for", "while", "switch", "return", "sizeof", "typeof",
    "else", "do", "case", "default", "_Generic", "break", "continue",
    "goto", "struct", "union", "enum",
}

_SCALAR_TYPES = (
    r"int|unsigned(?:\s+int)?|long|short|char|"
    r"uint32_t|int32_t|size_t|bool|_Bool"
)


def _basename(file: str) -> str:
    return Path(file).name


def _is_ident(name: str) -> bool:
    return bool(re.fullmatch(r"[A-Za-z_]\w*", name))


# Calls the BMC encoder models (bmc._model_call): a callee that only calls
# these can still be inlined. __VERIFIER_nondet_* is matched by prefix.
_MODELLED_CALLS = {
    "abs", "labs", "llabs", "__builtin_expect", "rand", "__VERIFIER_assume",
    "assume_abort_if_not", "abort", "exit", "_Exit", "quick_exit", "reach_error",
    "__assert_fail", "__VERIFIER_error", "printLine", "printWLine", "printIntLine", "printShortLine",
    "printLongLine", "printLongLongLine", "printSizeTLine", "printHexCharLine", "printUnsignedLine", "printHexUnsignedCharLine",
}


def _has_calls(body: str) -> bool:
    for m in re.finditer(r"\b([A-Za-z_]\w*)\s*\(", body or ""):
        n = m.group(1)
        if n in _KW or n in _MODELLED_CALLS or n.startswith("__VERIFIER_nondet_"):
            continue
        return True
    return False


def _returns_value(callee: FunctionInfo) -> bool:
    rt = (callee.return_type or "int").strip()
    return rt != "void" and not rt.endswith(" void")


# Integer type names the BMC encoder declares (bmc._CDECL_TYPE).
_DECL_TYPE = (
    r"(?:(?:unsigned|signed)\s+(?:long\s+long|long|short|char|int)(?:\s+int)?|"
    r"long\s+long(?:\s+int)?|long(?:\s+int)?|short(?:\s+int)?|"
    r"unsigned|signed|int|char|_Bool|bool|u?int(?:8|16|32|64)_t|"
    r"size_t|ssize_t|ptrdiff_t|u?intptr_t|u?intmax_t)"
)


def _callee_ret_type(callee: FunctionInfo) -> str | None:
    """The callee's full return type, from its signature.

    `return_type` keeps only the last word (`unsigned char` would read as
    `char`). None: not a type the encoder can declare, so no inlining.
    """
    sig = callee.signature or ""
    at = sig.find(callee.name + "(")
    if at < 0:
        at = sig.find(callee.name)
    if at < 0:
        return None
    head = sig[:at]
    head = re.sub(r"\[\[[^\]]*\]\]|\b(?:static|inline|__inline__|__inline|extern|constexpr)\b",
                  " ", head)
    head = " ".join(head.split())
    if head == "void":
        return head
    if not re.fullmatch(_DECL_TYPE, head):
        return None
    return head


def _inlineable_callee(callee: FunctionInfo) -> bool:
    if not callee.static:
        return False
    if callee.kind not in ("SCALAR", "VOID"):
        return False
    if _has_calls(callee.body):
        return False
    if _returns_value(callee) and _callee_ret_type(callee) is None:
        return False
    return True


def _lookup(
    index: dict[tuple[str, str], FunctionInfo],
    file: str,
    name: str,
) -> FunctionInfo | None:
    return index.get((_basename(file), name))


def _split_args(args: str) -> list[str]:
    args = args.strip()
    if not args:
        return []
    out: list[str] = []
    depth = 0
    start = 0
    for i, ch in enumerate(args):
        if ch == "(":
            depth += 1
        elif ch == ")":
            depth -= 1
        elif ch == "," and depth == 0:
            out.append(args[start:i].strip())
            start = i + 1
    out.append(args[start:].strip())
    return out


def _parse_call(text: str, start: int) -> tuple[str, str, int] | None:
    m = re.match(r"[A-Za-z_]\w*", text[start:])
    if not m:
        return None
    name = m.group(0)
    if name in _KW or not _is_ident(name):
        return None
    pos = start + len(name)
    while pos < len(text) and text[pos] in " \t\n":
        pos += 1
    if pos >= len(text) or text[pos] != "(":
        return None
    depth = 0
    for i in range(pos, len(text)):
        ch = text[i]
        if ch == "(":
            depth += 1
        elif ch == ")":
            depth -= 1
            if depth == 0:
                return name, text[pos + 1 : i], i + 1
    return None


def _match_balanced(text: str, open_ch: str, close_ch: str) -> tuple[str, str] | None:
    if not text.startswith(open_ch):
        return None
    depth = 0
    for i, ch in enumerate(text):
        if ch == open_ch:
            depth += 1
        elif ch == close_ch:
            depth -= 1
            if depth == 0:
                return text[1:i], text[i + 1 :]
    return None


def _take_stmt(text: str) -> tuple[str, str]:
    depth = 0
    for i, ch in enumerate(text):
        if ch == "(":
            depth += 1
        elif ch == ")":
            depth -= 1
        elif ch == "{" and depth == 0:
            break
        elif ch == ";" and depth == 0:
            return text[: i + 1], text[i + 1 :]
    return text, ""


def _rename_params(body: str, rename: dict[str, str]) -> str:
    """One pass over the identifiers (the C++ engine's rename_params)."""
    return re.sub(r"[A-Za-z_]\w*", lambda m: rename.get(m.group(0), m.group(0)), body)


def _adapt_callee_body(body: str, rename: dict[str, str], ret_var: str | None) -> str | None:
    """A `return` must leave the inlined body.

    Every return becomes `{ ret = X; break; }` inside `do { ... } while (0)`.
    A callee with its own loop or switch (where that break would bind to the
    wrong statement) is inlined only when its single return is its last
    top-level statement; otherwise None (not inlined).
    """
    body = _rename_params(body, rename).strip()
    rets = list(re.finditer(r"\breturn\b", body))
    if not rets:
        return body
    single_final = False
    if len(rets) == 1:
        at = rets[0].start()
        depth = body[:at].count("{") - body[:at].count("}")
        semi = body.find(";", at)
        single_final = depth == 0 and semi >= 0 and not body[semi + 1:].strip()
    if single_final:
        if ret_var:
            body = re.sub(r"\breturn\s+([^;]+);", lambda m: f"{ret_var} = {m.group(1)};", body)
        return re.sub(r"\breturn\s*;", "", body).strip()
    if re.search(r"\b(?:for|while|do|switch)\b", body):
        return None
    if ret_var:
        body = re.sub(r"\breturn\s+([^;]+);", lambda m: f"{{ {ret_var} = {m.group(1)}; break; }}", body)
    body = re.sub(r"\breturn\s*;", "break;", body)
    return "do {\n" + body + "\n} while (0);"


def _add_callee_locals(body: str, prefix: str, rename: dict[str, str]) -> None:
    """Locals the callee declares, renamed with the call-site prefix."""
    type_words = {"int", "unsigned", "signed", "long", "short", "char", "_Bool", "bool"}
    for m in re.finditer(r"\b" + _DECL_TYPE + r"\s+([A-Za-z_]\w*)\s*(?=[=;,\[)])", body):
        name = m.group(1)
        if name in type_words or name in rename:
            continue
        rename[name] = f"{prefix}_l_{name}"


def _param_type(typ: str) -> str:
    t = " ".join((typ or "int").split())
    return t or "int"


def _build_inline_block(
    callee: FunctionInfo,
    args: list[str],
    *,
    prefix: str,
    tail: str,
) -> str | None:
    if len(args) != len(callee.params):
        return None
    decls: list[str] = []
    rename: dict[str, str] = {}
    for i, ((typ, pname), arg) in enumerate(zip(callee.params, args)):
        temp = f"{prefix}_i{i}"
        rename[pname] = temp
        decls.append(f"{_param_type(typ)} {temp} = {arg};")
    ret_var = f"{prefix}_ret" if _returns_value(callee) else None
    if ret_var:
        rt = _callee_ret_type(callee)
        if rt is None:
            return None
        decls.append(f"{rt} {ret_var};")
    _add_callee_locals(callee.body, prefix, rename)
    adapted = _adapt_callee_body(callee.body, rename, ret_var)
    if adapted is None:
        return None
    parts = decls + ([adapted] if adapted else [])
    if tail:
        parts.append(tail.rstrip(";") + ";")
    inner = "\n".join(parts)
    return "{\n" + inner + "\n}"


def _try_inline_stmt(
    stmt: str,
    fn: FunctionInfo,
    index: dict[tuple[str, str], FunctionInfo],
    site: int,
) -> str:
    raw = stmt
    s = stmt.strip()
    if not s.endswith(";"):
        return raw

    prefix = f"_h{site}"

    # return callee(...);
    m = re.match(r"return\s+", s)
    if m:
        call = _parse_call(s, m.end())
        if call is None:
            return raw
        name, args_src, _ = call
        if not s[m.end() :].lstrip().startswith(name):
            return raw
        callee = _lookup(index, fn.file, name)
        if callee is None or callee.name == fn.name or not _inlineable_callee(callee):
            return raw
        rest = s[call[2] :].strip()
        if rest != ";":
            return raw
        args = _split_args(args_src)
        block = _build_inline_block(
            callee,
            args,
            prefix=prefix,
            tail="return " + (f"{prefix}_ret" if _returns_value(callee) else "0"),
        )
        return block if block else raw

    # lhs = callee(...);
    m = re.match(r"([A-Za-z_]\w*)\s*=\s*", s)
    if m:
        lhs = m.group(1)
        call = _parse_call(s, m.end())
        if call is None:
            return raw
        name, args_src, end = call
        callee = _lookup(index, fn.file, name)
        if callee is None or callee.name == fn.name or not _inlineable_callee(callee):
            return raw
        if not _returns_value(callee):
            return raw
        rest = s[end:].strip()
        if rest != ";":
            return raw
        args = _split_args(args_src)
        block = _build_inline_block(
            callee,
            args,
            prefix=prefix,
            tail=f"{lhs} = {prefix}_ret",
        )
        return block if block else raw

    # type name = callee(...);
    m = re.match(
        rf"(?P<typ>{_SCALAR_TYPES})\s+(?P<name>[A-Za-z_]\w*)\s*=\s*",
        s,
    )
    if m:
        lhs = m.group("name")
        call = _parse_call(s, m.end())
        if call is None:
            return raw
        name, args_src, end = call
        callee = _lookup(index, fn.file, name)
        if callee is None or callee.name == fn.name or not _inlineable_callee(callee):
            return raw
        if not _returns_value(callee):
            return raw
        rest = s[end:].strip()
        if rest != ";":
            return raw
        args = _split_args(args_src)
        block = _build_inline_block(
            callee,
            args,
            prefix=prefix,
            tail=f"{lhs} = {prefix}_ret",
        )
        # The declaration stays in the caller's scope; the block assigns it.
        return f"{m.group('typ')} {lhs};\n{block}" if block else raw

    # callee(...);  void helper, no result use
    call = _parse_call(s, 0)
    if call is None:
        return raw
    name, args_src, end = call
    if s[end:].strip() != ";":
        return raw
    callee = _lookup(index, fn.file, name)
    if callee is None or callee.name == fn.name or not _inlineable_callee(callee):
        return raw
    if _returns_value(callee):
        return raw
    args = _split_args(args_src)
    block = _build_inline_block(callee, args, prefix=prefix, tail="")
    return block if block else raw


def _transform_body(
    body: str,
    fn: FunctionInfo,
    index: dict[tuple[str, str], FunctionInfo],
    counter: list[int],
) -> str:
    text = body
    out: list[str] = []
    while text:
        text = text.lstrip()
        if not text:
            break
        if text.startswith("{"):
            matched = _match_balanced(text, "{", "}")
            if matched is None:
                out.append(text)
                break
            inner, rest = matched
            out.append("{")
            out.append(_transform_body(inner, fn, index, counter))
            out.append("}")
            text = rest
            continue
        stmt, text = _take_stmt(text)
        if not stmt.strip():
            continue
        new_stmt = _try_inline_stmt(stmt, fn, index, counter[0])
        if new_stmt != stmt:
            counter[0] += 1
        out.append(new_stmt)
    return "".join(out)


def _inline_function(
    fn: FunctionInfo,
    index: dict[tuple[str, str], FunctionInfo],
) -> FunctionInfo:
    if fn.kind == "POINTER":
        return fn
    counter = [0]
    new_body = _transform_body(fn.body, fn, index, counter)
    if counter[0] == 0:
        return fn
    return replace(fn, body=new_body)


def inline_static(functions: list[FunctionInfo]) -> list[FunctionInfo]:
    """Inline static scalar callees (one level, same file) into caller bodies."""
    index = {(_basename(f.file), f.name): f for f in functions}
    return [_inline_function(fn, index) for fn in functions]
