"""Path-sensitive interval analysis on the SCALAR C subset.

Over-approximation: a hit is FAILED / FINDS, never a proof. A function
whose ranges stay inside defined integer behaviour is silent — silence
is not PROVED. POINTER / local heap is skipped (BMC already says
NEEDS-HARNESS). Parse failure / unencoded syntax is skipped
(BMC already says ERROR or NEEDS-HARNESS).
"""

from __future__ import annotations

from dataclasses import dataclass
import re

from prism import laws
from prism.bmc import (
    INT_MAX,
    INT_MIN,
    ParseFail,
    WIDTH,
    _CAST_WORDS,
    _brace,
    _char_lit_value,
    _is_computed_goto,
    _is_ident,
    _is_nested_function,
    _looks_like_decl,
    _paren,
    _split_comma,
    _starts_kw,
    _stmt,
    _tok,
    _type_is_unsigned,
    unencoded_layout_prefix,
    unencoded_layout_stmt,
    unencoded_syntax_reason,
)
from prism.cparse import body_needs_pointer_harness
from prism.models import Finding, FunctionInfo


@dataclass
class R:
    lo: int
    hi: int
    unsigned: bool = False

    def empty(self) -> bool:
        return self.lo > self.hi

    def contains(self, v: int) -> bool:
        return self.lo <= v <= self.hi


TOP = R(INT_MIN, INT_MAX)
BOT = R(1, 0)
_STR_LIT = re.compile(r'"([^"\\]|\\.)*"')
_CALL_DUMMY = R(1, 1)


def _interval_tok(src: str) -> list[str]:
    """Tokenize after replacing string literals (bmc._tok drops them)."""
    return _tok(_STR_LIT.sub("1", src or "0"))


def _clip(lo: int, hi: int, unsigned: bool = False) -> R:
    return R(max(lo, INT_MIN), min(hi, INT_MAX), unsigned)


def _join(a: R, b: R) -> R:
    if a.empty():
        return b
    if b.empty():
        return a
    return R(min(a.lo, b.lo), max(a.hi, b.hi), a.unsigned and b.unsigned)


def _meet(a: R, b: R) -> R:
    if a.empty() or b.empty():
        return BOT
    return R(max(a.lo, b.lo), min(a.hi, b.hi), a.unsigned or b.unsigned)


def _copy(st: dict[str, R]) -> dict[str, R]:
    return {k: R(v.lo, v.hi, v.unsigned) for k, v in st.items()}


def _join_state(a: dict[str, R], b: dict[str, R]) -> dict[str, R]:
    out = _copy(a)
    for k, v in b.items():
        out[k] = _join(out[k], v) if k in out else v
    for k, v in a.items():
        if k not in b:
            out[k] = _join(v, TOP)
    return out


class _Alarm(Exception):
    def __init__(self, cls: str, msg: str) -> None:
        self.cls = cls
        self.msg = msg
        super().__init__(msg)


class _Return(Exception):
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


def _sizeof_interval(inner: list[str]) -> int:
    if not inner:
        return WIDTH // 8
    if "*" in inner:
        return WIDTH // 8
    joined = " ".join(inner)
    if joined in _TYPE_SIZE:
        return _TYPE_SIZE[joined]
    return WIDTH // 8


_FLOAT_TYPE = re.compile(r"\b(?:float|double|_Float\d+|__float128)\b")
_FLOAT_DECL = re.compile(r"\b(?:float|double|_Float\d+|__float128)\b([^;(){}]*);")
_DECL_NAME = re.compile(r"^\s*\**\s*([A-Za-z_]\w*)")


def _float_locals(body: str) -> set[str]:
    """`double a = 1, *b, c[4];`: the first identifier of each comma segment."""
    out: set[str] = set()
    for m in _FLOAT_DECL.finditer(body):
        depth = 0
        seg = ""
        for c in m.group(1) + ",":
            if c in "[(":
                depth += 1
            elif c in "])":
                depth -= 1
            if c == "," and depth == 0:
                nm = _DECL_NAME.search(seg)
                if nm:
                    out.add(nm.group(1))
                seg = ""
            else:
                seg += c
    return out


class _Engine:
    def __init__(self, params: list[tuple[str, str]]) -> None:
        self.st: dict[str, R] = {}
        self.unsigned: set[str] = set()
        # A float/double parameter or local is outside the integer domain:
        # an expression that reads one is unencoded, never an overflow alarm.
        self.floats: set[str] = set()
        for typ, name in params:
            if not name:
                continue
            if _FLOAT_TYPE.search(typ):
                self.floats.add(name)
                continue
            u = _type_is_unsigned(typ)
            if u:
                self.unsigned.add(name)
            self.st[name] = R(INT_MIN, INT_MAX, u)
        self.live = True

    def get(self, name: str) -> R:
        if name in self.floats:
            raise ParseFail("floating-point unencoded")
        cur = self.st.get(name, TOP)
        if name in self.unsigned:
            return R(cur.lo, cur.hi, True)
        return cur

    def set(self, name: str, r: R) -> None:
        if name in self.unsigned:
            r = R(r.lo, r.hi, True)
        self.st[name] = r


def _fork(e: _Engine) -> _Engine:
    n = _Engine([])
    n.st = _copy(e.st)
    n.unsigned = set(e.unsigned)
    n.floats = set(e.floats)
    n.live = e.live
    return n


def interval_function(fn: FunctionInfo) -> Finding | None:
    """One finding if the over-approx sees integer UB; else None."""
    if fn.kind == "POINTER":
        return None
    if fn.kind == "OTHER":
        return None
    if body_needs_pointer_harness(fn.body):
        return None
    # unencoded_syntax_reason is the costly gate (hundreds of regexes per
    # body) and only matters when the engine alarms: every other outcome
    # is None either way. So run the cheap engine first and consult the
    # gate only for an alarm, or for an unexpected error (where the gate
    # used to run first and may have skipped the function).
    eng = _Engine(fn.params)
    eng.floats |= _float_locals(fn.body or "")
    try:
        _stmts(eng, fn.body or "")
    except _Alarm as a:
        if unencoded_syntax_reason(fn, "interval"):
            return None
        return Finding(
            stage="interval", file=fn.file, function=fn.name, line=fn.line,
            strength=laws.STRENGTH_FINDS, extra={"oracle": "interval"},
            status=laws.FAILED, cls=a.cls, message=f"interval: {a.msg}",
        )
    except (ParseFail, _Return, ValueError):
        return None
    except Exception:
        if unencoded_syntax_reason(fn, "interval"):
            return None
        raise
    return None


def run_interval(functions: list[FunctionInfo]) -> list[Finding]:
    out: list[Finding] = []
    for fn in functions:
        rec = interval_function(fn)
        if rec is not None:
            out.append(rec)
    return out


def _stmts(e: _Engine, text: str, bound: int = 8) -> None:
    text = (text or "").strip()
    while text and e.live:
        text = text.lstrip()
        if not text:
            break
        if text.startswith("{"):
            inner, rest = _brace(text)
            _stmts(e, inner, bound)
            text = rest
            continue
        if _starts_kw(text, "if"):
            text = _if(e, text, bound)
            continue
        if _starts_kw(text, "while"):
            text = _while(e, text, bound)
            continue
        if _starts_kw(text, "for"):
            text = _for(e, text, bound)
            continue
        if _starts_kw(text, "do"):
            text = _do(e, text, bound)
            continue
        if _starts_kw(text, "switch"):
            raise ParseFail("switch")
        if _starts_kw(text, "return"):
            stmt, text = _stmt(text)
            expr = stmt[len("return"):].rstrip(";").strip()
            if expr:
                _eval(e, expr)
            raise _Return()
        if _starts_kw(text, "break") or _starts_kw(text, "continue"):
            _, text = _stmt(text)
            e.live = False
            return
        if _starts_kw(text, "assert"):
            stmt, text = _stmt(text)
            inner = stmt[stmt.find("(") + 1: stmt.rfind(")")]
            _eval(e, inner)
            continue
        if _is_nested_function(text):
            raise ParseFail("nested function unencoded")
        if _starts_kw(text, "goto"):
            if _is_computed_goto(text):
                raise ParseFail("computed goto unencoded")
            raise ParseFail("goto unencoded")
        if _starts_kw(text, "throw"):
            raise ParseFail("throw unencoded")
        if (_starts_kw(text, "asm") or _starts_kw(text, "__asm__")
                or _starts_kw(text, "__asm")):
            raise ParseFail("asm unencoded")
        if _starts_kw(text, "try") or _starts_kw(text, "catch"):
            raise ParseFail("try unencoded")
        miss = unencoded_layout_prefix(text)
        if miss:
            raise ParseFail(miss)
        stmt, text = _stmt(text)
        miss = unencoded_layout_stmt(stmt)
        if miss:
            raise ParseFail(miss)
        if _looks_like_decl(stmt):
            _decl(e, stmt)
        else:
            _assign(e, stmt)


def _decl(e: _Engine, stmt: str) -> None:
    stmt = stmt.rstrip(";").strip()
    m = re.match(
        r"(?:int|unsigned(?:\s+int)?|long|short|char|uint32_t|int32_t|size_t)"
        r"(?:\s+const)?\s+([A-Za-z_]\w*)(?:\s*=\s*(.*))?$",
        stmt,
    )
    if not m:
        arr = re.search(r"\[([^\]]+)\]", stmt)
        if arr and not re.fullmatch(r"\d+", arr.group(1).strip()):
            raise ParseFail("VLA unencoded")
        if "[" in stmt:
            raise ParseFail("array decl")
        # `unsigned long int un = ..., ur, i;`: a local of a type (or a
        # declarator list) the engine does not model. Untracked, its uses
        # would read a signed full range and alarm falsely (tinyexpr ncr).
        raise ParseFail("unmodelled declaration")
    name, init = m.group(1), m.group(2)
    if _type_is_unsigned(stmt[: m.start(1)]):
        e.unsigned.add(name)
    e.set(name, _eval(e, init) if init else TOP)


def _assign(e: _Engine, stmt: str) -> None:
    stmt = stmt.rstrip(";").strip()
    if not stmt:
        return
    parts = _split_comma(stmt)
    if len(parts) > 1:
        for part in parts:
            piece = part.strip()
            if piece:
                _assign(e, piece)
        return
    m = re.match(r"([A-Za-z_]\w*)\s*(\+\+|--)$", stmt)
    if m:
        name, op = m.group(1), m.group(2)
        cur = e.get(name)
        e.set(name, _binop(e, cur, "+" if op == "++" else "-", R(1, 1)))
        return
    m = re.match(r"(\+\+|--)([A-Za-z_]\w*)$", stmt)
    if m:
        op, name = m.group(1), m.group(2)
        cur = e.get(name)
        e.set(name, _binop(e, cur, "+" if op == "++" else "-", R(1, 1)))
        return
    m = re.match(r"([A-Za-z_]\w*)\s*([+\-*/%&|^]|<<|>>)?=\s*(.*)$", stmt)
    if not m:
        _eval(e, stmt)
        return
    name, op, rhs = m.group(1), m.group(2), m.group(3)
    rv = _eval(e, rhs)
    if op:
        e.set(name, _binop(e, e.get(name), op, rv))
    else:
        e.set(name, rv)


def _if(e: _Engine, text: str, bound: int) -> str:
    rest = text[2:].lstrip()
    cond, after = _paren(rest)
    then_src, after = _take_block(after)
    else_src = None
    stripped = after.lstrip()
    if _starts_kw(stripped, "else"):
        else_src, after = _take_block(stripped[4:])
    then_e = _fork(e)
    _refine(then_e, cond, True)
    else_e = _fork(e)
    _refine(else_e, cond, False)
    then_dead = else_dead = False
    try:
        _stmts(then_e, then_src, bound)
    except _Return:
        then_dead = True
    if else_src is not None:
        try:
            _stmts(else_e, else_src, bound)
        except _Return:
            else_dead = True
    if then_dead and else_dead:
        e.live = False
        return after
    if then_dead:
        e.st = else_e.st
        e.unsigned = set(else_e.unsigned)
        return after
    if else_dead:
        e.st = then_e.st
        e.unsigned = set(then_e.unsigned)
        return after
    e.st = _join_state(then_e.st, else_e.st)
    return after


def _do(e: _Engine, text: str, bound: int) -> str:
    rest = text[2:].lstrip()
    body, after = _take_block(rest)
    after = after.lstrip()
    if not _starts_kw(after, "while"):
        raise ParseFail("do without while")
    after = after[5:].lstrip()
    cond, after = _paren(after)
    after = after.lstrip()
    if after.startswith(";"):
        after = after[1:]
    body_e = _fork(e)
    try:
        _stmts(body_e, body, bound)
    except _Return:
        e.st = body_e.st
        e.unsigned = set(body_e.unsigned)
        return after
    e.st = _join_state(e.st, body_e.st)
    for _ in range(max(1, bound) - 1):
        body_e = _fork(e)
        _refine(body_e, cond, True)
        try:
            _stmts(body_e, body, bound)
        except _Return:
            break
        e.st = _join_state(e.st, body_e.st)
    _refine(e, cond, False)
    return after


def _while(e: _Engine, text: str, bound: int) -> str:
    rest = text[5:].lstrip()
    cond, after = _paren(rest)
    body, after = _take_block(after)
    for _ in range(max(1, bound)):
        body_e = _fork(e)
        _refine(body_e, cond, True)
        try:
            _stmts(body_e, body, bound)
        except _Return:
            break
        e.st = _join_state(e.st, body_e.st)
    _refine(e, cond, False)
    return after


def _for(e: _Engine, text: str, bound: int) -> str:
    rest = text[3:].lstrip()
    header, after = _paren(rest)
    body, after = _take_block(after)
    parts = [p.strip() for p in _split_for(header)]
    init = parts[0] if parts else ""
    cond = parts[1] if len(parts) > 1 else ""
    step = parts[2] if len(parts) > 2 else ""
    if init:
        init_stmt = init if init.endswith(";") else init + ";"
        miss = unencoded_layout_stmt(init_stmt)
        if miss:
            raise ParseFail(miss)
        if _looks_like_decl(init_stmt):
            _decl(e, init)
        else:
            _assign(e, init)
    for _ in range(max(1, bound)):
        body_e = _fork(e)
        if cond:
            _refine(body_e, cond, True)
        try:
            _stmts(body_e, body, bound)
            if step:
                _assign(body_e, step)
        except _Return:
            break
        e.st = _join_state(e.st, body_e.st)
    if cond:
        _refine(e, cond, False)
    return after


def _split_for(header: str) -> list[str]:
    parts: list[str] = []
    depth = 0
    cur: list[str] = []
    for ch in header:
        if ch == "(":
            depth += 1
            cur.append(ch)
        elif ch == ")":
            depth = max(0, depth - 1)
            cur.append(ch)
        elif ch == ";" and depth == 0:
            parts.append("".join(cur))
            cur = []
        else:
            cur.append(ch)
    parts.append("".join(cur))
    return parts


def _take_block(text: str) -> tuple[str, str]:
    text = text.lstrip()
    if text.startswith("{"):
        inner, rest = _brace(text)
        return inner, rest
    stmt, rest = _stmt(text)
    return stmt, rest


_CMP = ("==", "!=", "<=", ">=", "<", ">")


def _refine(e: _Engine, cond: str, truth: bool) -> None:
    """Intersect a variable's range when `cond` is a simple comparison."""
    tokens = _tok(cond)
    idx = next((i for i, t in enumerate(tokens) if t in _CMP), None)
    if idx is None or idx == 0 or not _is_ident(tokens[0]) or idx != 1:
        return
    name, op = tokens[0], tokens[1]
    rhs_s = " ".join(tokens[2:])
    try:
        rhs = _eval(e, rhs_s)
    except (ParseFail, _Alarm):
        return
    if rhs.lo != rhs.hi:
        if op == "==" and truth:
            e.set(name, _meet(e.get(name), rhs))
        return
    c = rhs.lo
    cur = e.get(name)
    if not truth:
        op = {
            "<": ">=", ">": "<=", "<=": ">", ">=": "<",
            "==": "!=", "!=": "==",
        }[op]
    if op == "<":
        e.set(name, _meet(cur, R(INT_MIN, c - 1)))
    elif op == "<=":
        e.set(name, _meet(cur, R(INT_MIN, c)))
    elif op == ">":
        e.set(name, _meet(cur, R(c + 1, INT_MAX)))
    elif op == ">=":
        e.set(name, _meet(cur, R(c, INT_MAX)))
    elif op == "==":
        e.set(name, _meet(cur, R(c, c)))
    elif op == "!=":
        if cur.lo == c and cur.hi > c:
            e.set(name, R(c + 1, cur.hi))
        elif cur.hi == c and cur.lo < c:
            e.set(name, R(cur.lo, c - 1))
        elif cur.lo == cur.hi == c:
            e.set(name, BOT)


def _eval(e: _Engine, src: str) -> R:
    tokens = _interval_tok(src)
    pos = 0

    def peek() -> str:
        return tokens[pos] if pos < len(tokens) else ""

    def eat(t: str | None = None) -> str:
        nonlocal pos
        if pos >= len(tokens):
            raise ParseFail("eof")
        got = tokens[pos]
        if t is not None and got != t:
            raise ParseFail(f"expected {t} got {got}")
        pos += 1
        return got

    prec = {
        "||": 10, "&&": 20,
        "==": 30, "!=": 30, "<": 30, ">": 30, "<=": 30, ">=": 30,
        "+": 40, "-": 40,
        "*": 50, "/": 50, "%": 50,
        "<<": 45, ">>": 45,
    }

    def nud() -> R:
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
                n = _sizeof_interval(inner)
                return R(n, n)
            name = eat()
            n = _sizeof_interval([name])
            return R(n, n)
        if t == "(":
            if peek() == "{":
                raise ParseFail("statement-expr unencoded")
            if peek() in _CAST_WORDS:
                while peek() and peek() != ")":
                    if peek() not in _CAST_WORDS and peek() != "*":
                        break
                    eat()
                eat(")")
                return parse(90)
            v = parse(0)
            eat(")")
            return v
        if t == "-":
            v = parse(90)
            return _uneg(e, v)
        if t == "+":
            return parse(90)
        if t == "!":
            v = parse(90)
            if v.lo == 0 and v.hi == 0:
                return R(1, 1)
            if not v.contains(0):
                return R(0, 0)
            return R(0, 1)
        if t == "~":
            v = parse(90)
            if v.lo == v.hi:
                return R(~v.lo, ~v.lo)
            return TOP
        if t in ("++", "--"):
            name = eat()
            cur = e.get(name)
            nxt = _binop(e, cur, "+" if t == "++" else "-", R(1, 1))
            e.set(name, nxt)
            return nxt
        if t.isdigit() or t.startswith("0x"):
            n = int(t, 0)
            # Integer suffix (the tokenizer splits `16U` / `5ULL`): U makes the
            # literal unsigned; L / LL is a 64-bit literal outside this 32-bit
            # domain (`state * 6364136223846793005ULL` is not a signed overflow).
            uns = False
            sfx = peek()
            if sfx and not sfx.strip("uUlL"):
                if any(ch in "lL" for ch in sfx):
                    raise ParseFail("64-bit literal unencoded")
                uns = True
                eat()
            if n > INT_MAX:
                n -= 1 << WIDTH
            return R(n, n, uns)
        if len(t) >= 3 and t.startswith("'") and t.endswith("'"):
            return R(_char_lit_value(t), _char_lit_value(t))
        if _is_ident(t):
            if t in ("_Generic", "offsetof"):
                raise ParseFail(f"{t} unencoded")
            if peek() == "(":
                eat("(")
                if peek() and peek() != ")":
                    parse(2)
                    while peek() == ",":
                        eat(",")
                        parse(2)
                eat(")")
                return _CALL_DUMMY
            if peek() == "[":
                raise ParseFail("index")
            if peek() in ("++", "--"):
                op = eat()
                cur = e.get(t)
                nxt = _binop(e, cur, "+" if op == "++" else "-", R(1, 1))
                e.set(t, nxt)
                return cur
            return e.get(t)
        raise ParseFail(f"nud {t}")

    def parse(minp: int) -> R:
        left = nud()
        while peek() in prec and prec[peek()] >= minp:
            op = eat()
            right = parse(prec[op] + 1)
            left = _binop(e, left, op, right)
        if minp <= 5 and peek() == "?":
            eat("?")
            then_v = parse(0)
            eat(":")
            else_v = parse(5)
            left = _join(then_v, else_v)
        if minp <= 1 and peek() == ",":
            eat(",")
            left = parse(0)
        return left

    v = parse(0)
    if pos != len(tokens):
        raise ParseFail(f"trailing {tokens[pos:]}")
    return v


def _uneg(e: _Engine, v: R) -> R:
    if not v.unsigned and v.contains(INT_MIN):
        raise _Alarm("INT-SIGNED-OVF", "negation of INT_MIN")
    return R(-v.hi, -v.lo, v.unsigned)


def _ovf_add(a: int, b: int) -> bool:
    s = a + b
    return s < INT_MIN or s > INT_MAX


def _ovf_sub(a: int, b: int) -> bool:
    s = a - b
    return s < INT_MIN or s > INT_MAX


def _ovf_mul(a: int, b: int) -> bool:
    p = a * b
    return p < INT_MIN or p > INT_MAX


def _binop(e: _Engine, a: R, op: str, b: R) -> R:
    if a.empty() or b.empty():
        return BOT
    u = a.unsigned or b.unsigned
    if op == "+":
        if not u and (_ovf_add(a.lo, b.lo) or _ovf_add(a.lo, b.hi) or _ovf_add(a.hi, b.lo) or _ovf_add(a.hi, b.hi)):
            raise _Alarm("INT-SIGNED-OVF", "signed + may overflow")
        if u and (_ovf_add(a.lo, b.lo) or _ovf_add(a.lo, b.hi) or _ovf_add(a.hi, b.lo) or _ovf_add(a.hi, b.hi)):
            return R(INT_MIN, INT_MAX, True)
        return _clip(a.lo + b.lo, a.hi + b.hi, u)
    if op == "-":
        if not u and (_ovf_sub(a.lo, b.lo) or _ovf_sub(a.lo, b.hi) or _ovf_sub(a.hi, b.lo) or _ovf_sub(a.hi, b.hi)):
            raise _Alarm("INT-SIGNED-OVF", "signed - may overflow")
        if u and (_ovf_sub(a.lo, b.lo) or _ovf_sub(a.lo, b.hi) or _ovf_sub(a.hi, b.lo) or _ovf_sub(a.hi, b.hi)):
            return R(INT_MIN, INT_MAX, True)
        return _clip(a.lo - b.hi, a.hi - b.lo, u)
    if op == "*":
        corners = (a.lo * b.lo, a.lo * b.hi, a.hi * b.lo, a.hi * b.hi)
        if any(c < INT_MIN or c > INT_MAX for c in corners):
            if u:
                return R(INT_MIN, INT_MAX, True)
            raise _Alarm("INT-SIGNED-OVF", "signed * may overflow")
        return _clip(min(corners), max(corners), u)
    if op in ("/", "%"):
        if b.contains(0):
            raise _Alarm("INT-DIV-ZERO", "divisor range includes 0")
        if not u and a.contains(INT_MIN) and b.contains(-1):
            raise _Alarm("INT-SIGNED-OVF", "INT_MIN / -1")
        if b.lo == b.hi:
            if op == "/":
                lo, hi = a.lo // b.lo, a.hi // b.lo
                return R(min(lo, hi), max(lo, hi), u)
            lo, hi = a.lo % b.lo, a.hi % b.lo
            return R(min(lo, hi), max(lo, hi), u)
        return R(INT_MIN, INT_MAX, u)
    if op in ("<<", ">>"):
        if u:
            if b.hi >= WIDTH:
                raise _Alarm("INT-SHIFT-UB", "shift amount out of 0..31")
        elif b.lo < 0 or b.hi >= WIDTH:
            raise _Alarm("INT-SHIFT-UB", "shift amount out of 0..31")
        if not u and op == "<<" and a.contains(1) and b.hi >= WIDTH - 1:
            raise _Alarm("INT-SHIFT-UB", "1<<31 is undefined for signed int")
        if a.lo == a.hi and b.lo == b.hi:
            v = a.lo << b.lo if op == "<<" else a.lo >> b.lo
            return R(v, v, u)
        return R(INT_MIN, INT_MAX, u)
    if op in ("==", "!=", "<", ">", "<=", ">="):
        return R(0, 1)
    if op in ("&", "|", "^"):
        return R(INT_MIN, INT_MAX, u)
    raise ParseFail(f"op {op}")
