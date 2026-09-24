"""ESBMC-method BMC over a SCALAR C subset, via Z3.

Statuses are the ParanoidBSD set. POINTER functions are NEEDS-HARNESS.
Missing Z3 is NOTRUN. Unparseable is ERROR, never a proof.

Supported: ints, constant-size arrays, if/else, while/for/do-while with
bounded unwind, for-header decls (`for (int i = 0; ...)`), ++/-- (add/sub
1 with signed overflow), switch/case/break/default (C fallthrough),
enum { NAME = val }, assert(), sizeof, ternary `?:`, comma operator,
continue, +, -, *, /, %, <<, >>, comparisons, assignments. Uninitialised
locals are tracked; a read is UNINIT-READ. `goto` is encoded when it
is structured (a jump out to a later statement of the same or an enclosing
list; a backward goto whose statements form a loop, unwound like one);
any other goto is NEEDS-HARNESS, never a proof and never ERROR. A VLA is NEEDS-HARNESS (missing bound), not a closed
proof. A recursive self-call is NEEDS-HARNESS: an unconstrained
result is not a proof of the callee, and arithmetic on that havoc
is not a counterexample of the original. `return buf` of a local
array is NEEDS-HARNESS (dangling decay), like `return &x`. `alloca`
/ `__builtin_alloca` is NEEDS-HARNESS (unmodeled stack frame), like
malloc. C++ `throw` is NEEDS-HARNESS (missing exception model), never
a parse ERROR and never a proof. `setjmp`/`longjmp`/`va_list`/`va_start`/`va_arg`
are NEEDS-HARNESS (missing nonlocal/variadic model), never a parse
ERROR and never a proof.
Designated initializers `{[i]=n}` / `{.f=n}`, `_Alignof`/`alignof`,
C++ range-for, and C++ lambdas are NEEDS-HARNESS (missing model),
never ERROR and never a vacuous proof. `_Static_assert` /
`static_assert` statements are skipped when the rest of the body
still encodes; `sizeof` still encodes.
`return buf + 0` / `return buf + i` / `return 0 + buf` / `return (buf)`
of a local array is the same dangling decay as `return buf`. `return (&x)`
is the same unmodeled address-of as `return &x`. A local
`const T x` is NEEDS-HARNESS (missing const model), never a parse
ERROR; `const` on a parameter is stripped and is not that case.
`for (const int i = 0; ...)` is the same missing const model.
A local `struct S s` / anonymous `struct { int x; } s` / unknown
typedef is NEEDS-HARNESS (missing layout), never a trailing-token ERROR.
`register int x` / `auto int x` / C++ `auto x = 1` are NEEDS-HARNESS
(missing storage-class or auto-type model), never a parse ERROR. Inline `asm` /
`__asm__`, `_Generic`, GNU statement expressions `({ ... })`, C++
`try`/`catch`, `offsetof`, C++ `new`/`delete`, and `volatile` /
`_Atomic` decls are NEEDS-HARNESS (missing model), never a parse
ERROR and never a proof. A body that is only an unmodeled system/
exit/pthread_create/printf/`__builtin_unreachable`/`__builtin_trap`
with no encoded UB properties is NEEDS-HARNESS, not a vacuous
unbounded proof.
Unsigned parameters use unsigned compares and wrap; signed overflow
is not reported for them.

k-induction: when BMC is BOUNDED, havoc + k=1 then k=2 concatenated
iterations. SAT on a havoced step stays BOUNDED (never a FAILED of the
original). UNSAT is PROVED-UNBOUNDED and is never folded into a bounded
proof. Nested loops stay unencoded BOUNDED.
"""

from __future__ import annotations

from dataclasses import dataclass, field, replace
import functools
from pathlib import Path
from typing import Any
import re

from prism import laws
from prism.cparse import body_needs_pointer_harness
from prism.models import Finding, FunctionInfo


# The encoder gates on ~1000 distinct literal patterns: more than the `re`
# module's own 512-entry cache, which then thrashes and recompiles every
# pattern on every function. Same semantics as re.search / re.match, with a
# cache large enough to hold them all.
@functools.lru_cache(maxsize=4096)
def _rx(pattern: str, flags: int = 0) -> re.Pattern[str]:
    return re.compile(pattern, flags)


def _re_search(pattern: str, string: str, flags: int = 0) -> re.Match[str] | None:
    return _rx(pattern, flags).search(string)


def _re_match(pattern: str, string: str, flags: int = 0) -> re.Match[str] | None:
    return _rx(pattern, flags).match(string)


try:
    import z3
    HAS_Z3 = True
except Exception:  # pragma: no cover
    z3 = None  # type: ignore
    HAS_Z3 = False


WIDTH = 32
INT_MIN = -(1 << (WIDTH - 1))
INT_MAX = (1 << (WIDTH - 1)) - 1


def slt(a, b):
    return a < b


def sgt(a, b):
    return a > b


def sle(a, b):
    return a <= b


def sge(a, b):
    return a >= b


def uge(a, b):
    return z3.UGE(a, b)


def ult(a, b):
    return z3.ULT(a, b)


def ugt(a, b):
    return z3.UGT(a, b)


def ule(a, b):
    return z3.ULE(a, b)


_UNSIGNED_TY = re.compile(
    r"\bunsigned\b|\bsize_t\b|\buint\d*_t\b|\bu_int\b|\bu_long\b",
    re.I,
)

_CALL_KW = {
    "if", "for", "while", "switch", "return", "sizeof", "typeof",
    "__typeof__", "else", "do", "case", "default", "_Generic",
    "break", "continue", "goto", "struct", "union", "enum", "assert",
    "static_assert", "_Static_assert", "alignof", "_Alignof",
    "__attribute__", "offsetof",
}


def _type_is_unsigned(typ: str) -> bool:
    return bool(_UNSIGNED_TY.search(typ or ""))


def _type_width(typ: str) -> int:
    t = re.sub(r"\s+", " ", (typ or "").lower())
    if "long long" in t or _re_search(r"\b[iu]nt64_t\b", t):
        return 64
    return WIDTH


def _bv_min(w: int):
    return z3.BitVecVal(-(1 << (w - 1)), w)


def _bv_zero(w: int):
    return z3.BitVecVal(0, w)


# --------------------------------------------------------------------------
# Typed bitvector encoder for the scalar C subset. The C++ engine mirrors
# this in src/prism/bmc_encoder.inc; both must give identical verdicts.
#
# Semantics (C11, LP64 x86-64; docs/CONFORMANCE.md S1-S7, F1):
#  * every value carries its C type (width, signedness): TV;
#  * integer promotions (6.3.1.1) and the usual arithmetic conversions
#    (6.3.1.8) are applied before every arithmetic operator and comparison;
#  * signed + - * overflow is checked in both directions, / and % check
#    zero and MIN / -1, << checks the count, a negative left operand and an
#    unrepresentable result, >> takes its type from the promoted left operand;
#  * &&, || and ?: evaluate their right operands only under their condition;
#  * break / continue / early loop exits merge their states (no path is
#    silently dropped); loops are unrolled (bounded) or, for the unbounded
#    proof, havocked (every variable the loop assigns is made arbitrary);
#  * anything not modelled (unknown identifier, unexpanded macro, call to a
#    function that is neither inlined nor modelled) is never a proof: it is
#    UNENCODED (NEEDS-HARNESS), and call arguments are still checked.

_CTYPE_NAMED: dict[str, tuple[int, bool]] = {
    "_bool": (1, True), "bool": (1, True),
    "int8_t": (8, False), "uint8_t": (8, True),
    "int16_t": (16, False), "uint16_t": (16, True),
    "int32_t": (32, False), "uint32_t": (32, True),
    "int64_t": (64, False), "uint64_t": (64, True),
    "size_t": (64, True), "ssize_t": (64, False),
    "ptrdiff_t": (64, False), "intptr_t": (64, False),
    "uintptr_t": (64, True), "intmax_t": (64, False),
    "uintmax_t": (64, True),
    "u_char": (8, True), "u_short": (16, True),
    "u_int": (32, True), "u_long": (64, True),
    "wchar_t": (32, False), "char8_t": (8, True),
    "char16_t": (16, True), "char32_t": (32, True),
}


def _ctype_parse(typ: str) -> tuple[int, bool] | None:
    """(width, unsigned) of an integer type name, None if not modelled."""
    t = (typ or "").lower()
    if any(c in t for c in "*[&("):
        return None
    t = _re_sub(
        r"\b(?:const|volatile|register|auto|static|extern|inline|restrict|__restrict|__restrict__)\b",
        " ", t,
    )
    words = t.split()
    if not words:
        return None
    if len(words) == 1 and words[0] in _CTYPE_NAMED:
        return _CTYPE_NAMED[words[0]]
    is_u = is_s = False
    n_long = n_short = n_char = n_int = 0
    for wd in words:
        if wd == "unsigned":
            is_u = True
        elif wd == "signed":
            is_s = True
        elif wd == "long":
            n_long += 1
        elif wd == "short":
            n_short += 1
        elif wd == "char":
            n_char += 1
        elif wd == "int":
            n_int += 1
        else:
            return None
    if (is_u and is_s) or n_long > 2 or n_short > 1 or n_char > 1 or n_int > 1:
        return None
    if n_char and (n_long or n_short or n_int):
        return None
    if n_short and n_long:
        return None
    if n_char:
        return (8, is_u)
    if n_short:
        return (16, is_u)
    if n_long:
        return (64, is_u)
    return (32, is_u)


def _ctype_of(typ: str) -> tuple[int, bool]:
    ct = _ctype_parse(typ)
    if ct is not None:
        return ct
    return (WIDTH, _type_is_unsigned(typ))


def _re_sub(pattern: str, repl: str, s: str, flags: int = 0) -> str:
    return _rx(pattern, flags).sub(repl, s)


class TV:
    """A typed value: a bitvector of width w holding a value of C type (w, u)."""

    __slots__ = ("v", "w", "u")

    def __init__(self, v: Any, w: int = WIDTH, u: bool = False) -> None:
        self.v = v
        self.w = w
        self.u = u


@dataclass
class Arr:
    a: Any
    n: int
    w: int = WIDTH
    u: bool = False


@dataclass
class _State:
    path: Any
    vars: dict[str, Any]
    arrays: dict[str, Arr]
    uninit: dict[str, Any]


@dataclass
class _Jump:
    """Pending jumps of one loop (break/continue) or switch (break)."""

    loop: bool = True
    breaks: list[_State] = field(default_factory=list)
    conts: list[_State] = field(default_factory=list)


@dataclass
class Prop:
    name: str
    cls: str
    cond: Any  # z3 Bool, true means VIOLATION
    loc: int


class _Enc:
    def __init__(self, unwind: int) -> None:
        self.unwind = unwind
        self.s = z3.Solver()
        self.s.set("timeout", 8000)
        self.vars: dict[str, Any] = {}
        self.arrays: dict[str, Arr] = {}
        self.alias: dict[str, str] = {}  # pointer -> array it aliases
        # name -> Bool, true = maybe uninit. "@a" -> Array(BV, Bool): the
        # per-element shadow of local array a (true = element maybe unwritten).
        self.uninit: dict[str, Any] = {}
        self.value_arrays: list[str] = []  # arrays used as values (escape to calls)
        self.props: list[Prop] = []
        self.pc = 0
        self.fresh = 0
        self.unwind_ok = True
        self.path_true = z3.BoolVal(True)
        self.unsigned: set[str] = set()
        self.bits: dict[str, int] = {}  # declared (visible) scalars -> width
        self.unmodelled: list[str] = []  # calls whose callee is not modelled
        self.call_vars: list[Any] = []  # their unconstrained results
        self.jumps: list[_Jump] = []
        self.scopes: list[list[str]] = []
        self.havoc = False  # loops are havocked (unbounded step) instead of unrolled
        self.cxx = False  # C++ source: call arguments may bind references
        self.shift_rules = 0  # Parser.shift_rules

    def retag_unsigned(self) -> None:
        return None

    def type_of(self, n: str) -> tuple[int, bool]:
        return (self.bits.get(n, WIDTH), n in self.unsigned)

    def canonical(self, n: str) -> str:
        return self.alias.get(n, n)

    def is_array(self, n: str) -> bool:
        return self.canonical(n) in self.arrays

    def declared(self, n: str) -> bool:
        return n in self.bits or n in self.arrays or n in self.alias

    def _note_scope(self, n: str) -> None:
        if self.scopes:
            self.scopes[-1].append(n)

    def declare(self, n: str, t: tuple[int, bool]) -> None:
        self.bits[n] = t[0]
        if t[1]:
            self.unsigned.add(n)
        else:
            self.unsigned.discard(n)
        self._note_scope(n)

    def declare_array(self, n: str, a: Arr) -> None:
        self.arrays[n] = a
        self._note_scope(n)

    def declare_alias(self, n: str, target: str) -> None:
        self.alias[n] = self.canonical(target)
        self._note_scope(n)

    def push_scope(self) -> None:
        self.scopes.append([])

    def pop_scope(self) -> None:
        if not self.scopes:
            return
        for n in self.scopes.pop():
            self.bits.pop(n, None)
            self.unsigned.discard(n)
            self.vars.pop(n, None)
            self.uninit.pop(n, None)
            self.uninit.pop("@" + n, None)
            self.arrays.pop(n, None)
            self.alias.pop(n, None)

    def bv(self, name: str | None = None, width: int | None = None) -> Any:
        self.fresh += 1
        w = width or WIDTH
        return z3.BitVec(name or f"t{self.fresh}", w)

    def get(self, name: str) -> TV:
        w, u = self.type_of(name)
        if name not in self.vars:
            self.vars[name] = self.bv(name, w)
        return TV(self.vars[name], w, u)

    def conv(self, x: TV, t: tuple[int, bool]) -> TV:
        w, u = t
        if w == 1:
            if x.w == 1:
                return TV(x.v, 1, True)
            return TV(z3.If(x.v != _bv_zero(x.w), z3.BitVecVal(1, 1), z3.BitVecVal(0, 1)), 1, True)
        v = x.v
        if x.w < w:
            v = z3.ZeroExt(w - x.w, v) if x.u else z3.SignExt(w - x.w, v)
        elif x.w > w:
            v = z3.Extract(w - 1, 0, v)
        return TV(v, w, u)

    def promote(self, x: TV) -> TV:
        """Integer promotions (C11 6.3.1.1p2): narrower than int -> int."""
        if x.w < 32:
            return self.conv(x, (32, False))
        return x

    def common(self, a: TV, b: TV) -> tuple[int, bool]:
        """Usual arithmetic conversions (C11 6.3.1.8) after promotion."""
        wa, wb = max(a.w, 32), max(b.w, 32)
        ua, ub = a.w >= 32 and a.u, b.w >= 32 and b.u
        if ua == ub:
            return (max(wa, wb), ua)
        uw, sw = (wa, wb) if ua else (wb, wa)
        if uw >= sw:
            return (uw, True)
        return (sw, False)

    def set(self, name: str, val: TV) -> None:
        self.vars[name] = self.conv(val, self.type_of(name)).v

    def mark_init(self, name: str) -> None:
        self.uninit[name] = z3.BoolVal(False)

    def mark_uninit(self, name: str) -> None:
        self.uninit[name] = z3.BoolVal(True)

    def check_read(self, name: str) -> None:
        flag = self.uninit.get(name)
        if flag is None:
            return
        try:
            if z3.is_false(z3.simplify(flag)):
                return
        except Exception:
            pass
        self.add_prop("uninit", "UNINIT-READ", flag, self.pc)

    def shadow_init(self, name: str, init: bool) -> None:
        """Declare the element shadow of array `name`: all written or none."""
        self.uninit["@" + name] = z3.K(z3.BitVecSort(WIDTH), z3.BoolVal(not init))

    def mark_elem_init(self, name: str, idx: Any) -> None:
        u = self.uninit.get("@" + name)
        if u is not None:
            self.uninit["@" + name] = z3.Store(u, idx, z3.BoolVal(False))

    def check_elem(self, name: str, i: TV, n: int) -> None:
        """Reading an element never written is an indeterminate value
        (C11 6.3.2.1p2, 6.7.9p10): UNINIT-READ. Out-of-bounds indices are the
        OOB property's, not this one's."""
        u = self.uninit.get("@" + name)
        if u is None:
            return
        viol = z3.And(z3.Not(_oob(self, i, n)), z3.Select(u, _index32(self, i)))
        try:
            if z3.is_false(z3.simplify(viol)):
                return
        except Exception:
            pass
        self.add_prop("uninit", "UNINIT-READ", viol, self.pc)

    def havoc_shadow(self, name: str, tag: str) -> None:
        """A havocked loop may write any element but never unwrites one."""
        u = self.uninit.get("@" + name)
        if u is None:
            return
        self.fresh += 1
        h = z3.Array(f"{tag}{self.fresh}_u", z3.BitVecSort(WIDTH), z3.BoolSort())
        x = z3.BitVec(f"{tag}{self.fresh}_x", WIDTH)
        self.uninit["@" + name] = z3.Lambda([x], z3.And(z3.Select(u, x), z3.Select(h, x)))

    def escape_array(self, name: str) -> None:
        """Array passed to an unmodelled call: the callee may write any element.
        Its contents become unknown (quantified like a call result) and its
        elements count as written; the unmodelled call already rules out a
        proof (S6), so this only avoids false alarms."""
        arr = self.arrays.get(name)
        if arr is None:
            return
        self.fresh += 1
        a = z3.Array(f"{name}_esc{self.fresh}", z3.BitVecSort(WIDTH), z3.BitVecSort(arr.w))
        self.call_vars.append(a)
        self.arrays[name] = Arr(a, arr.n, arr.w, arr.u)
        if "@" + name in self.uninit:
            self.shadow_init(name, True)

    def escape_scalar(self, name: str) -> None:
        """C++ scalar passed to an unmodelled call: bound to a non-const
        reference parameter, the callee may write it. Its value becomes unknown
        (quantified like a call result) and it counts as written; no proof
        follows (S6)."""
        w, _u = self.type_of(name)
        v = self.bv(f"{name}_esc{self.fresh + 1}", w)
        self.call_vars.append(v)
        self.vars[name] = v
        self.mark_init(name)

    def add_prop(self, name: str, cls: str, viol: Any, loc: int) -> None:
        self.props.append(Prop(name, cls, z3.And(self.path_true, viol), loc))

    def assume(self, cond: Any) -> None:
        self.path_true = z3.And(self.path_true, cond)

    def snap(self) -> _State:
        return _State(self.path_true, dict(self.vars), dict(self.arrays), dict(self.uninit))

    def load(self, st: _State) -> None:
        self.path_true = st.path
        self.vars = dict(st.vars)
        self.arrays = dict(st.arrays)
        self.uninit = dict(st.uninit)

    def int_val(self, v: int) -> TV:
        return TV(z3.BitVecVal(v, 32), 32, False)


def _bool_tv(b: Any) -> TV:
    return TV(z3.If(b, z3.BitVecVal(1, 32), z3.BitVecVal(0, 32)), 32, False)


def _merge_states(e: _Enc, states: list[_State], base: _State) -> _State:
    """Merge disjoint states, each selected by its own path condition.

    Only names visible in `base` (the state before the construct) survive:
    anything declared inside the construct is out of scope afterwards.
    """
    live = [st for st in states if not z3.is_false(st.path)]
    if not live:
        return _State(z3.BoolVal(False), dict(base.vars), dict(base.arrays), dict(base.uninit))
    path = live[0].path if len(live) == 1 else z3.Or(*[st.path for st in live])
    out_vars: dict[str, Any] = {}
    for k, v0 in base.vars.items():
        acc = live[-1].vars.get(k, v0)
        for st in reversed(live[:-1]):
            v = st.vars.get(k, v0)
            if not z3.eq(v, acc):
                acc = z3.If(st.path, v, acc)
        out_vars[k] = acc
    out_arrays: dict[str, Arr] = {}
    for k, a0 in base.arrays.items():
        last = live[-1].arrays.get(k, a0)
        acc_a = last.a
        for st in reversed(live[:-1]):
            va = st.arrays.get(k, a0).a
            if not z3.eq(va, acc_a):
                acc_a = z3.If(st.path, va, acc_a)
        out_arrays[k] = Arr(acc_a, last.n, last.w, last.u)
    out_uninit: dict[str, Any] = {}
    for k, u0 in base.uninit.items():
        acc = live[-1].uninit.get(k, u0)
        for st in reversed(live[:-1]):
            v = st.uninit.get(k, u0)
            if not z3.eq(v, acc):
                acc = z3.If(st.path, v, acc)
        out_uninit[k] = acc
    return _State(path, out_vars, out_arrays, out_uninit)


def _oob(e: _Enc, i0: TV, n: int) -> Any:
    i = e.promote(i0)
    bound = z3.BitVecVal(n, i.w)
    if i.u:
        return z3.UGE(i.v, bound)
    return z3.Or(i.v < _bv_zero(i.w), i.v >= bound)


def _index32(e: _Enc, i: TV) -> Any:
    return e.conv(e.promote(i), (WIDTH, False)).v


def _is_ident(t: str) -> bool:
    return bool(re.fullmatch(r"[A-Za-z_]\w*", t))


def _starts_kw(text: str, kw: str) -> bool:
    if not text.startswith(kw):
        return False
    if len(text) == len(kw):
        return True
    c = text[len(kw)]
    return not (c.isalnum() or c == "_")


def _is_computed_goto(text: str) -> bool:
    """GNU `goto *p` / `goto *&&lab` — missing model, not plain `goto` ERROR."""
    if not _starts_kw(text, "goto"):
        return False
    return text[len("goto"):].lstrip().startswith("*")


_NESTED_FN_HEAD = re.compile(
    r"(?:void|int|unsigned(?:\s+int)?|long(?:\s+int)?|short|char|"
    r"float|double|_Bool|bool)\s+[A-Za-z_]\w*\s*\([^)]*\)\s*\{"
)


def _is_nested_function(text: str) -> bool:
    """GNU nested function definition inside a function body."""
    return bool(_NESTED_FN_HEAD.match((text or "").lstrip()))


_DECL_KWS = (
    "int", "unsigned", "long", "short", "char",
    "uint32_t", "int32_t", "uint64_t", "int64_t", "size_t",
)

_DECL_TYPE = (
    r"(?:unsigned\s+long\s+long(?:\s+int)?|long\s+long(?:\s+int)?|"
    r"unsigned\s+long(?:\s+int)?|uint64_t|int64_t|"
    r"unsigned(?:\s+int)?|int|long|short|char|uint32_t|int32_t|size_t)"
)


def _looks_like_decl(stmt: str) -> bool:
    s = stmt.lstrip()
    return any(_starts_kw(s, kw) for kw in _DECL_KWS)


def _file_text_nocomments(text: str) -> str:
    try:
        from prism.cparse import strip_comments_keep_lines
        return strip_comments_keep_lines(text)
    except Exception:
        text = re.sub(r"/\*.*?\*/", " ", text, flags=re.S)
        return re.sub(r"//.*?$", " ", text, flags=re.M)


_C_INT = re.compile(r"\s*([+-]?)(0[xX][0-9a-fA-F]+|0[0-7]*|[1-9][0-9]*)", re.ASCII)


def _c_int_full(val: str) -> int | None:
    """strtoll(val, &end, 0) consuming all of val (C++ std::stoll + used)."""
    m = _C_INT.fullmatch(val)
    if not m or not val:
        return None
    digits = m.group(2)
    if digits[:2] in ("0x", "0X"):
        v = int(digits[2:], 16)
    elif len(digits) > 1 and digits[0] == "0":
        v = int(digits, 8)
    else:
        v = int(digits, 10)
    v = -v if m.group(1) == "-" else v
    if v < -(1 << 63) or v > (1 << 63) - 1:
        return None
    return v


def extract_enums(text: str) -> dict[str, int]:
    """Collect `enum { NAME = val, ... }` (and implicit 0,1,2,...) constants.

    An enumerator whose value cannot be computed here (e.g. `A = 1 << 3`)
    is left out together with the implicit enumerators that follow it, and a
    name defined with two different values is left out: an unknown
    enumerator is UNENCODED at its use, never a wrong constant.
    """
    text = _file_text_nocomments(text)
    out: dict[str, int] = {}
    ambiguous: set[str] = set()

    def drop(name: str) -> None:
        ambiguous.add(name)
        out.pop(name, None)

    for m in re.finditer(r"\benum\b(?:\s+[A-Za-z_]\w*)?\s*\{([^{}]*)\}", text):
        nxt: int | None = 0
        for part in m.group(1).split(","):
            part = " ".join(part.split())
            if not part:
                continue
            name = part
            if "=" in part:
                name, val = part.split("=", 1)
                name, val = name.strip(), val.strip().rstrip("uUlL")
                nxt = _c_int_full(val)
                if nxt is None and _is_ident(val) and val not in ambiguous:
                    nxt = out.get(val)
            if not _is_ident(name):
                nxt = None
                continue
            if nxt is None or nxt < -(1 << 31) or nxt > (1 << 31) - 1:
                drop(name)
                nxt = None
                continue
            if name in ambiguous:
                nxt += 1
                continue
            if name in out and out[name] != nxt:
                drop(name)
            else:
                out[name] = nxt
            nxt += 1
    return out


def extract_macros(text: str) -> dict[str, str]:
    """Object-like `#define NAME value` lines of the file.

    There is no preprocessor: a name defined twice with different values,
    #undef'd, or spanning lines is left out and stays UNENCODED at its use.
    Function-like macros are never expanded.
    """
    text = _file_text_nocomments(text)
    out: dict[str, str] = {}
    ambiguous: set[str] = set()
    for m in re.finditer(r"^[ \t]*#[ \t]*undef[ \t]+([A-Za-z_]\w*)", text, re.M | re.ASCII):
        ambiguous.add(m.group(1))
    for m in re.finditer(r"^[ \t]*#[ \t]*define[ \t]+([A-Za-z_]\w*)([ \t][^\n]*)?$", text,
                         re.M | re.ASCII):
        name = m.group(1)
        val = (m.group(2) or "").strip()
        bad = not val or any(c in val for c in ';{}"#\\')
        if bad or name in ambiguous:
            ambiguous.add(name)
            out.pop(name, None)
            continue
        if name in out and out[name] != val:
            ambiguous.add(name)
            out.pop(name, None)
            continue
        out[name] = val
    for n in ambiguous:
        out.pop(n, None)
    return out


def _read_fn_file(fn: FunctionInfo) -> str:
    path = Path(fn.file)
    if not path.is_file():
        return ""
    try:
        return path.read_text(encoding="utf-8", errors="replace")
    except OSError:
        return ""


def _enums_from_fn(fn: FunctionInfo) -> dict[str, int]:
    text = _read_fn_file(fn)
    return extract_enums(text) if text else {}


def _macros_from_fn(fn: FunctionInfo) -> dict[str, str]:
    text = _read_fn_file(fn)
    return extract_macros(text) if text else {}


@dataclass
class _SwitchArm:
    labels: list[Any]  # TV case value, or None for default
    code: str
    stops: bool  # arm ends with break (no fallthrough)


# Declaration type prefix the encoder models (a superset of _DECL_TYPE).
_CDECL_TYPE = (
    r"(?:(?:unsigned|signed)\s+(?:long\s+long|long|short|char|int)(?:\s+int)?|"
    r"long\s+long(?:\s+int)?|long(?:\s+int)?|short(?:\s+int)?|"
    r"unsigned|signed|int|char|_Bool|bool|u?int(?:8|16|32|64)_t|"
    r"size_t|ssize_t|ptrdiff_t|u?intptr_t|u?intmax_t)"
)

# A narrow string literal (the initialiser of a char array).
_STRING_LIT = re.compile(r'"(?:[^"\\]|\\.)*"')

_CDECL_KWS = (
    "int", "unsigned", "signed", "long", "short", "char", "_Bool", "bool",
    "int8_t", "uint8_t", "int16_t", "uint16_t", "uint32_t", "int32_t",
    "uint64_t", "int64_t", "size_t", "ssize_t", "ptrdiff_t", "intptr_t",
    "uintptr_t", "intmax_t", "uintmax_t",
)


def _looks_like_cdecl(stmt: str) -> bool:
    s = stmt.lstrip()
    return any(_starts_kw(s, kw) for kw in _CDECL_KWS)


def _assigned_names(text: str) -> set[str]:
    """Names a loop may assign (over-approximation is sound: more havoc)."""
    out: set[str] = set()
    for pat in (
        r"([A-Za-z_]\w*)\s*(?:\[[^\]]*\]\s*)?(?:<<|>>|[-+*/%&|^])?=(?!=)",
        r"(?:\+\+|--)\s*([A-Za-z_]\w*)",
        r"([A-Za-z_]\w*)\s*(?:\+\+|--)",
    ):
        for m in _rx(pat).finditer(text):
            out.add(m.group(1))
    for m in _rx(r"[A-Za-z_]\w*").finditer(text):
        out.add("@" + m.group(0))  # every mention (arrays)
    return out


def _const_false_cond(src: str) -> bool:
    s = src.strip()
    while len(s) >= 2 and s[0] == "(" and s[-1] == ")":
        s = s[1:-1].strip()
    return s in ("0", "false")


def _block_or_stmt_c(text: str) -> tuple[str, str]:
    """Like _block_or_stmt, but a nested if/loop/switch body is one statement
    including its own else (dangling else binds to the innermost if)."""
    text = text.lstrip()
    if text.startswith("{"):
        return _brace(text)
    for kw in ("if", "switch", "while", "for", "do"):
        if _starts_kw(text, kw):
            return _consume_stmt_src_c(text)
    return _stmt(text)


def _consume_stmt_src_c(text: str) -> tuple[str, str]:
    """_consume_stmt_src over _block_or_stmt_c (the encoder's statement split)."""
    raw = text
    text = text.lstrip()
    skip = len(raw) - len(text)

    def taken(rest: str) -> tuple[str, str]:
        return raw[skip:len(raw) - len(rest)], rest

    if not text:
        return "", ""
    if text.startswith("{"):
        _, rest = _brace(text)
        return taken(rest)
    if _starts_kw(text, "do"):
        rest = text[2:].lstrip()
        _, rest = _block_or_stmt_c(rest)
        rest = rest.lstrip()
        if not _starts_kw(rest, "while"):
            raise ParseFail("do without while")
        rest = rest[5:].lstrip()
        _, rest = _paren(rest)
        rest = rest.lstrip()
        if rest.startswith(";"):
            rest = rest[1:]
        return taken(rest)
    for kw in ("if", "switch", "while", "for"):
        if _starts_kw(text, kw):
            rest = text[len(kw):].lstrip()
            if kw == "if" and rest.startswith("constexpr"):
                raise ParseFail("if constexpr unencoded")
            _, rest = _paren(rest)
            _, rest = _block_or_stmt_c(rest)
            if kw == "if":
                r2 = rest.lstrip()
                if _starts_kw(r2, "else"):
                    _, rest = _block_or_stmt_c(r2[4:])
            return taken(rest)
    return _stmt(text)


def _label_at(text: str) -> tuple[str, int] | None:
    """A statement label `name:` at the start of text (not `case`/`default`,
    not `a::b`): the name and the offset just after the colon."""
    m = re.match(r"([A-Za-z_]\w*)\s*:(?!:)", text)
    if m is None or m.group(1) in ("case", "default", "public", "private", "protected"):
        return None
    return m.group(1), m.end()


def _next_labeled_stmt(text: str) -> tuple[list[str], str]:
    """One statement of a list, with any labels in front of it: (labels, rest)."""
    labels: list[str] = []
    text = text.lstrip()
    while True:
        lab = _label_at(text)
        if lab is None:
            break
        labels.append(lab[0])
        text = text[lab[1]:].lstrip()
    if not text:
        return labels, ""
    _, rest = _consume_stmt_src_c(text)
    return labels, rest


def _list_has_label(text: str, name: str) -> bool:
    """Is `name` the label of a statement of this list (not a nested one)?"""
    try:
        t = text
        for _ in range(100000):
            if not t.strip():
                return False
            labels, rest = _next_labeled_stmt(t)
            if name in labels:
                return True
            if len(rest) >= len(t):
                return False
            t = rest
    except ParseFail:
        pass
    return False


def _goto_rx(name: str) -> str:
    return r"\bgoto\s+" + name + r"\s*;"


def _goto_complete(e: _Enc, st: _State, cur: _State) -> _State:
    """A goto state completed for a merge at its label (`cur`: the state
    there). A name the label sees that was declared after the goto is
    indeterminate on the goto's path: not modelled, never a guess. A scalar
    the goto state never read still holds its initial symbol (_Enc.get)."""
    out = _State(st.path, dict(st.vars), dict(st.arrays), dict(st.uninit))
    for k in cur.vars:
        if k not in out.vars:
            out.vars[k] = z3.BitVec(k, e.type_of(k)[0])
    for k in cur.arrays:
        if k not in out.arrays:
            raise ParseFail(f"unstructured goto unencoded: array {k} declared past the goto")
    for k in cur.uninit:
        if k not in out.uninit:
            out.uninit[k] = z3.BoolVal(False)
    return out


def _goto_merge_union(e: _Enc, states: list[_State]) -> _State:
    """Merge states whose visible names may differ (the exits of a goto
    loop): over the union of their names."""
    keys = _State(states[0].path, dict(states[0].vars), dict(states[0].arrays),
                  dict(states[0].uninit))
    for st in states:
        for k, v in st.vars.items():
            keys.vars.setdefault(k, v)
        for k, a in st.arrays.items():
            keys.arrays.setdefault(k, a)
        for k, u in st.uninit.items():
            keys.uninit.setdefault(k, u)
    done = [_goto_complete(e, st, keys) for st in states]
    return _merge_states(e, done, done[0])


def _check_sat(e: _Enc, cond: Any) -> Any:
    s = z3.Solver()
    s.set("timeout", 2000)
    s.add(e.path_true)
    s.add(cond)
    return s.check()


class Parser:
    """Recursive-descent over a statement list. Not a C compiler."""

    def __init__(
        self,
        body: str,
        params: list[tuple[str, str]],
        unwind: int,
        enums: dict[str, int] | None = None,
        macros: dict[str, str] | None = None,
        havoc: bool = False,
    ) -> None:
        self.body = body
        self.params = params
        self.unwind = unwind
        self.enums: dict[str, int] = dict(enums or {})
        self.macros: dict[str, str] = dict(macros or {})
        self.havoc = havoc
        self.cxx = False  # C++ source (_Enc.cxx)
        # Shift rules (_apply_binop): 0 = C (and C++98/03), 11 = C++11..17
        # (CWG 1457: E1 >= 0 and E1 * 2^E2 representable in the unsigned
        # type), 20 = C++20 and later (P1236: only the count can be undefined).
        self.shift_rules = 0
        self.err: str | None = None
        # goto (structured patterns only). A forward goto keeps its state
        # until its label, which must be a later statement of the goto's own
        # statement list or of an enclosing one (a jump out of blocks, loops
        # and switches); a backward goto must lie in the statements from its
        # label to the end of the label's list: those statements are unwound
        # like a loop body. Anything else is "unstructured goto unencoded".
        self._pending_gotos: dict[str, list[tuple[_State, list[list[str]]]]] = {}
        self._label_loops: list[tuple[str, list[_State]]] = []
        self._lists: list[list[str]] = []  # [unconsumed text] of each active statement list

    def run(self) -> _Enc | None:
        if not HAS_Z3:
            return None
        e = _Enc(self.unwind)
        e.havoc = self.havoc
        e.cxx = self.cxx
        e.shift_rules = self.shift_rules
        try:
            for typ, name in self.params:
                if not name:
                    continue
                t = _ctype_parse(typ)
                if t is None:
                    raise ParseFail(f"UNENCODED: parameter type '{(typ or '').strip()}' of {name}")
                e.declare(name, t)
                e.get(name)  # unconstrained input
            e.push_scope()
            self._stmts(e, self._prep(self.body))
            e.pop_scope()
            if self._pending_gotos:
                raise ParseFail(
                    f"unstructured goto unencoded: label {min(self._pending_gotos)} not reached")
        except ParseFail as ex:
            self.err = str(ex)
            return None
        except z3.Z3Exception as ex:
            self.err = f"z3: {ex}"
            return None
        return e

    def _prep(self, body: str) -> str:
        body = re.sub(r"#.*", " ", body)
        return body

    def _block(self, e: _Enc, text: str) -> None:
        e.push_scope()
        self._stmts(e, text)
        e.pop_scope()

    def _stmts(self, e: _Enc, text: str) -> None:
        cell = [text.strip()]
        self._lists.append(cell)
        try:
            self._stmts_in(e, cell)
        finally:
            self._lists.pop()

    def _stmts_in(self, e: _Enc, cell: list[str]) -> None:
        text = cell[0]
        while text:
            cell[0] = text
            text = text.lstrip()
            if not text:
                break
            if text.startswith("{"):
                inner, rest = _brace(text)
                self._block(e, inner)
                text = rest
                continue
            if _re_match(r"if\s+constexpr\b", text):
                raise ParseFail("if constexpr unencoded")
            if _re_match(r"constexpr\b", text):
                raise ParseFail("constexpr unencoded")
            if "<=>" in text:
                raise ParseFail("spaceship unencoded")
            if _re_search(r"__attribute__\s*\(\s*\(\s*cleanup", text):
                raise ParseFail("cleanup unencoded")
            if _re_search(r"\bstd\s*::\s*expected\b|\bexpected\s*<", text):
                raise ParseFail("expected unencoded")
            if _re_search(r"\bformat_to(?:_n)?\s*\(", text):
                raise ParseFail("format_to unencoded")
            if _re_search(r"\bstd\s*::\s*(?:format|print|println)\b", text):
                raise ParseFail("format unencoded")
            if _re_search(r"\bstd\s*::\s*jthread\b", text):
                raise ParseFail("jthread unencoded")
            if _re_search(
                r"\bstd\s*::\s*(?:async|future|promise)\b"
                r"|(?:future|promise)\s*<",
                text,
            ):
                raise ParseFail("async unencoded")
            if _re_search(r"\bstd\s*::\s*function\b|function\s*<", text):
                raise ParseFail("function unencoded")
            if _re_search(r"\bstd\s*::\s*mdspan\b|mdspan\s*<", text):
                raise ParseFail("mdspan unencoded")
            if _re_search(
                r"\bstd\s*::\s*(?:mutex|lock_guard|unique_lock|scoped_lock)\b"
                r"|(?:lock_guard|unique_lock|scoped_lock)\s*<",
                text,
            ):
                raise ParseFail("std mutex unencoded")
            if _re_search(r"\bcondition_variable_any\b", text):
                raise ParseFail("condition_variable_any unencoded")
            if _re_search(r"\bshared_timed_mutex\b", text):
                raise ParseFail("shared_timed_mutex unencoded")
            if _re_search(r"\brecursive_timed_mutex\b", text):
                raise ParseFail("recursive_timed_mutex unencoded")
            if _re_search(r"\berror_category\b", text):
                raise ParseFail("error_category unencoded")
            if _re_search(r"\bnested_exception\b", text):
                raise ParseFail("nested_exception unencoded")
            if _re_search(r"\bwstring_convert\b", text):
                raise ParseFail("wstring_convert unencoded")
            if _re_search(r"\bsystem_error\b", text):
                raise ParseFail("system_error unencoded")
            if _re_search(r"\b(?:current_zone|tzdb)\b", text):
                raise ParseFail("tzdb unencoded")
            if _re_search(r"\bis_scoped_enum\b", text):
                raise ParseFail("is_scoped_enum unencoded")
            if _re_search(r"\b(?:views\s*::\s*)?enumerate(?:_view)?\b", text):
                raise ParseFail("enumerate unencoded")
            if _re_search(r"\bcartesian_product(?:_view)?\b", text):
                raise ParseFail("cartesian_product unencoded")
            if _re_search(
                r"\b(?:views\s*::\s*chunk(?:_by)?|chunk(?:_by|_view))\b",
                text,
            ):
                raise ParseFail("chunk unencoded")
            if _re_search(r"\b(?:views\s*::\s*slide|slide_view)\b", text):
                raise ParseFail("slide unencoded")
            if _re_search(
                r"\b(?:views\s*::\s*adjacent(?:_transform)?|"
                r"adjacent(?:_transform|_view))\b",
                text,
            ):
                raise ParseFail("adjacent unencoded")
            if _re_search(r"\bjoin_with(?:_view)?\b", text):
                raise ParseFail("join_with unencoded")
            if _re_search(r"views\s*::\s*join|\bjoin_view\b", text):
                raise ParseFail("views::join unencoded")
            if _re_search(r"\bzip_transform(?:_view)?\b", text):
                raise ParseFail("zip_transform unencoded")
            if _re_search(r"views\s*::\s*zip|\bzip_view\b", text):
                raise ParseFail("views::zip unencoded")
            if _re_search(r"\bas_rvalue(?:_view)?\b", text):
                raise ParseFail("as_rvalue unencoded")
            if _re_search(r"\bfrom_range\b", text):
                raise ParseFail("from_range unencoded")
            if _re_search(r"\b(?:views\s*::\s*)?stride(?:_view)?\b", text):
                raise ParseFail("stride unencoded")
            if _re_search(r"\b(?:views\s*::\s*repeat|repeat_view)\b", text):
                raise ParseFail("repeat unencoded")
            if _re_search(r"\b(?:views\s*::\s*take\b|\btake_view\b)", text):
                raise ParseFail("take unencoded")
            if _re_search(r"\b(?:views\s*::\s*drop\b|\bdrop_view\b)", text):
                raise ParseFail("drop unencoded")
            if _re_search(r"\b(?:views\s*::\s*filter\b|\bfilter_view\b)", text):
                raise ParseFail("filter unencoded")
            if _re_search(
                r"\b(?:views\s*::\s*transform\b|\btransform_view\b)",
                text,
            ):
                raise ParseFail("transform_view unencoded")
            if _re_search(
                r"\b(?:views\s*::\s*elements\b|\belements_view\b)",
                text,
            ):
                raise ParseFail("elements unencoded")
            if _re_search(r"\b(?:views\s*::\s*iota\b|\biota_view\b)", text):
                raise ParseFail("iota unencoded")
            if _re_search(r"\breference_wrapper\b", text):
                raise ParseFail("reference_wrapper unencoded")
            if _re_search(r"\bstd\s*::\s*endian\b", text):
                raise ParseFail("std::endian unencoded")
            if _re_search(r"\bstd\s*::\s*apply\s*\(", text):
                raise ParseFail("std::apply unencoded")
            if _re_search(
                r"\b(?:bit_ceil|bit_floor|has_single_bit|"
                r"std\s*::\s*popcount)\s*\(",
                text,
            ):
                raise ParseFail("bit_ceil unencoded")
            if _re_search(r"\bstd\s*::\s*bit_width\s*\(", text):
                raise ParseFail("bit_width unencoded")
            if _re_search(r"\bstd\s*::\s*gcd\s*\(", text):
                raise ParseFail("gcd unencoded")
            if _re_search(r"\bstd\s*::\s*lcm\s*\(", text):
                raise ParseFail("lcm unencoded")
            if _re_search(r"\bstd\s*::\s*clamp\s*\(", text):
                raise ParseFail("clamp unencoded")
            if _re_search(r"\bstd\s*::\s*exchange\s*\(", text):
                raise ParseFail("exchange unencoded")
            if _re_search(r"\bstd\s*::\s*to_address\s*\(", text):
                raise ParseFail("to_address unencoded")
            if _re_search(r"\bstd\s*::\s*addressof\s*\(", text):
                raise ParseFail("addressof unencoded")
            if _re_search(r"\bassume_aligned\s*\(", text):
                raise ParseFail("assume_aligned unencoded")
            if _re_search(r"\bas_const\s*\(", text):
                raise ParseFail("as_const unencoded")
            if _re_search(r"\btransform_(?:inclusive|exclusive)_scan\s*\(", text):
                raise ParseFail("transform_inclusive_scan unencoded")
            if _re_search(r"\bexclusive_scan\s*\(", text):
                raise ParseFail("exclusive_scan unencoded")
            if _re_search(r"\binclusive_scan\s*\(", text):
                raise ParseFail("inclusive_scan unencoded")
            if _re_search(r"\btransform_reduce\s*\(", text):
                raise ParseFail("transform_reduce unencoded")
            if _re_search(r"\bstd\s*::\s*reduce\s*\(", text):
                raise ParseFail("std::reduce unencoded")
            if _re_search(
                r"\buninitialized_(?:fill(?:_n)?|default_construct(?:_n)?)\s*\(",
                text,
            ):
                raise ParseFail("uninitialized_fill unencoded")
            if _re_search(r"\buninitialized_value_construct(?:_n)?\s*\(", text):
                raise ParseFail("uninitialized_value_construct unencoded")
            if _re_search(r"\buninitialized_(?:copy|move)(?:_n)?\s*\(", text):
                raise ParseFail("uninitialized_copy unencoded")
            if _re_search(r"\b(?:construct_at|destroy_at)\s*\(", text):
                raise ParseFail("construct_at unencoded")
            if _re_search(r"\bdestroy_n\s*\(", text):
                raise ParseFail("destroy_n unencoded")
            if _re_search(
                r"\b(?:add_sat|sub_sat|mul_sat|div_sat|saturate_cast)\s*\(",
                text,
            ):
                raise ParseFail("add_sat unencoded")
            if _re_search(r"\btype_identity\b", text):
                raise ParseFail("type_identity unencoded")
            if _re_search(r"\bnontype\b", text):
                raise ParseFail("nontype unencoded")
            if _re_search(r"\bis_layout_compatible\b", text):
                raise ParseFail("is_layout_compatible unencoded")
            if _re_search(r"\bis_pointer_interconvertible_(?:with_class|base_of)\b", text):
                raise ParseFail("is_pointer_interconvertible unencoded")
            if _re_search(r"\bbasic_const_iterator\b", text):
                raise ParseFail("basic_const_iterator unencoded")
            if _re_search(r"\bis_corresponding_member\b", text):
                raise ParseFail("is_corresponding_member unencoded")
            if _re_search(r"\branges\s*::\s*to\s*[<(]", text):
                raise ParseFail("ranges::to unencoded")
            if _re_search(r"\bforward_like\b", text):
                raise ParseFail("forward_like unencoded")
            if _re_search(r"\bmake_exception_ptr\s*\(", text):
                raise ParseFail("make_exception_ptr unencoded")
            if _re_search(r"\b(?:set|get)_terminate\s*\(", text):
                raise ParseFail("set_terminate unencoded")
            if _re_search(r"\bis_constant_evaluated\s*\(", text):
                raise ParseFail("is_constant_evaluated unencoded")
            if _re_search(r"\bstd\s*::\s*lerp\s*\(", text):
                raise ParseFail("lerp unencoded")
            if _re_search(r"\bstd\s*::\s*midpoint\s*\(", text):
                raise ParseFail("midpoint unencoded")
            if _re_search(
                r"\bstd\s*::\s*(?:cmp_(?:less|greater|less_equal|"
                r"greater_equal|equal_to|not_equal_to)|in_range)\s*\(",
                text,
            ):
                raise ParseFail("cmp_less unencoded")
            if _re_search(r"\bstd\s*::\s*count[lr]_(?:zero|one)\s*\(", text):
                raise ParseFail("countl_zero unencoded")
            if _re_search(r"\bstd\s*::\s*unreachable\s*\(", text):
                raise ParseFail("std::unreachable unencoded")
            if _re_search(r"\buncaught_exceptions\s*\(", text):
                raise ParseFail("uncaught_exceptions unencoded")
            if _re_search(
                r"\bstd\s*::\s*(?:condition_variable|shared_mutex)\b"
                r"|\b(?:condition_variable|shared_mutex)\b",
                text,
            ):
                raise ParseFail("condition_variable unencoded")
            if _re_search(r"\bstd\s*::\s*atomic_ref\b|atomic_ref\s*<", text):
                raise ParseFail("atomic_ref unencoded")
            if _re_search(r"\bstd\s*::\s*generator\b|generator\s*<", text):
                raise ParseFail("generator unencoded")
            if _re_search(r"\[\[\s*assume\s*\(", text):
                raise ParseFail("assume unencoded")
            if _re_search(r"\bstd\s*::\s*bind\s*\(", text):
                raise ParseFail("std bind unencoded")
            if _re_search(
                r"__attribute__\s*\(\s*\(\s*(?:__)?vector_size"
                r"|\b__vector_size\b",
                text,
            ):
                raise ParseFail("vector_size unencoded")
            if _re_match(r"requires\s*\(", text) or _re_match(r"concept\s+", text):
                raise ParseFail("concepts unencoded")
            if _re_search(r"\(\s*\.\.\.\s*[+\-|&^]|[+\-|&^]\s*\.\.\.\s*\)", text):
                raise ParseFail("fold unencoded")
            if _starts_kw(text, "if"):
                text = self._if(e, text)
                continue
            if _starts_kw(text, "switch"):
                text = self._switch(e, text)
                continue
            if _starts_kw(text, "do"):
                text = self._do(e, text)
                continue
            if _starts_kw(text, "while"):
                text = self._while(e, text)
                continue
            if _starts_kw(text, "for"):
                text = self._for(e, text)
                continue
            if _starts_kw(text, "assert"):
                text = self._assert(e, text)
                continue
            if (_starts_kw(text, "static_assert")
                    or _starts_kw(text, "_Static_assert")):
                _, text = _stmt(text)
                continue
            if _starts_kw(text, "return"):
                stmt, text = _stmt(text)
                self._return(e, stmt)
                continue
            if _starts_kw(text, "break"):
                _, text = _stmt(text)
                if not e.jumps:
                    raise ParseFail("break outside loop/switch")
                e.jumps[-1].breaks.append(e.snap())
                e.path_true = z3.BoolVal(False)
                continue
            if _starts_kw(text, "continue"):
                _, text = _stmt(text)
                loops = [j for j in e.jumps if j.loop]
                if not loops:
                    raise ParseFail("continue outside loop")
                loops[-1].conts.append(e.snap())
                e.path_true = z3.BoolVal(False)
                continue
            if _is_nested_function(text):
                raise ParseFail("nested function unencoded")
            if _starts_kw(text, "goto"):
                if _is_computed_goto(text):
                    raise ParseFail("computed goto unencoded")
                stmt, text = _stmt(text)
                gm = re.match(r"goto\s+([A-Za-z_]\w*)\s*;\s*$", stmt)
                if gm is None:
                    raise ParseFail(f"unstructured goto unencoded: {stmt.strip()}")
                cell[0] = text
                self._goto(e, gm.group(1))
                continue
            lab = _label_at(text)
            if lab is not None:
                cell[0] = text[lab[1]:]
                self._label(e, lab[0], cell)
                text = cell[0]
                continue
            if _starts_kw(text, "throw"):
                raise ParseFail("throw unencoded")
            if (_starts_kw(text, "asm") or _starts_kw(text, "__asm__")
                    or _starts_kw(text, "__asm")):
                raise ParseFail("asm unencoded")
            if _starts_kw(text, "try") or _starts_kw(text, "catch"):
                raise ParseFail("try unencoded")
            if _starts_kw(text, "case") or _starts_kw(text, "default"):
                raise ParseFail("case/default outside switch")
            miss = unencoded_layout_prefix(text)
            if miss:
                raise ParseFail(miss)
            stmt, text = _stmt(text)
            miss = unencoded_layout_stmt(stmt)
            if miss:
                raise ParseFail(miss)
            if _looks_like_cdecl(stmt):
                self._decl(e, stmt)
            else:
                self._assign_or_expr(e, stmt)

    def _goto(self, e: _Enc, name: str) -> None:
        # Backward: a goto inside the statements its label heads (innermost first).
        for lname, backs in reversed(self._label_loops):
            if lname == name:
                backs.append(e.snap())
                e.path_true = z3.BoolVal(False)
                return
        # Forward: the label is a later statement of this list or an enclosing one.
        if not any(_list_has_label(c[0], name) for c in reversed(self._lists)):
            raise ParseFail(
                f"unstructured goto unencoded: goto {name} is not a jump out to a later statement")
        self._pending_gotos.setdefault(name, []).append(
            (e.snap(), [list(sc) for sc in e.scopes]))
        e.path_true = z3.BoolVal(False)

    def _label(self, e: _Enc, name: str, cell: list[str]) -> None:
        pend = self._pending_gotos.pop(name, None)
        if pend:
            cur = e.snap()
            states = [cur]
            for st, scopes in pend:
                # The label's list is the goto's or an enclosing one, so its
                # scopes are a prefix of the goto's: a name added to them since
                # was declared between the goto and the label.
                if len(scopes) < len(e.scopes):
                    raise ParseFail("unstructured goto unencoded: jump into a block")
                for d, frame in enumerate(e.scopes):
                    for n in frame:
                        if n not in scopes[d]:
                            raise ParseFail(
                                f"unstructured goto unencoded: goto {name} jumps past "
                                f"the declaration of {n}")
                states.append(_goto_complete(e, st, cur))
            e.load(_merge_states(e, states, cur))
        text = cell[0]
        if not re.search(_goto_rx(name), text):
            return
        # Backward gotos: the region is every statement from the label up to
        # the last one that contains `goto name`; it runs like a loop body
        # whose `goto name` is a continue.
        region, after = "", text
        guard = 0
        while guard < 100000 and re.search(_goto_rx(name), after):
            guard += 1
            _, rest = _next_labeled_stmt(after)
            if len(rest) >= len(after):
                raise ParseFail(f"unstructured goto unencoded: goto {name}")
            region += after[:len(after) - len(rest)]
            after = rest
        cell[0] = after  # the enclosing list continues after the region
        if e.havoc:
            # Unbounded step, as _loop_havoc: every name the region may assign
            # is arbitrary at the label; one pass is checked from there.
            for n in sorted(_assigned_names(region)):
                if n.startswith("@"):
                    an = e.canonical(n[1:])
                    arr = e.arrays.get(an)
                    if arr is not None:
                        e.fresh += 1
                        e.arrays[an] = Arr(
                            z3.Const(f"{an}_hv{e.fresh}", arr.a.sort()), arr.n, arr.w, arr.u,
                        )
                        e.havoc_shadow(an, f"{an}_hvu")
                    continue
                if n not in e.bits:
                    continue
                w, _u = e.type_of(n)
                e.vars[n] = e.bv(f"{n}_hv{e.fresh + 1}", w)
                flag = e.uninit.get(n)
                if flag is not None:
                    e.fresh += 1
                    e.uninit[n] = z3.And(flag, z3.Bool(f"{n}_hvu{e.fresh}"))
            self._label_loops.append((name, []))
            try:
                self._stmts(e, region)
            finally:
                self._label_loops.pop()  # re-entries are covered by the havoc
            return
        base = e.snap()
        exits: list[_State] = []
        closed = False
        frame: tuple[str, list[_State]] = (name, [])
        self._label_loops.append(frame)
        try:
            for _ in range(max(e.unwind, 1)):
                frame[1].clear()
                self._stmts(e, region)
                exits.append(e.snap())
                live = [_goto_complete(e, b, base) for b in frame[1]
                        if not z3.is_false(b.path)]
                frame[1].clear()
                if not live:
                    closed = True
                    break
                e.load(_merge_states(e, live, base))
                if _check_sat(e, z3.BoolVal(True)) == z3.unsat:
                    closed = True
                    break
        finally:
            self._label_loops.pop()
        if not closed:
            e.unwind_ok = False  # still jumping back after `unwind` passes: cut
        e.load(_goto_merge_union(e, exits))

    def _decl(self, e: _Enc, stmt: str) -> None:
        stmt = stmt.rstrip(";").strip()
        parts = _split_comma(stmt)
        if len(parts) <= 1:
            self._decl_one(e, stmt)
            return
        # int x = 1, y, z = 2;  ->  one declaration per declarator.
        m = _re_match("(" + _CDECL_TYPE + r")\s+[A-Za-z_*]", parts[0])
        if not m:
            raise ParseFail(f"unparsed decl: {stmt[:80]}")
        prefix = m.group(1)
        self._decl_one(e, parts[0].strip())
        for part in parts[1:]:
            self._decl_one(e, prefix + " " + part.strip())

    def _decl_one(self, e: _Enc, stmt: str) -> None:
        def shadow(name: str) -> None:
            if e.declared(name):
                raise ParseFail(f"UNENCODED: shadowed declaration of {name}")

        # int *p = buf;  (harness pointer alias of a local array)
        mptr = _rx(_CDECL_TYPE + r"\s*\*+\s*([A-Za-z_]\w*)(?:\s*=\s*(.*))?$").match(stmt)
        if mptr and mptr.end() == len(stmt):
            name, init = mptr.group(1), mptr.group(2)
            if not init:
                raise ParseFail(f"uninitialised pointer decl: {stmt[:80]}")
            src = init.strip()
            if _is_ident(src) and e.is_array(src):
                shadow(name)
                e.declare_alias(name, src)
                return
            raise ParseFail(f"pointer decl must alias an array: {stmt[:80]}")
        # int a[n];  VLA is a missing bound, not a closed proof.
        marr = _rx("(" + _CDECL_TYPE + r")\s+([A-Za-z_]\w*)\s*\[([^\]]+)\](?:\s*=\s*(.*))?$").match(stmt)
        if marr and marr.end() == len(stmt):
            typ, name, dim, init = marr.group(1), marr.group(2), marr.group(3).strip(), marr.group(4)
            if init and _re_search(
                r"\{\s*(?:\[[^\]]+\]|\.[A-Za-z_]\w*)\s*=",
                init,
            ):
                raise ParseFail("designated init unencoded")
            if not re.fullmatch(r"\d+", dim):
                raise ParseFail("VLA unencoded")
            ct = _ctype_parse(typ)
            if ct is None:
                raise ParseFail(f"UNENCODED: element type of {name}")
            shadow(name)
            n = int(dim)
            init = (init or "").strip()
            if init.startswith("{") and init.endswith("}"):
                # Aggregate initialiser (C11 6.7.9p21): the listed elements in
                # order, every other element zero. The items are evaluated
                # (their UB is checked) before the name is in scope.
                items = [x.strip() for x in _split_comma(init[1:-1])]
                if items and not items[-1]:
                    items.pop()  # trailing comma, or {}
                if any(not x or x.startswith("{") for x in items):
                    raise ParseFail(f"UNENCODED: nested initialiser of {name}")
                if len(items) > n:
                    raise ParseFail(f"UNENCODED: excess initialisers of {name}")
                vals = [self._expr(e, x) for x in items]
                arr = z3.K(z3.BitVecSort(WIDTH), z3.BitVecVal(0, ct[0]))
                for k, v in enumerate(vals):
                    arr = z3.Store(arr, z3.BitVecVal(k, WIDTH), e.conv(v, ct).v)
                e.declare_array(name, Arr(arr, n, ct[0], ct[1]))
                e.shadow_init(name, True)
                return
            # `= __prism_unconstrained`: a caller's object materialised by a
            # drafted harness (src/prism/ai/harness.cpp): any contents, written.
            if init == "__prism_unconstrained":
                pass
            elif init and not (ct[0] == 8 and _STRING_LIT.fullmatch(init)):
                raise ParseFail(f"UNENCODED: initialiser of {name}")
            # No initialiser: contents arbitrary, every element unwritten. A
            # string literal writes every element (6.7.9p14, p21); its
            # characters are not modelled (arbitrary contents: a sound
            # over-approximation, never a proof about the values).
            e.fresh += 1
            arr = z3.Array(f"{name}_arr{e.fresh}", z3.BitVecSort(WIDTH), z3.BitVecSort(ct[0]))
            e.declare_array(name, Arr(arr, n, ct[0], ct[1]))
            # `_h_p`: the harness buffer standing for the caller's object
            # behind pointer parameter p (prism/harness.py). Its contents are
            # the caller's, assumed initialised like the `// requires:` size.
            e.shadow_init(name, bool(init) or name.startswith("_h_"))
            return
        # int x = 0;  unsigned n;  int x;
        m = _rx("(" + _CDECL_TYPE + r")\s+([A-Za-z_]\w*)(?:\s*=\s*(.*))?$").match(stmt)
        if not m or m.end() != len(stmt):
            if _re_search(r":\s*[A-Za-z_]", stmt):
                raise ParseFail("range-for unencoded")
            raise ParseFail(f"unparsed decl: {stmt[:80]}")
        typ, name, init = m.group(1), m.group(2), m.group(3)
        ct = _ctype_parse(typ)
        if ct is None:
            raise ParseFail(f"UNENCODED: type of {name}")
        shadow(name)
        if init is not None:
            # The initialiser is evaluated before the name is in scope.
            val = self._expr(e, init)
            e.declare(name, ct)
            e.set(name, val)
            e.mark_init(name)
        else:
            e.declare(name, ct)
            v = e.bv(name + "_uninit", ct[0])
            e.set(name, TV(v, ct[0], ct[1]))
            e.mark_uninit(name)

    def _assign_or_expr(self, e: _Enc, stmt: str) -> None:
        stmt = stmt.rstrip(";").strip()
        if not stmt:
            return
        parts = _split_comma(stmt)
        if len(parts) > 1:
            for part in parts:
                piece = part.strip()
                if piece:
                    self._assign_or_expr(e, piece)
            return
        # *p = e  (single-element store through a pointer alias)
        m = _re_match(r"\*\s*([A-Za-z_]\w*)\s*=\s*(.+)$", stmt)
        if m and m.end() == len(stmt) and not m.group(2).startswith("="):
            self._astore(e, m.group(1), "0", m.group(2))
            return
        # a[i] = e
        m = _re_match(r"([A-Za-z_]\w*)\s*\[(.+)\]\s*=\s*(.+)$", stmt)
        if m and m.end() == len(stmt) and not m.group(3).startswith("="):
            self._astore(e, m.group(1), m.group(2), m.group(3))
            return
        m = _re_match(r"([A-Za-z_]\w*)\s*((?:<<|>>|[-+*/%|&^])?=)\s*(.+)$", stmt)
        if m and m.end() == len(stmt) and not m.group(3).startswith("="):
            name, op, rhs = m.group(1), m.group(2), m.group(3)
            if name not in e.bits:
                if e.declared(name):
                    raise ParseFail(f"UNENCODED: assignment to array/pointer {name}")
                raise ParseFail(f"UNENCODED: assignment to undeclared {name}")
            val = self._expr(e, rhs)
            if op == "=":
                e.set(name, val)
            else:
                e.check_read(name)
                cur = e.get(name)
                e.set(name, self._binop(e, cur, op[:-1], val, stmt))
            e.mark_init(name)
            return
        # expression statement
        self._expr(e, stmt)

    def _astore(self, e: _Enc, name0: str, idx: str, rhs: str) -> None:
        name = e.canonical(name0)
        if name not in e.arrays:
            raise ParseFail(f"UNENCODED: store to unknown array {name0}")
        i = self._expr(e, idx)
        v = self._expr(e, rhs)
        arr = e.arrays[name]
        e.add_prop("oob-write", "MEM-OOB-WRITE", _oob(e, i, arr.n), e.pc)
        e.arrays[name] = Arr(
            z3.Store(arr.a, _index32(e, i), e.conv(v, (arr.w, arr.u)).v), arr.n, arr.w, arr.u,
        )
        e.mark_elem_init(name, _index32(e, i))

    def _assert(self, e: _Enc, text: str) -> str:
        m = _re_match(r"assert\s*\((.*)\)\s*;", text, re.S)
        if not m:
            # assert(x); may span
            inner, rest = _paren_stmt(text[text.find("("):])
            cond = self._expr(e, inner)
            e.add_prop("assert", "FUNC-CONTRACT", z3.Not(_as_bool(cond)), e.pc)
            return rest
        cond = self._expr(e, m.group(1))
        e.add_prop("assert", "FUNC-CONTRACT", z3.Not(_as_bool(cond)), e.pc)
        return text[m.end():]

    def _return(self, e: _Enc, stmt: str) -> None:
        rest = stmt.strip()
        if rest.startswith("return"):
            expr = rest[6:].strip().rstrip(";").strip()
            if expr:
                val = self._expr(e, expr)
                e.vars["__ret"] = val.v
        e.path_true = z3.BoolVal(False)

    def _if(self, e: _Enc, text: str) -> str:
        rest = text[2:].lstrip()
        if rest.startswith("constexpr"):
            raise ParseFail("if constexpr unencoded")
        cond_src, rest = _paren(rest)
        then_src, rest = _block_or_stmt_c(rest)
        else_src = None
        rest2 = rest.lstrip()
        if _starts_kw(rest2, "else"):
            else_src, rest = _block_or_stmt_c(rest2[4:])
        cond = _as_bool(self._expr(e, cond_src))
        base = e.snap()
        e.path_true = z3.And(base.path, cond)
        self._block(e, then_src)
        then_st = e.snap()
        e.load(base)
        e.path_true = z3.And(base.path, z3.Not(cond))
        if else_src is not None:
            self._block(e, else_src)
        else_st = e.snap()
        e.load(_merge_states(e, [then_st, else_st], base))
        return rest

    def _run_body(self, e: _Enc, body: str) -> None:
        """One execution of a loop body; `continue` states rejoin at its end."""
        base = e.snap()
        e.jumps[-1].conts = []
        self._block(e, body)
        conts = e.jumps[-1].conts
        if conts:
            states = list(conts) + [e.snap()]
            e.jumps[-1].conts = []
            e.load(_merge_states(e, states, base))

    def _loop(self, e: _Enc, kind: str, cond_src: str, body: str, incr: str) -> None:
        """Bounded unrolling.

        Every iteration's exit (condition false) and every break is kept and
        merged after the loop; a path still looping after `unwind`
        iterations is cut and makes the result BOUNDED (unwind_ok=False).
        """
        if e.havoc:
            self._loop_havoc(e, kind, cond_src, body, incr)
            return
        base = e.snap()
        e.jumps.append(_Jump(True))
        exits: list[_State] = []
        iters = e.unwind
        if kind == "do":
            self._run_body(e, body)
            iters = max(e.unwind - 1, 0)
        stopped = False
        for _ in range(iters):
            cond = _as_bool(self._expr(e, cond_src))
            ex = e.snap()
            ex.path = z3.And(e.path_true, z3.Not(cond))
            exits.append(ex)
            e.path_true = z3.And(e.path_true, cond)
            if _check_sat(e, z3.BoolVal(True)) == z3.unsat:
                stopped = True
                break
            self._run_body(e, body)
            if incr:
                self._assign_or_expr(e, incr)
        if not stopped:
            cond = _as_bool(self._expr(e, cond_src))
            if _check_sat(e, cond) != z3.unsat:
                e.unwind_ok = False
            ex = e.snap()
            ex.path = z3.And(e.path_true, z3.Not(cond))
            exits.append(ex)
        j = e.jumps.pop()
        exits.extend(j.breaks)
        e.load(_merge_states(e, exits, base))

    def _loop_havoc(self, e: _Enc, kind: str, cond_src: str, body: str, incr: str) -> None:
        """Unbounded step: every variable (and array) the loop may assign is
        made arbitrary, the condition and one body execution are checked
        from that state, and the loop is left with the condition false (or
        by break). Every concrete iteration starts in a havocked state, so no
        UB inside the loop is missed and every post-loop state is covered.
        """
        if _const_false_cond(cond_src):
            if kind != "do":
                return
            base = e.snap()
            e.jumps.append(_Jump(True))
            self._run_body(e, body)
            j = e.jumps.pop()
            e.load(_merge_states(e, list(j.breaks) + [e.snap()], base))
            return
        names = _assigned_names(cond_src + ";\n" + body + ";\n" + incr)
        for n in sorted(names):
            if n.startswith("@"):
                an = e.canonical(n[1:])
                arr = e.arrays.get(an)
                if arr is not None:
                    e.fresh += 1
                    e.arrays[an] = Arr(
                        z3.Const(f"{an}_hv{e.fresh}", arr.a.sort()), arr.n, arr.w, arr.u,
                    )
                    e.havoc_shadow(an, f"{an}_hvu")
                continue
            if n not in e.bits:
                continue
            w, _u = e.type_of(n)
            e.vars[n] = e.bv(f"{n}_hv{e.fresh + 1}", w)
            flag = e.uninit.get(n)
            if flag is not None:
                e.fresh += 1
                e.uninit[n] = z3.And(flag, z3.Bool(f"{n}_hvu{e.fresh}"))
        base = e.snap()
        e.jumps.append(_Jump(True))
        exits: list[_State] = []
        if kind == "do":
            self._run_body(e, body)
            cond = _as_bool(self._expr(e, cond_src))
            ex = e.snap()
            ex.path = z3.And(e.path_true, z3.Not(cond))
            exits.append(ex)
        else:
            cond = _as_bool(self._expr(e, cond_src))
            ex = e.snap()
            ex.path = z3.And(e.path_true, z3.Not(cond))
            exits.append(ex)
            e.path_true = z3.And(e.path_true, cond)
            self._run_body(e, body)
            if incr:
                self._assign_or_expr(e, incr)
        j = e.jumps.pop()
        exits.extend(j.breaks)
        e.load(_merge_states(e, exits, base))

    def _while(self, e: _Enc, text: str) -> str:
        rest = text[5:].lstrip()
        cond_src, rest = _paren(rest)
        body, rest = _block_or_stmt_c(rest)
        self._loop(e, "while", cond_src, body, "")
        return rest

    def _do(self, e: _Enc, text: str) -> str:
        rest = text[2:].lstrip()
        body, rest = _block_or_stmt_c(rest)
        rest = rest.lstrip()
        if not _starts_kw(rest, "while"):
            raise ParseFail("do without while")
        rest = rest[5:].lstrip()
        cond_src, rest = _paren(rest)
        rest = rest.lstrip()
        if rest.startswith(";"):
            rest = rest[1:]
        self._loop(e, "do", cond_src, body, "")
        return rest

    def _for(self, e: _Enc, text: str) -> str:
        rest = text[3:].lstrip()
        head, rest = _paren(rest)
        parts = _split_semi(head)
        while len(parts) < 3:
            parts.append("")
        if len(parts) > 3:
            raise ParseFail("unparsed for header")
        init, cond_src, incr = (p.strip() for p in parts)
        body, rest = _block_or_stmt_c(rest)
        e.push_scope()
        if init:
            init_stmt = init if init.endswith(";") else init + ";"
            miss = unencoded_layout_stmt(init_stmt)
            if miss:
                raise ParseFail(miss)
            if _looks_like_cdecl(init_stmt):
                self._decl(e, init_stmt)
            else:
                self._assign_or_expr(e, init_stmt)
        incr_stmt = "" if not incr else (incr if incr.endswith(";") else incr + ";")
        self._loop(e, "for", cond_src or "1", body, incr_stmt)
        e.pop_scope()
        return rest

    def _switch(self, e: _Enc, text: str) -> str:
        rest = text[6:].lstrip()
        cond_src, rest = _paren(rest)
        body, rest = _block_or_stmt_c(rest)
        scrut = e.promote(self._expr(e, cond_src))
        arms = self._parse_switch_arms(e, body)
        if not arms:
            return rest
        st = (scrut.w, scrut.u)
        case_eqs = []
        has_default = False
        for arm in arms:
            for lab in arm.labels:
                if lab is None:
                    has_default = True
                else:
                    case_eqs.append(scrut.v == e.conv(lab, st).v)
        any_case = z3.Or(*case_eqs) if case_eqs else z3.BoolVal(False)
        base = e.snap()
        e.jumps.append(_Jump(False))
        taken: list[_State] = []
        for i, arm in enumerate(arms):
            if not arm.labels:
                continue
            parts = [
                z3.Not(any_case) if lab is None else scrut.v == e.conv(lab, st).v
                for lab in arm.labels
            ]
            cond = parts[0] if len(parts) == 1 else z3.Or(*parts)
            e.load(base)
            e.path_true = z3.And(base.path, cond)
            self._block(e, _arm_code(arms, i))
            taken.append(e.snap())
        if not has_default:
            taken.append(_State(z3.And(base.path, z3.Not(any_case)),
                                dict(base.vars), dict(base.arrays), dict(base.uninit)))
        j = e.jumps.pop()
        taken.extend(j.breaks)
        e.load(_merge_states(e, taken, base))
        return rest

    def _parse_switch_arms(self, e: _Enc, body: str) -> list[_SwitchArm]:
        arms: list[_SwitchArm] = []
        labels: list[Any] = []
        chunks: list[str] = []
        stops = False
        text = body

        def flush() -> None:
            nonlocal labels, chunks, stops
            if labels or chunks:
                arms.append(_SwitchArm(list(labels), "\n".join(chunks), stops))
            labels, chunks, stops = [], [], False

        while text:
            text = text.lstrip()
            if not text:
                break
            if _starts_kw(text, "case"):
                if chunks or stops:
                    flush()
                src, text = _upto_colon(text[4:].lstrip())
                if _re_search(r"\.\.\.", src):
                    raise ParseFail("case-range unencoded")
                labels.append(self._expr(e, src))
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
            src, text = _consume_stmt_src_c(text)
            if stops:
                continue
            if src.strip():
                chunks.append(src.strip())
        flush()
        return arms

    def _expr(self, e: _Enc, src: str) -> TV:
        return _parse_expr(e, src.strip(), self)

    def _binop(self, e: _Enc, a: TV, op: str, b: TV, loc: str) -> TV:
        return apply_binop(e, a, op, b)


class ParseFail(Exception):
    pass


class _Continue(Exception):
    """Skip the rest of the current loop body; caught by while/for/do."""


def _split_semi(s: str) -> list[str]:
    parts: list[str] = []
    cur: list[str] = []
    depth = 0
    for ch in s:
        if ch == "(":
            depth += 1
        elif ch == ")":
            depth -= 1
        if ch == ";" and depth == 0:
            parts.append("".join(cur))
            cur = []
        else:
            cur.append(ch)
    parts.append("".join(cur))
    return parts


def _split_comma(s: str) -> list[str]:
    """Split on commas at paren/bracket/brace depth 0."""
    parts: list[str] = []
    cur: list[str] = []
    pdepth = bdepth = 0
    for ch in s:
        if ch == "(":
            pdepth += 1
        elif ch == ")":
            pdepth -= 1
        elif ch in "[{":
            bdepth += 1
        elif ch in "]}":
            bdepth -= 1
        if ch == "," and pdepth == 0 and bdepth == 0:
            parts.append("".join(cur))
            cur = []
        else:
            cur.append(ch)
    parts.append("".join(cur))
    return parts


def _paren(text: str) -> tuple[str, str]:
    text = text.lstrip()
    if not text.startswith("("):
        raise ParseFail("expected (")
    depth = 0
    for i, ch in enumerate(text):
        if ch == "(":
            depth += 1
        elif ch == ")":
            depth -= 1
            if depth == 0:
                return text[1:i], text[i + 1:]
    raise ParseFail("unbalanced (")


def _paren_stmt(text: str) -> tuple[str, str]:
    inner, rest = _paren(text)
    rest = rest.lstrip()
    if rest.startswith(";"):
        rest = rest[1:]
    return inner, rest


def _brace(text: str) -> tuple[str, str]:
    text = text.lstrip()
    if not text.startswith("{"):
        raise ParseFail("expected {")
    depth = 0
    for i, ch in enumerate(text):
        if ch == "{":
            depth += 1
        elif ch == "}":
            depth -= 1
            if depth == 0:
                return text[1:i], text[i + 1:]
    raise ParseFail("unbalanced {")


def _block_or_stmt(text: str) -> tuple[str, str]:
    text = text.lstrip()
    if text.startswith("{"):
        return _brace(text)
    stmt, rest = _stmt(text)
    return stmt, rest


def _stmt(text: str) -> tuple[str, str]:
    depth = 0
    brace = 0  # inside a `= { ... }` aggregate initialiser
    for i, ch in enumerate(text):
        if brace:
            if ch == "{":
                brace += 1
            elif ch == "}":
                brace -= 1
            continue
        if ch == "(":
            depth += 1
        elif ch == ")":
            depth -= 1
        elif ch == "{" and depth == 0:
            if text[:i].rstrip().endswith("="):
                brace = 1
                continue
            # shouldn't start a stmt with {
            break
        elif ch == ";" and depth == 0:
            return text[: i + 1], text[i + 1:]
    raise ParseFail(f"no semicolon in {text[:80]!r}")


def _upto_colon(text: str) -> tuple[str, str]:
    depth = 0
    for i, ch in enumerate(text):
        if ch == "(":
            depth += 1
        elif ch == ")":
            depth -= 1
        elif ch == ":" and depth == 0:
            return text[:i], text[i + 1:]
    raise ParseFail("expected :")


def _consume_stmt_src(text: str) -> tuple[str, str]:
    """One statement's source (if/for/while/switch/block/semi) and the rest."""
    raw = text
    text = text.lstrip()
    skip = len(raw) - len(text)

    def taken(rest: str) -> tuple[str, str]:
        idx = len(raw) - len(rest) if rest is not None else len(raw)
        return raw[skip:idx], rest

    if not text:
        return "", ""
    if text.startswith("{"):
        _, rest = _brace(text)
        return taken(rest)
    if _starts_kw(text, "do"):
        rest = text[2:].lstrip()
        _, rest = _block_or_stmt(rest)
        rest = rest.lstrip()
        if not _starts_kw(rest, "while"):
            raise ParseFail("do without while")
        rest = rest[5:].lstrip()
        _, rest = _paren(rest)
        rest = rest.lstrip()
        if rest.startswith(";"):
            rest = rest[1:]
        return taken(rest)
    for kw in ("if", "switch", "while", "for"):
        if _starts_kw(text, kw):
            rest = text[len(kw):].lstrip()
            if kw == "if" and rest.startswith("constexpr"):
                raise ParseFail("if constexpr unencoded")
            _, rest = _paren(rest)
            _, rest = _block_or_stmt(rest)
            if kw == "if":
                r2 = rest.lstrip()
                if _starts_kw(r2, "else"):
                    _, rest = _block_or_stmt(r2[4:])
            return taken(rest)
    return _stmt(text)


def _arm_code(arms: list[_SwitchArm], i: int) -> str:
    parts: list[str] = []
    for j in range(i, len(arms)):
        if arms[j].code.strip():
            parts.append(arms[j].code)
        if arms[j].stops:
            break
    return "\n".join(parts)


def _as_bool(v: Any) -> Any:
    if isinstance(v, TV):
        return v.v != z3.BitVecVal(0, v.w)
    if isinstance(v, z3.BoolRef):
        return v
    return v != 0


def _signed_ovf(wide: Any, w: int) -> Any:
    """Signed result `wide` (computed exactly in more bits) does not fit in w bits."""
    narrow = z3.Extract(w - 1, 0, wide)
    return z3.SignExt(wide.size() - w, narrow) != wide


def apply_binop(e: _Enc, a: TV, op: str, b: TV) -> TV:
    if op in ("<<", ">>"):
        # C11 6.5.7: promotions on each operand separately; the result has
        # the type of the promoted left operand.
        a = e.promote(a)
        b = e.promote(b)
        w = a.w
        if b.u:
            bad_count = z3.UGE(b.v, z3.BitVecVal(w, b.w))
        else:
            bad_count = z3.Or(b.v < _bv_zero(b.w), b.v >= z3.BitVecVal(w, b.w))
        e.add_prop("shift", "INT-SHIFT-UB", bad_count, e.pc)
        cnt = e.conv(b, (w, True)).v
        if op == "<<":
            if not a.u and e.shift_rules < 20:
                # Negative left operand, or a * 2^b not representable (C11
                # 6.5.7p4): in the type (C, C++98/03), or in its unsigned
                # counterpart (C++11..17, CWG 1457: 1 << 31 is defined).
                # C++20 (P1236) defines every signed left shift whose count
                # is in range: only `shift` above applies.
                e.add_prop("shift-neg", "INT-SHIFT-UB", a.v < _bv_zero(w), e.pc)
                keep = w if e.shift_rules >= 11 else w - 1
                top = z3.LShR(a.v, z3.BitVecVal(keep, w) - cnt)
                e.add_prop("shift31", "INT-SHIFT-UB",
                           z3.And(z3.Not(bad_count), a.v >= _bv_zero(w), top != _bv_zero(w)), e.pc)
            return TV(a.v << cnt, w, a.u)
        return TV(z3.LShR(a.v, cnt) if a.u else a.v >> cnt, w, a.u)
    w, u = e.common(a, b)
    a = e.conv(a, (w, u))
    b = e.conv(b, (w, u))
    if op in ("+", "-", "*"):
        if op == "+":
            r = a.v + b.v
        elif op == "-":
            r = a.v - b.v
        else:
            r = a.v * b.v
        if not u:
            # Both directions (S1). + and - are computed exactly one bit
            # wider; * uses Z3's overflow/underflow predicates, which are
            # exact and far cheaper than a 2w-bit product.
            if op == "*":
                viol = z3.Or(z3.Not(z3.BVMulNoOverflow(a.v, b.v, True)),
                             z3.Not(z3.BVMulNoUnderflow(a.v, b.v)))
            else:
                wa, wb = z3.SignExt(1, a.v), z3.SignExt(1, b.v)
                viol = _signed_ovf(wa + wb if op == "+" else wa - wb, w)
            e.add_prop("ovf" + op, "INT-SIGNED-OVF", viol, e.pc)
        return TV(r, w, u)
    if op in ("/", "%"):
        div = op == "/"
        z = _bv_zero(w)
        e.add_prop("div0" if div else "mod0", "INT-DIV-ZERO", b.v == z, e.pc)
        if u:
            r = z3.UDiv(a.v, b.v) if div else z3.URem(a.v, b.v)
            return TV(z3.If(b.v == z, z, r), w, True)
        # INT_MIN / -1 and INT_MIN % -1 are both undefined (6.5.5p6).
        e.add_prop("divovf" if div else "modovf", "INT-SIGNED-OVF",
                   z3.And(a.v == _bv_min(w), b.v == z3.BitVecVal(-1, w)), e.pc)
        r = a.v / b.v if div else z3.SRem(a.v, b.v)
        return TV(z3.If(b.v == z, z, r), w, False)
    if op == "&":
        return TV(a.v & b.v, w, u)
    if op == "|":
        return TV(a.v | b.v, w, u)
    if op == "^":
        return TV(a.v ^ b.v, w, u)
    if op == "==":
        return _bool_tv(a.v == b.v)
    if op == "!=":
        return _bool_tv(a.v != b.v)
    if op == "<":
        return _bool_tv(z3.ULT(a.v, b.v) if u else a.v < b.v)
    if op == ">":
        return _bool_tv(z3.UGT(a.v, b.v) if u else a.v > b.v)
    if op == "<=":
        return _bool_tv(z3.ULE(a.v, b.v) if u else a.v <= b.v)
    if op == ">=":
        return _bool_tv(z3.UGE(a.v, b.v) if u else a.v >= b.v)
    raise ParseFail(f"op {op}")


def _guarded(e: _Enc, cond: Any, fn: Any) -> TV:
    """Evaluate fn() only on the paths where cond holds (&&, ||, ?:), then
    merge its side effects back: state = cond ? after : before."""
    base = e.snap()
    e.path_true = z3.And(base.path, cond)
    r = fn()
    taken = e.snap()
    skipped = _State(z3.And(base.path, z3.Not(cond)), dict(base.vars), dict(base.arrays),
                     dict(base.uninit))
    e.load(_merge_states(e, [taken, skipped], base))
    return r


def _cchar_lit_value(tok: str) -> int:
    """A character constant: type int, value of the (signed, x86-64) char."""
    inner = tok[1:-1]
    if not inner:
        raise ParseFail("empty character literal")
    bad = ParseFail(f"UNENCODED: character literal {tok}")
    if inner[0] == "\\" and len(inner) >= 2:
        esc = inner[1]
        if esc == "x":
            if not re.fullmatch(r"[0-9a-fA-F]{1,8}", inner[2:]):
                raise bad
            v = int(inner[2:], 16)
        elif "0" <= esc <= "7":
            if not re.fullmatch(r"[0-7]{1,8}", inner[1:]):
                raise bad
            v = int(inner[1:], 8)
        else:
            if len(inner) != 2:
                raise bad
            table = {"n": 10, "t": 9, "r": 13, "a": 7, "b": 8, "f": 12, "v": 11,
                     "\\": 92, "'": 39, '"': 34, "?": 63}
            if esc not in table:
                raise bad
            v = table[esc]
    else:
        if len(inner) != 1 or ord(inner) > 127:
            raise bad
        v = ord(inner)
    if v > 255:
        raise bad
    return v - 256 if v > 127 else v


def _int_literal(tok: str) -> TV:
    """Integer constant with its C type (C11 6.4.4.1p5, LP64)."""
    m = re.fullmatch(r"(0[xX][0-9a-fA-F]+|0[bB][01]+|[0-9]+)([uUlL]*)", tok)
    if not m:
        raise ParseFail(f"UNENCODED: integer literal {tok}")
    digits, suf = m.group(1), m.group(2)
    has_u = any(c in "uU" for c in suf)
    nl = sum(1 for c in suf if c in "lL")
    if len(suf) - (1 if has_u else 0) != nl or nl > 2 or sum(1 for c in suf if c in "uU") > 1:
        raise ParseFail(f"UNENCODED: integer literal {tok}")
    decimal = True
    if digits[:2] in ("0x", "0X"):
        decimal = False
        val = int(digits[2:], 16)
    elif digits[:2] in ("0b", "0B"):
        decimal = False
        val = int(digits[2:], 2)
    elif len(digits) > 1 and digits[0] == "0":
        decimal = False
        if not re.fullmatch(r"[0-7]+", digits):
            raise ParseFail(f"UNENCODED: integer literal {tok}")
        val = int(digits, 8)
    else:
        val = int(digits, 10)
    if val > (1 << 64) - 1:
        raise ParseFail(f"UNENCODED: integer literal {tok}")
    if has_u:
        cands = ([(32, True)] if nl == 0 else []) + [(64, True)]
    elif decimal:
        cands = ([(32, False)] if nl == 0 else []) + [(64, False)]
    else:
        cands = ([(32, False), (32, True)] if nl == 0 else []) + [(64, False), (64, True)]
    for w, u in cands:
        maxv = (1 << w) - 1 if u else (1 << (w - 1)) - 1
        if val <= maxv:
            return TV(z3.BitVecVal(val, w), w, u)
    raise ParseFail(f"UNENCODED: integer literal {tok} has no type")


_CAST_TYPE_WORDS = {
    "char", "short", "int", "long", "unsigned", "signed",
    "const", "volatile", "void", "_Bool", "bool",
    "int8_t", "uint8_t", "int16_t", "uint16_t",
    "uint32_t", "int32_t", "uint64_t", "int64_t", "size_t", "ssize_t",
    "ptrdiff_t", "intptr_t", "uintptr_t", "intmax_t", "uintmax_t",
}

# Object-like macros the encoder knows without a preprocessor: <limits.h>,
# <stdint.h>, <stdlib.h>, <stdbool.h> (LP64 glibc values). A #define in the
# file itself takes precedence (Parser.macros).
_BUILTIN_MACROS: dict[str, str] = {
    "CHAR_BIT": "8",
    "SCHAR_MIN": "(-128)", "SCHAR_MAX": "127", "UCHAR_MAX": "255",
    "CHAR_MIN": "(-128)", "CHAR_MAX": "127",
    "SHRT_MIN": "(-32768)", "SHRT_MAX": "32767", "USHRT_MAX": "65535",
    "INT_MIN": "(-2147483647 - 1)", "INT_MAX": "2147483647",
    "UINT_MAX": "4294967295U",
    "LONG_MIN": "(-9223372036854775807L - 1L)", "LONG_MAX": "9223372036854775807L",
    "ULONG_MAX": "18446744073709551615UL",
    "LLONG_MIN": "(-9223372036854775807LL - 1LL)", "LLONG_MAX": "9223372036854775807LL",
    "ULLONG_MAX": "18446744073709551615ULL",
    "INT8_MIN": "(-128)", "INT8_MAX": "127", "UINT8_MAX": "255",
    "INT16_MIN": "(-32768)", "INT16_MAX": "32767", "UINT16_MAX": "65535",
    "INT32_MIN": "(-2147483647 - 1)", "INT32_MAX": "2147483647",
    "UINT32_MAX": "4294967295U",
    "INT64_MIN": "(-9223372036854775807L - 1L)", "INT64_MAX": "9223372036854775807L",
    "UINT64_MAX": "18446744073709551615UL",
    "SIZE_MAX": "18446744073709551615UL",
    "RAND_MAX": "2147483647",
    "EXIT_SUCCESS": "0", "EXIT_FAILURE": "1",
    "true": "1", "false": "0", "NULL": "0",
}

_CTOK = re.compile(
    r"(0[xX][0-9a-fA-F]+[uUlL]*)|(0[bB][01]+[uUlL]*)|([0-9]+[uUlL]*)|"
    r"('(?:\\.|[^\\'])+')|(\"(?:\\.|[^\\\"])*\")|"
    r"([A-Za-z_]\w*)|(::|->|&&|\|\||==|!=|<=|>=|<<|>>|\+\+|--)|"
    r"([+\-*/%<>=!&|^~()[\],?:.;{}])|(\S)",
    re.ASCII,
)


def _ctok(src: str) -> list[str]:
    return [m.group(0) for m in _CTOK.finditer(src)]


def _expand_macros(toks: list[str], e: _Enc, file_macros: dict[str, str],
                   depth: int = 0) -> list[str]:
    """Expand object-like macros (file #defines first, then the builtin
    table) that do not name a visible variable."""
    if depth > 8:
        raise ParseFail("UNENCODED: macro expansion too deep")
    out: list[str] = []
    for t in toks:
        rep = None
        if _is_ident(t) and not e.declared(t):
            rep = file_macros.get(t)
            if rep is None:
                rep = _BUILTIN_MACROS.get(t)
        if rep is None:
            out.append(t)
            continue
        out.append("(")
        out.extend(_expand_macros(_ctok(rep), e, file_macros, depth + 1))
        out.append(")")
    return out


_NONDET: dict[str, tuple[int, bool]] = {
    "__VERIFIER_nondet_int": (32, False), "__VERIFIER_nondet_uint": (32, True),
    "__VERIFIER_nondet_unsigned": (32, True),
    "__VERIFIER_nondet_long": (64, False), "__VERIFIER_nondet_ulong": (64, True),
    "__VERIFIER_nondet_longlong": (64, False), "__VERIFIER_nondet_ulonglong": (64, True),
    "__VERIFIER_nondet_short": (16, False), "__VERIFIER_nondet_ushort": (16, True),
    "__VERIFIER_nondet_char": (8, False), "__VERIFIER_nondet_uchar": (8, True),
    "__VERIFIER_nondet_bool": (1, True), "__VERIFIER_nondet__Bool": (1, True),
    "__VERIFIER_nondet_size_t": (64, True),
}

_NORETURN = {
    "abort", "exit", "_Exit", "quick_exit", "reach_error", "__assert_fail",
    "__VERIFIER_error",
}

# Juliet support-library output helpers (io.c): print one scalar or a string
# literal; no undefined behaviour for any argument value.
_PRINT_HELPERS = {
    "printLine", "printWLine", "printIntLine", "printShortLine", "printLongLine",
    "printLongLongLine", "printSizeTLine", "printHexCharLine", "printUnsignedLine",
    "printHexUnsignedCharLine",
}


def _model_call(e: _Enc, t: str, args: list[TV]) -> TV:
    """Calls whose semantics are modelled. Everything else: arguments are
    checked, the result is unconstrained, and the verdict can never be a
    proof."""
    def nargs(n: int) -> None:
        if len(args) != n:
            raise ParseFail(f"UNENCODED: call to {t} with {len(args)} arguments")

    if t in ("abs", "labs", "llabs"):
        nargs(1)
        ct = (32, False) if t == "abs" else (64, False)
        a = e.conv(e.promote(args[0]), ct)
        # abs(INT_MIN) is undefined (C11 7.22.6.1p2).
        e.add_prop("abs", "INT-SIGNED-OVF", a.v == _bv_min(ct[0]), e.pc)
        return TV(z3.If(a.v < _bv_zero(ct[0]), _bv_zero(ct[0]) - a.v, a.v), ct[0], False)
    if t == "__builtin_expect":
        nargs(2)
        return e.conv(args[0], (64, False))
    if t in _NONDET:
        nargs(0)
        w, u = _NONDET[t]
        return TV(e.bv(f"nondet_{e.fresh + 1}", w), w, u)
    if t == "rand":
        nargs(0)
        v = e.bv(f"rand_{e.fresh + 1}", 32)
        e.assume(v >= _bv_zero(32))
        return TV(v, 32, False)
    if t in ("__VERIFIER_assume", "assume_abort_if_not"):
        # SV-COMP harness convention: execution continues only if cond != 0.
        nargs(1)
        e.assume(_as_bool(args[0]))
        return e.int_val(0)
    if t in _NORETURN:
        # Does not return: nothing after it runs on this path.
        e.path_true = z3.BoolVal(False)
        return e.int_val(0)
    if t in _PRINT_HELPERS:
        nargs(1)
        return e.int_val(0)
    if t not in e.unmodelled:
        e.unmodelled.append(t)
    r = e.bv(f"call_{t}_{e.fresh + 1}", 32)
    e.call_vars.append(r)
    return TV(r, 32, False)


def _sizeof_type(inner: list[str], e: _Enc) -> int:
    if "*" in inner:
        return 8
    joined = " ".join(inner)
    if len(inner) == 1 and _is_ident(inner[0]) and e.declared(inner[0]):
        name = inner[0]
        if name in e.alias:
            return 8
        if name in e.arrays:
            a = e.arrays[name]
            return a.n * max(1, a.w // 8)
        return max(1, e.type_of(name)[0] // 8)
    ct = _ctype_parse(joined)
    if ct is not None:
        return max(1, ct[0] // 8)
    if len(inner) == 4 and e.is_array(inner[0]) and inner[1] == "[" and inner[3] == "]":
        return max(1, e.arrays[e.canonical(inner[0])].w // 8)
    raise ParseFail(f"UNENCODED: sizeof {joined}")


def _parse_expr(e: _Enc, src: str, parser: Parser) -> TV:
    src = src.strip()
    return _pratt(e, src, parser)


_PREC = {
    "||": 10, "&&": 20,
    "|": 30, "^": 40, "&": 50,
    "==": 60, "!=": 60,
    "<": 70, ">": 70, "<=": 70, ">=": 70,
    "<<": 80, ">>": 80,
    "+": 90, "-": 90,
    "*": 100, "/": 100, "%": 100,
}


_LV_START = frozenset({"(", "++", "--", "*"})
_LV_ANY = frozenset({"=", "+=", "-=", "*=", "/=", "%=", "&=", "|=", "^=", "<<=", ">>=", "?", ",", ".", "->"})


def _lvalue_names(e: _Enc, tok: list[str], a: int, b: int) -> list[str]:
    """Names a C++ call argument tok[a:b] may bind by reference (F10)."""

    def var(n: str) -> bool:
        return n in e.bits or e.is_array(n)

    if b == a + 1:
        return [tok[a]] if var(tok[a]) else []
    if b > a + 1 and tok[a + 1] == "[" and tok[b - 1] == "]" and e.is_array(tok[a]):
        depth, whole = 0, True  # a[i] as a whole (not a[i] + 1)
        for k in range(a + 1, b):
            if tok[k] == "[":
                depth += 1
            elif tok[k] == "]":
                depth -= 1
                if depth == 0 and k + 1 != b:
                    whole = False
        if whole:
            return [tok[a]]
    if not ((b > a and tok[a] in _LV_START) or any(t in _LV_ANY for t in tok[a:b])):
        return []
    return list(dict.fromkeys(t for t in tok[a:b] if var(t)))


def _pratt(e: _Enc, src: str, parser: Parser) -> TV:
    tokens = _expand_macros(_ctok(src), e, parser.macros)
    pos = 0

    def peek(k: int = 0) -> str:
        return tokens[pos + k] if pos + k < len(tokens) else ""

    def eat(t: str | None = None) -> str:
        nonlocal pos
        if pos >= len(tokens):
            raise ParseFail("unexpected end of expression")
        got = tokens[pos]
        if t is not None and got != t:
            raise ParseFail(f"expected {t} got {got}")
        pos += 1
        return got

    def incdec(name: str, op: str, prefix: bool) -> TV:
        if name not in e.bits:
            if e.declared(name):
                raise ParseFail(f"UNENCODED: {op} on array/pointer {name}")
            raise ParseFail(f"UNENCODED: identifier {name}")
        e.check_read(name)
        cur = e.get(name)
        new = apply_binop(e, cur, "+" if op == "++" else "-", e.int_val(1))
        e.set(name, new)
        e.mark_init(name)
        return e.get(name) if prefix else cur

    def nud() -> TV:
        t = eat()
        if len(t) >= 3 and t.startswith("'") and t.endswith("'"):
            return e.int_val(_cchar_lit_value(t))
        if len(t) >= 2 and t.startswith('"') and t.endswith('"'):
            # string literal is a non-null address, not a proof of the bytes
            return e.int_val(1)
        if t in ("alignof", "_Alignof"):
            raise ParseFail("alignof unencoded")
        if t == "sizeof":
            inner: list[str] = []
            if peek() == "(":
                eat("(")
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
            else:
                inner.append(eat())
            return TV(z3.BitVecVal(_sizeof_type(inner, e), 64), 64, True)
        if t == "(":
            if peek() == "{":
                raise ParseFail("statement-expr unencoded")
            if peek() in _CAST_TYPE_WORDS:
                words: list[str] = []
                while peek() and peek() != ")":
                    if peek() not in _CAST_TYPE_WORDS and peek() != "*":
                        break
                    words.append(eat())
                eat(")")
                v = parse(110)
                if all(wd in ("void", "const", "volatile") for wd in words):
                    return e.int_val(0)  # (void)x: evaluated, value unused
                joined = " ".join(words)
                ct = _ctype_parse(joined)
                if ct is None:
                    raise ParseFail(f"UNENCODED: cast to {joined}")
                return e.conv(v, ct)
            v = parse(0)
            eat(")")
            return v
        if t in ("++", "--"):
            name = eat()
            if not _is_ident(name):
                raise ParseFail(f"prefix {t} needs an identifier")
            return incdec(name, t, True)
        if t == "*":
            name = peek()
            if not _is_ident(name) or not e.is_array(name):
                raise ParseFail(f"deref of {name!r}")
            eat()
            arr = e.arrays[e.canonical(name)]
            idx = e.int_val(0)
            e.add_prop("oob-read", "MEM-OOB-READ", _oob(e, idx, arr.n), e.pc)
            e.check_elem(e.canonical(name), idx, arr.n)
            return TV(z3.Select(arr.a, _index32(e, idx)), arr.w, arr.u)
        if t == "-":
            v = e.promote(parse(110))
            if not v.u:
                e.add_prop("neg", "INT-SIGNED-OVF", v.v == _bv_min(v.w), e.pc)
            return TV(_bv_zero(v.w) - v.v, v.w, v.u)
        if t == "+":
            return e.promote(parse(110))
        if t == "!":
            return _bool_tv(z3.Not(_as_bool(parse(110))))
        if t == "~":
            v = e.promote(parse(110))
            return TV(~v.v, v.w, v.u)
        if t == "&":
            raise ParseFail("UNENCODED: address-of")
        if "0" <= t[0] <= "9":
            return _int_literal(t)
        if _is_ident(t):
            if peek() == "::":
                raise ParseFail(f"UNENCODED: qualified name {t}::{peek(1)}")
            if t == "static_cast" and peek() == "<":
                # static_cast<T>(e) between integer types is the C cast (T)e.
                eat("<")
                sc_words: list[str] = []
                while peek() and peek() != ">":
                    sc_words.append(eat())
                eat(">")
                joined = " ".join(sc_words)
                ct = _ctype_parse(joined)
                if ct is None:
                    raise ParseFail(f"UNENCODED: static_cast to {joined}")
                eat("(")
                v = parse(0)
                eat(")")
                return e.conv(v, ct)
            if t in ("_Generic", "offsetof"):
                raise ParseFail(f"{t} unencoded")
            if t in ("__int128", "__int128_t", "_BitInt"):
                raise ParseFail("128-bit unencoded")
            if t in ("_Decimal32", "_Decimal64", "_Decimal128"):
                raise ParseFail("decimal-float unencoded")
            if t in ("_Float16", "_Float32", "_Float64", "__fp16"):
                raise ParseFail("extra-IEEE unencoded")
            if t in ("typeof_unqual", "__typeof_unqual__"):
                raise ParseFail("typeof_unqual unencoded")
            if t in ("start_lifetime_as", "start_lifetime_as_array"):
                raise ParseFail("start_lifetime_as unencoded")
            if t in ("shared_from_this", "enable_shared_from_this") or (
                t == "std" and peek() in (
                    "shared_from_this", "enable_shared_from_this",
                )
            ):
                raise ParseFail("shared_from_this unencoded")
            if t == "__builtin_choose_expr":
                raise ParseFail("choose_expr unencoded")
            if t == "requires" and peek() == "(":
                raise ParseFail("concepts unencoded")
            if t == "restrict":
                raise ParseFail("restrict unencoded")
            if t == "nullptr":
                raise ParseFail("nullptr unencoded")
            if t == "launder" or (t == "std" and peek() == "launder"):
                raise ParseFail("launder unencoded")
            if t in ("dlopen", "dlsym", "dlclose"):
                raise ParseFail("dlopen unencoded")
            if t in (
                "__builtin_clz", "__builtin_ctz",
                "__builtin_clzll", "__builtin_ctzll",
            ):
                raise ParseFail("clz unencoded")
            if t in (
                "__atomic_load", "__atomic_store",
                "__sync_fetch_and_add", "__sync_bool_compare_and_swap",
            ):
                raise ParseFail("atomic builtin unencoded")
            if t == "bit_cast":
                raise ParseFail("bit_cast unencoded")
            if t in (
                "optional", "variant", "span", "expected",
                "function", "mdspan", "future", "promise",
                "atomic_ref", "generator",
            ) and peek() == "<":
                raise ParseFail(f"std::{t} unencoded")
            if t == "condition_variable_any":
                raise ParseFail("condition_variable_any unencoded")
            if t == "shared_timed_mutex":
                raise ParseFail("shared_timed_mutex unencoded")
            if t == "recursive_timed_mutex":
                raise ParseFail("recursive_timed_mutex unencoded")
            if t == "error_category":
                raise ParseFail("error_category unencoded")
            if t == "nested_exception":
                raise ParseFail("nested_exception unencoded")
            if t == "wstring_convert":
                raise ParseFail("wstring_convert unencoded")
            if t == "system_error":
                raise ParseFail("system_error unencoded")
            if t in ("current_zone", "tzdb"):
                raise ParseFail("tzdb unencoded")
            if t == "is_scoped_enum":
                raise ParseFail("is_scoped_enum unencoded")
            if t in ("enumerate", "enumerate_view"):
                raise ParseFail("enumerate unencoded")
            if t in ("cartesian_product", "cartesian_product_view"):
                raise ParseFail("cartesian_product unencoded")
            if t in ("chunk_by", "chunk_view"):
                raise ParseFail("chunk unencoded")
            if t == "slide_view":
                raise ParseFail("slide unencoded")
            if t in ("adjacent_transform", "adjacent_view"):
                raise ParseFail("adjacent unencoded")
            if t in ("join_with", "join_with_view"):
                raise ParseFail("join_with unencoded")
            if t == "join_view":
                raise ParseFail("views::join unencoded")
            if t in ("zip_transform", "zip_transform_view"):
                raise ParseFail("zip_transform unencoded")
            if t == "zip_view":
                raise ParseFail("views::zip unencoded")
            if t in ("as_rvalue", "as_rvalue_view"):
                raise ParseFail("as_rvalue unencoded")
            if t in ("from_range", "from_range_t"):
                raise ParseFail("from_range unencoded")
            if t in ("stride_view",):
                raise ParseFail("stride unencoded")
            if t == "repeat_view":
                raise ParseFail("repeat unencoded")
            if t == "take_view":
                raise ParseFail("take unencoded")
            if t == "drop_view":
                raise ParseFail("drop unencoded")
            if t == "filter_view":
                raise ParseFail("filter unencoded")
            if t == "transform_view":
                raise ParseFail("transform_view unencoded")
            if t == "elements_view":
                raise ParseFail("elements unencoded")
            if t == "iota_view":
                raise ParseFail("iota unencoded")
            if t == "reference_wrapper":
                raise ParseFail("reference_wrapper unencoded")
            if t == "uncaught_exceptions" and peek() == "(":
                raise ParseFail("uncaught_exceptions unencoded")
            if t in ("bit_ceil", "bit_floor", "has_single_bit") and peek() == "(":
                raise ParseFail("bit_ceil unencoded")
            if t == "bit_width" and peek() == "(":
                raise ParseFail("bit_width unencoded")
            if t == "gcd" and peek() == "(":
                raise ParseFail("gcd unencoded")
            if t == "lcm" and peek() == "(":
                raise ParseFail("lcm unencoded")
            if t == "clamp" and peek() == "(":
                raise ParseFail("clamp unencoded")
            if t == "exchange" and peek() == "(":
                raise ParseFail("exchange unencoded")
            if t == "to_address" and peek() == "(":
                raise ParseFail("to_address unencoded")
            if t == "addressof" and peek() == "(":
                raise ParseFail("addressof unencoded")
            if t == "assume_aligned" and peek() == "(":
                raise ParseFail("assume_aligned unencoded")
            if t == "as_const" and peek() == "(":
                raise ParseFail("as_const unencoded")
            if t in (
                "transform_inclusive_scan", "transform_exclusive_scan",
            ) and peek() == "(":
                raise ParseFail("transform_inclusive_scan unencoded")
            if t == "exclusive_scan" and peek() == "(":
                raise ParseFail("exclusive_scan unencoded")
            if t == "inclusive_scan" and peek() == "(":
                raise ParseFail("inclusive_scan unencoded")
            if t == "transform_reduce" and peek() == "(":
                raise ParseFail("transform_reduce unencoded")
            if t in (
                "uninitialized_fill", "uninitialized_fill_n",
                "uninitialized_default_construct",
                "uninitialized_default_construct_n",
            ) and peek() == "(":
                raise ParseFail("uninitialized_fill unencoded")
            if t in (
                "uninitialized_value_construct",
                "uninitialized_value_construct_n",
            ) and peek() == "(":
                raise ParseFail("uninitialized_value_construct unencoded")
            if t in (
                "uninitialized_copy", "uninitialized_move",
                "uninitialized_copy_n", "uninitialized_move_n",
            ) and peek() == "(":
                raise ParseFail("uninitialized_copy unencoded")
            if t in ("construct_at", "destroy_at") and peek() == "(":
                raise ParseFail("construct_at unencoded")
            if t == "destroy_n" and peek() == "(":
                raise ParseFail("destroy_n unencoded")
            if t in (
                "add_sat", "sub_sat", "mul_sat", "div_sat", "saturate_cast",
            ) and peek() == "(":
                raise ParseFail("add_sat unencoded")
            if t == "type_identity":
                raise ParseFail("type_identity unencoded")
            if t == "nontype":
                raise ParseFail("nontype unencoded")
            if t == "is_layout_compatible":
                raise ParseFail("is_layout_compatible unencoded")
            if t in (
                "is_pointer_interconvertible_with_class",
                "is_pointer_interconvertible_base_of",
            ):
                raise ParseFail("is_pointer_interconvertible unencoded")
            if t == "basic_const_iterator":
                raise ParseFail("basic_const_iterator unencoded")
            if t == "is_corresponding_member":
                raise ParseFail("is_corresponding_member unencoded")
            if t == "forward_like":
                raise ParseFail("forward_like unencoded")
            if t == "make_exception_ptr" and peek() == "(":
                raise ParseFail("make_exception_ptr unencoded")
            if t in ("set_terminate", "get_terminate") and peek() == "(":
                raise ParseFail("set_terminate unencoded")
            if t == "is_constant_evaluated" and peek() == "(":
                raise ParseFail("is_constant_evaluated unencoded")
            if t == "lerp" and peek() == "(":
                raise ParseFail("lerp unencoded")
            if t == "midpoint" and peek() == "(":
                raise ParseFail("midpoint unencoded")
            if t in (
                "cmp_less", "cmp_greater", "cmp_less_equal",
                "cmp_greater_equal", "cmp_equal_to", "cmp_not_equal_to",
                "in_range",
            ) and peek() == "(":
                raise ParseFail("cmp_less unencoded")
            if t in (
                "countl_zero", "countr_zero", "countl_one", "countr_one",
            ) and peek() == "(":
                raise ParseFail("countl_zero unencoded")
            if t == "unreachable" and peek() == "(":
                raise ParseFail("std::unreachable unencoded")
            if t in ("condition_variable", "shared_mutex"):
                raise ParseFail("condition_variable unencoded")
            if peek() == "[":
                eat("[")
                idx = parse(0)
                eat("]")
                if not e.is_array(t):
                    raise ParseFail(f"UNENCODED: index into unknown array {t}")
                arr = e.arrays[e.canonical(t)]
                e.add_prop("oob-read", "MEM-OOB-READ", _oob(e, idx, arr.n), e.pc)
                e.check_elem(e.canonical(t), idx, arr.n)
                return TV(z3.Select(arr.a, _index32(e, idx)), arr.w, arr.u)
            if peek() == "(" and not e.declared(t):
                eat("(")
                args: list[TV] = []
                spans: list[tuple[int, int]] = []  # token range of each argument
                n_values = len(e.value_arrays)
                if peek() == ")":
                    eat(")")
                else:
                    while True:
                        start = pos
                        args.append(parse(2))
                        spans.append((start, pos))
                        if peek() == ",":
                            eat(",")
                            continue
                        eat(")")
                        break
                n_calls = len(e.call_vars)
                res = _model_call(e, t, args)
                if len(e.call_vars) > n_calls:
                    # unmodelled callee: every array its arguments mention escapes
                    for an in dict.fromkeys(e.value_arrays[n_values:]):
                        e.escape_array(an)
                    # C++: a parameter may be a non-const reference, so an
                    # lvalue argument may be written by the callee
                    # (docs/CONFORMANCE.md F10). What it may name becomes
                    # unknown, quantified like a call result: a named scalar,
                    # or the array of an element; an argument of any other
                    # lvalue shape (parenthesised, ++x, *p, x = y, c ? x : y)
                    # lets every scalar and array it mentions escape.
                    if e.cxx:
                        for a, b in spans:
                            for n in _lvalue_names(e, tokens, a, b):
                                if e.is_array(n):
                                    e.escape_array(e.canonical(n))
                                else:
                                    e.escape_scalar(n)
                del e.value_arrays[n_values:]
                return res
            if peek() in ("++", "--"):
                op = eat()
                return incdec(t, op, False)
            if e.is_array(t):
                # pointer/array used as a value: non-null by construction
                e.value_arrays.append(e.canonical(t))
                return e.int_val(1)
            if t in e.bits:
                e.check_read(t)
                return e.get(t)
            if t in parser.enums:
                return e.int_val(parser.enums[t])
            raise ParseFail(f"UNENCODED: identifier {t}")
        raise ParseFail(f"bad token {t}")

    def parse(minp: int) -> TV:
        left = nud()
        while peek() in _PREC and _PREC[peek()] >= minp:
            op = eat()
            p = _PREC[op] + 1
            if op == "&&":
                lb = _as_bool(left)
                right = _guarded(e, lb, lambda: parse(p))
                left = _bool_tv(z3.And(lb, _as_bool(right)))
            elif op == "||":
                lb = _as_bool(left)
                right = _guarded(e, z3.Not(lb), lambda: parse(p))
                left = _bool_tv(z3.Or(lb, _as_bool(right)))
            else:
                right = parse(p)
                left = apply_binop(e, left, op, right)
        # C ternary binds below ||, right-associative; only the taken arm runs.
        if minp <= 5 and peek() == "?":
            eat("?")
            c = _as_bool(left)
            then_v = _guarded(e, c, lambda: parse(0))
            eat(":")
            else_v = _guarded(e, z3.Not(c), lambda: parse(5))
            ct = e.common(then_v, else_v)
            left = TV(z3.If(c, e.conv(then_v, ct).v, e.conv(else_v, ct).v), ct[0], ct[1])
        # Comma: evaluate left for side effects; value is the right.
        if minp <= 1 and peek() == ",":
            eat(",")
            left = parse(0)
        return left

    v = parse(0)
    if pos != len(tokens):
        raise ParseFail(f"trailing {tokens[pos:]}")
    return v


def _tok(src: str) -> list[str]:
    spec = [
        r"0x[0-9a-fA-F]+",
        r"\d+",
        r"'(?:\\.|[^\\'])'",
        r'"(?:\\.|[^\\"])*"',
        r"[A-Za-z_]\w*",
        r"&&|\|\||==|!=|<=|>=|<<|>>|\+\+|--",
        r"[+\-*/%<>=!&|^~()[\],?:]",
    ]
    rx = re.compile("|".join(f"({s})" for s in spec))
    return [m.group(0) for m in rx.finditer(src)]


def _cex(model: Any, params: list[tuple[str, str]]) -> str:
    bits = []
    for typ, name in params:
        if not name:
            continue
        try:
            d = model.eval(z3.BitVec(name, _ctype_of(typ)[0]), model_completion=True)
            bits.append(f"{name}={d}")
        except Exception:
            pass
    return ", ".join(bits)


_CAST_WORDS = {
    "char", "short", "int", "long", "unsigned", "signed",
    "const", "volatile", "void", "_Bool", "bool",
    "uint32_t", "int32_t", "uint64_t", "int64_t", "size_t",
}


def _char_lit_value(tok: str) -> int:
    inner = tok[1:-1]
    if not inner:
        raise ParseFail("empty character literal")
    if inner[0] == "\\" and len(inner) >= 2:
        esc = inner[1]
        return {
            "n": 10, "t": 9, "r": 13, "0": 0,
            "\\": 92, "'": 39, '"': 34,
        }.get(esc, ord(esc))
    return ord(inner[0])


def _string_lit_value(tok: str) -> str:
    inner = tok[1:-1]
    out: list[str] = []
    i = 0
    while i < len(inner):
        if inner[i] == "\\" and i + 1 < len(inner):
            esc = inner[i + 1]
            out.append({
                "n": "\n", "t": "\t", "r": "\r", "0": "\0",
                "\\": "\\", '"': '"', "'": "'",
            }.get(esc, esc))
            i += 2
            continue
        out.append(inner[i])
        i += 1
    return "".join(out)


_TYPE_SIZE = {
    "char": 1, "signed char": 1, "unsigned char": 1,
    "short": 2, "short int": 2, "signed short": 2, "unsigned short": 2,
    "int": 4, "signed": 4, "signed int": 4, "unsigned": 4, "unsigned int": 4,
    "long": 4, "long int": 4, "unsigned long": 4,
    "long long": 8, "long long int": 8, "unsigned long long": 8,
    "uint32_t": 4, "int32_t": 4, "size_t": 4,
    "_Bool": 1, "bool": 1,
}


def k_induction(fn: FunctionInfo, unwind: int) -> Finding:
    """Base case = BMC; inductive step (k=1) = every loop havocked.

    Each variable a loop may assign is arbitrary at the loop head; the
    condition, one body execution and everything after the loop are checked
    from there (Parser._loop_havoc). Every reachable state is covered, so an
    unsat step proves the function for all unrollings. SAT on the havocked
    step is not a counterexample of the original function: the record stays
    BOUNDED. Closing the step is PROVED-UNBOUNDED and is never merged down
    into PROVED/BOUNDED.
    """
    rec = bmc_function(fn, unwind, try_unbounded=True)
    extra = dict(rec.extra or {})
    if rec.status != laws.BOUNDED:
        extra["k_induction"] = "not-needed"
        rec.extra = extra
        return rec
    base: dict[str, Any] = dict(stage="bmc", file=fn.file, function=fn.name, line=fn.line,
                                cls="", strength=laws.STRENGTH_PROVES, extra={})
    step = _bmc_once(fn, 1, True, _enums_from_fn(fn), base,
                     macros=_macros_from_fn(fn), havoc=True)
    extra["k_steps"] = [step.status]
    extra["k_induction_tried"] = [1]
    extra["k_induction_k"] = 1
    if step.status in {laws.PROVED, laws.PROVED_UNBOUNDED}:
        extra["k_induction"] = "closed"
        extra["unwind_closed"] = True
        return Finding(
            stage="bmc", status=laws.PROVED_UNBOUNDED,
            file=fn.file, function=fn.name, line=fn.line, cls="",
            message="k-induction step closed at k=1; not a bounded-only result",
            strength=laws.STRENGTH_PROVES, extra=extra,
        )
    if step.status == laws.FAILED:
        extra["k_induction"] = "step-open"
        extra["k_induction_cls"] = step.cls
        rec.extra = extra
        return rec
    extra["k_induction"] = "unencoded"
    rec.extra = extra
    return rec


def _unwind_schedule(unwind: int) -> list[int]:
    """ESBMC-style k = 1, 2, 4, …, K. First SAT at the smallest k wins."""
    ks: list[int] = []
    k = 1
    while k < max(1, unwind):
        ks.append(k)
        k *= 2
    if unwind not in ks:
        ks.append(max(1, unwind))
    return ks


def _param_premise(cex: str, params: list[tuple[str, str]]) -> str:
    """ParanoidBSD: does the counterexample name a parameter?"""
    if not cex or "=" not in cex:
        return "unattributed"
    named = {p.split("=", 1)[0].strip() for p in cex.split(",") if "=" in p}
    par = {n for _, n in params if n}
    if named & par:
        return "named"
    return "local"


def _cxx_source(file: str) -> bool:
    """C++ unless the file is C (.c, .i): a header may be either, so it counts
    as C++ (call arguments may bind references; _Enc.cxx)."""
    return Path(file or "").suffix.lower() not in (".c", ".i")


# The default of the C++ compilers PRISM runs (clang++ 16-18, g++ 11-14: gnu++17).
_DEFAULT_CXX_STD = 17

_CXX_STD_YEARS = {
    "98": 3, "03": 3, "0x": 11, "11": 11, "1y": 14, "14": 14, "1z": 17, "17": 17,
    "2a": 20, "20": 20, "2b": 23, "23": 23, "2c": 26, "26": 26,
}


def cxx_std_year(flag: str) -> int:
    """The year of a `-std=` flag's C++ standard (c++98/03 -> 3, c++0x/11 ->
    11, c++2a/20 -> 20, gnu++ alike); 0 for anything else."""
    if flag.startswith("-std="):
        flag = flag[5:]
    if flag.startswith("gnu++"):
        flag = flag[5:]
    elif flag.startswith("c++"):
        flag = flag[3:]
    else:
        return 0
    return _CXX_STD_YEARS.get(flag, 0)


def _compile_db_std(root: Path, src: Path) -> int:
    """The C++ standard year of src's -std= in compile_commands.json (root or
    root/build), as the C++ engine's lints read it; 0 when none."""
    import json
    import shlex
    base = root if root.is_dir() else root.parent
    try:
        want = src.resolve()
    except OSError:
        return 0
    for cand in (base / "compile_commands.json", base / "build" / "compile_commands.json"):
        if not cand.is_file():
            continue
        try:
            entries = json.loads(cand.read_text(encoding="utf-8", errors="replace"))
        except (OSError, ValueError):
            continue
        for ent in entries if isinstance(entries, list) else []:
            if not isinstance(ent, dict) or not isinstance(ent.get("file"), str):
                continue
            d = Path(ent.get("directory") or "")
            f = Path(ent["file"])
            if not f.is_absolute():
                f = d / f
            try:
                if f.resolve() != want:
                    continue
            except OSError:
                continue
            args = ent.get("arguments")
            if not isinstance(args, list):
                cmd = ent.get("command")
                try:
                    args = shlex.split(cmd) if isinstance(cmd, str) else []
                except ValueError:
                    args = []
            year = 0
            for a in args:
                if isinstance(a, str) and a.startswith("-std="):
                    year = cxx_std_year(a)  # the last one wins, as in the compiler
            return year
    return 0


def with_cxx_std(functions: list[FunctionInfo], root: Path) -> list[FunctionInfo]:
    """functions with cxx_std set from the -std= of their unit in
    compile_commands.json (root or root/build)."""
    root = Path(root)
    seen: dict[str, int] = {}
    out = []
    for fn in functions:
        if fn.file not in seen:
            seen[fn.file] = _compile_db_std(root, root / fn.file if root.is_dir() else root)
        out.append(replace(fn, cxx_std=seen[fn.file]))
    return out


def _shift_rules_for(fn: FunctionInfo) -> int:
    """Signed left-shift rules of fn's unit (Parser.shift_rules). Only a C++
    source file (not a header, which may be C) gets C++ rules; its standard
    is its -std= (FunctionInfo.cxx_std) or else the compilers' default."""
    ext = Path(fn.file or "").suffix
    cxx = ext == ".C" or ext.lower() in (".cc", ".cpp", ".cxx", ".c++", ".cp", ".ii")
    if not cxx:
        return 0
    std = fn.cxx_std if fn.cxx_std > 0 else _DEFAULT_CXX_STD
    return 20 if std >= 20 else 11 if std >= 11 else 0


def _bmc_once(
    fn: FunctionInfo,
    unwind: int,
    try_unbounded: bool,
    enums: dict[str, int],
    base: dict,
    macros: dict[str, str] | None = None,
    havoc: bool = False,
) -> Finding:
    p = Parser(fn.body, fn.params, unwind, enums=enums, macros=macros, havoc=havoc)
    p.cxx = _cxx_source(fn.file)
    p.shift_rules = _shift_rules_for(fn)
    enc = p.run()
    if enc is None:
        b = dict(base)
        b["strength"] = laws.STRENGTH_SOME
        err = p.err or "parse failed"
        msg = harness_for_parsefail(err, "bitvector BMC")
        if msg:
            return Finding(**b, status=laws.NEEDS_HARNESS, message=msg)
        return Finding(**b, status=laws.ERROR,
                       message=f"BMC frontend: {err}")

    for prop in enc.props:
        s = z3.Solver()
        s.set("timeout", 8000)
        s.add(prop.cond)
        r = s.check()
        if enc.call_vars and r != z3.unsat:
            # A violation that needs an unmodelled call to return some
            # particular value is not a refutation (the callee may never
            # return it): it must hold for every value the calls return.
            s.reset()
            s.set("timeout", 8000)
            s.add(z3.ForAll(enc.call_vars, prop.cond))
            r = s.check()
            if r != z3.sat:
                continue
        if r == z3.sat:
            cex = _cex(s.model(), fn.params) or f"{prop.name}=sat"
            return Finding(
                stage="bmc", status=laws.FAILED, file=fn.file, function=fn.name,
                line=fn.line, cls=prop.cls,
                message=f"{prop.name}: {prop.cls}",
                strength=laws.STRENGTH_PROVES,
                counterexample=cex,
                extra={
                    "oracle": False,
                    "unwind": unwind,
                    "param_premise": _param_premise(cex, fn.params),
                },
            )
        if r == z3.unknown:
            return Finding(**base, status=laws.UNKNOWN,
                           message=f"solver unknown on {prop.name}")

    # S6: a call that is neither inlined nor modelled has an unknown effect
    # and may itself be undefined; its arguments were checked above, but no
    # verdict of this function can be a proof.
    if enc.unmodelled:
        b = dict(base)
        b["strength"] = laws.STRENGTH_SOME
        return Finding(
            **b, status=laws.NEEDS_HARNESS,
            message=f"UNENCODED: call to {','.join(enc.unmodelled)} not modelled "
            "(arguments checked, result unconstrained): not a proof",
        )

    # Vacuous "properties hold" of system/exit/pthread_create/printf is not a
    # proof of those calls. Mixed bodies that did encode overflow/OOB
    # keep FAILED/PROVED of *those* properties.
    if not enc.props and _has_unencoded_libc_effect(fn):
        b = dict(base)
        b["strength"] = laws.STRENGTH_SOME
        return Finding(
            **b, status=laws.NEEDS_HARNESS,
            message="unmodeled libc side-effect: unconstrained call is not a proof",
        )

    if enc.unwind_ok and try_unbounded:
        st = laws.PROVED_UNBOUNDED
        return Finding(
            stage="bmc", status=st, file=fn.file, function=fn.name, line=fn.line,
            cls="", message="encoded UB properties hold"
                      + (" for all unrollings" if st == laws.PROVED_UNBOUNDED
                         else f" within unwind {unwind}"),
            strength=laws.STRENGTH_PROVES,
            extra={"unwind": unwind, "unwind_closed": enc.unwind_ok},
        )
    if enc.unwind_ok:
        return Finding(
            stage="bmc", status=laws.PROVED, file=fn.file, function=fn.name,
            line=fn.line, cls="", message=f"properties hold, loops closed at unwind {unwind}",
            strength=laws.STRENGTH_PROVES, extra={"unwind": unwind},
        )
    return Finding(
        stage="bmc", status=laws.BOUNDED, file=fn.file, function=fn.name,
        line=fn.line, cls="",
        message=f"no violation within unwind {unwind}; loops did not close",
        strength=laws.STRENGTH_PROVES, extra={"unwind": unwind, "unwind_closed": False},
    )


def _has_unencoded_cxx(fn: FunctionInfo) -> bool:
    """string_view / span / std:: are not in the bitvector encoder."""
    blob = f"{fn.return_type or ''} {fn.body or ''}"
    return bool(_re_search(r"\bstd::|\bstring_view\b|\bspan\b", blob))


def _has_unencoded_float(fn: FunctionInfo) -> bool:
    """IEEE float is not in the bitvector encoder. Missing model, not ERROR."""
    if _re_search(r"\bfloat\b|\bdouble\b", fn.return_type or "", re.I):
        return True
    for typ, _ in fn.params:
        if _re_search(r"\bfloat\b|\bdouble\b", typ or "", re.I):
            return True
    return bool(_re_search(
        r"\d+\.\d+[fFlL]?|\b(?:float|double)\b",
        fn.body or "",
    ))


def _call_names(body: str) -> list[str]:
    out: list[str] = []
    for m in re.finditer(r"\b([A-Za-z_]\w*)\s*\(", body or ""):
        n = m.group(1)
        if n not in _CALL_KW:
            out.append(n)
    return out


def _has_self_call(fn: FunctionInfo) -> bool:
    """True when the body names this function as a callee.

    The encoder treats a call as an unconstrained BitVec. That is not a
    proof of the callee, and overflow on the havoc is not a cex of the
    original recursive function.
    """
    name = fn.name
    if not name:
        return False
    return name in _call_names(fn.body or "")


_UNENCODED_CSTR = {
    "strcpy", "strcat", "strncpy", "strncat",
    "sprintf", "vsprintf", "snprintf", "vsnprintf",
    "gets", "scanf", "sscanf", "fscanf", "strtok",
    "memcpy", "memmove",
    "mkstemp", "mkstemps", "mkdtemp",
    "tmpnam", "tempnam", "tmpnam_r",
    "popen", "pclose",
}


def _has_unencoded_cstr(fn: FunctionInfo) -> bool:
    """Libc string copies are unconstrained BitVecs in BMC, not a buffer model."""
    return any(n in _UNENCODED_CSTR for n in _call_names(fn.body or ""))


_UNENCODED_LIBC_EFFECT = {
    "system", "exit", "_exit", "abort", "pthread_create",
    "thrd_create", "thrd_join", "thrd_detach", "thrd_exit",
    "thrd_sleep", "thrd_yield", "thrd_current", "thrd_equal",
    "printf", "fprintf",
    "umask", "srand", "srandom", "signal", "mktemp",
    "fork", "vfork",
    "execl", "execlp", "execle", "execv", "execvp", "execve", "execvpe",
    "mmap", "munmap", "mprotect",
    "ioctl",
    "dlopen", "dlsym", "dlclose",
    "accept", "accept4",
    "chmod", "fchmod",
    "setuid", "seteuid", "setgid",
    "socket", "bind", "listen", "connect",
    "pipe", "pipe2",
    "dup", "dup2", "dup3",
    "fcntl",
    "wait", "waitpid", "waitid",
    "unlink",
    "mkfifo", "mknod",
    "select", "pselect", "poll", "epoll_wait", "epoll_ctl",
    "send", "recv", "sendto", "recvfrom", "shutdown",
    "kill", "raise", "alarm",
    "getaddrinfo", "freeaddrinfo",
    "pthread_join", "pthread_detach", "pthread_once",
    "sem_wait", "sem_post", "sem_init", "sem_destroy",
    "openat", "flock",
    "posix_memalign", "aligned_alloc",
    "chown", "fchown", "lchown", "symlink", "readlink",
    "opendir", "readdir", "closedir", "fdopendir",
    "setrlimit", "getrlimit",
    "getsockopt", "setsockopt",
    "stat", "lstat", "fstat",
    "mkdir", "rmdir", "rename",
    "getpwuid", "getpwnam", "crypt",
    "clock_gettime", "gettimeofday",
    "shm_open", "shm_unlink",
    "posix_spawn", "posix_spawnp",
    "glob", "globfree",
    "fseek", "ftell", "rewind", "fgetpos", "fsetpos",
    "sleep", "usleep", "nanosleep",
    "access",
    "getopt", "getopt_long", "getopt_long_only",
    "uname", "gethostname",
    "sendfile", "copy_file_range",
    "memfd_create", "eventfd", "timerfd_create",
    "prctl", "ptrace",
    "tcgetattr", "tcsetattr", "cfmakeraw",
    "sysconf", "pathconf", "fpathconf",
    "getrusage",
    "nftw", "ftw",
    "wordexp", "wordfree",
    "getlogin", "getlogin_r", "ttyname", "ttyname_r",
    "inet_pton", "inet_ntop", "inet_aton",
    "mlock", "munlock", "mlockall", "munlockall",
    "madvise", "posix_madvise",
    "splice", "vmsplice",
    "inotify_init", "inotify_init1", "inotify_add_watch",
    "fsync", "fdatasync",
    "getrandom", "getentropy",
    "getline", "getdelim",
    "asprintf", "vasprintf",
    "strlcpy", "strlcat",
    "explicit_bzero", "memset_s", "explicit_memset",
    "isatty",
    "posix_openpt", "ptsname", "ptsname_r", "grantpt", "unlockpt",
    "umount2", "umount", "mount",
    "fmemopen", "open_memstream", "open_wmemstream",
    "scandir",
    "lsetxattr", "fsetxattr", "setxattr", "listxattr",
    "removexattr", "getxattr",
    "sched_setaffinity", "sched_getaffinity",
    "aio_suspend", "aio_return", "aio_error", "aio_write", "aio_read",
    "io_uring_register", "io_uring_setup", "io_uring_enter",
    "capset", "capget",
    "statx",
    "pidfd_send_signal", "pidfd_getfd", "pidfd_open",
    "fanotify_init", "fanotify_mark",
    "seccomp",
    "getgrnam", "getgrgid", "getspnam",
    "posix_fallocate", "fallocate",
    "close_range",
    "landlock_create_ruleset", "landlock_add_rule", "landlock_restrict_self",
    "getpriority", "setpriority",
    "signalfd",
    "bpf",
    "userfaultfd",
    "getpass",
    "initgroups", "setgroups",
    "clone", "unshare", "setns",
    "openat2",
    "sendmmsg", "recvmmsg",
    "name_to_handle_at", "open_by_handle_at",
    "process_madvise",
    "personality",
    "quotactl",
    "pivot_root",
    "membarrier",
    "pkey_alloc",
    "statfs",
    "syncfs",
    "prlimit", "prlimit64",
    "process_vm_readv",
    "perf_event_open",
    "clone3",
    "kcmp",
    "keyctl",
    "fsopen", "fsmount", "open_tree", "move_mount", "fspick", "fsconfig",
    "process_mrelease",
    "memfd_secret",
    "ioprio_set", "ioprio_get",
    "mq_open",
    "shmget",
    "futex",
    "adjtimex",
    "sethostname",
    "reboot",
    "swapon", "swapoff",
    "acct",
    "ioperm", "iopl",
    "mincore",
    "rseq",
    "timer_create",
    "semget",
    "msgget",
    "klogctl",
    "mount_setattr",
    "getcpu",
    "init_module", "finit_module", "delete_module",
    "kexec_load", "kexec_file_load",
    "quotactl_fd",
    "pkey_free", "pkey_mprotect",
    "tgkill",
    "add_key",
    "semctl",
    "msgctl",
    "shmctl",
    "timer_settime",
    "setdomainname",
    "io_submit", "io_getevents",
    "io_setup", "io_destroy", "io_cancel", "io_pgetevents",
    "request_key",
    "tkill",
    "timer_delete", "timer_gettime", "timer_getoverrun",
    "mq_unlink", "mq_timedsend", "mq_timedreceive", "mq_notify",
    "mq_getsetattr",
    "shmat", "shmdt",
    "semop", "semtimedop",
    "msgsnd", "msgrcv",
    "sync_file_range",
    "msync", "mremap",
    "socketpair",
    "sysinfo",
    "clock_settime", "clock_adjtime", "clock_nanosleep",
    "settimeofday",
    "gettid",
    "sched_setscheduler", "sched_getscheduler",
    "sched_setparam", "sched_getparam",
    "setitimer", "getitimer",
    "nice",
    "arch_prctl",
    "getdents", "getdents64",
    "utimensat", "futimens", "utimes",
    "linkat",
    "mbind", "set_mempolicy", "get_mempolicy",
    "futex_waitv",
    "syslog",
    "setpgid", "setsid", "getsid",
    "setreuid", "setregid", "setresuid", "setresgid",
    "getgroups",
    "epoll_create", "epoll_create1",
    "timerfd_settime", "timerfd_gettime",
    "remap_file_pages",
    "migrate_pages", "move_pages",
    "cachestat",
    "map_shadow_stack",
    "sched_yield",
    "setfsuid", "setfsgid",
    "wait4", "wait3",
    "preadv2", "pwritev2", "preadv", "pwritev",
    "sendmsg", "recvmsg",
    "getsockname", "getpeername",
    "epoll_pwait", "epoll_pwait2",
    "inotify_rm_watch",
    "eventfd_read", "eventfd_write",
    "sched_setattr", "sched_getattr",
    "renameat2",
    "execveat",
    "mlock2",
    "faccessat2",
    "ustat",
    "vhangup",
    "mseal",
    "futex_wake", "futex_wait", "futex_requeue",
    "listmount", "statmount",
    "lsm_get_self_attr", "lsm_set_self_attr", "lsm_list_modules",
    "set_mempolicy_home_node",
    "file_getattr", "file_setattr",
    "setxattrat", "getxattrat", "listxattrat", "removexattrat",
    "fchmodat2",
    "rt_sigqueueinfo", "rt_tgsigqueueinfo",
    "open_tree_attr",
    "posix_fadvise", "posix_fadvise64",
    "readahead",
    "sigaction",
    "sigprocmask", "sigsuspend",
    "sem_open", "sem_close", "sem_unlink",
    "pthread_rwlock_rdlock", "pthread_rwlock_wrlock",
    "pthread_rwlock_unlock", "pthread_rwlock_init",
    "pthread_rwlock_destroy",
    "pthread_rwlock_tryrdlock", "pthread_rwlock_trywrlock",
    "pthread_rwlock_timedrdlock", "pthread_rwlock_timedwrlock",
    "pthread_cond_wait", "pthread_cond_timedwait",
    "pthread_cond_signal", "pthread_cond_broadcast",
    "pthread_cond_init", "pthread_cond_destroy",
    "sigaltstack",
    "renameat",
    "faccessat",
    "fchmodat",
    "pthread_barrier_wait", "pthread_barrier_init",
    "pthread_barrier_destroy",
    "symlinkat",
    "unlinkat",
    "mkdirat",
    "mknodat",
    "readlinkat",
    "fstatat",
    "pthread_spin_lock", "pthread_spin_unlock", "pthread_spin_trylock",
    "pthread_spin_init", "pthread_spin_destroy",
    "pthread_key_create", "pthread_key_delete",
    "pthread_setspecific", "pthread_getspecific",
    "pthread_cancel",
    "pthread_kill",
    "pthread_sigmask",
    "pthread_atfork",
    "pledge", "unveil",
    "sysctl", "sysctlbyname",
    "kqueue", "kevent",
    "pause",
    "ppoll",
    "sigpending", "sigwaitinfo", "sigtimedwait", "sigwait",
    "sigqueue",
    "getcontext", "setcontext", "swapcontext", "makecontext",
    "sem_timedwait",
    "pthread_attr_init", "pthread_attr_destroy",
    "pthread_attr_setstacksize", "pthread_attr_setstack",
    "pthread_attr_setdetachstate",
    "pthread_attr_getstacksize", "pthread_attr_getstack",
    "pthread_attr_getdetachstate",
    "cap_enter",
    "cap_rights_limit", "cap_rights_get",
    "pdfork",
    "procctl",
    "closefrom",
    "issetugid",
    "arc4random", "arc4random_buf", "arc4random_uniform",
    "chflags", "fchflags", "lchflags",
    "getfsstat",
    "pthread_yield",
    "sem_trywait", "sem_getvalue",
    "adjtime", "ntp_adjtime",
    "revoke",
    "ktrace",
    "rfork",
    "jail", "jail_attach", "jail_get", "jail_set", "jail_remove",
    "setlogin",
    "getresuid", "getresgid",
    "getpeereid",
    "strtonum",
    "reallocarray",
    "timingsafe_bcmp", "timingsafe_memcmp",
    "getprogname", "setprogname",
    "setproctitle", "daemon",
    "cap_fcntls_limit", "cap_ioctls_limit",
    "pdgetpid", "pdwait4",
    "kldload", "kldunload", "kldfind", "kldsym", "kldstat",
    "extattr_set_file", "extattr_get_file", "extattr_delete_file",
    "extattr_list_file",
    "extattr_set_fd", "extattr_get_fd", "extattr_delete_fd",
    "extattr_list_fd",
    "extattr_set_link", "extattr_get_link", "extattr_delete_link",
    "extattr_list_link",
    "mac_set_proc", "mac_get_proc", "mac_set_fd", "mac_get_fd",
    "mac_set_file", "mac_get_file",
    "auditon", "getaudit", "setaudit", "auditctl",
    "kvm_open", "kvm_openfiles", "kvm_getprocs", "kvm_close", "kvm_nlist",
    "reallocf",
    "uuidgen",
    "setfib",
    "ntp_gettime",
    "crypt_newhash", "crypt_checkpass",
    "wait6",
    "cpuset_setaffinity", "cpuset_getaffinity",
    "rtprio", "rtprio_thread",
    "kenv",
    "getfh", "fhopen", "fhstat", "fhstatfs", "getfhat",
    "getmntinfo",
    "nmount",
    "strmode",
    "getosreldate",
    "cap_sandboxed",
    "getgrouplist",
    "eaccess",
    "login_getclass", "setusercontext",
    "fflagstostr", "strtofflags",
    "getdirentries",
    "kinfo_getproc", "kinfo_getfile", "kinfo_getvmmap",
    "_umtx_op",
    "thr_new", "thr_kill2", "thr_kill", "thr_self", "thr_exit",
    "thr_suspend", "thr_wake",
    "thrd_create", "thrd_join", "thrd_detach", "thrd_exit",
    "thrd_sleep", "thrd_yield", "thrd_current", "thrd_equal",
    "modfind", "modstat", "modnext", "modfnext",
    "lpathconf",
    "getloginclass", "setloginclass",
    "getfsent", "setfsent", "endfsent",
    "minherit",
    "cap_getmode",
    "nfssvc",
    "sysarch",
    "getpagesizes",
    "sbrk", "brk",
    "ksem_open", "ksem_close", "ksem_unlink", "ksem_wait", "ksem_post",
    "cap_getrights",
    "devname", "devname_r",
    "getbootfile",
    "kldfirstmod", "kldnextmod",
    "fhlink", "fhlinkat", "fhreadlink",
    "valloc",
    "getdomainname",
    "fts_open", "fts_read", "fts_children", "fts_close", "fts_set",
    "getvfsbyname",
    "unmount",
    "getpagesize",
    "lio_listio",
    "clock_getcpuclockid",
    "pthread_getcpuclockid",
    "sched_get_priority_max", "sched_get_priority_min",
    "posix_spawn_file_actions_init", "posix_spawnattr_init",
    "kld_isloaded", "kld_load",
    "dlfunc", "dlvsym",
    "posix_typed_mem_open", "posix_typed_mem_get_info",
    "throw_with_nested", "rethrow_if_nested",

    "__atomic_load", "__atomic_store",
    "__sync_fetch_and_add", "__sync_bool_compare_and_swap",
    "__builtin_unreachable", "__builtin_trap",
    "__builtin_clz", "__builtin_ctz", "__builtin_clzll", "__builtin_ctzll",
    "__builtin_choose_expr",
}


def _has_unencoded_libc_effect(fn: FunctionInfo) -> bool:
    """system/exit/pthread_create/printf/builtin trap have no encoded UB model.

    Applied only on the vacuous-proof path (no encoded properties), not
    as a generic 'any call' gate — that would break inlining of bump().
    """
    return any(n in _UNENCODED_LIBC_EFFECT for n in _call_names(fn.body or ""))


def unencoded_syntax_reason(fn: FunctionInfo, engine: str) -> str | None:
    r"""NEEDS-HARNESS message for syntax the encoder does not model.

    Missing model, not a parse ERROR and not a proof. Plain `goto` is not
    this case (the encoder models structured gotos). `offsetof` is in `_CALL_KW` so it is not a libc-effect
    call; the expression parser would otherwise havoc it and prove.
    `volatile` / `_Atomic` are a missing memory model, not a race proof.
    Local `const T x` is a missing const model; `const` on a parameter
    is not this case. `for (const int i = ...)` is that case; a cast
    `(const int)` is not. Local `struct S s` / anonymous `struct { }`
    / unknown typedef is missing layout, not a trailing-token ERROR.
    `register` / `auto` prefix decls are a missing storage-class model.
    Function-local `static int x` / `extern int x` are a missing storage-
    duration model when they would otherwise be a trailing-token ERROR.
    GNU `__auto_type` is a missing auto-type model. Anonymous
    `enum { RED = 1 } e;` is missing layout; named `enum { NAME = val }`
    constants used as ints are not that case. `_Alignas` / compound
    literals `(int){0}` in a local decl are missing models.
    C++ `new`/`delete` without a local pointer decl would otherwise be
    a trailing-token ERROR (`new int`); with a pointer decl the local-
    pointer gate already fires first.
    `memcpy`/`memmove`/`mkstemp` are unconstrained in every engine;
    `strcpy` stays BMC-only so the concrete oracle can still CRASH.
    `char t[] = "…"` is a missing string-array model, not a parse ERROR.
    Unary address-of in a call (`printf(..., &n)`) is a missing pointer-value
    model, not a `bad token &` ERROR.     `chroot()` is an unmodeled jail.
    `_Thread_local` / C++ `thread_local` locals are a missing TLS model.
    `_Complex` / `_Imaginary` locals are a missing complex model (not IEEE).
    `typeof(n) y` / `__typeof__(n) y` are a missing typeof model; `sizeof`
    still encodes. GNU nested `void inner(void) { }` is a missing nested-
    function model.     Computed `goto *p` is a missing computed-goto model;
    plain `goto label` is encoded (or "unstructured goto unencoded").
    GNU `&&label` (label address) is a missing label-address model;
    it is not computed `goto *` and not plain `goto`.
    C++ `dynamic_cast` / `typeid` / `reinterpret_cast` are missing RTTI
    or type-pun models; `static_cast` is not this case.
    `__attribute__((packed))` / `__packed` locals are a missing packed-
    layout model. C++ `co_await` / `co_yield` / `co_return` are a missing
    coroutine model. Wide `L"…"` / `L'x'` are a missing wide-char model.
    `fork`/`exec*` is unconstrained process spawn, not a proof.
    `mmap`/`munmap`/`mprotect` are a missing VM model.
    `wcscpy`/`wcscat`/`wcsncpy`/`wcsncat` are unconstrained wide copies;
    `strcpy`/`strcat`/`sprintf` are not this case.
    `__int128` / `_BitInt` are a missing 128-bit model; ordinary `int`
    is not. GNU `case 1 ... 3:` is a missing case-range model; ordinary
    `case 1:` still encodes. `std::bit_cast` / `bit_cast<` is a missing
    type-pun model; `static_cast` is not. `if constexpr` is a missing
    compile-time-if model; ordinary `if (n)` still encodes.
    `dlopen`/`dlsym`/`dlclose` are a missing dynamic-loader model.
    `__builtin_clz`/`__builtin_ctz` (and `ll`) are UB on 0; an
    unconstrained call is not a proof. `accept`/`accept4` and
    `chmod`/`fchmod` are unconstrained libc effects, not a proof
    (like `ioctl`). `setuid`/`seteuid`/`setgid`, `socket`, `bind`,
    `unlink`, and `mkfifo`/`mknod` are unconstrained libc effects,
    not a proof (like `ioctl`). `strcpy` stays out of this function.
    C23 `nullptr` as an identifier
    in the body is a missing nullptr model; a comment is not that case
    and ordinary `return 0` still encodes. `std::launder` / `launder(`
    is a missing lifetime model. C++ fold `(... op pack)` is a missing
    fold-expression model; GNU `case 1 ... 3:` is not that case.
    `_Decimal32`/`_Decimal64`/`_Decimal128` are a missing decimal-float
    model. `strcpy`/`strcat`/`sprintf` stay out of this function.
    C++ `std::vector` / `vector<` is a missing container model.
    `listen`/`connect` are unconstrained socket ops, not a proof.
    `pipe`/`pipe2` are a missing pipe model. `dup`/`dup2`/`dup3`
    are a missing fd model. `fcntl` is unconstrained.
    `wait`/`waitpid`/`waitid` are unconstrained process wait.
    C++ `std::expected` / `expected<` is a missing expected model.
    C++ `std::format` / `std::print` / `std::println` is a missing
    format model; a bare `format(` is not this case.
    C++ spaceship `<=>` / `operator<=>` is a missing three-way
    comparison model; ordinary `<=` / `>=` still encode.
    GNU `__attribute__((cleanup` is a missing cleanup model.
    `constexpr` in a function body is a missing constexpr model;
    `if constexpr` is the compile-time-if gate; ordinary `const int`
    is the const-local gate. `strcpy`/`strcat`/`sprintf` stay out.
    `select`/`pselect`/`poll`/`epoll_wait`/`epoll_ctl` are unconstrained
    I/O multiplex. `send`/`recv`/`sendto`/`recvfrom`/`shutdown` are
    unconstrained socket I/O. `kill`/`raise`/`alarm` are unconstrained
    signal delivery. `getaddrinfo`/`freeaddrinfo` are unconstrained DNS.
    `pthread_join`/`pthread_detach`, `sem_wait`/`sem_post`, `openat`,
    `flock`, `chown`/`fchown`/`lchown`, and `symlink`/`readlink` are
    unconstrained libc effects, not a proof (like `fcntl`).
    C++ `std::jthread` is a missing jthread model (distinct from
    `std::thread`). C++ `std::async`/`std::future`/`std::promise` is
    a missing future model. C++ `std::function` / `function<` is a
    missing type-erased callable model. C++ `std::mdspan` / `mdspan<`
    is a missing mdspan model. C++ `std::mutex` / `std::lock_guard` /
    `std::unique_lock` / `std::scoped_lock` is a missing C++ mutex
    model; `pthread_mutex_t`/`mtx_t` stay on the C lock gate. GNU
    `__attribute__((vector_size` / `__vector_size` is a missing SIMD
    vector model; ordinary `int` is not. `strcpy` stays out.
    `pthread_join`/`pthread_detach`/`pthread_once` are unconstrained
    thread join (pthread_create is already a libc-effect gate).
    ISO C11 `thrd_create`/`thrd_join`/`thrd_detach`/`thrd_exit`/
    `thrd_sleep`/`thrd_yield`/`thrd_current`/`thrd_equal` are a missing
    C11 threads model (not POSIX `pthread_*`, not FreeBSD `thr_*`).
    `sem_wait`/`sem_post`/`sem_init`/`sem_destroy` are a missing
    semaphore model. `openat`/`flock` are unconstrained fd ops.
    `posix_memalign`/`aligned_alloc` are a missing aligned-alloc model.
    C++ `std::condition_variable` / `condition_variable` /
    `std::shared_mutex` / `shared_mutex` are a missing condvar/
    shared-mutex model. C++ `std::atomic_ref` / `atomic_ref<` is a
    missing atomic_ref model. C++ `std::generator` / `generator<`
    is a missing generator model. C++ `[[assume(` is a missing assume
    attribute; ordinary functions still encode. C++ `std::bind(` is
    a missing bind model; POSIX `bind(` stays on the socket-bind gate
    and is not this case. GNU/C11
    `__atomic_load`/`__atomic_store`/`__sync_fetch_and_add`/
    `__sync_bool_compare_and_swap` are a missing atomic-builtin
    model; ordinary `+=` is not. `chown`/`fchown`/`lchown`/
    `symlink`/`readlink` are unconstrained fs ops. `strcpy` stays out.
    `opendir`/`readdir`/`closedir`/`fdopendir` are a missing DIR*
    model (FILE/DIR locals may already be a pointer harness; VOID
    `opendir()` plants are still not a proof). `setrlimit`/`getrlimit`,
    `getsockopt`/`setsockopt`, `stat`/`lstat`/`fstat` (struct local
    may fire first), `mkdir`/`rmdir`/`rename`, and
    `getpwuid`/`getpwnam`/`crypt` are unconstrained libc effects.
    C++ `std::any` / `any_cast` is a missing any model. C++
    `std::filesystem` / `std::fs::` / `filesystem::` is a missing
    filesystem model. C++ `std::regex` / `std::regex_` is a missing
    regex model (tight; a bare `regex` is not this case). C++
    `std::latch` / `std::barrier` / `std::counting_semaphore` is a
    missing sync-primitive model (std:: prefix). C++ `std::from_chars`
    / `from_chars(` is a missing charconv model.     C++ `std::visit` is
    a missing visitor model (tight; a bare `visit(` is not this case).
    C++ `initializer_list<` is a missing temporary-lifetime model.
    `clock_gettime`/`gettimeofday` are unconstrained clocks; a bare
    `time(` is not this case (too broad). `shm_open`/`shm_unlink`,
    `posix_spawn`/`posix_spawnp`, `glob(`/`globfree` (not `glob_t`),
    `fseek`/`ftell`/`rewind`/`fgetpos`/`fsetpos`,
    `sleep`/`usleep`/`nanosleep`, and `access` are unconstrained libc
    effects (VOID sleep/access would otherwise vacuous-PROVE).
    `getopt`/`getopt_long`/`getopt_long_only`, `uname`/`gethostname`,
    `sendfile`/`copy_file_range`, `memfd_create`/`eventfd`/`timerfd_create`,
    `prctl`/`ptrace`, and `tcgetattr`/`tcsetattr`/`cfmakeraw` are
    unconstrained libc effects (VOID plants would otherwise
    vacuous-PROVE). C++
    `std::source_location`, `std::stacktrace`,
    `std::stop_token`/`std::stop_source`/`std::stop_callback`,
    `std::flat_map` / `flat_map<`, `import`/`export module`, and
    `#pragma pack` are missing models. `strcpy`/`strcat`/`sprintf`
    stay out.
    C++ `std::chrono` / `chrono::`,
    `std::function_ref` / `function_ref<`,
    `std::move_only_function` / `move_only_function<` /
    `std::copyable_function` / `copyable_function<`,
    `std::flat_set` / `flat_set<`, `std::ranges::views` /
    `std::views::`, and `std::inplace_vector` / `inplace_vector<`
    are missing models. `strcpy`/`strcat`/`sprintf` stay out.
    `sysconf`/`pathconf`/`fpathconf`, `getrusage` (not a bare
    `times(` — that can match C++ chrono), `nftw`/`ftw`,
    `wordexp`/`wordfree`, `getlogin`/`getlogin_r`/`ttyname`/
    `ttyname_r`, and `inet_pton`/`inet_ntop`/`inet_aton` are
    unconstrained libc effects (VOID plants would otherwise
    vacuous-PROVE). `mlock`/`munlock`/`mlockall`/`munlockall`,
    `madvise`/`posix_madvise`, `splice`/`vmsplice` (not bare
    `tee(` — that matches C++ iostream), `inotify_init`/
    `inotify_init1`/`inotify_add_watch`, `fsync`/`fdatasync`
    (not bare `sync()`), `getrandom`/`getentropy`,
    `getline`/`getdelim`, and `asprintf`/`vasprintf` are
    unconstrained libc effects (VOID plants would otherwise
    vacuous-PROVE). `strlcpy`/`strlcat` (not `strcpy`/`strcat`),
    `explicit_bzero`/`memset_s`/`explicit_memset`, `isatty`,
    `posix_openpt`/`ptsname_r`/`ptsname`/`grantpt`/`unlockpt`,
    `umount2`/`umount`/`mount`, `open_wmemstream`/`open_memstream`/
    `fmemopen`, and `scandir` are unconstrained libc effects
    (VOID plants would otherwise vacuous-PROVE).
    `setxattr`/`lsetxattr`/`fsetxattr`/`getxattr`/`listxattr`/
    `removexattr`, `sched_setaffinity`/`sched_getaffinity`,
    `aio_read`/`aio_write`/`aio_error`/`aio_return`/`aio_suspend`,
    `io_uring_setup`/`io_uring_enter`/`io_uring_register`,
    `capset`/`capget`, `statx`, and
    `pidfd_open`/`pidfd_send_signal`/`pidfd_getfd` are unconstrained
    libc effects (VOID plants would otherwise vacuous-PROVE).
    `fanotify_init`/`fanotify_mark`, `seccomp`, `getgrnam`/`getgrgid`/
    `getspnam`, `posix_fallocate`/`fallocate`, `close_range`,
    `landlock_create_ruleset`/`landlock_add_rule`/`landlock_restrict_self`,
    `getpriority`/`setpriority`, and `signalfd` are unconstrained libc
    effects (VOID plants would otherwise vacuous-PROVE).
    `bpf`, `userfaultfd`, `getpass`, `initgroups`/`setgroups`,
    `unshare`/`setns`/`clone`, `openat2` (not `openat`),
    `sendmmsg`/`recvmmsg` (not `send`/`recv`/`sendto`),
    `name_to_handle_at`/`open_by_handle_at`, `process_madvise`
    (not `madvise`), `personality`, and `quotactl` are unconstrained
    libc effects (VOID plants would otherwise vacuous-PROVE).
    `pivot_root`, `membarrier`, `pkey_alloc` (not a bare `pkey`),
    `statfs` (not `stat`/`fstat`/`statx`), `syncfs` (not `fsync`,
    not bare `sync()`), `prlimit`/`prlimit64` (not `prctl`),
    `process_vm_readv` (not `process_madvise`), and
    `perf_event_open` are unconstrained libc effects (VOID plants
    would otherwise vacuous-PROVE).
    `clone3` (not `clone`/`unshare`/`setns`), `kcmp`, `keyctl`,
    `fsopen`/`fsmount`/`open_tree`/`move_mount`/`fspick`/`fsconfig`
    (not `openat`/`mount`), `process_mrelease` (not `process_madvise`),
    `memfd_secret` (not `memfd_create`), `ioprio_set`/`ioprio_get`,
    `mq_open`, `shmget` (not `shm_open`), `futex`, `adjtimex`, and
    `sethostname` (not `gethostname`/`uname`) are unconstrained libc
    effects (VOID plants would otherwise vacuous-PROVE).
    `reboot`, `swapon`/`swapoff`, `acct`, `ioperm`/`iopl`,
    `mincore`, `rseq`, `timer_create` (not `timerfd_create`),
    `semget` (not `sem_wait`/`sem_init`), `msgget`, `klogctl`
    (not a bare `syslog(`), `mount_setattr` (not `mount`/`umount`),
    and `getcpu` are unconstrained libc effects (VOID plants
    would otherwise vacuous-PROVE).
    `init_module`/`finit_module`/`delete_module`, `kexec_load`
    (not a bare `kexec`), `quotactl_fd` (not `quotactl`),
    `pkey_free` (not `pkey_alloc`), `tgkill` (not `kill`),
    `add_key` (not `keyctl`), `semctl` (not `semget`/`sem_wait`),
    `msgctl` (not `msgget`), `shmctl` (not `shmget`/`shm_open`),
    `timer_settime` (not `timer_create`/`timerfd_create`),
    `setdomainname` (not `sethostname`/`gethostname`), and
    `io_submit`/`io_getevents` (not `io_uring_*`/`aio_read`)
    are unconstrained libc effects (VOID plants would otherwise
    vacuous-PROVE).
    `io_setup`/`io_destroy`/`io_cancel`/`io_pgetevents` (not
    `io_submit`/`io_getevents`/`io_uring_*`/`aio_read`),
    `request_key` (not `keyctl`/`add_key`), `tkill` (not `tgkill`/
    `kill`), `timer_delete`/`timer_gettime`/`timer_getoverrun`
    (not `timer_create`/`timer_settime`/`timerfd_*`),
    `mq_unlink`/`mq_timedsend`/`mq_timedreceive`/`mq_notify`/
    `mq_getsetattr` (not `mq_open`), `shmat`/`shmdt` (not `shmget`/
    `shmctl`/`shm_open`), `semop`/`semtimedop` (not `semget`/
    `semctl`/`sem_wait`/`sem_post`), `msgsnd`/`msgrcv` (not
    `msgget`/`msgctl`), `sync_file_range` (not bare `sync(`),
    `msync`/`mremap` (not `mmap`/`mprotect`/`munmap`),
    `socketpair` (not `socket`/`pipe`), and `sysinfo` (not
    `getrusage`/`uname`) are unconstrained libc effects (VOID
    plants would otherwise vacuous-PROVE).
    C++ `std::pmr`/`pmr::` (not a bare `pmr` ident),
    `u8string`/`u8string_view` (not `string_view`/`wstring`),
    `unordered_multimap` (not `unordered_map`/`multimap`/
    `flat_multimap`), `unordered_multiset` (not `unordered_set`/
    `multiset`/`flat_multiset`), `shared_lock` (not `unique_lock`
    and not `shared_mutex` as the only token),     and `atomic_flag`
    (not `std::atomic<`) are missing models.
    C++ `condition_variable_any` (not `condition_variable`),
    `recursive_mutex` (not `std::mutex`/`recursive_timed_mutex`),
    `timed_mutex` (not `std::mutex`/`recursive_timed_mutex`/
    `shared_timed_mutex`), `ifstream`/`ofstream`/`fstream` (not
    `stringstream`/`spanstream`/`iostream`), `this_thread` (not
    `std::thread`/`jthread`), and `call_once` (not `pthread_once`)
    are missing models.
    `clock_settime`/`clock_adjtime`/`clock_nanosleep` (not
    `clock_gettime`/`gettimeofday`), `settimeofday` (not
    `gettimeofday`), `gettid` (not `gettimeofday`/`gettime`/`getpid`),
    `sched_setscheduler`/`sched_getscheduler`/`sched_setparam`/
    `sched_getparam` (not `sched_setaffinity`), `setitimer`/`getitimer`
    (not `timer_create`/`timer_settime`/`timerfd_*`), `nice` (not
    `getpriority`/`setpriority`), `arch_prctl` (not `prctl`),
    `getdents`/`getdents64`, `utimensat`/`futimens`/`utimes` (not a
    bare `time(`), `linkat` (not `unlink`/`symlink`), `mbind`/
    `set_mempolicy`/`get_mempolicy` (not `mmap`/`mprotect`), and
    `futex_waitv` (before a bare `futex`) are unconstrained libc
    effects (VOID plants would otherwise vacuous-PROVE).
    `syslog` (not `klogctl`), `setpgid`/`setsid`/`getsid` (not
    `getpid`/`setuid`), `setreuid`/`setregid`/`setresuid`/`setresgid`
    (not `setuid`/`seteuid`/`setgid`), `getgroups` (not `initgroups`/
    `setgroups`), `epoll_create`/`epoll_create1` (not `epoll_wait`/
    `epoll_ctl`), `timerfd_settime`/`timerfd_gettime` (not
    `timerfd_create`/`timer_settime`/`timer_create`),
    `remap_file_pages` (not `mmap`/`mprotect`/`mremap`),
    `migrate_pages`/`move_pages` (not `process_vm_readv`/`mbind`),
    `cachestat`, `map_shadow_stack`, `sched_yield` (not
    `sched_setaffinity`/`sched_setscheduler`), and `setfsuid`/`setfsgid`
    (not `setuid`/`setgid`) are unconstrained libc effects (VOID plants
    would otherwise vacuous-PROVE).
    `wait4`/`wait3` (not `wait`/`waitpid`/`waitid`), `preadv2`/`pwritev2`/
    `preadv`/`pwritev` (not `read`/`write`/`pread`/`pwrite`), `sendmsg`/
    `recvmsg` (not `sendmmsg`/`recvmmsg`/`sendto`/`send(`), `getsockname`/
    `getpeername` (not `getsockopt`/`setsockopt`), `epoll_pwait`/
    `epoll_pwait2` (before `epoll_wait`; not `epoll_create`),
    `inotify_rm_watch` (not `inotify_init`/`inotify_add_watch`),
    `eventfd_read`/`eventfd_write` (not `eventfd(`/`memfd_create`),
    `sched_setattr`/`sched_getattr` (not `sched_setaffinity`/
    `sched_setscheduler`/`sched_yield`), `renameat2` (not `rename`/
    `renameat`), `execveat` (not `execve`/`execl`/`execvp`), `mlock2`
    (not `mlock`/`mlockall`), and `faccessat2` (not `access`/`faccessat`)
    are unconstrained libc effects (VOID plants would otherwise
    vacuous-PROVE).
    `ustat` (not `stat`/`fstat`/`statx`/`statfs`/`statmount`),
    `vhangup`, `mseal` (not `mlock`/`msync`/`mmap`), `futex_wake`/
    `futex_wait`/`futex_requeue` (after `futex_waitv`; not bare `futex(`),
    `listmount`/`statmount` (not `mount`/`stat`/`statfs`),
    `lsm_get_self_attr`/`lsm_set_self_attr`/`lsm_list_modules`,
    `set_mempolicy_home_node` (before `set_mempolicy`; not `mbind`),
    `file_getattr`/`file_setattr` (not `getattr`/`stat`),
    `setxattrat`/`getxattrat`/`listxattrat`/`removexattrat` (before
    `setxattr`/`getxattr`), `fchmodat2` (not `chmod`/`fchmod`/`fchmodat`
    as a prefix steal), `rt_sigqueueinfo`/`rt_tgsigqueueinfo` (not
    `kill`/`tgkill`/`signalfd`), and `open_tree_attr` (before
    `open_tree`/`fsopen`) are unconstrained libc effects (VOID plants
    would otherwise vacuous-PROVE).
    `posix_fadvise`/`posix_fadvise64` (not `posix_madvise`/`madvise`),
    `readahead`, `sigaction` (not `signal(`/`signalfd`),
    `sigprocmask`/`sigsuspend` (not `signal`/`sigaction`),
    `sem_open`/`sem_close`/`sem_unlink` (after `sem_wait`/`sem_post`;
    not `semget`/`semctl`/`semop`),
    `pthread_rwlock_rdlock`/`wrlock`/`unlock`/`init`/`destroy` and
    try/timed variants (not `pthread_mutex`/`pthread_join`),
    `pthread_cond_wait`/`timedwait`/`signal`/`broadcast`/`init`/
    `destroy` (not C++ `condition_variable`), `sigaltstack`,
    `renameat` (after `renameat2`; word-bounded, not `rename`),
    `faccessat` (after `faccessat2`; not `access`),
    `fchmodat` (after `fchmodat2`; not `chmod`),
    `pthread_barrier_wait`/`init`/`destroy` (not C++ `std::barrier`),
    `symlinkat` (before `symlink`; not `symlink(`),
    `unlinkat` (before `unlink`; not `unlink(`),
    `mkdirat` (before `mkdir`; not `mkdir(`),
    `mknodat` (before `mknod`/`mkfifo`),
    `readlinkat` (before `readlink`; not `readlink(`),
    `fstatat` (before `fstat`/`stat`/`statx`/`statfs`/`statmount`/`ustat`),
    `pthread_spin_lock`/`unlock`/`trylock`/`init`/`destroy` (not
    `pthread_mutex`/`pthread_rwlock`/`pthread_join`),
    `pthread_key_create`/`key_delete`/`setspecific`/`getspecific`,
    `pthread_cancel` (not `pthread_create`/`pthread_join`),
    `pthread_kill` (not POSIX `kill(`/`tgkill`/`tkill`),
    `pthread_sigmask` (not `sigprocmask`/`sigaction`),
    `pthread_atfork` (not `fork`/`vfork`),
    `pledge` (OpenBSD sandbox; call required, not a comment-only
    English word), `unveil`, `sysctlbyname`/`sysctl` (longer first),
    `kqueue` (not `kevent`), `kevent` (not `kqueue`), `pause`
    (not `pselect`/`nanosleep`), `ppoll` (before `poll(`; not
    `poll`/`pselect`/`epoll`; `"poll" in "ppoll"` is TRUE — use
    word-bounded `ppoll`), `sigwaitinfo`/`sigtimedwait`/`sigpending`/`sigwait`
    (longer first; not `sigaction`/`sigprocmask`/`signalfd`),
    `sigqueue` (after `rt_sigqueueinfo`/`rt_tgsigqueueinfo`;
    `"sigqueue" in "rt_sigqueueinfo"` is TRUE — use word-bounded `sigqueue`),
    `getcontext`/`setcontext`/`swapcontext`/`makecontext` (not
    `setjmp`/`longjmp`), `sem_timedwait` (after `sem_wait`/`sem_open`;
    `sem_wait(` does not match `sem_timedwait`), and
    `pthread_attr_init`/`destroy`/`setstacksize`/`setstack`/
    `setdetachstate` (not `pthread_create`/`pthread_join`/`pthread_atfork`),
    `cap_enter` (FreeBSD Capsicum; not `pledge`),
    `cap_rights_limit`/`cap_rights_get` (not `capset`/`capget`/`pledge`),
    `pdfork` (after `fork`/`pthread_atfork`; not `fork`/`vfork`;
    `"fork" in "pdfork"` is TRUE — word-bounded `pdfork`),
    `procctl` (not `prctl`/`arch_prctl`),
    `closefrom` (not `close_range`/`close(`),
    `issetugid`,
    `arc4random`/`arc4random_buf`/`arc4random_uniform` (not
    `getrandom`/`getentropy`/`rand`),
    `chflags`/`fchflags`/`lchflags` (not `chmod`/`chown`),
    `getfsstat` (not `statfs`/`statvfs`/`stat`),
    `pthread_yield` (not `sched_yield`),
    `sem_trywait`/`sem_getvalue` (not `sem_wait`/`sem_timedwait`/`sem_open`;
    `"wait" in "trywait"` — word-bounded `sem_trywait`),
    `ntp_adjtime`/`adjtime` (after `adjtimex`; not `adjtimex`/
    `clock_adjtime`; `"adjtime" in "adjtimex"` is TRUE — `\badjtime\b`),
    `revoke`,
    `ktrace` (not `ptrace`/`prctl`),
    `rfork` (after `pdfork`; not `fork`/`vfork`/`pdfork`;
    `"fork" in "rfork"` is TRUE — `\brfork\b`),
    `jail`/`jail_attach`/`jail_get`/`jail_set`/`jail_remove`
    (not `chroot`),
    `setlogin` (not `getlogin`/`getlogin_r`; `\bsetlogin\b`),
    `getresuid`/`getresgid` (not `setresuid`/`setreuid`/`setuid`;
    `\bgetresuid\b`),
    `getpeereid` (not `getpeername`),
    `strtonum` (not `strtol`/`atoi`),
    `reallocarray` (not `realloc(`; `"realloc" in "reallocarray"` is
    TRUE — `\breallocarray\b`),
    `timingsafe_bcmp`/`timingsafe_memcmp` (not `memcmp`/`explicit_bzero`/
    `bzero`),
    `getprogname`/`setprogname`,
    `setproctitle`/`daemon` (libc `daemon(`; not a comment word),
    `cap_fcntls_limit`/`cap_ioctls_limit` (not `cap_enter`/
    `cap_rights_limit`; `"fcntl" in "cap_fcntls_limit"` —
    `\bcap_fcntls_limit\b`),
    `pdgetpid`/`pdwait4` (not `pdfork`/`wait4`/`getpid`;
    `"wait4" in "pdwait4"` is TRUE — `\bpdwait4\b`),
    `kldload`/`kldunload`/`kldfind`/`kldsym`/`kldstat` (not `dlopen`/
    `init_module`),
    `extattr_set_file`/`extattr_get_file`/`extattr_delete_file` (not
    `setxattr`/`getxattr`),
    `mac_set_proc`/`mac_get_proc`/`mac_set_fd` (not `pledge`),
    `auditon`/`getaudit`/`setaudit`/`auditctl`,
    `kvm_open`/`kvm_getprocs`/`kvm_close`,
    `reallocf` (not `realloc(`/`reallocarray(`; after `reallocarray`;
    `"realloc" in "reallocf"` is TRUE — `\breallocf\b`),
    `uuidgen`,
    `setfib`,
    `ntp_gettime` (after `ntp_adjtime`; not `ntp_adjtime`/`adjtime`/
    `gettimeofday`),
    `crypt_newhash`/`crypt_checkpass` (not bare `crypt(`;
    `"crypt" in "crypt_newhash"` is TRUE — `\bcrypt_newhash\b`),
    `wait6` (after `wait4`/`pdwait4`; not `wait`/`wait4`/`waitpid`/
    `waitid`/`pdwait4`; `"wait" in "wait6"` is TRUE — `\bwait6\b`),
    `cpuset_setaffinity`/`cpuset_getaffinity` (not `sched_setaffinity`),
    `rtprio`/`rtprio_thread` (not `nice`/`getpriority`),
    `kenv` (not `getenv`),
    `getfh`/`fhopen`/`fhstat`/`fhstatfs`/`getfhat` (not `open`/`stat`/
    `statfs`),
    `getmntinfo` (not `getfsstat`/`statfs`/`getmntent`),
    `nmount` (before `mount`; not `mount`/`umount`/`listmount`;
    `"mount" in "nmount"` is TRUE — `\bnmount\b`),
    `strmode` (not `strtonum`),
    `getosreldate` (not `uname`),
    `cap_sandboxed` (not `cap_enter`/`cap_fcntls_limit`),
    `getgrouplist` (not `getgroups`/`initgroups`/`setgroups`),
    `eaccess` (before `access`; not `access`/`faccessat`/`faccessat2`;
    `"access" in "eaccess"` is TRUE — `\beaccess\b`),
    `login_getclass`/`setusercontext` (not `getlogin`/`setlogin`;
    `\blogin_getclass\b`),
    `fflagstostr`/`strtofflags` (not `strmode`/`strtonum`),
    `getdirentries` (not `getdents`/`readdir`/`opendir`;
    `"getdents" in "getdirentries"` — `\bgetdirentries\b`),
    `kinfo_getproc`/`kinfo_getfile`/`kinfo_getvmmap` (not `kvm_open`/
    `kvm_getprocs`),
    `_umtx_op` (not `futex`/`futex_wait`),
    `thr_new`/`thr_kill`/`thr_kill2` (not `pthread_create`/`pthread_kill`;
    `"kill" in "thr_kill"` — `\bthr_kill\b`),
    `modfind`/`modstat`/`modnext`/`modfnext` (not `kldload`/`dlopen`),
    `lpathconf` (before `pathconf`; not `pathconf`/`fpathconf`/`sysconf`;
    `"pathconf" in "lpathconf"` is TRUE — `\blpathconf\b`),
    `getloginclass`/`setloginclass` (not `getlogin`/`setlogin`/
    `login_getclass`; `"login" in all` — require `loginclass`),
    `getfsent`/`setfsent`/`endfsent` (not `getfsstat`/`getmntinfo`),
    `minherit` (not `mmap`/`mprotect`/`mlock`),
    `cap_getmode` (not `cap_enter`/`cap_sandboxed`/`cap_fcntls_limit`),
    `nfssvc`, `sysarch` (not `syscall`),
    `getpagesizes` (before `getpagesize`; not `getpagesize`/`sysconf`;
    `"pagesize" in both` — require `getpagesizes`),
    `sbrk`/`brk` (before `brk`; not `malloc`; word-bounded `sbrk`
    then `brk (` so `"brk" in "sbrk"` / `abort` do not steal),
    `ksem_open`/`ksem_close`/`ksem_unlink` (before `sem_open`;
    not `sem_open`/`sem_wait`; `"sem_" in "ksem_"` — require `ksem_`),
    `cap_getrights` (not `cap_rights_get`/`cap_rights_limit`/
    `cap_getmode`; `"rights" in both` — require `cap_getrights`),
    `devname`/`devname_r`, `getbootfile`,
    `kldfirstmod`/`kldnextmod` (not `kldload`/`modfind`),
    `fhlink`/`fhlinkat`/`fhreadlink` (before `link`/`linkat`;
    not `link`/`linkat`/`getfh`; `"link" in "fhlink"` — `\bfhlink`),
    `valloc` (not `posix_memalign`/`aligned_alloc`/`malloc`),
    `getdomainname` (not `setdomainname`/`gethostname`/`uname`;
    `"domainname" in both get/set` — require `getdomainname`),
    `fts_open`/`fts_read`/`fts_children`/`fts_close`/`fts_set` (not
    `opendir`/`readdir`),
    `getvfsbyname` (not `getfsstat`/`getfsent`),
    `unmount` (before `umount`/`mount`; `"mount" in "unmount"` is TRUE —
    `\bunmount\b`),
    `getpagesize` (after `getpagesizes`; not `getpagesizes`/`sysconf`;
    `"getpagesize" in "getpagesizes"` is TRUE —
    `\bgetpagesize(?!s)\s*\(`),
    `lio_listio` (not `aio_read`/`aio_write`),
    `clock_getcpuclockid` (not `clock_gettime`/`clock_settime`),
    `pthread_getcpuclockid` (not `pthread_create`/`clock_getcpuclockid`),
    `sched_get_priority_max`/`sched_get_priority_min` (not
    `sched_setaffinity`/`getpriority`),
    `posix_spawn_file_actions_init`/`posix_spawnattr_init` (before
    `posix_spawn`; not `posix_spawn`/`posix_spawnp`),
    `kld_isloaded`/`kld_load` (not `kldload`/`kldfirstmod`;
    `"kldload" vs `kld_load` — require `kld_isloaded` or `kld_load`),
    `dlfunc`/`dlvsym` (not `dlopen`/`dlsym`),
    `posix_typed_mem_open`/`posix_typed_mem_get_info` (not `shm_open`/`mmap`)
    are unconstrained libc effects (VOID plants would otherwise
    vacuous-PROVE).
    C++ `error_category` (not `error_code`/`system_error` as the only
    token), `nested_exception`/`throw_with_nested`/`rethrow_if_nested`
    (not `exception_ptr`), `atomic_thread_fence`/`atomic_signal_fence`
    (not `atomic_flag`/`atomic_ref`/`__atomic_`),
    `notify_all_at_thread_exit` (not `condition_variable`/`notify_all`
    as the only token), `wstring_convert` (not a bare `wstring`/
    `wstring_view`/`codecvt`), and `std::invoke` (not `invoke_result`
    or a bare `invoke(`) are missing models.
    C++ `std::apply` (not `std::invoke`), `reference_wrapper` (not
    `std::function`; `std::ref`/`std::cref` if tight), `std::endian`
    (not `byteswap`), `bit_ceil`/`bit_floor`/`has_single_bit`/
    `std::popcount` (not `byteswap`/`__builtin_clz`),
    `uncaught_exceptions` (not `exception_ptr`/`current_exception` as
    the only token), `current_exception` (not `exception_ptr`/
    `uncaught_exceptions` as the only token), `quick_exit`/`at_quick_exit`
    (before `exit`; not `abort`/`_exit`), `to_array` (before `std::array`),
    `zoned_time` (before generic chrono/`tzdb`; not `current_zone`),
    `kill_dependency` (not POSIX `kill(`), `std::rotl`/`std::rotr`
    (not `byteswap`/`bit_ceil`),     `std::bit_width` (not `bit_ceil`/
    `bit_floor`/`has_single_bit`), `std::gcd`, `std::lcm`,
    `std::clamp` (not `midpoint`/`lerp`), `std::exchange` (not
    `std::move`),     `std::to_address` (not `addressof`),
    `std::addressof` (not `to_address`),
    `std::assume_aligned` (not `[[assume` / `std::unreachable`),
    `std::as_const` (not `const_cast`),
    `as_rvalue`/`as_rvalue_view` (not `std::as_const`/`views::as_const`),
    `std::exclusive_scan`,
    `std::transform_inclusive_scan`/`transform_exclusive_scan`
    (before `inclusive_scan`; not bare `inclusive_scan`/
    `exclusive_scan`),
    `std::inclusive_scan` (not `exclusive_scan`; `"scan" in both` —
    require `inclusive_scan`), `std::transform_reduce`,
    `std::reduce` (after `transform_reduce`; require `std::` so
    English "reduce" / `transform_reduce` do not steal),
    `uninitialized_fill`/`uninitialized_fill_n`/
    `uninitialized_default_construct` (not `uninitialized_copy`),
    `uninitialized_value_construct`/`uninitialized_value_construct_n`
    (after `uninitialized_fill`; not `uninitialized_fill`/
    `uninitialized_copy`/`uninitialized_default_construct`),
    `uninitialized_copy`/`uninitialized_move`/`uninitialized_copy_n`/
    `uninitialized_move_n`, `construct_at`/`destroy_at` (not
    `to_address`/`addressof`), `destroy_n` (not `destroy_at`;
    require `destroy_n`), `std::add_sat`/`sub_sat`/`mul_sat`/
    `div_sat`/`saturate_cast`, `std::type_identity` (not `typeid(`),
    `std::nontype`, `std::is_layout_compatible`,
    `is_pointer_interconvertible_with_class`/
    `is_pointer_interconvertible_base_of`,
    `std::basic_const_iterator` (not ordinary iterator),
    `std::is_corresponding_member`,
    `std::ranges::to` (not `to_array`/`to_chars`/`to_address`),
    `from_range`/`from_range_t` (not `ranges::to` / range-for),
    `forward_like` (not `std::forward(` /
    `std::move`),
    `std::make_exception_ptr` (not `current_exception`/`exception_ptr`
    as the only token), `std::set_terminate`/`std::get_terminate`
    (not `std::terminate(`/`std::unreachable`),
    `is_constant_evaluated` (not `if constexpr` / body `constexpr`),
    `std::lerp`, `std::midpoint`,
    `std::cmp_less`/`cmp_greater`/`in_range`,
    `std::countl_zero`/`countr_zero`/`countl_one`/`countr_one`
    (not `popcount`/`rotl`/`bit_ceil`), `std::unreachable` (not
    `__builtin_unreachable`),     `views::enumerate`/`enumerate_view`
    (not the `enum` keyword), `is_scoped_enum` (not the `enum`
    keyword / `views::enumerate`), `cartesian_product`/
    `cartesian_product_view` (not `views::zip`), `views::chunk`/
    `chunk_by`/`chunk_view` (not English `chunk`), `views::slide`/
    `slide_view`, `views::adjacent`/`adjacent_transform`/
    `adjacent_view` (not `adjtime`), `join_with`/`join_with_view`
    (before `views::join`; `"join" in "join_with"` is TRUE —
    require `join_with`), `views::join`/`join_view` (before
    generic `std::views::`; not `views::zip`),
    `zip_transform`/`zip_transform_view` (before `views::zip`;
    `"zip" in "zip_transform"` is TRUE — require `zip_transform`),
    `views::stride`/`stride_view`, `views::repeat`/`repeat_view`
    (not English `repeat`; require `views::repeat` or `repeat_view`),
    `views::take_while`/`take_while_view` (before `views::take`; not
    `views::take`/`take_view`; `"take" in "take_while"` is TRUE —
    require `take_while`),
    `views::take`/`take_view` (not `take_while` / English take;
    require `views::take` or `take_view`),
    `views::drop_while`/`drop_while_view` (before `views::drop`; not
    `views::drop`),
    `views::drop`/`drop_view` (not `drop_while`),
    `views::keys`/`keys_view` (not `std::map`/`flat_map`),
    `views::values`/`values_view`,
    `views::reverse`/`reverse_view` (not `std::reverse`),
    `views::counted`/`counted_view` (not `counted_iterator`/`std::counted`),
    `views::filter`/`filter_view`,
    `views::transform`/`transform_view` (after `transform_reduce`/
    `transform_inclusive_scan`; not `transform_reduce`;
    require `views::transform` or `transform_view`),
    `views::elements`/`elements_view`, and `views::iota`/`iota_view`
    are missing models.
    C++ `shared_timed_mutex` (not `shared_mutex`/`timed_mutex` as
    the only token), `recursive_timed_mutex` (not `recursive_mutex`),
    `system_error` (not `error_code` as the only token), `current_zone`/
    `tzdb` (not generic `chrono`), `views::zip`/`zip_view` (not
    generic `std::views::`), and `format_to`/`format_to_n` (not
    `std::format`/`print` as the only match) are missing models.
    C++ `std::wstring`/`wstring_view` (not a bare `string_view`),
    `std::multimap<` (not `map`, not `flat_multimap`),
    `std::multiset<` (not `flat_multiset`),
    `std::binary_semaphore` (not `counting_semaphore`),
    `std::error_code`, and `std::byteswap(` are missing models.
    C++ `std::set<` (not `flat_set`/`unordered_set`/`multiset`),
    `std::queue<` (not `deque`/`priority_queue`), `std::stack<`
    (not `stacktrace`), `std::priority_queue`, `std::array<` (not
    `valarray`, not C arrays), and `unordered_set<` (not
    `unordered_map`) are missing models.
    C++ `std::tuple`/`tuple<`, `std::deque`/`deque<`,
    `std::forward_list`/`forward_list<`, `std::list<` (not
    `initializer_list`, not `forward_list`), `std::unordered_map`/
    `unordered_map<`, and `std::map<` (not `flat_map`) are
    missing models.
    C++ `std::weak_ptr`/`weak_ptr<`, `std::exception_ptr`,
    `std::coroutine_handle`, `std::valarray`/`valarray<`,
    `std::to_underlying`/`to_underlying(`, and
    `std::unexpected<`/`unexpected<` (not `unexpected(`) are
    missing models.
    C++ `std::out_ptr`/`out_ptr(`/`inout_ptr(`,
    `std::flat_multimap`/`flat_multimap<`/`flat_multiset<`,
    `std::spanstream`/`ispanstream`/`ospanstream`,
    `std::barrier`/`barrier<`, and
    `std::execution::task`/`std::task<` are missing models.
    C++ `std::osyncstream`/`osyncstream`/`basic_osyncstream`,
    `std::packaged_task`/`packaged_task<`,
    `std::flat_multiset`/`flat_multiset<`,
    `std::syncbuf`/`basic_syncbuf`/`syncbuf`, and
    `std::counted_iterator`/`counted_iterator` are missing models.
    `strcpy`/`strcat`/`sprintf` stay out.
    C++ `std::rcu` /
    `rcu_synchronize` / `rcu_obj<`, `std::linalg` / `linalg::`,
    `#embed`, tight `import` / `export module`, and `std::meta` /
    `^^` are missing models. `strcpy`/`strcat`/`sprintf` stay out.
    C++ `std::to_chars` / `to_chars(`,
    `std::hazard_pointer` / `hazard_pointer`,
    `std::text_encoding` / `text_encoding`, and
    `std::simd` / `std::experimental::simd` / `simd<` are
    missing models. C++ `std::hive` / `hive<`,
    `std::execution::`, `std::indirect` / `indirect<` /
    `std::polymorphic` / `polymorphic<`, and
    `std::bitset<` / `bitset<`, and
    `stringstream` / `ostringstream` / `istringstream` are missing models.
    C++ `std::rcu` / `rcu_obj<` / `rcu_synchronize(`,
    `std::linalg` / `linalg::`, `sync_wait(`, `#embed`,
    `contract_assert(` / `[[pre:` / `[[post:`, and
    `^^` / `std::meta::` / `define_aggregate` / `define_class`
    are missing models. `strcpy`/`strcat`/`sprintf` stay out.
    C++ `catch (...)` is a missing exception model (tight; other)
    `try`/`catch` still use the generic try/catch gate).
    C++ `throw new` is a missing exception/heap model; `throw E()`
    is not that case and still uses the throw gate.
    `restrict` in a SCALAR/VOID body is a missing qualifier model;
    POINTER parameters are already NEEDS-HARNESS. `_Float16` /
    `_Float32` / `_Float64` / `__fp16` are a missing extra-IEEE model;
    ordinary `int` is not, and IEEE `float`/`double` stay on their
    own gate.     `std::start_lifetime_as` / `start_lifetime_as_array`
    are a missing lifetime model. C++ `requires (` / `concept ` in a
    body or function head are a missing concepts model; ordinary
    functions still encode. GNU `__builtin_choose_expr` is missing.
    `typeof_unqual` / `__typeof_unqual__` are a missing typeof model;
    `sizeof` still encodes. `shared_from_this` /
    `enable_shared_from_this` are a missing shared-lifetime model.
    `strcpy`/`strcat`/`sprintf` stay out.
    """
    body = fn.body or ""
    if _re_search(r"\bgoto\s+\*", body):
        return (
            f"computed goto unencoded: {engine} is not a computed-goto model"
        )
    if _re_search(r"(?:^|[;{(]|=\s*)\&\&[A-Za-z_]\w*", body, re.M):
        return (
            f"label-address unencoded: {engine} is not a label-address model"
        )
    if _re_search(r"\b(?:_Thread_local|thread_local)\b", body):
        return (
            f"thread-local unencoded: {engine} is not a TLS model"
        )
    if _re_search(r"\b(?:_Complex|_Imaginary)\b", body):
        return (
            f"complex unencoded: {engine} is not a complex arithmetic model"
        )
    if _re_search(r"\b(?:typeof_unqual|__typeof_unqual__)\s*\(", body):
        return f"typeof_unqual unencoded: {engine} is not a typeof model"
    if _re_search(r"\b(?:typeof|__typeof__)\s*\(", body):
        return f"typeof unencoded: {engine} is not a typeof model"
    if _re_search(
        r"(?m)(?:^|[;{])\s*(?:void|int|unsigned(?:\s+int)?|long(?:\s+int)?|"
        r"short|char|float|double|_Bool|bool)\s+[A-Za-z_]\w*\s*\([^)]*\)\s*\{",
        body,
    ):
        return (
            f"nested function unencoded: {engine} is not a "
            "nested-function model"
        )
    if _re_search(r"\b(?:_Alignof|alignof)\s*\(", body):
        return f"alignof unencoded: {engine} is not an alignment model"
    if _re_search(r"\bva_arg\s*\(", body):
        return f"va_arg unencoded: {engine} is not a variadic model"
    if _re_search(r"\{\s*(?:\[[^\]]+\]|\.[A-Za-z_]\w*)\s*=", body):
        return (
            f"designated init unencoded: {engine} is not a "
            "designated-init model"
        )
    if _re_search(r"\bfor\s*\([^)]*:[^)]*\)", body):
        return (
            f"C++ range-for unencoded: {engine} is not a range-for model"
        )
    if _re_search(r"\[\s*[^\]]*\]\s*(?:\([^)]*\))?\s*\{", body):
        return f"C++ lambda unencoded: {engine} is not a lambda model"
    if _re_search(r"\bconst_cast\s*<", body):
        return (
            f"C++ const_cast unencoded: {engine} is not a cv-qualifier model"
        )
    if _re_search(r"\bdynamic_cast\s*<", body):
        return (
            f"C++ dynamic_cast unencoded: {engine} is not an RTTI model"
        )
    if _re_search(r"\btype_identity\b", body):
        return (
            f"C++ type_identity unencoded: {engine} is not a "
            "type_identity model"
        )
    if _re_search(r"\btypeid\s*\(", body):
        return f"C++ typeid unencoded: {engine} is not an RTTI model"
    if _re_search(r"\breinterpret_cast\s*<", body):
        return (
            f"C++ reinterpret_cast unencoded: {engine} is not a type-pun model"
        )
    if _re_search(r"\bstd::bit_cast\b|bit_cast\s*<", body):
        return f"bit_cast unencoded: {engine} is not a type-pun model"
    if _re_search(
        r"(?m)\bstd\s*::\s*jthread\b"
        r"|(?:^|[;{])\s*jthread\s+[A-Za-z_]\w*\s*\(",
        body,
    ):
        return (
            f"C++ std::jthread unencoded: {engine} is not a jthread model"
        )
    if _re_search(
        r"(?m)\bstd\s*::\s*thread\b"
        r"|(?:^|[;{])\s*thread\s+[A-Za-z_]\w*\s*\(",
        body,
    ):
        return (
            f"C++ std::thread unencoded: {engine} is not a "
            "thread-lifetime model"
        )
    if _re_search(
        r"\bstd\s*::\s*(?:async|future|promise)\b"
        r"|(?:future|promise)\s*<",
        body,
    ):
        return (
            f"C++ std::async unencoded: {engine} is not a future model"
        )
    if _re_search(r"\b(?:std\s*::\s*)?packaged_task\b|\bpackaged_task\s*<", body):
        return (
            f"C++ packaged_task unencoded: {engine} is not a "
            "packaged_task model"
        )
    if _re_search(r"\b(?:std\s*::\s*)?counted_iterator\b", body):
        return (
            f"C++ counted_iterator unencoded: {engine} is not a "
            "counted_iterator model"
        )
    if _re_search(r"\b(?:std\s*::\s*)?weak_ptr\s*<", body):
        return (
            f"C++ weak_ptr unencoded: {engine} is not a weak_ptr model"
        )
    if _re_search(r"\b(?:std\s*::\s*)?nested_exception\b", body):
        return (
            f"C++ nested_exception unencoded: {engine} is not a "
            "nested_exception model"
        )
    if _re_search(
        r"\b(?:throw_with_nested|rethrow_if_nested)\s*\(",
        body,
    ):
        return (
            f"throw_with_nested unencoded: unconstrained "
            f"throw_with_nested is not a proof ({engine})"
        )
    if _re_search(r"\buncaught_exceptions\s*\(", body):
        return (
            f"C++ uncaught_exceptions unencoded: {engine} is not an "
            "uncaught_exceptions model"
        )
    if _re_search(r"\bcurrent_exception\s*\(", body):
        return (
            f"C++ current_exception unencoded: {engine} is not a "
            "current_exception model"
        )
    if _re_search(r"\bmake_exception_ptr\s*\(", body):
        return (
            f"C++ make_exception_ptr unencoded: {engine} is not a "
            "make_exception_ptr model"
        )
    if _re_search(r"\b(?:std\s*::\s*)?exception_ptr\b", body):
        return (
            f"C++ exception_ptr unencoded: {engine} is not an "
            "exception_ptr model"
        )
    if _re_search(r"\b(?:std\s*::\s*)?coroutine_handle\b", body):
        return (
            f"C++ coroutine_handle unencoded: {engine} is not a "
            "coroutine_handle model"
        )
    if _re_search(r"\b(?:std\s*::\s*)?valarray\s*<", body):
        return (
            f"C++ valarray unencoded: {engine} is not a valarray model"
        )
    if _re_search(r"\b(?:std\s*::\s*)?to_underlying\s*\(", body):
        return (
            f"C++ to_underlying unencoded: {engine} is not a "
            "to_underlying model"
        )
    if _re_search(r"\b(?:std\s*::\s*)?unexpected\s*<", body):
        return (
            f"C++ unexpected unencoded: {engine} is not an unexpected model"
        )
    if _re_search(r"\b(?:std\s*::\s*)?function_ref\s*<", body):
        return (
            f"C++ function_ref unencoded: {engine} is not a "
            "function_ref model"
        )
    if _re_search(
        r"\bstd\s*::\s*(?:move_only_function|copyable_function)\b"
        r"|\b(?:move_only_function|copyable_function)\s*<",
        body,
    ):
        return (
            f"C++ move_only_function unencoded: {engine} is not a "
            "move_only_function model"
        )
    if _re_search(r"\breference_wrapper\b|\bstd\s*::\s*(?:cref|ref)\s*\(", body):
        return (
            f"C++ reference_wrapper unencoded: {engine} is not a "
            "reference_wrapper model"
        )
    if _re_search(r"\bstd\s*::\s*function\b|function\s*<", body):
        return (
            f"C++ std::function unencoded: {engine} is not a "
            "type-erased callable model"
        )
    if _re_search(r"\bstd\s*::\s*mdspan\b|mdspan\s*<", body):
        return (
            f"C++ std::mdspan unencoded: {engine} is not an mdspan model"
        )
    if _re_search(
        r"\bstd\s*::\s*(?:mutex|lock_guard|unique_lock|scoped_lock)\b"
        r"|(?:lock_guard|unique_lock|scoped_lock)\s*<",
        body,
    ):
        return (
            f"C++ std::mutex unencoded: {engine} is not a C++ mutex model"
        )
    if _re_search(r"\bnotify_all_at_thread_exit\s*\(", body):
        return (
            f"C++ notify_all_at_thread_exit unencoded: {engine} is not a "
            "notify_all_at_thread_exit model"
        )
    if _re_search(r"\bcondition_variable_any\b", body):
        return (
            f"C++ condition_variable_any unencoded: {engine} is not a "
            "condition_variable_any model"
        )
    if _re_search(r"\bshared_timed_mutex\b", body):
        return (
            f"C++ shared_timed_mutex unencoded: {engine} is not a "
            "shared_timed_mutex model"
        )
    if _re_search(
        r"\bstd\s*::\s*(?:condition_variable|shared_mutex)\b"
        r"|\b(?:condition_variable|shared_mutex)\b",
        body,
    ):
        return (
            f"C++ std::condition_variable unencoded: {engine} is not a "
            "condvar/shared-mutex model"
        )
    if _re_search(r"\bstd\s*::\s*atomic_ref\b|atomic_ref\s*<", body):
        return (
            f"C++ std::atomic_ref unencoded: {engine} is not an "
            "atomic_ref model"
        )
    if _re_search(r"\bstd\s*::\s*generator\b|generator\s*<", body):
        return (
            f"C++ std::generator unencoded: {engine} is not a generator model"
        )
    if _re_search(r"\[\[\s*assume\s*\(", body):
        return (
            f"C++ assume unencoded: {engine} is not an assume-attribute model"
        )
    if _re_search(r"\bstd\s*::\s*bind\s*\(", body):
        return (
            f"C++ std::bind unencoded: {engine} is not a bind model"
        )
    if _re_search(r"\bstd\s*::\s*any\b|\bany_cast\s*<|\bany_cast\b", body):
        return (
            f"C++ std::any unencoded: {engine} is not an any model"
        )
    if _re_search(
        r"\bstd\s*::\s*filesystem\b|\bstd\s*::\s*fs\s*::|\bfilesystem\s*::",
        body,
    ):
        return (
            f"C++ std::filesystem unencoded: {engine} is not a "
            "filesystem model"
        )
    if _re_search(
        r"\bstd\s*::\s*regex\b|\bstd\s*::\s*regex_"
        r"|\bregex\s+[A-Za-z_]\w*\s*[\({]",
        body,
    ):
        return (
            f"C++ std::regex unencoded: {engine} is not a regex model"
        )
    if _re_search(
        r"\bstd\s*::\s*(?:latch|barrier|counting_semaphore)\b"
        r"|\blatch\s+[A-Za-z_]\w*\s*\("
        r"|\bbarrier\s*<",
        body,
    ):
        return (
            f"C++ std::latch/barrier unencoded: {engine} is not a "
            "sync primitive model"
        )
    if _re_search(r"\bstd\s*::\s*from_chars\b|\bfrom_chars\s*\(", body):
        return (
            f"C++ from_chars unencoded: {engine} is not a charconv model"
        )
    if _re_search(r"\b(?:std\s*::\s*)?to_chars\s*\(", body):
        return (
            f"C++ to_chars unencoded: {engine} is not a charconv model"
        )
    if _re_search(r"\b(?:std\s*::\s*)?hazard_pointer\b", body):
        return (
            f"C++ hazard_pointer unencoded: {engine} is not a "
            "hazard_pointer model"
        )
    if _re_search(r"\b(?:std\s*::\s*)?text_encoding\b", body):
        return (
            f"C++ text_encoding unencoded: {engine} is not a "
            "text_encoding model"
        )
    if _re_search(
        r"\b(?:std\s*::\s*(?:experimental\s*::\s*)?)?simd\s*<",
        body,
    ):
        return (
            f"C++ simd unencoded: {engine} is not a simd model"
        )
    if _re_search(r"\bstd\s*::\s*visit\b", body):
        return (
            f"C++ std::visit unencoded: {engine} is not a visitor model"
        )
    if _re_search(r"\b(?:std\s*::\s*)?source_location\b", body):
        return (
            f"C++ source_location unencoded: {engine} is not a "
            "source_location model"
        )
    if _re_search(r"\b(?:std\s*::\s*)?stacktrace\b", body):
        return (
            f"C++ stacktrace unencoded: {engine} is not a stacktrace model"
        )
    if _re_search(r"\bstd\s*::\s*stop_(?:token|source|callback)\b", body):
        return (
            f"C++ stop_token unencoded: {engine} is not a stop_token model"
        )
    if _re_search(r"\bstd\s*::\s*flat_map\b|\bflat_map\s*<", body):
        return (
            f"C++ flat_map unencoded: {engine} is not a flat_map model"
        )
    if _re_search(r"\bstd\s*::\s*flat_set\b|\bflat_set\s*<", body):
        return (
            f"C++ flat_set unencoded: {engine} is not a flat_set model"
        )
    if _re_search(r"\bstd\s*::\s*flat_multiset\b|\bflat_multiset\s*<", body):
        return (
            f"C++ flat_multiset unencoded: {engine} is not a "
            "flat_multiset model"
        )
    if _re_search(r"\bstd\s*::\s*flat_multimap\b|\bflat_multimap\s*<", body):
        return (
            f"C++ flat_multimap unencoded: {engine} is not a "
            "flat_multimap model"
        )
    if _re_search(r"\bzoned_time\b", body):
        return (
            f"C++ zoned_time unencoded: {engine} is not a zoned_time model"
        )
    if _re_search(r"\b(?:current_zone|tzdb)\b", body):
        return (
            f"C++ tzdb unencoded: {engine} is not a tzdb model"
        )
    if _re_search(r"\bstd\s*::\s*chrono\b|\bchrono\s*::", body):
        return (
            f"C++ chrono unencoded: {engine} is not a chrono model"
        )
    if _re_search(r"\bzip_transform(?:_view)?\b", body):
        return (
            f"C++ zip_transform unencoded: {engine} is not a "
            "zip_transform model"
        )
    if _re_search(r"views\s*::\s*zip|\bzip_view\b", body):
        return (
            f"C++ views::zip unencoded: {engine} is not a views::zip model"
        )
    if _re_search(r"\bas_rvalue(?:_view)?\b", body):
        return (
            f"C++ as_rvalue unencoded: {engine} is not an as_rvalue model"
        )
    if _re_search(r"\bis_scoped_enum\b", body):
        return (
            f"C++ is_scoped_enum unencoded: {engine} is not an "
            "is_scoped_enum model"
        )
    if _re_search(r"\b(?:views\s*::\s*)?enumerate(?:_view)?\b", body):
        return (
            f"C++ views::enumerate unencoded: {engine} is not a "
            "views::enumerate model"
        )
    if _re_search(r"\bcartesian_product(?:_view)?\b", body):
        return (
            f"C++ cartesian_product unencoded: {engine} is not a "
            "cartesian_product model"
        )
    if _re_search(
        r"\b(?:views\s*::\s*chunk(?:_by)?|chunk(?:_by|_view))\b",
        body,
    ):
        return (
            f"C++ views::chunk unencoded: {engine} is not a "
            "views::chunk model"
        )
    if _re_search(r"\b(?:views\s*::\s*slide|slide_view)\b", body):
        return (
            f"C++ views::slide unencoded: {engine} is not a "
            "views::slide model"
        )
    if _re_search(
        r"\b(?:views\s*::\s*adjacent(?:_transform)?|"
        r"adjacent(?:_transform|_view))\b",
        body,
    ):
        return (
            f"C++ views::adjacent unencoded: {engine} is not a "
            "views::adjacent model"
        )
    if _re_search(r"\bjoin_with(?:_view)?\b", body):
        return (
            f"C++ join_with unencoded: {engine} is not a join_with model"
        )
    if _re_search(r"views\s*::\s*join|\bjoin_view\b", body):
        return (
            f"C++ views::join unencoded: {engine} is not a views::join model"
        )
    if _re_search(r"\b(?:views\s*::\s*)?stride(?:_view)?\b", body):
        return (
            f"C++ views::stride unencoded: {engine} is not a "
            "views::stride model"
        )
    if _re_search(r"\b(?:views\s*::\s*repeat|repeat_view)\b", body):
        return (
            f"C++ views::repeat unencoded: {engine} is not a "
            "views::repeat model"
        )
    if _re_search(r"\btake_while(?:_view)?\b", body):
        return (
            f"C++ views::take_while unencoded: {engine} is not a "
            "take_while model"
        )
    if _re_search(r"\b(?:views\s*::\s*take\b|\btake_view\b)", body):
        return (
            f"C++ views::take unencoded: {engine} is not a "
            "views::take model"
        )
    if _re_search(r"\bdrop_while(?:_view)?\b", body):
        return (
            f"C++ views::drop_while unencoded: {engine} is not a "
            "drop_while model"
        )
    if _re_search(r"\b(?:views\s*::\s*drop\b|\bdrop_view\b)", body):
        return (
            f"C++ views::drop unencoded: {engine} is not a "
            "views::drop model"
        )
    if _re_search(r"\b(?:views\s*::\s*keys\b|\bkeys_view\b)", body):
        return (
            f"C++ views::keys unencoded: {engine} is not a "
            "keys model"
        )
    if _re_search(r"\b(?:views\s*::\s*values\b|\bvalues_view\b)", body):
        return (
            f"C++ views::values unencoded: {engine} is not a "
            "values model"
        )
    if _re_search(r"\b(?:views\s*::\s*reverse\b|\breverse_view\b)", body):
        return (
            f"C++ views::reverse unencoded: {engine} is not a "
            "reverse_view model"
        )
    if _re_search(r"\b(?:views\s*::\s*counted\b|\bcounted_view\b)", body):
        return (
            f"C++ views::counted unencoded: {engine} is not a "
            "counted_view model"
        )
    if _re_search(r"\b(?:views\s*::\s*filter\b|\bfilter_view\b)", body):
        return (
            f"C++ views::filter unencoded: {engine} is not a "
            "views::filter model"
        )
    if _re_search(r"\b(?:views\s*::\s*transform\b|\btransform_view\b)", body):
        return (
            f"C++ views::transform unencoded: {engine} is not a "
            "transform_view model"
        )
    if _re_search(r"\b(?:views\s*::\s*elements\b|\belements_view\b)", body):
        return (
            f"C++ views::elements unencoded: {engine} is not an "
            "elements model"
        )
    if _re_search(r"\b(?:views\s*::\s*iota\b|\biota_view\b)", body):
        return (
            f"C++ views::iota unencoded: {engine} is not an "
            "iota model"
        )
    if _re_search(r"\bfrom_range\b", body):
        return (
            f"C++ from_range unencoded: {engine} is not a from_range model"
        )
    if _re_search(
        r"\bstd\s*::\s*ranges\s*::\s*views\b|\bstd\s*::\s*views\s*::",
        body,
    ):
        return (
            f"C++ ranges views unencoded: {engine} is not a "
            "ranges-views model"
        )
    if _re_search(r"\bstd\s*::\s*hive\b|\bhive\s*<", body):
        return (
            f"C++ hive unencoded: {engine} is not a hive model"
        )
    if _re_search(r"\bstd\s*::\s*(?:execution\s*::\s*)?task\s*<", body):
        return (
            f"C++ std::task unencoded: {engine} is not a task model"
        )
    if _re_search(r"\bstd\s*::\s*execution\s*::", body):
        return (
            f"C++ execution unencoded: {engine} is not an execution model"
        )
    if _re_search(
        r"\bstd\s*::\s*indirect\b|\bindirect\s*<"
        r"|\bstd\s*::\s*polymorphic\b|\bpolymorphic\s*<",
        body,
    ):
        return (
            f"C++ indirect unencoded: {engine} is not an "
            "indirect/polymorphic model"
        )
    if _re_search(r"\bstd\s*::\s*bitset\s*<|\bbitset\s*<", body):
        return (
            f"C++ bitset unencoded: {engine} is not a bitset model"
        )
    if _re_search(
        r"\b(?:std\s*::\s*)?(?:basic_)?(?:string|ostring|istring)stream\b",
        body,
    ):
        return (
            f"C++ stringstream unencoded: {engine} is not a "
            "stringstream model"
        )
    if _re_search(r"\b(?:std\s*::\s*)?(?:basic_)?osyncstream\b", body):
        return (
            f"C++ osyncstream unencoded: {engine} is not an "
            "osyncstream model"
        )
    if _re_search(r"\b(?:std\s*::\s*)?(?:basic_)?syncbuf\b", body):
        return (
            f"C++ syncbuf unencoded: {engine} is not a syncbuf model"
        )
    if _re_search(
        r"\b(?:std\s*::\s*)?(?:basic_)?(?:i|o)?spanstream\b",
        body,
    ):
        return (
            f"C++ spanstream unencoded: {engine} is not a "
            "spanstream model"
        )
    if _re_search(
        r"\b(?:std\s*::\s*)?(?:inout_ptr|out_ptr)\s*(?:<|\()",
        body,
    ):
        return (
            f"C++ out_ptr unencoded: {engine} is not an out_ptr model"
        )
    if _re_search(
        r"\bstd\s*::\s*rcu\b|\brcu_synchronize\s*\(|\brcu_obj\s*<",
        body,
    ):
        return (
            f"C++ rcu unencoded: {engine} is not an rcu model"
        )
    if _re_search(r"\bstd\s*::\s*linalg\b|\blinalg\s*::", body):
        return (
            f"C++ linalg unencoded: {engine} is not a linalg model"
        )
    if _re_search(r"\bsync_wait\s*\(", body):
        return (
            f"C++ sync_wait unencoded: {engine} is not an execution model"
        )
    if _re_search(r"\bcontract_assert\s*\(|\[\[\s*(?:pre|post)\s*:", body):
        return (
            f"C++ contracts unencoded: {engine} is not a contracts model"
        )
    if _re_search(
        r"\bstd\s*::\s*meta\b|\^\^"
        r"|\bdefine_aggregate\s*\(|\bdefine_class\s*\(",
        body,
    ):
        return (
            f"C++ reflection unencoded: {engine} is not a reflection model"
        )
    if _re_search(r"\b(?:std\s*::\s*)?initializer_list\s*<", body):
        return (
            f"C++ initializer_list unencoded: {engine} is not a "
            "temporary-lifetime model"
        )
    if _re_search(r"\b(?:std\s*::\s*)?optional\s*<", body):
        return (
            f"C++ std::optional unencoded: {engine} is not an optional model"
        )
    if _re_search(r"\bstd\s*::\s*expected\b|\bexpected\s*<", body):
        return (
            f"C++ std::expected unencoded: {engine} is not an expected model"
        )
    if _re_search(r"\bformat_to(?:_n)?\s*\(", body):
        return (
            f"C++ format_to unencoded: {engine} is not a format_to model"
        )
    if _re_search(r"\bstd\s*::\s*(?:format|print|println)\b", body):
        return (
            f"C++ std::format unencoded: {engine} is not a format model"
        )
    if _re_search(r"<=>", body):
        return (
            f"C++ spaceship unencoded: {engine} is not a "
            "three-way comparison model"
        )
    if _re_search(r"\b(?:std\s*::\s*)?variant\s*<", body):
        return (
            f"C++ std::variant unencoded: {engine} is not a variant model"
        )
    if _re_search(r"\b(?:std\s*::\s*)?span\s*<", body):
        return (
            f"C++ std::span unencoded: {engine} is not a span-lifetime model"
        )
    if _re_search(r"\bstd\s*::\s*inplace_vector\b|\binplace_vector\s*<", body):
        return (
            f"C++ inplace_vector unencoded: {engine} is not an "
            "inplace_vector model"
        )
    if _re_search(r"\b(?:std\s*::\s*)?vector\s*<", body):
        return (
            f"C++ std::vector unencoded: {engine} is not a container model"
        )
    if _re_search(r"\bcatch\s*\(\s*\.\.\.\s*\)", body):
        return (
            f"C++ catch-all unencoded: {engine} is not an exception model"
        )
    if _re_search(r"\bthrow\s+new\b", body):
        return (
            f"C++ throw-new unencoded: {engine} is not an exception model"
        )
    if _re_search(r"\bstd\s*::\s*launder\b|\blaunder\s*\(", body):
        return (
            f"C++ launder unencoded: {engine} is not a lifetime model"
        )
    if _re_search(
        r"\b(?:std\s*::\s*)?start_lifetime_as(?:_array)?\b",
        body,
    ):
        return (
            f"C++ start_lifetime_as unencoded: {engine} is not a lifetime model"
        )
    if _re_search(
        r"\b(?:enable_shared_from_this|shared_from_this)\b",
        body,
    ):
        return (
            f"C++ shared_from_this unencoded: {engine} is not a "
            "shared-lifetime model"
        )
    if _re_search(r"\bif\s+constexpr\b", body):
        return (
            f"if constexpr unencoded: {engine} is not a compile-time-if model"
        )
    if _re_search(r"\bconstexpr\b", body):
        return (
            f"constexpr unencoded: {engine} is not a constexpr model"
        )
    blob = f"{fn.signature or ''}\n{body}"
    if _re_search(r"\brequires\s*\(", blob) or _re_search(r"\bconcept\s+", blob):
        return (
            f"C++ concepts unencoded: {engine} is not a concepts model"
        )
    if _re_search(r"\bcase\s+[^:'\"]+?\s*\.\.\.\s*[^:'\"]+?:", body):
        return (
            f"case-range unencoded: {engine} is not a case-range model"
        )
    if _re_search(r"\(\s*\.\.\.\s*[+\-|&^]|[+\-|&^]\s*\.\.\.\s*\)", body):
        return (
            f"C++ fold unencoded: {engine} is not a fold-expression model"
        )
    if _re_search(r"\b(?:__int128(?:_t)?|_BitInt)\b", body):
        return f"128-bit unencoded: {engine} is not a 128-bit model"
    if _re_search(r"\b(?:_Decimal32|_Decimal64|_Decimal128)\b", body):
        return (
            f"decimal float unencoded: {engine} is not a decimal-float model"
        )
    if _re_search(r"\b(?:_Float16|_Float32|_Float64|__fp16)\b", body):
        return (
            f"extra-IEEE float unencoded: {engine} is not an extra-IEEE model"
        )
    code = re.sub(r"/\*.*?\*/", " ", body, flags=re.S)
    code = re.sub(r"//.*?$", " ", code, flags=re.M)
    if _re_search(r"\bnullptr\b", code):
        return (
            f"C23 nullptr unencoded: {engine} is not a nullptr model"
        )
    if _re_search(r"\bco_(?:await|yield|return)\b", body):
        return (
            f"C++ coroutine unencoded: {engine} is not a coroutine model"
        )
    if _re_search(
        r"__attribute__\s*\(\s*\(\s*packed\s*\)\s*\)|\b__packed\b",
        body,
    ):
        return (
            f"packed layout unencoded: {engine} is not a packed-layout model"
        )
    pack_src = body
    path = fn.file or ""
    if path:
        try:
            pth = Path(path)
            if pth.is_file() and pth.stat().st_size < 1_000_000:
                pack_src = (
                    pth.read_text(encoding="utf-8", errors="replace")
                    + "\n"
                    + body
                )
        except OSError:
            pack_src = body
    pack_code = re.sub(r"/\*.*?\*/", " ", pack_src, flags=re.S)
    pack_code = re.sub(r"//.*?$", " ", pack_code, flags=re.M)
    if _re_search(r"\bpragma\s+pack\b", pack_code):
        return (
            f"pragma pack unencoded: {engine} is not a packed-layout model"
        )
    import_code = re.sub(r"/\*.*?\*/", " ", body, flags=re.S)
    import_code = re.sub(r"//.*?$", " ", import_code, flags=re.M)
    if _re_search(r"\bimport\s+[A-Za-z_]", import_code) or _re_search(
        r"\bexport\s+module\b",
        import_code,
    ):
        return (
            f"C++ module import unencoded: {engine} is not a modules model"
        )
    if _re_search(r"\#\s*embed\b", body):
        return (
            f"C++ embed unencoded: {engine} is not an embed model"
        )
    if _re_search(
        r"\bstd\s*::\s*rcu\b|\brcu_synchronize\s*\(|\brcu_obj\s*<",
        body,
    ):
        return (
            f"C++ rcu unencoded: {engine} is not an rcu model"
        )
    if _re_search(r"\bstd\s*::\s*linalg\b|\blinalg\s*::", body):
        return (
            f"C++ linalg unencoded: {engine} is not a linalg model"
        )
    if _re_search(r"\bstd\s*::\s*meta\b|\^\^", body):
        return (
            f"C++ reflection unencoded: {engine} is not a reflection model"
        )
    if _re_search(r"__attribute__\s*\(\s*\(\s*cleanup", body):
        return (
            f"cleanup attribute unencoded: {engine} is not a cleanup model"
        )
    if _re_search(
        r"__attribute__\s*\(\s*\(\s*(?:__)?vector_size|\b__vector_size\b",
        body,
    ):
        return (
            f"vector_size unencoded: {engine} is not a SIMD vector model"
        )
    if _re_search(r"\bL'(?:\\.|[^\\'])'", body):
        return (
            f"wide character unencoded: {engine} is not a wide-char model"
        )
    if _re_search(r'\bL"(?:\\.|[^\\"])*"', body):
        return (
            f"wide character unencoded: {engine} is not a wide-char model"
        )
    if _re_search(r"\bexecveat\s*\(", body):
        return (
            f"execveat unencoded: unconstrained execveat is not a "
            f"proof ({engine})"
        )
    if _re_search(r"\bpthread_atfork\s*\(", body):
        return (
            f"pthread_atfork unencoded: unconstrained pthread_atfork "
            f"is not a proof ({engine})"
        )
    if _re_search(r"\bpledge\s*\(", body):
        return (
            f"pledge unencoded: unconstrained pledge is not a "
            f"proof ({engine})"
        )
    if _re_search(r"\bmac_(?:set|get)_(?:proc|fd|file)\s*\(", body):
        return (
            f"mac_set_proc unencoded: unconstrained mac_set_proc is not a "
            f"proof ({engine})"
        )
    if _re_search(r"\bcap_getmode\s*\(", body):
        return (
            f"cap_getmode unencoded: unconstrained cap_getmode is not a "
            f"proof ({engine})"
        )
    if _re_search(r"\bcap_getrights\s*\(", body):
        return (
            f"cap_getrights unencoded: unconstrained cap_getrights is not a "
            f"proof ({engine})"
        )
    if _re_search(r"\bcap_enter\s*\(", body):
        return (
            f"cap_enter unencoded: unconstrained cap_enter is not a "
            f"proof ({engine})"
        )
    if _re_search(r"\bcap_sandboxed\s*\(", body):
        return (
            f"cap_sandboxed unencoded: unconstrained cap_sandboxed is not a "
            f"proof ({engine})"
        )
    if _re_search(r"\bcap_rights_(?:limit|get)\s*\(", body):
        return (
            f"cap_rights unencoded: unconstrained cap_rights_limit "
            f"is not a proof ({engine})"
        )
    if _re_search(r"\bcap_(?:fcntls|ioctls)_limit\s*\(", body):
        return (
            f"cap_fcntls unencoded: unconstrained cap_fcntls_limit "
            f"is not a proof ({engine})"
        )
    if _re_search(r"\bunveil\s*\(", body):
        return (
            f"unveil unencoded: unconstrained unveil is not a "
            f"proof ({engine})"
        )
    if _re_search(r"\bsysctl(?:byname)?\s*\(", body):
        return (
            f"sysctl unencoded: unconstrained sysctl is not a "
            f"proof ({engine})"
        )
    if _re_search(r"\bkqueue\s*\(", body):
        return (
            f"kqueue unencoded: unconstrained kqueue is not a "
            f"proof ({engine})"
        )
    if _re_search(r"\bkevent\s*\(", body):
        return (
            f"kevent unencoded: unconstrained kevent is not a "
            f"proof ({engine})"
        )
    if _re_search(r"\bpause\s*\(", body):
        return (
            f"pause unencoded: unconstrained pause is not a "
            f"proof ({engine})"
        )
    if _re_search(r"\b(?:get|set|swap|make)context\s*\(", body):
        return (
            f"getcontext unencoded: unconstrained getcontext is not a "
            f"proof ({engine})"
        )
    if _re_search(
        r"\bpthread_attr_(?:init|destroy|setstacksize|setstack|"
        r"setdetachstate|getstacksize|getstack|getdetachstate)\s*\(",
        body,
    ):
        return (
            f"pthread_attr unencoded: unconstrained pthread_attr_init "
            f"is not a proof ({engine})"
        )
    if _re_search(
        r"\b(?:fork|vfork|execlp|execle|execl|"
        r"execvpe|execvp|execve|execv)\s*\(",
        body,
    ):
        return (
            f"process spawn unencoded: unconstrained fork/exec is not a "
            f"proof ({engine})"
        )
    if _re_search(r"\bpdfork\s*\(", body):
        return (
            f"pdfork unencoded: unconstrained pdfork is not a "
            f"proof ({engine})"
        )
    if _re_search(r"\brfork\s*\(", body):
        return (
            f"rfork unencoded: unconstrained rfork is not a "
            f"proof ({engine})"
        )
    if _re_search(r"\bminherit\s*\(", body):
        return (
            f"minherit unencoded: unconstrained minherit is not a "
            f"proof ({engine})"
        )
    if _re_search(r"\bnfssvc\s*\(", body):
        return (
            f"nfssvc unencoded: unconstrained nfssvc is not a "
            f"proof ({engine})"
        )
    if _re_search(r"\bsysarch\s*\(", body):
        return (
            f"sysarch unencoded: unconstrained sysarch is not a "
            f"proof ({engine})"
        )
    if _re_search(r"\bgetbootfile\s*\(", body):
        return (
            f"getbootfile unencoded: unconstrained getbootfile is not a "
            f"proof ({engine})"
        )
    if _re_search(r"\bdevname(?:_r)?\s*\(", body):
        return (
            f"devname unencoded: unconstrained devname is not a "
            f"proof ({engine})"
        )
    if _re_search(r"\bsbrk\s*\(", body):
        return (
            f"sbrk unencoded: unconstrained sbrk is not a "
            f"proof ({engine})"
        )
    if _re_search(r"\bbrk\s*\(", body):
        return (
            f"brk unencoded: unconstrained brk is not a "
            f"proof ({engine})"
        )
    if _re_search(r"\b(?:mmap|munmap|mprotect)\s*\(", body):
        return f"mmap unencoded: {engine} is not a VM model"
    if _re_search(r"\bioctl\s*\(", body):
        return (
            f"ioctl unencoded: unconstrained ioctl is not a "
            f"proof ({engine})"
        )
    if _re_search(r"\bmod(?:find|stat|next|fnext)\s*\(", body):
        return (
            f"modfind unencoded: unconstrained modfind is not a "
            f"proof ({engine})"
        )
    if _re_search(r"\bkld(?:firstmod|nextmod)\s*\(", body):
        return (
            f"kldfirstmod unencoded: unconstrained kldfirstmod is not a "
            f"proof ({engine})"
        )
    if _re_search(r"\bkld_(?:isloaded|load)\s*\(", body):
        return (
            f"kld_load unencoded: unconstrained kld_load is not a "
            f"proof ({engine})"
        )
    if _re_search(r"\bkld(?:load|unload|find|sym|stat)\s*\(", body):
        return (
            f"kldload unencoded: unconstrained kldload is not a "
            f"proof ({engine})"
        )
    if _re_search(r"\b(?:dlfunc|dlvsym)\s*\(", body):
        return (
            f"dlfunc unencoded: unconstrained dlfunc is not a "
            f"proof ({engine})"
        )
    if _re_search(r"\b(?:dlopen|dlsym|dlclose)\s*\(", body):
        return (
            f"dlopen unencoded: {engine} is not a dynamic-loader model"
        )
    if _re_search(
        r"\b(?:__builtin_clzll|__builtin_ctzll|"
        r"__builtin_clz|__builtin_ctz)\s*\(",
        body,
    ):
        return (
            f"clz unencoded: unconstrained __builtin_clz is UB on 0 "
            f"and is not a proof ({engine})"
        )
    if _re_search(r"\b__builtin_choose_expr\s*\(", body):
        return (
            f"choose_expr unencoded: {engine} is not a "
            "__builtin_choose_expr model"
        )
    if _re_search(r"\bstd\s*::\s*unreachable\s*\(", body):
        return (
            f"C++ std::unreachable unencoded: {engine} is not an "
            "unreachable model"
        )
    if _re_search(r"\b(?:accept4|accept)\s*\(", body):
        return (
            f"accept unencoded: unconstrained accept is not a "
            f"proof ({engine})"
        )
    if _re_search(r"\bfchmodat2\s*\(", body):
        return (
            f"fchmodat2 unencoded: unconstrained fchmodat2 is not a "
            f"proof ({engine})"
        )
    if _re_search(r"\bfchmodat\s*\(", body):
        return (
            f"fchmodat unencoded: unconstrained fchmodat is not a "
            f"proof ({engine})"
        )
    if _re_search(r"\b(?:fchmod|chmod)\s*\(", body):
        return (
            f"chmod unencoded: unconstrained chmod is not a "
            f"proof ({engine})"
        )
    if _re_search(r"\b(?:f|l)?chflags\s*\(", body):
        return (
            f"chflags unencoded: unconstrained chflags is not a "
            f"proof ({engine})"
        )
    if _re_search(
        r"\b(?:setreuid|setregid|setresuid|setresgid)\s*\(",
        body,
    ):
        return (
            f"setreuid unencoded: unconstrained setreuid is not a "
            f"proof ({engine})"
        )
    if _re_search(r"\bgetres(?:uid|gid)\s*\(", body):
        return (
            f"getresuid unencoded: unconstrained getresuid is not a "
            f"proof ({engine})"
        )
    if _re_search(r"\b(?:setfsuid|setfsgid)\s*\(", body):
        return (
            f"setfsuid unencoded: unconstrained setfsuid is not a "
            f"proof ({engine})"
        )
    if _re_search(r"\b(?:setpgid|setsid|getsid)\s*\(", body):
        return (
            f"setpgid unencoded: unconstrained setpgid is not a "
            f"proof ({engine})"
        )
    if _re_search(r"\b(?:setuid|seteuid|setgid)\s*\(", body):
        return (
            f"setuid unencoded: unconstrained setuid is not a "
            f"proof ({engine})"
        )
    if _re_search(r"\bsocket\s*\(", body):
        return (
            f"socket unencoded: unconstrained socket is not a "
            f"proof ({engine})"
        )
    if _re_search(r"(?<![:\w])bind\s*\(", body):
        return (
            f"bind unencoded: unconstrained bind is not a "
            f"proof ({engine})"
        )
    if _re_search(r"\blisten\s*\(", body):
        return (
            f"listen unencoded: unconstrained listen is not a "
            f"proof ({engine})"
        )
    if _re_search(r"\bconnect\s*\(", body):
        return (
            f"connect unencoded: unconstrained connect is not a "
            f"proof ({engine})"
        )
    if _re_search(r"\bpipe2?\s*\(", body):
        return f"pipe unencoded: {engine} is not a pipe model"
    if _re_search(r"\bdup[23]?\s*\(", body):
        return f"dup unencoded: {engine} is not an fd model"
    if _re_search(r"\bfcntl\s*\(", body):
        return (
            f"fcntl unencoded: unconstrained fcntl is not a "
            f"proof ({engine})"
        )
    if _re_search(r"\bpd(?:getpid|wait4)\s*\(", body):
        return (
            f"pdgetpid unencoded: unconstrained pdgetpid is not a "
            f"proof ({engine})"
        )
    if _re_search(r"\b(?:wait4|wait3)\s*\(", body):
        return (
            f"wait4 unencoded: unconstrained wait4 is not a "
            f"proof ({engine})"
        )
    if _re_search(r"\bwait6\s*\(", body):
        return (
            f"wait6 unencoded: unconstrained wait6 is not a "
            f"proof ({engine})"
        )
    if _re_search(r"\bwait(?:pid|id)?\s*\(", body):
        return (
            f"wait unencoded: unconstrained wait is not a "
            f"proof ({engine})"
        )
    if _re_search(r"\bunlinkat\s*\(", body):
        return (
            f"unlinkat unencoded: unconstrained unlinkat is not a "
            f"proof ({engine})"
        )
    if _re_search(r"\bunlink\s*\(", body):
        return (
            f"unlink unencoded: unconstrained unlink is not a "
            f"proof ({engine})"
        )
    if _re_search(r"\bmknodat\s*\(", body):
        return (
            f"mknodat unencoded: unconstrained mknodat is not a "
            f"proof ({engine})"
        )
    if _re_search(r"\b(?:mkfifo|mknod)\s*\(", body):
        return (
            f"mkfifo unencoded: unconstrained mkfifo is not a "
            f"proof ({engine})"
        )
    if _re_search(r"\bepoll_create(?:1)?\s*\(", body):
        return (
            f"epoll_create unencoded: unconstrained epoll_create is not a "
            f"proof ({engine})"
        )
    if _re_search(r"\bepoll_pwait(?:2)?\s*\(", body):
        return (
            f"epoll_pwait unencoded: unconstrained epoll_pwait is not a "
            f"proof ({engine})"
        )
    if _re_search(r"\bppoll\s*\(", body):
        return (
            f"ppoll unencoded: unconstrained ppoll is not a "
            f"proof ({engine})"
        )
    if _re_search(
        r"\b(?:pselect|select|epoll_wait|epoll_ctl|poll)\s*\(",
        body,
    ):
        return (
            f"select unencoded: unconstrained I/O multiplex is not a "
            f"proof ({engine})"
        )
    if _re_search(r"\b(?:sendmsg|recvmsg)\s*\(", body):
        return (
            f"sendmsg unencoded: unconstrained sendmsg is not a "
            f"proof ({engine})"
        )
    if _re_search(
        r"\b(?:sendto|recvfrom|send|recv|shutdown)\s*\(",
        body,
    ):
        return (
            f"send unencoded: unconstrained socket I/O is not a "
            f"proof ({engine})"
        )
    if _re_search(r"\brt_(?:tgsigqueueinfo|sigqueueinfo)\s*\(", body):
        return (
            f"rt_sigqueueinfo unencoded: unconstrained rt_sigqueueinfo "
            f"is not a proof ({engine})"
        )
    if _re_search(r"\bsigqueue\s*\(", body):
        return (
            f"sigqueue unencoded: unconstrained sigqueue is not a "
            f"proof ({engine})"
        )
    if _re_search(r"\bkill_dependency\s*\(", body):
        return (
            f"C++ kill_dependency unencoded: {engine} is not a "
            "kill_dependency model"
        )
    if _re_search(r"\bthr_(?:new|kill2|kill|self|exit|suspend|wake)\s*\(", body):
        return (
            f"thr_kill unencoded: unconstrained thr_kill is not a "
            f"proof ({engine})"
        )
    if _re_search(r"\bpthread_kill\s*\(", body):
        return (
            f"pthread_kill unencoded: unconstrained pthread_kill is not a "
            f"proof ({engine})"
        )
    if _re_search(r"\b(?:kill|raise|alarm)\s*\(", body):
        return (
            f"kill unencoded: unconstrained signal delivery is not a "
            f"proof ({engine})"
        )
    if _re_search(r"\b(?:getaddrinfo|freeaddrinfo)\s*\(", body):
        return (
            f"addrinfo unencoded: unconstrained DNS is not a "
            f"proof ({engine})"
        )
    if _re_search(r"\bpthread_cancel\s*\(", body):
        return (
            f"pthread_cancel unencoded: unconstrained pthread_cancel "
            f"is not a proof ({engine})"
        )
    if _re_search(
        r"\bpthread_(?:key_create|key_delete|setspecific|getspecific)\s*\(",
        body,
    ):
        return (
            f"pthread_key_create unencoded: unconstrained "
            f"pthread_key_create is not a proof ({engine})"
        )
    if _re_search(r"\b(?:pthread_join|pthread_detach)\s*\(", body):
        return (
            f"pthread_join unencoded: unconstrained pthread_join is not a "
            f"proof ({engine})"
        )
    if _re_search(
        r"\bthrd_(?:create|join|detach|exit|sleep|yield|current|equal)\s*\(",
        body,
    ):
        return (
            f"ISO C11 thrd unencoded: unconstrained thrd_* is not a "
            f"proof ({engine})"
        )
    if _re_search(r"\b(?:sem_wait|sem_post)\s*\(", body):
        return (
            f"sem unencoded: unconstrained sem_wait is not a "
            f"proof ({engine})"
        )
    if _re_search(r"\bopenat\s*\(", body):
        return (
            f"openat unencoded: unconstrained openat is not a "
            f"proof ({engine})"
        )
    if _re_search(r"\bflock\s*\(", body):
        return (
            f"flock unencoded: unconstrained flock is not a "
            f"proof ({engine})"
        )
    if _re_search(r"\b(?:fchown|lchown|chown)\s*\(", body):
        return (
            f"chown unencoded: unconstrained chown is not a "
            f"proof ({engine})"
        )
    if _re_search(r"\bsymlinkat\s*\(", body):
        return (
            f"symlinkat unencoded: unconstrained symlinkat is not a "
            f"proof ({engine})"
        )
    if _re_search(r"\breadlinkat\s*\(", body):
        return (
            f"readlinkat unencoded: unconstrained readlinkat is not a "
            f"proof ({engine})"
        )
    if _re_search(r"\b(?:symlink|readlink)\s*\(", body):
        return (
            f"symlink unencoded: unconstrained symlink is not a "
            f"proof ({engine})"
        )
    if _re_search(r"\b(?:pthread_join|pthread_detach|pthread_once)\s*\(", body):
        return (
            f"pthread join unencoded: unconstrained thread join is not a "
            f"proof ({engine})"
        )
    if _re_search(r"\b(?:sem_wait|sem_post|sem_init|sem_destroy)\s*\(", body):
        return f"sem unencoded: {engine} is not a semaphore model"
    if _re_search(r"\bksem_(?:open|close|unlink|wait|post)\s*\(", body):
        return (
            f"ksem_open unencoded: unconstrained ksem_open is not a "
            f"proof ({engine})"
        )
    if _re_search(r"\bsem_(?:open|close|unlink)\s*\(", body):
        return (
            f"sem_open unencoded: unconstrained sem_open is not a "
            f"proof ({engine})"
        )
    if _re_search(r"\bsem_timedwait\s*\(", body):
        return (
            f"sem_timedwait unencoded: unconstrained sem_timedwait is not a "
            f"proof ({engine})"
        )
    if _re_search(r"\bsem_(?:trywait|getvalue)\s*\(", body):
        return (
            f"sem_trywait unencoded: unconstrained sem_trywait is not a "
            f"proof ({engine})"
        )
    if _re_search(
        r"\bpthread_spin_(?:try)?(?:lock|unlock|init|destroy)\s*\(",
        body,
    ):
        return (
            f"pthread_spin unencoded: unconstrained pthread_spin_lock "
            f"is not a proof ({engine})"
        )
    if _re_search(
        r"\bpthread_rwlock_(?:try|timed)?(?:rdlock|wrlock|unlock|init|destroy)\s*\(",
        body,
    ):
        return (
            f"pthread_rwlock unencoded: unconstrained pthread_rwlock "
            f"is not a proof ({engine})"
        )
    if _re_search(
        r"\bpthread_cond_(?:timedwait|wait|signal|broadcast|init|destroy)\s*\(",
        body,
    ):
        return (
            f"pthread_cond unencoded: unconstrained pthread_cond_wait "
            f"is not a proof ({engine})"
        )
    if _re_search(r"\bpthread_barrier_(?:wait|init|destroy)\s*\(", body):
        return (
            f"pthread_barrier unencoded: unconstrained pthread_barrier_wait "
            f"is not a proof ({engine})"
        )
    if _re_search(r"\bopenat\s*\(", body):
        return (
            f"openat unencoded: unconstrained openat is not a "
            f"proof ({engine})"
        )
    if _re_search(r"\bflock\s*\(", body):
        return (
            f"flock unencoded: unconstrained flock is not a "
            f"proof ({engine})"
        )
    if _re_search(r"\b(?:posix_memalign|aligned_alloc)\s*\(", body):
        return (
            f"aligned_alloc unencoded: {engine} is not an aligned-alloc model"
        )
    if _re_search(r"\bvalloc\s*\(", body):
        return (
            f"valloc unencoded: unconstrained valloc is not a "
            f"proof ({engine})"
        )
    if _re_search(
        r"\b(?:__atomic_load|__atomic_store|"
        r"__sync_fetch_and_add|__sync_bool_compare_and_swap)\s*\(",
        body,
    ):
        return (
            f"atomic builtin unencoded: {engine} is not an "
            "atomic-builtin model"
        )
    if _re_search(r"\b(?:fchown|lchown|chown)\s*\(", body):
        return (
            f"chown unencoded: unconstrained chown is not a "
            f"proof ({engine})"
        )
    if _re_search(r"\b(?:symlink|readlink)\s*\(", body):
        return (
            f"symlink unencoded: unconstrained symlink/readlink is not a "
            f"proof ({engine})"
        )
    if _re_search(r"\bfts_(?:open|read|children|close|set)\s*\(", body):
        return (
            f"fts_open unencoded: unconstrained fts_open is not a "
            f"proof ({engine})"
        )
    if _re_search(r"\b(?:fdopendir|opendir|readdir|closedir)\s*\(", body):
        return (
            f"opendir unencoded: {engine} is not a DIR* model"
        )
    if _re_search(r"\b(?:setrlimit|getrlimit)\s*\(", body):
        return (
            f"setrlimit unencoded: unconstrained setrlimit is not a "
            f"proof ({engine})"
        )
    if _re_search(r"\b(?:getsockname|getpeername)\s*\(", body):
        return (
            f"getsockname unencoded: unconstrained getsockname is not a "
            f"proof ({engine})"
        )
    if _re_search(r"\bgetpeereid\s*\(", body):
        return (
            f"getpeereid unencoded: unconstrained getpeereid is not a "
            f"proof ({engine})"
        )
    if _re_search(r"\b(?:getsockopt|setsockopt)\s*\(", body):
        return (
            f"getsockopt unencoded: unconstrained socket opts is not a "
            f"proof ({engine})"
        )
    if _re_search(r"\b(?:listmount|statmount)\s*\(", body):
        return (
            f"listmount unencoded: unconstrained listmount/statmount "
            f"is not a proof ({engine})"
        )
    if _re_search(r"\bustat\s*\(", body):
        return (
            f"ustat unencoded: unconstrained ustat is not a "
            f"proof ({engine})"
        )
    if _re_search(r"\bfile_(?:get|set)attr\s*\(", body):
        return (
            f"file_getattr unencoded: unconstrained file_getattr "
            f"is not a proof ({engine})"
        )
    if _re_search(r"\bfh(?:linkat|link|readlink)\s*\(", body):
        return (
            f"fhlink unencoded: unconstrained fhlink is not a "
            f"proof ({engine})"
        )
    if _re_search(r"\b(?:getfh|fhopen|fhstatfs|fhstat|getfhat)\s*\(", body):
        return (
            f"getfh unencoded: unconstrained getfh is not a "
            f"proof ({engine})"
        )
    if _re_search(r"\bfstatat\s*\(", body):
        return (
            f"fstatat unencoded: unconstrained fstatat is not a "
            f"proof ({engine})"
        )
    if _re_search(r"\b(?:lstat|fstat|stat)\s*\(", body):
        return (
            f"stat unencoded: unconstrained stat is not a "
            f"proof ({engine})"
        )
    if _re_search(r"\brenameat2\s*\(", body):
        return (
            f"renameat2 unencoded: unconstrained renameat2 is not a "
            f"proof ({engine})"
        )
    if _re_search(r"\brenameat\s*\(", body):
        return (
            f"renameat unencoded: unconstrained renameat is not a "
            f"proof ({engine})"
        )
    if _re_search(r"\bmkdirat\s*\(", body):
        return (
            f"mkdirat unencoded: unconstrained mkdirat is not a "
            f"proof ({engine})"
        )
    if _re_search(r"\b(?:mkdir|rmdir|rename)\s*\(", body):
        return (
            f"mkdir unencoded: unconstrained mkdir is not a "
            f"proof ({engine})"
        )
    if _re_search(r"\bcrypt_(?:newhash|checkpass)\s*\(", body):
        return (
            f"crypt_newhash unencoded: unconstrained crypt_newhash is not a "
            f"proof ({engine})"
        )
    if _re_search(r"\bkenv\s*\(", body):
        return (
            f"kenv unencoded: unconstrained kenv is not a "
            f"proof ({engine})"
        )
    if _re_search(r"\b(?:getpwuid|getpwnam|crypt)\s*\(", body):
        return (
            f"getpwuid unencoded: unconstrained getpwuid is not a "
            f"proof ({engine})"
        )
    if _re_search(
        r"\b(?:clock_settime|clock_adjtime|clock_nanosleep)\s*\(",
        body,
    ):
        return (
            f"clock_settime unencoded: unconstrained clock_settime "
            f"is not a proof ({engine})"
        )
    if _re_search(r"\bsettimeofday\s*\(", body):
        return (
            f"settimeofday unencoded: unconstrained settimeofday is not a "
            f"proof ({engine})"
        )
    if _re_search(r"\bpthread_getcpuclockid\s*\(", body):
        return (
            f"pthread_getcpuclockid unencoded: unconstrained "
            f"pthread_getcpuclockid is not a proof ({engine})"
        )
    if _re_search(r"\bclock_getcpuclockid\s*\(", body):
        return (
            f"clock_getcpuclockid unencoded: unconstrained "
            f"clock_getcpuclockid is not a proof ({engine})"
        )
    if _re_search(r"\b(?:clock_gettime|gettimeofday)\s*\(", body):
        return (
            f"clock_gettime unencoded: unconstrained clock_gettime is not a "
            f"proof ({engine})"
        )
    if _re_search(r"\bposix_typed_mem_(?:open|get_info)\s*\(", body):
        return (
            f"posix_typed_mem_open unencoded: unconstrained "
            f"posix_typed_mem_open is not a proof ({engine})"
        )
    if _re_search(r"\b(?:shm_open|shm_unlink)\s*\(", body):
        return (
            f"shm unencoded: unconstrained shm_open is not a "
            f"proof ({engine})"
        )
    if _re_search(
        r"\bposix_spawn(?:_file_actions|attr)_init\s*\(",
        body,
    ):
        return (
            f"posix_spawn_file_actions_init unencoded: unconstrained "
            f"posix_spawn_file_actions_init is not a proof ({engine})"
        )
    if _re_search(r"\b(?:posix_spawnp|posix_spawn)\s*\(", body):
        return (
            f"posix_spawn unencoded: unconstrained posix_spawn is not a "
            f"proof ({engine})"
        )
    if _re_search(r"\b(?:globfree|glob)\s*\(", body):
        return (
            f"glob unencoded: unconstrained glob is not a "
            f"proof ({engine})"
        )
    if _re_search(r"\b(?:fseek|ftell|rewind|fgetpos|fsetpos)\s*\(", body):
        return (
            f"fseek unencoded: unconstrained fseek is not a "
            f"proof ({engine})"
        )
    if _re_search(r"\b(?:nanosleep|usleep|sleep)\s*\(", body):
        return (
            f"sleep unencoded: unconstrained sleep is not a "
            f"proof ({engine})"
        )
    if _re_search(r"\bfaccessat2\s*\(", body):
        return (
            f"faccessat2 unencoded: unconstrained faccessat2 is not a "
            f"proof ({engine})"
        )
    if _re_search(r"\bfaccessat\s*\(", body):
        return (
            f"faccessat unencoded: unconstrained faccessat is not a "
            f"proof ({engine})"
        )
    if _re_search(r"\bvhangup\s*\(", body):
        return (
            f"vhangup unencoded: unconstrained vhangup is not a "
            f"proof ({engine})"
        )
    if _re_search(r"\b(?:at_)?quick_exit\s*\(", body):
        return (
            f"quick_exit unencoded: unconstrained quick_exit is not a "
            f"proof ({engine})"
        )
    if _re_search(r"\beaccess\s*\(", body):
        return (
            f"eaccess unencoded: unconstrained eaccess is not a "
            f"proof ({engine})"
        )
    if _re_search(r"\baccess\s*\(", body):
        return (
            f"access unencoded: unconstrained access is not a "
            f"proof ({engine})"
        )
    if _re_search(r"\b(?:getopt_long_only|getopt_long|getopt)\s*\(", body):
        return (
            f"getopt unencoded: unconstrained getopt is not a "
            f"proof ({engine})"
        )
    if _re_search(r"\bgetosreldate\s*\(", body):
        return (
            f"getosreldate unencoded: unconstrained getosreldate is not a "
            f"proof ({engine})"
        )
    if _re_search(r"\bgetdomainname\s*\(", body):
        return (
            f"getdomainname unencoded: unconstrained getdomainname "
            f"is not a proof ({engine})"
        )
    if _re_search(r"\b(?:uname|gethostname)\s*\(", body):
        return (
            f"uname unencoded: unconstrained uname is not a "
            f"proof ({engine})"
        )
    if _re_search(r"\b(?:sendfile|copy_file_range)\s*\(", body):
        return (
            f"sendfile unencoded: unconstrained sendfile is not a "
            f"proof ({engine})"
        )
    if _re_search(r"\b(?:preadv2|pwritev2|preadv|pwritev)\s*\(", body):
        return (
            f"preadv unencoded: unconstrained preadv is not a "
            f"proof ({engine})"
        )
    if _re_search(
        r"\b(?:timerfd_settime|timerfd_gettime)\s*\(",
        body,
    ):
        return (
            f"timerfd_settime unencoded: unconstrained "
            f"timerfd_settime is not a proof ({engine})"
        )
    if _re_search(r"\beventfd_(?:read|write)\s*\(", body):
        return (
            f"eventfd_read unencoded: unconstrained eventfd_read is not a "
            f"proof ({engine})"
        )
    if _re_search(
        r"\b(?:memfd_create|eventfd|timerfd_create)\s*\(",
        body,
    ):
        return (
            f"memfd unencoded: unconstrained memfd_create is not a "
            f"proof ({engine})"
        )
    if _re_search(r"\barch_prctl\s*\(", body):
        return (
            f"arch_prctl unencoded: unconstrained arch_prctl is not a "
            f"proof ({engine})"
        )
    if _re_search(r"\bprocctl\s*\(", body):
        return (
            f"procctl unencoded: unconstrained procctl is not a "
            f"proof ({engine})"
        )
    if _re_search(r"\b(?:prctl|ptrace)\s*\(", body):
        return (
            f"prctl unencoded: unconstrained prctl is not a "
            f"proof ({engine})"
        )
    if _re_search(r"\bktrace\s*\(", body):
        return (
            f"ktrace unencoded: unconstrained ktrace is not a "
            f"proof ({engine})"
        )
    if _re_search(r"\b(?:tcgetattr|tcsetattr|cfmakeraw)\s*\(", body):
        return (
            f"tcgetattr unencoded: unconstrained tcgetattr is not a "
            f"proof ({engine})"
        )
    if _re_search(r"\bgetpagesizes\s*\(", body):
        return (
            f"getpagesizes unencoded: unconstrained getpagesizes is not a "
            f"proof ({engine})"
        )
    if _re_search(r"\bgetpagesize(?!s)\s*\(", body):
        return (
            f"getpagesize unencoded: unconstrained getpagesize is not a "
            f"proof ({engine})"
        )
    if _re_search(r"\blpathconf\s*\(", body):
        return (
            f"lpathconf unencoded: unconstrained lpathconf is not a "
            f"proof ({engine})"
        )
    if _re_search(r"\b(?:sysconf|fpathconf|pathconf)\s*\(", body):
        return (
            f"sysconf unencoded: unconstrained sysconf is not a "
            f"proof ({engine})"
        )
    if _re_search(r"\bgetrusage\s*\(", body):
        return (
            f"getrusage unencoded: unconstrained getrusage is not a "
            f"proof ({engine})"
        )
    if _re_search(r"\b(?:nftw|ftw)\s*\(", body):
        return (
            f"nftw unencoded: unconstrained nftw is not a "
            f"proof ({engine})"
        )
    if _re_search(r"\b(?:wordexp|wordfree)\s*\(", body):
        return (
            f"wordexp unencoded: unconstrained wordexp is not a "
            f"proof ({engine})"
        )
    if _re_search(r"\b(?:get|set)loginclass\s*\(", body):
        return (
            f"loginclass unencoded: unconstrained getloginclass is not a "
            f"proof ({engine})"
        )
    if _re_search(r"\b(?:login_getclass|setusercontext)\s*\(", body):
        return (
            f"login_getclass unencoded: unconstrained login_getclass is not a "
            f"proof ({engine})"
        )
    if _re_search(r"\bsetlogin\s*\(", body):
        return (
            f"setlogin unencoded: unconstrained setlogin is not a "
            f"proof ({engine})"
        )
    if _re_search(
        r"\b(?:getlogin_r|getlogin|ttyname_r|ttyname)\s*\(",
        body,
    ):
        return (
            f"getlogin unencoded: unconstrained getlogin is not a "
            f"proof ({engine})"
        )
    if _re_search(r"\b(?:inet_pton|inet_ntop|inet_aton)\s*\(", body):
        return (
            f"inet_pton unencoded: unconstrained inet_pton is not a "
            f"proof ({engine})"
        )
    if _re_search(r"\bmseal\s*\(", body):
        return (
            f"mseal unencoded: unconstrained mseal is not a "
            f"proof ({engine})"
        )
    if _re_search(r"\bmlock2\s*\(", body):
        return (
            f"mlock2 unencoded: unconstrained mlock2 is not a "
            f"proof ({engine})"
        )
    if _re_search(r"\b(?:munlockall|mlockall|munlock|mlock)\s*\(", body):
        return (
            f"mlock unencoded: unconstrained mlock is not a "
            f"proof ({engine})"
        )
    if _re_search(r"\bposix_fadvise(?:64)?\s*\(", body):
        return (
            f"posix_fadvise unencoded: unconstrained posix_fadvise is not a "
            f"proof ({engine})"
        )
    if _re_search(r"\breadahead\s*\(", body):
        return (
            f"readahead unencoded: unconstrained readahead is not a "
            f"proof ({engine})"
        )
    if _re_search(r"\b(?:posix_madvise|madvise)\s*\(", body):
        return (
            f"madvise unencoded: unconstrained madvise is not a "
            f"proof ({engine})"
        )
    if _re_search(r"\b(?:vmsplice|splice)\s*\(", body):
        return (
            f"splice unencoded: unconstrained splice is not a "
            f"proof ({engine})"
        )
    if _re_search(r"\binotify_rm_watch\s*\(", body):
        return (
            f"inotify_rm_watch unencoded: unconstrained inotify_rm_watch "
            f"is not a proof ({engine})"
        )
    if _re_search(
        r"\b(?:inotify_init1|inotify_init|inotify_add_watch)\s*\(",
        body,
    ):
        return (
            f"inotify unencoded: unconstrained inotify is not a "
            f"proof ({engine})"
        )
    if _re_search(r"\b(?:fdatasync|fsync)\s*\(", body):
        return (
            f"fsync unencoded: unconstrained fsync is not a "
            f"proof ({engine})"
        )
    if _re_search(r"\b(?:getrandom|getentropy)\s*\(", body):
        return (
            f"getrandom unencoded: unconstrained getrandom is not a "
            f"proof ({engine})"
        )
    if _re_search(r"\barc4random(?:_buf|_uniform)?\s*\(", body):
        return (
            f"arc4random unencoded: unconstrained arc4random is not a "
            f"proof ({engine})"
        )
    if _re_search(r"\bissetugid\s*\(", body):
        return (
            f"issetugid unencoded: unconstrained issetugid is not a "
            f"proof ({engine})"
        )
    if _re_search(r"\b(?:getdelim|getline)\s*\(", body):
        return (
            f"getline unencoded: unconstrained getline is not a "
            f"proof ({engine})"
        )
    if _re_search(r"\b(?:vasprintf|asprintf)\s*\(", body):
        return (
            f"asprintf unencoded: unconstrained asprintf is not a "
            f"proof ({engine})"
        )
    if _re_search(r"\b(?:strlcpy|strlcat)\s*\(", body):
        return (
            f"strlcpy unencoded: unconstrained strlcpy is not a "
            f"proof ({engine})"
        )
    if _re_search(
        r"\b(?:explicit_bzero|memset_s|explicit_memset)\s*\(",
        body,
    ):
        return (
            f"explicit_bzero unencoded: unconstrained explicit_bzero "
            f"is not a proof ({engine})"
        )
    if _re_search(r"\btimingsafe_(?:bcmp|memcmp)\s*\(", body):
        return (
            f"timingsafe_bcmp unencoded: unconstrained timingsafe_bcmp "
            f"is not a proof ({engine})"
        )
    if _re_search(r"\bisatty\s*\(", body):
        return (
            f"isatty unencoded: unconstrained isatty is not a "
            f"proof ({engine})"
        )
    if _re_search(
        r"\b(?:posix_openpt|ptsname_r|ptsname|grantpt|unlockpt)\s*\(",
        body,
    ):
        return (
            f"ptsname unencoded: unconstrained ptsname is not a "
            f"proof ({engine})"
        )
    if _re_search(r"\bnmount\s*\(", body):
        return (
            f"nmount unencoded: unconstrained nmount is not a "
            f"proof ({engine})"
        )
    if _re_search(r"\bunmount\b", body):
        return (
            f"unmount unencoded: unconstrained unmount is not a "
            f"proof ({engine})"
        )
    if _re_search(r"\b(?:umount2|umount|mount)\s*\(", body):
        return (
            f"mount unencoded: unconstrained mount is not a "
            f"proof ({engine})"
        )
    if _re_search(
        r"\b(?:open_wmemstream|open_memstream|fmemopen)\s*\(",
        body,
    ):
        return (
            f"fmemopen unencoded: unconstrained fmemopen is not a "
            f"proof ({engine})"
        )
    if _re_search(r"\bscandir\s*\(", body):
        return (
            f"scandir unencoded: unconstrained scandir is not a "
            f"proof ({engine})"
        )
    if _re_search(r"\bextattr_(?:set|get|delete|list)_(?:file|fd|link)\s*\(", body):
        return (
            f"extattr unencoded: unconstrained extattr_set_file is not a "
            f"proof ({engine})"
        )
    if _re_search(
        r"\b(?:set|get|list|remove)xattrat\s*\(",
        body,
    ):
        return (
            f"setxattrat unencoded: unconstrained setxattrat is not a "
            f"proof ({engine})"
        )
    if _re_search(
        r"\b(?:lsetxattr|fsetxattr|setxattr|listxattr|"
        r"removexattr|getxattr)\s*\(",
        body,
    ):
        return (
            f"setxattr unencoded: unconstrained setxattr is not a "
            f"proof ({engine})"
        )
    if _re_search(r"\b(?:sched_setattr|sched_getattr)\s*\(", body):
        return (
            f"sched_setattr unencoded: unconstrained sched_setattr is not a "
            f"proof ({engine})"
        )
    if _re_search(r"\bsched_yield\s*\(", body):
        return (
            f"sched_yield unencoded: unconstrained sched_yield is not a "
            f"proof ({engine})"
        )
    if _re_search(r"\bpthread_yield\s*\(", body):
        return (
            f"pthread_yield unencoded: unconstrained pthread_yield is not a "
            f"proof ({engine})"
        )
    if _re_search(r"\bcpuset_(?:set|get)affinity\s*\(", body):
        return (
            f"cpuset unencoded: unconstrained cpuset_setaffinity is not a "
            f"proof ({engine})"
        )
    if _re_search(r"\bsched_get_priority_(?:max|min)\s*\(", body):
        return (
            f"sched_get_priority_max unencoded: unconstrained "
            f"sched_get_priority_max is not a proof ({engine})"
        )
    if _re_search(r"\b(?:sched_setaffinity|sched_getaffinity)\s*\(", body):
        return (
            f"sched unencoded: unconstrained sched_setaffinity is not a "
            f"proof ({engine})"
        )
    if _re_search(
        r"\b(?:sched_setscheduler|sched_getscheduler|"
        r"sched_setparam|sched_getparam)\s*\(",
        body,
    ):
        return (
            f"sched_setscheduler unencoded: unconstrained "
            f"sched_setscheduler is not a proof ({engine})"
        )
    if _re_search(r"\blio_listio\s*\(", body):
        return (
            f"lio_listio unencoded: unconstrained lio_listio is not a "
            f"proof ({engine})"
        )
    if _re_search(
        r"\b(?:aio_suspend|aio_return|aio_error|aio_write|aio_read)\s*\(",
        body,
    ):
        return (
            f"aio unencoded: unconstrained aio_read is not a "
            f"proof ({engine})"
        )
    if _re_search(
        r"\b(?:io_uring_register|io_uring_setup|io_uring_enter)\s*\(",
        body,
    ):
        return (
            f"io_uring unencoded: unconstrained io_uring_setup is not a "
            f"proof ({engine})"
        )
    if _re_search(r"\b(?:capset|capget)\s*\(", body):
        return (
            f"capset unencoded: unconstrained capset is not a "
            f"proof ({engine})"
        )
    if _re_search(r"\bstatx\s*\(", body):
        return (
            f"statx unencoded: unconstrained statx is not a "
            f"proof ({engine})"
        )
    if _re_search(
        r"\b(?:pidfd_send_signal|pidfd_getfd|pidfd_open)\s*\(",
        body,
    ):
        return (
            f"pidfd unencoded: unconstrained pidfd_open is not a "
            f"proof ({engine})"
        )
    if _re_search(r"\b(?:fanotify_init|fanotify_mark)\s*\(", body):
        return (
            f"fanotify unencoded: unconstrained fanotify_init is not a "
            f"proof ({engine})"
        )
    if _re_search(r"\bseccomp\s*\(", body):
        return (
            f"seccomp unencoded: unconstrained seccomp is not a "
            f"proof ({engine})"
        )
    if _re_search(r"\b(?:getgrnam|getgrgid|getspnam)\s*\(", body):
        return (
            f"getgrnam unencoded: unconstrained getgrnam is not a "
            f"proof ({engine})"
        )
    if _re_search(r"\b(?:posix_fallocate|fallocate)\s*\(", body):
        return (
            f"fallocate unencoded: unconstrained posix_fallocate is not a "
            f"proof ({engine})"
        )
    if _re_search(r"\bclose_range\s*\(", body):
        return (
            f"close_range unencoded: unconstrained close_range is not a "
            f"proof ({engine})"
        )
    if _re_search(r"\bclosefrom\s*\(", body):
        return (
            f"closefrom unencoded: unconstrained closefrom is not a "
            f"proof ({engine})"
        )
    if _re_search(
        r"\blsm_(?:get_self_attr|set_self_attr|list_modules)\s*\(",
        body,
    ):
        return (
            f"lsm_get_self_attr unencoded: unconstrained "
            f"lsm_get_self_attr is not a proof ({engine})"
        )
    if _re_search(
        r"\b(?:landlock_create_ruleset|landlock_add_rule|"
        r"landlock_restrict_self)\s*\(",
        body,
    ):
        return (
            f"landlock unencoded: unconstrained landlock_create_ruleset "
            f"is not a proof ({engine})"
        )
    if _re_search(r"\brtprio(?:_thread)?\s*\(", body):
        return (
            f"rtprio unencoded: unconstrained rtprio is not a "
            f"proof ({engine})"
        )
    if _re_search(r"\b(?:getpriority|setpriority)\s*\(", body):
        return (
            f"getpriority unencoded: unconstrained getpriority is not a "
            f"proof ({engine})"
        )
    if _re_search(r"\bsignalfd\s*\(", body):
        return (
            f"signalfd unencoded: unconstrained signalfd is not a "
            f"proof ({engine})"
        )
    if _re_search(r"\bsigaction\s*\(", body):
        return (
            f"sigaction unencoded: unconstrained sigaction is not a "
            f"proof ({engine})"
        )
    if _re_search(r"\bpthread_sigmask\s*\(", body):
        return (
            f"pthread_sigmask unencoded: unconstrained pthread_sigmask "
            f"is not a proof ({engine})"
        )
    if _re_search(r"\bsig(?:procmask|suspend)\s*\(", body):
        return (
            f"sigprocmask unencoded: unconstrained sigprocmask is not a "
            f"proof ({engine})"
        )
    if _re_search(r"\bsig(?:waitinfo|timedwait|pending|wait)\s*\(", body):
        return (
            f"sigwait unencoded: unconstrained sigwait is not a "
            f"proof ({engine})"
        )
    if _re_search(r"\bsigaltstack\s*\(", body):
        return (
            f"sigaltstack unencoded: unconstrained sigaltstack is not a "
            f"proof ({engine})"
        )
    if _re_search(r"\bbpf\s*\(", body):
        return (
            f"bpf unencoded: unconstrained bpf is not a "
            f"proof ({engine})"
        )
    if _re_search(r"\buserfaultfd\s*\(", body):
        return (
            f"userfaultfd unencoded: unconstrained userfaultfd is not a "
            f"proof ({engine})"
        )
    if _re_search(r"\bgetpass\s*\(", body):
        return (
            f"getpass unencoded: unconstrained getpass is not a "
            f"proof ({engine})"
        )
    if _re_search(r"\bgetgrouplist\s*\(", body):
        return (
            f"getgrouplist unencoded: unconstrained getgrouplist is not a "
            f"proof ({engine})"
        )
    if _re_search(r"\bgetgroups\s*\(", body):
        return (
            f"getgroups unencoded: unconstrained getgroups is not a "
            f"proof ({engine})"
        )
    if _re_search(r"\b(?:initgroups|setgroups)\s*\(", body):
        return (
            f"initgroups unencoded: unconstrained initgroups is not a "
            f"proof ({engine})"
        )
    if _re_search(r"\b(?:unshare|setns|clone)\s*\(", body):
        return (
            f"unshare unencoded: unconstrained unshare/clone is not a "
            f"proof ({engine})"
        )
    if _re_search(r"\bopenat2\s*\(", body):
        return (
            f"openat2 unencoded: unconstrained openat2 is not a "
            f"proof ({engine})"
        )
    if _re_search(r"\b(?:sendmmsg|recvmmsg)\s*\(", body):
        return (
            f"sendmmsg unencoded: unconstrained sendmmsg is not a "
            f"proof ({engine})"
        )
    if _re_search(r"\b(?:name_to_handle_at|open_by_handle_at)\s*\(", body):
        return (
            f"name_to_handle unencoded: unconstrained name_to_handle_at "
            f"is not a proof ({engine})"
        )
    if _re_search(r"\bprocess_madvise\s*\(", body):
        return (
            f"process_madvise unencoded: unconstrained process_madvise "
            f"is not a proof ({engine})"
        )
    if _re_search(r"\bpersonality\s*\(", body):
        return (
            f"personality unencoded: unconstrained personality is not a "
            f"proof ({engine})"
        )
    if _re_search(r"\bquotactl\s*\(", body):
        return (
            f"quotactl unencoded: unconstrained quotactl is not a "
            f"proof ({engine})"
        )
    if _re_search(r"\bpivot_root\s*\(", body):
        return (
            f"pivot_root unencoded: unconstrained pivot_root is not a "
            f"proof ({engine})"
        )
    if _re_search(r"\bmembarrier\s*\(", body):
        return (
            f"membarrier unencoded: unconstrained membarrier is not a "
            f"proof ({engine})"
        )
    if _re_search(r"\bpkey_alloc\s*\(", body):
        return (
            f"pkey_alloc unencoded: unconstrained pkey_alloc is not a "
            f"proof ({engine})"
        )
    if _re_search(r"\bstatfs\s*\(", body):
        return (
            f"statfs unencoded: unconstrained statfs is not a "
            f"proof ({engine})"
        )
    if _re_search(r"\bgetmntinfo\s*\(", body):
        return (
            f"getmntinfo unencoded: unconstrained getmntinfo is not a "
            f"proof ({engine})"
        )
    if _re_search(r"\bgetvfsbyname\s*\(", body):
        return (
            f"getvfsbyname unencoded: unconstrained getvfsbyname is not a "
            f"proof ({engine})"
        )
    if _re_search(r"\b(?:get|set|end)fsent\s*\(", body):
        return (
            f"getfsent unencoded: unconstrained getfsent is not a "
            f"proof ({engine})"
        )
    if _re_search(r"\bgetfsstat\s*\(", body):
        return (
            f"getfsstat unencoded: unconstrained getfsstat is not a "
            f"proof ({engine})"
        )
    if _re_search(r"\bsyncfs\s*\(", body):
        return (
            f"syncfs unencoded: unconstrained syncfs is not a "
            f"proof ({engine})"
        )
    if _re_search(r"\b(?:prlimit64|prlimit)\s*\(", body):
        return (
            f"prlimit unencoded: unconstrained prlimit is not a "
            f"proof ({engine})"
        )
    if _re_search(r"\b(?:migrate_pages|move_pages)\s*\(", body):
        return (
            f"move_pages unencoded: unconstrained move_pages is not a "
            f"proof ({engine})"
        )
    if _re_search(r"\bprocess_vm_readv\s*\(", body):
        return (
            f"process_vm_readv unencoded: unconstrained "
            f"process_vm_readv is not a proof ({engine})"
        )
    if _re_search(r"\bperf_event_open\s*\(", body):
        return (
            f"perf_event_open unencoded: unconstrained "
            f"perf_event_open is not a proof ({engine})"
        )
    if _re_search(r"\bclone3\s*\(", body):
        return (
            f"clone3 unencoded: unconstrained clone3 is not a "
            f"proof ({engine})"
        )
    if _re_search(r"\bkcmp\s*\(", body):
        return (
            f"kcmp unencoded: unconstrained kcmp is not a "
            f"proof ({engine})"
        )
    if _re_search(r"\bkeyctl\s*\(", body):
        return (
            f"keyctl unencoded: unconstrained keyctl is not a "
            f"proof ({engine})"
        )
    if _re_search(r"\bopen_tree_attr\s*\(", body):
        return (
            f"open_tree_attr unencoded: unconstrained open_tree_attr "
            f"is not a proof ({engine})"
        )
    if _re_search(
        r"\b(?:fsopen|fsmount|open_tree|move_mount|fspick|fsconfig)\s*\(",
        body,
    ):
        return (
            f"fsopen unencoded: unconstrained fsopen is not a "
            f"proof ({engine})"
        )
    if _re_search(r"\bprocess_mrelease\s*\(", body):
        return (
            f"process_mrelease unencoded: unconstrained "
            f"process_mrelease is not a proof ({engine})"
        )
    if _re_search(r"\bmemfd_secret\s*\(", body):
        return (
            f"memfd_secret unencoded: unconstrained memfd_secret "
            f"is not a proof ({engine})"
        )
    if _re_search(r"\b(?:ioprio_set|ioprio_get)\s*\(", body):
        return (
            f"ioprio unencoded: unconstrained ioprio is not a "
            f"proof ({engine})"
        )
    if _re_search(r"\bmq_open\s*\(", body):
        return (
            f"mq_open unencoded: unconstrained mq_open is not a "
            f"proof ({engine})"
        )
    if _re_search(r"\bshmget\s*\(", body):
        return (
            f"shmget unencoded: unconstrained shmget is not a "
            f"proof ({engine})"
        )
    if _re_search(r"\b_umtx_op\s*\(", body):
        return (
            f"_umtx_op unencoded: unconstrained _umtx_op is not a "
            f"proof ({engine})"
        )
    if _re_search(r"\bfutex_waitv\s*\(", body):
        return (
            f"futex_waitv unencoded: unconstrained futex_waitv is not a "
            f"proof ({engine})"
        )
    if _re_search(r"\bfutex_(?:wake|wait|requeue)\s*\(", body):
        return (
            f"futex_wait unencoded: unconstrained futex_wait is not a "
            f"proof ({engine})"
        )
    if _re_search(r"\bfutex\s*\(", body):
        return (
            f"futex unencoded: unconstrained futex is not a "
            f"proof ({engine})"
        )
    if _re_search(r"\badjtimex\s*\(", body):
        return (
            f"adjtimex unencoded: unconstrained adjtimex is not a "
            f"proof ({engine})"
        )
    if _re_search(r"\b(?:ntp_)?adjtime\s*\(", body):
        return (
            f"adjtime unencoded: unconstrained adjtime is not a "
            f"proof ({engine})"
        )
    if _re_search(r"\bntp_gettime\s*\(", body):
        return (
            f"ntp_gettime unencoded: unconstrained ntp_gettime is not a "
            f"proof ({engine})"
        )
    if _re_search(r"\brevoke\s*\(", body):
        return (
            f"revoke unencoded: unconstrained revoke is not a "
            f"proof ({engine})"
        )
    if _re_search(r"\bjail(?:_attach|_get|_set|_remove)?\s*\(", body):
        return (
            f"jail unencoded: unconstrained jail is not a "
            f"proof ({engine})"
        )
    if _re_search(r"\b(?:fflagstostr|strtofflags)\s*\(", body):
        return (
            f"fflagstostr unencoded: unconstrained fflagstostr is not a "
            f"proof ({engine})"
        )
    if _re_search(r"\bstrmode\s*\(", body):
        return (
            f"strmode unencoded: unconstrained strmode is not a "
            f"proof ({engine})"
        )
    if _re_search(r"\bstrtonum\s*\(", body):
        return (
            f"strtonum unencoded: unconstrained strtonum is not a "
            f"proof ({engine})"
        )
    if _re_search(r"\breallocarray\s*\(", body):
        return (
            f"reallocarray unencoded: unconstrained reallocarray is not a "
            f"proof ({engine})"
        )
    if _re_search(r"\breallocf\s*\(", body):
        return (
            f"reallocf unencoded: unconstrained reallocf is not a "
            f"proof ({engine})"
        )
    if _re_search(r"\b(?:get|set)progname\s*\(", body):
        return (
            f"getprogname unencoded: unconstrained getprogname is not a "
            f"proof ({engine})"
        )
    if _re_search(r"\b(?:setproctitle|daemon)\s*\(", body):
        return (
            f"daemon unencoded: unconstrained daemon is not a "
            f"proof ({engine})"
        )
    if _re_search(r"\b(?:auditon|getaudit|setaudit|auditctl)\s*\(", body):
        return (
            f"auditon unencoded: unconstrained auditon is not a "
            f"proof ({engine})"
        )
    if _re_search(r"\bkinfo_get(?:proc|file|vmmap)\s*\(", body):
        return (
            f"kinfo_getproc unencoded: unconstrained kinfo_getproc is not a "
            f"proof ({engine})"
        )
    if _re_search(r"\bkvm_(?:open|openfiles|getprocs|close|nlist)\s*\(", body):
        return (
            f"kvm_open unencoded: unconstrained kvm_open is not a "
            f"proof ({engine})"
        )
    if _re_search(r"\buuidgen\s*\(", body):
        return (
            f"uuidgen unencoded: unconstrained uuidgen is not a "
            f"proof ({engine})"
        )
    if _re_search(r"\bsetfib\s*\(", body):
        return (
            f"setfib unencoded: unconstrained setfib is not a "
            f"proof ({engine})"
        )
    if _re_search(r"\bsethostname\s*\(", body):
        return (
            f"sethostname unencoded: unconstrained sethostname is not a "
            f"proof ({engine})"
        )
    if _re_search(r"\breboot\s*\(", body):
        return (
            f"reboot unencoded: unconstrained reboot is not a "
            f"proof ({engine})"
        )
    if _re_search(r"\b(?:swapon|swapoff)\s*\(", body):
        return (
            f"swapon unencoded: unconstrained swapon is not a "
            f"proof ({engine})"
        )
    if _re_search(r"\bacct\s*\(", body):
        return (
            f"acct unencoded: unconstrained acct is not a "
            f"proof ({engine})"
        )
    if _re_search(r"\b(?:ioperm|iopl)\s*\(", body):
        return (
            f"ioperm unencoded: unconstrained ioperm is not a "
            f"proof ({engine})"
        )
    if _re_search(r"\bmincore\s*\(", body):
        return (
            f"mincore unencoded: unconstrained mincore is not a "
            f"proof ({engine})"
        )
    if _re_search(r"\brseq\s*\(", body):
        return (
            f"rseq unencoded: unconstrained rseq is not a "
            f"proof ({engine})"
        )
    if _re_search(r"\btimer_create\s*\(", body):
        return (
            f"timer_create unencoded: unconstrained timer_create "
            f"is not a proof ({engine})"
        )
    if _re_search(r"\bsemget\s*\(", body):
        return (
            f"semget unencoded: unconstrained semget is not a "
            f"proof ({engine})"
        )
    if _re_search(r"\bmsgget\s*\(", body):
        return (
            f"msgget unencoded: unconstrained msgget is not a "
            f"proof ({engine})"
        )
    if _re_search(r"\bsyslog\s*\(", body):
        return (
            f"syslog unencoded: unconstrained syslog is not a "
            f"proof ({engine})"
        )
    if _re_search(r"\bklogctl\s*\(", body):
        return (
            f"klogctl unencoded: unconstrained klogctl is not a "
            f"proof ({engine})"
        )
    if _re_search(r"\bmount_setattr\s*\(", body):
        return (
            f"mount_setattr unencoded: unconstrained "
            f"mount_setattr is not a proof ({engine})"
        )
    if _re_search(r"\bgetcpu\s*\(", body):
        return (
            f"getcpu unencoded: unconstrained getcpu is not a "
            f"proof ({engine})"
        )
    if _re_search(
        r"\b(?:init_module|finit_module|delete_module)\s*\(",
        body,
    ):
        return (
            f"init_module unencoded: unconstrained init_module "
            f"is not a proof ({engine})"
        )
    if _re_search(r"\b(?:kexec_load|kexec_file_load)\s*\(", body):
        return (
            f"kexec unencoded: unconstrained kexec_load is not a "
            f"proof ({engine})"
        )
    if _re_search(r"\bquotactl_fd\s*\(", body):
        return (
            f"quotactl_fd unencoded: unconstrained quotactl_fd "
            f"is not a proof ({engine})"
        )
    if _re_search(r"\b(?:pkey_free|pkey_mprotect)\s*\(", body):
        return (
            f"pkey_free unencoded: unconstrained pkey_free is not a "
            f"proof ({engine})"
        )
    if _re_search(r"\btgkill\s*\(", body):
        return (
            f"tgkill unencoded: unconstrained tgkill is not a "
            f"proof ({engine})"
        )
    if _re_search(r"\badd_key\s*\(", body):
        return (
            f"add_key unencoded: unconstrained add_key is not a "
            f"proof ({engine})"
        )
    if _re_search(r"\bsemctl\s*\(", body):
        return (
            f"semctl unencoded: unconstrained semctl is not a "
            f"proof ({engine})"
        )
    if _re_search(r"\bmsgctl\s*\(", body):
        return (
            f"msgctl unencoded: unconstrained msgctl is not a "
            f"proof ({engine})"
        )
    if _re_search(r"\bshmctl\s*\(", body):
        return (
            f"shmctl unencoded: unconstrained shmctl is not a "
            f"proof ({engine})"
        )
    if _re_search(r"\btimer_settime\s*\(", body):
        return (
            f"timer_settime unencoded: unconstrained timer_settime "
            f"is not a proof ({engine})"
        )
    if _re_search(r"\bsetdomainname\s*\(", body):
        return (
            f"setdomainname unencoded: unconstrained setdomainname "
            f"is not a proof ({engine})"
        )
    if _re_search(r"\b(?:io_submit|io_getevents)\s*\(", body):
        return (
            f"io_submit unencoded: unconstrained io_submit is not a "
            f"proof ({engine})"
        )
    if _re_search(
        r"\b(?:io_setup|io_destroy|io_cancel|io_pgetevents)\s*\(",
        body,
    ):
        return (
            f"io_setup unencoded: unconstrained io_setup is not a "
            f"proof ({engine})"
        )
    if _re_search(r"\brequest_key\s*\(", body):
        return (
            f"request_key unencoded: unconstrained request_key is not a "
            f"proof ({engine})"
        )
    if _re_search(r"\btkill\s*\(", body):
        return (
            f"tkill unencoded: unconstrained tkill is not a "
            f"proof ({engine})"
        )
    if _re_search(
        r"\b(?:timer_delete|timer_gettime|timer_getoverrun)\s*\(",
        body,
    ):
        return (
            f"timer_delete unencoded: unconstrained timer_delete "
            f"is not a proof ({engine})"
        )
    if _re_search(
        r"\b(?:mq_unlink|mq_timedsend|mq_timedreceive|mq_notify|"
        r"mq_getsetattr)\s*\(",
        body,
    ):
        return (
            f"mq_unlink unencoded: unconstrained mq_unlink is not a "
            f"proof ({engine})"
        )
    if _re_search(r"\b(?:shmat|shmdt)\s*\(", body):
        return (
            f"shmat unencoded: unconstrained shmat is not a "
            f"proof ({engine})"
        )
    if _re_search(r"\b(?:semop|semtimedop)\s*\(", body):
        return (
            f"semop unencoded: unconstrained semop is not a "
            f"proof ({engine})"
        )
    if _re_search(r"\b(?:msgsnd|msgrcv)\s*\(", body):
        return (
            f"msgsnd unencoded: unconstrained msgsnd is not a "
            f"proof ({engine})"
        )
    if _re_search(r"\bsync_file_range\s*\(", body):
        return (
            f"sync_file_range unencoded: unconstrained "
            f"sync_file_range is not a proof ({engine})"
        )
    if _re_search(r"\bremap_file_pages\s*\(", body):
        return (
            f"remap_file_pages unencoded: unconstrained "
            f"remap_file_pages is not a proof ({engine})"
        )
    if _re_search(r"\b(?:msync|mremap)\s*\(", body):
        return (
            f"msync unencoded: unconstrained msync is not a "
            f"proof ({engine})"
        )
    if _re_search(r"\bsocketpair\s*\(", body):
        return (
            f"socketpair unencoded: unconstrained socketpair is not a "
            f"proof ({engine})"
        )
    if _re_search(r"\bsysinfo\s*\(", body):
        return (
            f"sysinfo unencoded: unconstrained sysinfo is not a "
            f"proof ({engine})"
        )
    if _re_search(r"\bgettid\s*\(", body):
        return (
            f"gettid unencoded: unconstrained gettid is not a "
            f"proof ({engine})"
        )
    if _re_search(r"\b(?:setitimer|getitimer)\s*\(", body):
        return (
            f"setitimer unencoded: unconstrained setitimer is not a "
            f"proof ({engine})"
        )
    if _re_search(r"\bnice\s*\(", body):
        return (
            f"nice unencoded: unconstrained nice is not a "
            f"proof ({engine})"
        )
    if _re_search(r"\bgetdirentries\s*\(", body):
        return (
            f"getdirentries unencoded: unconstrained getdirentries is not a "
            f"proof ({engine})"
        )
    if _re_search(r"\bgetdents(?:64)?\s*\(", body):
        return (
            f"getdents unencoded: unconstrained getdents is not a "
            f"proof ({engine})"
        )
    if _re_search(r"\b(?:utimensat|futimens|utimes)\s*\(", body):
        return (
            f"utimensat unencoded: unconstrained utimensat is not a "
            f"proof ({engine})"
        )
    if _re_search(r"\blinkat\s*\(", body):
        return (
            f"linkat unencoded: unconstrained linkat is not a "
            f"proof ({engine})"
        )
    if _re_search(r"\bset_mempolicy_home_node\s*\(", body):
        return (
            f"set_mempolicy_home_node unencoded: unconstrained "
            f"set_mempolicy_home_node is not a proof ({engine})"
        )
    if _re_search(
        r"\b(?:mbind|set_mempolicy|get_mempolicy)\s*\(",
        body,
    ):
        return (
            f"mbind unencoded: unconstrained mbind is not a "
            f"proof ({engine})"
        )
    if _re_search(r"\bcachestat\s*\(", body):
        return (
            f"cachestat unencoded: unconstrained cachestat is not a "
            f"proof ({engine})"
        )
    if _re_search(r"\bmap_shadow_stack\s*\(", body):
        return (
            f"map_shadow_stack unencoded: unconstrained "
            f"map_shadow_stack is not a proof ({engine})"
        )
    if _re_search(r"\bstd\s*::\s*pmr\b|\bpmr\s*::", body):
        return (
            f"C++ pmr unencoded: {engine} is not a pmr model"
        )
    if _re_search(r"\b(?:std\s*::\s*)?u8string(?:_view)?\b", body):
        return (
            f"C++ u8string unencoded: {engine} is not a u8string model"
        )
    if _re_search(r"\b(?:std\s*::\s*)?unordered_multimap\s*<", body):
        return (
            f"C++ unordered_multimap unencoded: {engine} is not an "
            "unordered_multimap model"
        )
    if _re_search(r"\b(?:std\s*::\s*)?unordered_multiset\s*<", body):
        return (
            f"C++ unordered_multiset unencoded: {engine} is not an "
            "unordered_multiset model"
        )
    if _re_search(r"\b(?:std\s*::\s*)?shared_lock\b", body):
        return (
            f"C++ shared_lock unencoded: {engine} is not a "
            "shared_lock model"
        )
    if _re_search(r"\batomic_(?:thread|signal)_fence\s*\(", body):
        return (
            f"C++ atomic_thread_fence unencoded: {engine} is not an "
            "atomic_thread_fence model"
        )
    if _re_search(r"\b(?:std\s*::\s*)?atomic_flag\b", body):
        return (
            f"C++ atomic_flag unencoded: {engine} is not an "
            "atomic_flag model"
        )
    if _re_search(r"\brecursive_timed_mutex\b", body):
        return (
            f"C++ recursive_timed_mutex unencoded: {engine} is not a "
            "recursive_timed_mutex model"
        )
    if _re_search(r"\brecursive_mutex\b", body):
        return (
            f"C++ recursive_mutex unencoded: {engine} is not a "
            "recursive_mutex model"
        )
    if _re_search(r"\btimed_mutex\b", body):
        return (
            f"C++ timed_mutex unencoded: {engine} is not a "
            "timed_mutex model"
        )
    if _re_search(r"\b(?:ifstream|ofstream|fstream)\b", body):
        return (
            f"C++ fstream unencoded: {engine} is not an fstream model"
        )
    if _re_search(r"\bthis_thread\b", body):
        return (
            f"C++ this_thread unencoded: {engine} is not a "
            "this_thread model"
        )
    if _re_search(r"\bcall_once\s*\(", body):
        return (
            f"C++ call_once unencoded: {engine} is not a call_once model"
        )
    if _re_search(r"\b(?:std\s*::\s*)?tuple\s*<", body):
        return (
            f"C++ tuple unencoded: {engine} is not a tuple model"
        )
    if _re_search(r"\b(?:std\s*::\s*)?deque\s*<", body):
        return (
            f"C++ deque unencoded: {engine} is not a deque model"
        )
    if _re_search(r"\b(?:std\s*::\s*)?forward_list\s*<", body):
        return (
            f"C++ forward_list unencoded: {engine} is not a "
            "forward_list model"
        )
    if _re_search(r"\bstd\s*::\s*list\s*<", body):
        return (
            f"C++ std::list unencoded: {engine} is not a list model"
        )
    if _re_search(r"\b(?:std\s*::\s*)?unordered_map\s*<", body):
        return (
            f"C++ unordered_map unencoded: {engine} is not an "
            "unordered_map model"
        )
    if _re_search(r"\bstd\s*::\s*map\s*<", body):
        return (
            f"C++ std::map unencoded: {engine} is not a map model"
        )
    if _re_search(r"\bunordered_set\s*<", body):
        return (
            f"C++ unordered_set unencoded: {engine} is not an "
            "unordered_set model"
        )
    if _re_search(r"\bstd\s*::\s*set\s*<", body):
        return (
            f"C++ std::set unencoded: {engine} is not a set model"
        )
    if _re_search(r"\bpriority_queue\s*<", body):
        return (
            f"C++ priority_queue unencoded: {engine} is not a "
            "priority_queue model"
        )
    if _re_search(r"\bstd\s*::\s*queue\s*<", body):
        return (
            f"C++ std::queue unencoded: {engine} is not a queue model"
        )
    if _re_search(r"\bstd\s*::\s*stack\s*<", body):
        return (
            f"C++ std::stack unencoded: {engine} is not a stack model"
        )
    if _re_search(r"\bto_array\s*[<(]", body):
        return (
            f"C++ to_array unencoded: {engine} is not a to_array model"
        )
    if _re_search(r"\bfrom_range\b", body):
        return (
            f"C++ from_range unencoded: {engine} is not a from_range model"
        )
    if _re_search(r"\branges\s*::\s*to\s*[<(]", body):
        return (
            f"C++ ranges::to unencoded: {engine} is not a ranges::to model"
        )
    if _re_search(r"\bstd\s*::\s*array\s*<", body):
        return (
            f"C++ std::array unencoded: {engine} is not an array model"
        )
    if _re_search(r"\b(?:std\s*::\s*)?wstring_convert\b", body):
        return (
            f"C++ wstring_convert unencoded: {engine} is not a "
            "wstring_convert model"
        )
    if _re_search(r"\b(?:std\s*::\s*)?wstring(?:_view)?\b", body):
        return (
            f"C++ wstring unencoded: {engine} is not a wstring model"
        )
    if _re_search(r"\b(?:std\s*::\s*)?multimap\s*<", body):
        return (
            f"C++ multimap unencoded: {engine} is not a multimap model"
        )
    if _re_search(r"\b(?:std\s*::\s*)?multiset\s*<", body):
        return (
            f"C++ multiset unencoded: {engine} is not a multiset model"
        )
    if _re_search(r"\bbinary_semaphore\b", body):
        return (
            f"C++ binary_semaphore unencoded: {engine} is not a "
            "binary_semaphore model"
        )
    if _re_search(r"\berror_category\b", body):
        return (
            f"C++ error_category unencoded: {engine} is not an "
            "error_category model"
        )
    if _re_search(r"\bsystem_error\b", body):
        return (
            f"C++ system_error unencoded: {engine} is not a "
            "system_error model"
        )
    if _re_search(r"\b(?:std\s*::\s*)?error_code\b", body):
        return (
            f"C++ error_code unencoded: {engine} is not an "
            "error_code model"
        )
    if _re_search(r"\bstd\s*::\s*apply\s*\(", body):
        return (
            f"C++ std::apply unencoded: {engine} is not an apply model"
        )
    if _re_search(r"\bstd\s*::\s*invoke\s*\(", body):
        return (
            f"C++ std::invoke unencoded: {engine} is not an invoke model"
        )
    if _re_search(r"\bstd\s*::\s*endian\b", body):
        return (
            f"C++ std::endian unencoded: {engine} is not an endian model"
        )
    if _re_search(r"\bstd\s*::\s*rot[lr]\s*\(", body):
        return (
            f"C++ std::rotl unencoded: {engine} is not a rotl model"
        )
    if _re_search(
        r"\b(?:bit_ceil|bit_floor|has_single_bit|std\s*::\s*popcount)\s*\(",
        body,
    ):
        return (
            f"C++ bit_ceil unencoded: {engine} is not a bit_ceil model"
        )
    if _re_search(r"\bstd\s*::\s*bit_width\s*\(", body):
        return (
            f"C++ std::bit_width unencoded: {engine} is not a bit_width model"
        )
    if _re_search(r"\bstd\s*::\s*gcd\s*\(", body):
        return (
            f"C++ std::gcd unencoded: {engine} is not a gcd model"
        )
    if _re_search(r"\bstd\s*::\s*lcm\s*\(", body):
        return (
            f"C++ std::lcm unencoded: {engine} is not a lcm model"
        )
    if _re_search(r"\bstd\s*::\s*clamp\s*\(", body):
        return (
            f"C++ std::clamp unencoded: {engine} is not a clamp model"
        )
    if _re_search(r"\bstd\s*::\s*exchange\s*\(", body):
        return (
            f"C++ std::exchange unencoded: {engine} is not an exchange model"
        )
    if _re_search(r"\bstd\s*::\s*to_address\s*\(", body):
        return (
            f"C++ std::to_address unencoded: {engine} is not a "
            "to_address model"
        )
    if _re_search(r"\b(?:construct_at|destroy_at)\s*\(", body):
        return (
            f"C++ construct_at unencoded: {engine} is not a "
            "construct_at model"
        )
    if _re_search(r"\bdestroy_n\s*\(", body):
        return (
            f"C++ destroy_n unencoded: {engine} is not a destroy_n model"
        )
    if _re_search(r"\bstd\s*::\s*addressof\s*\(", body):
        return (
            f"C++ std::addressof unencoded: {engine} is not an "
            "addressof model"
        )
    if _re_search(r"\bassume_aligned\s*\(", body):
        return (
            f"C++ assume_aligned unencoded: {engine} is not an "
            "assume_aligned model"
        )
    if _re_search(r"\bas_rvalue(?:_view)?\b", body):
        return (
            f"C++ as_rvalue unencoded: {engine} is not an as_rvalue model"
        )
    if _re_search(r"\bas_const\s*\(", body):
        return (
            f"C++ as_const unencoded: {engine} is not an as_const model"
        )
    if _re_search(r"\btransform_(?:inclusive|exclusive)_scan\s*\(", body):
        return (
            f"C++ transform_inclusive_scan unencoded: {engine} is not a "
            "transform_inclusive_scan model"
        )
    if _re_search(r"\bexclusive_scan\s*\(", body):
        return (
            f"C++ exclusive_scan unencoded: {engine} is not an "
            "exclusive_scan model"
        )
    if _re_search(r"\binclusive_scan\s*\(", body):
        return (
            f"C++ inclusive_scan unencoded: {engine} is not an "
            "inclusive_scan model"
        )
    if _re_search(r"\btransform_reduce\s*\(", body):
        return (
            f"C++ transform_reduce unencoded: {engine} is not a "
            "transform_reduce model"
        )
    if _re_search(r"\bstd\s*::\s*reduce\s*\(", body):
        return (
            f"C++ std::reduce unencoded: {engine} is not a reduce model"
        )
    if _re_search(
        r"\buninitialized_(?:fill(?:_n)?|default_construct(?:_n)?)\s*\(",
        body,
    ):
        return (
            f"C++ uninitialized_fill unencoded: {engine} is not an "
            "uninitialized_fill model"
        )
    if _re_search(r"\buninitialized_value_construct(?:_n)?\s*\(", body):
        return (
            f"C++ uninitialized_value_construct unencoded: {engine} is not an "
            "uninitialized_value_construct model"
        )
    if _re_search(r"\buninitialized_(?:copy|move)(?:_n)?\s*\(", body):
        return (
            f"C++ uninitialized_copy unencoded: {engine} is not an "
            "uninitialized_copy model"
        )
    if _re_search(
        r"\b(?:add_sat|sub_sat|mul_sat|div_sat|saturate_cast)\s*\(",
        body,
    ):
        return (
            f"C++ add_sat unencoded: {engine} is not an add_sat model"
        )
    if _re_search(r"\bnontype\b", body):
        return (
            f"C++ nontype unencoded: {engine} is not a nontype model"
        )
    if _re_search(r"\bis_layout_compatible\b", body):
        return (
            f"C++ is_layout_compatible unencoded: {engine} is not an "
            "is_layout_compatible model"
        )
    if _re_search(r"\bis_pointer_interconvertible_(?:with_class|base_of)\b", body):
        return (
            f"C++ is_pointer_interconvertible unencoded: {engine} is not an "
            "is_pointer_interconvertible model"
        )
    if _re_search(r"\bbasic_const_iterator\b", body):
        return (
            f"C++ basic_const_iterator unencoded: {engine} is not a "
            "basic_const_iterator model"
        )
    if _re_search(r"\bis_corresponding_member\b", body):
        return (
            f"C++ is_corresponding_member unencoded: {engine} is not an "
            "is_corresponding_member model"
        )
    if _re_search(r"\bforward_like\b", body):
        return (
            f"C++ forward_like unencoded: {engine} is not a "
            "forward_like model"
        )
    if _re_search(r"\b(?:set|get)_terminate\s*\(", body):
        return (
            f"C++ set_terminate unencoded: {engine} is not a "
            "set_terminate model"
        )
    if _re_search(r"\bis_constant_evaluated\s*\(", body):
        return (
            f"C++ is_constant_evaluated unencoded: {engine} is not an "
            "is_constant_evaluated model"
        )
    if _re_search(r"\bstd\s*::\s*lerp\s*\(", body):
        return (
            f"C++ std::lerp unencoded: {engine} is not a lerp model"
        )
    if _re_search(r"\bstd\s*::\s*midpoint\s*\(", body):
        return (
            f"C++ std::midpoint unencoded: {engine} is not a midpoint model"
        )
    if _re_search(
        r"\bstd\s*::\s*(?:cmp_(?:less|greater|less_equal|"
        r"greater_equal|equal_to|not_equal_to)|in_range)\s*\(",
        body,
    ):
        return (
            f"C++ std::cmp_less unencoded: {engine} is not a cmp_less model"
        )
    if _re_search(r"\bstd\s*::\s*count[lr]_(?:zero|one)\s*\(", body):
        return (
            f"C++ std::countl_zero unencoded: {engine} is not a "
            "countl_zero model"
        )
    if _re_search(r"\b(?:std\s*::\s*)?byteswap\s*\(", body):
        return (
            f"C++ byteswap unencoded: {engine} is not a byteswap model"
        )
    if _re_search(r"\b(?:wcscpy|wcscat|wcsncpy|wcsncat)\s*\(", body):
        return (
            f"wide-string copy unencoded: unconstrained wcscpy is not a "
            f"proof of the buffer ({engine})"
        )
    if _re_search(
        r"\b(?:memcpy|memmove|mkstemp|mkstemps|mkdtemp|"
        r"tmpnam|tempnam|tmpnam_r|chroot|jail_attach|jail_remove|jail_get|jail_set|jail|popen|pclose|"
        r"umask|srand|srandom|mktemp|signal|"
        r"pthread_atfork|pdfork|pdwait4|pdgetpid|rfork|pledge|unveil|cap_getmode|cap_getrights|cap_fcntls_limit|cap_ioctls_limit|cap_sandboxed|cap_enter|cap_rights_limit|cap_rights_get|sysctlbyname|sysctl|"
        r"kqueue|kevent|pause|"
        r"getcontext|setcontext|swapcontext|makecontext|"
        r"pthread_attr_(?:init|destroy|setstacksize|setstack|"
        r"setdetachstate|getstacksize|getstack|getdetachstate)|"
        r"fork|vfork|execlp|execle|execl|"
        r"execveat|execvpe|execvp|execve|execv|"
        r"mmap|munmap|mprotect|minherit|sbrk|brk|cap_ioctls_limit|ioctl|"
        r"modfind|modstat|modnext|modfnext|kldfirstmod|kldnextmod|kld_isloaded|kld_load|kldload|kldunload|kldfind|kldsym|kldstat|"
        r"nfssvc|sysarch|getbootfile|devname_r|devname|"
        r"dlfunc|dlvsym|dlopen|dlsym|dlclose|accept4|accept|fchmodat2|fchmodat|fchmod|chmod|lchflags|fchflags|chflags|"
        r"setreuid|setregid|setresuid|setresgid|getresuid|getresgid|setfsuid|setfsgid|"
        r"setpgid|setsid|getsid|"
        r"setuid|seteuid|setgid|socket|listen|connect|unlinkat|unlink|"
        r"mkfifo|mknodat|mknod|pipe2|pipe|dup3|dup2|dup|cap_fcntls_limit|fcntl|"
        r"waitpid|waitid|pdwait4|wait6|wait4|wait3|wait|"
        r"pselect|select|epoll_create1|epoll_create|epoll_pwait2|epoll_pwait|"
        r"epoll_wait|epoll_ctl|ppoll|poll|"
        r"sendmsg|recvmsg|sendto|recvfrom|send|recv|shutdown|"
        r"kill_dependency|rt_tgsigqueueinfo|rt_sigqueueinfo|sigqueue|thr_kill2|thr_kill|thr_new|thr_self|thr_exit|thr_suspend|thr_wake|pthread_kill|kill|raise|alarm|getaddrinfo|freeaddrinfo|"
        r"pthread_cancel|pthread_key_create|pthread_key_delete|"
        r"pthread_setspecific|pthread_getspecific|"
        r"pthread_join|pthread_detach|pthread_once|"
        r"pthread_spin_(?:try)?(?:lock|unlock|init|destroy)|"
        r"pthread_rwlock_(?:try|timed)?(?:rdlock|wrlock|unlock|init|destroy)|"
        r"pthread_cond_(?:timedwait|wait|signal|broadcast|init|destroy)|"
        r"pthread_barrier_(?:wait|init|destroy)|"
        r"sem_timedwait|sem_trywait|sem_getvalue|sem_wait|sem_post|sem_init|sem_destroy|ksem_open|ksem_close|ksem_unlink|ksem_wait|ksem_post|sem_open|sem_close|sem_unlink|"
        r"openat|flock|valloc|posix_memalign|aligned_alloc|"
        r"fchown|lchown|chown|symlinkat|symlink|readlinkat|readlink|"
        r"fts_open|fts_read|fts_children|fts_close|fts_set|fdopendir|opendir|readdir|closedir|"
        r"setrlimit|getrlimit|getsockname|getpeername|getpeereid|getsockopt|setsockopt|"
        r"lstat|fstatat|fstat|statmount|listmount|ustat|file_getattr|file_setattr|fhlinkat|fhlink|fhreadlink|getfhat|getfh|fhopen|fhstatfs|fhstat|stat|mkdirat|mkdir|rmdir|renameat2|renameat|rename|"
        r"getpwuid|getpwnam|crypt_newhash|crypt_checkpass|crypt|"
        r"clock_settime|clock_adjtime|clock_nanosleep|settimeofday|"
        r"pthread_getcpuclockid|clock_getcpuclockid|"
        r"clock_gettime|gettimeofday|"
        r"posix_typed_mem_open|posix_typed_mem_get_info|shm_open|shm_unlink|"
        r"posix_spawn_file_actions_init|posix_spawnattr_init|posix_spawnp|posix_spawn|globfree|glob|"
        r"fseek|ftell|rewind|fgetpos|fsetpos|"
        r"nanosleep|usleep|sleep|faccessat2|faccessat|vhangup|at_quick_exit|quick_exit|eaccess|access|"
        r"getopt_long_only|getopt_long|getopt|"
        r"getosreldate|uname|gethostname|sendfile|copy_file_range|"
        r"preadv2|pwritev2|preadv|pwritev|"
        r"memfd_create|eventfd_read|eventfd_write|eventfd|"
        r"timerfd_settime|timerfd_gettime|timerfd_create|"
        r"arch_prctl|procctl|prctl|ptrace|ktrace|tcgetattr|tcsetattr|cfmakeraw|"
        r"getpagesizes|getpagesize|lpathconf|sysconf|fpathconf|pathconf|getrusage|"
        r"nftw|ftw|wordexp|wordfree|"
        r"getloginclass|setloginclass|login_getclass|setusercontext|setlogin|getlogin_r|getlogin|ttyname_r|ttyname|"
        r"inet_pton|inet_ntop|inet_aton|"
        r"munlockall|mlockall|munlock|mlock2|mseal|mlock|"
        r"posix_fadvise64|posix_fadvise|readahead|posix_madvise|madvise|vmsplice|splice|"
        r"inotify_init1|inotify_init|inotify_add_watch|inotify_rm_watch|"
        r"fdatasync|fsync|arc4random_uniform|arc4random_buf|arc4random|getrandom|getentropy|issetugid|"
        r"getdelim|getline|vasprintf|asprintf|"
        r"strlcpy|strlcat|timingsafe_memcmp|timingsafe_bcmp|explicit_bzero|memset_s|explicit_memset|"
        r"isatty|posix_openpt|ptsname_r|ptsname|grantpt|unlockpt|"
        r"unmount|nmount|umount2|umount|mount|"
        r"open_wmemstream|open_memstream|fmemopen|scandir|"
        r"setxattrat|getxattrat|listxattrat|removexattrat|"
        r"extattr_set_file|extattr_get_file|extattr_delete_file|extattr_list_file|"
        r"extattr_set_fd|extattr_get_fd|extattr_delete_fd|extattr_list_fd|"
        r"extattr_set_link|extattr_get_link|extattr_delete_link|extattr_list_link|"
        r"lsetxattr|fsetxattr|setxattr|listxattr|removexattr|getxattr|"
        r"pthread_yield|sched_yield|sched_setattr|sched_getattr|cpuset_setaffinity|cpuset_getaffinity|sched_get_priority_max|sched_get_priority_min|sched_setaffinity|"
        r"sched_getaffinity|"
        r"sched_setscheduler|sched_getscheduler|"
        r"sched_setparam|sched_getparam|"
        r"lio_listio|aio_suspend|aio_return|aio_error|aio_write|aio_read|"
        r"io_uring_register|io_uring_setup|io_uring_enter|"
        r"capset|capget|statx|"
        r"pidfd_send_signal|pidfd_getfd|pidfd_open|"
        r"fanotify_mark|fanotify_init|seccomp|"
        r"getgrnam|getgrgid|getspnam|"
        r"posix_fallocate|fallocate|close_range|closefrom|"
        r"lsm_get_self_attr|lsm_set_self_attr|lsm_list_modules|"
        r"landlock_create_ruleset|landlock_add_rule|landlock_restrict_self|"
        r"rtprio_thread|rtprio|getpriority|setpriority|signalfd|sigaction|pthread_sigmask|sigprocmask|sigsuspend|sigaltstack|"
        r"sigwaitinfo|sigtimedwait|sigpending|sigwait|"
        r"bpf|userfaultfd|getpass|getgrouplist|getgroups|initgroups|setgroups|"
        r"unshare|setns|clone|openat2|sendmmsg|recvmmsg|"
        r"name_to_handle_at|open_by_handle_at|process_madvise|"
        r"personality|quotactl|"
        r"pivot_root|membarrier|pkey_alloc|"
        r"getmntinfo|getvfsbyname|getfsent|setfsent|endfsent|getfsstat|statfs|syncfs|prlimit64|prlimit|"
        r"migrate_pages|move_pages|process_vm_readv|perf_event_open|"
        r"clone3|kcmp|keyctl|"
        r"open_tree_attr|fsopen|fsmount|open_tree|move_mount|fspick|fsconfig|"
        r"process_mrelease|memfd_secret|"
        r"ioprio_set|ioprio_get|mq_open|shmget|_umtx_op|futex_waitv|futex_wake|futex_wait|futex_requeue|futex|ntp_adjtime|ntp_gettime|adjtimex|adjtime|revoke|fflagstostr|strtofflags|strmode|strtonum|reallocarray|reallocf|getprogname|setprogname|setproctitle|daemon|"
        r"auditon|getaudit|setaudit|auditctl|"
        r"kinfo_getproc|kinfo_getfile|kinfo_getvmmap|kvm_openfiles|kvm_open|kvm_getprocs|kvm_close|kvm_nlist|"
        r"uuidgen|setfib|kenv|"
        r"mac_set_proc|mac_get_proc|mac_set_fd|mac_get_fd|mac_set_file|mac_get_file|"
        r"sethostname|"
        r"reboot|swapon|swapoff|acct|ioperm|iopl|mincore|rseq|"
        r"timer_create|semget|msgget|syslog|klogctl|mount_setattr|getcpu|"
        r"init_module|finit_module|delete_module|"
        r"kexec_load|kexec_file_load|quotactl_fd|"
        r"pkey_free|pkey_mprotect|tgkill|add_key|"
        r"semctl|msgctl|shmctl|timer_settime|getdomainname|setdomainname|"
        r"io_submit|io_getevents|"
        r"io_setup|io_destroy|io_cancel|io_pgetevents|"
        r"request_key|tkill|"
        r"timer_delete|timer_gettime|timer_getoverrun|"
        r"mq_unlink|mq_timedsend|mq_timedreceive|mq_notify|mq_getsetattr|"
        r"shmat|shmdt|semop|semtimedop|msgsnd|msgrcv|"
        r"sync_file_range|remap_file_pages|msync|mremap|socketpair|sysinfo|"
        r"gettid|setitimer|getitimer|nice|"
        r"getdirentries|getdents64|getdents|utimensat|futimens|utimes|linkat|"
        r"set_mempolicy_home_node|mbind|set_mempolicy|get_mempolicy|"
        r"cachestat|map_shadow_stack|"
        r"throw_with_nested|rethrow_if_nested|"
        r"__atomic_load|__atomic_store|"
        r"__sync_fetch_and_add|__sync_bool_compare_and_swap|"
        r"wcscpy|wcscat|wcsncpy|wcsncat)\s*\(",
        body,
    ):
        return (
            f"libc buffer/tempfile unencoded: unconstrained call is not a "
            f"proof of the buffer ({engine})"
        )
    if _re_search(r"(?:,|\()\s*&", body):
        return (
            f"address-of unencoded: {engine} is not a pointer-value model"
        )
    if _re_search(
        r"\b(?:char|unsigned\s+char)\s+[A-Za-z_]\w*\s*\[\s*\]\s*=",
        body,
    ):
        return (
            f"array string-init unencoded: {engine} is not a string-array model"
        )
    if _re_search(r"\b(?:asm|__asm__|__asm)\b", body):
        return f"inline asm unencoded: {engine} is not an assembly model"
    if _re_search(r"\b_Generic\b", body):
        return f"_Generic unencoded: {engine} is not a type-generic model"
    if _re_search(r"\(\s*\{", body):
        return f"GNU statement expression unencoded: {engine} is not a GNU-C model"
    if _re_search(r"\b(?:try|catch)\b", body):
        return f"C++ try/catch unencoded: {engine} is not an exception model"
    if _re_search(r"\boffsetof\b", body):
        return f"offsetof unencoded: {engine} is not a struct-layout model"
    if _re_search(r"\b(?:new|delete)\b", body):
        return f"C++ new/delete unencoded: {engine} is not a heap-lifetime model"
    if _re_search(r"\brestrict\b", body):
        return (
            f"restrict unencoded: {engine} is not a restrict-qualifier model"
        )
    # Decl qualifier only — not `__asm__ volatile` and not the word in a string.
    if _re_search(
        r"(?m)(?:^|[;{(])\s*(?:(?:static|extern|auto|register|const)\s+)*"
        r"(?:_Atomic|volatile)\b"
        r"|\b_Atomic\s*\("
        r"|\b(?:int|unsigned|signed|long|short|char|uint\w*|int\w*|"
        r"_Bool|bool|void|float|double)\s+(?:const\s+)?volatile\b",
        body,
    ):
        return f"volatile/_Atomic unencoded: {engine} is not a memory-model"
    if _re_search(r"\b(?:pthread_mutex_t|mtx_t)\b", body):
        return f"mutex object unencoded: {engine} is not a lock model"
    # Local `const T x` — missing const model. Parameter `const int` is
    # stripped in cparse and is not this case. Casts `(const int)` start
    # with `(` so they are not this match. `for (const int i = 0;` is.
    if _re_search(
        r"(?m)(?:^|[;{])\s*(?:(?:static|extern|auto|register)\s+)*const\s+"
        r"|\b(?:int|unsigned|signed|long|short|char|uint\w*|int\w*|"
        r"_Bool|bool|void|float|double|size_t)\s+const\s+[A-Za-z_]"
        r"|\bfor\s*\(\s*(?:(?:static|extern|auto|register)\s+)*const\s+",
        body,
    ):
        return f"const local unencoded: {engine} is not a const model"
    # `register int x` / `auto int x` / C++ `auto x = 1` — missing model.
    if _re_search(r"(?m)(?:^|[;{(])\s*(?:register|auto)\b", body):
        return (
            f"register/auto unencoded: {engine} is not a storage-class "
            "or auto-type model"
        )
    if _re_search(r"\b__auto_type\b", body):
        return (
            f"__auto_type unencoded: {engine} is not a storage-class "
            "or auto-type model"
        )
    # Function-local `static int x` / `extern int x` — missing duration.
    if _re_search(
        r"(?m)(?:^|[;{])\s*(?:static|extern)\s+" + _DECL_TYPE + r"\s+[A-Za-z_]\w*",
        body,
    ):
        return (
            f"static/extern local unencoded: {engine} is not a "
            "storage-duration model"
        )
    # Anonymous `enum { RED = 1 } e;` — missing layout, not a proof.
    if _re_search(r"enum\s*\{[^}]+\}\s+[A-Za-z_]\w*", body):
        return (
            f"anonymous enum local unencoded: {engine} is not a layout model"
        )
    if _re_search(r"(?m)(?:^|[;{])\s*(?:_Alignas|alignas)\s*\(", body):
        return f"_Alignas unencoded: {engine} is not an alignment model"
    if _re_search(
        r"(?m)(?:^|[;{])\s*(?:(?:static|extern|auto|register|const)\s+)*"
        + _DECL_TYPE + r"\s+[A-Za-z_]\w*\s*=\s*\([^)]*\)\s*\{",
        body,
    ):
        return (
            f"compound literal unencoded: {engine} is not a "
            "compound-literal model"
        )
    # Local `struct S s;` / `enum E e;` / anonymous `struct { int x; } s;`
    # — missing layout, not trailing ERROR. One `(?m)` only: a second
    # inline flag in the middle of the pattern is a Python re error.
    if _re_search(
        r"(?m)(?:^|[;{])\s*(?:(?:static|extern|auto|register|const)\s+)*"
        r"(?:struct|union)(?:\s+[A-Za-z_]\w*)?\s*\{"
        r"|(?:^|[;{])\s*(?:(?:static|extern|auto|register|const)\s+)*"
        r"(?:struct|union|enum)\s+[A-Za-z_]\w*\s+[A-Za-z_]\w*",
        body,
    ):
        return f"struct/union local unencoded: {engine} is not a layout model"
    for m in re.finditer(
        r"(?m)(?:^|[;{])\s*(?:(?:static|extern|auto|register)\s+)*"
        r"([A-Za-z_]\w*)\s+[A-Za-z_]\w*\s*(?:[=;\[])",
        body,
    ):
        if m.group(1) not in _STMT_START_WORDS:
            return (
                f"unknown typedef local unencoded: {engine} is not a layout model"
            )
    return None


_STMT_START_WORDS = {
    "if", "for", "while", "switch", "return", "sizeof", "typeof",
    "else", "do", "case", "default", "goto", "break", "continue",
    "assert", "throw", "try", "catch", "asm", "__asm__", "__asm",
    "typedef", "static", "extern", "auto", "register",
    "int", "unsigned", "signed", "long", "short", "char",
    "uint32_t", "int32_t", "uint64_t", "int64_t", "size_t",
    "void", "float", "double", "_Bool", "bool",
    "const", "volatile", "_Atomic", "struct", "union", "enum",
}


def unencoded_layout_prefix(text: str) -> str | None:
    """Named ParseFail when `_stmt` would stop at `{` inside a layout decl.

    `struct { int x; } s;` is missing layout, not a generic no-semicolon ERROR.
    `enum { RED = 1 } e;` and compound-literal inits hit the same `{` rule.
    Generic trailing / no-semicolon / case-outside-switch stay ERROR.
    """
    s = (text or "").lstrip()
    if _re_match(r"(?:struct|union)\s*(?:[A-Za-z_]\w*\s*)?\{", s):
        return "struct unencoded"
    if _re_match(r"enum\s*\{", s):
        return "anon enum unencoded"
    if _re_match(r"(?:static|extern)\b", s):
        return "storage-duration unencoded"
    if _re_match(r"(?:_Alignas|alignas)\s*\(", s):
        return "alignas unencoded"
    if _re_match(r"__auto_type\b", s):
        return "storage-class unencoded"
    if _re_match(r"(?:_Thread_local|thread_local)\b", s):
        return "thread-local unencoded"
    if _re_match(r"(?:_Complex|_Imaginary)\b", s):
        return "complex unencoded"
    if _re_match(r"(?:_Decimal32|_Decimal64|_Decimal128)\b", s):
        return "decimal-float unencoded"
    if _re_match(r"(?:_Float16|_Float32|_Float64|__fp16)\b", s):
        return "extra-IEEE unencoded"
    if _re_match(r"(?:typeof_unqual|__typeof_unqual__)\s*\(", s):
        return "typeof_unqual unencoded"
    if _re_match(r"(?:typeof|__typeof__)\s*\(", s):
        return "typeof unencoded"
    if _re_match(r"constexpr\b", s):
        return "constexpr unencoded"
    if _re_match(r"\[\[\s*assume\s*\(", s):
        return "assume unencoded"
    if _re_match(
        r"(?:int|unsigned(?:\s+int)?|long|short|char|uint32_t|int32_t|size_t)"
        r"\s+\w+\s*=\s*\([^)]*\)\s*\{",
        s,
    ):
        return "compound-lit unencoded"
    return None


def unencoded_layout_stmt(stmt: str) -> str | None:
    """Named ParseFail for const/struct/unknown-typedef locals.

    Generic trailing / no-semicolon / case-outside-switch stay ERROR.
    """
    s = (stmt or "").strip()
    if not s:
        return None
    if _re_search(r"\b(?:__int128(?:_t)?|_BitInt)\b", s):
        return "128-bit unencoded"
    if _re_search(r"\b(?:_Decimal32|_Decimal64|_Decimal128)\b", s):
        return "decimal-float unencoded"
    if _re_search(r"\b(?:_Float16|_Float32|_Float64|__fp16)\b", s):
        return "extra-IEEE unencoded"
    if _starts_kw(s, "constexpr"):
        return "constexpr unencoded"
    if _re_search(r"\[\[\s*assume\s*\(", s):
        return "assume unencoded"
    if _re_search(r"__attribute__\s*\(\s*\(\s*cleanup", s):
        return "cleanup unencoded"
    if _re_search(
        r"__attribute__\s*\(\s*\(\s*(?:__)?vector_size|\b__vector_size\b",
        s,
    ):
        return "vector_size unencoded"
    if _starts_kw(s, "const"):
        return "const unencoded"
    if _starts_kw(s, "register") or _starts_kw(s, "auto"):
        return "storage-class unencoded"
    if _re_match(r"__auto_type\b", s):
        return "storage-class unencoded"
    if _starts_kw(s, "static") or _starts_kw(s, "extern"):
        return "storage-duration unencoded"
    if _re_match(r"(?:_Thread_local|thread_local)\b", s):
        return "thread-local unencoded"
    if _re_match(r"(?:_Complex|_Imaginary)\b", s):
        return "complex unencoded"
    if _re_match(r"(?:typeof_unqual|__typeof_unqual__)\s*\(", s):
        return "typeof_unqual unencoded"
    if _re_match(r"(?:typeof|__typeof__)\s*\(", s):
        return "typeof unencoded"
    if _is_nested_function(s):
        return "nested function unencoded"
    if _re_match(r"(?:_Alignas|alignas)\s*\(", s):
        return "alignas unencoded"
    if _re_search(r"=\s*\([^)]*\)\s*\{", s):
        return "compound-lit unencoded"
    if _starts_kw(s, "struct") or _starts_kw(s, "union"):
        if _re_match(r"(?:struct|union)\s*\{", s):
            return "struct unencoded"
        if _re_match(r"(?:struct|union)\s+[A-Za-z_]\w*\s*\{", s):
            return "struct unencoded"
        if _re_match(r"(?:struct|union)\s+[A-Za-z_]\w*\s+[A-Za-z_]\w*", s):
            return "struct unencoded"
        return None
    if _starts_kw(s, "enum"):
        if _re_match(r"enum\s*\{", s):
            return "anon enum unencoded"
        if _re_match(r"enum\s+[A-Za-z_]\w*\s+[A-Za-z_]\w*", s):
            return "struct unencoded"
        return None
    m = _re_match(
        r"([A-Za-z_]\w*)\s+[A-Za-z_]\w*\s*(?:[=;\[]|$)",
        s,
    )
    if not m:
        return None
    if m.group(1) in _STMT_START_WORDS:
        return None
    return "typedef local unencoded"


def harness_for_parsefail(err: str, engine: str) -> str | None:
    """Map a frontend ParseFail to NEEDS-HARNESS. A goto the encoder does not
    model (unstructured) is NEEDS-HARNESS too; an unmapped failure is ERROR."""
    if err.startswith("UNENCODED: "):
        return f"{err} (not modelled by {engine}): not a proof"
    low = (err or "").lower()
    if "vla" in low:
        if engine == "bitvector BMC":
            return "VLA size is a missing bound, not a closed BMC proof"
        return f"VLA size is a missing bound, not a closed {engine} run"
    if "throw unencoded" in low:
        return f"C++ throw unencoded: {engine} is not an exception model"
    if "asm unencoded" in low:
        return f"inline asm unencoded: {engine} is not an assembly model"
    if "try unencoded" in low:
        return f"C++ try/catch unencoded: {engine} is not an exception model"
    if "statement-expr unencoded" in low:
        return f"GNU statement expression unencoded: {engine} is not a GNU-C model"
    if "_generic unencoded" in low:
        return f"_Generic unencoded: {engine} is not a type-generic model"
    if "offsetof unencoded" in low:
        return f"offsetof unencoded: {engine} is not a struct-layout model"
    if "const unencoded" in low:
        return f"const local unencoded: {engine} is not a const model"
    if "storage-class unencoded" in low:
        return (
            f"register/auto unencoded: {engine} is not a storage-class "
            "or auto-type model"
        )
    if "storage-duration unencoded" in low:
        return (
            f"static/extern local unencoded: {engine} is not a "
            "storage-duration model"
        )
    if "anon enum unencoded" in low:
        return (
            f"anonymous enum local unencoded: {engine} is not a layout model"
        )
    if "alignas unencoded" in low:
        return f"_Alignas unencoded: {engine} is not an alignment model"
    if "compound-lit unencoded" in low:
        return (
            f"compound literal unencoded: {engine} is not a "
            "compound-literal model"
        )
    if "struct unencoded" in low:
        return f"struct/union local unencoded: {engine} is not a layout model"
    if "typedef local unencoded" in low:
        return f"unknown typedef local unencoded: {engine} is not a layout model"
    if "unstructured goto unencoded" in low:
        return (
            f"{err} ({engine} models only a goto out to a later statement and a "
            "backward goto that forms a loop): not a proof"
        )
    if "computed goto unencoded" in low:
        return (
            f"computed goto unencoded: {engine} is not a computed-goto model"
        )
    if "label-address unencoded" in low:
        return (
            f"label-address unencoded: {engine} is not a label-address model"
        )
    if "dynamic_cast unencoded" in low:
        return f"C++ dynamic_cast unencoded: {engine} is not an RTTI model"
    if "type_identity" in low:
        return (
            f"C++ type_identity unencoded: {engine} is not a "
            "type_identity model"
        )
    if "typeid unencoded" in low:
        return f"C++ typeid unencoded: {engine} is not an RTTI model"
    if "reinterpret_cast unencoded" in low:
        return (
            f"C++ reinterpret_cast unencoded: {engine} is not a type-pun model"
        )
    if "coroutine unencoded" in low:
        return (
            f"C++ coroutine unencoded: {engine} is not a coroutine model"
        )
    if (
        "packed layout unencoded" in low
        or "packed unencoded" in low
        or "pragma pack" in low
    ):
        return (
            f"packed layout unencoded: {engine} is not a packed-layout model"
        )
    if "wide character unencoded" in low or "wide-char unencoded" in low:
        return (
            f"wide character unencoded: {engine} is not a wide-char model"
        )
    if "thread-local unencoded" in low:
        return f"thread-local unencoded: {engine} is not a TLS model"
    if "complex unencoded" in low:
        return (
            f"complex unencoded: {engine} is not a complex arithmetic model"
        )
    if "typeof_unqual unencoded" in low:
        return f"typeof_unqual unencoded: {engine} is not a typeof model"
    if "typeof unencoded" in low:
        return f"typeof unencoded: {engine} is not a typeof model"
    if "nested function unencoded" in low:
        return (
            f"nested function unencoded: {engine} is not a "
            "nested-function model"
        )
    if "designated init unencoded" in low:
        return (
            f"designated init unencoded: {engine} is not a "
            "designated-init model"
        )
    if "alignof unencoded" in low:
        return f"alignof unencoded: {engine} is not an alignment model"
    if "va_arg unencoded" in low:
        return f"va_arg unencoded: {engine} is not a variadic model"
    if "range-for unencoded" in low:
        return (
            f"C++ range-for unencoded: {engine} is not a range-for model"
        )
    if "lambda unencoded" in low:
        return f"C++ lambda unencoded: {engine} is not a lambda model"
    if "_static_assert unencoded" in low:
        return (
            f"_Static_assert unencoded: {engine} is not a "
            "static-assert model"
        )
    if "if constexpr unencoded" in low:
        return (
            f"if constexpr unencoded: {engine} is not a compile-time-if model"
        )
    if "constexpr unencoded" in low:
        return (
            f"constexpr unencoded: {engine} is not a constexpr model"
        )
    if "case-range unencoded" in low:
        return (
            f"case-range unencoded: {engine} is not a case-range model"
        )
    if "128-bit unencoded" in low or "__int128" in low or "_bitint" in low:
        return f"128-bit unencoded: {engine} is not a 128-bit model"
    if "bit_cast unencoded" in low or "bit_cast" in low:
        return f"bit_cast unencoded: {engine} is not a type-pun model"
    if "std::thread unencoded" in low or "thread-lifetime unencoded" in low:
        return (
            f"C++ std::thread unencoded: {engine} is not a "
            "thread-lifetime model"
        )
    if "std::optional unencoded" in low:
        return (
            f"C++ std::optional unencoded: {engine} is not an optional model"
        )
    if "std::variant unencoded" in low:
        return (
            f"C++ std::variant unencoded: {engine} is not a variant model"
        )
    if "std::span unencoded" in low:
        return (
            f"C++ std::span unencoded: {engine} is not a span-lifetime model"
        )
    if "inplace_vector" in low:
        return (
            f"C++ inplace_vector unencoded: {engine} is not an "
            "inplace_vector model"
        )
    if "std::vector unencoded" in low or "vector unencoded" in low:
        return (
            f"C++ std::vector unencoded: {engine} is not a container model"
        )
    if "catch-all unencoded" in low:
        return (
            f"C++ catch-all unencoded: {engine} is not an exception model"
        )
    if "throw-new unencoded" in low:
        return (
            f"C++ throw-new unencoded: {engine} is not an exception model"
        )
    if _re_search(r"\bexecveat\b", low):
        return (
            f"execveat unencoded: unconstrained execveat is not a "
            f"proof ({engine})"
        )
    if _re_search(r"\bpthread_atfork\b", low):
        return (
            f"pthread_atfork unencoded: unconstrained pthread_atfork "
            f"is not a proof ({engine})"
        )
    if _re_search(r"\bpledge\b", low):
        return (
            f"pledge unencoded: unconstrained pledge is not a "
            f"proof ({engine})"
        )
    if _re_search(r"\bmac_(?:set|get)_(?:proc|fd|file)\b", low):
        return (
            f"mac_set_proc unencoded: unconstrained mac_set_proc is not a "
            f"proof ({engine})"
        )
    if _re_search(r"\bcap_getmode\b", low):
        return (
            f"cap_getmode unencoded: unconstrained cap_getmode is not a "
            f"proof ({engine})"
        )
    if _re_search(r"\bcap_getrights\b", low):
        return (
            f"cap_getrights unencoded: unconstrained cap_getrights is not a "
            f"proof ({engine})"
        )
    if _re_search(r"\bcap_enter\b", low):
        return (
            f"cap_enter unencoded: unconstrained cap_enter is not a "
            f"proof ({engine})"
        )
    if _re_search(r"\bcap_sandboxed\b", low):
        return (
            f"cap_sandboxed unencoded: unconstrained cap_sandboxed is not a "
            f"proof ({engine})"
        )
    if _re_search(r"\bcap_rights_(?:limit|get)\b", low):
        return (
            f"cap_rights unencoded: unconstrained cap_rights_limit "
            f"is not a proof ({engine})"
        )
    if _re_search(r"\bcap_(?:fcntls|ioctls)_limit\b", low):
        return (
            f"cap_fcntls unencoded: unconstrained cap_fcntls_limit "
            f"is not a proof ({engine})"
        )
    if _re_search(r"\bunveil\b", low):
        return (
            f"unveil unencoded: unconstrained unveil is not a "
            f"proof ({engine})"
        )
    if _re_search(r"\bsysctl(?:byname)?\b", low):
        return (
            f"sysctl unencoded: unconstrained sysctl is not a "
            f"proof ({engine})"
        )
    if _re_search(r"\bkqueue\b", low):
        return (
            f"kqueue unencoded: unconstrained kqueue is not a "
            f"proof ({engine})"
        )
    if _re_search(r"\bkevent\b", low):
        return (
            f"kevent unencoded: unconstrained kevent is not a "
            f"proof ({engine})"
        )
    if _re_search(r"\bpause\b", low):
        return (
            f"pause unencoded: unconstrained pause is not a "
            f"proof ({engine})"
        )
    if _re_search(r"\b(?:get|set|swap|make)context\b", low):
        return (
            f"getcontext unencoded: unconstrained getcontext is not a "
            f"proof ({engine})"
        )
    if _re_search(
        r"\bpthread_attr_(?:init|destroy|setstacksize|setstack|"
        r"setdetachstate|getstacksize|getstack|getdetachstate)\b",
        low,
    ) or "pthread_attr unencoded" in low:
        return (
            f"pthread_attr unencoded: unconstrained pthread_attr_init "
            f"is not a proof ({engine})"
        )
    if "process spawn unencoded" in low:
        return (
            f"process spawn unencoded: unconstrained fork/exec is not a "
            f"proof ({engine})"
        )
    if _re_search(r"\bpdfork\b", low):
        return (
            f"pdfork unencoded: unconstrained pdfork is not a "
            f"proof ({engine})"
        )
    if _re_search(r"\brfork\b", low):
        return (
            f"rfork unencoded: unconstrained rfork is not a "
            f"proof ({engine})"
        )
    if _re_search(r"\bminherit\b", low):
        return (
            f"minherit unencoded: unconstrained minherit is not a "
            f"proof ({engine})"
        )
    if _re_search(r"\bnfssvc\b", low):
        return (
            f"nfssvc unencoded: unconstrained nfssvc is not a "
            f"proof ({engine})"
        )
    if _re_search(r"\bsysarch\b", low):
        return (
            f"sysarch unencoded: unconstrained sysarch is not a "
            f"proof ({engine})"
        )
    if _re_search(r"\bgetbootfile\b", low):
        return (
            f"getbootfile unencoded: unconstrained getbootfile is not a "
            f"proof ({engine})"
        )
    if _re_search(r"\bdevname(?:_r)?\b", low):
        return (
            f"devname unencoded: unconstrained devname is not a "
            f"proof ({engine})"
        )
    if _re_search(r"\bsbrk\b", low):
        return (
            f"sbrk unencoded: unconstrained sbrk is not a "
            f"proof ({engine})"
        )
    if _re_search(r"\bbrk\s*\(", low) or _re_search(r"\bbrk\b", low):
        return (
            f"brk unencoded: unconstrained brk is not a "
            f"proof ({engine})"
        )
    if "mmap unencoded" in low:
        return f"mmap unencoded: {engine} is not a VM model"
    if "wide-string copy unencoded" in low or "wcscpy unencoded" in low:
        return (
            f"wide-string copy unencoded: unconstrained wcscpy is not a "
            f"proof of the buffer ({engine})"
        )
    if _re_search(r"\b(?:dlfunc|dlvsym)\b", low) or "dlfunc unencoded" in low:
        return (
            f"dlfunc unencoded: unconstrained dlfunc is not a "
            f"proof ({engine})"
        )
    if "dlopen unencoded" in low or "dlsym unencoded" in low or "dlclose unencoded" in low:
        return f"dlopen unencoded: {engine} is not a dynamic-loader model"
    if _re_search(r"\bmod(?:find|stat|next|fnext)\b", low):
        return (
            f"modfind unencoded: unconstrained modfind is not a "
            f"proof ({engine})"
        )
    if _re_search(r"\bkld(?:firstmod|nextmod)\b", low):
        return (
            f"kldfirstmod unencoded: unconstrained kldfirstmod is not a "
            f"proof ({engine})"
        )
    if _re_search(r"\bkld_(?:isloaded|load)\b", low):
        return (
            f"kld_load unencoded: unconstrained kld_load is not a "
            f"proof ({engine})"
        )
    if _re_search(r"\bkld(?:load|unload|find|sym|stat)\b", low):
        return (
            f"kldload unencoded: unconstrained kldload is not a "
            f"proof ({engine})"
        )
    if "clz unencoded" in low or "ctz unencoded" in low or "__builtin_clz" in low:
        return (
            f"clz unencoded: unconstrained __builtin_clz is UB on 0 "
            f"and is not a proof ({engine})"
        )
    if "nullptr unencoded" in low:
        return f"C23 nullptr unencoded: {engine} is not a nullptr model"
    if "launder unencoded" in low:
        return f"C++ launder unencoded: {engine} is not a lifetime model"
    if "fold unencoded" in low:
        return (
            f"C++ fold unencoded: {engine} is not a fold-expression model"
        )
    if "decimal-float unencoded" in low or "decimal float unencoded" in low:
        return (
            f"decimal float unencoded: {engine} is not a decimal-float model"
        )
    if "extra-ieee unencoded" in low or "_float16" in low or "__fp16" in low:
        return (
            f"extra-IEEE float unencoded: {engine} is not an extra-IEEE model"
        )
    if "start_lifetime_as" in low:
        return (
            f"C++ start_lifetime_as unencoded: {engine} is not a lifetime model"
        )
    if "shared_from_this unencoded" in low or "shared-lifetime unencoded" in low:
        return (
            f"C++ shared_from_this unencoded: {engine} is not a "
            "shared-lifetime model"
        )
    if "concepts unencoded" in low or "concept unencoded" in low:
        return (
            f"C++ concepts unencoded: {engine} is not a concepts model"
        )
    if "choose_expr unencoded" in low or "__builtin_choose_expr" in low:
        return (
            f"choose_expr unencoded: {engine} is not a "
            "__builtin_choose_expr model"
        )
    if _re_search(r"\b__builtin_unreachable\b", low):
        return (
            f"__builtin_unreachable unencoded: unconstrained "
            f"__builtin_unreachable is not a proof ({engine})"
        )
    if _re_search(r"\b__builtin_trap\b", low):
        return (
            f"__builtin_trap unencoded: unconstrained "
            f"__builtin_trap is not a proof ({engine})"
        )
    if _re_search(r"\bstd\s*::\s*unreachable\b", low):
        return (
            f"C++ std::unreachable unencoded: {engine} is not an "
            "unreachable model"
        )
    if "restrict unencoded" in low:
        return (
            f"restrict unencoded: {engine} is not a restrict-qualifier model"
        )
    if "listen unencoded" in low:
        return (
            f"listen unencoded: unconstrained listen is not a "
            f"proof ({engine})"
        )
    if "connect unencoded" in low:
        return (
            f"connect unencoded: unconstrained connect is not a "
            f"proof ({engine})"
        )
    if "pipe unencoded" in low:
        return f"pipe unencoded: {engine} is not a pipe model"
    if "dup unencoded" in low:
        return f"dup unencoded: {engine} is not an fd model"
    if "fcntl unencoded" in low:
        return (
            f"fcntl unencoded: unconstrained fcntl is not a "
            f"proof ({engine})"
        )
    if _re_search(r"\bpd(?:getpid|wait4)\b", low):
        return (
            f"pdgetpid unencoded: unconstrained pdgetpid is not a "
            f"proof ({engine})"
        )
    if _re_search(r"\b(?:wait4|wait3)\b", low):
        return (
            f"wait4 unencoded: unconstrained wait4 is not a "
            f"proof ({engine})"
        )
    if _re_search(r"\bwait6\b", low):
        return (
            f"wait6 unencoded: unconstrained wait6 is not a "
            f"proof ({engine})"
        )
    if _re_search(r"\bsem_trywait\b", low) or _re_search(r"\bsem_getvalue\b", low):
        return (
            f"sem_trywait unencoded: unconstrained sem_trywait is not a "
            f"proof ({engine})"
        )
    if "wait unencoded" in low:
        return (
            f"wait unencoded: unconstrained wait is not a "
            f"proof ({engine})"
        )
    if _re_search(r"\bunlinkat\b", low):
        return (
            f"unlinkat unencoded: unconstrained unlinkat is not a "
            f"proof ({engine})"
        )
    if _re_search(r"\bunlink\b", low):
        return (
            f"unlink unencoded: unconstrained unlink is not a "
            f"proof ({engine})"
        )
    if _re_search(r"\bmknodat\b", low):
        return (
            f"mknodat unencoded: unconstrained mknodat is not a "
            f"proof ({engine})"
        )
    if "unexpected<" in low or "unexpected <" in low or "unexpected unencoded" in low:
        return (
            f"C++ unexpected unencoded: {engine} is not an unexpected model"
        )
    if "expected unencoded" in low or "std::expected" in low:
        return (
            f"C++ std::expected unencoded: {engine} is not an expected model"
        )
    if "format_to" in low:
        return (
            f"C++ format_to unencoded: {engine} is not a format_to model"
        )
    if "format unencoded" in low or "std::format" in low:
        return (
            f"C++ std::format unencoded: {engine} is not a format model"
        )
    if "spaceship unencoded" in low:
        return (
            f"C++ spaceship unencoded: {engine} is not a "
            "three-way comparison model"
        )
    if "cleanup unencoded" in low:
        return (
            f"cleanup attribute unencoded: {engine} is not a cleanup model"
        )
    if "jthread unencoded" in low:
        return (
            f"C++ std::jthread unencoded: {engine} is not a jthread model"
        )
    if "async unencoded" in low or "std::async" in low or "future unencoded" in low or "promise unencoded" in low:
        return (
            f"C++ std::async unencoded: {engine} is not a future model"
        )
    if "function_ref" in low:
        return (
            f"C++ function_ref unencoded: {engine} is not a "
            "function_ref model"
        )
    if (
        "move_only_function" in low
        or "copyable_function" in low
    ):
        return (
            f"C++ move_only_function unencoded: {engine} is not a "
            "move_only_function model"
        )
    if _re_search(r"\breference_wrapper\b", low) or _re_search(
        r"\bstd\s*::\s*(?:cref|ref)\s*\(", low
    ):
        return (
            f"C++ reference_wrapper unencoded: {engine} is not a "
            "reference_wrapper model"
        )
    if "function unencoded" in low or "std::function" in low:
        return (
            f"C++ std::function unencoded: {engine} is not a "
            "type-erased callable model"
        )
    if "mdspan unencoded" in low or "std::mdspan" in low:
        return (
            f"C++ std::mdspan unencoded: {engine} is not an mdspan model"
        )
    if "std mutex unencoded" in low or "std::mutex" in low:
        return (
            f"C++ std::mutex unencoded: {engine} is not a C++ mutex model"
        )
    if "vector_size unencoded" in low:
        return (
            f"vector_size unencoded: {engine} is not a SIMD vector model"
        )
    if _re_search(r"\bppoll\b", low):
        return (
            f"ppoll unencoded: unconstrained ppoll is not a "
            f"proof ({engine})"
        )
    if _re_search(r"\bepoll_pwait(?:2)?\b", low):
        return (
            f"epoll_pwait unencoded: unconstrained epoll_pwait is not a "
            f"proof ({engine})"
        )
    if _re_search(r"\bepoll_create(?:1)?\b", low):
        return (
            f"epoll_create unencoded: unconstrained epoll_create is not a "
            f"proof ({engine})"
        )
    if "select unencoded" in low:
        return (
            f"select unencoded: unconstrained I/O multiplex is not a "
            f"proof ({engine})"
        )
    if "send unencoded" in low:
        return (
            f"send unencoded: unconstrained socket I/O is not a "
            f"proof ({engine})"
        )
    if "kill_dependency" in low:
        return (
            f"C++ kill_dependency unencoded: {engine} is not a "
            "kill_dependency model"
        )
    if _re_search(r"\brt_(?:tgsigqueueinfo|sigqueueinfo)\b", low):
        return (
            f"rt_sigqueueinfo unencoded: unconstrained rt_sigqueueinfo "
            f"is not a proof ({engine})"
        )
    if _re_search(r"\bsigqueue\b", low):
        return (
            f"sigqueue unencoded: unconstrained sigqueue is not a "
            f"proof ({engine})"
        )
    if _re_search(r"\bthr_(?:new|kill2|kill|self|exit|suspend|wake)\b", low):
        return (
            f"thr_kill unencoded: unconstrained thr_kill is not a "
            f"proof ({engine})"
        )
    if _re_search(r"\bpthread_kill\b", low):
        return (
            f"pthread_kill unencoded: unconstrained pthread_kill is not a "
            f"proof ({engine})"
        )
    if "kill unencoded" in low:
        return (
            f"kill unencoded: unconstrained signal delivery is not a "
            f"proof ({engine})"
        )
    if "addrinfo unencoded" in low:
        return (
            f"addrinfo unencoded: unconstrained DNS is not a "
            f"proof ({engine})"
        )
    if _re_search(r"\bpthread_cancel\b", low):
        return (
            f"pthread_cancel unencoded: unconstrained pthread_cancel "
            f"is not a proof ({engine})"
        )
    if _re_search(
        r"\bpthread_(?:key_create|key_delete|setspecific|getspecific)\b",
        low,
    ) or "pthread_key_create unencoded" in low:
        return (
            f"pthread_key_create unencoded: unconstrained "
            f"pthread_key_create is not a proof ({engine})"
        )
    if "pthread join unencoded" in low or _re_search(
        r"\b(?:pthread_join|pthread_detach)\b", low
    ):
        return (
            f"pthread join unencoded: unconstrained thread join is not a "
            f"proof ({engine})"
        )
    if _re_search(
        r"\bpthread_spin_(?:try)?(?:lock|unlock|init|destroy)\b",
        low,
    ) or "pthread_spin unencoded" in low:
        return (
            f"pthread_spin unencoded: unconstrained pthread_spin_lock "
            f"is not a proof ({engine})"
        )
    if _re_search(
        r"\bpthread_rwlock_(?:try|timed)?(?:rdlock|wrlock|unlock|init|destroy)\b",
        low,
    ) or "pthread_rwlock unencoded" in low:
        return (
            f"pthread_rwlock unencoded: unconstrained pthread_rwlock "
            f"is not a proof ({engine})"
        )
    if _re_search(
        r"\bpthread_cond_(?:timedwait|wait|signal|broadcast|init|destroy)\b",
        low,
    ) or "pthread_cond unencoded" in low:
        return (
            f"pthread_cond unencoded: unconstrained pthread_cond_wait "
            f"is not a proof ({engine})"
        )
    if "sem unencoded" in low:
        return f"sem unencoded: {engine} is not a semaphore model"
    if _re_search(r"\bksem_", low):
        return (
            f"ksem_open unencoded: unconstrained ksem_open is not a "
            f"proof ({engine})"
        )
    if _re_search(r"\bsem_(?:open|close|unlink)\b", low):
        return (
            f"sem_open unencoded: unconstrained sem_open is not a "
            f"proof ({engine})"
        )
    if _re_search(r"\bsem_timedwait\b", low):
        return (
            f"sem_timedwait unencoded: unconstrained sem_timedwait is not a "
            f"proof ({engine})"
        )
    if "openat unencoded" in low:
        return (
            f"openat unencoded: unconstrained openat is not a "
            f"proof ({engine})"
        )
    if "flock unencoded" in low:
        return (
            f"flock unencoded: unconstrained flock is not a "
            f"proof ({engine})"
        )
    if "aligned_alloc unencoded" in low or "posix_memalign" in low:
        return (
            f"aligned_alloc unencoded: {engine} is not an aligned-alloc model"
        )
    if _re_search(r"\bvalloc\b", low):
        return (
            f"valloc unencoded: unconstrained valloc is not a "
            f"proof ({engine})"
        )
    if _re_search(r"\bnotify_all_at_thread_exit\b", low):
        return (
            f"C++ notify_all_at_thread_exit unencoded: {engine} is not a "
            "notify_all_at_thread_exit model"
        )
    if "condition_variable_any" in low:
        return (
            f"C++ condition_variable_any unencoded: {engine} is not a "
            "condition_variable_any model"
        )
    if "shared_timed_mutex" in low:
        return (
            f"C++ shared_timed_mutex unencoded: {engine} is not a "
            "shared_timed_mutex model"
        )
    if "condition_variable unencoded" in low or "shared_mutex" in low:
        return (
            f"C++ std::condition_variable unencoded: {engine} is not a "
            "condvar/shared-mutex model"
        )
    if "atomic_ref unencoded" in low or "std::atomic_ref" in low:
        return (
            f"C++ std::atomic_ref unencoded: {engine} is not an "
            "atomic_ref model"
        )
    if "generator unencoded" in low or "std::generator" in low:
        return (
            f"C++ std::generator unencoded: {engine} is not a generator model"
        )
    if _re_search(r"\bassume_aligned\b", low) or "assume_aligned unencoded" in low:
        return (
            f"C++ assume_aligned unencoded: {engine} is not an "
            "assume_aligned model"
        )
    if "assume unencoded" in low:
        return (
            f"C++ assume unencoded: {engine} is not an assume-attribute model"
        )
    if "std bind unencoded" in low or "std::bind" in low:
        return (
            f"C++ std::bind unencoded: {engine} is not a bind model"
        )
    if "atomic builtin unencoded" in low:
        return (
            f"atomic builtin unencoded: {engine} is not an "
            "atomic-builtin model"
        )
    if "chown unencoded" in low:
        return (
            f"chown unencoded: unconstrained chown is not a "
            f"proof ({engine})"
        )
    if _re_search(r"\bsymlinkat\b", low):
        return (
            f"symlinkat unencoded: unconstrained symlinkat is not a "
            f"proof ({engine})"
        )
    if _re_search(r"\breadlinkat\b", low):
        return (
            f"readlinkat unencoded: unconstrained readlinkat is not a "
            f"proof ({engine})"
        )
    if _re_search(r"\b(?:symlink|readlink)\b", low) or (
        "symlink unencoded" in low or "readlink unencoded" in low
    ):
        return (
            f"symlink unencoded: unconstrained symlink/readlink is not a "
            f"proof ({engine})"
        )
    if _re_search(r"\bfts_(?:open|read|children|close|set)\b", low):
        return (
            f"fts_open unencoded: unconstrained fts_open is not a "
            f"proof ({engine})"
        )
    if "opendir unencoded" in low or "dir* model" in low:
        return f"opendir unencoded: {engine} is not a DIR* model"
    if "setrlimit unencoded" in low or "getrlimit unencoded" in low:
        return (
            f"setrlimit unencoded: unconstrained setrlimit is not a "
            f"proof ({engine})"
        )
    if _re_search(r"\b(?:getsockname|getpeername)\b", low):
        return (
            f"getsockname unencoded: unconstrained getsockname is not a "
            f"proof ({engine})"
        )
    if _re_search(r"\bgetpeereid\b", low):
        return (
            f"getpeereid unencoded: unconstrained getpeereid is not a "
            f"proof ({engine})"
        )
    if "getsockopt unencoded" in low or "setsockopt unencoded" in low:
        return (
            f"getsockopt unencoded: unconstrained socket opts is not a "
            f"proof ({engine})"
        )
    if _re_search(r"\b(?:listmount|statmount)\b", low):
        return (
            f"listmount unencoded: unconstrained listmount/statmount "
            f"is not a proof ({engine})"
        )
    if "ustat" in low:
        return (
            f"ustat unencoded: unconstrained ustat is not a "
            f"proof ({engine})"
        )
    if _re_search(r"\bfile_(?:get|set)attr\b", low):
        return (
            f"file_getattr unencoded: unconstrained file_getattr "
            f"is not a proof ({engine})"
        )
    if _re_search(r"\bfh(?:linkat|link|readlink)\b", low) or _re_search(
        r"\bfhlink", low
    ):
        return (
            f"fhlink unencoded: unconstrained fhlink is not a "
            f"proof ({engine})"
        )
    if _re_search(r"\b(?:getfh|fhopen|fhstatfs|fhstat|getfhat)\b", low):
        return (
            f"getfh unencoded: unconstrained getfh is not a "
            f"proof ({engine})"
        )
    if _re_search(r"\bfstatat\b", low):
        return (
            f"fstatat unencoded: unconstrained fstatat is not a "
            f"proof ({engine})"
        )
    if _re_search(r"\bgetmntinfo\b", low):
        return (
            f"getmntinfo unencoded: unconstrained getmntinfo is not a "
            f"proof ({engine})"
        )
    if _re_search(r"\bgetvfsbyname\b", low):
        return (
            f"getvfsbyname unencoded: unconstrained getvfsbyname is not a "
            f"proof ({engine})"
        )
    if _re_search(r"\b(?:get|set|end)fsent\b", low):
        return (
            f"getfsent unencoded: unconstrained getfsent is not a "
            f"proof ({engine})"
        )
    if _re_search(r"\bgetfsstat\b", low):
        return (
            f"getfsstat unencoded: unconstrained getfsstat is not a "
            f"proof ({engine})"
        )
    if (
        _re_search(r"\b(?:lstat|fstat|stat)\b", low)
        or "stat unencoded" in low
        or "lstat unencoded" in low
        or "fstat unencoded" in low
    ):
        return (
            f"stat unencoded: unconstrained stat is not a "
            f"proof ({engine})"
        )
    if _re_search(r"\brenameat2\b", low):
        return (
            f"renameat2 unencoded: unconstrained renameat2 is not a "
            f"proof ({engine})"
        )
    if _re_search(r"\brenameat\b", low):
        return (
            f"renameat unencoded: unconstrained renameat is not a "
            f"proof ({engine})"
        )
    if _re_search(r"\bmkdirat\b", low):
        return (
            f"mkdirat unencoded: unconstrained mkdirat is not a "
            f"proof ({engine})"
        )
    if (
        _re_search(r"\b(?:mkdir|rmdir|rename)\b", low)
        or "mkdir unencoded" in low
        or "rmdir unencoded" in low
        or "rename unencoded" in low
    ):
        return (
            f"mkdir unencoded: unconstrained mkdir is not a "
            f"proof ({engine})"
        )
    if _re_search(r"\bcrypt_(?:newhash|checkpass)\b", low):
        return (
            f"crypt_newhash unencoded: unconstrained crypt_newhash is not a "
            f"proof ({engine})"
        )
    if _re_search(r"\bkenv\b", low):
        return (
            f"kenv unencoded: unconstrained kenv is not a "
            f"proof ({engine})"
        )
    if "getpwuid unencoded" in low or "getpwnam unencoded" in low or "crypt unencoded" in low:
        return (
            f"getpwuid unencoded: unconstrained getpwuid is not a "
            f"proof ({engine})"
        )
    if "std::any" in low or "any_cast" in low or "any unencoded" in low:
        return f"C++ std::any unencoded: {engine} is not an any model"
    if (
        "filesystem unencoded" in low
        or "std::filesystem" in low
        or "std::fs::" in low
    ):
        return (
            f"C++ std::filesystem unencoded: {engine} is not a "
            "filesystem model"
        )
    if "std::regex" in low or "regex unencoded" in low:
        return f"C++ std::regex unencoded: {engine} is not a regex model"
    if _re_search(r"\bpthread_barrier_(?:wait|init|destroy)\b", low) or (
        "pthread_barrier unencoded" in low
    ):
        return (
            f"pthread_barrier unencoded: unconstrained pthread_barrier_wait "
            f"is not a proof ({engine})"
        )
    if (
        "std::latch" in low
        or "std::barrier" in low
        or "counting_semaphore" in low
        or "latch unencoded" in low
        or "barrier unencoded" in low
        or "latch/barrier" in low
    ):
        return (
            f"C++ std::latch/barrier unencoded: {engine} is not a "
            "sync primitive model"
        )
    if "from_chars" in low or "charconv unencoded" in low:
        return (
            f"C++ from_chars unencoded: {engine} is not a charconv model"
        )
    if "std::visit" in low or "visit unencoded" in low:
        return f"C++ std::visit unencoded: {engine} is not a visitor model"
    if "initializer_list" in low:
        return (
            f"C++ initializer_list unencoded: {engine} is not a "
            "temporary-lifetime model"
        )
    if (
        "clock_settime" in low
        or "clock_adjtime" in low
        or "clock_nanosleep" in low
    ):
        return (
            f"clock_settime unencoded: unconstrained clock_settime "
            f"is not a proof ({engine})"
        )
    if "settimeofday" in low:
        return (
            f"settimeofday unencoded: unconstrained settimeofday is not a "
            f"proof ({engine})"
        )
    if _re_search(r"\bpthread_getcpuclockid\b", low):
        return (
            f"pthread_getcpuclockid unencoded: unconstrained "
            f"pthread_getcpuclockid is not a proof ({engine})"
        )
    if _re_search(r"\bclock_getcpuclockid\b", low):
        return (
            f"clock_getcpuclockid unencoded: unconstrained "
            f"clock_getcpuclockid is not a proof ({engine})"
        )
    if "clock_gettime" in low or "gettimeofday" in low:
        return (
            f"clock_gettime unencoded: unconstrained clock_gettime is not a "
            f"proof ({engine})"
        )
    if _re_search(r"\bposix_typed_mem_(?:open|get_info)\b", low):
        return (
            f"posix_typed_mem_open unencoded: unconstrained "
            f"posix_typed_mem_open is not a proof ({engine})"
        )
    if "shmget" in low:
        return (
            f"shmget unencoded: unconstrained shmget is not a "
            f"proof ({engine})"
        )
    if "shm_open" in low or "shm_unlink" in low or "shm unencoded" in low:
        return (
            f"shm unencoded: unconstrained shm_open is not a "
            f"proof ({engine})"
        )
    if _re_search(r"\bposix_spawn(?:_file_actions|attr)_init\b", low):
        return (
            f"posix_spawn_file_actions_init unencoded: unconstrained "
            f"posix_spawn_file_actions_init is not a proof ({engine})"
        )
    if "posix_spawn" in low:
        return (
            f"posix_spawn unencoded: unconstrained posix_spawn is not a "
            f"proof ({engine})"
        )
    if "glob unencoded" in low or "globfree" in low:
        return (
            f"glob unencoded: unconstrained glob is not a "
            f"proof ({engine})"
        )
    if (
        "fseek unencoded" in low
        or "ftell unencoded" in low
        or "fgetpos" in low
        or "fsetpos" in low
    ):
        return (
            f"fseek unencoded: unconstrained fseek is not a "
            f"proof ({engine})"
        )
    if "sleep unencoded" in low or "nanosleep" in low or "usleep" in low:
        return (
            f"sleep unencoded: unconstrained sleep is not a "
            f"proof ({engine})"
        )
    if _re_search(r"\bfchmodat2\b", low):
        return (
            f"fchmodat2 unencoded: unconstrained fchmodat2 is not a "
            f"proof ({engine})"
        )
    if _re_search(r"\bfchmodat\b", low):
        return (
            f"fchmodat unencoded: unconstrained fchmodat is not a "
            f"proof ({engine})"
        )
    if _re_search(r"\b(?:f|l)?chflags\b", low):
        return (
            f"chflags unencoded: unconstrained chflags is not a "
            f"proof ({engine})"
        )
    if _re_search(r"\bfaccessat2\b", low):
        return (
            f"faccessat2 unencoded: unconstrained faccessat2 is not a "
            f"proof ({engine})"
        )
    if _re_search(r"\bfaccessat\b", low):
        return (
            f"faccessat unencoded: unconstrained faccessat is not a "
            f"proof ({engine})"
        )
    if "vhangup" in low:
        return (
            f"vhangup unencoded: unconstrained vhangup is not a "
            f"proof ({engine})"
        )
    if _re_search(r"\b(?:at_)?quick_exit\b", low):
        return (
            f"quick_exit unencoded: unconstrained quick_exit is not a "
            f"proof ({engine})"
        )
    if _re_search(r"\beaccess\b", low):
        return (
            f"eaccess unencoded: unconstrained eaccess is not a "
            f"proof ({engine})"
        )
    if "access unencoded" in low:
        return (
            f"access unencoded: unconstrained access is not a "
            f"proof ({engine})"
        )
    if "source_location" in low:
        return (
            f"C++ source_location unencoded: {engine} is not a "
            "source_location model"
        )
    if "stacktrace unencoded" in low or "std::stacktrace" in low:
        return (
            f"C++ stacktrace unencoded: {engine} is not a stacktrace model"
        )
    if "stop_token" in low or "stop_source" in low or "stop_callback" in low:
        return (
            f"C++ stop_token unencoded: {engine} is not a stop_token model"
        )
    if "flat_map" in low:
        return (
            f"C++ flat_map unencoded: {engine} is not a flat_map model"
        )
    if "flat_set" in low:
        return (
            f"C++ flat_set unencoded: {engine} is not a flat_set model"
        )
    if "flat_multiset" in low:
        return (
            f"C++ flat_multiset unencoded: {engine} is not a "
            "flat_multiset model"
        )
    if "flat_multimap" in low:
        return (
            f"C++ flat_multimap unencoded: {engine} is not a "
            "flat_multimap model"
        )
    if "zoned_time" in low:
        return (
            f"C++ zoned_time unencoded: {engine} is not a zoned_time model"
        )
    if "current_zone" in low or _re_search(r"\btzdb\b", low):
        return (
            f"C++ tzdb unencoded: {engine} is not a tzdb model"
        )
    if "chrono" in low:
        return (
            f"C++ chrono unencoded: {engine} is not a chrono model"
        )
    if _re_search(r"\bzip_transform(?:_view)?\b", low) or (
        "zip_transform unencoded" in low
    ):
        return (
            f"C++ zip_transform unencoded: {engine} is not a "
            "zip_transform model"
        )
    if "views::zip" in low or "zip_view" in low:
        return (
            f"C++ views::zip unencoded: {engine} is not a views::zip model"
        )
    if _re_search(r"\bis_scoped_enum\b", low) or "is_scoped_enum unencoded" in low:
        return (
            f"C++ is_scoped_enum unencoded: {engine} is not an "
            "is_scoped_enum model"
        )
    if _re_search(r"\b(?:views\s*::\s*)?enumerate(?:_view)?\b", low) or (
        "enumerate unencoded" in low
    ):
        return (
            f"C++ views::enumerate unencoded: {engine} is not a "
            "views::enumerate model"
        )
    if _re_search(r"\bcartesian_product(?:_view)?\b", low) or (
        "cartesian_product unencoded" in low
    ):
        return (
            f"C++ cartesian_product unencoded: {engine} is not a "
            "cartesian_product model"
        )
    if _re_search(
        r"\b(?:views\s*::\s*chunk(?:_by)?|chunk(?:_by|_view))\b",
        low,
    ) or "chunk unencoded" in low:
        return (
            f"C++ views::chunk unencoded: {engine} is not a "
            "views::chunk model"
        )
    if _re_search(r"\b(?:views\s*::\s*slide|slide_view)\b", low) or (
        "slide unencoded" in low
    ):
        return (
            f"C++ views::slide unencoded: {engine} is not a "
            "views::slide model"
        )
    if _re_search(
        r"\b(?:views\s*::\s*adjacent(?:_transform)?|"
        r"adjacent(?:_transform|_view))\b",
        low,
    ) or "adjacent unencoded" in low:
        return (
            f"C++ views::adjacent unencoded: {engine} is not a "
            "views::adjacent model"
        )
    if _re_search(r"\bjoin_with(?:_view)?\b", low) or "join_with unencoded" in low:
        return (
            f"C++ join_with unencoded: {engine} is not a join_with model"
        )
    if "views::join" in low or "join_view" in low:
        return (
            f"C++ views::join unencoded: {engine} is not a views::join model"
        )
    if _re_search(r"\b(?:views\s*::\s*)?stride(?:_view)?\b", low) or (
        "stride unencoded" in low
    ):
        return (
            f"C++ views::stride unencoded: {engine} is not a "
            "views::stride model"
        )
    if _re_search(r"\b(?:views\s*::\s*repeat|repeat_view)\b", low) or (
        "repeat unencoded" in low
    ):
        return (
            f"C++ views::repeat unencoded: {engine} is not a "
            "views::repeat model"
        )
    if _re_search(r"\btake_while(?:_view)?\b", low) or (
        "take_while unencoded" in low
    ):
        return (
            f"C++ views::take_while unencoded: {engine} is not a "
            "take_while model"
        )
    if _re_search(r"\b(?:views\s*::\s*take\b|\btake_view\b)", low) or (
        "take unencoded" in low
    ):
        return (
            f"C++ views::take unencoded: {engine} is not a "
            "views::take model"
        )
    if _re_search(r"\bdrop_while(?:_view)?\b", low) or (
        "drop_while unencoded" in low
    ):
        return (
            f"C++ views::drop_while unencoded: {engine} is not a "
            "drop_while model"
        )
    if _re_search(r"\b(?:views\s*::\s*drop\b|\bdrop_view\b)", low) or (
        "drop unencoded" in low
    ):
        return (
            f"C++ views::drop unencoded: {engine} is not a "
            "views::drop model"
        )
    if _re_search(r"\b(?:views\s*::\s*keys\b|\bkeys_view\b)", low) or (
        "keys unencoded" in low
    ):
        return (
            f"C++ views::keys unencoded: {engine} is not a "
            "keys model"
        )
    if _re_search(r"\b(?:views\s*::\s*values\b|\bvalues_view\b)", low) or (
        "values unencoded" in low
    ):
        return (
            f"C++ views::values unencoded: {engine} is not a "
            "values model"
        )
    if _re_search(r"\b(?:views\s*::\s*reverse\b|\breverse_view\b)", low) or (
        "reverse_view unencoded" in low or "views::reverse unencoded" in low
    ):
        return (
            f"C++ views::reverse unencoded: {engine} is not a "
            "reverse_view model"
        )
    if _re_search(r"\b(?:views\s*::\s*counted\b|\bcounted_view\b)", low) or (
        "counted_view unencoded" in low or "views::counted unencoded" in low
    ):
        return (
            f"C++ views::counted unencoded: {engine} is not a "
            "counted_view model"
        )
    if _re_search(r"\b(?:views\s*::\s*filter\b|\bfilter_view\b)", low) or (
        "filter unencoded" in low
    ):
        return (
            f"C++ views::filter unencoded: {engine} is not a "
            "views::filter model"
        )
    if _re_search(r"\b(?:views\s*::\s*transform\b|\btransform_view\b)", low) or (
        "transform_view unencoded" in low
        or "views::transform unencoded" in low
    ):
        return (
            f"C++ views::transform unencoded: {engine} is not a "
            "transform_view model"
        )
    if _re_search(r"\b(?:views\s*::\s*elements\b|\belements_view\b)", low) or (
        "elements unencoded" in low
    ):
        return (
            f"C++ views::elements unencoded: {engine} is not an "
            "elements model"
        )
    if _re_search(r"\b(?:views\s*::\s*iota\b|\biota_view\b)", low) or (
        "iota unencoded" in low
    ):
        return (
            f"C++ views::iota unencoded: {engine} is not an "
            "iota model"
        )
    if (
        "ranges views" in low
        or "ranges-views" in low
        or "std::views" in low
        or "std::ranges::views" in low
    ):
        return (
            f"C++ ranges views unencoded: {engine} is not a "
            "ranges-views model"
        )
    if "getopt" in low:
        return (
            f"getopt unencoded: unconstrained getopt is not a "
            f"proof ({engine})"
        )
    if _re_search(r"\bgetosreldate\b", low):
        return (
            f"getosreldate unencoded: unconstrained getosreldate is not a "
            f"proof ({engine})"
        )
    if _re_search(r"\bgetdomainname\b", low):
        return (
            f"getdomainname unencoded: unconstrained getdomainname "
            f"is not a proof ({engine})"
        )
    if "uname" in low or "gethostname" in low:
        return (
            f"uname unencoded: unconstrained uname is not a "
            f"proof ({engine})"
        )
    if "sethostname" in low:
        return (
            f"sethostname unencoded: unconstrained sethostname is not a "
            f"proof ({engine})"
        )
    if _re_search(r"\b(?:preadv2|pwritev2|preadv|pwritev)\b", low):
        return (
            f"preadv unencoded: unconstrained preadv is not a "
            f"proof ({engine})"
        )
    if "sendfile" in low or "copy_file_range" in low:
        return (
            f"sendfile unencoded: unconstrained sendfile is not a "
            f"proof ({engine})"
        )
    if "memfd_secret" in low:
        return (
            f"memfd_secret unencoded: unconstrained memfd_secret "
            f"is not a proof ({engine})"
        )
    if _re_search(r"\b(?:timerfd_settime|timerfd_gettime)\b", low):
        return (
            f"timerfd_settime unencoded: unconstrained "
            f"timerfd_settime is not a proof ({engine})"
        )
    if _re_search(r"\beventfd_(?:read|write)\b", low):
        return (
            f"eventfd_read unencoded: unconstrained eventfd_read is not a "
            f"proof ({engine})"
        )
    if (
        "memfd" in low
        or "eventfd" in low
        or "timerfd_create" in low
    ):
        return (
            f"memfd unencoded: unconstrained memfd_create is not a "
            f"proof ({engine})"
        )
    if "arch_prctl" in low:
        return (
            f"arch_prctl unencoded: unconstrained arch_prctl is not a "
            f"proof ({engine})"
        )
    if _re_search(r"\bprocctl\b", low):
        return (
            f"procctl unencoded: unconstrained procctl is not a "
            f"proof ({engine})"
        )
    if _re_search(r"\bktrace\b", low):
        return (
            f"ktrace unencoded: unconstrained ktrace is not a "
            f"proof ({engine})"
        )
    if "prctl" in low or "ptrace" in low:
        return (
            f"prctl unencoded: unconstrained prctl is not a "
            f"proof ({engine})"
        )
    if (
        "tcgetattr" in low
        or "tcsetattr" in low
        or "cfmakeraw" in low
    ):
        return (
            f"tcgetattr unencoded: unconstrained tcgetattr is not a "
            f"proof ({engine})"
        )
    if "module import unencoded" in low or "export module" in low:
        return (
            f"C++ module import unencoded: {engine} is not a modules model"
        )
    if _re_search(r"\bgetpagesizes\b", low):
        return (
            f"getpagesizes unencoded: unconstrained getpagesizes is not a "
            f"proof ({engine})"
        )
    if _re_search(r"\bgetpagesize(?!s)\b", low):
        return (
            f"getpagesize unencoded: unconstrained getpagesize is not a "
            f"proof ({engine})"
        )
    if _re_search(r"\blpathconf\b", low):
        return (
            f"lpathconf unencoded: unconstrained lpathconf is not a "
            f"proof ({engine})"
        )
    if _re_search(r"\b(?:sysconf|fpathconf|pathconf)\b", low):
        return (
            f"sysconf unencoded: unconstrained sysconf is not a "
            f"proof ({engine})"
        )
    if "getrusage" in low:
        return (
            f"getrusage unencoded: unconstrained getrusage is not a "
            f"proof ({engine})"
        )
    if "nftw" in low or "ftw unencoded" in low:
        return (
            f"nftw unencoded: unconstrained nftw is not a "
            f"proof ({engine})"
        )
    if "wordexp" in low or "wordfree" in low:
        return (
            f"wordexp unencoded: unconstrained wordexp is not a "
            f"proof ({engine})"
        )
    if _re_search(r"\b(?:get|set)loginclass\b", low):
        return (
            f"loginclass unencoded: unconstrained getloginclass is not a "
            f"proof ({engine})"
        )
    if _re_search(r"\b(?:login_getclass|setusercontext)\b", low):
        return (
            f"login_getclass unencoded: unconstrained login_getclass is not a "
            f"proof ({engine})"
        )
    if _re_search(r"\bsetlogin\b", low):
        return (
            f"setlogin unencoded: unconstrained setlogin is not a "
            f"proof ({engine})"
        )
    if _re_search(r"\bgetlogin(?:_r)?\b", low) or _re_search(r"\bttyname(?:_r)?\b", low):
        return (
            f"getlogin unencoded: unconstrained getlogin is not a "
            f"proof ({engine})"
        )
    if "inet_pton" in low or "inet_ntop" in low or "inet_aton" in low:
        return (
            f"inet_pton unencoded: unconstrained inet_pton is not a "
            f"proof ({engine})"
        )
    if _re_search(r"\bmseal\b", low):
        return (
            f"mseal unencoded: unconstrained mseal is not a "
            f"proof ({engine})"
        )
    if _re_search(r"\bmlock2\b", low):
        return (
            f"mlock2 unencoded: unconstrained mlock2 is not a "
            f"proof ({engine})"
        )
    if (
        "munlockall" in low
        or "mlockall" in low
        or "munlock" in low
        or "mlock" in low
    ):
        return (
            f"mlock unencoded: unconstrained mlock is not a "
            f"proof ({engine})"
        )
    if _re_search(r"\bposix_fadvise(?:64)?\b", low):
        return (
            f"posix_fadvise unencoded: unconstrained posix_fadvise is not a "
            f"proof ({engine})"
        )
    if _re_search(r"\breadahead\b", low):
        return (
            f"readahead unencoded: unconstrained readahead is not a "
            f"proof ({engine})"
        )
    if "posix_madvise" in low or (
        "madvise" in low and "process_madvise" not in low
    ):
        return (
            f"madvise unencoded: unconstrained madvise is not a "
            f"proof ({engine})"
        )
    if "vmsplice" in low or "splice" in low:
        return (
            f"splice unencoded: unconstrained splice is not a "
            f"proof ({engine})"
        )
    if _re_search(r"\binotify_rm_watch\b", low):
        return (
            f"inotify_rm_watch unencoded: unconstrained inotify_rm_watch "
            f"is not a proof ({engine})"
        )
    if "inotify" in low:
        return (
            f"inotify unencoded: unconstrained inotify is not a "
            f"proof ({engine})"
        )
    if "fdatasync" in low or (
        "fsync" in low and "syncfs" not in low
    ):
        return (
            f"fsync unencoded: unconstrained fsync is not a "
            f"proof ({engine})"
        )
    if "getrandom" in low or "getentropy" in low:
        return (
            f"getrandom unencoded: unconstrained getrandom is not a "
            f"proof ({engine})"
        )
    if _re_search(r"\barc4random(?:_buf|_uniform)?\b", low):
        return (
            f"arc4random unencoded: unconstrained arc4random is not a "
            f"proof ({engine})"
        )
    if _re_search(r"\bissetugid\b", low):
        return (
            f"issetugid unencoded: unconstrained issetugid is not a "
            f"proof ({engine})"
        )
    if "getdelim" in low or "getline" in low:
        return (
            f"getline unencoded: unconstrained getline is not a "
            f"proof ({engine})"
        )
    if "vasprintf" in low or "asprintf" in low:
        return (
            f"asprintf unencoded: unconstrained asprintf is not a "
            f"proof ({engine})"
        )
    if "strlcpy" in low or "strlcat" in low:
        return (
            f"strlcpy unencoded: unconstrained strlcpy is not a "
            f"proof ({engine})"
        )
    if (
        "explicit_bzero" in low
        or "memset_s" in low
        or "explicit_memset" in low
    ):
        return (
            f"explicit_bzero unencoded: unconstrained explicit_bzero "
            f"is not a proof ({engine})"
        )
    if _re_search(r"\btimingsafe_(?:bcmp|memcmp)\b", low):
        return (
            f"timingsafe_bcmp unencoded: unconstrained timingsafe_bcmp "
            f"is not a proof ({engine})"
        )
    if "isatty" in low:
        return (
            f"isatty unencoded: unconstrained isatty is not a "
            f"proof ({engine})"
        )
    if (
        "ptsname" in low
        or "posix_openpt" in low
        or "grantpt" in low
        or "unlockpt" in low
    ):
        return (
            f"ptsname unencoded: unconstrained ptsname is not a "
            f"proof ({engine})"
        )
    if _re_search(r"\bnmount\b", low):
        return (
            f"nmount unencoded: unconstrained nmount is not a "
            f"proof ({engine})"
        )
    if _re_search(r"\bunmount\b", low):
        return (
            f"unmount unencoded: unconstrained unmount is not a "
            f"proof ({engine})"
        )
    if _re_search(r"\b(?:umount2|umount|mount)\b", low):
        return (
            f"mount unencoded: unconstrained mount is not a "
            f"proof ({engine})"
        )
    if (
        "open_wmemstream" in low
        or "open_memstream" in low
        or "fmemopen" in low
    ):
        return (
            f"fmemopen unencoded: unconstrained fmemopen is not a "
            f"proof ({engine})"
        )
    if "scandir" in low:
        return (
            f"scandir unencoded: unconstrained scandir is not a "
            f"proof ({engine})"
        )
    if _re_search(r"\bextattr_(?:set|get|delete|list)_(?:file|fd|link)\b", low):
        return (
            f"extattr unencoded: unconstrained extattr_set_file is not a "
            f"proof ({engine})"
        )
    if _re_search(r"\b(?:set|get|list|remove)xattrat\b", low):
        return (
            f"setxattrat unencoded: unconstrained setxattrat is not a "
            f"proof ({engine})"
        )
    if (
        _re_search(r"\b(?:lsetxattr|fsetxattr|setxattr)\b", low)
        or _re_search(r"\b(?:listxattr|removexattr|getxattr)\b", low)
    ):
        return (
            f"setxattr unencoded: unconstrained setxattr is not a "
            f"proof ({engine})"
        )
    if _re_search(r"\b(?:sched_setattr|sched_getattr)\b", low):
        return (
            f"sched_setattr unencoded: unconstrained sched_setattr is not a "
            f"proof ({engine})"
        )
    if _re_search(r"\bsched_yield\b", low):
        return (
            f"sched_yield unencoded: unconstrained sched_yield is not a "
            f"proof ({engine})"
        )
    if _re_search(r"\bpthread_yield\b", low):
        return (
            f"pthread_yield unencoded: unconstrained pthread_yield is not a "
            f"proof ({engine})"
        )
    if _re_search(r"\bcpuset_(?:set|get)affinity\b", low):
        return (
            f"cpuset unencoded: unconstrained cpuset_setaffinity is not a "
            f"proof ({engine})"
        )
    if _re_search(r"\bsched_get_priority_(?:max|min)\b", low):
        return (
            f"sched_get_priority_max unencoded: unconstrained "
            f"sched_get_priority_max is not a proof ({engine})"
        )
    if "sched_setaffinity" in low or "sched_getaffinity" in low:
        return (
            f"sched unencoded: unconstrained sched_setaffinity is not a "
            f"proof ({engine})"
        )
    if (
        "sched_setscheduler" in low
        or "sched_getscheduler" in low
        or "sched_setparam" in low
        or "sched_getparam" in low
    ):
        return (
            f"sched_setscheduler unencoded: unconstrained "
            f"sched_setscheduler is not a proof ({engine})"
        )
    if _re_search(r"\blio_listio\b", low):
        return (
            f"lio_listio unencoded: unconstrained lio_listio is not a "
            f"proof ({engine})"
        )
    if (
        "aio_read" in low
        or "aio_write" in low
        or "aio_error" in low
        or "aio_return" in low
        or "aio_suspend" in low
        or "aio unencoded" in low
    ):
        return (
            f"aio unencoded: unconstrained aio_read is not a "
            f"proof ({engine})"
        )
    if "io_uring" in low:
        return (
            f"io_uring unencoded: unconstrained io_uring_setup is not a "
            f"proof ({engine})"
        )
    if "capset" in low or "capget" in low:
        return (
            f"capset unencoded: unconstrained capset is not a "
            f"proof ({engine})"
        )
    if "statx" in low:
        return (
            f"statx unencoded: unconstrained statx is not a "
            f"proof ({engine})"
        )
    if "pidfd" in low:
        return (
            f"pidfd unencoded: unconstrained pidfd_open is not a "
            f"proof ({engine})"
        )
    if "fanotify" in low:
        return (
            f"fanotify unencoded: unconstrained fanotify_init is not a "
            f"proof ({engine})"
        )
    if "seccomp" in low:
        return (
            f"seccomp unencoded: unconstrained seccomp is not a "
            f"proof ({engine})"
        )
    if "getgrnam" in low or "getgrgid" in low or "getspnam" in low:
        return (
            f"getgrnam unencoded: unconstrained getgrnam is not a "
            f"proof ({engine})"
        )
    if "fallocate" in low:
        return (
            f"fallocate unencoded: unconstrained posix_fallocate is not a "
            f"proof ({engine})"
        )
    if "close_range" in low:
        return (
            f"close_range unencoded: unconstrained close_range is not a "
            f"proof ({engine})"
        )
    if _re_search(r"\bclosefrom\b", low):
        return (
            f"closefrom unencoded: unconstrained closefrom is not a "
            f"proof ({engine})"
        )
    if _re_search(r"\blsm_(?:get_self_attr|set_self_attr|list_modules)\b", low):
        return (
            f"lsm_get_self_attr unencoded: unconstrained "
            f"lsm_get_self_attr is not a proof ({engine})"
        )
    if "landlock" in low:
        return (
            f"landlock unencoded: unconstrained landlock_create_ruleset "
            f"is not a proof ({engine})"
        )
    if _re_search(r"\brtprio(?:_thread)?\b", low):
        return (
            f"rtprio unencoded: unconstrained rtprio is not a "
            f"proof ({engine})"
        )
    if "getpriority" in low or "setpriority" in low:
        return (
            f"getpriority unencoded: unconstrained getpriority is not a "
            f"proof ({engine})"
        )
    if "signalfd" in low:
        return (
            f"signalfd unencoded: unconstrained signalfd is not a "
            f"proof ({engine})"
        )
    if _re_search(r"\bsigaction\b", low):
        return (
            f"sigaction unencoded: unconstrained sigaction is not a "
            f"proof ({engine})"
        )
    if _re_search(r"\bpthread_sigmask\b", low):
        return (
            f"pthread_sigmask unencoded: unconstrained pthread_sigmask "
            f"is not a proof ({engine})"
        )
    if _re_search(r"\bsig(?:procmask|suspend)\b", low):
        return (
            f"sigprocmask unencoded: unconstrained sigprocmask is not a "
            f"proof ({engine})"
        )
    if _re_search(r"\bsig(?:waitinfo|timedwait|pending|wait)\b", low):
        return (
            f"sigwait unencoded: unconstrained sigwait is not a "
            f"proof ({engine})"
        )
    if _re_search(r"\bsigaltstack\b", low):
        return (
            f"sigaltstack unencoded: unconstrained sigaltstack is not a "
            f"proof ({engine})"
        )
    if "bpf" in low:
        return (
            f"bpf unencoded: unconstrained bpf is not a "
            f"proof ({engine})"
        )
    if "userfaultfd" in low:
        return (
            f"userfaultfd unencoded: unconstrained userfaultfd is not a "
            f"proof ({engine})"
        )
    if "getpass" in low:
        return (
            f"getpass unencoded: unconstrained getpass is not a "
            f"proof ({engine})"
        )
    if _re_search(r"\bgetgrouplist\b", low):
        return (
            f"getgrouplist unencoded: unconstrained getgrouplist is not a "
            f"proof ({engine})"
        )
    if _re_search(r"\bgetgroups\b", low):
        return (
            f"getgroups unencoded: unconstrained getgroups is not a "
            f"proof ({engine})"
        )
    if "initgroups" in low or "setgroups" in low:
        return (
            f"initgroups unencoded: unconstrained initgroups is not a "
            f"proof ({engine})"
        )
    if (
        "unshare" in low
        or "setns" in low
        or "clone(" in low
        or "clone (" in low
    ):
        return (
            f"unshare unencoded: unconstrained unshare/clone is not a "
            f"proof ({engine})"
        )
    if "openat2" in low:
        return (
            f"openat2 unencoded: unconstrained openat2 is not a "
            f"proof ({engine})"
        )
    if _re_search(r"\b(?:sendmsg|recvmsg)\b", low):
        return (
            f"sendmsg unencoded: unconstrained sendmsg is not a "
            f"proof ({engine})"
        )
    if "sendmmsg" in low or "recvmmsg" in low:
        return (
            f"sendmmsg unencoded: unconstrained sendmmsg is not a "
            f"proof ({engine})"
        )
    if "name_to_handle" in low or "open_by_handle" in low:
        return (
            f"name_to_handle unencoded: unconstrained name_to_handle_at "
            f"is not a proof ({engine})"
        )
    if "process_madvise" in low:
        return (
            f"process_madvise unencoded: unconstrained process_madvise "
            f"is not a proof ({engine})"
        )
    if "personality" in low:
        return (
            f"personality unencoded: unconstrained personality is not a "
            f"proof ({engine})"
        )
    if "quotactl" in low:
        return (
            f"quotactl unencoded: unconstrained quotactl is not a "
            f"proof ({engine})"
        )
    if "pivot_root" in low:
        return (
            f"pivot_root unencoded: unconstrained pivot_root is not a "
            f"proof ({engine})"
        )
    if "membarrier" in low:
        return (
            f"membarrier unencoded: unconstrained membarrier is not a "
            f"proof ({engine})"
        )
    if "pkey_alloc" in low:
        return (
            f"pkey_alloc unencoded: unconstrained pkey_alloc is not a "
            f"proof ({engine})"
        )
    if "statfs" in low:
        return (
            f"statfs unencoded: unconstrained statfs is not a "
            f"proof ({engine})"
        )
    if "syncfs" in low:
        return (
            f"syncfs unencoded: unconstrained syncfs is not a "
            f"proof ({engine})"
        )
    if "prlimit" in low:
        return (
            f"prlimit unencoded: unconstrained prlimit is not a "
            f"proof ({engine})"
        )
    if _re_search(r"\b(?:migrate_pages|move_pages)\b", low):
        return (
            f"move_pages unencoded: unconstrained move_pages is not a "
            f"proof ({engine})"
        )
    if "process_vm_readv" in low:
        return (
            f"process_vm_readv unencoded: unconstrained "
            f"process_vm_readv is not a proof ({engine})"
        )
    if "perf_event_open" in low:
        return (
            f"perf_event_open unencoded: unconstrained "
            f"perf_event_open is not a proof ({engine})"
        )
    if "clone3" in low:
        return (
            f"clone3 unencoded: unconstrained clone3 is not a "
            f"proof ({engine})"
        )
    if "kcmp" in low:
        return (
            f"kcmp unencoded: unconstrained kcmp is not a "
            f"proof ({engine})"
        )
    if "keyctl" in low:
        return (
            f"keyctl unencoded: unconstrained keyctl is not a "
            f"proof ({engine})"
        )
    if _re_search(r"\bopen_tree_attr\b", low):
        return (
            f"open_tree_attr unencoded: unconstrained open_tree_attr "
            f"is not a proof ({engine})"
        )
    if (
        "fsopen" in low
        or "fsmount" in low
        or _re_search(r"\bopen_tree\b", low)
        or "move_mount" in low
        or "fspick" in low
        or "fsconfig" in low
    ):
        return (
            f"fsopen unencoded: unconstrained fsopen is not a "
            f"proof ({engine})"
        )
    if "process_mrelease" in low:
        return (
            f"process_mrelease unencoded: unconstrained "
            f"process_mrelease is not a proof ({engine})"
        )
    if "ioprio" in low:
        return (
            f"ioprio unencoded: unconstrained ioprio is not a "
            f"proof ({engine})"
        )
    if "mq_open" in low:
        return (
            f"mq_open unencoded: unconstrained mq_open is not a "
            f"proof ({engine})"
        )
    if _re_search(r"\b_umtx_op\b", low):
        return (
            f"_umtx_op unencoded: unconstrained _umtx_op is not a "
            f"proof ({engine})"
        )
    if _re_search(r"\bfutex_waitv\b", low):
        return (
            f"futex_waitv unencoded: unconstrained futex_waitv is not a "
            f"proof ({engine})"
        )
    if _re_search(r"\bfutex_(?:wake|wait|requeue)\b", low):
        return (
            f"futex_wait unencoded: unconstrained futex_wait is not a "
            f"proof ({engine})"
        )
    if _re_search(r"\bfutex\b", low):
        return (
            f"futex unencoded: unconstrained futex is not a "
            f"proof ({engine})"
        )
    if "adjtimex" in low:
        return (
            f"adjtimex unencoded: unconstrained adjtimex is not a "
            f"proof ({engine})"
        )
    if _re_search(r"\b(?:ntp_)?adjtime\b", low):
        return (
            f"adjtime unencoded: unconstrained adjtime is not a "
            f"proof ({engine})"
        )
    if _re_search(r"\bntp_gettime\b", low):
        return (
            f"ntp_gettime unencoded: unconstrained ntp_gettime is not a "
            f"proof ({engine})"
        )
    if _re_search(r"\brevoke\b", low):
        return (
            f"revoke unencoded: unconstrained revoke is not a "
            f"proof ({engine})"
        )
    if _re_search(r"\bjail(?:_attach|_get|_set|_remove)?\b", low):
        return (
            f"jail unencoded: unconstrained jail is not a "
            f"proof ({engine})"
        )
    if _re_search(r"\b(?:fflagstostr|strtofflags)\b", low):
        return (
            f"fflagstostr unencoded: unconstrained fflagstostr is not a "
            f"proof ({engine})"
        )
    if _re_search(r"\bstrmode\b", low):
        return (
            f"strmode unencoded: unconstrained strmode is not a "
            f"proof ({engine})"
        )
    if _re_search(r"\bstrtonum\b", low):
        return (
            f"strtonum unencoded: unconstrained strtonum is not a "
            f"proof ({engine})"
        )
    if _re_search(r"\breallocarray\b", low):
        return (
            f"reallocarray unencoded: unconstrained reallocarray is not a "
            f"proof ({engine})"
        )
    if _re_search(r"\breallocf\b", low):
        return (
            f"reallocf unencoded: unconstrained reallocf is not a "
            f"proof ({engine})"
        )
    if _re_search(r"\b(?:get|set)progname\b", low):
        return (
            f"getprogname unencoded: unconstrained getprogname is not a "
            f"proof ({engine})"
        )
    if _re_search(r"\b(?:setproctitle|daemon)\b", low):
        return (
            f"daemon unencoded: unconstrained daemon is not a "
            f"proof ({engine})"
        )
    if _re_search(r"\b(?:auditon|getaudit|setaudit|auditctl)\b", low):
        return (
            f"auditon unencoded: unconstrained auditon is not a "
            f"proof ({engine})"
        )
    if _re_search(r"\bkinfo_get(?:proc|file|vmmap)\b", low):
        return (
            f"kinfo_getproc unencoded: unconstrained kinfo_getproc is not a "
            f"proof ({engine})"
        )
    if _re_search(r"\bkvm_(?:open|openfiles|getprocs|close|nlist)\b", low):
        return (
            f"kvm_open unencoded: unconstrained kvm_open is not a "
            f"proof ({engine})"
        )
    if _re_search(r"\buuidgen\b", low):
        return (
            f"uuidgen unencoded: unconstrained uuidgen is not a "
            f"proof ({engine})"
        )
    if _re_search(r"\bsetfib\b", low):
        return (
            f"setfib unencoded: unconstrained setfib is not a "
            f"proof ({engine})"
        )
    if "reboot" in low:
        return (
            f"reboot unencoded: unconstrained reboot is not a "
            f"proof ({engine})"
        )
    if "swapon" in low or "swapoff" in low:
        return (
            f"swapon unencoded: unconstrained swapon is not a "
            f"proof ({engine})"
        )
    if "acct" in low:
        return (
            f"acct unencoded: unconstrained acct is not a "
            f"proof ({engine})"
        )
    if "ioperm" in low or _re_search(r"\biopl\b", low):
        return (
            f"ioperm unencoded: unconstrained ioperm is not a "
            f"proof ({engine})"
        )
    if "mincore" in low:
        return (
            f"mincore unencoded: unconstrained mincore is not a "
            f"proof ({engine})"
        )
    if _re_search(r"\brseq\b", low):
        return (
            f"rseq unencoded: unconstrained rseq is not a "
            f"proof ({engine})"
        )
    if "timer_create" in low and "timerfd" not in low:
        return (
            f"timer_create unencoded: unconstrained timer_create "
            f"is not a proof ({engine})"
        )
    if "semget" in low:
        return (
            f"semget unencoded: unconstrained semget is not a "
            f"proof ({engine})"
        )
    if "msgget" in low:
        return (
            f"msgget unencoded: unconstrained msgget is not a "
            f"proof ({engine})"
        )
    if _re_search(r"\bsyslog\b", low):
        return (
            f"syslog unencoded: unconstrained syslog is not a "
            f"proof ({engine})"
        )
    if "klogctl" in low:
        return (
            f"klogctl unencoded: unconstrained klogctl is not a "
            f"proof ({engine})"
        )
    if "mount_setattr" in low:
        return (
            f"mount_setattr unencoded: unconstrained "
            f"mount_setattr is not a proof ({engine})"
        )
    if _re_search(r"\bgetcpu\b", low):
        return (
            f"getcpu unencoded: unconstrained getcpu is not a "
            f"proof ({engine})"
        )
    if "init_module" in low or "finit_module" in low or "delete_module" in low:
        return (
            f"init_module unencoded: unconstrained init_module "
            f"is not a proof ({engine})"
        )
    if "kexec" in low:
        return (
            f"kexec unencoded: unconstrained kexec_load is not a "
            f"proof ({engine})"
        )
    if "quotactl_fd" in low:
        return (
            f"quotactl_fd unencoded: unconstrained quotactl_fd "
            f"is not a proof ({engine})"
        )
    if "pkey_free" in low or "pkey_mprotect" in low:
        return (
            f"pkey_free unencoded: unconstrained pkey_free is not a "
            f"proof ({engine})"
        )
    if "tgkill" in low:
        return (
            f"tgkill unencoded: unconstrained tgkill is not a "
            f"proof ({engine})"
        )
    if "add_key" in low:
        return (
            f"add_key unencoded: unconstrained add_key is not a "
            f"proof ({engine})"
        )
    if "semctl" in low:
        return (
            f"semctl unencoded: unconstrained semctl is not a "
            f"proof ({engine})"
        )
    if "msgctl" in low:
        return (
            f"msgctl unencoded: unconstrained msgctl is not a "
            f"proof ({engine})"
        )
    if "shmctl" in low:
        return (
            f"shmctl unencoded: unconstrained shmctl is not a "
            f"proof ({engine})"
        )
    if "timer_settime" in low:
        return (
            f"timer_settime unencoded: unconstrained timer_settime "
            f"is not a proof ({engine})"
        )
    if "setdomainname" in low:
        return (
            f"setdomainname unencoded: unconstrained setdomainname "
            f"is not a proof ({engine})"
        )
    if "io_submit" in low or "io_getevents" in low:
        return (
            f"io_submit unencoded: unconstrained io_submit is not a "
            f"proof ({engine})"
        )
    if _re_search(
        r"\b(?:io_setup|io_destroy|io_cancel|io_pgetevents)\b",
        low,
    ):
        return (
            f"io_setup unencoded: unconstrained io_setup is not a "
            f"proof ({engine})"
        )
    if "request_key" in low:
        return (
            f"request_key unencoded: unconstrained request_key is not a "
            f"proof ({engine})"
        )
    if _re_search(r"\btkill\b", low):
        return (
            f"tkill unencoded: unconstrained tkill is not a "
            f"proof ({engine})"
        )
    if (
        "timer_delete" in low
        or "timer_gettime" in low
        or "timer_getoverrun" in low
    ):
        return (
            f"timer_delete unencoded: unconstrained timer_delete "
            f"is not a proof ({engine})"
        )
    if (
        "mq_unlink" in low
        or "mq_timedsend" in low
        or "mq_timedreceive" in low
        or "mq_notify" in low
        or "mq_getsetattr" in low
    ):
        return (
            f"mq_unlink unencoded: unconstrained mq_unlink is not a "
            f"proof ({engine})"
        )
    if _re_search(r"\b(?:shmat|shmdt)\b", low):
        return (
            f"shmat unencoded: unconstrained shmat is not a "
            f"proof ({engine})"
        )
    if _re_search(r"\b(?:semop|semtimedop)\b", low):
        return (
            f"semop unencoded: unconstrained semop is not a "
            f"proof ({engine})"
        )
    if _re_search(r"\b(?:msgsnd|msgrcv)\b", low):
        return (
            f"msgsnd unencoded: unconstrained msgsnd is not a "
            f"proof ({engine})"
        )
    if "sync_file_range" in low:
        return (
            f"sync_file_range unencoded: unconstrained "
            f"sync_file_range is not a proof ({engine})"
        )
    if _re_search(r"\bremap_file_pages\b", low):
        return (
            f"remap_file_pages unencoded: unconstrained "
            f"remap_file_pages is not a proof ({engine})"
        )
    if _re_search(r"\b(?:msync|mremap)\b", low):
        return (
            f"msync unencoded: unconstrained msync is not a "
            f"proof ({engine})"
        )
    if _re_search(r"\bsocketpair\b", low):
        return (
            f"socketpair unencoded: unconstrained socketpair is not a "
            f"proof ({engine})"
        )
    if _re_search(r"\bsysinfo\b", low):
        return (
            f"sysinfo unencoded: unconstrained sysinfo is not a "
            f"proof ({engine})"
        )
    if _re_search(r"\bgettid\b", low):
        return (
            f"gettid unencoded: unconstrained gettid is not a "
            f"proof ({engine})"
        )
    if "setitimer" in low or "getitimer" in low:
        return (
            f"setitimer unencoded: unconstrained setitimer is not a "
            f"proof ({engine})"
        )
    if _re_search(r"\bnice\b", low):
        return (
            f"nice unencoded: unconstrained nice is not a "
            f"proof ({engine})"
        )
    if _re_search(r"\bgetdirentries\b", low):
        return (
            f"getdirentries unencoded: unconstrained getdirentries is not a "
            f"proof ({engine})"
        )
    if _re_search(r"\bgetdents(?:64)?\b", low):
        return (
            f"getdents unencoded: unconstrained getdents is not a "
            f"proof ({engine})"
        )
    if "utimensat" in low or "futimens" in low or "utimes" in low:
        return (
            f"utimensat unencoded: unconstrained utimensat is not a "
            f"proof ({engine})"
        )
    if _re_search(r"\blinkat\b", low):
        return (
            f"linkat unencoded: unconstrained linkat is not a "
            f"proof ({engine})"
        )
    if _re_search(r"\bset_mempolicy_home_node\b", low):
        return (
            f"set_mempolicy_home_node unencoded: unconstrained "
            f"set_mempolicy_home_node is not a proof ({engine})"
        )
    if (
        "mbind" in low
        or _re_search(r"\bset_mempolicy\b", low)
        or _re_search(r"\bget_mempolicy\b", low)
    ):
        return (
            f"mbind unencoded: unconstrained mbind is not a "
            f"proof ({engine})"
        )
    if _re_search(r"\b(?:setreuid|setregid|setresuid|setresgid)\b", low):
        return (
            f"setreuid unencoded: unconstrained setreuid is not a "
            f"proof ({engine})"
        )
    if _re_search(r"\bgetres(?:uid|gid)\b", low):
        return (
            f"getresuid unencoded: unconstrained getresuid is not a "
            f"proof ({engine})"
        )
    if _re_search(r"\b(?:setfsuid|setfsgid)\b", low):
        return (
            f"setfsuid unencoded: unconstrained setfsuid is not a "
            f"proof ({engine})"
        )
    if _re_search(r"\b(?:setpgid|setsid|getsid)\b", low):
        return (
            f"setpgid unencoded: unconstrained setpgid is not a "
            f"proof ({engine})"
        )
    if _re_search(r"\bcachestat\b", low):
        return (
            f"cachestat unencoded: unconstrained cachestat is not a "
            f"proof ({engine})"
        )
    if _re_search(r"\bmap_shadow_stack\b", low):
        return (
            f"map_shadow_stack unencoded: unconstrained "
            f"map_shadow_stack is not a proof ({engine})"
        )
    if "std::pmr" in low or "pmr::" in low:
        return (
            f"C++ pmr unencoded: {engine} is not a pmr model"
        )
    if "u8string" in low:
        return (
            f"C++ u8string unencoded: {engine} is not a u8string model"
        )
    if "unordered_multimap" in low:
        return (
            f"C++ unordered_multimap unencoded: {engine} is not an "
            "unordered_multimap model"
        )
    if "unordered_multiset" in low:
        return (
            f"C++ unordered_multiset unencoded: {engine} is not an "
            "unordered_multiset model"
        )
    if "shared_lock" in low:
        return (
            f"C++ shared_lock unencoded: {engine} is not a "
            "shared_lock model"
        )
    if _re_search(r"\batomic_(?:thread|signal)_fence\b", low):
        return (
            f"C++ atomic_thread_fence unencoded: {engine} is not an "
            "atomic_thread_fence model"
        )
    if "atomic_flag" in low:
        return (
            f"C++ atomic_flag unencoded: {engine} is not an "
            "atomic_flag model"
        )
    if "recursive_timed_mutex" in low:
        return (
            f"C++ recursive_timed_mutex unencoded: {engine} is not a "
            "recursive_timed_mutex model"
        )
    if "recursive_mutex" in low:
        return (
            f"C++ recursive_mutex unencoded: {engine} is not a "
            "recursive_mutex model"
        )
    if _re_search(r"\btimed_mutex\b", low):
        return (
            f"C++ timed_mutex unencoded: {engine} is not a "
            "timed_mutex model"
        )
    if _re_search(r"\b(?:ifstream|ofstream|fstream)\b", low):
        return (
            f"C++ fstream unencoded: {engine} is not an fstream model"
        )
    if "this_thread" in low:
        return (
            f"C++ this_thread unencoded: {engine} is not a "
            "this_thread model"
        )
    if _re_search(r"\bcall_once\b", low):
        return (
            f"C++ call_once unencoded: {engine} is not a call_once model"
        )
    if "tuple" in low:
        return (
            f"C++ tuple unencoded: {engine} is not a tuple model"
        )
    if "deque" in low:
        return (
            f"C++ deque unencoded: {engine} is not a deque model"
        )
    if "forward_list" in low:
        return (
            f"C++ forward_list unencoded: {engine} is not a "
            "forward_list model"
        )
    if "std::list" in low or (
        "list<" in low
        and "initializer_list" not in low
        and "forward_list" not in low
    ):
        return (
            f"C++ std::list unencoded: {engine} is not a list model"
        )
    if "unordered_map" in low:
        return (
            f"C++ unordered_map unencoded: {engine} is not an "
            "unordered_map model"
        )
    if "std::map" in low or (
        "map<" in low
        and "flat_map" not in low
        and "unordered_map" not in low
        and "multimap" not in low
    ):
        return (
            f"C++ std::map unencoded: {engine} is not a map model"
        )
    if "unordered_set" in low:
        return (
            f"C++ unordered_set unencoded: {engine} is not an "
            "unordered_set model"
        )
    if "std::set" in low or (
        "set<" in low
        and "flat_set" not in low
        and "unordered_set" not in low
        and "multiset" not in low
    ):
        return (
            f"C++ std::set unencoded: {engine} is not a set model"
        )
    if "priority_queue" in low:
        return (
            f"C++ priority_queue unencoded: {engine} is not a "
            "priority_queue model"
        )
    if "std::queue" in low or (
        "queue<" in low
        and "priority_queue" not in low
        and "deque" not in low
    ):
        return (
            f"C++ std::queue unencoded: {engine} is not a queue model"
        )
    if ("std::stack" in low or "stack<" in low) and "stacktrace" not in low:
        return (
            f"C++ std::stack unencoded: {engine} is not a stack model"
        )
    if "to_array" in low:
        return (
            f"C++ to_array unencoded: {engine} is not a to_array model"
        )
    if _re_search(r"\bfrom_range\b", low) or "from_range unencoded" in low:
        return (
            f"C++ from_range unencoded: {engine} is not a from_range model"
        )
    if _re_search(r"\branges\s*::\s*to\b", low) or "ranges::to unencoded" in low:
        return (
            f"C++ ranges::to unencoded: {engine} is not a ranges::to model"
        )
    if "std::array" in low or (
        "array<" in low
        and "valarray" not in low
    ):
        return (
            f"C++ std::array unencoded: {engine} is not an array model"
        )
    if "wstring_convert" in low:
        return (
            f"C++ wstring_convert unencoded: {engine} is not a "
            "wstring_convert model"
        )
    if "wstring" in low:
        return (
            f"C++ wstring unencoded: {engine} is not a wstring model"
        )
    if "multimap" in low and "flat_multimap" not in low:
        return (
            f"C++ multimap unencoded: {engine} is not a multimap model"
        )
    if "multiset" in low and "flat_multiset" not in low:
        return (
            f"C++ multiset unencoded: {engine} is not a multiset model"
        )
    if "binary_semaphore" in low:
        return (
            f"C++ binary_semaphore unencoded: {engine} is not a "
            "binary_semaphore model"
        )
    if "error_category" in low:
        return (
            f"C++ error_category unencoded: {engine} is not an "
            "error_category model"
        )
    if "system_error" in low:
        return (
            f"C++ system_error unencoded: {engine} is not a "
            "system_error model"
        )
    if "error_code" in low:
        return (
            f"C++ error_code unencoded: {engine} is not an "
            "error_code model"
        )
    if "std::apply" in low:
        return (
            f"C++ std::apply unencoded: {engine} is not an apply model"
        )
    if "std::invoke" in low:
        return (
            f"C++ std::invoke unencoded: {engine} is not an invoke model"
        )
    if _re_search(r"\bstd\s*::\s*endian\b", low) or "endian unencoded" in low:
        return (
            f"C++ std::endian unencoded: {engine} is not an endian model"
        )
    if _re_search(r"\bstd\s*::\s*rot[lr]\b", low) or "rotl unencoded" in low:
        return (
            f"C++ std::rotl unencoded: {engine} is not a rotl model"
        )
    if (
        "bit_ceil" in low
        or "bit_floor" in low
        or "has_single_bit" in low
        or "std::popcount" in low
    ):
        return (
            f"C++ bit_ceil unencoded: {engine} is not a bit_ceil model"
        )
    if _re_search(r"\bstd\s*::\s*bit_width\b", low) or "bit_width unencoded" in low:
        return (
            f"C++ std::bit_width unencoded: {engine} is not a bit_width model"
        )
    if _re_search(r"\bstd\s*::\s*gcd\b", low) or "gcd unencoded" in low:
        return (
            f"C++ std::gcd unencoded: {engine} is not a gcd model"
        )
    if _re_search(r"\bstd\s*::\s*lcm\b", low) or "lcm unencoded" in low:
        return (
            f"C++ std::lcm unencoded: {engine} is not a lcm model"
        )
    if _re_search(r"\bstd\s*::\s*clamp\b", low) or "clamp unencoded" in low:
        return (
            f"C++ std::clamp unencoded: {engine} is not a clamp model"
        )
    if _re_search(r"\bstd\s*::\s*exchange\b", low) or "exchange unencoded" in low:
        return (
            f"C++ std::exchange unencoded: {engine} is not an exchange model"
        )
    if _re_search(r"\bstd\s*::\s*to_address\b", low) or "to_address unencoded" in low:
        return (
            f"C++ std::to_address unencoded: {engine} is not a "
            "to_address model"
        )
    if _re_search(r"\b(?:construct_at|destroy_at)\b", low) or (
        "construct_at unencoded" in low
    ):
        return (
            f"C++ construct_at unencoded: {engine} is not a "
            "construct_at model"
        )
    if _re_search(r"\bdestroy_n\b", low) or "destroy_n unencoded" in low:
        return (
            f"C++ destroy_n unencoded: {engine} is not a destroy_n model"
        )
    if _re_search(r"\b(?:std\s*::\s*)?addressof\b", low) or (
        "addressof unencoded" in low
    ):
        return (
            f"C++ std::addressof unencoded: {engine} is not an "
            "addressof model"
        )
    if _re_search(r"\bas_rvalue(?:_view)?\b", low) or "as_rvalue unencoded" in low:
        return (
            f"C++ as_rvalue unencoded: {engine} is not an as_rvalue model"
        )
    if _re_search(r"\bas_const\b", low) or "as_const unencoded" in low:
        return (
            f"C++ as_const unencoded: {engine} is not an as_const model"
        )
    if (
        _re_search(r"\btransform_(?:inclusive|exclusive)_scan\b", low)
        or "transform_inclusive_scan unencoded" in low
        or "transform_exclusive_scan unencoded" in low
    ):
        return (
            f"C++ transform_inclusive_scan unencoded: {engine} is not a "
            "transform_inclusive_scan model"
        )
    if _re_search(r"\bexclusive_scan\b", low) or "exclusive_scan unencoded" in low:
        return (
            f"C++ exclusive_scan unencoded: {engine} is not an "
            "exclusive_scan model"
        )
    if _re_search(r"\binclusive_scan\b", low) or "inclusive_scan unencoded" in low:
        return (
            f"C++ inclusive_scan unencoded: {engine} is not an "
            "inclusive_scan model"
        )
    if _re_search(r"\btransform_reduce\b", low) or "transform_reduce unencoded" in low:
        return (
            f"C++ transform_reduce unencoded: {engine} is not a "
            "transform_reduce model"
        )
    if _re_search(r"\bstd\s*::\s*reduce\b", low) or "std::reduce unencoded" in low:
        return (
            f"C++ std::reduce unencoded: {engine} is not a reduce model"
        )
    if _re_search(
        r"\buninitialized_(?:fill(?:_n)?|default_construct(?:_n)?)\b",
        low,
    ) or "uninitialized_fill unencoded" in low:
        return (
            f"C++ uninitialized_fill unencoded: {engine} is not an "
            "uninitialized_fill model"
        )
    if _re_search(r"\buninitialized_value_construct(?:_n)?\b", low) or (
        "uninitialized_value_construct unencoded" in low
    ):
        return (
            f"C++ uninitialized_value_construct unencoded: {engine} is not an "
            "uninitialized_value_construct model"
        )
    if _re_search(r"\buninitialized_(?:copy|move)(?:_n)?\b", low) or (
        "uninitialized_copy unencoded" in low
    ):
        return (
            f"C++ uninitialized_copy unencoded: {engine} is not an "
            "uninitialized_copy model"
        )
    if (
        _re_search(r"\b(?:add_sat|sub_sat|mul_sat|div_sat|saturate_cast)\b", low)
        or "add_sat unencoded" in low
    ):
        return (
            f"C++ add_sat unencoded: {engine} is not an add_sat model"
        )
    if _re_search(r"\bnontype\b", low) or "nontype unencoded" in low:
        return (
            f"C++ nontype unencoded: {engine} is not a nontype model"
        )
    if _re_search(r"\bis_layout_compatible\b", low) or (
        "is_layout_compatible unencoded" in low
    ):
        return (
            f"C++ is_layout_compatible unencoded: {engine} is not an "
            "is_layout_compatible model"
        )
    if _re_search(r"\bis_pointer_interconvertible_(?:with_class|base_of)\b", low) or (
        "is_pointer_interconvertible unencoded" in low
    ):
        return (
            f"C++ is_pointer_interconvertible unencoded: {engine} is not an "
            "is_pointer_interconvertible model"
        )
    if _re_search(r"\bbasic_const_iterator\b", low) or (
        "basic_const_iterator unencoded" in low
    ):
        return (
            f"C++ basic_const_iterator unencoded: {engine} is not a "
            "basic_const_iterator model"
        )
    if _re_search(r"\bis_corresponding_member\b", low) or (
        "is_corresponding_member unencoded" in low
    ):
        return (
            f"C++ is_corresponding_member unencoded: {engine} is not an "
            "is_corresponding_member model"
        )
    if _re_search(r"\bforward_like\b", low) or "forward_like unencoded" in low:
        return (
            f"C++ forward_like unencoded: {engine} is not a "
            "forward_like model"
        )
    if _re_search(r"\b(?:set|get)_terminate\b", low) or (
        "set_terminate unencoded" in low
    ):
        return (
            f"C++ set_terminate unencoded: {engine} is not a "
            "set_terminate model"
        )
    if _re_search(r"\bis_constant_evaluated\b", low) or (
        "is_constant_evaluated unencoded" in low
    ):
        return (
            f"C++ is_constant_evaluated unencoded: {engine} is not an "
            "is_constant_evaluated model"
        )
    if _re_search(r"\bstd\s*::\s*lerp\b", low) or "lerp unencoded" in low:
        return (
            f"C++ std::lerp unencoded: {engine} is not a lerp model"
        )
    if _re_search(r"\bstd\s*::\s*midpoint\b", low) or "midpoint unencoded" in low:
        return (
            f"C++ std::midpoint unencoded: {engine} is not a midpoint model"
        )
    if _re_search(
        r"\bstd\s*::\s*(?:cmp_(?:less|greater|less_equal|"
        r"greater_equal|equal_to|not_equal_to)|in_range)\b",
        low,
    ) or "cmp_less unencoded" in low:
        return (
            f"C++ std::cmp_less unencoded: {engine} is not a cmp_less model"
        )
    if _re_search(r"\bstd\s*::\s*count[lr]_(?:zero|one)\b", low) or (
        "countl_zero unencoded" in low
    ):
        return (
            f"C++ std::countl_zero unencoded: {engine} is not a "
            "countl_zero model"
        )
    if "byteswap" in low:
        return (
            f"C++ byteswap unencoded: {engine} is not a byteswap model"
        )
    if "to_chars" in low and "from_chars" not in low:
        return (
            f"C++ to_chars unencoded: {engine} is not a charconv model"
        )
    if "hazard_pointer" in low:
        return (
            f"C++ hazard_pointer unencoded: {engine} is not a "
            "hazard_pointer model"
        )
    if "text_encoding" in low:
        return (
            f"C++ text_encoding unencoded: {engine} is not a "
            "text_encoding model"
        )
    if "simd" in low:
        return (
            f"C++ simd unencoded: {engine} is not a simd model"
        )
    if "hive" in low:
        return (
            f"C++ hive unencoded: {engine} is not a hive model"
        )
    if "packaged_task" in low:
        return (
            f"C++ packaged_task unencoded: {engine} is not a "
            "packaged_task model"
        )
    if "osyncstream" in low:
        return (
            f"C++ osyncstream unencoded: {engine} is not an "
            "osyncstream model"
        )
    if "syncbuf" in low:
        return (
            f"C++ syncbuf unencoded: {engine} is not a syncbuf model"
        )
    if _re_search(r"\b(?:views\s*::\s*counted\b|\bcounted_view\b)", low) or (
        "counted_view unencoded" in low or "views::counted unencoded" in low
    ):
        return (
            f"C++ views::counted unencoded: {engine} is not a "
            "counted_view model"
        )
    if "counted_iterator" in low:
        return (
            f"C++ counted_iterator unencoded: {engine} is not a "
            "counted_iterator model"
        )
    if "weak_ptr" in low:
        return (
            f"C++ weak_ptr unencoded: {engine} is not a weak_ptr model"
        )
    if "nested_exception" in low:
        return (
            f"C++ nested_exception unencoded: {engine} is not a "
            "nested_exception model"
        )
    if "throw_with_nested" in low or "rethrow_if_nested" in low:
        return (
            f"throw_with_nested unencoded: unconstrained "
            f"throw_with_nested is not a proof ({engine})"
        )
    if "uncaught_exceptions" in low:
        return (
            f"C++ uncaught_exceptions unencoded: {engine} is not an "
            "uncaught_exceptions model"
        )
    if "current_exception" in low:
        return (
            f"C++ current_exception unencoded: {engine} is not a "
            "current_exception model"
        )
    if _re_search(r"\bmake_exception_ptr\b", low) or (
        "make_exception_ptr unencoded" in low
    ):
        return (
            f"C++ make_exception_ptr unencoded: {engine} is not a "
            "make_exception_ptr model"
        )
    if "exception_ptr" in low:
        return (
            f"C++ exception_ptr unencoded: {engine} is not an "
            "exception_ptr model"
        )
    if "coroutine_handle" in low:
        return (
            f"C++ coroutine_handle unencoded: {engine} is not a "
            "coroutine_handle model"
        )
    if "valarray" in low:
        return (
            f"C++ valarray unencoded: {engine} is not a valarray model"
        )
    if "to_underlying" in low:
        return (
            f"C++ to_underlying unencoded: {engine} is not a "
            "to_underlying model"
        )
    if "unexpected<" in low or "unexpected <" in low:
        return (
            f"C++ unexpected unencoded: {engine} is not an unexpected model"
        )
    if "std::task" in low or "execution::task" in low or "task unencoded" in low:
        return (
            f"C++ std::task unencoded: {engine} is not a task model"
        )
    if "execution" in low:
        return (
            f"C++ execution unencoded: {engine} is not an execution model"
        )
    if "indirect" in low or "polymorphic" in low:
        return (
            f"C++ indirect unencoded: {engine} is not an "
            "indirect/polymorphic model"
        )
    if "bitset" in low:
        return (
            f"C++ bitset unencoded: {engine} is not a bitset model"
        )
    if "stringstream" in low:
        return (
            f"C++ stringstream unencoded: {engine} is not a "
            "stringstream model"
        )
    if "spanstream" in low:
        return (
            f"C++ spanstream unencoded: {engine} is not a "
            "spanstream model"
        )
    if "out_ptr" in low or "inout_ptr" in low:
        return (
            f"C++ out_ptr unencoded: {engine} is not an out_ptr model"
        )
    if "rcu unencoded" in low or "rcu_obj" in low or "rcu_synchronize" in low:
        return (
            f"C++ rcu unencoded: {engine} is not an rcu model"
        )
    if "linalg" in low:
        return (
            f"C++ linalg unencoded: {engine} is not a linalg model"
        )
    if "sync_wait" in low:
        return (
            f"C++ sync_wait unencoded: {engine} is not an execution model"
        )
    if "#embed" in low or "embed unencoded" in low:
        return (
            f"C++ #embed unencoded: {engine} is not an embed model"
        )
    if "contracts unencoded" in low or "contract_assert" in low:
        return (
            f"C++ contracts unencoded: {engine} is not a contracts model"
        )
    if (
        "reflection unencoded" in low
        or "define_aggregate" in low
        or "define_class" in low
        or "std::meta" in low
    ):
        return (
            f"C++ reflection unencoded: {engine} is not a reflection model"
        )
    return None


def _has_unencoded_throw(fn: FunctionInfo) -> bool:
    """C++ throw is not in the bitvector encoder. Missing model, not ERROR."""
    return bool(_re_search(r"\bthrow\b", fn.body or ""))


def _has_unencoded_setjmp(fn: FunctionInfo) -> bool:
    """setjmp/longjmp/va_list are not in the bitvector encoder.

    Missing nonlocal-control / variadic model, not a parse ERROR and
    not a proof. Plain `goto` is not this case.
    """
    return bool(_re_search(
        r"\b(?:setjmp|longjmp|va_list|va_start)\b",
        fn.body or "",
    ))


def bmc_function(
    fn: FunctionInfo,
    unwind: int,
    try_unbounded: bool = True,
    enums: dict[str, int] | None = None,
    allow_local_pointers: bool = False,
    incremental: bool = True,
) -> Finding:
    base: dict[str, Any] = dict(stage="bmc", file=fn.file, function=fn.name, line=fn.line,
                cls="", strength=laws.STRENGTH_PROVES, extra={})
    if not HAS_Z3:
        base["extra"] = {"install": "pip install z3-solver"}
        return Finding(
            **base,
            status=laws.NOTRUN,
            message="z3 not installed",
        )
    if fn.kind == "POINTER":
        base["strength"] = laws.STRENGTH_SOME
        return Finding(**base, status=laws.NEEDS_HARNESS,
                       message="pointer parameter: unguarded BMC reports missing preconditions, not defects")
    if fn.kind == "OTHER":
        base["strength"] = laws.STRENGTH_SOME
        return Finding(**base, status=laws.NEEDS_HARNESS,
                       message="non-scalar parameter: unguarded BMC reports missing preconditions, not defects")
    syn = unencoded_syntax_reason(fn, "bitvector BMC")
    if syn:
        base["strength"] = laws.STRENGTH_SOME
        return Finding(**base, status=laws.NEEDS_HARNESS, message=syn)
    if not allow_local_pointers and body_needs_pointer_harness(fn.body):
        base["strength"] = laws.STRENGTH_SOME
        return Finding(
            **base, status=laws.NEEDS_HARNESS,
            message="local pointer or heap object: unguarded BMC reports missing preconditions, not defects",
        )
    if _has_self_call(fn):
        base["strength"] = laws.STRENGTH_SOME
        return Finding(
            **base, status=laws.NEEDS_HARNESS,
            message="recursive call unencoded: unconstrained result is not a proof of the callee",
        )
    if _has_unencoded_float(fn):
        base["strength"] = laws.STRENGTH_SOME
        return Finding(
            **base, status=laws.NEEDS_HARNESS,
            message="float/double unencoded: bitvector BMC is not an IEEE model",
        )
    if _has_unencoded_cxx(fn):
        base["strength"] = laws.STRENGTH_SOME
        return Finding(
            **base, status=laws.NEEDS_HARNESS,
            message="C++ view/span unencoded: bitvector BMC is not a lifetime model",
        )
    if _has_unencoded_cstr(fn):
        base["strength"] = laws.STRENGTH_SOME
        return Finding(
            **base, status=laws.NEEDS_HARNESS,
            message="libc string copy unencoded: unconstrained call is not a proof of the buffer",
        )
    if _has_unencoded_throw(fn):
        base["strength"] = laws.STRENGTH_SOME
        return Finding(
            **base, status=laws.NEEDS_HARNESS,
            message="C++ throw unencoded: bitvector BMC is not an exception model",
        )
    if _has_unencoded_setjmp(fn):
        base["strength"] = laws.STRENGTH_SOME
        return Finding(
            **base, status=laws.NEEDS_HARNESS,
            message="setjmp/longjmp/va_list unencoded: bitvector BMC is not a nonlocal-control model",
        )
    if enums is None:
        enums = _enums_from_fn(fn)
    macros = _macros_from_fn(fn)
    schedule = _unwind_schedule(unwind) if incremental else [unwind]
    last: Finding | None = None
    tried: list[int] = []
    for k in schedule:
        rec = _bmc_once(fn, k, try_unbounded, enums, base, macros=macros)
        extra = dict(rec.extra or {})
        tried.append(k)
        extra["incremental_k"] = k
        extra["incremental"] = list(tried)
        rec.extra = extra
        if rec.status != laws.BOUNDED:
            return rec
        last = rec
    return last if last is not None else Finding(
        **base, status=laws.ERROR, message="empty unwind schedule",
    )


_CTOR_NAME = re.compile(r"(?:^|::)([A-Za-z_]\w*)::\1$")


def _dynamic_init_before_main(functions: list[FunctionInfo], out: list[Finding]) -> None:
    """A C++ constructor defined in the analysed code may run before main (a
    global object's dynamic initialisation) and fail or throw there; bmc does
    not model static initialisation, so a proof of `main` is not the
    program's: NEEDS-HARNESS (docs/CONFORMANCE.md S8). Same rule in bmc.cpp.
    """
    ctor_name = ""
    for fn in functions:
        if _CTOR_NAME.search(fn.name):
            ctor_name = fn.name
            break
    if not ctor_name:
        return
    for f in out:
        if f.function != "main" or f.status not in (laws.PROVED, laws.PROVED_UNBOUNDED, laws.BOUNDED):
            continue
        extra = dict(f.extra or {})
        extra["verdict_before_static_init"] = f.status
        f.extra = extra
        f.status = laws.NEEDS_HARNESS
        f.strength = laws.STRENGTH_SOME
        f.message = (f"dynamic initialisation before main unencoded: constructor {ctor_name} "
                     "may run for a global object; bitvector BMC does not model static initialisation")


def run_bmc(functions: list[FunctionInfo], unwind: int) -> list[Finding]:
    from prism.inline import inline_static
    out: list[Finding] = []
    for fn in inline_static(functions):
        # R1: one function the encoder cannot handle (a Z3 sort error, ...)
        # is an ERROR for that function, never a crash of the whole stage.
        try:
            out.append(k_induction(fn, unwind))
        except Exception as ex:  # noqa: BLE001 - Law 7: record, never lose the stage
            out.append(Finding(
                stage="bmc", status=laws.ERROR, file=fn.file, function=fn.name,
                line=fn.line, cls="", message=f"BMC internal error: {ex}",
                strength=laws.STRENGTH_SOME, extra={},
            ))
    _dynamic_init_before_main(functions, out)
    return out
