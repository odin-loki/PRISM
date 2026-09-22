"""ESBMC-method BMC over a SCALAR C subset, via Z3.

Statuses are the ParanoidBSD set. POINTER functions are NEEDS-HARNESS.
Missing Z3 is NOTRUN. Unparseable is ERROR, never a proof.

Supported: ints, constant-size arrays, if/else, while/for/do-while with
bounded unwind, for-header decls (`for (int i = 0; ...)`), ++/-- (add/sub
1 with signed overflow), switch/case/break/default (C fallthrough),
enum { NAME = val }, assert(), sizeof, ternary `?:`, comma operator,
continue, +, -, *, /, %, <<, >>, comparisons, assignments. Uninitialised
locals are tracked; a read is UNINIT-READ. `goto` is unencoded ERROR,
never a proof. A VLA is NEEDS-HARNESS (missing bound), not a closed
proof. A recursive self-call is NEEDS-HARNESS: an unconstrained
result is not a proof of the callee, and arithmetic on that havoc
is not a counterexample of the original. `return buf` of a local
array is NEEDS-HARNESS (dangling decay), like `return &x`. `alloca`
/ `__builtin_alloca` is NEEDS-HARNESS (unmodeled stack frame), like
malloc. C++ `throw` is NEEDS-HARNESS (missing exception model), never
a parse ERROR and never a proof. `setjmp`/`longjmp`/`va_list`/`va_start`/`va_arg`
are NEEDS-HARNESS (missing nonlocal/variadic model), never a parse
ERROR and never a proof; `goto` stays ERROR and is not that case.
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

from dataclasses import dataclass, replace
from pathlib import Path
from typing import Any
import re

from prism import laws
from prism.cparse import body_needs_pointer_harness
from prism.models import Finding, FunctionInfo

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
    if "long long" in t or re.search(r"\b[iu]nt64_t\b", t):
        return 64
    return WIDTH


def _bv_min(w: int):
    return z3.BitVecVal(-(1 << (w - 1)), w)


def _bv_zero(w: int):
    return z3.BitVecVal(0, w)


def _width_of(e: "_Enc", v: Any) -> int:
    return int(e.wtag.get(id(v), WIDTH))


def _resize(v: Any, src: int, dst: int, unsigned: bool) -> Any:
    if src == dst:
        return v
    if dst > src:
        return z3.ZeroExt(dst - src, v) if unsigned else z3.SignExt(dst - src, v)
    return z3.Extract(dst - 1, 0, v)


def _align_pair(e: "_Enc", a: Any, b: Any) -> tuple[Any, Any, int]:
    wa, wb = _width_of(e, a), _width_of(e, b)
    w = max(wa, wb)
    ua, ub = _is_u(e, a), _is_u(e, b)
    if wa < w:
        a = _resize(a, wa, w, ua)
        e.wtag[id(a)] = w
        if ua:
            e.utag[id(a)] = True
    if wb < w:
        b = _resize(b, wb, w, ub)
        e.wtag[id(b)] = w
        if ub:
            e.utag[id(b)] = True
    return a, b, w


def _tag(e: "_Enc", v: Any, unsigned: bool = False, width: int | None = None) -> Any:
    e.utag[id(v)] = bool(unsigned)
    if width is not None:
        e.wtag[id(v)] = int(width)
    return v


def _is_u(e: "_Enc", v: Any) -> bool:
    return bool(e.utag.get(id(v), False))


def _oob(e: "_Enc", i: Any, n: int) -> Any:
    """Unsigned index cannot be negative; C usual conversions apply."""
    bound = z3.BitVecVal(n, WIDTH)
    if _is_u(e, i):
        return z3.UGE(i, bound)
    return z3.Or(slt(i, 0), sge(i, n))


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
        self.arrays: dict[str, tuple[Any, int]] = {}
        self.uninit: dict[str, Any] = {}  # name -> Bool, true = maybe uninit
        self.props: list[Prop] = []
        self.pc = 0
        self.fresh = 0
        self.unwind_ok = True
        self.path_true = z3.BoolVal(True)
        self.unsigned: set[str] = set()
        self.utag: dict[int, bool] = {}
        self.bits: dict[str, int] = {}
        self.wtag: dict[int, int] = {}

    def retag_unsigned(self) -> None:
        for n in self.unsigned:
            v = self.vars.get(n)
            if v is not None:
                self.utag[id(v)] = True
        for n, w in self.bits.items():
            v = self.vars.get(n)
            if v is not None:
                self.wtag[id(v)] = w

    def bv(self, name: str | None = None, width: int | None = None) -> Any:
        self.fresh += 1
        w = width or WIDTH
        v = z3.BitVec(name or f"t{self.fresh}", w)
        self.wtag[id(v)] = w
        return v

    def get(self, name: str) -> Any:
        w = self.bits.get(name, WIDTH)
        if name not in self.vars:
            self.vars[name] = self.bv(name, w)
        v = self.vars[name]
        self.wtag[id(v)] = w
        if name in self.unsigned:
            self.utag[id(v)] = True
        return v

    def set(self, name: str, val: Any) -> None:
        w = self.bits.get(name, WIDTH)
        vw = _width_of(self, val)
        if vw != w:
            val = _resize(val, vw, w, name in self.unsigned)
        self.vars[name] = val
        self.wtag[id(val)] = w
        if name in self.unsigned:
            self.utag[id(val)] = True

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

    def add_prop(self, name: str, cls: str, viol: Any, loc: int) -> None:
        self.props.append(Prop(name, cls, z3.And(self.path_true, viol), loc))

    def assume(self, cond: Any) -> None:
        self.path_true = z3.And(self.path_true, cond)


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


def extract_enums(text: str) -> dict[str, int]:
    """Collect `enum { NAME = val, ... }` (and implicit 0,1,2,...) constants."""
    try:
        from prism.cparse import strip_comments_keep_lines
        text = strip_comments_keep_lines(text)
    except Exception:
        text = re.sub(r"/\*.*?\*/", " ", text, flags=re.S)
        text = re.sub(r"//.*?$", " ", text, flags=re.M)
    out: dict[str, int] = {}
    for m in re.finditer(r"\benum\b(?:\s+[A-Za-z_]\w*)?\s*\{([^{}]*)\}", text):
        nxt = 0
        for part in m.group(1).split(","):
            part = " ".join(part.split())
            if not part:
                continue
            if "=" in part:
                name, val = part.split("=", 1)
                name, val = name.strip(), val.strip().rstrip("uUlL")
                if not _is_ident(name):
                    continue
                try:
                    nxt = int(val, 0)
                except ValueError:
                    if val in out:
                        nxt = out[val]
                    else:
                        continue
                out[name] = nxt
                nxt += 1
            elif _is_ident(part):
                out[part] = nxt
                nxt += 1
    return out


def _enums_from_fn(fn: FunctionInfo) -> dict[str, int]:
    path = Path(fn.file)
    if not path.is_file():
        return {}
    try:
        return extract_enums(path.read_text(encoding="utf-8", errors="replace"))
    except OSError:
        return {}


@dataclass
class _SwitchArm:
    labels: list[Any]  # z3 BitVec case value, or None for default
    code: str
    stops: bool  # arm ends with break (no fallthrough)


class Parser:
    """Recursive-descent over a statement list. Not a C compiler."""

    def __init__(
        self,
        body: str,
        params: list[tuple[str, str]],
        unwind: int,
        enums: dict[str, int] | None = None,
    ) -> None:
        self.body = body
        self.params = params
        self.unwind = unwind
        self.enums: dict[str, int] = dict(enums or {})
        self.err: str | None = None

    def run(self) -> _Enc | None:
        if not HAS_Z3:
            return None
        e = _Enc(self.unwind)
        for typ, name in self.params:
            if not name:
                continue
            if _type_is_unsigned(typ):
                e.unsigned.add(name)
            e.bits[name] = _type_width(typ)
            e.get(name)  # unconstrained input
        try:
            self._stmts(e, self._prep(self.body))
        except ParseFail as ex:
            self.err = str(ex)
            return None
        return e

    def _prep(self, body: str) -> str:
        body = re.sub(r"#.*", " ", body)
        return body

    def _stmts(self, e: _Enc, text: str) -> None:
        text = text.strip()
        while text:
            text = text.lstrip()
            if not text:
                break
            if text.startswith("{"):
                inner, rest = _brace(text)
                self._stmts(e, inner)
                text = rest
                continue
            if re.match(r"if\s+constexpr\b", text):
                raise ParseFail("if constexpr unencoded")
            if re.match(r"constexpr\b", text):
                raise ParseFail("constexpr unencoded")
            if "<=>" in text:
                raise ParseFail("spaceship unencoded")
            if re.search(r"__attribute__\s*\(\s*\(\s*cleanup", text):
                raise ParseFail("cleanup unencoded")
            if re.search(r"\bstd\s*::\s*expected\b|\bexpected\s*<", text):
                raise ParseFail("expected unencoded")
            if re.search(r"\bformat_to(?:_n)?\s*\(", text):
                raise ParseFail("format_to unencoded")
            if re.search(r"\bstd\s*::\s*(?:format|print|println)\b", text):
                raise ParseFail("format unencoded")
            if re.search(r"\bstd\s*::\s*jthread\b", text):
                raise ParseFail("jthread unencoded")
            if re.search(
                r"\bstd\s*::\s*(?:async|future|promise)\b"
                r"|(?:future|promise)\s*<",
                text,
            ):
                raise ParseFail("async unencoded")
            if re.search(r"\bstd\s*::\s*function\b|function\s*<", text):
                raise ParseFail("function unencoded")
            if re.search(r"\bstd\s*::\s*mdspan\b|mdspan\s*<", text):
                raise ParseFail("mdspan unencoded")
            if re.search(
                r"\bstd\s*::\s*(?:mutex|lock_guard|unique_lock|scoped_lock)\b"
                r"|(?:lock_guard|unique_lock|scoped_lock)\s*<",
                text,
            ):
                raise ParseFail("std mutex unencoded")
            if re.search(r"\bcondition_variable_any\b", text):
                raise ParseFail("condition_variable_any unencoded")
            if re.search(r"\bshared_timed_mutex\b", text):
                raise ParseFail("shared_timed_mutex unencoded")
            if re.search(r"\brecursive_timed_mutex\b", text):
                raise ParseFail("recursive_timed_mutex unencoded")
            if re.search(r"\berror_category\b", text):
                raise ParseFail("error_category unencoded")
            if re.search(r"\bnested_exception\b", text):
                raise ParseFail("nested_exception unencoded")
            if re.search(r"\bwstring_convert\b", text):
                raise ParseFail("wstring_convert unencoded")
            if re.search(r"\bsystem_error\b", text):
                raise ParseFail("system_error unencoded")
            if re.search(r"\b(?:current_zone|tzdb)\b", text):
                raise ParseFail("tzdb unencoded")
            if re.search(r"\bis_scoped_enum\b", text):
                raise ParseFail("is_scoped_enum unencoded")
            if re.search(r"\b(?:views\s*::\s*)?enumerate(?:_view)?\b", text):
                raise ParseFail("enumerate unencoded")
            if re.search(r"\bcartesian_product(?:_view)?\b", text):
                raise ParseFail("cartesian_product unencoded")
            if re.search(
                r"\b(?:views\s*::\s*chunk(?:_by)?|chunk(?:_by|_view))\b",
                text,
            ):
                raise ParseFail("chunk unencoded")
            if re.search(r"\b(?:views\s*::\s*slide|slide_view)\b", text):
                raise ParseFail("slide unencoded")
            if re.search(
                r"\b(?:views\s*::\s*adjacent(?:_transform)?|"
                r"adjacent(?:_transform|_view))\b",
                text,
            ):
                raise ParseFail("adjacent unencoded")
            if re.search(r"\bjoin_with(?:_view)?\b", text):
                raise ParseFail("join_with unencoded")
            if re.search(r"views\s*::\s*join|\bjoin_view\b", text):
                raise ParseFail("views::join unencoded")
            if re.search(r"\bzip_transform(?:_view)?\b", text):
                raise ParseFail("zip_transform unencoded")
            if re.search(r"views\s*::\s*zip|\bzip_view\b", text):
                raise ParseFail("views::zip unencoded")
            if re.search(r"\bas_rvalue(?:_view)?\b", text):
                raise ParseFail("as_rvalue unencoded")
            if re.search(r"\bfrom_range\b", text):
                raise ParseFail("from_range unencoded")
            if re.search(r"\b(?:views\s*::\s*)?stride(?:_view)?\b", text):
                raise ParseFail("stride unencoded")
            if re.search(r"\b(?:views\s*::\s*repeat|repeat_view)\b", text):
                raise ParseFail("repeat unencoded")
            if re.search(r"\b(?:views\s*::\s*take\b|\btake_view\b)", text):
                raise ParseFail("take unencoded")
            if re.search(r"\b(?:views\s*::\s*drop\b|\bdrop_view\b)", text):
                raise ParseFail("drop unencoded")
            if re.search(r"\b(?:views\s*::\s*filter\b|\bfilter_view\b)", text):
                raise ParseFail("filter unencoded")
            if re.search(
                r"\b(?:views\s*::\s*transform\b|\btransform_view\b)",
                text,
            ):
                raise ParseFail("transform_view unencoded")
            if re.search(
                r"\b(?:views\s*::\s*elements\b|\belements_view\b)",
                text,
            ):
                raise ParseFail("elements unencoded")
            if re.search(r"\b(?:views\s*::\s*iota\b|\biota_view\b)", text):
                raise ParseFail("iota unencoded")
            if re.search(r"\breference_wrapper\b", text):
                raise ParseFail("reference_wrapper unencoded")
            if re.search(r"\bstd\s*::\s*endian\b", text):
                raise ParseFail("std::endian unencoded")
            if re.search(r"\bstd\s*::\s*apply\s*\(", text):
                raise ParseFail("std::apply unencoded")
            if re.search(
                r"\b(?:bit_ceil|bit_floor|has_single_bit|"
                r"std\s*::\s*popcount)\s*\(",
                text,
            ):
                raise ParseFail("bit_ceil unencoded")
            if re.search(r"\bstd\s*::\s*bit_width\s*\(", text):
                raise ParseFail("bit_width unencoded")
            if re.search(r"\bstd\s*::\s*gcd\s*\(", text):
                raise ParseFail("gcd unencoded")
            if re.search(r"\bstd\s*::\s*lcm\s*\(", text):
                raise ParseFail("lcm unencoded")
            if re.search(r"\bstd\s*::\s*clamp\s*\(", text):
                raise ParseFail("clamp unencoded")
            if re.search(r"\bstd\s*::\s*exchange\s*\(", text):
                raise ParseFail("exchange unencoded")
            if re.search(r"\bstd\s*::\s*to_address\s*\(", text):
                raise ParseFail("to_address unencoded")
            if re.search(r"\bstd\s*::\s*addressof\s*\(", text):
                raise ParseFail("addressof unencoded")
            if re.search(r"\bassume_aligned\s*\(", text):
                raise ParseFail("assume_aligned unencoded")
            if re.search(r"\bas_const\s*\(", text):
                raise ParseFail("as_const unencoded")
            if re.search(r"\btransform_(?:inclusive|exclusive)_scan\s*\(", text):
                raise ParseFail("transform_inclusive_scan unencoded")
            if re.search(r"\bexclusive_scan\s*\(", text):
                raise ParseFail("exclusive_scan unencoded")
            if re.search(r"\binclusive_scan\s*\(", text):
                raise ParseFail("inclusive_scan unencoded")
            if re.search(r"\btransform_reduce\s*\(", text):
                raise ParseFail("transform_reduce unencoded")
            if re.search(r"\bstd\s*::\s*reduce\s*\(", text):
                raise ParseFail("std::reduce unencoded")
            if re.search(
                r"\buninitialized_(?:fill(?:_n)?|default_construct(?:_n)?)\s*\(",
                text,
            ):
                raise ParseFail("uninitialized_fill unencoded")
            if re.search(r"\buninitialized_value_construct(?:_n)?\s*\(", text):
                raise ParseFail("uninitialized_value_construct unencoded")
            if re.search(r"\buninitialized_(?:copy|move)(?:_n)?\s*\(", text):
                raise ParseFail("uninitialized_copy unencoded")
            if re.search(r"\b(?:construct_at|destroy_at)\s*\(", text):
                raise ParseFail("construct_at unencoded")
            if re.search(r"\bdestroy_n\s*\(", text):
                raise ParseFail("destroy_n unencoded")
            if re.search(
                r"\b(?:add_sat|sub_sat|mul_sat|div_sat|saturate_cast)\s*\(",
                text,
            ):
                raise ParseFail("add_sat unencoded")
            if re.search(r"\btype_identity\b", text):
                raise ParseFail("type_identity unencoded")
            if re.search(r"\bnontype\b", text):
                raise ParseFail("nontype unencoded")
            if re.search(r"\bis_layout_compatible\b", text):
                raise ParseFail("is_layout_compatible unencoded")
            if re.search(r"\bis_pointer_interconvertible_(?:with_class|base_of)\b", text):
                raise ParseFail("is_pointer_interconvertible unencoded")
            if re.search(r"\bbasic_const_iterator\b", text):
                raise ParseFail("basic_const_iterator unencoded")
            if re.search(r"\bis_corresponding_member\b", text):
                raise ParseFail("is_corresponding_member unencoded")
            if re.search(r"\branges\s*::\s*to\s*[<(]", text):
                raise ParseFail("ranges::to unencoded")
            if re.search(r"\bforward_like\b", text):
                raise ParseFail("forward_like unencoded")
            if re.search(r"\bmake_exception_ptr\s*\(", text):
                raise ParseFail("make_exception_ptr unencoded")
            if re.search(r"\b(?:set|get)_terminate\s*\(", text):
                raise ParseFail("set_terminate unencoded")
            if re.search(r"\bis_constant_evaluated\s*\(", text):
                raise ParseFail("is_constant_evaluated unencoded")
            if re.search(r"\bstd\s*::\s*lerp\s*\(", text):
                raise ParseFail("lerp unencoded")
            if re.search(r"\bstd\s*::\s*midpoint\s*\(", text):
                raise ParseFail("midpoint unencoded")
            if re.search(
                r"\bstd\s*::\s*(?:cmp_(?:less|greater|less_equal|"
                r"greater_equal|equal_to|not_equal_to)|in_range)\s*\(",
                text,
            ):
                raise ParseFail("cmp_less unencoded")
            if re.search(r"\bstd\s*::\s*count[lr]_(?:zero|one)\s*\(", text):
                raise ParseFail("countl_zero unencoded")
            if re.search(r"\bstd\s*::\s*unreachable\s*\(", text):
                raise ParseFail("std::unreachable unencoded")
            if re.search(r"\buncaught_exceptions\s*\(", text):
                raise ParseFail("uncaught_exceptions unencoded")
            if re.search(
                r"\bstd\s*::\s*(?:condition_variable|shared_mutex)\b"
                r"|\b(?:condition_variable|shared_mutex)\b",
                text,
            ):
                raise ParseFail("condition_variable unencoded")
            if re.search(r"\bstd\s*::\s*atomic_ref\b|atomic_ref\s*<", text):
                raise ParseFail("atomic_ref unencoded")
            if re.search(r"\bstd\s*::\s*generator\b|generator\s*<", text):
                raise ParseFail("generator unencoded")
            if re.search(r"\[\[\s*assume\s*\(", text):
                raise ParseFail("assume unencoded")
            if re.search(r"\bstd\s*::\s*bind\s*\(", text):
                raise ParseFail("std bind unencoded")
            if re.search(
                r"__attribute__\s*\(\s*\(\s*(?:__)?vector_size"
                r"|\b__vector_size\b",
                text,
            ):
                raise ParseFail("vector_size unencoded")
            if re.match(r"requires\s*\(", text) or re.match(r"concept\s+", text):
                raise ParseFail("concepts unencoded")
            if re.search(r"\(\s*\.\.\.\s*[+\-|&^]|[+\-|&^]\s*\.\.\.\s*\)", text):
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
                stmt, text = _stmt(text)
                # terminates the innermost switch arm (path-kill)
                e.path_true = z3.BoolVal(False)
                continue
            if _starts_kw(text, "continue"):
                _, text = _stmt(text)
                # skip the rest of this loop body; the loop catches it
                raise _Continue()
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
            if _starts_kw(text, "case") or _starts_kw(text, "default"):
                raise ParseFail("case/default outside switch")
            miss = unencoded_layout_prefix(text)
            if miss:
                raise ParseFail(miss)
            stmt, text = _stmt(text)
            miss = unencoded_layout_stmt(stmt)
            if miss:
                raise ParseFail(miss)
            if _looks_like_decl(stmt):
                self._decl(e, stmt)
            else:
                self._assign_or_expr(e, stmt)

    def _decl(self, e: _Enc, stmt: str) -> None:
        stmt = stmt.rstrip(";").strip()
        # int *p = buf;  (harness pointer alias of a local array)
        mptr = re.match(
            _DECL_TYPE + r"\s*\*+\s*([A-Za-z_]\w*)(?:\s*=\s*(.*))?$",
            stmt,
        )
        if mptr:
            name, init = mptr.group(1), mptr.group(2)
            if init is None:
                raise ParseFail(f"uninitialised pointer decl: {stmt[:80]}")
            src = init.strip()
            if _is_ident(src) and src in e.arrays:
                e.arrays[name] = e.arrays[src]
                return
            raise ParseFail(f"pointer decl must alias an array: {stmt[:80]}")
        # int a[n];  VLA is a missing bound, not a closed proof.
        marr = re.match(
            _DECL_TYPE + r"\s+([A-Za-z_]\w*)\s*\[([^\]]+)\](?:\s*=\s*(.*))?$",
            stmt,
        )
        if marr:
            name, dim, init = marr.group(1), marr.group(2).strip(), marr.group(3)
            if init and re.search(
                r"\{\s*(?:\[[^\]]+\]|\.[A-Za-z_]\w*)\s*=",
                init,
            ):
                raise ParseFail("designated init unencoded")
            if not re.fullmatch(r"\d+", dim):
                raise ParseFail("VLA unencoded")
            n = int(dim)
            arr = z3.Array(name, z3.BitVecSort(WIDTH), z3.BitVecSort(WIDTH))
            e.arrays[name] = (arr, n)
            return
        # int x = 0;  unsigned n;  int x;
        m = re.match(
            _DECL_TYPE + r"\s+([A-Za-z_]\w*)(?:\s*=\s*(.*))?$",
            stmt,
        )
        if not m:
            if re.search(r":\s*[A-Za-z_]", stmt):
                raise ParseFail("range-for unencoded")
            raise ParseFail(f"unparsed decl: {stmt[:80]}")
        name, init = m.group(1), m.group(2)
        prefix = stmt[: m.start(1)]
        if _type_is_unsigned(prefix):
            e.unsigned.add(name)
        e.bits[name] = _type_width(prefix)
        if init is not None:
            e.set(name, self._expr(e, init))
            e.mark_init(name)
        else:
            # uninitialised local: symbolic garbage; a later read is UNINIT-READ
            v = e.bv(name + "_uninit", e.bits[name])
            e.set(name, v)
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
        m = re.match(r"\*\s*([A-Za-z_]\w*)\s*=\s*(.+)$", stmt)
        if m:
            self._astore(e, m.group(1), "0", m.group(2))
            return
        # a[i] = e
        m = re.match(r"([A-Za-z_]\w*)\s*\[(.+)\]\s*=\s*(.+)$", stmt)
        if m:
            self._astore(e, m.group(1), m.group(2), m.group(3))
            return
        m = re.match(r"([A-Za-z_]\w*)\s*([+\-*/%|&^]?=)\s*(.+)$", stmt)
        if m:
            name, op, rhs = m.group(1), m.group(2), m.group(3)
            val = self._expr(e, rhs)
            if op == "=":
                e.set(name, val)
            else:
                e.check_read(name)
                cur = e.get(name)
                e.set(name, self._binop(e, cur, op[0], val, stmt))
            e.mark_init(name)
            return
        # expression statement
        self._expr(e, stmt)

    def _astore(self, e: _Enc, name: str, idx: str, rhs: str) -> None:
        if name not in e.arrays:
            raise ParseFail(f"unknown array {name}")
        arr, n = e.arrays[name]
        i = self._expr(e, idx)
        v = self._expr(e, rhs)
        e.add_prop("oob-write", "MEM-OOB-WRITE", _oob(e, i, n), e.pc)
        e.arrays[name] = (z3.Store(arr, i, v), n)

    def _assert(self, e: _Enc, text: str) -> str:
        m = re.match(r"assert\s*\((.*)\)\s*;", text, re.S)
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
                e.set("__ret", val)
        e.path_true = z3.BoolVal(False)

    def _if(self, e: _Enc, text: str) -> str:
        rest = text[2:].lstrip()
        if rest.startswith("constexpr"):
            raise ParseFail("if constexpr unencoded")
        cond_src, rest = _paren(rest)
        then_src, rest = _block_or_stmt(rest)
        else_src = None
        rest2 = rest.lstrip()
        if rest2.startswith("else"):
            else_src, rest = _block_or_stmt(rest2[4:])
        cond = _as_bool(self._expr(e, cond_src))
        saved_vars = dict(e.vars)
        saved_arr = dict(e.arrays)
        saved_uninit = dict(e.uninit)
        saved_path = e.path_true
        e.path_true = z3.And(saved_path, cond)
        self._stmts(e, then_src)
        then_vars, then_arr, then_uninit, then_path = (
            dict(e.vars), dict(e.arrays), dict(e.uninit), e.path_true
        )
        e.vars, e.arrays, e.uninit, e.path_true = (
            saved_vars, saved_arr, saved_uninit, z3.And(saved_path, z3.Not(cond))
        )
        if else_src:
            self._stmts(e, else_src)
        else_vars, else_arr, else_uninit, else_path = (
            dict(e.vars), dict(e.arrays), dict(e.uninit), e.path_true
        )
        names = set(then_vars) | set(else_vars)
        merged = {}
        for n in names:
            a = then_vars.get(n, saved_vars.get(n))
            b = else_vars.get(n, saved_vars.get(n))
            if a is None or b is None:
                merged[n] = a if a is not None else b
            elif z3.eq(a, b):
                merged[n] = a
            else:
                merged[n] = z3.If(cond, a, b)
        e.vars = merged
        e.retag_unsigned()
        e.arrays = then_arr if then_arr else else_arr
        e.uninit = _merge_uninit(cond, then_uninit, else_uninit, saved_uninit)
        e.path_true = z3.simplify(z3.Or(then_path, else_path))
        return rest

    def _while(self, e: _Enc, text: str) -> str:
        rest = text[5:].lstrip()
        cond_src, rest = _paren(rest)
        body, rest = _block_or_stmt(rest)
        closed = False
        for _ in range(e.unwind):
            cond = _as_bool(self._expr(e, cond_src))
            # if cond can still be true after K, unwind assertion
            s = z3.Solver()
            s.set("timeout", 2000)
            s.add(e.path_true)
            s.add(cond)
            r = s.check()
            if r == z3.unsat:
                closed = True
                break
            e.assume(cond)
            try:
                self._stmts(e, body)
            except _Continue:
                pass
        else:
            cond = _as_bool(self._expr(e, cond_src))
            s = z3.Solver()
            s.set("timeout", 2000)
            s.add(e.path_true)
            s.add(cond)
            if s.check() == z3.sat:
                e.unwind_ok = False
                e.assume(z3.Not(cond))  # bound the leftover
            else:
                closed = True
        if closed:
            pass
        return rest

    def _do(self, e: _Enc, text: str) -> str:
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
        try:
            self._stmts(e, body)
        except _Continue:
            pass
        remaining = max(e.unwind - 1, 0)
        closed = False
        for _ in range(remaining):
            cond = _as_bool(self._expr(e, cond_src))
            s = z3.Solver()
            s.set("timeout", 2000)
            s.add(e.path_true)
            s.add(cond)
            if s.check() == z3.unsat:
                closed = True
                break
            e.assume(cond)
            try:
                self._stmts(e, body)
            except _Continue:
                pass
        else:
            cond = _as_bool(self._expr(e, cond_src))
            s = z3.Solver()
            s.set("timeout", 2000)
            s.add(e.path_true)
            s.add(cond)
            if s.check() == z3.sat:
                e.unwind_ok = False
                e.assume(z3.Not(cond))
            else:
                closed = True
        if closed:
            pass
        return rest

    def _switch(self, e: _Enc, text: str) -> str:
        rest = text[6:].lstrip()
        cond_src, rest = _paren(rest)
        body, rest = _block_or_stmt(rest)
        scrut = self._expr(e, cond_src)
        arms = self._parse_switch_arms(e, body)
        if not arms:
            return rest

        case_eqs = []
        has_default = False
        for arm in arms:
            for lab in arm.labels:
                if lab is None:
                    has_default = True
                else:
                    case_eqs.append(scrut == lab)
        any_case = z3.Or(*case_eqs) if case_eqs else z3.BoolVal(False)

        saved_vars = dict(e.vars)
        saved_arr = dict(e.arrays)
        saved_uninit = dict(e.uninit)
        saved_path = e.path_true
        taken: list[tuple[Any, dict, dict, dict, Any]] = []

        for i, arm in enumerate(arms):
            if not arm.labels:
                continue
            parts = []
            for lab in arm.labels:
                if lab is None:
                    parts.append(z3.Not(any_case))
                else:
                    parts.append(scrut == lab)
            cond = parts[0] if len(parts) == 1 else z3.Or(*parts)
            e.vars = dict(saved_vars)
            e.arrays = dict(saved_arr)
            e.uninit = dict(saved_uninit)
            e.path_true = z3.And(saved_path, cond)
            self._stmts(e, _arm_code(arms, i))
            taken.append((cond, dict(e.vars), dict(e.arrays), dict(e.uninit), e.path_true))

        if not has_default:
            skip = z3.Not(any_case)
            taken.append(
                (skip, dict(saved_vars), dict(saved_arr), dict(saved_uninit),
                 z3.And(saved_path, skip))
            )

        if not taken:
            e.vars, e.arrays, e.uninit, e.path_true = (
                saved_vars, saved_arr, saved_uninit, saved_path
            )
            return rest

        names: set[str] = set()
        for _, vs, _, _, _ in taken:
            names |= set(vs)
        names |= set(saved_vars)
        merged: dict[str, Any] = {}
        for n in names:
            acc = saved_vars.get(n)
            for cond, vs, _, _, _ in reversed(taken):
                v = vs.get(n, saved_vars.get(n))
                if acc is None:
                    acc = v
                elif v is None:
                    pass
                elif z3.eq(acc, v):
                    pass
                else:
                    acc = z3.If(cond, v, acc)
            if acc is not None:
                merged[n] = acc
        e.vars = merged
        e.retag_unsigned()
        e.arrays = dict(saved_arr)
        for _, _, arrs, _, _ in taken:
            for k, av in arrs.items():
                e.arrays[k] = av
        unames: set[str] = set(saved_uninit)
        for _, _, _, un, _ in taken:
            unames |= set(un)
        u_acc: dict[str, Any] = dict(saved_uninit)
        for n in unames:
            accu = saved_uninit.get(n, z3.BoolVal(False))
            for cond, _, _, un, _ in reversed(taken):
                v = un.get(n, saved_uninit.get(n, z3.BoolVal(False)))
                if z3.eq(accu, v):
                    pass
                else:
                    accu = z3.If(cond, v, accu)
            u_acc[n] = accu
        e.uninit = u_acc
        e.path_true = z3.simplify(z3.Or(*[p for _, _, _, _, p in taken]))
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
                arms.append(_SwitchArm(labels, "\n".join(chunks), stops))
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
                if re.search(r"\.\.\.", src):
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
            src, text = _consume_stmt_src(text)
            if stops:
                continue  # unreachable after break until next label
            if src.strip():
                chunks.append(src.strip())
        flush()
        return arms

    def _for(self, e: _Enc, text: str) -> str:
        rest = text[3:].lstrip()
        head, rest = _paren(rest)
        parts = [p.strip() for p in _split_semi(head)]
        while len(parts) < 3:
            parts.append("")
        init, cond_src, incr = parts[0], parts[1], parts[2]
        if init:
            for piece in _split_comma(init):
                piece = piece.strip()
                if not piece:
                    continue
                init_stmt = piece if piece.endswith(";") else piece + ";"
                miss = unencoded_layout_stmt(init_stmt)
                if miss:
                    raise ParseFail(miss)
                if _looks_like_decl(init_stmt):
                    self._decl(e, init_stmt)
                else:
                    self._assign_or_expr(e, init_stmt)
        body, rest = _block_or_stmt(rest)
        closed = False
        for _ in range(e.unwind):
            cond = _as_bool(self._expr(e, cond_src or "1"))
            s = z3.Solver()
            s.set("timeout", 2000)
            s.add(e.path_true)
            s.add(cond)
            if s.check() == z3.unsat:
                closed = True
                break
            e.assume(cond)
            try:
                self._stmts(e, body)
            except _Continue:
                pass
            if incr:
                incr_stmt = incr if incr.endswith(";") else incr + ";"
                self._assign_or_expr(e, incr_stmt)
        else:
            cond = _as_bool(self._expr(e, cond_src or "1"))
            s = z3.Solver()
            s.set("timeout", 2000)
            s.add(e.path_true)
            s.add(cond)
            if s.check() == z3.sat:
                e.unwind_ok = False
                e.assume(z3.Not(cond))
            else:
                closed = True
        if closed:
            pass
        return rest

    def _expr(self, e: _Enc, src: str) -> Any:
        src = src.strip()
        return _parse_expr(e, src, self)

    def _binop(self, e: _Enc, a: Any, op: str, b: Any, loc: str) -> Any:
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
    """Split on commas at paren/bracket depth 0."""
    parts: list[str] = []
    cur: list[str] = []
    pdepth = bdepth = 0
    for ch in s:
        if ch == "(":
            pdepth += 1
        elif ch == ")":
            pdepth -= 1
        elif ch == "[":
            bdepth += 1
        elif ch == "]":
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
    for i, ch in enumerate(text):
        if ch == "(":
            depth += 1
        elif ch == ")":
            depth -= 1
        elif ch == "{" and depth == 0:
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
    if isinstance(v, z3.BoolRef):
        return v
    return v != 0


def _merge_uninit(
    cond: Any,
    then_u: dict[str, Any],
    else_u: dict[str, Any],
    saved_u: dict[str, Any],
) -> dict[str, Any]:
    names = set(then_u) | set(else_u) | set(saved_u)
    merged: dict[str, Any] = {}
    false = z3.BoolVal(False)
    for n in names:
        a = then_u.get(n, saved_u.get(n, false))
        b = else_u.get(n, saved_u.get(n, false))
        if z3.eq(a, b):
            merged[n] = a
        else:
            merged[n] = z3.If(cond, a, b)
    return merged


def apply_binop(e: _Enc, a: Any, op: str, b: Any) -> Any:
    a, b, w = _align_pair(e, a, b)
    u = _is_u(e, a) or _is_u(e, b)
    if op in "+-*/":
        if op == "+":
            r = a + b
            if not u:
                ov = z3.Not(z3.BVAddNoOverflow(a, b, True))
                e.add_prop("ovf+", "INT-SIGNED-OVF", ov, e.pc)
            return _tag(e, r, u, w)
        if op == "-":
            r = a - b
            if not u:
                try:
                    ov = z3.Not(z3.BVSubNoUnderflow(a, b, True))
                except Exception:
                    ov = z3.BoolVal(False)
                e.add_prop("ovf-", "INT-SIGNED-OVF", ov, e.pc)
            return _tag(e, r, u, w)
        if op == "*":
            r = a * b
            if not u:
                ov = z3.Not(z3.BVMulNoOverflow(a, b, True))
                e.add_prop("ovf*", "INT-SIGNED-OVF", ov, e.pc)
            return _tag(e, r, u, w)
        if op == "/":
            e.add_prop("div0", "INT-DIV-ZERO", b == 0, e.pc)
            z = _bv_zero(w)
            if u:
                r = z3.If(b == 0, z, z3.UDiv(a, b))
                return _tag(e, r, True, w)
            e.add_prop("divovf", "INT-SIGNED-OVF",
                       z3.And(a == _bv_min(w), b == -1), e.pc)
            return _tag(e, z3.If(b == 0, z, a / b), False, w)
    if op == "%":
        e.add_prop("mod0", "INT-DIV-ZERO", b == 0, e.pc)
        rem = z3.URem(a, b) if u else z3.SRem(a, b)
        r = z3.If(b == 0, _bv_zero(w), rem)
        return _tag(e, r, u, w)
    if op == "<<":
        if u:
            e.add_prop("shift", "INT-SHIFT-UB", uge(b, w), e.pc)
        else:
            e.add_prop("shift", "INT-SHIFT-UB",
                       z3.Or(slt(b, 0), uge(b, w)), e.pc)
            e.add_prop("shift31", "INT-SHIFT-UB",
                       z3.And(a == 1, uge(b, w - 1)), e.pc)
        return _tag(e, a << b, u, w)
    if op == ">>":
        if u:
            e.add_prop("shift", "INT-SHIFT-UB", uge(b, w), e.pc)
            return _tag(e, z3.LShR(a, b), True, w)
        e.add_prop("shift", "INT-SHIFT-UB",
                   z3.Or(slt(b, 0), uge(b, w)), e.pc)
        return _tag(e, a >> b, False, w)
    if op == "&":
        return _tag(e, a & b, u, w)
    if op == "|":
        return _tag(e, a | b, u, w)
    if op == "^":
        return _tag(e, a ^ b, u, w)
    one, zero = z3.BitVecVal(1, WIDTH), z3.BitVecVal(0, WIDTH)
    if op == "==":
        return _tag(e, z3.If(a == b, one, zero), False, WIDTH)
    if op == "!=":
        return _tag(e, z3.If(a != b, one, zero), False, WIDTH)
    if op == "<":
        pred = ult(a, b) if u else slt(a, b)
        return _tag(e, z3.If(pred, one, zero), False, WIDTH)
    if op == ">":
        pred = ugt(a, b) if u else sgt(a, b)
        return _tag(e, z3.If(pred, one, zero), False, WIDTH)
    if op == "<=":
        pred = ule(a, b) if u else sle(a, b)
        return _tag(e, z3.If(pred, one, zero), False, WIDTH)
    if op == ">=":
        pred = uge(a, b) if u else sge(a, b)
        return _tag(e, z3.If(pred, one, zero), False, WIDTH)
    raise ParseFail(f"op {op}")


def _parse_expr(e: _Enc, src: str, parser: Parser) -> Any:
    src = src.strip()
    # ternary
    # logical
    return _pratt(e, src, parser)


def _pratt(e: _Enc, src: str, parser: Parser) -> Any:
    tokens = _tok(src)
    pos = 0

    def peek() -> str:
        return tokens[pos] if pos < len(tokens) else ""

    def eat(t: str | None = None) -> str:
        nonlocal pos
        if pos >= len(tokens):
            raise ParseFail("unexpected end of expression")
        got = tokens[pos]
        if t is not None and got != t:
            raise ParseFail(f"expected {t} got {got}")
        pos += 1
        return got

    def nud() -> Any:
        t = eat()
        if len(t) >= 3 and t.startswith("'") and t.endswith("'"):
            return z3.BitVecVal(_char_lit_value(t), WIDTH)
        if len(t) >= 2 and t.startswith('"') and t.endswith('"'):
            # string literal is a non-null address, not a proof of the bytes
            return z3.BitVecVal(1, WIDTH)
        if t in ("alignof", "_Alignof"):
            raise ParseFail("alignof unencoded")
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
                return z3.BitVecVal(_sizeof_tokens(inner, e), WIDTH)
            name = eat()
            return z3.BitVecVal(_sizeof_tokens([name], e), WIDTH)
        if t == "(":
            if peek() == "{":
                raise ParseFail("statement-expr unencoded")
            if peek() in _CAST_WORDS:
                words: list[str] = []
                while peek() and peek() != ")":
                    if peek() not in _CAST_WORDS and peek() != "*":
                        break
                    words.append(eat())
                eat(")")
                v = parse(110)
                joined = " ".join(words)
                dst = _type_width(joined)
                src = _width_of(e, v)
                u = _type_is_unsigned(joined)
                v = _resize(v, src, dst, u)
                return _tag(e, v, u, dst)
            v = parse(0)
            eat(")")
            return v
        if t in ("++", "--"):
            name = eat()
            if not _is_ident(name):
                raise ParseFail(f"prefix {t} needs an identifier")
            e.check_read(name)
            cur = e.get(name)
            one = z3.BitVecVal(1, WIDTH)
            new = apply_binop(e, cur, "+" if t == "++" else "-", one)
            e.set(name, new)
            e.mark_init(name)
            return new
        if t == "*":
            name = peek()
            if not _is_ident(name) or name not in e.arrays:
                raise ParseFail(f"deref of {name!r}")
            eat()
            arr, n = e.arrays[name]
            idx = z3.BitVecVal(0, WIDTH)
            e.add_prop("oob-read", "MEM-OOB-READ", _oob(e, idx, n), e.pc)
            return z3.Select(arr, idx)
        if t == "-":
            v = parse(110)
            w = _width_of(e, v)
            z = _bv_zero(w)
            r = z - v
            if not _is_u(e, v):
                e.add_prop("neg", "INT-SIGNED-OVF", v == _bv_min(w), e.pc)
            return _tag(e, r, _is_u(e, v), w)
        if t == "!":
            v = parse(110)
            return z3.If(_as_bool(v), z3.BitVecVal(0, WIDTH), z3.BitVecVal(1, WIDTH))
        if t == "~":
            return ~parse(110)
        if t.isdigit() or (t.startswith("0x")):
            return _tag(e, z3.BitVecVal(int(t, 0), WIDTH), False, WIDTH)
        if _is_ident(t):
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
                if t not in e.arrays:
                    raise ParseFail(f"unknown array {t}")
                arr, n = e.arrays[t]
                e.add_prop("oob-read", "MEM-OOB-READ", _oob(e, idx, n), e.pc)
                return z3.Select(arr, idx)
            if peek() == "(":
                # function call — unconstrained result, not a proof of callees
                eat("(")
                depth = 1
                while depth:
                    ntok = eat()
                    if ntok == "(":
                        depth += 1
                    elif ntok == ")":
                        depth -= 1
                return e.bv("call_" + t)
            if peek() in ("++", "--"):
                op = eat()
                e.check_read(t)
                cur = e.get(t)
                one = z3.BitVecVal(1, WIDTH)
                new = apply_binop(e, cur, "+" if op == "++" else "-", one)
                e.set(t, new)
                e.mark_init(t)
                return cur
            if t in e.arrays:
                # pointer/array used as a value: non-null by construction
                return z3.BitVecVal(1, WIDTH)
            if t in e.vars:
                e.check_read(t)
                return e.get(t)
            if t in parser.enums:
                return z3.BitVecVal(parser.enums[t], WIDTH)
            return e.get(t)
        raise ParseFail(f"bad token {t}")

    PREC = {
        "||": 10, "&&": 20,
        "|": 30, "^": 40, "&": 50,
        "==": 60, "!=": 60,
        "<": 70, ">": 70, "<=": 70, ">=": 70,
        "<<": 80, ">>": 80,
        "+": 90, "-": 90,
        "*": 100, "/": 100, "%": 100,
    }

    def parse(minp: int) -> Any:
        left = nud()
        while peek() in PREC and PREC[peek()] >= minp:
            op = eat()
            right = parse(PREC[op] + 1)
            if op == "&&":
                left = z3.If(z3.And(_as_bool(left), _as_bool(right)),
                             z3.BitVecVal(1, WIDTH), z3.BitVecVal(0, WIDTH))
            elif op == "||":
                left = z3.If(z3.Or(_as_bool(left), _as_bool(right)),
                             z3.BitVecVal(1, WIDTH), z3.BitVecVal(0, WIDTH))
            else:
                left = apply_binop(e, left, op, right)
        # C ternary binds below ||, right-associative.
        if minp <= 5 and peek() == "?":
            eat("?")
            then_v = parse(0)
            eat(":")
            else_v = parse(5)
            left = z3.If(_as_bool(left), then_v, else_v)
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
            d = model.eval(z3.BitVec(name, _type_width(typ)), model_completion=True)
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


def _sizeof_tokens(inner: list[str], e: _Enc) -> int:
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
        if name in e.arrays:
            _arr, n = e.arrays[name]
            return n * (WIDTH // 8)
        return WIDTH // 8
    if "[" in inner:
        return WIDTH // 8
    return WIDTH // 8


def _loop_kw(src: str) -> bool:
    return bool(re.search(r"\b(do|while|for)\b", src or ""))


def _extract_simple_loops(text: str) -> list[tuple[str, str, str]] | None:
    """Top-level and branch-nested loop-free loops: (kind, cond, body).

    Nested loops are unencoded (None). ParseFail is unencoded.
    """
    loops: list[tuple[str, str, str]] = []

    def walk(src: str) -> None:
        while src:
            src = src.lstrip()
            if not src:
                break
            if src.startswith("{"):
                inner, src = _brace(src)
                walk(inner)
                continue
            if _starts_kw(src, "do"):
                rest = src[2:].lstrip()
                body, rest = _block_or_stmt(rest)
                rest = rest.lstrip()
                if not _starts_kw(rest, "while"):
                    raise ParseFail("do without while")
                rest = rest[5:].lstrip()
                cond, rest = _paren(rest)
                rest = rest.lstrip()
                if rest.startswith(";"):
                    rest = rest[1:]
                if _loop_kw(body):
                    raise ParseFail("nested loop")
                loops.append(("do", cond, body))
                src = rest
                continue
            if _starts_kw(src, "while"):
                rest = src[5:].lstrip()
                cond, rest = _paren(rest)
                body, rest = _block_or_stmt(rest)
                if _loop_kw(body):
                    raise ParseFail("nested loop")
                loops.append(("while", cond, body))
                src = rest
                continue
            if _starts_kw(src, "for"):
                rest = src[3:].lstrip()
                head, rest = _paren(rest)
                body, rest = _block_or_stmt(rest)
                if _loop_kw(body):
                    raise ParseFail("nested loop")
                parts = [p.strip() for p in _split_semi(head)]
                while len(parts) < 3:
                    parts.append("")
                _init, cond, incr = parts[0], parts[1], parts[2]
                incr_stmt = incr if not incr or incr.endswith(";") else incr + ";"
                loops.append(("for", cond or "1", f"{body}\n{incr_stmt}"))
                src = rest
                continue
            if _starts_kw(src, "if"):
                rest = src[2:].lstrip()
                _, rest = _paren(rest)
                then_src, rest = _block_or_stmt(rest)
                walk(then_src)
                r2 = rest.lstrip()
                if _starts_kw(r2, "else"):
                    else_src, rest = _block_or_stmt(r2[4:])
                    walk(else_src)
                src = rest
                continue
            if _starts_kw(src, "switch"):
                rest = src[6:].lstrip()
                _, rest = _paren(rest)
                body, rest = _block_or_stmt(rest)
                walk(body)
                src = rest
                continue
            if _starts_kw(src, "case"):
                rest = src[4:].lstrip()
                _, src = _upto_colon(rest)
                continue
            if _starts_kw(src, "default"):
                rest = src[7:].lstrip()
                if not rest.startswith(":"):
                    raise ParseFail("expected : after default")
                src = rest[1:]
                continue
            _, src = _consume_stmt_src(src)

    try:
        walk(text or "")
    except ParseFail:
        return None
    return loops


def _k_step_body(kind: str, cond: str, body: str, k: int) -> str:
    """k concatenated iterations after havoc. do-while runs the body k times."""
    piece = body if kind == "do" else f"if ({cond}) {{\n{body}\n}}"
    return "\n".join(piece for _ in range(max(1, k)))


def k_induction(fn: FunctionInfo, unwind: int) -> Finding:
    """Base case = BMC; step = havoc + k=1 then k=2 iterations.

    SAT on a havoced step is not a counterexample of the original
    function: the record stays BOUNDED. Closing the step is
    PROVED-UNBOUNDED and is never merged down into PROVED/BOUNDED.
    Nested loops stay unencoded.
    """
    rec = bmc_function(fn, unwind, try_unbounded=True)
    extra = dict(rec.extra or {})
    if rec.status != laws.BOUNDED:
        extra["k_induction"] = "not-needed"
        rec.extra = extra
        return rec
    loops = _extract_simple_loops(fn.body or "")
    if not loops:
        extra["k_induction"] = "unencoded"
        rec.extra = extra
        return rec

    last_open_cls = ""
    tried: list[int] = []
    for kstep in (1, 2):
        step_open = False
        step_cls = ""
        unencoded = False
        k_steps: list[str] = []
        for kind, cond, body in loops:
            piece = _k_step_body(kind, cond, body, kstep)
            cloned = replace(fn, body=piece)
            step = bmc_function(
                cloned, unwind=1, try_unbounded=False,
                allow_local_pointers=True,
            )
            k_steps.append(step.status)
            if step.status == laws.FAILED:
                step_open = True
                step_cls = step.cls
            elif step.status not in {laws.PROVED, laws.PROVED_UNBOUNDED}:
                unencoded = True
        tried.append(kstep)
        extra["k_steps"] = k_steps
        extra["k_induction_tried"] = list(tried)
        extra["k_induction_k"] = kstep
        if not step_open and not unencoded:
            extra["k_induction"] = "closed"
            extra["unwind_closed"] = True
            return Finding(
                stage="bmc", status=laws.PROVED_UNBOUNDED,
                file=fn.file, function=fn.name, line=fn.line, cls="",
                message=f"k-induction step closed at k={kstep}; "
                "not a bounded-only result",
                strength=laws.STRENGTH_PROVES, extra=extra,
            )
        if step_open:
            last_open_cls = step_cls
        if unencoded:
            break

    extra["k_induction_tried"] = tried
    if last_open_cls:
        extra["k_induction"] = "step-open"
        extra["k_induction_cls"] = last_open_cls
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


def _bmc_once(
    fn: FunctionInfo,
    unwind: int,
    try_unbounded: bool,
    enums: dict[str, int],
    base: dict,
) -> Finding:
    p = Parser(fn.body, fn.params, unwind, enums=enums)
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
    return bool(re.search(r"\bstd::|\bstring_view\b|\bspan\b", blob))


def _has_unencoded_float(fn: FunctionInfo) -> bool:
    """IEEE float is not in the bitvector encoder. Missing model, not ERROR."""
    if re.search(r"\bfloat\b|\bdouble\b", fn.return_type or "", re.I):
        return True
    for typ, _ in fn.params:
        if re.search(r"\bfloat\b|\bdouble\b", typ or "", re.I):
            return True
    return bool(re.search(
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

    Missing model, not a parse ERROR and not a proof. `goto` is not
    this case. `offsetof` is in `_CALL_KW` so it is not a libc-effect
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
    plain `goto label` stays ERROR and is not this case.
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
    if re.search(r"\bgoto\s+\*", body):
        return (
            f"computed goto unencoded: {engine} is not a computed-goto model"
        )
    if re.search(r"(?:^|[;{(]|=\s*)\&\&[A-Za-z_]\w*", body, re.M):
        return (
            f"label-address unencoded: {engine} is not a label-address model"
        )
    if re.search(r"\b(?:_Thread_local|thread_local)\b", body):
        return (
            f"thread-local unencoded: {engine} is not a TLS model"
        )
    if re.search(r"\b(?:_Complex|_Imaginary)\b", body):
        return (
            f"complex unencoded: {engine} is not a complex arithmetic model"
        )
    if re.search(r"\b(?:typeof_unqual|__typeof_unqual__)\s*\(", body):
        return f"typeof_unqual unencoded: {engine} is not a typeof model"
    if re.search(r"\b(?:typeof|__typeof__)\s*\(", body):
        return f"typeof unencoded: {engine} is not a typeof model"
    if re.search(
        r"(?m)(?:^|[;{])\s*(?:void|int|unsigned(?:\s+int)?|long(?:\s+int)?|"
        r"short|char|float|double|_Bool|bool)\s+[A-Za-z_]\w*\s*\([^)]*\)\s*\{",
        body,
    ):
        return (
            f"nested function unencoded: {engine} is not a "
            "nested-function model"
        )
    if re.search(r"\b(?:_Alignof|alignof)\s*\(", body):
        return f"alignof unencoded: {engine} is not an alignment model"
    if re.search(r"\bva_arg\s*\(", body):
        return f"va_arg unencoded: {engine} is not a variadic model"
    if re.search(r"\{\s*(?:\[[^\]]+\]|\.[A-Za-z_]\w*)\s*=", body):
        return (
            f"designated init unencoded: {engine} is not a "
            "designated-init model"
        )
    if re.search(r"\bfor\s*\([^)]*:[^)]*\)", body):
        return (
            f"C++ range-for unencoded: {engine} is not a range-for model"
        )
    if re.search(r"\[\s*[^\]]*\]\s*(?:\([^)]*\))?\s*\{", body):
        return f"C++ lambda unencoded: {engine} is not a lambda model"
    if re.search(r"\bconst_cast\s*<", body):
        return (
            f"C++ const_cast unencoded: {engine} is not a cv-qualifier model"
        )
    if re.search(r"\bdynamic_cast\s*<", body):
        return (
            f"C++ dynamic_cast unencoded: {engine} is not an RTTI model"
        )
    if re.search(r"\btype_identity\b", body):
        return (
            f"C++ type_identity unencoded: {engine} is not a "
            "type_identity model"
        )
    if re.search(r"\btypeid\s*\(", body):
        return f"C++ typeid unencoded: {engine} is not an RTTI model"
    if re.search(r"\breinterpret_cast\s*<", body):
        return (
            f"C++ reinterpret_cast unencoded: {engine} is not a type-pun model"
        )
    if re.search(r"\bstd::bit_cast\b|bit_cast\s*<", body):
        return f"bit_cast unencoded: {engine} is not a type-pun model"
    if re.search(
        r"(?m)\bstd\s*::\s*jthread\b"
        r"|(?:^|[;{])\s*jthread\s+[A-Za-z_]\w*\s*\(",
        body,
    ):
        return (
            f"C++ std::jthread unencoded: {engine} is not a jthread model"
        )
    if re.search(
        r"(?m)\bstd\s*::\s*thread\b"
        r"|(?:^|[;{])\s*thread\s+[A-Za-z_]\w*\s*\(",
        body,
    ):
        return (
            f"C++ std::thread unencoded: {engine} is not a "
            "thread-lifetime model"
        )
    if re.search(
        r"\bstd\s*::\s*(?:async|future|promise)\b"
        r"|(?:future|promise)\s*<",
        body,
    ):
        return (
            f"C++ std::async unencoded: {engine} is not a future model"
        )
    if re.search(r"\b(?:std\s*::\s*)?packaged_task\b|\bpackaged_task\s*<", body):
        return (
            f"C++ packaged_task unencoded: {engine} is not a "
            "packaged_task model"
        )
    if re.search(r"\b(?:std\s*::\s*)?counted_iterator\b", body):
        return (
            f"C++ counted_iterator unencoded: {engine} is not a "
            "counted_iterator model"
        )
    if re.search(r"\b(?:std\s*::\s*)?weak_ptr\s*<", body):
        return (
            f"C++ weak_ptr unencoded: {engine} is not a weak_ptr model"
        )
    if re.search(r"\b(?:std\s*::\s*)?nested_exception\b", body):
        return (
            f"C++ nested_exception unencoded: {engine} is not a "
            "nested_exception model"
        )
    if re.search(
        r"\b(?:throw_with_nested|rethrow_if_nested)\s*\(",
        body,
    ):
        return (
            f"throw_with_nested unencoded: unconstrained "
            f"throw_with_nested is not a proof ({engine})"
        )
    if re.search(r"\buncaught_exceptions\s*\(", body):
        return (
            f"C++ uncaught_exceptions unencoded: {engine} is not an "
            "uncaught_exceptions model"
        )
    if re.search(r"\bcurrent_exception\s*\(", body):
        return (
            f"C++ current_exception unencoded: {engine} is not a "
            "current_exception model"
        )
    if re.search(r"\bmake_exception_ptr\s*\(", body):
        return (
            f"C++ make_exception_ptr unencoded: {engine} is not a "
            "make_exception_ptr model"
        )
    if re.search(r"\b(?:std\s*::\s*)?exception_ptr\b", body):
        return (
            f"C++ exception_ptr unencoded: {engine} is not an "
            "exception_ptr model"
        )
    if re.search(r"\b(?:std\s*::\s*)?coroutine_handle\b", body):
        return (
            f"C++ coroutine_handle unencoded: {engine} is not a "
            "coroutine_handle model"
        )
    if re.search(r"\b(?:std\s*::\s*)?valarray\s*<", body):
        return (
            f"C++ valarray unencoded: {engine} is not a valarray model"
        )
    if re.search(r"\b(?:std\s*::\s*)?to_underlying\s*\(", body):
        return (
            f"C++ to_underlying unencoded: {engine} is not a "
            "to_underlying model"
        )
    if re.search(r"\b(?:std\s*::\s*)?unexpected\s*<", body):
        return (
            f"C++ unexpected unencoded: {engine} is not an unexpected model"
        )
    if re.search(r"\b(?:std\s*::\s*)?function_ref\s*<", body):
        return (
            f"C++ function_ref unencoded: {engine} is not a "
            "function_ref model"
        )
    if re.search(
        r"\bstd\s*::\s*(?:move_only_function|copyable_function)\b"
        r"|\b(?:move_only_function|copyable_function)\s*<",
        body,
    ):
        return (
            f"C++ move_only_function unencoded: {engine} is not a "
            "move_only_function model"
        )
    if re.search(r"\breference_wrapper\b|\bstd\s*::\s*(?:cref|ref)\s*\(", body):
        return (
            f"C++ reference_wrapper unencoded: {engine} is not a "
            "reference_wrapper model"
        )
    if re.search(r"\bstd\s*::\s*function\b|function\s*<", body):
        return (
            f"C++ std::function unencoded: {engine} is not a "
            "type-erased callable model"
        )
    if re.search(r"\bstd\s*::\s*mdspan\b|mdspan\s*<", body):
        return (
            f"C++ std::mdspan unencoded: {engine} is not an mdspan model"
        )
    if re.search(
        r"\bstd\s*::\s*(?:mutex|lock_guard|unique_lock|scoped_lock)\b"
        r"|(?:lock_guard|unique_lock|scoped_lock)\s*<",
        body,
    ):
        return (
            f"C++ std::mutex unencoded: {engine} is not a C++ mutex model"
        )
    if re.search(r"\bnotify_all_at_thread_exit\s*\(", body):
        return (
            f"C++ notify_all_at_thread_exit unencoded: {engine} is not a "
            "notify_all_at_thread_exit model"
        )
    if re.search(r"\bcondition_variable_any\b", body):
        return (
            f"C++ condition_variable_any unencoded: {engine} is not a "
            "condition_variable_any model"
        )
    if re.search(r"\bshared_timed_mutex\b", body):
        return (
            f"C++ shared_timed_mutex unencoded: {engine} is not a "
            "shared_timed_mutex model"
        )
    if re.search(
        r"\bstd\s*::\s*(?:condition_variable|shared_mutex)\b"
        r"|\b(?:condition_variable|shared_mutex)\b",
        body,
    ):
        return (
            f"C++ std::condition_variable unencoded: {engine} is not a "
            "condvar/shared-mutex model"
        )
    if re.search(r"\bstd\s*::\s*atomic_ref\b|atomic_ref\s*<", body):
        return (
            f"C++ std::atomic_ref unencoded: {engine} is not an "
            "atomic_ref model"
        )
    if re.search(r"\bstd\s*::\s*generator\b|generator\s*<", body):
        return (
            f"C++ std::generator unencoded: {engine} is not a generator model"
        )
    if re.search(r"\[\[\s*assume\s*\(", body):
        return (
            f"C++ assume unencoded: {engine} is not an assume-attribute model"
        )
    if re.search(r"\bstd\s*::\s*bind\s*\(", body):
        return (
            f"C++ std::bind unencoded: {engine} is not a bind model"
        )
    if re.search(r"\bstd\s*::\s*any\b|\bany_cast\s*<|\bany_cast\b", body):
        return (
            f"C++ std::any unencoded: {engine} is not an any model"
        )
    if re.search(
        r"\bstd\s*::\s*filesystem\b|\bstd\s*::\s*fs\s*::|\bfilesystem\s*::",
        body,
    ):
        return (
            f"C++ std::filesystem unencoded: {engine} is not a "
            "filesystem model"
        )
    if re.search(
        r"\bstd\s*::\s*regex\b|\bstd\s*::\s*regex_"
        r"|\bregex\s+[A-Za-z_]\w*\s*[\({]",
        body,
    ):
        return (
            f"C++ std::regex unencoded: {engine} is not a regex model"
        )
    if re.search(
        r"\bstd\s*::\s*(?:latch|barrier|counting_semaphore)\b"
        r"|\blatch\s+[A-Za-z_]\w*\s*\("
        r"|\bbarrier\s*<",
        body,
    ):
        return (
            f"C++ std::latch/barrier unencoded: {engine} is not a "
            "sync primitive model"
        )
    if re.search(r"\bstd\s*::\s*from_chars\b|\bfrom_chars\s*\(", body):
        return (
            f"C++ from_chars unencoded: {engine} is not a charconv model"
        )
    if re.search(r"\b(?:std\s*::\s*)?to_chars\s*\(", body):
        return (
            f"C++ to_chars unencoded: {engine} is not a charconv model"
        )
    if re.search(r"\b(?:std\s*::\s*)?hazard_pointer\b", body):
        return (
            f"C++ hazard_pointer unencoded: {engine} is not a "
            "hazard_pointer model"
        )
    if re.search(r"\b(?:std\s*::\s*)?text_encoding\b", body):
        return (
            f"C++ text_encoding unencoded: {engine} is not a "
            "text_encoding model"
        )
    if re.search(
        r"\b(?:std\s*::\s*(?:experimental\s*::\s*)?)?simd\s*<",
        body,
    ):
        return (
            f"C++ simd unencoded: {engine} is not a simd model"
        )
    if re.search(r"\bstd\s*::\s*visit\b", body):
        return (
            f"C++ std::visit unencoded: {engine} is not a visitor model"
        )
    if re.search(r"\b(?:std\s*::\s*)?source_location\b", body):
        return (
            f"C++ source_location unencoded: {engine} is not a "
            "source_location model"
        )
    if re.search(r"\b(?:std\s*::\s*)?stacktrace\b", body):
        return (
            f"C++ stacktrace unencoded: {engine} is not a stacktrace model"
        )
    if re.search(r"\bstd\s*::\s*stop_(?:token|source|callback)\b", body):
        return (
            f"C++ stop_token unencoded: {engine} is not a stop_token model"
        )
    if re.search(r"\bstd\s*::\s*flat_map\b|\bflat_map\s*<", body):
        return (
            f"C++ flat_map unencoded: {engine} is not a flat_map model"
        )
    if re.search(r"\bstd\s*::\s*flat_set\b|\bflat_set\s*<", body):
        return (
            f"C++ flat_set unencoded: {engine} is not a flat_set model"
        )
    if re.search(r"\bstd\s*::\s*flat_multiset\b|\bflat_multiset\s*<", body):
        return (
            f"C++ flat_multiset unencoded: {engine} is not a "
            "flat_multiset model"
        )
    if re.search(r"\bstd\s*::\s*flat_multimap\b|\bflat_multimap\s*<", body):
        return (
            f"C++ flat_multimap unencoded: {engine} is not a "
            "flat_multimap model"
        )
    if re.search(r"\bzoned_time\b", body):
        return (
            f"C++ zoned_time unencoded: {engine} is not a zoned_time model"
        )
    if re.search(r"\b(?:current_zone|tzdb)\b", body):
        return (
            f"C++ tzdb unencoded: {engine} is not a tzdb model"
        )
    if re.search(r"\bstd\s*::\s*chrono\b|\bchrono\s*::", body):
        return (
            f"C++ chrono unencoded: {engine} is not a chrono model"
        )
    if re.search(r"\bzip_transform(?:_view)?\b", body):
        return (
            f"C++ zip_transform unencoded: {engine} is not a "
            "zip_transform model"
        )
    if re.search(r"views\s*::\s*zip|\bzip_view\b", body):
        return (
            f"C++ views::zip unencoded: {engine} is not a views::zip model"
        )
    if re.search(r"\bas_rvalue(?:_view)?\b", body):
        return (
            f"C++ as_rvalue unencoded: {engine} is not an as_rvalue model"
        )
    if re.search(r"\bis_scoped_enum\b", body):
        return (
            f"C++ is_scoped_enum unencoded: {engine} is not an "
            "is_scoped_enum model"
        )
    if re.search(r"\b(?:views\s*::\s*)?enumerate(?:_view)?\b", body):
        return (
            f"C++ views::enumerate unencoded: {engine} is not a "
            "views::enumerate model"
        )
    if re.search(r"\bcartesian_product(?:_view)?\b", body):
        return (
            f"C++ cartesian_product unencoded: {engine} is not a "
            "cartesian_product model"
        )
    if re.search(
        r"\b(?:views\s*::\s*chunk(?:_by)?|chunk(?:_by|_view))\b",
        body,
    ):
        return (
            f"C++ views::chunk unencoded: {engine} is not a "
            "views::chunk model"
        )
    if re.search(r"\b(?:views\s*::\s*slide|slide_view)\b", body):
        return (
            f"C++ views::slide unencoded: {engine} is not a "
            "views::slide model"
        )
    if re.search(
        r"\b(?:views\s*::\s*adjacent(?:_transform)?|"
        r"adjacent(?:_transform|_view))\b",
        body,
    ):
        return (
            f"C++ views::adjacent unencoded: {engine} is not a "
            "views::adjacent model"
        )
    if re.search(r"\bjoin_with(?:_view)?\b", body):
        return (
            f"C++ join_with unencoded: {engine} is not a join_with model"
        )
    if re.search(r"views\s*::\s*join|\bjoin_view\b", body):
        return (
            f"C++ views::join unencoded: {engine} is not a views::join model"
        )
    if re.search(r"\b(?:views\s*::\s*)?stride(?:_view)?\b", body):
        return (
            f"C++ views::stride unencoded: {engine} is not a "
            "views::stride model"
        )
    if re.search(r"\b(?:views\s*::\s*repeat|repeat_view)\b", body):
        return (
            f"C++ views::repeat unencoded: {engine} is not a "
            "views::repeat model"
        )
    if re.search(r"\btake_while(?:_view)?\b", body):
        return (
            f"C++ views::take_while unencoded: {engine} is not a "
            "take_while model"
        )
    if re.search(r"\b(?:views\s*::\s*take\b|\btake_view\b)", body):
        return (
            f"C++ views::take unencoded: {engine} is not a "
            "views::take model"
        )
    if re.search(r"\bdrop_while(?:_view)?\b", body):
        return (
            f"C++ views::drop_while unencoded: {engine} is not a "
            "drop_while model"
        )
    if re.search(r"\b(?:views\s*::\s*drop\b|\bdrop_view\b)", body):
        return (
            f"C++ views::drop unencoded: {engine} is not a "
            "views::drop model"
        )
    if re.search(r"\b(?:views\s*::\s*keys\b|\bkeys_view\b)", body):
        return (
            f"C++ views::keys unencoded: {engine} is not a "
            "keys model"
        )
    if re.search(r"\b(?:views\s*::\s*values\b|\bvalues_view\b)", body):
        return (
            f"C++ views::values unencoded: {engine} is not a "
            "values model"
        )
    if re.search(r"\b(?:views\s*::\s*reverse\b|\breverse_view\b)", body):
        return (
            f"C++ views::reverse unencoded: {engine} is not a "
            "reverse_view model"
        )
    if re.search(r"\b(?:views\s*::\s*counted\b|\bcounted_view\b)", body):
        return (
            f"C++ views::counted unencoded: {engine} is not a "
            "counted_view model"
        )
    if re.search(r"\b(?:views\s*::\s*filter\b|\bfilter_view\b)", body):
        return (
            f"C++ views::filter unencoded: {engine} is not a "
            "views::filter model"
        )
    if re.search(r"\b(?:views\s*::\s*transform\b|\btransform_view\b)", body):
        return (
            f"C++ views::transform unencoded: {engine} is not a "
            "transform_view model"
        )
    if re.search(r"\b(?:views\s*::\s*elements\b|\belements_view\b)", body):
        return (
            f"C++ views::elements unencoded: {engine} is not an "
            "elements model"
        )
    if re.search(r"\b(?:views\s*::\s*iota\b|\biota_view\b)", body):
        return (
            f"C++ views::iota unencoded: {engine} is not an "
            "iota model"
        )
    if re.search(r"\bfrom_range\b", body):
        return (
            f"C++ from_range unencoded: {engine} is not a from_range model"
        )
    if re.search(
        r"\bstd\s*::\s*ranges\s*::\s*views\b|\bstd\s*::\s*views\s*::",
        body,
    ):
        return (
            f"C++ ranges views unencoded: {engine} is not a "
            "ranges-views model"
        )
    if re.search(r"\bstd\s*::\s*hive\b|\bhive\s*<", body):
        return (
            f"C++ hive unencoded: {engine} is not a hive model"
        )
    if re.search(r"\bstd\s*::\s*(?:execution\s*::\s*)?task\s*<", body):
        return (
            f"C++ std::task unencoded: {engine} is not a task model"
        )
    if re.search(r"\bstd\s*::\s*execution\s*::", body):
        return (
            f"C++ execution unencoded: {engine} is not an execution model"
        )
    if re.search(
        r"\bstd\s*::\s*indirect\b|\bindirect\s*<"
        r"|\bstd\s*::\s*polymorphic\b|\bpolymorphic\s*<",
        body,
    ):
        return (
            f"C++ indirect unencoded: {engine} is not an "
            "indirect/polymorphic model"
        )
    if re.search(r"\bstd\s*::\s*bitset\s*<|\bbitset\s*<", body):
        return (
            f"C++ bitset unencoded: {engine} is not a bitset model"
        )
    if re.search(
        r"\b(?:std\s*::\s*)?(?:basic_)?(?:string|ostring|istring)stream\b",
        body,
    ):
        return (
            f"C++ stringstream unencoded: {engine} is not a "
            "stringstream model"
        )
    if re.search(r"\b(?:std\s*::\s*)?(?:basic_)?osyncstream\b", body):
        return (
            f"C++ osyncstream unencoded: {engine} is not an "
            "osyncstream model"
        )
    if re.search(r"\b(?:std\s*::\s*)?(?:basic_)?syncbuf\b", body):
        return (
            f"C++ syncbuf unencoded: {engine} is not a syncbuf model"
        )
    if re.search(
        r"\b(?:std\s*::\s*)?(?:basic_)?(?:i|o)?spanstream\b",
        body,
    ):
        return (
            f"C++ spanstream unencoded: {engine} is not a "
            "spanstream model"
        )
    if re.search(
        r"\b(?:std\s*::\s*)?(?:inout_ptr|out_ptr)\s*(?:<|\()",
        body,
    ):
        return (
            f"C++ out_ptr unencoded: {engine} is not an out_ptr model"
        )
    if re.search(
        r"\bstd\s*::\s*rcu\b|\brcu_synchronize\s*\(|\brcu_obj\s*<",
        body,
    ):
        return (
            f"C++ rcu unencoded: {engine} is not an rcu model"
        )
    if re.search(r"\bstd\s*::\s*linalg\b|\blinalg\s*::", body):
        return (
            f"C++ linalg unencoded: {engine} is not a linalg model"
        )
    if re.search(r"\bsync_wait\s*\(", body):
        return (
            f"C++ sync_wait unencoded: {engine} is not an execution model"
        )
    if re.search(r"\bcontract_assert\s*\(|\[\[\s*(?:pre|post)\s*:", body):
        return (
            f"C++ contracts unencoded: {engine} is not a contracts model"
        )
    if re.search(
        r"\bstd\s*::\s*meta\b|\^\^"
        r"|\bdefine_aggregate\s*\(|\bdefine_class\s*\(",
        body,
    ):
        return (
            f"C++ reflection unencoded: {engine} is not a reflection model"
        )
    if re.search(r"\b(?:std\s*::\s*)?initializer_list\s*<", body):
        return (
            f"C++ initializer_list unencoded: {engine} is not a "
            "temporary-lifetime model"
        )
    if re.search(r"\b(?:std\s*::\s*)?optional\s*<", body):
        return (
            f"C++ std::optional unencoded: {engine} is not an optional model"
        )
    if re.search(r"\bstd\s*::\s*expected\b|\bexpected\s*<", body):
        return (
            f"C++ std::expected unencoded: {engine} is not an expected model"
        )
    if re.search(r"\bformat_to(?:_n)?\s*\(", body):
        return (
            f"C++ format_to unencoded: {engine} is not a format_to model"
        )
    if re.search(r"\bstd\s*::\s*(?:format|print|println)\b", body):
        return (
            f"C++ std::format unencoded: {engine} is not a format model"
        )
    if re.search(r"<=>", body):
        return (
            f"C++ spaceship unencoded: {engine} is not a "
            "three-way comparison model"
        )
    if re.search(r"\b(?:std\s*::\s*)?variant\s*<", body):
        return (
            f"C++ std::variant unencoded: {engine} is not a variant model"
        )
    if re.search(r"\b(?:std\s*::\s*)?span\s*<", body):
        return (
            f"C++ std::span unencoded: {engine} is not a span-lifetime model"
        )
    if re.search(r"\bstd\s*::\s*inplace_vector\b|\binplace_vector\s*<", body):
        return (
            f"C++ inplace_vector unencoded: {engine} is not an "
            "inplace_vector model"
        )
    if re.search(r"\b(?:std\s*::\s*)?vector\s*<", body):
        return (
            f"C++ std::vector unencoded: {engine} is not a container model"
        )
    if re.search(r"\bcatch\s*\(\s*\.\.\.\s*\)", body):
        return (
            f"C++ catch-all unencoded: {engine} is not an exception model"
        )
    if re.search(r"\bthrow\s+new\b", body):
        return (
            f"C++ throw-new unencoded: {engine} is not an exception model"
        )
    if re.search(r"\bstd\s*::\s*launder\b|\blaunder\s*\(", body):
        return (
            f"C++ launder unencoded: {engine} is not a lifetime model"
        )
    if re.search(
        r"\b(?:std\s*::\s*)?start_lifetime_as(?:_array)?\b",
        body,
    ):
        return (
            f"C++ start_lifetime_as unencoded: {engine} is not a lifetime model"
        )
    if re.search(
        r"\b(?:enable_shared_from_this|shared_from_this)\b",
        body,
    ):
        return (
            f"C++ shared_from_this unencoded: {engine} is not a "
            "shared-lifetime model"
        )
    if re.search(r"\bif\s+constexpr\b", body):
        return (
            f"if constexpr unencoded: {engine} is not a compile-time-if model"
        )
    if re.search(r"\bconstexpr\b", body):
        return (
            f"constexpr unencoded: {engine} is not a constexpr model"
        )
    blob = f"{fn.signature or ''}\n{body}"
    if re.search(r"\brequires\s*\(", blob) or re.search(r"\bconcept\s+", blob):
        return (
            f"C++ concepts unencoded: {engine} is not a concepts model"
        )
    if re.search(r"\bcase\s+[^:'\"]+?\s*\.\.\.\s*[^:'\"]+?:", body):
        return (
            f"case-range unencoded: {engine} is not a case-range model"
        )
    if re.search(r"\(\s*\.\.\.\s*[+\-|&^]|[+\-|&^]\s*\.\.\.\s*\)", body):
        return (
            f"C++ fold unencoded: {engine} is not a fold-expression model"
        )
    if re.search(r"\b(?:__int128(?:_t)?|_BitInt)\b", body):
        return f"128-bit unencoded: {engine} is not a 128-bit model"
    if re.search(r"\b(?:_Decimal32|_Decimal64|_Decimal128)\b", body):
        return (
            f"decimal float unencoded: {engine} is not a decimal-float model"
        )
    if re.search(r"\b(?:_Float16|_Float32|_Float64|__fp16)\b", body):
        return (
            f"extra-IEEE float unencoded: {engine} is not an extra-IEEE model"
        )
    code = re.sub(r"/\*.*?\*/", " ", body, flags=re.S)
    code = re.sub(r"//.*?$", " ", code, flags=re.M)
    if re.search(r"\bnullptr\b", code):
        return (
            f"C23 nullptr unencoded: {engine} is not a nullptr model"
        )
    if re.search(r"\bco_(?:await|yield|return)\b", body):
        return (
            f"C++ coroutine unencoded: {engine} is not a coroutine model"
        )
    if re.search(
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
    if re.search(r"\bpragma\s+pack\b", pack_code):
        return (
            f"pragma pack unencoded: {engine} is not a packed-layout model"
        )
    import_code = re.sub(r"/\*.*?\*/", " ", body, flags=re.S)
    import_code = re.sub(r"//.*?$", " ", import_code, flags=re.M)
    if re.search(r"\bimport\s+[A-Za-z_]", import_code) or re.search(
        r"\bexport\s+module\b",
        import_code,
    ):
        return (
            f"C++ module import unencoded: {engine} is not a modules model"
        )
    if re.search(r"\#\s*embed\b", body):
        return (
            f"C++ embed unencoded: {engine} is not an embed model"
        )
    if re.search(
        r"\bstd\s*::\s*rcu\b|\brcu_synchronize\s*\(|\brcu_obj\s*<",
        body,
    ):
        return (
            f"C++ rcu unencoded: {engine} is not an rcu model"
        )
    if re.search(r"\bstd\s*::\s*linalg\b|\blinalg\s*::", body):
        return (
            f"C++ linalg unencoded: {engine} is not a linalg model"
        )
    if re.search(r"\bstd\s*::\s*meta\b|\^\^", body):
        return (
            f"C++ reflection unencoded: {engine} is not a reflection model"
        )
    if re.search(r"__attribute__\s*\(\s*\(\s*cleanup", body):
        return (
            f"cleanup attribute unencoded: {engine} is not a cleanup model"
        )
    if re.search(
        r"__attribute__\s*\(\s*\(\s*(?:__)?vector_size|\b__vector_size\b",
        body,
    ):
        return (
            f"vector_size unencoded: {engine} is not a SIMD vector model"
        )
    if re.search(r"\bL'(?:\\.|[^\\'])'", body):
        return (
            f"wide character unencoded: {engine} is not a wide-char model"
        )
    if re.search(r'\bL"(?:\\.|[^\\"])*"', body):
        return (
            f"wide character unencoded: {engine} is not a wide-char model"
        )
    if re.search(r"\bexecveat\s*\(", body):
        return (
            f"execveat unencoded: unconstrained execveat is not a "
            f"proof ({engine})"
        )
    if re.search(r"\bpthread_atfork\s*\(", body):
        return (
            f"pthread_atfork unencoded: unconstrained pthread_atfork "
            f"is not a proof ({engine})"
        )
    if re.search(r"\bpledge\s*\(", body):
        return (
            f"pledge unencoded: unconstrained pledge is not a "
            f"proof ({engine})"
        )
    if re.search(r"\bmac_(?:set|get)_(?:proc|fd|file)\s*\(", body):
        return (
            f"mac_set_proc unencoded: unconstrained mac_set_proc is not a "
            f"proof ({engine})"
        )
    if re.search(r"\bcap_getmode\s*\(", body):
        return (
            f"cap_getmode unencoded: unconstrained cap_getmode is not a "
            f"proof ({engine})"
        )
    if re.search(r"\bcap_getrights\s*\(", body):
        return (
            f"cap_getrights unencoded: unconstrained cap_getrights is not a "
            f"proof ({engine})"
        )
    if re.search(r"\bcap_enter\s*\(", body):
        return (
            f"cap_enter unencoded: unconstrained cap_enter is not a "
            f"proof ({engine})"
        )
    if re.search(r"\bcap_sandboxed\s*\(", body):
        return (
            f"cap_sandboxed unencoded: unconstrained cap_sandboxed is not a "
            f"proof ({engine})"
        )
    if re.search(r"\bcap_rights_(?:limit|get)\s*\(", body):
        return (
            f"cap_rights unencoded: unconstrained cap_rights_limit "
            f"is not a proof ({engine})"
        )
    if re.search(r"\bcap_(?:fcntls|ioctls)_limit\s*\(", body):
        return (
            f"cap_fcntls unencoded: unconstrained cap_fcntls_limit "
            f"is not a proof ({engine})"
        )
    if re.search(r"\bunveil\s*\(", body):
        return (
            f"unveil unencoded: unconstrained unveil is not a "
            f"proof ({engine})"
        )
    if re.search(r"\bsysctl(?:byname)?\s*\(", body):
        return (
            f"sysctl unencoded: unconstrained sysctl is not a "
            f"proof ({engine})"
        )
    if re.search(r"\bkqueue\s*\(", body):
        return (
            f"kqueue unencoded: unconstrained kqueue is not a "
            f"proof ({engine})"
        )
    if re.search(r"\bkevent\s*\(", body):
        return (
            f"kevent unencoded: unconstrained kevent is not a "
            f"proof ({engine})"
        )
    if re.search(r"\bpause\s*\(", body):
        return (
            f"pause unencoded: unconstrained pause is not a "
            f"proof ({engine})"
        )
    if re.search(r"\b(?:get|set|swap|make)context\s*\(", body):
        return (
            f"getcontext unencoded: unconstrained getcontext is not a "
            f"proof ({engine})"
        )
    if re.search(
        r"\bpthread_attr_(?:init|destroy|setstacksize|setstack|"
        r"setdetachstate|getstacksize|getstack|getdetachstate)\s*\(",
        body,
    ):
        return (
            f"pthread_attr unencoded: unconstrained pthread_attr_init "
            f"is not a proof ({engine})"
        )
    if re.search(
        r"\b(?:fork|vfork|execlp|execle|execl|"
        r"execvpe|execvp|execve|execv)\s*\(",
        body,
    ):
        return (
            f"process spawn unencoded: unconstrained fork/exec is not a "
            f"proof ({engine})"
        )
    if re.search(r"\bpdfork\s*\(", body):
        return (
            f"pdfork unencoded: unconstrained pdfork is not a "
            f"proof ({engine})"
        )
    if re.search(r"\brfork\s*\(", body):
        return (
            f"rfork unencoded: unconstrained rfork is not a "
            f"proof ({engine})"
        )
    if re.search(r"\bminherit\s*\(", body):
        return (
            f"minherit unencoded: unconstrained minherit is not a "
            f"proof ({engine})"
        )
    if re.search(r"\bnfssvc\s*\(", body):
        return (
            f"nfssvc unencoded: unconstrained nfssvc is not a "
            f"proof ({engine})"
        )
    if re.search(r"\bsysarch\s*\(", body):
        return (
            f"sysarch unencoded: unconstrained sysarch is not a "
            f"proof ({engine})"
        )
    if re.search(r"\bgetbootfile\s*\(", body):
        return (
            f"getbootfile unencoded: unconstrained getbootfile is not a "
            f"proof ({engine})"
        )
    if re.search(r"\bdevname(?:_r)?\s*\(", body):
        return (
            f"devname unencoded: unconstrained devname is not a "
            f"proof ({engine})"
        )
    if re.search(r"\bsbrk\s*\(", body):
        return (
            f"sbrk unencoded: unconstrained sbrk is not a "
            f"proof ({engine})"
        )
    if re.search(r"\bbrk\s*\(", body):
        return (
            f"brk unencoded: unconstrained brk is not a "
            f"proof ({engine})"
        )
    if re.search(r"\b(?:mmap|munmap|mprotect)\s*\(", body):
        return f"mmap unencoded: {engine} is not a VM model"
    if re.search(r"\bioctl\s*\(", body):
        return (
            f"ioctl unencoded: unconstrained ioctl is not a "
            f"proof ({engine})"
        )
    if re.search(r"\bmod(?:find|stat|next|fnext)\s*\(", body):
        return (
            f"modfind unencoded: unconstrained modfind is not a "
            f"proof ({engine})"
        )
    if re.search(r"\bkld(?:firstmod|nextmod)\s*\(", body):
        return (
            f"kldfirstmod unencoded: unconstrained kldfirstmod is not a "
            f"proof ({engine})"
        )
    if re.search(r"\bkld_(?:isloaded|load)\s*\(", body):
        return (
            f"kld_load unencoded: unconstrained kld_load is not a "
            f"proof ({engine})"
        )
    if re.search(r"\bkld(?:load|unload|find|sym|stat)\s*\(", body):
        return (
            f"kldload unencoded: unconstrained kldload is not a "
            f"proof ({engine})"
        )
    if re.search(r"\b(?:dlfunc|dlvsym)\s*\(", body):
        return (
            f"dlfunc unencoded: unconstrained dlfunc is not a "
            f"proof ({engine})"
        )
    if re.search(r"\b(?:dlopen|dlsym|dlclose)\s*\(", body):
        return (
            f"dlopen unencoded: {engine} is not a dynamic-loader model"
        )
    if re.search(
        r"\b(?:__builtin_clzll|__builtin_ctzll|"
        r"__builtin_clz|__builtin_ctz)\s*\(",
        body,
    ):
        return (
            f"clz unencoded: unconstrained __builtin_clz is UB on 0 "
            f"and is not a proof ({engine})"
        )
    if re.search(r"\b__builtin_choose_expr\s*\(", body):
        return (
            f"choose_expr unencoded: {engine} is not a "
            "__builtin_choose_expr model"
        )
    if re.search(r"\bstd\s*::\s*unreachable\s*\(", body):
        return (
            f"C++ std::unreachable unencoded: {engine} is not an "
            "unreachable model"
        )
    if re.search(r"\b(?:accept4|accept)\s*\(", body):
        return (
            f"accept unencoded: unconstrained accept is not a "
            f"proof ({engine})"
        )
    if re.search(r"\bfchmodat2\s*\(", body):
        return (
            f"fchmodat2 unencoded: unconstrained fchmodat2 is not a "
            f"proof ({engine})"
        )
    if re.search(r"\bfchmodat\s*\(", body):
        return (
            f"fchmodat unencoded: unconstrained fchmodat is not a "
            f"proof ({engine})"
        )
    if re.search(r"\b(?:fchmod|chmod)\s*\(", body):
        return (
            f"chmod unencoded: unconstrained chmod is not a "
            f"proof ({engine})"
        )
    if re.search(r"\b(?:f|l)?chflags\s*\(", body):
        return (
            f"chflags unencoded: unconstrained chflags is not a "
            f"proof ({engine})"
        )
    if re.search(
        r"\b(?:setreuid|setregid|setresuid|setresgid)\s*\(",
        body,
    ):
        return (
            f"setreuid unencoded: unconstrained setreuid is not a "
            f"proof ({engine})"
        )
    if re.search(r"\bgetres(?:uid|gid)\s*\(", body):
        return (
            f"getresuid unencoded: unconstrained getresuid is not a "
            f"proof ({engine})"
        )
    if re.search(r"\b(?:setfsuid|setfsgid)\s*\(", body):
        return (
            f"setfsuid unencoded: unconstrained setfsuid is not a "
            f"proof ({engine})"
        )
    if re.search(r"\b(?:setpgid|setsid|getsid)\s*\(", body):
        return (
            f"setpgid unencoded: unconstrained setpgid is not a "
            f"proof ({engine})"
        )
    if re.search(r"\b(?:setuid|seteuid|setgid)\s*\(", body):
        return (
            f"setuid unencoded: unconstrained setuid is not a "
            f"proof ({engine})"
        )
    if re.search(r"\bsocket\s*\(", body):
        return (
            f"socket unencoded: unconstrained socket is not a "
            f"proof ({engine})"
        )
    if re.search(r"(?<![:\w])bind\s*\(", body):
        return (
            f"bind unencoded: unconstrained bind is not a "
            f"proof ({engine})"
        )
    if re.search(r"\blisten\s*\(", body):
        return (
            f"listen unencoded: unconstrained listen is not a "
            f"proof ({engine})"
        )
    if re.search(r"\bconnect\s*\(", body):
        return (
            f"connect unencoded: unconstrained connect is not a "
            f"proof ({engine})"
        )
    if re.search(r"\bpipe2?\s*\(", body):
        return f"pipe unencoded: {engine} is not a pipe model"
    if re.search(r"\bdup[23]?\s*\(", body):
        return f"dup unencoded: {engine} is not an fd model"
    if re.search(r"\bfcntl\s*\(", body):
        return (
            f"fcntl unencoded: unconstrained fcntl is not a "
            f"proof ({engine})"
        )
    if re.search(r"\bpd(?:getpid|wait4)\s*\(", body):
        return (
            f"pdgetpid unencoded: unconstrained pdgetpid is not a "
            f"proof ({engine})"
        )
    if re.search(r"\b(?:wait4|wait3)\s*\(", body):
        return (
            f"wait4 unencoded: unconstrained wait4 is not a "
            f"proof ({engine})"
        )
    if re.search(r"\bwait6\s*\(", body):
        return (
            f"wait6 unencoded: unconstrained wait6 is not a "
            f"proof ({engine})"
        )
    if re.search(r"\bwait(?:pid|id)?\s*\(", body):
        return (
            f"wait unencoded: unconstrained wait is not a "
            f"proof ({engine})"
        )
    if re.search(r"\bunlinkat\s*\(", body):
        return (
            f"unlinkat unencoded: unconstrained unlinkat is not a "
            f"proof ({engine})"
        )
    if re.search(r"\bunlink\s*\(", body):
        return (
            f"unlink unencoded: unconstrained unlink is not a "
            f"proof ({engine})"
        )
    if re.search(r"\bmknodat\s*\(", body):
        return (
            f"mknodat unencoded: unconstrained mknodat is not a "
            f"proof ({engine})"
        )
    if re.search(r"\b(?:mkfifo|mknod)\s*\(", body):
        return (
            f"mkfifo unencoded: unconstrained mkfifo is not a "
            f"proof ({engine})"
        )
    if re.search(r"\bepoll_create(?:1)?\s*\(", body):
        return (
            f"epoll_create unencoded: unconstrained epoll_create is not a "
            f"proof ({engine})"
        )
    if re.search(r"\bepoll_pwait(?:2)?\s*\(", body):
        return (
            f"epoll_pwait unencoded: unconstrained epoll_pwait is not a "
            f"proof ({engine})"
        )
    if re.search(r"\bppoll\s*\(", body):
        return (
            f"ppoll unencoded: unconstrained ppoll is not a "
            f"proof ({engine})"
        )
    if re.search(
        r"\b(?:pselect|select|epoll_wait|epoll_ctl|poll)\s*\(",
        body,
    ):
        return (
            f"select unencoded: unconstrained I/O multiplex is not a "
            f"proof ({engine})"
        )
    if re.search(r"\b(?:sendmsg|recvmsg)\s*\(", body):
        return (
            f"sendmsg unencoded: unconstrained sendmsg is not a "
            f"proof ({engine})"
        )
    if re.search(
        r"\b(?:sendto|recvfrom|send|recv|shutdown)\s*\(",
        body,
    ):
        return (
            f"send unencoded: unconstrained socket I/O is not a "
            f"proof ({engine})"
        )
    if re.search(r"\brt_(?:tgsigqueueinfo|sigqueueinfo)\s*\(", body):
        return (
            f"rt_sigqueueinfo unencoded: unconstrained rt_sigqueueinfo "
            f"is not a proof ({engine})"
        )
    if re.search(r"\bsigqueue\s*\(", body):
        return (
            f"sigqueue unencoded: unconstrained sigqueue is not a "
            f"proof ({engine})"
        )
    if re.search(r"\bkill_dependency\s*\(", body):
        return (
            f"C++ kill_dependency unencoded: {engine} is not a "
            "kill_dependency model"
        )
    if re.search(r"\bthr_(?:new|kill2|kill|self|exit|suspend|wake)\s*\(", body):
        return (
            f"thr_kill unencoded: unconstrained thr_kill is not a "
            f"proof ({engine})"
        )
    if re.search(r"\bpthread_kill\s*\(", body):
        return (
            f"pthread_kill unencoded: unconstrained pthread_kill is not a "
            f"proof ({engine})"
        )
    if re.search(r"\b(?:kill|raise|alarm)\s*\(", body):
        return (
            f"kill unencoded: unconstrained signal delivery is not a "
            f"proof ({engine})"
        )
    if re.search(r"\b(?:getaddrinfo|freeaddrinfo)\s*\(", body):
        return (
            f"addrinfo unencoded: unconstrained DNS is not a "
            f"proof ({engine})"
        )
    if re.search(r"\bpthread_cancel\s*\(", body):
        return (
            f"pthread_cancel unencoded: unconstrained pthread_cancel "
            f"is not a proof ({engine})"
        )
    if re.search(
        r"\bpthread_(?:key_create|key_delete|setspecific|getspecific)\s*\(",
        body,
    ):
        return (
            f"pthread_key_create unencoded: unconstrained "
            f"pthread_key_create is not a proof ({engine})"
        )
    if re.search(r"\b(?:pthread_join|pthread_detach)\s*\(", body):
        return (
            f"pthread_join unencoded: unconstrained pthread_join is not a "
            f"proof ({engine})"
        )
    if re.search(
        r"\bthrd_(?:create|join|detach|exit|sleep|yield|current|equal)\s*\(",
        body,
    ):
        return (
            f"ISO C11 thrd unencoded: unconstrained thrd_* is not a "
            f"proof ({engine})"
        )
    if re.search(r"\b(?:sem_wait|sem_post)\s*\(", body):
        return (
            f"sem unencoded: unconstrained sem_wait is not a "
            f"proof ({engine})"
        )
    if re.search(r"\bopenat\s*\(", body):
        return (
            f"openat unencoded: unconstrained openat is not a "
            f"proof ({engine})"
        )
    if re.search(r"\bflock\s*\(", body):
        return (
            f"flock unencoded: unconstrained flock is not a "
            f"proof ({engine})"
        )
    if re.search(r"\b(?:fchown|lchown|chown)\s*\(", body):
        return (
            f"chown unencoded: unconstrained chown is not a "
            f"proof ({engine})"
        )
    if re.search(r"\bsymlinkat\s*\(", body):
        return (
            f"symlinkat unencoded: unconstrained symlinkat is not a "
            f"proof ({engine})"
        )
    if re.search(r"\breadlinkat\s*\(", body):
        return (
            f"readlinkat unencoded: unconstrained readlinkat is not a "
            f"proof ({engine})"
        )
    if re.search(r"\b(?:symlink|readlink)\s*\(", body):
        return (
            f"symlink unencoded: unconstrained symlink is not a "
            f"proof ({engine})"
        )
    if re.search(r"\b(?:pthread_join|pthread_detach|pthread_once)\s*\(", body):
        return (
            f"pthread join unencoded: unconstrained thread join is not a "
            f"proof ({engine})"
        )
    if re.search(r"\b(?:sem_wait|sem_post|sem_init|sem_destroy)\s*\(", body):
        return f"sem unencoded: {engine} is not a semaphore model"
    if re.search(r"\bksem_(?:open|close|unlink|wait|post)\s*\(", body):
        return (
            f"ksem_open unencoded: unconstrained ksem_open is not a "
            f"proof ({engine})"
        )
    if re.search(r"\bsem_(?:open|close|unlink)\s*\(", body):
        return (
            f"sem_open unencoded: unconstrained sem_open is not a "
            f"proof ({engine})"
        )
    if re.search(r"\bsem_timedwait\s*\(", body):
        return (
            f"sem_timedwait unencoded: unconstrained sem_timedwait is not a "
            f"proof ({engine})"
        )
    if re.search(r"\bsem_(?:trywait|getvalue)\s*\(", body):
        return (
            f"sem_trywait unencoded: unconstrained sem_trywait is not a "
            f"proof ({engine})"
        )
    if re.search(
        r"\bpthread_spin_(?:try)?(?:lock|unlock|init|destroy)\s*\(",
        body,
    ):
        return (
            f"pthread_spin unencoded: unconstrained pthread_spin_lock "
            f"is not a proof ({engine})"
        )
    if re.search(
        r"\bpthread_rwlock_(?:try|timed)?(?:rdlock|wrlock|unlock|init|destroy)\s*\(",
        body,
    ):
        return (
            f"pthread_rwlock unencoded: unconstrained pthread_rwlock "
            f"is not a proof ({engine})"
        )
    if re.search(
        r"\bpthread_cond_(?:timedwait|wait|signal|broadcast|init|destroy)\s*\(",
        body,
    ):
        return (
            f"pthread_cond unencoded: unconstrained pthread_cond_wait "
            f"is not a proof ({engine})"
        )
    if re.search(r"\bpthread_barrier_(?:wait|init|destroy)\s*\(", body):
        return (
            f"pthread_barrier unencoded: unconstrained pthread_barrier_wait "
            f"is not a proof ({engine})"
        )
    if re.search(r"\bopenat\s*\(", body):
        return (
            f"openat unencoded: unconstrained openat is not a "
            f"proof ({engine})"
        )
    if re.search(r"\bflock\s*\(", body):
        return (
            f"flock unencoded: unconstrained flock is not a "
            f"proof ({engine})"
        )
    if re.search(r"\b(?:posix_memalign|aligned_alloc)\s*\(", body):
        return (
            f"aligned_alloc unencoded: {engine} is not an aligned-alloc model"
        )
    if re.search(r"\bvalloc\s*\(", body):
        return (
            f"valloc unencoded: unconstrained valloc is not a "
            f"proof ({engine})"
        )
    if re.search(
        r"\b(?:__atomic_load|__atomic_store|"
        r"__sync_fetch_and_add|__sync_bool_compare_and_swap)\s*\(",
        body,
    ):
        return (
            f"atomic builtin unencoded: {engine} is not an "
            "atomic-builtin model"
        )
    if re.search(r"\b(?:fchown|lchown|chown)\s*\(", body):
        return (
            f"chown unencoded: unconstrained chown is not a "
            f"proof ({engine})"
        )
    if re.search(r"\b(?:symlink|readlink)\s*\(", body):
        return (
            f"symlink unencoded: unconstrained symlink/readlink is not a "
            f"proof ({engine})"
        )
    if re.search(r"\bfts_(?:open|read|children|close|set)\s*\(", body):
        return (
            f"fts_open unencoded: unconstrained fts_open is not a "
            f"proof ({engine})"
        )
    if re.search(r"\b(?:fdopendir|opendir|readdir|closedir)\s*\(", body):
        return (
            f"opendir unencoded: {engine} is not a DIR* model"
        )
    if re.search(r"\b(?:setrlimit|getrlimit)\s*\(", body):
        return (
            f"setrlimit unencoded: unconstrained setrlimit is not a "
            f"proof ({engine})"
        )
    if re.search(r"\b(?:getsockname|getpeername)\s*\(", body):
        return (
            f"getsockname unencoded: unconstrained getsockname is not a "
            f"proof ({engine})"
        )
    if re.search(r"\bgetpeereid\s*\(", body):
        return (
            f"getpeereid unencoded: unconstrained getpeereid is not a "
            f"proof ({engine})"
        )
    if re.search(r"\b(?:getsockopt|setsockopt)\s*\(", body):
        return (
            f"getsockopt unencoded: unconstrained socket opts is not a "
            f"proof ({engine})"
        )
    if re.search(r"\b(?:listmount|statmount)\s*\(", body):
        return (
            f"listmount unencoded: unconstrained listmount/statmount "
            f"is not a proof ({engine})"
        )
    if re.search(r"\bustat\s*\(", body):
        return (
            f"ustat unencoded: unconstrained ustat is not a "
            f"proof ({engine})"
        )
    if re.search(r"\bfile_(?:get|set)attr\s*\(", body):
        return (
            f"file_getattr unencoded: unconstrained file_getattr "
            f"is not a proof ({engine})"
        )
    if re.search(r"\bfh(?:linkat|link|readlink)\s*\(", body):
        return (
            f"fhlink unencoded: unconstrained fhlink is not a "
            f"proof ({engine})"
        )
    if re.search(r"\b(?:getfh|fhopen|fhstatfs|fhstat|getfhat)\s*\(", body):
        return (
            f"getfh unencoded: unconstrained getfh is not a "
            f"proof ({engine})"
        )
    if re.search(r"\bfstatat\s*\(", body):
        return (
            f"fstatat unencoded: unconstrained fstatat is not a "
            f"proof ({engine})"
        )
    if re.search(r"\b(?:lstat|fstat|stat)\s*\(", body):
        return (
            f"stat unencoded: unconstrained stat is not a "
            f"proof ({engine})"
        )
    if re.search(r"\brenameat2\s*\(", body):
        return (
            f"renameat2 unencoded: unconstrained renameat2 is not a "
            f"proof ({engine})"
        )
    if re.search(r"\brenameat\s*\(", body):
        return (
            f"renameat unencoded: unconstrained renameat is not a "
            f"proof ({engine})"
        )
    if re.search(r"\bmkdirat\s*\(", body):
        return (
            f"mkdirat unencoded: unconstrained mkdirat is not a "
            f"proof ({engine})"
        )
    if re.search(r"\b(?:mkdir|rmdir|rename)\s*\(", body):
        return (
            f"mkdir unencoded: unconstrained mkdir is not a "
            f"proof ({engine})"
        )
    if re.search(r"\bcrypt_(?:newhash|checkpass)\s*\(", body):
        return (
            f"crypt_newhash unencoded: unconstrained crypt_newhash is not a "
            f"proof ({engine})"
        )
    if re.search(r"\bkenv\s*\(", body):
        return (
            f"kenv unencoded: unconstrained kenv is not a "
            f"proof ({engine})"
        )
    if re.search(r"\b(?:getpwuid|getpwnam|crypt)\s*\(", body):
        return (
            f"getpwuid unencoded: unconstrained getpwuid is not a "
            f"proof ({engine})"
        )
    if re.search(
        r"\b(?:clock_settime|clock_adjtime|clock_nanosleep)\s*\(",
        body,
    ):
        return (
            f"clock_settime unencoded: unconstrained clock_settime "
            f"is not a proof ({engine})"
        )
    if re.search(r"\bsettimeofday\s*\(", body):
        return (
            f"settimeofday unencoded: unconstrained settimeofday is not a "
            f"proof ({engine})"
        )
    if re.search(r"\bpthread_getcpuclockid\s*\(", body):
        return (
            f"pthread_getcpuclockid unencoded: unconstrained "
            f"pthread_getcpuclockid is not a proof ({engine})"
        )
    if re.search(r"\bclock_getcpuclockid\s*\(", body):
        return (
            f"clock_getcpuclockid unencoded: unconstrained "
            f"clock_getcpuclockid is not a proof ({engine})"
        )
    if re.search(r"\b(?:clock_gettime|gettimeofday)\s*\(", body):
        return (
            f"clock_gettime unencoded: unconstrained clock_gettime is not a "
            f"proof ({engine})"
        )
    if re.search(r"\bposix_typed_mem_(?:open|get_info)\s*\(", body):
        return (
            f"posix_typed_mem_open unencoded: unconstrained "
            f"posix_typed_mem_open is not a proof ({engine})"
        )
    if re.search(r"\b(?:shm_open|shm_unlink)\s*\(", body):
        return (
            f"shm unencoded: unconstrained shm_open is not a "
            f"proof ({engine})"
        )
    if re.search(
        r"\bposix_spawn(?:_file_actions|attr)_init\s*\(",
        body,
    ):
        return (
            f"posix_spawn_file_actions_init unencoded: unconstrained "
            f"posix_spawn_file_actions_init is not a proof ({engine})"
        )
    if re.search(r"\b(?:posix_spawnp|posix_spawn)\s*\(", body):
        return (
            f"posix_spawn unencoded: unconstrained posix_spawn is not a "
            f"proof ({engine})"
        )
    if re.search(r"\b(?:globfree|glob)\s*\(", body):
        return (
            f"glob unencoded: unconstrained glob is not a "
            f"proof ({engine})"
        )
    if re.search(r"\b(?:fseek|ftell|rewind|fgetpos|fsetpos)\s*\(", body):
        return (
            f"fseek unencoded: unconstrained fseek is not a "
            f"proof ({engine})"
        )
    if re.search(r"\b(?:nanosleep|usleep|sleep)\s*\(", body):
        return (
            f"sleep unencoded: unconstrained sleep is not a "
            f"proof ({engine})"
        )
    if re.search(r"\bfaccessat2\s*\(", body):
        return (
            f"faccessat2 unencoded: unconstrained faccessat2 is not a "
            f"proof ({engine})"
        )
    if re.search(r"\bfaccessat\s*\(", body):
        return (
            f"faccessat unencoded: unconstrained faccessat is not a "
            f"proof ({engine})"
        )
    if re.search(r"\bvhangup\s*\(", body):
        return (
            f"vhangup unencoded: unconstrained vhangup is not a "
            f"proof ({engine})"
        )
    if re.search(r"\b(?:at_)?quick_exit\s*\(", body):
        return (
            f"quick_exit unencoded: unconstrained quick_exit is not a "
            f"proof ({engine})"
        )
    if re.search(r"\beaccess\s*\(", body):
        return (
            f"eaccess unencoded: unconstrained eaccess is not a "
            f"proof ({engine})"
        )
    if re.search(r"\baccess\s*\(", body):
        return (
            f"access unencoded: unconstrained access is not a "
            f"proof ({engine})"
        )
    if re.search(r"\b(?:getopt_long_only|getopt_long|getopt)\s*\(", body):
        return (
            f"getopt unencoded: unconstrained getopt is not a "
            f"proof ({engine})"
        )
    if re.search(r"\bgetosreldate\s*\(", body):
        return (
            f"getosreldate unencoded: unconstrained getosreldate is not a "
            f"proof ({engine})"
        )
    if re.search(r"\bgetdomainname\s*\(", body):
        return (
            f"getdomainname unencoded: unconstrained getdomainname "
            f"is not a proof ({engine})"
        )
    if re.search(r"\b(?:uname|gethostname)\s*\(", body):
        return (
            f"uname unencoded: unconstrained uname is not a "
            f"proof ({engine})"
        )
    if re.search(r"\b(?:sendfile|copy_file_range)\s*\(", body):
        return (
            f"sendfile unencoded: unconstrained sendfile is not a "
            f"proof ({engine})"
        )
    if re.search(r"\b(?:preadv2|pwritev2|preadv|pwritev)\s*\(", body):
        return (
            f"preadv unencoded: unconstrained preadv is not a "
            f"proof ({engine})"
        )
    if re.search(
        r"\b(?:timerfd_settime|timerfd_gettime)\s*\(",
        body,
    ):
        return (
            f"timerfd_settime unencoded: unconstrained "
            f"timerfd_settime is not a proof ({engine})"
        )
    if re.search(r"\beventfd_(?:read|write)\s*\(", body):
        return (
            f"eventfd_read unencoded: unconstrained eventfd_read is not a "
            f"proof ({engine})"
        )
    if re.search(
        r"\b(?:memfd_create|eventfd|timerfd_create)\s*\(",
        body,
    ):
        return (
            f"memfd unencoded: unconstrained memfd_create is not a "
            f"proof ({engine})"
        )
    if re.search(r"\barch_prctl\s*\(", body):
        return (
            f"arch_prctl unencoded: unconstrained arch_prctl is not a "
            f"proof ({engine})"
        )
    if re.search(r"\bprocctl\s*\(", body):
        return (
            f"procctl unencoded: unconstrained procctl is not a "
            f"proof ({engine})"
        )
    if re.search(r"\b(?:prctl|ptrace)\s*\(", body):
        return (
            f"prctl unencoded: unconstrained prctl is not a "
            f"proof ({engine})"
        )
    if re.search(r"\bktrace\s*\(", body):
        return (
            f"ktrace unencoded: unconstrained ktrace is not a "
            f"proof ({engine})"
        )
    if re.search(r"\b(?:tcgetattr|tcsetattr|cfmakeraw)\s*\(", body):
        return (
            f"tcgetattr unencoded: unconstrained tcgetattr is not a "
            f"proof ({engine})"
        )
    if re.search(r"\bgetpagesizes\s*\(", body):
        return (
            f"getpagesizes unencoded: unconstrained getpagesizes is not a "
            f"proof ({engine})"
        )
    if re.search(r"\bgetpagesize(?!s)\s*\(", body):
        return (
            f"getpagesize unencoded: unconstrained getpagesize is not a "
            f"proof ({engine})"
        )
    if re.search(r"\blpathconf\s*\(", body):
        return (
            f"lpathconf unencoded: unconstrained lpathconf is not a "
            f"proof ({engine})"
        )
    if re.search(r"\b(?:sysconf|fpathconf|pathconf)\s*\(", body):
        return (
            f"sysconf unencoded: unconstrained sysconf is not a "
            f"proof ({engine})"
        )
    if re.search(r"\bgetrusage\s*\(", body):
        return (
            f"getrusage unencoded: unconstrained getrusage is not a "
            f"proof ({engine})"
        )
    if re.search(r"\b(?:nftw|ftw)\s*\(", body):
        return (
            f"nftw unencoded: unconstrained nftw is not a "
            f"proof ({engine})"
        )
    if re.search(r"\b(?:wordexp|wordfree)\s*\(", body):
        return (
            f"wordexp unencoded: unconstrained wordexp is not a "
            f"proof ({engine})"
        )
    if re.search(r"\b(?:get|set)loginclass\s*\(", body):
        return (
            f"loginclass unencoded: unconstrained getloginclass is not a "
            f"proof ({engine})"
        )
    if re.search(r"\b(?:login_getclass|setusercontext)\s*\(", body):
        return (
            f"login_getclass unencoded: unconstrained login_getclass is not a "
            f"proof ({engine})"
        )
    if re.search(r"\bsetlogin\s*\(", body):
        return (
            f"setlogin unencoded: unconstrained setlogin is not a "
            f"proof ({engine})"
        )
    if re.search(
        r"\b(?:getlogin_r|getlogin|ttyname_r|ttyname)\s*\(",
        body,
    ):
        return (
            f"getlogin unencoded: unconstrained getlogin is not a "
            f"proof ({engine})"
        )
    if re.search(r"\b(?:inet_pton|inet_ntop|inet_aton)\s*\(", body):
        return (
            f"inet_pton unencoded: unconstrained inet_pton is not a "
            f"proof ({engine})"
        )
    if re.search(r"\bmseal\s*\(", body):
        return (
            f"mseal unencoded: unconstrained mseal is not a "
            f"proof ({engine})"
        )
    if re.search(r"\bmlock2\s*\(", body):
        return (
            f"mlock2 unencoded: unconstrained mlock2 is not a "
            f"proof ({engine})"
        )
    if re.search(r"\b(?:munlockall|mlockall|munlock|mlock)\s*\(", body):
        return (
            f"mlock unencoded: unconstrained mlock is not a "
            f"proof ({engine})"
        )
    if re.search(r"\bposix_fadvise(?:64)?\s*\(", body):
        return (
            f"posix_fadvise unencoded: unconstrained posix_fadvise is not a "
            f"proof ({engine})"
        )
    if re.search(r"\breadahead\s*\(", body):
        return (
            f"readahead unencoded: unconstrained readahead is not a "
            f"proof ({engine})"
        )
    if re.search(r"\b(?:posix_madvise|madvise)\s*\(", body):
        return (
            f"madvise unencoded: unconstrained madvise is not a "
            f"proof ({engine})"
        )
    if re.search(r"\b(?:vmsplice|splice)\s*\(", body):
        return (
            f"splice unencoded: unconstrained splice is not a "
            f"proof ({engine})"
        )
    if re.search(r"\binotify_rm_watch\s*\(", body):
        return (
            f"inotify_rm_watch unencoded: unconstrained inotify_rm_watch "
            f"is not a proof ({engine})"
        )
    if re.search(
        r"\b(?:inotify_init1|inotify_init|inotify_add_watch)\s*\(",
        body,
    ):
        return (
            f"inotify unencoded: unconstrained inotify is not a "
            f"proof ({engine})"
        )
    if re.search(r"\b(?:fdatasync|fsync)\s*\(", body):
        return (
            f"fsync unencoded: unconstrained fsync is not a "
            f"proof ({engine})"
        )
    if re.search(r"\b(?:getrandom|getentropy)\s*\(", body):
        return (
            f"getrandom unencoded: unconstrained getrandom is not a "
            f"proof ({engine})"
        )
    if re.search(r"\barc4random(?:_buf|_uniform)?\s*\(", body):
        return (
            f"arc4random unencoded: unconstrained arc4random is not a "
            f"proof ({engine})"
        )
    if re.search(r"\bissetugid\s*\(", body):
        return (
            f"issetugid unencoded: unconstrained issetugid is not a "
            f"proof ({engine})"
        )
    if re.search(r"\b(?:getdelim|getline)\s*\(", body):
        return (
            f"getline unencoded: unconstrained getline is not a "
            f"proof ({engine})"
        )
    if re.search(r"\b(?:vasprintf|asprintf)\s*\(", body):
        return (
            f"asprintf unencoded: unconstrained asprintf is not a "
            f"proof ({engine})"
        )
    if re.search(r"\b(?:strlcpy|strlcat)\s*\(", body):
        return (
            f"strlcpy unencoded: unconstrained strlcpy is not a "
            f"proof ({engine})"
        )
    if re.search(
        r"\b(?:explicit_bzero|memset_s|explicit_memset)\s*\(",
        body,
    ):
        return (
            f"explicit_bzero unencoded: unconstrained explicit_bzero "
            f"is not a proof ({engine})"
        )
    if re.search(r"\btimingsafe_(?:bcmp|memcmp)\s*\(", body):
        return (
            f"timingsafe_bcmp unencoded: unconstrained timingsafe_bcmp "
            f"is not a proof ({engine})"
        )
    if re.search(r"\bisatty\s*\(", body):
        return (
            f"isatty unencoded: unconstrained isatty is not a "
            f"proof ({engine})"
        )
    if re.search(
        r"\b(?:posix_openpt|ptsname_r|ptsname|grantpt|unlockpt)\s*\(",
        body,
    ):
        return (
            f"ptsname unencoded: unconstrained ptsname is not a "
            f"proof ({engine})"
        )
    if re.search(r"\bnmount\s*\(", body):
        return (
            f"nmount unencoded: unconstrained nmount is not a "
            f"proof ({engine})"
        )
    if re.search(r"\bunmount\b", body):
        return (
            f"unmount unencoded: unconstrained unmount is not a "
            f"proof ({engine})"
        )
    if re.search(r"\b(?:umount2|umount|mount)\s*\(", body):
        return (
            f"mount unencoded: unconstrained mount is not a "
            f"proof ({engine})"
        )
    if re.search(
        r"\b(?:open_wmemstream|open_memstream|fmemopen)\s*\(",
        body,
    ):
        return (
            f"fmemopen unencoded: unconstrained fmemopen is not a "
            f"proof ({engine})"
        )
    if re.search(r"\bscandir\s*\(", body):
        return (
            f"scandir unencoded: unconstrained scandir is not a "
            f"proof ({engine})"
        )
    if re.search(r"\bextattr_(?:set|get|delete|list)_(?:file|fd|link)\s*\(", body):
        return (
            f"extattr unencoded: unconstrained extattr_set_file is not a "
            f"proof ({engine})"
        )
    if re.search(
        r"\b(?:set|get|list|remove)xattrat\s*\(",
        body,
    ):
        return (
            f"setxattrat unencoded: unconstrained setxattrat is not a "
            f"proof ({engine})"
        )
    if re.search(
        r"\b(?:lsetxattr|fsetxattr|setxattr|listxattr|"
        r"removexattr|getxattr)\s*\(",
        body,
    ):
        return (
            f"setxattr unencoded: unconstrained setxattr is not a "
            f"proof ({engine})"
        )
    if re.search(r"\b(?:sched_setattr|sched_getattr)\s*\(", body):
        return (
            f"sched_setattr unencoded: unconstrained sched_setattr is not a "
            f"proof ({engine})"
        )
    if re.search(r"\bsched_yield\s*\(", body):
        return (
            f"sched_yield unencoded: unconstrained sched_yield is not a "
            f"proof ({engine})"
        )
    if re.search(r"\bpthread_yield\s*\(", body):
        return (
            f"pthread_yield unencoded: unconstrained pthread_yield is not a "
            f"proof ({engine})"
        )
    if re.search(r"\bcpuset_(?:set|get)affinity\s*\(", body):
        return (
            f"cpuset unencoded: unconstrained cpuset_setaffinity is not a "
            f"proof ({engine})"
        )
    if re.search(r"\bsched_get_priority_(?:max|min)\s*\(", body):
        return (
            f"sched_get_priority_max unencoded: unconstrained "
            f"sched_get_priority_max is not a proof ({engine})"
        )
    if re.search(r"\b(?:sched_setaffinity|sched_getaffinity)\s*\(", body):
        return (
            f"sched unencoded: unconstrained sched_setaffinity is not a "
            f"proof ({engine})"
        )
    if re.search(
        r"\b(?:sched_setscheduler|sched_getscheduler|"
        r"sched_setparam|sched_getparam)\s*\(",
        body,
    ):
        return (
            f"sched_setscheduler unencoded: unconstrained "
            f"sched_setscheduler is not a proof ({engine})"
        )
    if re.search(r"\blio_listio\s*\(", body):
        return (
            f"lio_listio unencoded: unconstrained lio_listio is not a "
            f"proof ({engine})"
        )
    if re.search(
        r"\b(?:aio_suspend|aio_return|aio_error|aio_write|aio_read)\s*\(",
        body,
    ):
        return (
            f"aio unencoded: unconstrained aio_read is not a "
            f"proof ({engine})"
        )
    if re.search(
        r"\b(?:io_uring_register|io_uring_setup|io_uring_enter)\s*\(",
        body,
    ):
        return (
            f"io_uring unencoded: unconstrained io_uring_setup is not a "
            f"proof ({engine})"
        )
    if re.search(r"\b(?:capset|capget)\s*\(", body):
        return (
            f"capset unencoded: unconstrained capset is not a "
            f"proof ({engine})"
        )
    if re.search(r"\bstatx\s*\(", body):
        return (
            f"statx unencoded: unconstrained statx is not a "
            f"proof ({engine})"
        )
    if re.search(
        r"\b(?:pidfd_send_signal|pidfd_getfd|pidfd_open)\s*\(",
        body,
    ):
        return (
            f"pidfd unencoded: unconstrained pidfd_open is not a "
            f"proof ({engine})"
        )
    if re.search(r"\b(?:fanotify_init|fanotify_mark)\s*\(", body):
        return (
            f"fanotify unencoded: unconstrained fanotify_init is not a "
            f"proof ({engine})"
        )
    if re.search(r"\bseccomp\s*\(", body):
        return (
            f"seccomp unencoded: unconstrained seccomp is not a "
            f"proof ({engine})"
        )
    if re.search(r"\b(?:getgrnam|getgrgid|getspnam)\s*\(", body):
        return (
            f"getgrnam unencoded: unconstrained getgrnam is not a "
            f"proof ({engine})"
        )
    if re.search(r"\b(?:posix_fallocate|fallocate)\s*\(", body):
        return (
            f"fallocate unencoded: unconstrained posix_fallocate is not a "
            f"proof ({engine})"
        )
    if re.search(r"\bclose_range\s*\(", body):
        return (
            f"close_range unencoded: unconstrained close_range is not a "
            f"proof ({engine})"
        )
    if re.search(r"\bclosefrom\s*\(", body):
        return (
            f"closefrom unencoded: unconstrained closefrom is not a "
            f"proof ({engine})"
        )
    if re.search(
        r"\blsm_(?:get_self_attr|set_self_attr|list_modules)\s*\(",
        body,
    ):
        return (
            f"lsm_get_self_attr unencoded: unconstrained "
            f"lsm_get_self_attr is not a proof ({engine})"
        )
    if re.search(
        r"\b(?:landlock_create_ruleset|landlock_add_rule|"
        r"landlock_restrict_self)\s*\(",
        body,
    ):
        return (
            f"landlock unencoded: unconstrained landlock_create_ruleset "
            f"is not a proof ({engine})"
        )
    if re.search(r"\brtprio(?:_thread)?\s*\(", body):
        return (
            f"rtprio unencoded: unconstrained rtprio is not a "
            f"proof ({engine})"
        )
    if re.search(r"\b(?:getpriority|setpriority)\s*\(", body):
        return (
            f"getpriority unencoded: unconstrained getpriority is not a "
            f"proof ({engine})"
        )
    if re.search(r"\bsignalfd\s*\(", body):
        return (
            f"signalfd unencoded: unconstrained signalfd is not a "
            f"proof ({engine})"
        )
    if re.search(r"\bsigaction\s*\(", body):
        return (
            f"sigaction unencoded: unconstrained sigaction is not a "
            f"proof ({engine})"
        )
    if re.search(r"\bpthread_sigmask\s*\(", body):
        return (
            f"pthread_sigmask unencoded: unconstrained pthread_sigmask "
            f"is not a proof ({engine})"
        )
    if re.search(r"\bsig(?:procmask|suspend)\s*\(", body):
        return (
            f"sigprocmask unencoded: unconstrained sigprocmask is not a "
            f"proof ({engine})"
        )
    if re.search(r"\bsig(?:waitinfo|timedwait|pending|wait)\s*\(", body):
        return (
            f"sigwait unencoded: unconstrained sigwait is not a "
            f"proof ({engine})"
        )
    if re.search(r"\bsigaltstack\s*\(", body):
        return (
            f"sigaltstack unencoded: unconstrained sigaltstack is not a "
            f"proof ({engine})"
        )
    if re.search(r"\bbpf\s*\(", body):
        return (
            f"bpf unencoded: unconstrained bpf is not a "
            f"proof ({engine})"
        )
    if re.search(r"\buserfaultfd\s*\(", body):
        return (
            f"userfaultfd unencoded: unconstrained userfaultfd is not a "
            f"proof ({engine})"
        )
    if re.search(r"\bgetpass\s*\(", body):
        return (
            f"getpass unencoded: unconstrained getpass is not a "
            f"proof ({engine})"
        )
    if re.search(r"\bgetgrouplist\s*\(", body):
        return (
            f"getgrouplist unencoded: unconstrained getgrouplist is not a "
            f"proof ({engine})"
        )
    if re.search(r"\bgetgroups\s*\(", body):
        return (
            f"getgroups unencoded: unconstrained getgroups is not a "
            f"proof ({engine})"
        )
    if re.search(r"\b(?:initgroups|setgroups)\s*\(", body):
        return (
            f"initgroups unencoded: unconstrained initgroups is not a "
            f"proof ({engine})"
        )
    if re.search(r"\b(?:unshare|setns|clone)\s*\(", body):
        return (
            f"unshare unencoded: unconstrained unshare/clone is not a "
            f"proof ({engine})"
        )
    if re.search(r"\bopenat2\s*\(", body):
        return (
            f"openat2 unencoded: unconstrained openat2 is not a "
            f"proof ({engine})"
        )
    if re.search(r"\b(?:sendmmsg|recvmmsg)\s*\(", body):
        return (
            f"sendmmsg unencoded: unconstrained sendmmsg is not a "
            f"proof ({engine})"
        )
    if re.search(r"\b(?:name_to_handle_at|open_by_handle_at)\s*\(", body):
        return (
            f"name_to_handle unencoded: unconstrained name_to_handle_at "
            f"is not a proof ({engine})"
        )
    if re.search(r"\bprocess_madvise\s*\(", body):
        return (
            f"process_madvise unencoded: unconstrained process_madvise "
            f"is not a proof ({engine})"
        )
    if re.search(r"\bpersonality\s*\(", body):
        return (
            f"personality unencoded: unconstrained personality is not a "
            f"proof ({engine})"
        )
    if re.search(r"\bquotactl\s*\(", body):
        return (
            f"quotactl unencoded: unconstrained quotactl is not a "
            f"proof ({engine})"
        )
    if re.search(r"\bpivot_root\s*\(", body):
        return (
            f"pivot_root unencoded: unconstrained pivot_root is not a "
            f"proof ({engine})"
        )
    if re.search(r"\bmembarrier\s*\(", body):
        return (
            f"membarrier unencoded: unconstrained membarrier is not a "
            f"proof ({engine})"
        )
    if re.search(r"\bpkey_alloc\s*\(", body):
        return (
            f"pkey_alloc unencoded: unconstrained pkey_alloc is not a "
            f"proof ({engine})"
        )
    if re.search(r"\bstatfs\s*\(", body):
        return (
            f"statfs unencoded: unconstrained statfs is not a "
            f"proof ({engine})"
        )
    if re.search(r"\bgetmntinfo\s*\(", body):
        return (
            f"getmntinfo unencoded: unconstrained getmntinfo is not a "
            f"proof ({engine})"
        )
    if re.search(r"\bgetvfsbyname\s*\(", body):
        return (
            f"getvfsbyname unencoded: unconstrained getvfsbyname is not a "
            f"proof ({engine})"
        )
    if re.search(r"\b(?:get|set|end)fsent\s*\(", body):
        return (
            f"getfsent unencoded: unconstrained getfsent is not a "
            f"proof ({engine})"
        )
    if re.search(r"\bgetfsstat\s*\(", body):
        return (
            f"getfsstat unencoded: unconstrained getfsstat is not a "
            f"proof ({engine})"
        )
    if re.search(r"\bsyncfs\s*\(", body):
        return (
            f"syncfs unencoded: unconstrained syncfs is not a "
            f"proof ({engine})"
        )
    if re.search(r"\b(?:prlimit64|prlimit)\s*\(", body):
        return (
            f"prlimit unencoded: unconstrained prlimit is not a "
            f"proof ({engine})"
        )
    if re.search(r"\b(?:migrate_pages|move_pages)\s*\(", body):
        return (
            f"move_pages unencoded: unconstrained move_pages is not a "
            f"proof ({engine})"
        )
    if re.search(r"\bprocess_vm_readv\s*\(", body):
        return (
            f"process_vm_readv unencoded: unconstrained "
            f"process_vm_readv is not a proof ({engine})"
        )
    if re.search(r"\bperf_event_open\s*\(", body):
        return (
            f"perf_event_open unencoded: unconstrained "
            f"perf_event_open is not a proof ({engine})"
        )
    if re.search(r"\bclone3\s*\(", body):
        return (
            f"clone3 unencoded: unconstrained clone3 is not a "
            f"proof ({engine})"
        )
    if re.search(r"\bkcmp\s*\(", body):
        return (
            f"kcmp unencoded: unconstrained kcmp is not a "
            f"proof ({engine})"
        )
    if re.search(r"\bkeyctl\s*\(", body):
        return (
            f"keyctl unencoded: unconstrained keyctl is not a "
            f"proof ({engine})"
        )
    if re.search(r"\bopen_tree_attr\s*\(", body):
        return (
            f"open_tree_attr unencoded: unconstrained open_tree_attr "
            f"is not a proof ({engine})"
        )
    if re.search(
        r"\b(?:fsopen|fsmount|open_tree|move_mount|fspick|fsconfig)\s*\(",
        body,
    ):
        return (
            f"fsopen unencoded: unconstrained fsopen is not a "
            f"proof ({engine})"
        )
    if re.search(r"\bprocess_mrelease\s*\(", body):
        return (
            f"process_mrelease unencoded: unconstrained "
            f"process_mrelease is not a proof ({engine})"
        )
    if re.search(r"\bmemfd_secret\s*\(", body):
        return (
            f"memfd_secret unencoded: unconstrained memfd_secret "
            f"is not a proof ({engine})"
        )
    if re.search(r"\b(?:ioprio_set|ioprio_get)\s*\(", body):
        return (
            f"ioprio unencoded: unconstrained ioprio is not a "
            f"proof ({engine})"
        )
    if re.search(r"\bmq_open\s*\(", body):
        return (
            f"mq_open unencoded: unconstrained mq_open is not a "
            f"proof ({engine})"
        )
    if re.search(r"\bshmget\s*\(", body):
        return (
            f"shmget unencoded: unconstrained shmget is not a "
            f"proof ({engine})"
        )
    if re.search(r"\b_umtx_op\s*\(", body):
        return (
            f"_umtx_op unencoded: unconstrained _umtx_op is not a "
            f"proof ({engine})"
        )
    if re.search(r"\bfutex_waitv\s*\(", body):
        return (
            f"futex_waitv unencoded: unconstrained futex_waitv is not a "
            f"proof ({engine})"
        )
    if re.search(r"\bfutex_(?:wake|wait|requeue)\s*\(", body):
        return (
            f"futex_wait unencoded: unconstrained futex_wait is not a "
            f"proof ({engine})"
        )
    if re.search(r"\bfutex\s*\(", body):
        return (
            f"futex unencoded: unconstrained futex is not a "
            f"proof ({engine})"
        )
    if re.search(r"\badjtimex\s*\(", body):
        return (
            f"adjtimex unencoded: unconstrained adjtimex is not a "
            f"proof ({engine})"
        )
    if re.search(r"\b(?:ntp_)?adjtime\s*\(", body):
        return (
            f"adjtime unencoded: unconstrained adjtime is not a "
            f"proof ({engine})"
        )
    if re.search(r"\bntp_gettime\s*\(", body):
        return (
            f"ntp_gettime unencoded: unconstrained ntp_gettime is not a "
            f"proof ({engine})"
        )
    if re.search(r"\brevoke\s*\(", body):
        return (
            f"revoke unencoded: unconstrained revoke is not a "
            f"proof ({engine})"
        )
    if re.search(r"\bjail(?:_attach|_get|_set|_remove)?\s*\(", body):
        return (
            f"jail unencoded: unconstrained jail is not a "
            f"proof ({engine})"
        )
    if re.search(r"\b(?:fflagstostr|strtofflags)\s*\(", body):
        return (
            f"fflagstostr unencoded: unconstrained fflagstostr is not a "
            f"proof ({engine})"
        )
    if re.search(r"\bstrmode\s*\(", body):
        return (
            f"strmode unencoded: unconstrained strmode is not a "
            f"proof ({engine})"
        )
    if re.search(r"\bstrtonum\s*\(", body):
        return (
            f"strtonum unencoded: unconstrained strtonum is not a "
            f"proof ({engine})"
        )
    if re.search(r"\breallocarray\s*\(", body):
        return (
            f"reallocarray unencoded: unconstrained reallocarray is not a "
            f"proof ({engine})"
        )
    if re.search(r"\breallocf\s*\(", body):
        return (
            f"reallocf unencoded: unconstrained reallocf is not a "
            f"proof ({engine})"
        )
    if re.search(r"\b(?:get|set)progname\s*\(", body):
        return (
            f"getprogname unencoded: unconstrained getprogname is not a "
            f"proof ({engine})"
        )
    if re.search(r"\b(?:setproctitle|daemon)\s*\(", body):
        return (
            f"daemon unencoded: unconstrained daemon is not a "
            f"proof ({engine})"
        )
    if re.search(r"\b(?:auditon|getaudit|setaudit|auditctl)\s*\(", body):
        return (
            f"auditon unencoded: unconstrained auditon is not a "
            f"proof ({engine})"
        )
    if re.search(r"\bkinfo_get(?:proc|file|vmmap)\s*\(", body):
        return (
            f"kinfo_getproc unencoded: unconstrained kinfo_getproc is not a "
            f"proof ({engine})"
        )
    if re.search(r"\bkvm_(?:open|openfiles|getprocs|close|nlist)\s*\(", body):
        return (
            f"kvm_open unencoded: unconstrained kvm_open is not a "
            f"proof ({engine})"
        )
    if re.search(r"\buuidgen\s*\(", body):
        return (
            f"uuidgen unencoded: unconstrained uuidgen is not a "
            f"proof ({engine})"
        )
    if re.search(r"\bsetfib\s*\(", body):
        return (
            f"setfib unencoded: unconstrained setfib is not a "
            f"proof ({engine})"
        )
    if re.search(r"\bsethostname\s*\(", body):
        return (
            f"sethostname unencoded: unconstrained sethostname is not a "
            f"proof ({engine})"
        )
    if re.search(r"\breboot\s*\(", body):
        return (
            f"reboot unencoded: unconstrained reboot is not a "
            f"proof ({engine})"
        )
    if re.search(r"\b(?:swapon|swapoff)\s*\(", body):
        return (
            f"swapon unencoded: unconstrained swapon is not a "
            f"proof ({engine})"
        )
    if re.search(r"\bacct\s*\(", body):
        return (
            f"acct unencoded: unconstrained acct is not a "
            f"proof ({engine})"
        )
    if re.search(r"\b(?:ioperm|iopl)\s*\(", body):
        return (
            f"ioperm unencoded: unconstrained ioperm is not a "
            f"proof ({engine})"
        )
    if re.search(r"\bmincore\s*\(", body):
        return (
            f"mincore unencoded: unconstrained mincore is not a "
            f"proof ({engine})"
        )
    if re.search(r"\brseq\s*\(", body):
        return (
            f"rseq unencoded: unconstrained rseq is not a "
            f"proof ({engine})"
        )
    if re.search(r"\btimer_create\s*\(", body):
        return (
            f"timer_create unencoded: unconstrained timer_create "
            f"is not a proof ({engine})"
        )
    if re.search(r"\bsemget\s*\(", body):
        return (
            f"semget unencoded: unconstrained semget is not a "
            f"proof ({engine})"
        )
    if re.search(r"\bmsgget\s*\(", body):
        return (
            f"msgget unencoded: unconstrained msgget is not a "
            f"proof ({engine})"
        )
    if re.search(r"\bsyslog\s*\(", body):
        return (
            f"syslog unencoded: unconstrained syslog is not a "
            f"proof ({engine})"
        )
    if re.search(r"\bklogctl\s*\(", body):
        return (
            f"klogctl unencoded: unconstrained klogctl is not a "
            f"proof ({engine})"
        )
    if re.search(r"\bmount_setattr\s*\(", body):
        return (
            f"mount_setattr unencoded: unconstrained "
            f"mount_setattr is not a proof ({engine})"
        )
    if re.search(r"\bgetcpu\s*\(", body):
        return (
            f"getcpu unencoded: unconstrained getcpu is not a "
            f"proof ({engine})"
        )
    if re.search(
        r"\b(?:init_module|finit_module|delete_module)\s*\(",
        body,
    ):
        return (
            f"init_module unencoded: unconstrained init_module "
            f"is not a proof ({engine})"
        )
    if re.search(r"\b(?:kexec_load|kexec_file_load)\s*\(", body):
        return (
            f"kexec unencoded: unconstrained kexec_load is not a "
            f"proof ({engine})"
        )
    if re.search(r"\bquotactl_fd\s*\(", body):
        return (
            f"quotactl_fd unencoded: unconstrained quotactl_fd "
            f"is not a proof ({engine})"
        )
    if re.search(r"\b(?:pkey_free|pkey_mprotect)\s*\(", body):
        return (
            f"pkey_free unencoded: unconstrained pkey_free is not a "
            f"proof ({engine})"
        )
    if re.search(r"\btgkill\s*\(", body):
        return (
            f"tgkill unencoded: unconstrained tgkill is not a "
            f"proof ({engine})"
        )
    if re.search(r"\badd_key\s*\(", body):
        return (
            f"add_key unencoded: unconstrained add_key is not a "
            f"proof ({engine})"
        )
    if re.search(r"\bsemctl\s*\(", body):
        return (
            f"semctl unencoded: unconstrained semctl is not a "
            f"proof ({engine})"
        )
    if re.search(r"\bmsgctl\s*\(", body):
        return (
            f"msgctl unencoded: unconstrained msgctl is not a "
            f"proof ({engine})"
        )
    if re.search(r"\bshmctl\s*\(", body):
        return (
            f"shmctl unencoded: unconstrained shmctl is not a "
            f"proof ({engine})"
        )
    if re.search(r"\btimer_settime\s*\(", body):
        return (
            f"timer_settime unencoded: unconstrained timer_settime "
            f"is not a proof ({engine})"
        )
    if re.search(r"\bsetdomainname\s*\(", body):
        return (
            f"setdomainname unencoded: unconstrained setdomainname "
            f"is not a proof ({engine})"
        )
    if re.search(r"\b(?:io_submit|io_getevents)\s*\(", body):
        return (
            f"io_submit unencoded: unconstrained io_submit is not a "
            f"proof ({engine})"
        )
    if re.search(
        r"\b(?:io_setup|io_destroy|io_cancel|io_pgetevents)\s*\(",
        body,
    ):
        return (
            f"io_setup unencoded: unconstrained io_setup is not a "
            f"proof ({engine})"
        )
    if re.search(r"\brequest_key\s*\(", body):
        return (
            f"request_key unencoded: unconstrained request_key is not a "
            f"proof ({engine})"
        )
    if re.search(r"\btkill\s*\(", body):
        return (
            f"tkill unencoded: unconstrained tkill is not a "
            f"proof ({engine})"
        )
    if re.search(
        r"\b(?:timer_delete|timer_gettime|timer_getoverrun)\s*\(",
        body,
    ):
        return (
            f"timer_delete unencoded: unconstrained timer_delete "
            f"is not a proof ({engine})"
        )
    if re.search(
        r"\b(?:mq_unlink|mq_timedsend|mq_timedreceive|mq_notify|"
        r"mq_getsetattr)\s*\(",
        body,
    ):
        return (
            f"mq_unlink unencoded: unconstrained mq_unlink is not a "
            f"proof ({engine})"
        )
    if re.search(r"\b(?:shmat|shmdt)\s*\(", body):
        return (
            f"shmat unencoded: unconstrained shmat is not a "
            f"proof ({engine})"
        )
    if re.search(r"\b(?:semop|semtimedop)\s*\(", body):
        return (
            f"semop unencoded: unconstrained semop is not a "
            f"proof ({engine})"
        )
    if re.search(r"\b(?:msgsnd|msgrcv)\s*\(", body):
        return (
            f"msgsnd unencoded: unconstrained msgsnd is not a "
            f"proof ({engine})"
        )
    if re.search(r"\bsync_file_range\s*\(", body):
        return (
            f"sync_file_range unencoded: unconstrained "
            f"sync_file_range is not a proof ({engine})"
        )
    if re.search(r"\bremap_file_pages\s*\(", body):
        return (
            f"remap_file_pages unencoded: unconstrained "
            f"remap_file_pages is not a proof ({engine})"
        )
    if re.search(r"\b(?:msync|mremap)\s*\(", body):
        return (
            f"msync unencoded: unconstrained msync is not a "
            f"proof ({engine})"
        )
    if re.search(r"\bsocketpair\s*\(", body):
        return (
            f"socketpair unencoded: unconstrained socketpair is not a "
            f"proof ({engine})"
        )
    if re.search(r"\bsysinfo\s*\(", body):
        return (
            f"sysinfo unencoded: unconstrained sysinfo is not a "
            f"proof ({engine})"
        )
    if re.search(r"\bgettid\s*\(", body):
        return (
            f"gettid unencoded: unconstrained gettid is not a "
            f"proof ({engine})"
        )
    if re.search(r"\b(?:setitimer|getitimer)\s*\(", body):
        return (
            f"setitimer unencoded: unconstrained setitimer is not a "
            f"proof ({engine})"
        )
    if re.search(r"\bnice\s*\(", body):
        return (
            f"nice unencoded: unconstrained nice is not a "
            f"proof ({engine})"
        )
    if re.search(r"\bgetdirentries\s*\(", body):
        return (
            f"getdirentries unencoded: unconstrained getdirentries is not a "
            f"proof ({engine})"
        )
    if re.search(r"\bgetdents(?:64)?\s*\(", body):
        return (
            f"getdents unencoded: unconstrained getdents is not a "
            f"proof ({engine})"
        )
    if re.search(r"\b(?:utimensat|futimens|utimes)\s*\(", body):
        return (
            f"utimensat unencoded: unconstrained utimensat is not a "
            f"proof ({engine})"
        )
    if re.search(r"\blinkat\s*\(", body):
        return (
            f"linkat unencoded: unconstrained linkat is not a "
            f"proof ({engine})"
        )
    if re.search(r"\bset_mempolicy_home_node\s*\(", body):
        return (
            f"set_mempolicy_home_node unencoded: unconstrained "
            f"set_mempolicy_home_node is not a proof ({engine})"
        )
    if re.search(
        r"\b(?:mbind|set_mempolicy|get_mempolicy)\s*\(",
        body,
    ):
        return (
            f"mbind unencoded: unconstrained mbind is not a "
            f"proof ({engine})"
        )
    if re.search(r"\bcachestat\s*\(", body):
        return (
            f"cachestat unencoded: unconstrained cachestat is not a "
            f"proof ({engine})"
        )
    if re.search(r"\bmap_shadow_stack\s*\(", body):
        return (
            f"map_shadow_stack unencoded: unconstrained "
            f"map_shadow_stack is not a proof ({engine})"
        )
    if re.search(r"\bstd\s*::\s*pmr\b|\bpmr\s*::", body):
        return (
            f"C++ pmr unencoded: {engine} is not a pmr model"
        )
    if re.search(r"\b(?:std\s*::\s*)?u8string(?:_view)?\b", body):
        return (
            f"C++ u8string unencoded: {engine} is not a u8string model"
        )
    if re.search(r"\b(?:std\s*::\s*)?unordered_multimap\s*<", body):
        return (
            f"C++ unordered_multimap unencoded: {engine} is not an "
            "unordered_multimap model"
        )
    if re.search(r"\b(?:std\s*::\s*)?unordered_multiset\s*<", body):
        return (
            f"C++ unordered_multiset unencoded: {engine} is not an "
            "unordered_multiset model"
        )
    if re.search(r"\b(?:std\s*::\s*)?shared_lock\b", body):
        return (
            f"C++ shared_lock unencoded: {engine} is not a "
            "shared_lock model"
        )
    if re.search(r"\batomic_(?:thread|signal)_fence\s*\(", body):
        return (
            f"C++ atomic_thread_fence unencoded: {engine} is not an "
            "atomic_thread_fence model"
        )
    if re.search(r"\b(?:std\s*::\s*)?atomic_flag\b", body):
        return (
            f"C++ atomic_flag unencoded: {engine} is not an "
            "atomic_flag model"
        )
    if re.search(r"\brecursive_timed_mutex\b", body):
        return (
            f"C++ recursive_timed_mutex unencoded: {engine} is not a "
            "recursive_timed_mutex model"
        )
    if re.search(r"\brecursive_mutex\b", body):
        return (
            f"C++ recursive_mutex unencoded: {engine} is not a "
            "recursive_mutex model"
        )
    if re.search(r"\btimed_mutex\b", body):
        return (
            f"C++ timed_mutex unencoded: {engine} is not a "
            "timed_mutex model"
        )
    if re.search(r"\b(?:ifstream|ofstream|fstream)\b", body):
        return (
            f"C++ fstream unencoded: {engine} is not an fstream model"
        )
    if re.search(r"\bthis_thread\b", body):
        return (
            f"C++ this_thread unencoded: {engine} is not a "
            "this_thread model"
        )
    if re.search(r"\bcall_once\s*\(", body):
        return (
            f"C++ call_once unencoded: {engine} is not a call_once model"
        )
    if re.search(r"\b(?:std\s*::\s*)?tuple\s*<", body):
        return (
            f"C++ tuple unencoded: {engine} is not a tuple model"
        )
    if re.search(r"\b(?:std\s*::\s*)?deque\s*<", body):
        return (
            f"C++ deque unencoded: {engine} is not a deque model"
        )
    if re.search(r"\b(?:std\s*::\s*)?forward_list\s*<", body):
        return (
            f"C++ forward_list unencoded: {engine} is not a "
            "forward_list model"
        )
    if re.search(r"\bstd\s*::\s*list\s*<", body):
        return (
            f"C++ std::list unencoded: {engine} is not a list model"
        )
    if re.search(r"\b(?:std\s*::\s*)?unordered_map\s*<", body):
        return (
            f"C++ unordered_map unencoded: {engine} is not an "
            "unordered_map model"
        )
    if re.search(r"\bstd\s*::\s*map\s*<", body):
        return (
            f"C++ std::map unencoded: {engine} is not a map model"
        )
    if re.search(r"\bunordered_set\s*<", body):
        return (
            f"C++ unordered_set unencoded: {engine} is not an "
            "unordered_set model"
        )
    if re.search(r"\bstd\s*::\s*set\s*<", body):
        return (
            f"C++ std::set unencoded: {engine} is not a set model"
        )
    if re.search(r"\bpriority_queue\s*<", body):
        return (
            f"C++ priority_queue unencoded: {engine} is not a "
            "priority_queue model"
        )
    if re.search(r"\bstd\s*::\s*queue\s*<", body):
        return (
            f"C++ std::queue unencoded: {engine} is not a queue model"
        )
    if re.search(r"\bstd\s*::\s*stack\s*<", body):
        return (
            f"C++ std::stack unencoded: {engine} is not a stack model"
        )
    if re.search(r"\bto_array\s*[<(]", body):
        return (
            f"C++ to_array unencoded: {engine} is not a to_array model"
        )
    if re.search(r"\bfrom_range\b", body):
        return (
            f"C++ from_range unencoded: {engine} is not a from_range model"
        )
    if re.search(r"\branges\s*::\s*to\s*[<(]", body):
        return (
            f"C++ ranges::to unencoded: {engine} is not a ranges::to model"
        )
    if re.search(r"\bstd\s*::\s*array\s*<", body):
        return (
            f"C++ std::array unencoded: {engine} is not an array model"
        )
    if re.search(r"\b(?:std\s*::\s*)?wstring_convert\b", body):
        return (
            f"C++ wstring_convert unencoded: {engine} is not a "
            "wstring_convert model"
        )
    if re.search(r"\b(?:std\s*::\s*)?wstring(?:_view)?\b", body):
        return (
            f"C++ wstring unencoded: {engine} is not a wstring model"
        )
    if re.search(r"\b(?:std\s*::\s*)?multimap\s*<", body):
        return (
            f"C++ multimap unencoded: {engine} is not a multimap model"
        )
    if re.search(r"\b(?:std\s*::\s*)?multiset\s*<", body):
        return (
            f"C++ multiset unencoded: {engine} is not a multiset model"
        )
    if re.search(r"\bbinary_semaphore\b", body):
        return (
            f"C++ binary_semaphore unencoded: {engine} is not a "
            "binary_semaphore model"
        )
    if re.search(r"\berror_category\b", body):
        return (
            f"C++ error_category unencoded: {engine} is not an "
            "error_category model"
        )
    if re.search(r"\bsystem_error\b", body):
        return (
            f"C++ system_error unencoded: {engine} is not a "
            "system_error model"
        )
    if re.search(r"\b(?:std\s*::\s*)?error_code\b", body):
        return (
            f"C++ error_code unencoded: {engine} is not an "
            "error_code model"
        )
    if re.search(r"\bstd\s*::\s*apply\s*\(", body):
        return (
            f"C++ std::apply unencoded: {engine} is not an apply model"
        )
    if re.search(r"\bstd\s*::\s*invoke\s*\(", body):
        return (
            f"C++ std::invoke unencoded: {engine} is not an invoke model"
        )
    if re.search(r"\bstd\s*::\s*endian\b", body):
        return (
            f"C++ std::endian unencoded: {engine} is not an endian model"
        )
    if re.search(r"\bstd\s*::\s*rot[lr]\s*\(", body):
        return (
            f"C++ std::rotl unencoded: {engine} is not a rotl model"
        )
    if re.search(
        r"\b(?:bit_ceil|bit_floor|has_single_bit|std\s*::\s*popcount)\s*\(",
        body,
    ):
        return (
            f"C++ bit_ceil unencoded: {engine} is not a bit_ceil model"
        )
    if re.search(r"\bstd\s*::\s*bit_width\s*\(", body):
        return (
            f"C++ std::bit_width unencoded: {engine} is not a bit_width model"
        )
    if re.search(r"\bstd\s*::\s*gcd\s*\(", body):
        return (
            f"C++ std::gcd unencoded: {engine} is not a gcd model"
        )
    if re.search(r"\bstd\s*::\s*lcm\s*\(", body):
        return (
            f"C++ std::lcm unencoded: {engine} is not a lcm model"
        )
    if re.search(r"\bstd\s*::\s*clamp\s*\(", body):
        return (
            f"C++ std::clamp unencoded: {engine} is not a clamp model"
        )
    if re.search(r"\bstd\s*::\s*exchange\s*\(", body):
        return (
            f"C++ std::exchange unencoded: {engine} is not an exchange model"
        )
    if re.search(r"\bstd\s*::\s*to_address\s*\(", body):
        return (
            f"C++ std::to_address unencoded: {engine} is not a "
            "to_address model"
        )
    if re.search(r"\b(?:construct_at|destroy_at)\s*\(", body):
        return (
            f"C++ construct_at unencoded: {engine} is not a "
            "construct_at model"
        )
    if re.search(r"\bdestroy_n\s*\(", body):
        return (
            f"C++ destroy_n unencoded: {engine} is not a destroy_n model"
        )
    if re.search(r"\bstd\s*::\s*addressof\s*\(", body):
        return (
            f"C++ std::addressof unencoded: {engine} is not an "
            "addressof model"
        )
    if re.search(r"\bassume_aligned\s*\(", body):
        return (
            f"C++ assume_aligned unencoded: {engine} is not an "
            "assume_aligned model"
        )
    if re.search(r"\bas_rvalue(?:_view)?\b", body):
        return (
            f"C++ as_rvalue unencoded: {engine} is not an as_rvalue model"
        )
    if re.search(r"\bas_const\s*\(", body):
        return (
            f"C++ as_const unencoded: {engine} is not an as_const model"
        )
    if re.search(r"\btransform_(?:inclusive|exclusive)_scan\s*\(", body):
        return (
            f"C++ transform_inclusive_scan unencoded: {engine} is not a "
            "transform_inclusive_scan model"
        )
    if re.search(r"\bexclusive_scan\s*\(", body):
        return (
            f"C++ exclusive_scan unencoded: {engine} is not an "
            "exclusive_scan model"
        )
    if re.search(r"\binclusive_scan\s*\(", body):
        return (
            f"C++ inclusive_scan unencoded: {engine} is not an "
            "inclusive_scan model"
        )
    if re.search(r"\btransform_reduce\s*\(", body):
        return (
            f"C++ transform_reduce unencoded: {engine} is not a "
            "transform_reduce model"
        )
    if re.search(r"\bstd\s*::\s*reduce\s*\(", body):
        return (
            f"C++ std::reduce unencoded: {engine} is not a reduce model"
        )
    if re.search(
        r"\buninitialized_(?:fill(?:_n)?|default_construct(?:_n)?)\s*\(",
        body,
    ):
        return (
            f"C++ uninitialized_fill unencoded: {engine} is not an "
            "uninitialized_fill model"
        )
    if re.search(r"\buninitialized_value_construct(?:_n)?\s*\(", body):
        return (
            f"C++ uninitialized_value_construct unencoded: {engine} is not an "
            "uninitialized_value_construct model"
        )
    if re.search(r"\buninitialized_(?:copy|move)(?:_n)?\s*\(", body):
        return (
            f"C++ uninitialized_copy unencoded: {engine} is not an "
            "uninitialized_copy model"
        )
    if re.search(
        r"\b(?:add_sat|sub_sat|mul_sat|div_sat|saturate_cast)\s*\(",
        body,
    ):
        return (
            f"C++ add_sat unencoded: {engine} is not an add_sat model"
        )
    if re.search(r"\bnontype\b", body):
        return (
            f"C++ nontype unencoded: {engine} is not a nontype model"
        )
    if re.search(r"\bis_layout_compatible\b", body):
        return (
            f"C++ is_layout_compatible unencoded: {engine} is not an "
            "is_layout_compatible model"
        )
    if re.search(r"\bis_pointer_interconvertible_(?:with_class|base_of)\b", body):
        return (
            f"C++ is_pointer_interconvertible unencoded: {engine} is not an "
            "is_pointer_interconvertible model"
        )
    if re.search(r"\bbasic_const_iterator\b", body):
        return (
            f"C++ basic_const_iterator unencoded: {engine} is not a "
            "basic_const_iterator model"
        )
    if re.search(r"\bis_corresponding_member\b", body):
        return (
            f"C++ is_corresponding_member unencoded: {engine} is not an "
            "is_corresponding_member model"
        )
    if re.search(r"\bforward_like\b", body):
        return (
            f"C++ forward_like unencoded: {engine} is not a "
            "forward_like model"
        )
    if re.search(r"\b(?:set|get)_terminate\s*\(", body):
        return (
            f"C++ set_terminate unencoded: {engine} is not a "
            "set_terminate model"
        )
    if re.search(r"\bis_constant_evaluated\s*\(", body):
        return (
            f"C++ is_constant_evaluated unencoded: {engine} is not an "
            "is_constant_evaluated model"
        )
    if re.search(r"\bstd\s*::\s*lerp\s*\(", body):
        return (
            f"C++ std::lerp unencoded: {engine} is not a lerp model"
        )
    if re.search(r"\bstd\s*::\s*midpoint\s*\(", body):
        return (
            f"C++ std::midpoint unencoded: {engine} is not a midpoint model"
        )
    if re.search(
        r"\bstd\s*::\s*(?:cmp_(?:less|greater|less_equal|"
        r"greater_equal|equal_to|not_equal_to)|in_range)\s*\(",
        body,
    ):
        return (
            f"C++ std::cmp_less unencoded: {engine} is not a cmp_less model"
        )
    if re.search(r"\bstd\s*::\s*count[lr]_(?:zero|one)\s*\(", body):
        return (
            f"C++ std::countl_zero unencoded: {engine} is not a "
            "countl_zero model"
        )
    if re.search(r"\b(?:std\s*::\s*)?byteswap\s*\(", body):
        return (
            f"C++ byteswap unencoded: {engine} is not a byteswap model"
        )
    if re.search(r"\b(?:wcscpy|wcscat|wcsncpy|wcsncat)\s*\(", body):
        return (
            f"wide-string copy unencoded: unconstrained wcscpy is not a "
            f"proof of the buffer ({engine})"
        )
    if re.search(
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
    if re.search(r"(?:,|\()\s*&", body):
        return (
            f"address-of unencoded: {engine} is not a pointer-value model"
        )
    if re.search(
        r"\b(?:char|unsigned\s+char)\s+[A-Za-z_]\w*\s*\[\s*\]\s*=",
        body,
    ):
        return (
            f"array string-init unencoded: {engine} is not a string-array model"
        )
    if re.search(r"\b(?:asm|__asm__|__asm)\b", body):
        return f"inline asm unencoded: {engine} is not an assembly model"
    if re.search(r"\b_Generic\b", body):
        return f"_Generic unencoded: {engine} is not a type-generic model"
    if re.search(r"\(\s*\{", body):
        return f"GNU statement expression unencoded: {engine} is not a GNU-C model"
    if re.search(r"\b(?:try|catch)\b", body):
        return f"C++ try/catch unencoded: {engine} is not an exception model"
    if re.search(r"\boffsetof\b", body):
        return f"offsetof unencoded: {engine} is not a struct-layout model"
    if re.search(r"\b(?:new|delete)\b", body):
        return f"C++ new/delete unencoded: {engine} is not a heap-lifetime model"
    if re.search(r"\brestrict\b", body):
        return (
            f"restrict unencoded: {engine} is not a restrict-qualifier model"
        )
    # Decl qualifier only — not `__asm__ volatile` and not the word in a string.
    if re.search(
        r"(?m)(?:^|[;{(])\s*(?:(?:static|extern|auto|register|const)\s+)*"
        r"(?:_Atomic|volatile)\b"
        r"|\b_Atomic\s*\("
        r"|\b(?:int|unsigned|signed|long|short|char|uint\w*|int\w*|"
        r"_Bool|bool|void|float|double)\s+(?:const\s+)?volatile\b",
        body,
    ):
        return f"volatile/_Atomic unencoded: {engine} is not a memory-model"
    if re.search(r"\b(?:pthread_mutex_t|mtx_t)\b", body):
        return f"mutex object unencoded: {engine} is not a lock model"
    # Local `const T x` — missing const model. Parameter `const int` is
    # stripped in cparse and is not this case. Casts `(const int)` start
    # with `(` so they are not this match. `for (const int i = 0;` is.
    if re.search(
        r"(?m)(?:^|[;{])\s*(?:(?:static|extern|auto|register)\s+)*const\s+"
        r"|\b(?:int|unsigned|signed|long|short|char|uint\w*|int\w*|"
        r"_Bool|bool|void|float|double|size_t)\s+const\s+[A-Za-z_]"
        r"|\bfor\s*\(\s*(?:(?:static|extern|auto|register)\s+)*const\s+",
        body,
    ):
        return f"const local unencoded: {engine} is not a const model"
    # `register int x` / `auto int x` / C++ `auto x = 1` — missing model.
    if re.search(r"(?m)(?:^|[;{(])\s*(?:register|auto)\b", body):
        return (
            f"register/auto unencoded: {engine} is not a storage-class "
            "or auto-type model"
        )
    if re.search(r"\b__auto_type\b", body):
        return (
            f"__auto_type unencoded: {engine} is not a storage-class "
            "or auto-type model"
        )
    # Function-local `static int x` / `extern int x` — missing duration.
    if re.search(
        r"(?m)(?:^|[;{])\s*(?:static|extern)\s+" + _DECL_TYPE + r"\s+[A-Za-z_]\w*",
        body,
    ):
        return (
            f"static/extern local unencoded: {engine} is not a "
            "storage-duration model"
        )
    # Anonymous `enum { RED = 1 } e;` — missing layout, not a proof.
    if re.search(r"enum\s*\{[^}]+\}\s+[A-Za-z_]\w*", body):
        return (
            f"anonymous enum local unencoded: {engine} is not a layout model"
        )
    if re.search(r"(?m)(?:^|[;{])\s*(?:_Alignas|alignas)\s*\(", body):
        return f"_Alignas unencoded: {engine} is not an alignment model"
    if re.search(
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
    if re.search(
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
    if re.match(r"(?:struct|union)\s*(?:[A-Za-z_]\w*\s*)?\{", s):
        return "struct unencoded"
    if re.match(r"enum\s*\{", s):
        return "anon enum unencoded"
    if re.match(r"(?:static|extern)\b", s):
        return "storage-duration unencoded"
    if re.match(r"(?:_Alignas|alignas)\s*\(", s):
        return "alignas unencoded"
    if re.match(r"__auto_type\b", s):
        return "storage-class unencoded"
    if re.match(r"(?:_Thread_local|thread_local)\b", s):
        return "thread-local unencoded"
    if re.match(r"(?:_Complex|_Imaginary)\b", s):
        return "complex unencoded"
    if re.match(r"(?:_Decimal32|_Decimal64|_Decimal128)\b", s):
        return "decimal-float unencoded"
    if re.match(r"(?:_Float16|_Float32|_Float64|__fp16)\b", s):
        return "extra-IEEE unencoded"
    if re.match(r"(?:typeof_unqual|__typeof_unqual__)\s*\(", s):
        return "typeof_unqual unencoded"
    if re.match(r"(?:typeof|__typeof__)\s*\(", s):
        return "typeof unencoded"
    if re.match(r"constexpr\b", s):
        return "constexpr unencoded"
    if re.match(r"\[\[\s*assume\s*\(", s):
        return "assume unencoded"
    if re.match(
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
    if re.search(r"\b(?:__int128(?:_t)?|_BitInt)\b", s):
        return "128-bit unencoded"
    if re.search(r"\b(?:_Decimal32|_Decimal64|_Decimal128)\b", s):
        return "decimal-float unencoded"
    if re.search(r"\b(?:_Float16|_Float32|_Float64|__fp16)\b", s):
        return "extra-IEEE unencoded"
    if _starts_kw(s, "constexpr"):
        return "constexpr unencoded"
    if re.search(r"\[\[\s*assume\s*\(", s):
        return "assume unencoded"
    if re.search(r"__attribute__\s*\(\s*\(\s*cleanup", s):
        return "cleanup unencoded"
    if re.search(
        r"__attribute__\s*\(\s*\(\s*(?:__)?vector_size|\b__vector_size\b",
        s,
    ):
        return "vector_size unencoded"
    if _starts_kw(s, "const"):
        return "const unencoded"
    if _starts_kw(s, "register") or _starts_kw(s, "auto"):
        return "storage-class unencoded"
    if re.match(r"__auto_type\b", s):
        return "storage-class unencoded"
    if _starts_kw(s, "static") or _starts_kw(s, "extern"):
        return "storage-duration unencoded"
    if re.match(r"(?:_Thread_local|thread_local)\b", s):
        return "thread-local unencoded"
    if re.match(r"(?:_Complex|_Imaginary)\b", s):
        return "complex unencoded"
    if re.match(r"(?:typeof_unqual|__typeof_unqual__)\s*\(", s):
        return "typeof_unqual unencoded"
    if re.match(r"(?:typeof|__typeof__)\s*\(", s):
        return "typeof unencoded"
    if _is_nested_function(s):
        return "nested function unencoded"
    if re.match(r"(?:_Alignas|alignas)\s*\(", s):
        return "alignas unencoded"
    if re.search(r"=\s*\([^)]*\)\s*\{", s):
        return "compound-lit unencoded"
    if _starts_kw(s, "struct") or _starts_kw(s, "union"):
        if re.match(r"(?:struct|union)\s*\{", s):
            return "struct unencoded"
        if re.match(r"(?:struct|union)\s+[A-Za-z_]\w*\s*\{", s):
            return "struct unencoded"
        if re.match(r"(?:struct|union)\s+[A-Za-z_]\w*\s+[A-Za-z_]\w*", s):
            return "struct unencoded"
        return None
    if _starts_kw(s, "enum"):
        if re.match(r"enum\s*\{", s):
            return "anon enum unencoded"
        if re.match(r"enum\s+[A-Za-z_]\w*\s+[A-Za-z_]\w*", s):
            return "struct unencoded"
        return None
    m = re.match(
        r"([A-Za-z_]\w*)\s+[A-Za-z_]\w*\s*(?:[=;\[]|$)",
        s,
    )
    if not m:
        return None
    if m.group(1) in _STMT_START_WORDS:
        return None
    return "typedef local unencoded"


def harness_for_parsefail(err: str, engine: str) -> str | None:
    """Map a frontend ParseFail to NEEDS-HARNESS. goto stays ERROR."""
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
    if re.search(r"\bexecveat\b", low):
        return (
            f"execveat unencoded: unconstrained execveat is not a "
            f"proof ({engine})"
        )
    if re.search(r"\bpthread_atfork\b", low):
        return (
            f"pthread_atfork unencoded: unconstrained pthread_atfork "
            f"is not a proof ({engine})"
        )
    if re.search(r"\bpledge\b", low):
        return (
            f"pledge unencoded: unconstrained pledge is not a "
            f"proof ({engine})"
        )
    if re.search(r"\bmac_(?:set|get)_(?:proc|fd|file)\b", low):
        return (
            f"mac_set_proc unencoded: unconstrained mac_set_proc is not a "
            f"proof ({engine})"
        )
    if re.search(r"\bcap_getmode\b", low):
        return (
            f"cap_getmode unencoded: unconstrained cap_getmode is not a "
            f"proof ({engine})"
        )
    if re.search(r"\bcap_getrights\b", low):
        return (
            f"cap_getrights unencoded: unconstrained cap_getrights is not a "
            f"proof ({engine})"
        )
    if re.search(r"\bcap_enter\b", low):
        return (
            f"cap_enter unencoded: unconstrained cap_enter is not a "
            f"proof ({engine})"
        )
    if re.search(r"\bcap_sandboxed\b", low):
        return (
            f"cap_sandboxed unencoded: unconstrained cap_sandboxed is not a "
            f"proof ({engine})"
        )
    if re.search(r"\bcap_rights_(?:limit|get)\b", low):
        return (
            f"cap_rights unencoded: unconstrained cap_rights_limit "
            f"is not a proof ({engine})"
        )
    if re.search(r"\bcap_(?:fcntls|ioctls)_limit\b", low):
        return (
            f"cap_fcntls unencoded: unconstrained cap_fcntls_limit "
            f"is not a proof ({engine})"
        )
    if re.search(r"\bunveil\b", low):
        return (
            f"unveil unencoded: unconstrained unveil is not a "
            f"proof ({engine})"
        )
    if re.search(r"\bsysctl(?:byname)?\b", low):
        return (
            f"sysctl unencoded: unconstrained sysctl is not a "
            f"proof ({engine})"
        )
    if re.search(r"\bkqueue\b", low):
        return (
            f"kqueue unencoded: unconstrained kqueue is not a "
            f"proof ({engine})"
        )
    if re.search(r"\bkevent\b", low):
        return (
            f"kevent unencoded: unconstrained kevent is not a "
            f"proof ({engine})"
        )
    if re.search(r"\bpause\b", low):
        return (
            f"pause unencoded: unconstrained pause is not a "
            f"proof ({engine})"
        )
    if re.search(r"\b(?:get|set|swap|make)context\b", low):
        return (
            f"getcontext unencoded: unconstrained getcontext is not a "
            f"proof ({engine})"
        )
    if re.search(
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
    if re.search(r"\bpdfork\b", low):
        return (
            f"pdfork unencoded: unconstrained pdfork is not a "
            f"proof ({engine})"
        )
    if re.search(r"\brfork\b", low):
        return (
            f"rfork unencoded: unconstrained rfork is not a "
            f"proof ({engine})"
        )
    if re.search(r"\bminherit\b", low):
        return (
            f"minherit unencoded: unconstrained minherit is not a "
            f"proof ({engine})"
        )
    if re.search(r"\bnfssvc\b", low):
        return (
            f"nfssvc unencoded: unconstrained nfssvc is not a "
            f"proof ({engine})"
        )
    if re.search(r"\bsysarch\b", low):
        return (
            f"sysarch unencoded: unconstrained sysarch is not a "
            f"proof ({engine})"
        )
    if re.search(r"\bgetbootfile\b", low):
        return (
            f"getbootfile unencoded: unconstrained getbootfile is not a "
            f"proof ({engine})"
        )
    if re.search(r"\bdevname(?:_r)?\b", low):
        return (
            f"devname unencoded: unconstrained devname is not a "
            f"proof ({engine})"
        )
    if re.search(r"\bsbrk\b", low):
        return (
            f"sbrk unencoded: unconstrained sbrk is not a "
            f"proof ({engine})"
        )
    if re.search(r"\bbrk\s*\(", low) or re.search(r"\bbrk\b", low):
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
    if re.search(r"\b(?:dlfunc|dlvsym)\b", low) or "dlfunc unencoded" in low:
        return (
            f"dlfunc unencoded: unconstrained dlfunc is not a "
            f"proof ({engine})"
        )
    if "dlopen unencoded" in low or "dlsym unencoded" in low or "dlclose unencoded" in low:
        return f"dlopen unencoded: {engine} is not a dynamic-loader model"
    if re.search(r"\bmod(?:find|stat|next|fnext)\b", low):
        return (
            f"modfind unencoded: unconstrained modfind is not a "
            f"proof ({engine})"
        )
    if re.search(r"\bkld(?:firstmod|nextmod)\b", low):
        return (
            f"kldfirstmod unencoded: unconstrained kldfirstmod is not a "
            f"proof ({engine})"
        )
    if re.search(r"\bkld_(?:isloaded|load)\b", low):
        return (
            f"kld_load unencoded: unconstrained kld_load is not a "
            f"proof ({engine})"
        )
    if re.search(r"\bkld(?:load|unload|find|sym|stat)\b", low):
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
    if re.search(r"\b__builtin_unreachable\b", low):
        return (
            f"__builtin_unreachable unencoded: unconstrained "
            f"__builtin_unreachable is not a proof ({engine})"
        )
    if re.search(r"\b__builtin_trap\b", low):
        return (
            f"__builtin_trap unencoded: unconstrained "
            f"__builtin_trap is not a proof ({engine})"
        )
    if re.search(r"\bstd\s*::\s*unreachable\b", low):
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
    if re.search(r"\bpd(?:getpid|wait4)\b", low):
        return (
            f"pdgetpid unencoded: unconstrained pdgetpid is not a "
            f"proof ({engine})"
        )
    if re.search(r"\b(?:wait4|wait3)\b", low):
        return (
            f"wait4 unencoded: unconstrained wait4 is not a "
            f"proof ({engine})"
        )
    if re.search(r"\bwait6\b", low):
        return (
            f"wait6 unencoded: unconstrained wait6 is not a "
            f"proof ({engine})"
        )
    if re.search(r"\bsem_trywait\b", low) or re.search(r"\bsem_getvalue\b", low):
        return (
            f"sem_trywait unencoded: unconstrained sem_trywait is not a "
            f"proof ({engine})"
        )
    if "wait unencoded" in low:
        return (
            f"wait unencoded: unconstrained wait is not a "
            f"proof ({engine})"
        )
    if re.search(r"\bunlinkat\b", low):
        return (
            f"unlinkat unencoded: unconstrained unlinkat is not a "
            f"proof ({engine})"
        )
    if re.search(r"\bunlink\b", low):
        return (
            f"unlink unencoded: unconstrained unlink is not a "
            f"proof ({engine})"
        )
    if re.search(r"\bmknodat\b", low):
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
    if re.search(r"\breference_wrapper\b", low) or re.search(
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
    if re.search(r"\bppoll\b", low):
        return (
            f"ppoll unencoded: unconstrained ppoll is not a "
            f"proof ({engine})"
        )
    if re.search(r"\bepoll_pwait(?:2)?\b", low):
        return (
            f"epoll_pwait unencoded: unconstrained epoll_pwait is not a "
            f"proof ({engine})"
        )
    if re.search(r"\bepoll_create(?:1)?\b", low):
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
    if re.search(r"\brt_(?:tgsigqueueinfo|sigqueueinfo)\b", low):
        return (
            f"rt_sigqueueinfo unencoded: unconstrained rt_sigqueueinfo "
            f"is not a proof ({engine})"
        )
    if re.search(r"\bsigqueue\b", low):
        return (
            f"sigqueue unencoded: unconstrained sigqueue is not a "
            f"proof ({engine})"
        )
    if re.search(r"\bthr_(?:new|kill2|kill|self|exit|suspend|wake)\b", low):
        return (
            f"thr_kill unencoded: unconstrained thr_kill is not a "
            f"proof ({engine})"
        )
    if re.search(r"\bpthread_kill\b", low):
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
    if re.search(r"\bpthread_cancel\b", low):
        return (
            f"pthread_cancel unencoded: unconstrained pthread_cancel "
            f"is not a proof ({engine})"
        )
    if re.search(
        r"\bpthread_(?:key_create|key_delete|setspecific|getspecific)\b",
        low,
    ) or "pthread_key_create unencoded" in low:
        return (
            f"pthread_key_create unencoded: unconstrained "
            f"pthread_key_create is not a proof ({engine})"
        )
    if "pthread join unencoded" in low or re.search(
        r"\b(?:pthread_join|pthread_detach)\b", low
    ):
        return (
            f"pthread join unencoded: unconstrained thread join is not a "
            f"proof ({engine})"
        )
    if re.search(
        r"\bpthread_spin_(?:try)?(?:lock|unlock|init|destroy)\b",
        low,
    ) or "pthread_spin unencoded" in low:
        return (
            f"pthread_spin unencoded: unconstrained pthread_spin_lock "
            f"is not a proof ({engine})"
        )
    if re.search(
        r"\bpthread_rwlock_(?:try|timed)?(?:rdlock|wrlock|unlock|init|destroy)\b",
        low,
    ) or "pthread_rwlock unencoded" in low:
        return (
            f"pthread_rwlock unencoded: unconstrained pthread_rwlock "
            f"is not a proof ({engine})"
        )
    if re.search(
        r"\bpthread_cond_(?:timedwait|wait|signal|broadcast|init|destroy)\b",
        low,
    ) or "pthread_cond unencoded" in low:
        return (
            f"pthread_cond unencoded: unconstrained pthread_cond_wait "
            f"is not a proof ({engine})"
        )
    if "sem unencoded" in low:
        return f"sem unencoded: {engine} is not a semaphore model"
    if re.search(r"\bksem_", low):
        return (
            f"ksem_open unencoded: unconstrained ksem_open is not a "
            f"proof ({engine})"
        )
    if re.search(r"\bsem_(?:open|close|unlink)\b", low):
        return (
            f"sem_open unencoded: unconstrained sem_open is not a "
            f"proof ({engine})"
        )
    if re.search(r"\bsem_timedwait\b", low):
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
    if re.search(r"\bvalloc\b", low):
        return (
            f"valloc unencoded: unconstrained valloc is not a "
            f"proof ({engine})"
        )
    if re.search(r"\bnotify_all_at_thread_exit\b", low):
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
    if re.search(r"\bassume_aligned\b", low) or "assume_aligned unencoded" in low:
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
    if re.search(r"\bsymlinkat\b", low):
        return (
            f"symlinkat unencoded: unconstrained symlinkat is not a "
            f"proof ({engine})"
        )
    if re.search(r"\breadlinkat\b", low):
        return (
            f"readlinkat unencoded: unconstrained readlinkat is not a "
            f"proof ({engine})"
        )
    if re.search(r"\b(?:symlink|readlink)\b", low) or (
        "symlink unencoded" in low or "readlink unencoded" in low
    ):
        return (
            f"symlink unencoded: unconstrained symlink/readlink is not a "
            f"proof ({engine})"
        )
    if re.search(r"\bfts_(?:open|read|children|close|set)\b", low):
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
    if re.search(r"\b(?:getsockname|getpeername)\b", low):
        return (
            f"getsockname unencoded: unconstrained getsockname is not a "
            f"proof ({engine})"
        )
    if re.search(r"\bgetpeereid\b", low):
        return (
            f"getpeereid unencoded: unconstrained getpeereid is not a "
            f"proof ({engine})"
        )
    if "getsockopt unencoded" in low or "setsockopt unencoded" in low:
        return (
            f"getsockopt unencoded: unconstrained socket opts is not a "
            f"proof ({engine})"
        )
    if re.search(r"\b(?:listmount|statmount)\b", low):
        return (
            f"listmount unencoded: unconstrained listmount/statmount "
            f"is not a proof ({engine})"
        )
    if "ustat" in low:
        return (
            f"ustat unencoded: unconstrained ustat is not a "
            f"proof ({engine})"
        )
    if re.search(r"\bfile_(?:get|set)attr\b", low):
        return (
            f"file_getattr unencoded: unconstrained file_getattr "
            f"is not a proof ({engine})"
        )
    if re.search(r"\bfh(?:linkat|link|readlink)\b", low) or re.search(
        r"\bfhlink", low
    ):
        return (
            f"fhlink unencoded: unconstrained fhlink is not a "
            f"proof ({engine})"
        )
    if re.search(r"\b(?:getfh|fhopen|fhstatfs|fhstat|getfhat)\b", low):
        return (
            f"getfh unencoded: unconstrained getfh is not a "
            f"proof ({engine})"
        )
    if re.search(r"\bfstatat\b", low):
        return (
            f"fstatat unencoded: unconstrained fstatat is not a "
            f"proof ({engine})"
        )
    if re.search(r"\bgetmntinfo\b", low):
        return (
            f"getmntinfo unencoded: unconstrained getmntinfo is not a "
            f"proof ({engine})"
        )
    if re.search(r"\bgetvfsbyname\b", low):
        return (
            f"getvfsbyname unencoded: unconstrained getvfsbyname is not a "
            f"proof ({engine})"
        )
    if re.search(r"\b(?:get|set|end)fsent\b", low):
        return (
            f"getfsent unencoded: unconstrained getfsent is not a "
            f"proof ({engine})"
        )
    if re.search(r"\bgetfsstat\b", low):
        return (
            f"getfsstat unencoded: unconstrained getfsstat is not a "
            f"proof ({engine})"
        )
    if (
        re.search(r"\b(?:lstat|fstat|stat)\b", low)
        or "stat unencoded" in low
        or "lstat unencoded" in low
        or "fstat unencoded" in low
    ):
        return (
            f"stat unencoded: unconstrained stat is not a "
            f"proof ({engine})"
        )
    if re.search(r"\brenameat2\b", low):
        return (
            f"renameat2 unencoded: unconstrained renameat2 is not a "
            f"proof ({engine})"
        )
    if re.search(r"\brenameat\b", low):
        return (
            f"renameat unencoded: unconstrained renameat is not a "
            f"proof ({engine})"
        )
    if re.search(r"\bmkdirat\b", low):
        return (
            f"mkdirat unencoded: unconstrained mkdirat is not a "
            f"proof ({engine})"
        )
    if (
        re.search(r"\b(?:mkdir|rmdir|rename)\b", low)
        or "mkdir unencoded" in low
        or "rmdir unencoded" in low
        or "rename unencoded" in low
    ):
        return (
            f"mkdir unencoded: unconstrained mkdir is not a "
            f"proof ({engine})"
        )
    if re.search(r"\bcrypt_(?:newhash|checkpass)\b", low):
        return (
            f"crypt_newhash unencoded: unconstrained crypt_newhash is not a "
            f"proof ({engine})"
        )
    if re.search(r"\bkenv\b", low):
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
    if re.search(r"\bpthread_barrier_(?:wait|init|destroy)\b", low) or (
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
    if re.search(r"\bpthread_getcpuclockid\b", low):
        return (
            f"pthread_getcpuclockid unencoded: unconstrained "
            f"pthread_getcpuclockid is not a proof ({engine})"
        )
    if re.search(r"\bclock_getcpuclockid\b", low):
        return (
            f"clock_getcpuclockid unencoded: unconstrained "
            f"clock_getcpuclockid is not a proof ({engine})"
        )
    if "clock_gettime" in low or "gettimeofday" in low:
        return (
            f"clock_gettime unencoded: unconstrained clock_gettime is not a "
            f"proof ({engine})"
        )
    if re.search(r"\bposix_typed_mem_(?:open|get_info)\b", low):
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
    if re.search(r"\bposix_spawn(?:_file_actions|attr)_init\b", low):
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
    if re.search(r"\bfchmodat2\b", low):
        return (
            f"fchmodat2 unencoded: unconstrained fchmodat2 is not a "
            f"proof ({engine})"
        )
    if re.search(r"\bfchmodat\b", low):
        return (
            f"fchmodat unencoded: unconstrained fchmodat is not a "
            f"proof ({engine})"
        )
    if re.search(r"\b(?:f|l)?chflags\b", low):
        return (
            f"chflags unencoded: unconstrained chflags is not a "
            f"proof ({engine})"
        )
    if re.search(r"\bfaccessat2\b", low):
        return (
            f"faccessat2 unencoded: unconstrained faccessat2 is not a "
            f"proof ({engine})"
        )
    if re.search(r"\bfaccessat\b", low):
        return (
            f"faccessat unencoded: unconstrained faccessat is not a "
            f"proof ({engine})"
        )
    if "vhangup" in low:
        return (
            f"vhangup unencoded: unconstrained vhangup is not a "
            f"proof ({engine})"
        )
    if re.search(r"\b(?:at_)?quick_exit\b", low):
        return (
            f"quick_exit unencoded: unconstrained quick_exit is not a "
            f"proof ({engine})"
        )
    if re.search(r"\beaccess\b", low):
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
    if "current_zone" in low or re.search(r"\btzdb\b", low):
        return (
            f"C++ tzdb unencoded: {engine} is not a tzdb model"
        )
    if "chrono" in low:
        return (
            f"C++ chrono unencoded: {engine} is not a chrono model"
        )
    if re.search(r"\bzip_transform(?:_view)?\b", low) or (
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
    if re.search(r"\bis_scoped_enum\b", low) or "is_scoped_enum unencoded" in low:
        return (
            f"C++ is_scoped_enum unencoded: {engine} is not an "
            "is_scoped_enum model"
        )
    if re.search(r"\b(?:views\s*::\s*)?enumerate(?:_view)?\b", low) or (
        "enumerate unencoded" in low
    ):
        return (
            f"C++ views::enumerate unencoded: {engine} is not a "
            "views::enumerate model"
        )
    if re.search(r"\bcartesian_product(?:_view)?\b", low) or (
        "cartesian_product unencoded" in low
    ):
        return (
            f"C++ cartesian_product unencoded: {engine} is not a "
            "cartesian_product model"
        )
    if re.search(
        r"\b(?:views\s*::\s*chunk(?:_by)?|chunk(?:_by|_view))\b",
        low,
    ) or "chunk unencoded" in low:
        return (
            f"C++ views::chunk unencoded: {engine} is not a "
            "views::chunk model"
        )
    if re.search(r"\b(?:views\s*::\s*slide|slide_view)\b", low) or (
        "slide unencoded" in low
    ):
        return (
            f"C++ views::slide unencoded: {engine} is not a "
            "views::slide model"
        )
    if re.search(
        r"\b(?:views\s*::\s*adjacent(?:_transform)?|"
        r"adjacent(?:_transform|_view))\b",
        low,
    ) or "adjacent unencoded" in low:
        return (
            f"C++ views::adjacent unencoded: {engine} is not a "
            "views::adjacent model"
        )
    if re.search(r"\bjoin_with(?:_view)?\b", low) or "join_with unencoded" in low:
        return (
            f"C++ join_with unencoded: {engine} is not a join_with model"
        )
    if "views::join" in low or "join_view" in low:
        return (
            f"C++ views::join unencoded: {engine} is not a views::join model"
        )
    if re.search(r"\b(?:views\s*::\s*)?stride(?:_view)?\b", low) or (
        "stride unencoded" in low
    ):
        return (
            f"C++ views::stride unencoded: {engine} is not a "
            "views::stride model"
        )
    if re.search(r"\b(?:views\s*::\s*repeat|repeat_view)\b", low) or (
        "repeat unencoded" in low
    ):
        return (
            f"C++ views::repeat unencoded: {engine} is not a "
            "views::repeat model"
        )
    if re.search(r"\btake_while(?:_view)?\b", low) or (
        "take_while unencoded" in low
    ):
        return (
            f"C++ views::take_while unencoded: {engine} is not a "
            "take_while model"
        )
    if re.search(r"\b(?:views\s*::\s*take\b|\btake_view\b)", low) or (
        "take unencoded" in low
    ):
        return (
            f"C++ views::take unencoded: {engine} is not a "
            "views::take model"
        )
    if re.search(r"\bdrop_while(?:_view)?\b", low) or (
        "drop_while unencoded" in low
    ):
        return (
            f"C++ views::drop_while unencoded: {engine} is not a "
            "drop_while model"
        )
    if re.search(r"\b(?:views\s*::\s*drop\b|\bdrop_view\b)", low) or (
        "drop unencoded" in low
    ):
        return (
            f"C++ views::drop unencoded: {engine} is not a "
            "views::drop model"
        )
    if re.search(r"\b(?:views\s*::\s*keys\b|\bkeys_view\b)", low) or (
        "keys unencoded" in low
    ):
        return (
            f"C++ views::keys unencoded: {engine} is not a "
            "keys model"
        )
    if re.search(r"\b(?:views\s*::\s*values\b|\bvalues_view\b)", low) or (
        "values unencoded" in low
    ):
        return (
            f"C++ views::values unencoded: {engine} is not a "
            "values model"
        )
    if re.search(r"\b(?:views\s*::\s*reverse\b|\breverse_view\b)", low) or (
        "reverse_view unencoded" in low or "views::reverse unencoded" in low
    ):
        return (
            f"C++ views::reverse unencoded: {engine} is not a "
            "reverse_view model"
        )
    if re.search(r"\b(?:views\s*::\s*counted\b|\bcounted_view\b)", low) or (
        "counted_view unencoded" in low or "views::counted unencoded" in low
    ):
        return (
            f"C++ views::counted unencoded: {engine} is not a "
            "counted_view model"
        )
    if re.search(r"\b(?:views\s*::\s*filter\b|\bfilter_view\b)", low) or (
        "filter unencoded" in low
    ):
        return (
            f"C++ views::filter unencoded: {engine} is not a "
            "views::filter model"
        )
    if re.search(r"\b(?:views\s*::\s*transform\b|\btransform_view\b)", low) or (
        "transform_view unencoded" in low
        or "views::transform unencoded" in low
    ):
        return (
            f"C++ views::transform unencoded: {engine} is not a "
            "transform_view model"
        )
    if re.search(r"\b(?:views\s*::\s*elements\b|\belements_view\b)", low) or (
        "elements unencoded" in low
    ):
        return (
            f"C++ views::elements unencoded: {engine} is not an "
            "elements model"
        )
    if re.search(r"\b(?:views\s*::\s*iota\b|\biota_view\b)", low) or (
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
    if re.search(r"\bgetosreldate\b", low):
        return (
            f"getosreldate unencoded: unconstrained getosreldate is not a "
            f"proof ({engine})"
        )
    if re.search(r"\bgetdomainname\b", low):
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
    if re.search(r"\b(?:preadv2|pwritev2|preadv|pwritev)\b", low):
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
    if re.search(r"\b(?:timerfd_settime|timerfd_gettime)\b", low):
        return (
            f"timerfd_settime unencoded: unconstrained "
            f"timerfd_settime is not a proof ({engine})"
        )
    if re.search(r"\beventfd_(?:read|write)\b", low):
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
    if re.search(r"\bprocctl\b", low):
        return (
            f"procctl unencoded: unconstrained procctl is not a "
            f"proof ({engine})"
        )
    if re.search(r"\bktrace\b", low):
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
    if re.search(r"\bgetpagesizes\b", low):
        return (
            f"getpagesizes unencoded: unconstrained getpagesizes is not a "
            f"proof ({engine})"
        )
    if re.search(r"\bgetpagesize(?!s)\b", low):
        return (
            f"getpagesize unencoded: unconstrained getpagesize is not a "
            f"proof ({engine})"
        )
    if re.search(r"\blpathconf\b", low):
        return (
            f"lpathconf unencoded: unconstrained lpathconf is not a "
            f"proof ({engine})"
        )
    if re.search(r"\b(?:sysconf|fpathconf|pathconf)\b", low):
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
    if re.search(r"\b(?:get|set)loginclass\b", low):
        return (
            f"loginclass unencoded: unconstrained getloginclass is not a "
            f"proof ({engine})"
        )
    if re.search(r"\b(?:login_getclass|setusercontext)\b", low):
        return (
            f"login_getclass unencoded: unconstrained login_getclass is not a "
            f"proof ({engine})"
        )
    if re.search(r"\bsetlogin\b", low):
        return (
            f"setlogin unencoded: unconstrained setlogin is not a "
            f"proof ({engine})"
        )
    if re.search(r"\bgetlogin(?:_r)?\b", low) or re.search(r"\bttyname(?:_r)?\b", low):
        return (
            f"getlogin unencoded: unconstrained getlogin is not a "
            f"proof ({engine})"
        )
    if "inet_pton" in low or "inet_ntop" in low or "inet_aton" in low:
        return (
            f"inet_pton unencoded: unconstrained inet_pton is not a "
            f"proof ({engine})"
        )
    if re.search(r"\bmseal\b", low):
        return (
            f"mseal unencoded: unconstrained mseal is not a "
            f"proof ({engine})"
        )
    if re.search(r"\bmlock2\b", low):
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
    if re.search(r"\bposix_fadvise(?:64)?\b", low):
        return (
            f"posix_fadvise unencoded: unconstrained posix_fadvise is not a "
            f"proof ({engine})"
        )
    if re.search(r"\breadahead\b", low):
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
    if re.search(r"\binotify_rm_watch\b", low):
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
    if re.search(r"\barc4random(?:_buf|_uniform)?\b", low):
        return (
            f"arc4random unencoded: unconstrained arc4random is not a "
            f"proof ({engine})"
        )
    if re.search(r"\bissetugid\b", low):
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
    if re.search(r"\btimingsafe_(?:bcmp|memcmp)\b", low):
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
    if re.search(r"\bnmount\b", low):
        return (
            f"nmount unencoded: unconstrained nmount is not a "
            f"proof ({engine})"
        )
    if re.search(r"\bunmount\b", low):
        return (
            f"unmount unencoded: unconstrained unmount is not a "
            f"proof ({engine})"
        )
    if re.search(r"\b(?:umount2|umount|mount)\b", low):
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
    if re.search(r"\bextattr_(?:set|get|delete|list)_(?:file|fd|link)\b", low):
        return (
            f"extattr unencoded: unconstrained extattr_set_file is not a "
            f"proof ({engine})"
        )
    if re.search(r"\b(?:set|get|list|remove)xattrat\b", low):
        return (
            f"setxattrat unencoded: unconstrained setxattrat is not a "
            f"proof ({engine})"
        )
    if (
        re.search(r"\b(?:lsetxattr|fsetxattr|setxattr)\b", low)
        or re.search(r"\b(?:listxattr|removexattr|getxattr)\b", low)
    ):
        return (
            f"setxattr unencoded: unconstrained setxattr is not a "
            f"proof ({engine})"
        )
    if re.search(r"\b(?:sched_setattr|sched_getattr)\b", low):
        return (
            f"sched_setattr unencoded: unconstrained sched_setattr is not a "
            f"proof ({engine})"
        )
    if re.search(r"\bsched_yield\b", low):
        return (
            f"sched_yield unencoded: unconstrained sched_yield is not a "
            f"proof ({engine})"
        )
    if re.search(r"\bpthread_yield\b", low):
        return (
            f"pthread_yield unencoded: unconstrained pthread_yield is not a "
            f"proof ({engine})"
        )
    if re.search(r"\bcpuset_(?:set|get)affinity\b", low):
        return (
            f"cpuset unencoded: unconstrained cpuset_setaffinity is not a "
            f"proof ({engine})"
        )
    if re.search(r"\bsched_get_priority_(?:max|min)\b", low):
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
    if re.search(r"\blio_listio\b", low):
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
    if re.search(r"\bclosefrom\b", low):
        return (
            f"closefrom unencoded: unconstrained closefrom is not a "
            f"proof ({engine})"
        )
    if re.search(r"\blsm_(?:get_self_attr|set_self_attr|list_modules)\b", low):
        return (
            f"lsm_get_self_attr unencoded: unconstrained "
            f"lsm_get_self_attr is not a proof ({engine})"
        )
    if "landlock" in low:
        return (
            f"landlock unencoded: unconstrained landlock_create_ruleset "
            f"is not a proof ({engine})"
        )
    if re.search(r"\brtprio(?:_thread)?\b", low):
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
    if re.search(r"\bsigaction\b", low):
        return (
            f"sigaction unencoded: unconstrained sigaction is not a "
            f"proof ({engine})"
        )
    if re.search(r"\bpthread_sigmask\b", low):
        return (
            f"pthread_sigmask unencoded: unconstrained pthread_sigmask "
            f"is not a proof ({engine})"
        )
    if re.search(r"\bsig(?:procmask|suspend)\b", low):
        return (
            f"sigprocmask unencoded: unconstrained sigprocmask is not a "
            f"proof ({engine})"
        )
    if re.search(r"\bsig(?:waitinfo|timedwait|pending|wait)\b", low):
        return (
            f"sigwait unencoded: unconstrained sigwait is not a "
            f"proof ({engine})"
        )
    if re.search(r"\bsigaltstack\b", low):
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
    if re.search(r"\bgetgrouplist\b", low):
        return (
            f"getgrouplist unencoded: unconstrained getgrouplist is not a "
            f"proof ({engine})"
        )
    if re.search(r"\bgetgroups\b", low):
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
    if re.search(r"\b(?:sendmsg|recvmsg)\b", low):
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
    if re.search(r"\b(?:migrate_pages|move_pages)\b", low):
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
    if re.search(r"\bopen_tree_attr\b", low):
        return (
            f"open_tree_attr unencoded: unconstrained open_tree_attr "
            f"is not a proof ({engine})"
        )
    if (
        "fsopen" in low
        or "fsmount" in low
        or re.search(r"\bopen_tree\b", low)
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
    if re.search(r"\b_umtx_op\b", low):
        return (
            f"_umtx_op unencoded: unconstrained _umtx_op is not a "
            f"proof ({engine})"
        )
    if re.search(r"\bfutex_waitv\b", low):
        return (
            f"futex_waitv unencoded: unconstrained futex_waitv is not a "
            f"proof ({engine})"
        )
    if re.search(r"\bfutex_(?:wake|wait|requeue)\b", low):
        return (
            f"futex_wait unencoded: unconstrained futex_wait is not a "
            f"proof ({engine})"
        )
    if re.search(r"\bfutex\b", low):
        return (
            f"futex unencoded: unconstrained futex is not a "
            f"proof ({engine})"
        )
    if "adjtimex" in low:
        return (
            f"adjtimex unencoded: unconstrained adjtimex is not a "
            f"proof ({engine})"
        )
    if re.search(r"\b(?:ntp_)?adjtime\b", low):
        return (
            f"adjtime unencoded: unconstrained adjtime is not a "
            f"proof ({engine})"
        )
    if re.search(r"\bntp_gettime\b", low):
        return (
            f"ntp_gettime unencoded: unconstrained ntp_gettime is not a "
            f"proof ({engine})"
        )
    if re.search(r"\brevoke\b", low):
        return (
            f"revoke unencoded: unconstrained revoke is not a "
            f"proof ({engine})"
        )
    if re.search(r"\bjail(?:_attach|_get|_set|_remove)?\b", low):
        return (
            f"jail unencoded: unconstrained jail is not a "
            f"proof ({engine})"
        )
    if re.search(r"\b(?:fflagstostr|strtofflags)\b", low):
        return (
            f"fflagstostr unencoded: unconstrained fflagstostr is not a "
            f"proof ({engine})"
        )
    if re.search(r"\bstrmode\b", low):
        return (
            f"strmode unencoded: unconstrained strmode is not a "
            f"proof ({engine})"
        )
    if re.search(r"\bstrtonum\b", low):
        return (
            f"strtonum unencoded: unconstrained strtonum is not a "
            f"proof ({engine})"
        )
    if re.search(r"\breallocarray\b", low):
        return (
            f"reallocarray unencoded: unconstrained reallocarray is not a "
            f"proof ({engine})"
        )
    if re.search(r"\breallocf\b", low):
        return (
            f"reallocf unencoded: unconstrained reallocf is not a "
            f"proof ({engine})"
        )
    if re.search(r"\b(?:get|set)progname\b", low):
        return (
            f"getprogname unencoded: unconstrained getprogname is not a "
            f"proof ({engine})"
        )
    if re.search(r"\b(?:setproctitle|daemon)\b", low):
        return (
            f"daemon unencoded: unconstrained daemon is not a "
            f"proof ({engine})"
        )
    if re.search(r"\b(?:auditon|getaudit|setaudit|auditctl)\b", low):
        return (
            f"auditon unencoded: unconstrained auditon is not a "
            f"proof ({engine})"
        )
    if re.search(r"\bkinfo_get(?:proc|file|vmmap)\b", low):
        return (
            f"kinfo_getproc unencoded: unconstrained kinfo_getproc is not a "
            f"proof ({engine})"
        )
    if re.search(r"\bkvm_(?:open|openfiles|getprocs|close|nlist)\b", low):
        return (
            f"kvm_open unencoded: unconstrained kvm_open is not a "
            f"proof ({engine})"
        )
    if re.search(r"\buuidgen\b", low):
        return (
            f"uuidgen unencoded: unconstrained uuidgen is not a "
            f"proof ({engine})"
        )
    if re.search(r"\bsetfib\b", low):
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
    if "ioperm" in low or re.search(r"\biopl\b", low):
        return (
            f"ioperm unencoded: unconstrained ioperm is not a "
            f"proof ({engine})"
        )
    if "mincore" in low:
        return (
            f"mincore unencoded: unconstrained mincore is not a "
            f"proof ({engine})"
        )
    if re.search(r"\brseq\b", low):
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
    if re.search(r"\bsyslog\b", low):
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
    if re.search(r"\bgetcpu\b", low):
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
    if re.search(
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
    if re.search(r"\btkill\b", low):
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
    if re.search(r"\b(?:shmat|shmdt)\b", low):
        return (
            f"shmat unencoded: unconstrained shmat is not a "
            f"proof ({engine})"
        )
    if re.search(r"\b(?:semop|semtimedop)\b", low):
        return (
            f"semop unencoded: unconstrained semop is not a "
            f"proof ({engine})"
        )
    if re.search(r"\b(?:msgsnd|msgrcv)\b", low):
        return (
            f"msgsnd unencoded: unconstrained msgsnd is not a "
            f"proof ({engine})"
        )
    if "sync_file_range" in low:
        return (
            f"sync_file_range unencoded: unconstrained "
            f"sync_file_range is not a proof ({engine})"
        )
    if re.search(r"\bremap_file_pages\b", low):
        return (
            f"remap_file_pages unencoded: unconstrained "
            f"remap_file_pages is not a proof ({engine})"
        )
    if re.search(r"\b(?:msync|mremap)\b", low):
        return (
            f"msync unencoded: unconstrained msync is not a "
            f"proof ({engine})"
        )
    if re.search(r"\bsocketpair\b", low):
        return (
            f"socketpair unencoded: unconstrained socketpair is not a "
            f"proof ({engine})"
        )
    if re.search(r"\bsysinfo\b", low):
        return (
            f"sysinfo unencoded: unconstrained sysinfo is not a "
            f"proof ({engine})"
        )
    if re.search(r"\bgettid\b", low):
        return (
            f"gettid unencoded: unconstrained gettid is not a "
            f"proof ({engine})"
        )
    if "setitimer" in low or "getitimer" in low:
        return (
            f"setitimer unencoded: unconstrained setitimer is not a "
            f"proof ({engine})"
        )
    if re.search(r"\bnice\b", low):
        return (
            f"nice unencoded: unconstrained nice is not a "
            f"proof ({engine})"
        )
    if re.search(r"\bgetdirentries\b", low):
        return (
            f"getdirentries unencoded: unconstrained getdirentries is not a "
            f"proof ({engine})"
        )
    if re.search(r"\bgetdents(?:64)?\b", low):
        return (
            f"getdents unencoded: unconstrained getdents is not a "
            f"proof ({engine})"
        )
    if "utimensat" in low or "futimens" in low or "utimes" in low:
        return (
            f"utimensat unencoded: unconstrained utimensat is not a "
            f"proof ({engine})"
        )
    if re.search(r"\blinkat\b", low):
        return (
            f"linkat unencoded: unconstrained linkat is not a "
            f"proof ({engine})"
        )
    if re.search(r"\bset_mempolicy_home_node\b", low):
        return (
            f"set_mempolicy_home_node unencoded: unconstrained "
            f"set_mempolicy_home_node is not a proof ({engine})"
        )
    if (
        "mbind" in low
        or re.search(r"\bset_mempolicy\b", low)
        or re.search(r"\bget_mempolicy\b", low)
    ):
        return (
            f"mbind unencoded: unconstrained mbind is not a "
            f"proof ({engine})"
        )
    if re.search(r"\b(?:setreuid|setregid|setresuid|setresgid)\b", low):
        return (
            f"setreuid unencoded: unconstrained setreuid is not a "
            f"proof ({engine})"
        )
    if re.search(r"\bgetres(?:uid|gid)\b", low):
        return (
            f"getresuid unencoded: unconstrained getresuid is not a "
            f"proof ({engine})"
        )
    if re.search(r"\b(?:setfsuid|setfsgid)\b", low):
        return (
            f"setfsuid unencoded: unconstrained setfsuid is not a "
            f"proof ({engine})"
        )
    if re.search(r"\b(?:setpgid|setsid|getsid)\b", low):
        return (
            f"setpgid unencoded: unconstrained setpgid is not a "
            f"proof ({engine})"
        )
    if re.search(r"\bcachestat\b", low):
        return (
            f"cachestat unencoded: unconstrained cachestat is not a "
            f"proof ({engine})"
        )
    if re.search(r"\bmap_shadow_stack\b", low):
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
    if re.search(r"\batomic_(?:thread|signal)_fence\b", low):
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
    if re.search(r"\btimed_mutex\b", low):
        return (
            f"C++ timed_mutex unencoded: {engine} is not a "
            "timed_mutex model"
        )
    if re.search(r"\b(?:ifstream|ofstream|fstream)\b", low):
        return (
            f"C++ fstream unencoded: {engine} is not an fstream model"
        )
    if "this_thread" in low:
        return (
            f"C++ this_thread unencoded: {engine} is not a "
            "this_thread model"
        )
    if re.search(r"\bcall_once\b", low):
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
    if re.search(r"\bfrom_range\b", low) or "from_range unencoded" in low:
        return (
            f"C++ from_range unencoded: {engine} is not a from_range model"
        )
    if re.search(r"\branges\s*::\s*to\b", low) or "ranges::to unencoded" in low:
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
    if re.search(r"\bstd\s*::\s*endian\b", low) or "endian unencoded" in low:
        return (
            f"C++ std::endian unencoded: {engine} is not an endian model"
        )
    if re.search(r"\bstd\s*::\s*rot[lr]\b", low) or "rotl unencoded" in low:
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
    if re.search(r"\bstd\s*::\s*bit_width\b", low) or "bit_width unencoded" in low:
        return (
            f"C++ std::bit_width unencoded: {engine} is not a bit_width model"
        )
    if re.search(r"\bstd\s*::\s*gcd\b", low) or "gcd unencoded" in low:
        return (
            f"C++ std::gcd unencoded: {engine} is not a gcd model"
        )
    if re.search(r"\bstd\s*::\s*lcm\b", low) or "lcm unencoded" in low:
        return (
            f"C++ std::lcm unencoded: {engine} is not a lcm model"
        )
    if re.search(r"\bstd\s*::\s*clamp\b", low) or "clamp unencoded" in low:
        return (
            f"C++ std::clamp unencoded: {engine} is not a clamp model"
        )
    if re.search(r"\bstd\s*::\s*exchange\b", low) or "exchange unencoded" in low:
        return (
            f"C++ std::exchange unencoded: {engine} is not an exchange model"
        )
    if re.search(r"\bstd\s*::\s*to_address\b", low) or "to_address unencoded" in low:
        return (
            f"C++ std::to_address unencoded: {engine} is not a "
            "to_address model"
        )
    if re.search(r"\b(?:construct_at|destroy_at)\b", low) or (
        "construct_at unencoded" in low
    ):
        return (
            f"C++ construct_at unencoded: {engine} is not a "
            "construct_at model"
        )
    if re.search(r"\bdestroy_n\b", low) or "destroy_n unencoded" in low:
        return (
            f"C++ destroy_n unencoded: {engine} is not a destroy_n model"
        )
    if re.search(r"\b(?:std\s*::\s*)?addressof\b", low) or (
        "addressof unencoded" in low
    ):
        return (
            f"C++ std::addressof unencoded: {engine} is not an "
            "addressof model"
        )
    if re.search(r"\bas_rvalue(?:_view)?\b", low) or "as_rvalue unencoded" in low:
        return (
            f"C++ as_rvalue unencoded: {engine} is not an as_rvalue model"
        )
    if re.search(r"\bas_const\b", low) or "as_const unencoded" in low:
        return (
            f"C++ as_const unencoded: {engine} is not an as_const model"
        )
    if (
        re.search(r"\btransform_(?:inclusive|exclusive)_scan\b", low)
        or "transform_inclusive_scan unencoded" in low
        or "transform_exclusive_scan unencoded" in low
    ):
        return (
            f"C++ transform_inclusive_scan unencoded: {engine} is not a "
            "transform_inclusive_scan model"
        )
    if re.search(r"\bexclusive_scan\b", low) or "exclusive_scan unencoded" in low:
        return (
            f"C++ exclusive_scan unencoded: {engine} is not an "
            "exclusive_scan model"
        )
    if re.search(r"\binclusive_scan\b", low) or "inclusive_scan unencoded" in low:
        return (
            f"C++ inclusive_scan unencoded: {engine} is not an "
            "inclusive_scan model"
        )
    if re.search(r"\btransform_reduce\b", low) or "transform_reduce unencoded" in low:
        return (
            f"C++ transform_reduce unencoded: {engine} is not a "
            "transform_reduce model"
        )
    if re.search(r"\bstd\s*::\s*reduce\b", low) or "std::reduce unencoded" in low:
        return (
            f"C++ std::reduce unencoded: {engine} is not a reduce model"
        )
    if re.search(
        r"\buninitialized_(?:fill(?:_n)?|default_construct(?:_n)?)\b",
        low,
    ) or "uninitialized_fill unencoded" in low:
        return (
            f"C++ uninitialized_fill unencoded: {engine} is not an "
            "uninitialized_fill model"
        )
    if re.search(r"\buninitialized_value_construct(?:_n)?\b", low) or (
        "uninitialized_value_construct unencoded" in low
    ):
        return (
            f"C++ uninitialized_value_construct unencoded: {engine} is not an "
            "uninitialized_value_construct model"
        )
    if re.search(r"\buninitialized_(?:copy|move)(?:_n)?\b", low) or (
        "uninitialized_copy unencoded" in low
    ):
        return (
            f"C++ uninitialized_copy unencoded: {engine} is not an "
            "uninitialized_copy model"
        )
    if (
        re.search(r"\b(?:add_sat|sub_sat|mul_sat|div_sat|saturate_cast)\b", low)
        or "add_sat unencoded" in low
    ):
        return (
            f"C++ add_sat unencoded: {engine} is not an add_sat model"
        )
    if re.search(r"\bnontype\b", low) or "nontype unencoded" in low:
        return (
            f"C++ nontype unencoded: {engine} is not a nontype model"
        )
    if re.search(r"\bis_layout_compatible\b", low) or (
        "is_layout_compatible unencoded" in low
    ):
        return (
            f"C++ is_layout_compatible unencoded: {engine} is not an "
            "is_layout_compatible model"
        )
    if re.search(r"\bis_pointer_interconvertible_(?:with_class|base_of)\b", low) or (
        "is_pointer_interconvertible unencoded" in low
    ):
        return (
            f"C++ is_pointer_interconvertible unencoded: {engine} is not an "
            "is_pointer_interconvertible model"
        )
    if re.search(r"\bbasic_const_iterator\b", low) or (
        "basic_const_iterator unencoded" in low
    ):
        return (
            f"C++ basic_const_iterator unencoded: {engine} is not a "
            "basic_const_iterator model"
        )
    if re.search(r"\bis_corresponding_member\b", low) or (
        "is_corresponding_member unencoded" in low
    ):
        return (
            f"C++ is_corresponding_member unencoded: {engine} is not an "
            "is_corresponding_member model"
        )
    if re.search(r"\bforward_like\b", low) or "forward_like unencoded" in low:
        return (
            f"C++ forward_like unencoded: {engine} is not a "
            "forward_like model"
        )
    if re.search(r"\b(?:set|get)_terminate\b", low) or (
        "set_terminate unencoded" in low
    ):
        return (
            f"C++ set_terminate unencoded: {engine} is not a "
            "set_terminate model"
        )
    if re.search(r"\bis_constant_evaluated\b", low) or (
        "is_constant_evaluated unencoded" in low
    ):
        return (
            f"C++ is_constant_evaluated unencoded: {engine} is not an "
            "is_constant_evaluated model"
        )
    if re.search(r"\bstd\s*::\s*lerp\b", low) or "lerp unencoded" in low:
        return (
            f"C++ std::lerp unencoded: {engine} is not a lerp model"
        )
    if re.search(r"\bstd\s*::\s*midpoint\b", low) or "midpoint unencoded" in low:
        return (
            f"C++ std::midpoint unencoded: {engine} is not a midpoint model"
        )
    if re.search(
        r"\bstd\s*::\s*(?:cmp_(?:less|greater|less_equal|"
        r"greater_equal|equal_to|not_equal_to)|in_range)\b",
        low,
    ) or "cmp_less unencoded" in low:
        return (
            f"C++ std::cmp_less unencoded: {engine} is not a cmp_less model"
        )
    if re.search(r"\bstd\s*::\s*count[lr]_(?:zero|one)\b", low) or (
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
    if re.search(r"\b(?:views\s*::\s*counted\b|\bcounted_view\b)", low) or (
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
    if re.search(r"\bmake_exception_ptr\b", low) or (
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
    return bool(re.search(r"\bthrow\b", fn.body or ""))


def _has_unencoded_setjmp(fn: FunctionInfo) -> bool:
    """setjmp/longjmp/va_list are not in the bitvector encoder.

    Missing nonlocal-control / variadic model, not a parse ERROR and
    not a proof. `goto` stays ERROR and is not this case.
    """
    return bool(re.search(
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
    schedule = _unwind_schedule(unwind) if incremental else [unwind]
    last: Finding | None = None
    tried: list[int] = []
    for k in schedule:
        rec = _bmc_once(fn, k, try_unbounded, enums, base)
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


def run_bmc(functions: list[FunctionInfo], unwind: int) -> list[Finding]:
    from prism.inline import inline_static
    return [k_induction(fn, unwind) for fn in inline_static(functions)]
