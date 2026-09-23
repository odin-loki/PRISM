"""Concrete interpreter of the PRISM SCALAR C subset, with UB oracles.

Used as a crash oracle when the host gcc has no UBSan (Strawberry, MSVC).
POINTER functions are skipped: ub=None, not a crash.
"""

from __future__ import annotations

from dataclasses import dataclass
from functools import lru_cache
from pathlib import Path
from typing import Any
import re
import struct

from prism.bmc import (
    INT_MAX,
    INT_MIN,
    ParseFail,
    WIDTH,
    _CAST_WORDS,
    _block_or_stmt as _bmc_block_or_stmt,
    _brace as _bmc_brace,
    _char_lit_value,
    _consume_stmt_src as _bmc_consume_stmt_src,
    _is_computed_goto,
    _is_ident as _bmc_is_ident,
    _is_nested_function,
    _paren as _bmc_paren,
    _split_comma as _bmc_split_comma,
    _split_semi as _bmc_split_semi,
    _starts_kw,
    _stmt as _bmc_stmt,
    _string_lit_value,
    _tok as _bmc_tok,
    _type_is_unsigned as _bmc_type_is_unsigned,
    _type_width as _bmc_type_width,
    _upto_colon as _bmc_upto_colon,
    extract_enums,
    unencoded_layout_prefix as _bmc_unencoded_layout_prefix,
    unencoded_layout_stmt as _bmc_unencoded_layout_stmt,
)
from prism.models import FunctionInfo

# The interpreter re-lexes the same body text on every run and every loop
# iteration. These bmc helpers are pure functions of their string argument,
# so memoizing them changes no result (exceptions are never cached: a
# ParseFail is re-raised at the same point every time). Cached str results
# are the same objects each time, so their hashes are computed once.
_MEMO = 4096
_brace = lru_cache(maxsize=_MEMO)(_bmc_brace)
_paren = lru_cache(maxsize=_MEMO)(_bmc_paren)
_block_or_stmt = lru_cache(maxsize=_MEMO)(_bmc_block_or_stmt)
_stmt = lru_cache(maxsize=_MEMO)(_bmc_stmt)
_consume_stmt_src = lru_cache(maxsize=_MEMO)(_bmc_consume_stmt_src)
_upto_colon = lru_cache(maxsize=_MEMO)(_bmc_upto_colon)
_is_ident = lru_cache(maxsize=_MEMO)(_bmc_is_ident)
_type_is_unsigned = lru_cache(maxsize=256)(_bmc_type_is_unsigned)
_type_width = lru_cache(maxsize=256)(_bmc_type_width)
unencoded_layout_prefix = lru_cache(maxsize=_MEMO)(_bmc_unencoded_layout_prefix)
unencoded_layout_stmt = lru_cache(maxsize=_MEMO)(_bmc_unencoded_layout_stmt)


@lru_cache(maxsize=_MEMO)
def _split_semi(s: str) -> tuple[str, ...]:
    return tuple(_bmc_split_semi(s))


@lru_cache(maxsize=_MEMO)
def _split_comma(s: str) -> tuple[str, ...]:
    return tuple(_bmc_split_comma(s))


@lru_cache(maxsize=_MEMO)
def _tok(src: str) -> tuple[str, ...]:
    return tuple(_bmc_tok(src))


@lru_cache(maxsize=256)
def _enums_from_text(text: str) -> dict[str, int]:
    return extract_enums(text)


_PREP_RE = re.compile(r"#.*")
_DECL_RE = re.compile(
    r"(?:int|unsigned(?:\s+int)?|long|short|char|uint32_t|int32_t|size_t)"
    r"\s+([A-Za-z_]\w*)(?:\s*\[(\d+)\])?(?:\s*=\s*(.*))?$"
)
_VLA_RE = re.compile(r"\[[^\]]+\]")
_ASTORE_RE = re.compile(r"([A-Za-z_]\w*)\s*\[(.+)\]\s*=\s*(.+)$")
_ASSIGN_RE = re.compile(r"([A-Za-z_]\w*)\s*([+\-*/%|&^]?=)\s*(.+)$")
_ASSERT_RE = re.compile(r"assert\s*\((.*)\)\s*;", re.S)
_DECL_PREFIXES = (
    "int ", "unsigned ", "long ", "short ", "char ",
    "uint32_t ", "int32_t ", "size_t ",
)
_PREC = {
    "||": 10, "&&": 20,
    "|": 30, "^": 40, "&": 50,
    "==": 60, "!=": 60,
    "<": 70, ">": 70, "<=": 70, ">=": 70,
    "<<": 80, ">>": 80,
    "+": 90, "-": 90,
    "*": 100, "/": 100, "%": 100,
}

INT64_MIN = -(1 << 63)
INT64_MAX = (1 << 63) - 1
MAX_STEPS = 10_000


@dataclass
class ExecResult:
    """Outcome of a concrete run. `.ub` is a taxonomy id or None."""

    ub: str | None
    value: int | None = None
    error: str | None = None
    steps: int = 0


class _UB(Exception):
    def __init__(self, cls: str) -> None:
        self.cls = cls
        super().__init__(cls)


class _Return(Exception):
    def __init__(self, value: int | None) -> None:
        self.value = value


class _Break(Exception):
    pass


class _Continue(Exception):
    pass


_TYPE_SIZE = {
    "char": 1, "signed char": 1, "unsigned char": 1,
    "short": 2, "short int": 2, "signed short": 2, "unsigned short": 2,
    "int": 4, "signed": 4, "signed int": 4, "unsigned": 4, "unsigned int": 4,
    "long": 4, "long int": 4, "unsigned long": 4,
    "long long": 8, "long long int": 8, "unsigned long long": 8,
    "uint32_t": 4, "int32_t": 4, "size_t": 4,
    "_Bool": 1, "bool": 1,
}


def _sizeof_concrete(inner: list[str], st: _St) -> int:
    """Bytes for sizeof(type) / sizeof ident. Pointers are WIDTH/8."""
    if not inner:
        return WIDTH // 8
    if "*" in inner:
        return WIDTH // 8
    joined = " ".join(inner)
    if joined in _TYPE_SIZE:
        return _TYPE_SIZE[joined]
    if len(inner) == 1 and _is_ident(inner[0]):
        name = inner[0]
        if name in st.arrays:
            return len(st.arrays[name]) * (WIDTH // 8)
        return WIDTH // 8
    return WIDTH // 8


def i32(x: int) -> int:
    x = int(x) & 0xFFFFFFFF
    return x - 0x100000000 if x >= 0x80000000 else x


def u32(x: int) -> int:
    return int(x) & 0xFFFFFFFF


def _truth(v: int) -> bool:
    return i32(v) != 0


class _St:
    def __init__(self, params: list[tuple[str, str]], args: dict[str, int],
                 enums: dict[str, int]) -> None:
        self.vars: dict[str, int] = {}
        self.arrays: dict[str, list[int]] = {}
        self.enums = dict(enums)
        self.unsigned: set[str] = set()
        self.bits: dict[str, int] = {}
        self.steps = 0
        for typ, name in params:
            if not name:
                continue
            if _type_is_unsigned(typ):
                self.unsigned.add(name)
            w = _type_width(typ)
            self.bits[name] = w
            raw = int(args.get(name, 0))
            self.vars[name] = i32(raw) if w <= 32 else raw
        # bits never changes after this; lets _tree_width skip the walk.
        self.wide = any(w > WIDTH for w in self.bits.values())

    def tick(self) -> None:
        self.steps += 1
        if self.steps > MAX_STEPS:
            raise _Return(self.vars.get("__ret"))


def execute(fn: FunctionInfo, args: dict[str, int],
            enums: dict[str, int] | None = None) -> ExecResult:
    """Run `fn` on concrete 32-bit signed `args`. First UB wins."""
    if fn.kind == "POINTER":
        return ExecResult(ub=None, value=None, error="skip-pointer")
    if enums is None:
        enums = _enums_for(fn)
    st = _St(fn.params, args, enums)
    try:
        _Parser(fn.body or "", st).run()
    except _UB as u:
        return ExecResult(ub=u.cls, value=None, steps=st.steps)
    except _Return as r:
        return ExecResult(ub=None, value=None if r.value is None else i32(r.value),
                          steps=st.steps)
    except ParseFail as ex:
        return ExecResult(ub=None, value=None, error=str(ex), steps=st.steps)
    except _Break:
        return ExecResult(ub=None, value=st.vars.get("__ret"), steps=st.steps)
    except Exception as ex:  # pragma: no cover — keep oracle from crashing the fuzzer
        return ExecResult(ub=None, value=None, error=str(ex), steps=st.steps)
    return ExecResult(ub=None, value=st.vars.get("__ret"), steps=st.steps)


def decode_args(fn: FunctionInfo, data: bytes) -> dict[str, int]:
    """Little-endian ints matching `fn.params` (PRISM fuzzer harness layout)."""
    from prism.fuzz import C_TYPE_SIZE

    args: dict[str, int] = {}
    off = 0
    for typ, name in fn.params:
        if not name:
            continue
        key = " ".join(typ.replace("*", " ").split())
        sz = C_TYPE_SIZE.get(key, 4)
        chunk = data[off:off + sz] if off < len(data) else b""
        chunk = chunk.ljust(sz, b"\x00")
        unsigned = (
            key.startswith("uint") or key.startswith("unsigned")
            or key in {"size_t", "uintptr_t"}
        )
        if sz == 1:
            fmt = "B" if unsigned else "b"
        elif sz == 2:
            fmt = "H" if unsigned else "h"
        elif sz == 8:
            fmt = "Q" if unsigned else "q"
        else:
            fmt = "I" if unsigned else "i"
            chunk = chunk[:4].ljust(4, b"\x00")
        raw = struct.unpack("<" + fmt, chunk[: struct.calcsize("<" + fmt)])[0]
        args[name] = int(raw) if sz >= 8 else i32(int(raw))
        off += sz
    return args


def pack_args(fn: FunctionInfo, args: dict[str, int]) -> bytes:
    from prism.fuzz import C_TYPE_SIZE

    raw = b""
    for typ, name in fn.params:
        if not name:
            continue
        key = " ".join(typ.replace("*", " ").split())
        sz = C_TYPE_SIZE.get(key, 4)
        v = int(args.get(name, 0)) & ((1 << (sz * 8)) - 1)
        raw += v.to_bytes(sz, "little", signed=False)
    return raw


def interesting_seeds(fn: FunctionInfo) -> list[bytes]:
    """Bit patterns that hit the planted SCALAR bugs in a handful of runs."""
    from prism.fuzz import param_nbytes

    n = param_nbytes(fn.params)
    if n <= 0:
        return [b"\x00"]
    values = [
        0,
        -1,
        INT_MAX,
        INT_MIN,
        1,
        4,
        31,
        32,
        100,
        INT_MAX - 100,
        INT_MAX - 99,
    ]
    out: list[bytes] = []
    seen: set[bytes] = set()

    def add(b: bytes) -> None:
        b = b[:n].ljust(n, b"\x00")
        if b not in seen:
            seen.add(b)
            out.append(b)

    add(b"\x00" * n)
    add(b"\xff" * n)
    slots = [name for _, name in fn.params if name] or ["_"]
    for v in values:
        blob = b""
        for _ in slots:
            blob += struct.pack("<i", i32(v))
        add(blob)
    # second param zero (div_param), first param INT_MAX (add_overflow / oob)
    if len(slots) >= 2:
        imax = struct.pack("<i", INT_MAX)
        z = struct.pack("<i", 0)
        add(imax + z * (len(slots) - 1))
        add(z + imax + z * (len(slots) - 2))
        add(struct.pack("<i", 1) + z * (len(slots) - 1))
    return out


def _enums_for(fn: FunctionInfo) -> dict[str, int]:
    path = Path(fn.file) if fn.file else Path()
    if path.is_file():
        try:
            text = path.read_text(encoding="utf-8", errors="replace")
        except OSError:
            return {}
        return dict(_enums_from_text(text))
    return {}


# ----- statement / expression interpreter (same subset as prism.bmc) -----

_STMT_KWS = (
    "if", "switch", "do", "while", "for", "assert", "return", "break", "continue",
)


@lru_cache(maxsize=_MEMO)
def _prep(body: str) -> str:
    return _PREP_RE.sub(" ", body)


@lru_cache(maxsize=_MEMO)
def _assign_plan(stmt: str) -> tuple[Any, ...]:
    """How `_assign_or_expr` splits `stmt`; a pure function of the text.

    ("",) empty, (",", pieces) comma list, ("[]=", name, idx, rhs) array
    store, ("=", name, op, rhs) (compound) assignment, ("e", expr) expression.
    """
    stmt = stmt.rstrip(";").strip()
    if not stmt:
        return ("",)
    parts = _split_comma(stmt)
    if len(parts) > 1:
        return (",", tuple(p.strip() for p in parts if p.strip()))
    m = _ASTORE_RE.match(stmt)
    if m:
        return ("[]=", m.group(1), m.group(2), m.group(3))
    m = _ASSIGN_RE.match(stmt)
    if m:
        return ("=", m.group(1), m.group(2), m.group(3))
    return ("e", stmt)


@lru_cache(maxsize=_MEMO)
def _stmt_kind(text: str) -> str:
    """Which `_stmts` branch `text` takes: "{", a keyword, a ParseFail
    message, or "" for a plain statement. Pure in `text`; checked in the
    same order as the original if-chain, so the first match still wins.
    """
    if text.startswith("{"):
        return "{"
    for kw in _STMT_KWS:
        if _starts_kw(text, kw):
            return kw
    if _is_nested_function(text):
        return "nested function unencoded"
    if _starts_kw(text, "goto"):
        if _is_computed_goto(text):
            return "computed goto unencoded"
        return "goto unencoded"
    if _starts_kw(text, "throw"):
        return "throw unencoded"
    if (_starts_kw(text, "asm") or _starts_kw(text, "__asm__")
            or _starts_kw(text, "__asm")):
        return "asm unencoded"
    if _starts_kw(text, "try") or _starts_kw(text, "catch"):
        return "try unencoded"
    if _starts_kw(text, "case") or _starts_kw(text, "default"):
        return "case/default outside switch"
    return ""


class _Parser:
    def __init__(self, body: str, st: _St) -> None:
        self.body = body
        self.st = st

    def run(self) -> None:
        self._stmts(self._prep(self.body))

    def _prep(self, body: str) -> str:
        return _prep(body)

    def _stmts(self, text: str) -> None:
        text = text.strip()
        while text:
            self.st.tick()
            text = text.lstrip()
            if not text:
                break
            kind = _stmt_kind(text)
            if kind == "{":
                inner, rest = _brace(text)
                self._stmts(inner)
                text = rest
                continue
            if kind == "if":
                text = self._if(text)
                continue
            if kind == "switch":
                text = self._switch(text)
                continue
            if kind == "do":
                text = self._do(text)
                continue
            if kind == "while":
                text = self._while(text)
                continue
            if kind == "for":
                text = self._for(text)
                continue
            if kind == "assert":
                text = self._assert(text)
                continue
            if kind == "return":
                stmt, text = _stmt(text)
                self._return(stmt)
                continue
            if kind == "break":
                _, text = _stmt(text)
                raise _Break()
            if kind == "continue":
                _, text = _stmt(text)
                raise _Continue()
            if kind:
                raise ParseFail(kind)
            miss = unencoded_layout_prefix(text)
            if miss:
                raise ParseFail(miss)
            stmt, text = _stmt(text)
            miss = unencoded_layout_stmt(stmt)
            if miss:
                raise ParseFail(miss)
            if stmt.startswith(_DECL_PREFIXES):
                self._decl(stmt)
            else:
                self._assign_or_expr(stmt)

    def _decl(self, stmt: str) -> None:
        stmt = stmt.rstrip(";").strip()
        m = _DECL_RE.match(stmt)
        if not m:
            if _VLA_RE.search(stmt):
                raise ParseFail("VLA unencoded")
            raise ParseFail(f"unparsed decl: {stmt[:80]}")
        name, dim, init = m.group(1), m.group(2), m.group(3)
        if dim:
            self.st.arrays[name] = [0] * int(dim)
            return
        if _type_is_unsigned(stmt[: m.start(1)]):
            self.st.unsigned.add(name)
        if init is not None:
            self.st.vars[name] = i32(self._eval(init))
        else:
            self.st.vars[name] = 0

    def _assign_or_expr(self, stmt: str) -> None:
        plan = _assign_plan(stmt)
        kind = plan[0]
        if kind == "":
            return
        if kind == ",":
            for piece in plan[1]:
                self._assign_or_expr(piece)
            return
        if kind == "[]=":
            self._astore(plan[1], plan[2], plan[3])
            return
        if kind == "=":
            name, op, rhs = plan[1], plan[2], plan[3]
            val = self._eval(rhs)
            if op == "=":
                w = self.st.bits.get(name, WIDTH)
                self.st.vars[name] = i32(val) if w <= 32 else int(val)
            else:
                cur = self.st.vars.get(name, 0)
                w = self.st.bits.get(name, WIDTH)
                r = _binop(cur, op[0], val, name in self.st.unsigned, w)
                self.st.vars[name] = i32(r) if w <= 32 else int(r)
            return
        self._eval(plan[1])

    def _astore(self, name: str, idx: str, rhs: str) -> None:
        if name not in self.st.arrays:
            raise ParseFail(f"unknown array {name}")
        arr = self.st.arrays[name]
        i = self._eval(idx)
        v = self._eval(rhs)
        unsigned_idx = (
            _is_ident(idx.strip()) and idx.strip() in self.st.unsigned
        )
        if unsigned_idx:
            ui = u32(i)
            if ui >= len(arr):
                raise _UB("MEM-OOB-WRITE")
            arr[ui] = i32(v)
            return
        if i < 0 or i >= len(arr):
            raise _UB("MEM-OOB-WRITE")
        arr[i] = i32(v)

    def _assert(self, text: str) -> str:
        m = _ASSERT_RE.match(text)
        if not m:
            from prism.bmc import _paren_stmt
            inner, rest = _paren_stmt(text[text.find("("):])
            if not _truth(self._eval(inner)):
                raise _UB("FUNC-CONTRACT")
            return rest
        if not _truth(self._eval(m.group(1))):
            raise _UB("FUNC-CONTRACT")
        return text[m.end():]

    def _return(self, stmt: str) -> None:
        rest = stmt.strip()
        if rest.lower().startswith("return"):
            rest = rest[6:].strip()
        if rest.endswith(";"):
            rest = rest[:-1].strip()
        val = None
        if rest:
            val = self._eval(rest)
        raise _Return(val)

    def _if(self, text: str) -> str:
        rest = text[2:].lstrip()
        cond_src, rest = _paren(rest)
        then_src, rest = _block_or_stmt(rest)
        else_src = None
        rest2 = rest.lstrip()
        if rest2.startswith("else"):
            else_src, rest = _block_or_stmt(rest2[4:])
        if _truth(self._eval(cond_src)):
            self._stmts(then_src)
        elif else_src:
            self._stmts(else_src)
        return rest

    def _run_loop_body(self, body: str) -> bool:
        """Run loop body. False means break out of the loop."""
        try:
            self._stmts(body)
        except _Break:
            return False
        except _Continue:
            pass
        return True

    def _while(self, text: str) -> str:
        rest = text[5:].lstrip()
        cond_src, rest = _paren(rest)
        body, rest = _block_or_stmt(rest)
        while _truth(self._eval(cond_src)):
            self.st.tick()
            if not self._run_loop_body(body):
                break
        return rest

    def _do(self, text: str) -> str:
        rest = text[2:].lstrip()
        body, rest = _block_or_stmt(rest)
        rest = rest.lstrip()
        if not _starts_kw(rest, "while"):
            raise ParseFail("do without while")
        rest = rest[5:].lstrip()
        cond_src, rest = _paren(rest)
        rest = rest.lstrip()
        if rest.startswith(";"):
            rest = rest[1:]
        if not self._run_loop_body(body):
            return rest
        while _truth(self._eval(cond_src)):
            self.st.tick()
            if not self._run_loop_body(body):
                break
        return rest

    def _for(self, text: str) -> str:
        rest = text[3:].lstrip()
        head, rest = _paren(rest)
        parts = [p.strip() for p in _split_semi(head)]
        while len(parts) < 3:
            parts.append("")
        init, cond_src, incr = parts[0], parts[1], parts[2]
        if init:
            init_stmt = init if init.endswith(";") else init + ";"
            miss = unencoded_layout_stmt(init_stmt)
            if miss:
                raise ParseFail(miss)
            self._assign_or_expr(init_stmt)
        body, rest = _block_or_stmt(rest)
        cond_src = cond_src or "1"
        while _truth(self._eval(cond_src)):
            self.st.tick()
            if not self._run_loop_body(body):
                break
            if incr:
                self._assign_or_expr(incr if incr.endswith(";") else incr + ";")
        return rest

    def _switch(self, text: str) -> str:
        rest = text[6:].lstrip()
        cond_src, rest = _paren(rest)
        body, rest = _block_or_stmt(rest)
        scrut = self._eval(cond_src)
        arms = self._parse_switch_arms(body)
        if not arms:
            return rest
        idx = None
        default_i = None
        for i, (labels, _code, _stops) in enumerate(arms):
            for lab in labels:
                if lab is None:
                    default_i = i
                elif lab == scrut:
                    idx = i
                    break
            if idx is not None:
                break
        if idx is None:
            idx = default_i
        if idx is None:
            return rest
        try:
            for j in range(idx, len(arms)):
                _labs, code, stops = arms[j]
                if code.strip():
                    self._stmts(code)
                if stops:
                    break
        except _Break:
            pass
        return rest

    def _parse_switch_arms(self, body: str) -> list[tuple[list[Any], str, bool]]:
        arms: list[tuple[list[Any], str, bool]] = []
        labels: list[Any] = []
        chunks: list[str] = []
        stops = False
        text = body

        def flush() -> None:
            nonlocal labels, chunks, stops
            if labels or chunks:
                arms.append((labels, "\n".join(chunks), stops))
            labels, chunks, stops = [], [], False

        while text:
            text = text.lstrip()
            if not text:
                break
            if _starts_kw(text, "case"):
                if chunks or stops:
                    flush()
                rest = text[4:].lstrip()
                src, text = _upto_colon(rest)
                labels.append(self._eval(src))
                continue
            if _starts_kw(text, "default"):
                if chunks or stops:
                    flush()
                rest = text[7:].lstrip()
                if not rest.startswith(":"):
                    raise ParseFail("expected : after default")
                labels.append(None)
                text = rest[1:]
                continue
            if _starts_kw(text, "break"):
                _, text = _stmt(text)
                stops = True
                continue
            src, text = _consume_stmt_src(text)
            if stops:
                continue
            if src.strip():
                chunks.append(src.strip())
        flush()
        return arms

    def _eval(self, src: str) -> int:
        tree = self._parse_expr(src)
        v = _eval_tree(tree, self.st)
        if _tree_width(tree, self.st) >= 64:
            return int(v)
        return i32(v)

    def _binop(self, a: int, op: str, b: int, unsigned: bool = False,
               width: int = WIDTH) -> int:
        return _binop(a, op, b, unsigned, width)

    def _parse_expr(self, src: str) -> Any:
        tokens = _tok(src.strip())
        if "sizeof" in tokens:
            # sizeof(arr) reads the live array table: never cached.
            return _parse_tokens(tokens, self.st)
        return _parse_expr_pure(src)


def _parse_tokens(tokens: tuple[str, ...], st: _St | None) -> Any:
    """Pratt parse of one expression. `st` is only read by `sizeof`."""
    pos = 0

    def peek() -> str:
        return tokens[pos] if pos < len(tokens) else ""

    def eat(t: str | None = None) -> str:
        nonlocal pos
        if pos >= len(tokens):
            raise ParseFail("unexpected end")
        got = tokens[pos]
        if t is not None and got != t:
            raise ParseFail(f"expected {t} got {got}")
        pos += 1
        return got

    PREC = _PREC

    def nud() -> Any:
        t = eat()
        if t == "sizeof":
            if peek() == "(":
                eat("(")
                inner: list[str] = []
                depth = 1
                while depth:
                    ntok = eat()
                    if ntok == "(":
                        depth += 1
                        inner.append(ntok)
                    elif ntok == ")":
                        depth -= 1
                        if depth:
                            inner.append(ntok)
                    else:
                        inner.append(ntok)
                return ("num", _sizeof_concrete(inner, _need_st(st)))
            name = eat()
            return ("num", _sizeof_concrete([name], _need_st(st)))
        if t == "(":
            if peek() == "{":
                raise ParseFail("statement-expr unencoded")
            if peek() in _CAST_WORDS:
                while peek() and peek() != ")":
                    if peek() not in _CAST_WORDS and peek() != "*":
                        break
                    eat()
                eat(")")
                return parse(110)
            v = parse(0)
            eat(")")
            return v
        if t in ("-", "!", "~"):
            return ("un", t, parse(110))
        if t in ("++", "--"):
            name = eat()
            if not _is_ident(name):
                raise ParseFail(f"bad token {name}")
            return ("pre", t, name)
        if len(t) >= 3 and t.startswith("'") and t.endswith("'"):
            return ("num", _char_lit_value(t))
        if len(t) >= 2 and t.startswith('"') and t.endswith('"'):
            return ("str", _string_lit_value(t))
        if t.isdigit() or t.startswith("0x") or t.startswith("0X"):
            return ("num", int(t, 0))
        if _is_ident(t):
            if t in ("_Generic", "offsetof"):
                raise ParseFail(f"{t} unencoded")
            if peek() == "[":
                eat("[")
                idx = parse(0)
                eat("]")
                return ("idx", t, idx)
            if peek() == "(":
                eat("(")
                args: list[Any] = []
                if peek() != ")":
                    # minp 2: above comma, so `f(a, b)` is two args.
                    args.append(parse(2))
                    while peek() == ",":
                        eat(",")
                        args.append(parse(2))
                eat(")")
                return ("call", t, args)
            if peek() in ("++", "--"):
                op = eat()
                return ("post", op, t)
            return ("id", t)
        raise ParseFail(f"bad token {t}")

    def parse(minp: int) -> Any:
        left = nud()
        while peek() in PREC and PREC[peek()] >= minp:
            op = eat()
            right = parse(PREC[op] + 1)
            left = ("bin", op, left, right)
        if minp <= 5 and peek() == "?":
            eat("?")
            then_t = parse(0)
            eat(":")
            else_t = parse(5)
            left = ("tern", left, then_t, else_t)
        if minp <= 1 and peek() == ",":
            eat(",")
            right = parse(0)
            left = ("comma", left, right)
        return left

    tree = parse(0)
    if pos != len(tokens):
        raise ParseFail(f"trailing {list(tokens[pos:])}")
    return tree


def _need_st(st: _St | None) -> _St:
    if st is None:  # pragma: no cover - sizeof trees never reach the cache
        raise ParseFail("sizeof needs state")
    return st


@lru_cache(maxsize=_MEMO)
def _parse_expr_pure(src: str) -> Any:
    """Parse tree of a sizeof-free expression; a pure function of `src`."""
    return _parse_tokens(_tok(src.strip()), None)


def _eval_cstr_copy(
    st: _St, dest_tree: Any, src_tree: Any, n: int | None, cat: bool,
) -> None:
    """strcpy/strncpy into a local array. Overflow is MEM-OOB-WRITE."""
    if not isinstance(dest_tree, tuple) or dest_tree[0] != "id":
        _eval_tree(dest_tree, st)
        _eval_tree(src_tree, st)
        return
    name = dest_tree[1]
    if name not in st.arrays:
        _eval_tree(src_tree, st)
        return
    arr = st.arrays[name]
    if not isinstance(src_tree, tuple) or src_tree[0] != "str":
        _eval_tree(src_tree, st)
        return
    src = src_tree[1]
    start = 0
    if cat:
        while start < len(arr) and arr[start]:
            start += 1
        if start >= len(arr):
            raise _UB("MEM-OOB-WRITE")
    if n is None:
        payload = [ord(c) & 0xFF for c in src] + [0]
    else:
        chars = src[:n]
        payload = [ord(c) & 0xFF for c in chars]
        if len(payload) < n:
            payload.extend([0] * (n - len(payload)))
    for i, b in enumerate(payload):
        idx = start + i
        if idx >= len(arr):
            raise _UB("MEM-OOB-WRITE")
        arr[idx] = b


def _eval_tree(tree: Any, st: _St) -> int:
    if not isinstance(tree, tuple):
        return i32(tree)
    kind = tree[0]
    if kind == "num":
        return i32(tree[1])
    if kind == "id":
        name = tree[1]
        if name in st.vars:
            w = st.bits.get(name, WIDTH)
            return i32(st.vars[name]) if w <= 32 else int(st.vars[name])
        if name in st.enums:
            return i32(st.enums[name])
        st.vars[name] = 0
        return 0
    if kind == "idx":
        name, idx_t = tree[1], tree[2]
        if name not in st.arrays:
            raise ParseFail(f"unknown array {name}")
        arr = st.arrays[name]
        idx = _eval_tree(idx_t, st)
        if _tree_unsigned(idx_t, st):
            ui = u32(idx)
            if ui >= len(arr):
                raise _UB("MEM-OOB-READ")
            return i32(arr[ui])
        if idx < 0 or idx >= len(arr):
            raise _UB("MEM-OOB-READ")
        return i32(arr[idx])
    if kind == "un":
        op, inner = tree[1], tree[2]
        v = _eval_tree(inner, st)
        if op == "-":
            if not _tree_unsigned(inner, st) and v == INT_MIN:
                raise _UB("INT-SIGNED-OVF")
            return i32(-v)
        if op == "!":
            return 0 if _truth(v) else 1
        if op == "~":
            return i32(~(v & 0xFFFFFFFF))
        raise ParseFail(f"unop {op}")
    if kind in ("post", "pre"):
        op, name = tree[1], tree[2]
        cur = st.vars.get(name, 0)
        w = st.bits.get(name, WIDTH)
        u = name in st.unsigned
        new = _binop(cur, "+" if op == "++" else "-", 1, u, w)
        st.vars[name] = new
        v = cur if kind == "post" else new
        return i32(v) if w <= 32 else int(v)
    if kind == "str":
        return 1
    if kind == "call":
        name, args = tree[1], tree[2]
        if name in {"strcpy", "strcat"} and len(args) >= 2:
            _eval_cstr_copy(st, args[0], args[1], n=None, cat=(name == "strcat"))
            return 0
        if name == "strncpy" and len(args) >= 3:
            n = _eval_tree(args[2], st)
            _eval_cstr_copy(st, args[0], args[1], n=max(0, int(n)), cat=False)
            return 0
        for a in args:
            _eval_tree(a, st)
        return 0
    if kind == "tern":
        cond, then_t, else_t = tree[1], tree[2], tree[3]
        if _truth(_eval_tree(cond, st)):
            return _eval_tree(then_t, st)
        return _eval_tree(else_t, st)
    if kind == "comma":
        _eval_tree(tree[1], st)
        return _eval_tree(tree[2], st)
    if kind == "bin":
        op, lhs, rhs = tree[1], tree[2], tree[3]
        if op == "&&":
            lv = _eval_tree(lhs, st)
            if not _truth(lv):
                return 0
            return 1 if _truth(_eval_tree(rhs, st)) else 0
        if op == "||":
            lv = _eval_tree(lhs, st)
            if _truth(lv):
                return 1
            return 1 if _truth(_eval_tree(rhs, st)) else 0
        a = _eval_tree(lhs, st)
        b = _eval_tree(rhs, st)
        u = _tree_unsigned(lhs, st) or _tree_unsigned(rhs, st)
        w = max(_tree_width(lhs, st), _tree_width(rhs, st))
        return _binop(a, op, b, u, w)
    raise ParseFail(f"bad tree {tree[:1]}")


def _tree_unsigned(tree: Any, st: _St) -> bool:
    if not st.unsigned:
        return False  # nothing unsigned: every branch below is False
    if not isinstance(tree, tuple):
        return False
    kind = tree[0]
    if kind == "id":
        return tree[1] in st.unsigned
    if kind == "bin":
        return _tree_unsigned(tree[2], st) or _tree_unsigned(tree[3], st)
    if kind == "un":
        return _tree_unsigned(tree[2], st)
    if kind == "tern":
        return _tree_unsigned(tree[2], st) or _tree_unsigned(tree[3], st)
    if kind == "comma":
        return _tree_unsigned(tree[2], st)
    return False


def _tree_width(tree: Any, st: _St) -> int:
    if not st.wide:
        return WIDTH  # every width below is st.bits (all WIDTH) or WIDTH
    if not isinstance(tree, tuple):
        return WIDTH
    kind = tree[0]
    if kind == "id":
        return st.bits.get(tree[1], WIDTH)
    if kind in ("post", "pre"):
        return st.bits.get(tree[2], WIDTH)
    if kind == "bin":
        return max(_tree_width(tree[2], st), _tree_width(tree[3], st))
    if kind == "un":
        return _tree_width(tree[2], st)
    if kind == "tern":
        return max(_tree_width(tree[2], st), _tree_width(tree[3], st))
    if kind == "comma":
        return _tree_width(tree[2], st)
    return WIDTH


def _tdiv(a: int, b: int) -> int:
    """C99 integer division (truncates toward zero), exact at any width.

    `int(a / b)` goes through a double and is wrong once the quotient
    exceeds 2**53 (64-bit operands); the C++ engine divides exactly.
    """
    q = abs(a) // abs(b)
    return q if (a < 0) == (b < 0) else -q


def _binop(a: int, op: str, b: int, unsigned: bool = False, width: int = WIDTH) -> int:
    if unsigned:
        ua, ub = u32(a), u32(b)
        if op == "+":
            return i32(ua + ub)
        if op == "-":
            return i32(ua - ub)
        if op == "*":
            return i32(ua * ub)
        if op == "/":
            if ub == 0:
                raise _UB("INT-DIV-ZERO")
            return i32(ua // ub)
        if op == "%":
            if ub == 0:
                raise _UB("INT-DIV-ZERO")
            return i32(ua % ub)
        if op == "<<":
            if ub >= WIDTH:
                raise _UB("INT-SHIFT-UB")
            return i32(ua << ub)
        if op == ">>":
            if ub >= WIDTH:
                raise _UB("INT-SHIFT-UB")
            return i32(ua >> ub)
        if op == "&":
            return i32(ua & ub)
        if op == "|":
            return i32(ua | ub)
        if op == "^":
            return i32(ua ^ ub)
        if op == "==":
            return 1 if ua == ub else 0
        if op == "!=":
            return 1 if ua != ub else 0
        if op == "<":
            return 1 if ua < ub else 0
        if op == ">":
            return 1 if ua > ub else 0
        if op == "<=":
            return 1 if ua <= ub else 0
        if op == ">=":
            return 1 if ua >= ub else 0
        raise ParseFail(f"op {op}")
    if width >= 64:
        a, b = int(a), int(b)
        lo, hi = INT64_MIN, INT64_MAX
        if op == "+":
            r = a + b
            if r < lo or r > hi:
                raise _UB("INT-SIGNED-OVF")
            return r
        if op == "-":
            r = a - b
            if r < lo or r > hi:
                raise _UB("INT-SIGNED-OVF")
            return r
        if op == "*":
            r = a * b
            if r < lo or r > hi:
                raise _UB("INT-SIGNED-OVF")
            return r
        if op == "/":
            if b == 0:
                raise _UB("INT-DIV-ZERO")
            if a == lo and b == -1:
                raise _UB("INT-SIGNED-OVF")
            return _tdiv(a, b)
        if op == "%":
            if b == 0:
                raise _UB("INT-DIV-ZERO")
            if a == lo and b == -1:
                raise _UB("INT-SIGNED-OVF")  # C11 6.5.5p6
            q = _tdiv(a, b)
            return a - q * b
        if op == "<<":
            if b < 0 or b >= 64:
                raise _UB("INT-SHIFT-UB")
            # Negative operand or unrepresentable result (C11 6.5.7p4).
            if a < 0 or a > (hi >> b):
                raise _UB("INT-SHIFT-UB")
            return a << b
        if op == ">>":
            if b < 0 or b >= 64:
                raise _UB("INT-SHIFT-UB")
            return a >> b
        if op == "&":
            return a & b
        if op == "|":
            return a | b
        if op == "^":
            return a ^ b
        if op == "==":
            return 1 if a == b else 0
        if op == "!=":
            return 1 if a != b else 0
        if op == "<":
            return 1 if a < b else 0
        if op == ">":
            return 1 if a > b else 0
        if op == "<=":
            return 1 if a <= b else 0
        if op == ">=":
            return 1 if a >= b else 0
        raise ParseFail(f"op {op}")
    a, b = i32(a), i32(b)
    if unsigned:
        ua, ub = u32(a), u32(b)
        if op == "+":
            return i32(ua + ub)
        if op == "-":
            return i32(ua - ub)
        if op == "*":
            return i32(ua * ub)
        if op == "/":
            if ub == 0:
                raise _UB("INT-DIV-ZERO")
            return i32(ua // ub)
        if op == "%":
            if ub == 0:
                raise _UB("INT-DIV-ZERO")
            return i32(ua % ub)
        if op == "<<":
            if ub >= WIDTH:
                raise _UB("INT-SHIFT-UB")
            return i32(ua << ub)
        if op == ">>":
            if ub >= WIDTH:
                raise _UB("INT-SHIFT-UB")
            return i32(ua >> ub)
        if op == "&":
            return i32(ua & ub)
        if op == "|":
            return i32(ua | ub)
        if op == "^":
            return i32(ua ^ ub)
        if op == "==":
            return 1 if ua == ub else 0
        if op == "!=":
            return 1 if ua != ub else 0
        if op == "<":
            return 1 if ua < ub else 0
        if op == ">":
            return 1 if ua > ub else 0
        if op == "<=":
            return 1 if ua <= ub else 0
        if op == ">=":
            return 1 if ua >= ub else 0
        raise ParseFail(f"op {op}")
    a, b = i32(a), i32(b)
    if op == "+":
        r = a + b
        if r < INT_MIN or r > INT_MAX:
            raise _UB("INT-SIGNED-OVF")
        return r
    if op == "-":
        r = a - b
        if r < INT_MIN or r > INT_MAX:
            raise _UB("INT-SIGNED-OVF")
        return r
    if op == "*":
        r = a * b
        if r < INT_MIN or r > INT_MAX:
            raise _UB("INT-SIGNED-OVF")
        return r
    if op == "/":
        if b == 0:
            raise _UB("INT-DIV-ZERO")
        if a == INT_MIN and b == -1:
            raise _UB("INT-SIGNED-OVF")
        return _tdiv(a, b)  # C99 toward zero
    if op == "%":
        if b == 0:
            raise _UB("INT-DIV-ZERO")
        if a == INT_MIN and b == -1:
            raise _UB("INT-SIGNED-OVF")  # C11 6.5.5p6
        q = _tdiv(a, b)
        return a - q * b
    if op == "<<":
        if b < 0 or b >= WIDTH:
            raise _UB("INT-SHIFT-UB")
        # Negative operand or unrepresentable result (C11 6.5.7p4).
        if a < 0 or a > (INT_MAX >> b):
            raise _UB("INT-SHIFT-UB")
        return i32(a << b)
    if op == ">>":
        if b < 0 or b >= WIDTH:
            raise _UB("INT-SHIFT-UB")
        return i32(a >> b)
    if op == "&":
        return i32(a & b)
    if op == "|":
        return i32(a | b)
    if op == "^":
        return i32(a ^ b)
    if op == "==":
        return 1 if a == b else 0
    if op == "!=":
        return 1 if a != b else 0
    if op == "<":
        return 1 if a < b else 0
    if op == ">":
        return 1 if a > b else 0
    if op == "<=":
        return 1 if a <= b else 0
    if op == ">=":
        return 1 if a >= b else 0
    raise ParseFail(f"op {op}")
