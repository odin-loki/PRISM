"""SV-COMP violation witnesses (format 2.0, YAML) from PRISM findings.

Roadmap 6.3. A PRISM ``FAILED`` finding carries a counterexample: for a
function with parameters, parameter assignments (``a=1, b=-2`` from the
Python engine, ``a=#x0000000f`` bit patterns from the C++ engine); for a
whole program, the values returned by ``__VERIFIER_nondet_*`` calls, in call
order, when the engine reports them. This module turns that into a violation
witness in the SV-COMP witness format 2.0:

    - entry_type: violation_sequence
      metadata: {format_version: "2.0", uuid, creation_time, producer, task}
      content:
        - segment: [ {waypoint: {type: function_return, action: follow,
                                 location: {...}, constraint: {value: "\\result == 5",
                                                               format: c_expression}}} ]
        - ...
        - segment: [ {waypoint: {type: target, action: follow, location: {...}}} ]

Format reference: https://gitlab.com/sosy-lab/benchmarking/sv-witnesses
(``README-YAML.md``, version 2.0). What this module does NOT do: validate the
witness. A witness is only worth points in SV-COMP once a validator
(CPAchecker, UAutomizer, ...) confirms it; PRISM has not run one (see
docs/SVCOMP.md).

Self-contained on purpose (standard library only): it ships next to the
BenchExec wrapper in an SV-COMP tool archive.
"""

from __future__ import annotations

import datetime as _dt
import hashlib
import re
import uuid as _uuid
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any

FORMAT_VERSION = "2.0"
PRODUCER = "PRISM"

# `name = value` pairs of a PRISM counterexample (both engines).
CEX_RE = re.compile(r"([A-Za-z_]\w*)\s*=\s*(#x[0-9a-fA-F]+|#b[01]+|-?0[xX][0-9a-fA-F]+|-?\d+|true|false)")
# Integer C types by width and signedness (for bit-pattern reinterpretation).
_WIDTH = {
    "char": (8, True), "signed char": (8, True), "unsigned char": (8, False),
    "short": (16, True), "unsigned short": (16, False),
    "int": (32, True), "unsigned": (32, False), "unsigned int": (32, False),
    "long": (64, True), "unsigned long": (64, False),
    "long long": (64, True), "unsigned long long": (64, False),
    "_Bool": (1, False), "bool": (1, False),
}


@dataclass
class Location:
    file_name: str
    line: int
    column: int = 1
    function: str | None = None

    def as_dict(self) -> dict[str, Any]:
        d: dict[str, Any] = {"file_name": self.file_name, "line": int(self.line), "column": max(1, int(self.column))}
        if self.function:
            d["function"] = self.function
        return d


@dataclass
class NondetValue:
    """One value returned by a ``__VERIFIER_nondet_*`` call on the violating path.

    ``location`` must point at the call (format 2.0: a ``function_return``
    waypoint's location is the closing parenthesis of the call).
    """

    location: Location
    value: int | float


@dataclass
class Counterexample:
    function: str
    target: Location
    params: dict[str, int] = field(default_factory=dict)
    param_location: Location | None = None  # first statement of the function body
    nondet: list[NondetValue] = field(default_factory=list)


def parse_assignments(cex: str) -> dict[str, int]:
    """``a=1, b=#xffffffff`` -> {"a": 1, "b": 4294967295} (raw bit patterns kept)."""
    vals: dict[str, int] = {}
    for name, v in CEX_RE.findall(cex or ""):
        if v in ("true", "false"):
            vals[name] = 1 if v == "true" else 0
        elif v.startswith("#x"):
            vals[name] = int(v[2:], 16)
        elif v.startswith("#b"):
            vals[name] = int(v[2:], 2)
        else:
            vals[name] = int(v, 0)
    return vals


def reinterpret(value: int, ctype: str) -> int:
    """Reinterpret a solver bit pattern in the C type (two's complement)."""
    t = " ".join(ctype.replace("signed int", "int").split())
    width, signed = _WIDTH.get(t, (0, True))
    if not width:
        return value
    value &= (1 << width) - 1
    if signed and value >= 1 << (width - 1):
        value -= 1 << width
    return value


def sha256_file(path: Path) -> str:
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def _c_literal(v: int | float) -> str:
    if isinstance(v, float):
        return repr(v)
    return str(v) if v >= 0 else f"({v})"


def build_violation_witness(
    cex: Counterexample,
    *,
    input_file: Path,
    input_file_name: str,
    specification: str,
    data_model: str = "LP64",
    language: str = "C",
    producer_version: str = "unknown",
    creation_time: str | None = None,
    witness_uuid: str | None = None,
) -> list[dict[str, Any]]:
    """The witness document (a YAML list with one ``violation_sequence`` entry)."""
    now = creation_time or _dt.datetime.now(_dt.timezone.utc).replace(microsecond=0).isoformat().replace("+00:00", "Z")
    segments: list[dict[str, Any]] = []
    if cex.params and cex.param_location is not None:
        conj = " && ".join(f"({k} == {_c_literal(v)})" for k, v in cex.params.items())
        segments.append({"segment": [{"waypoint": {
            "type": "assumption", "action": "follow",
            "location": cex.param_location.as_dict(),
            "constraint": {"value": conj, "format": "c_expression"},
        }}]})
    for nv in cex.nondet:
        segments.append({"segment": [{"waypoint": {
            "type": "function_return", "action": "follow",
            "location": nv.location.as_dict(),
            "constraint": {"value": f"\\result == {_c_literal(nv.value)}", "format": "c_expression"},
        }}]})
    segments.append({"segment": [{"waypoint": {
        "type": "target", "action": "follow", "location": cex.target.as_dict(),
    }}]})
    return [{
        "entry_type": "violation_sequence",
        "metadata": {
            "format_version": FORMAT_VERSION,
            "uuid": witness_uuid or str(_uuid.uuid4()),
            "creation_time": now,
            "producer": {"name": PRODUCER, "version": producer_version},
            "task": {
                "input_files": [input_file_name],
                "input_file_hashes": {input_file_name: sha256_file(input_file)},
                "specification": specification.strip(),
                "data_model": data_model,
                "language": language,
            },
        },
        "content": segments,
    }]


# --------------------------------------------------------------------------- YAML

_PLAIN = re.compile(r"^[A-Za-z_][A-Za-z0-9_.\-/]*$")
_RESERVED = {"true", "false", "null", "yes", "no", "on", "off", "~", "y", "n"}


def _scalar(v: Any) -> str:
    if isinstance(v, bool):
        return "true" if v else "false"
    if isinstance(v, (int, float)):
        return repr(v)
    s = str(v)
    if _PLAIN.match(s) and s.lower() not in _RESERVED:
        return s
    return '"' + s.replace("\\", "\\\\").replace('"', '\\"').replace("\n", "\\n") + '"'


def _emit(v: Any, indent: int, out: list[str], in_list: bool = False) -> None:
    pad = "  " * indent
    if isinstance(v, dict):
        first = True
        for k, val in v.items():
            lead = pad if not (in_list and first) else ""
            first = False
            if isinstance(val, (dict, list)) and val:
                out.append(f"{lead}{_scalar(k)}:")
                _emit(val, indent + 1, out)
            else:
                out.append(f"{lead}{_scalar(k)}: {_scalar(val) if not isinstance(val, (dict, list)) else ('{}' if isinstance(val, dict) else '[]')}")
    elif isinstance(v, list):
        for item in v:
            if isinstance(item, (dict, list)) and item:
                sub: list[str] = []
                _emit(item, indent + 1, sub, in_list=True)
                out.append(f"{pad}- {sub[0]}")
                out.extend(sub[1:])
            else:
                out.append(f"{pad}- {_scalar(item)}")
    else:
        out.append(pad + _scalar(v))


def to_yaml(doc: Any) -> str:
    """Block-style YAML for the witness (standard library only)."""
    out: list[str] = []
    _emit(doc, 0, out)
    return "\n".join(out) + "\n"


def write_witness(doc: list[dict[str, Any]], path: Path) -> None:
    Path(path).write_text(to_yaml(doc), encoding="utf-8")
