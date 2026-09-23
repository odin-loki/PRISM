"""Qt 6 GUI (PySide6). Same pipeline as the CLI. Native C++ GUI lives in src/gui.

Helpers in this module do not import Qt. Missing PySide6 or display is NOTRUN,
never CLEAN and never a fake window success.
"""

from __future__ import annotations

from pathlib import Path
import importlib.util
import os
import shutil
import subprocess
import sys
from typing import Any

from prism import journal, laws
from prism.config import Config
from prism.models import Finding, RunReport
from prism.pipeline import run_pipeline
from prism.taxonomy import coverage_from_report

# Package presence only — does not load QtWidgets or need a display.
HAS_PYSIDE6 = importlib.util.find_spec("PySide6") is not None

# Same columns the CLI report uses: status / stage / cls, plus location.
FINDING_COLUMNS = ("status", "stage", "cls", "file", "line", "function", "message")

_TAXONOMY_VERDICTS = {"COVERED", "PARTIAL", "GAP"}

_NOISE_STAGES = {"inventory", "classify", "unify"}

# Empty scope / missing report.json: 0, never n/a, never a proof.
MISSING_REPORT_LABEL = "confidence 0  (report.json missing; not a proof)"

_qt_ready = False


def skip_from_checks(fuzz: bool, repair: bool, optional: bool) -> list[str]:
    skip: list[str] = []
    if fuzz:
        skip.append("fuzz")
    if repair:
        skip.append("repair")
    if optional:
        skip.append("optional")
    return skip


def gui_import_status() -> str:
    """Toolkit availability. Missing PySide6 is NOTRUN. Never CLEAN."""
    if HAS_PYSIDE6:
        return "available"
    return laws.NOTRUN


def missing_pyside_finding() -> Finding | None:
    """A missing Qt binding is a NOTRUN finding, not a clean window."""
    if HAS_PYSIDE6:
        return None
    return Finding(
        stage="gui",
        status=laws.NOTRUN,
        file="",
        function=None,
        line=None,
        cls="",
        message="PySide6 not installed; Qt window is NOTRUN",
        strength=laws.STRENGTH_READS,
        extra={"install": "pip install PySide6"},
    )


def missing_display_finding(reason: str = "") -> Finding:
    """A missing Qt display is NOTRUN, never CLEAN and never a fake window."""
    msg = "no display; Qt window is NOTRUN"
    if reason:
        msg = f"{msg} ({reason})"
    return Finding(
        stage="gui",
        status=laws.NOTRUN,
        file="",
        function=None,
        line=None,
        cls="",
        message=msg,
        strength=laws.STRENGTH_READS,
        extra={"install": "QT_QPA_PLATFORM=offscreen or a real display"},
    )


def _confidence_number(value: Any) -> float:
    """Empty / missing / n/a / NaN is 0. Never a string placeholder."""
    if value is None:
        return 0.0
    if isinstance(value, str):
        s = value.strip().lower()
        if s in {"", "n/a", "na", "none", "null"}:
            return 0.0
        try:
            value = float(s)
        except ValueError:
            return 0.0
    try:
        n = float(value)
    except (TypeError, ValueError):
        return 0.0
    if n != n:  # NaN
        return 0.0
    return n


def confidence_product(report: RunReport) -> dict[str, float]:
    """visibility × answer × resolution. 0 is no data, not CLEAN, never n/a."""
    return {
        "visibility": _confidence_number(report.visibility),
        "answer": _confidence_number(report.answer),
        "resolution": _confidence_number(report.resolution),
        "confidence": _confidence_number(report.confidence),
    }


def confidence_label(report: RunReport) -> str:
    """Same line `python -m prism` prints for the confidence product.

    Empty scope is 0, never n/a, never CLEAN.
    """
    prod = confidence_product(report)
    return (
        f"confidence {prod['confidence']}  "
        f"(vis {prod['visibility']} x ans {prod['answer']} x res {prod['resolution']})"
    )


_LLM_STATUSES = {laws.HYPOTHESIS, laws.READS}
_IGNORE_FOR_COVER = {
    laws.NOTRUN, laws.CLEAN, laws.ERROR, laws.TIMEOUT,
    laws.UNKNOWN, laws.NEEDS_HARNESS, laws.NOSEED,
}


def _only_llm_hits(report: RunReport, cid: str) -> bool:
    """True when every non-noise finding for `cid` is LLM / HYPOTHESIS / READS."""
    saw = False
    for s in report.stages:
        for f in s.findings:
            if (f.cls or "") != cid:
                continue
            if f.status in _IGNORE_FOR_COVER:
                continue
            saw = True
            if s.name != "llm" and f.status not in _LLM_STATUSES:
                return False
    return saw


def refuse_llm_cover(report: RunReport, cid: str, verdict: str, best: str) -> str:
    """LLM cannot COVER. READS or only-LLM hits demote COVERED to PARTIAL."""
    if verdict != "COVERED":
        return verdict
    if best in {laws.STRENGTH_READS, laws.READS}:
        return "PARTIAL"
    if _only_llm_hits(report, cid):
        return "PARTIAL"
    return verdict


def taxonomy_rows(report: RunReport) -> list[dict[str, str]]:
    """COVERED / PARTIAL / GAP from the same report.json findings. No Qt.

    Always `coverage_from_report` — never a stale taxonomy.json / report.taxonomy
    key. CLEAN is never a taxonomy verdict and never a proof. The LLM is READS
    and cannot paint a class COVERED even if a helper tried to.
    """
    rows: list[dict[str, str]] = []
    for tax in coverage_from_report(report):
        cid = tax.get("id") or ""
        best = "" if not tax.get("best") else str(tax["best"])
        verdict = tax.get("verdict") or "GAP"
        if verdict == laws.CLEAN or verdict not in _TAXONOMY_VERDICTS:
            verdict = "GAP"
        verdict = refuse_llm_cover(report, cid, verdict, best)
        rows.append({
            "id": cid,
            "verdict": verdict,
            "best": best,
        })
    return rows


def finding_rows(report: RunReport) -> list[dict[str, str]]:
    """Table rows with the CLI finding vocabulary. No Qt, no QApplication.

    Status strings stay distinct: PROVED is not CLEAN, CLEAN is not a proof,
    NOTRUN is not CLEAN. Noise CLEAN/NOTRUN rows from inventory/classify/unify
    are dropped the same way report.md drops them.
    """
    rows: list[dict[str, str]] = []
    for s in report.stages:
        for f in s.findings:
            if f.status in {laws.NOTRUN, laws.CLEAN} and s.name in _NOISE_STAGES:
                continue
            rows.append({
                "status": f.status,
                "stage": f.stage or s.name,
                "cls": f.cls or "",
                "file": f.file or "",
                "line": "" if f.line is None else str(f.line),
                "function": f.function or "",
                "message": (f.message or "")[:200],
            })
    return rows


_STATUS_BG = {
    "PROVED-UNBOUNDED": "#1f6f3a",
    "PROVED": "#2a8148",
    "PROVED-ASSUMING": "#3a7a4a",
    "BOUNDED": "#6b6b2a",
    "FAILED": "#8b2e2e",
    "CRASH": "#a11",
    "CLEAN": "#2a4a6b",
    "HYPOTHESIS": "#5a3a7a",
    "NOTRUN": "#6b5a2a",
    "ERROR": "#5a5a5a",
    "NEEDS-HARNESS": "#5a4a2a",
    "TIMEOUT": "#4a4a4a",
    "UNKNOWN": "#4a4a4a",
}

_PROOF_GREEN = frozenset({
    _STATUS_BG["PROVED-UNBOUNDED"],
    _STATUS_BG["PROVED"],
    _STATUS_BG["PROVED-ASSUMING"],
})


def status_background(status: str) -> str:
    """Finding-status color. CLEAN is blue, never proof-green."""
    if status == laws.CLEAN:
        return _STATUS_BG[laws.CLEAN]
    return _STATUS_BG.get(status, "")


def taxonomy_background(verdict: str) -> str:
    """COVERED reuses PROVED green. CLEAN (if it arrived) is CLEAN blue."""
    if verdict == "COVERED":
        return _STATUS_BG["PROVED"]
    if verdict == "PARTIAL":
        return _STATUS_BG["BOUNDED"]
    if verdict == "GAP":
        return _STATUS_BG["NOTRUN"]
    if verdict == laws.CLEAN:
        return _STATUS_BG[laws.CLEAN]
    return ""


def prism_cpp_binary(explicit: str | None = None) -> str | None:
    """The C++ `prism` binary the assistant runs (explicit, PRISM_BIN, PATH)."""
    for cand in (explicit, os.environ.get("PRISM_BIN"), shutil.which("prism")):
        if cand and Path(cand).is_file() and os.access(cand, os.X_OK):
            return str(cand)
    return None


def assistant_reply(question: str, out_dir: Path, prism_bin: str | None = None,
                    use_model: bool = False, timeout: float = 120.0) -> str:
    """One assistant turn (roadmap 9.4): `prism ask` over <out_dir>/report.json.

    The assistant is C++ engine only (roadmap D8): this helper runs the C++
    binary and shows its answer, which always starts with the structured
    query. Without the binary or a report the reply is NOTRUN, never an
    invented answer. Nothing here can change a verdict.
    """
    q = question.strip()
    if not q:
        return ""
    report = Path(out_dir) / "report.json"
    if not report.is_file():
        return f"NOTRUN assistant: no report.json in {out_dir} (run the pipeline first)"
    exe = prism_cpp_binary(prism_bin)
    if exe is None:
        return ("NOTRUN assistant: C++ prism binary not found (set PRISM_BIN); the assistant "
                "runs `prism ask` (C++ engine only, roadmap D8)")
    cmd = [exe, "ask", q, "--report", str(report)]
    if not use_model:
        cmd.append("--no-llm")
    try:
        r = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout)
    except (OSError, subprocess.TimeoutExpired) as ex:
        return f"ERROR assistant: {ex}"
    if r.returncode != 0:
        return f"ERROR assistant: exit {r.returncode}: {(r.stderr or r.stdout).strip()[:500]}"
    return r.stdout.rstrip()


def _notrun_gui(reason: str, detail: str = "") -> int:
    """Missing toolkit/display is NOTRUN. Never a fake CLEAN window. Exit 0."""
    print(f"NOTRUN gui: {reason} — not a clean window")
    if detail:
        print(f"  {detail}")
    print("  or build prism_gui when Qt6 Widgets is present")
    return 0


def _ensure_qt() -> None:
    """Import PySide6 and bind Worker/MainWindow. Raises ImportError if missing."""
    global _qt_ready
    if _qt_ready:
        return

    from PySide6.QtCore import QObject, Qt, QThread, QTimer, Signal
    from PySide6.QtGui import QColor, QFont
    from PySide6.QtWidgets import (
        QApplication,
        QFileDialog,
        QHBoxLayout,
        QHeaderView,
        QLabel,
        QLineEdit,
        QMainWindow,
        QMessageBox,
        QPushButton,
        QSplitter,
        QTableWidget,
        QTableWidgetItem,
        QTextEdit,
        QVBoxLayout,
        QWidget,
        QCheckBox,
    )

    class Worker(QObject):
        done = Signal(object)
        failed = Signal(str)

        def __init__(self, cfg: Config) -> None:
            super().__init__()
            self.cfg = cfg

        def run(self) -> None:
            try:
                self.done.emit(run_pipeline(self.cfg))
            except Exception as ex:
                self.failed.emit(str(ex))

    class MainWindow(QMainWindow):
        def __init__(self, path: Path | None = None, allow_exec: bool = False) -> None:
            super().__init__()
            self.setWindowTitle("PRISM — hybrid code testing")
            self.resize(1280, 800)
            self._thread: QThread | None = None
            self._worker: Any = None
            self._out = Path("prism-out")
            self._last_logged: str | None = None
            self._timer = QTimer(self)
            self._timer.setInterval(400)
            self._timer.timeout.connect(self._poll_journal)

            root = QWidget()
            self.setCentralWidget(root)
            layout = QVBoxLayout(root)

            bar = QHBoxLayout()
            self.path_lbl = QLabel(str(path or Path("testdata").resolve()))
            self.path_lbl.setTextInteractionFlags(Qt.TextSelectableByMouse)
            pick = QPushButton("Open…")
            pick.clicked.connect(self._pick)
            self.llm_ck = QCheckBox("Qwen 3.5 9B")
            self.llm_ck.setChecked(True)
            self.resume_ck = QCheckBox("Resume last report")
            # Law 9: off unless the user opts in (or launched with --allow-exec).
            self.allow_exec_ck = QCheckBox("Allow executing scanned code")
            self.allow_exec_ck.setChecked(bool(allow_exec))
            self.allow_exec_ck.setToolTip(
                "--allow-exec: sanitizer/fuzz/diff harnesses, perl -c, cargo clippy, "
                "eslint, ParanoidBSD modules, LLM programs. Only on code you trust; "
                "off = those steps are NOTRUN.")
            self.run_btn = QPushButton("Run pipeline")
            self.run_btn.clicked.connect(self._run)
            bar.addWidget(self.path_lbl, 1)
            bar.addWidget(pick)
            bar.addWidget(self.llm_ck)
            bar.addWidget(self.resume_ck)
            bar.addWidget(self.allow_exec_ck)
            bar.addWidget(self.run_btn)
            layout.addLayout(bar)

            skip_bar = QHBoxLayout()
            skip_bar.addWidget(QLabel("Skip:"))
            self.skip_fuzz_ck = QCheckBox("fuzz")
            self.skip_repair_ck = QCheckBox("repair")
            self.skip_optional_ck = QCheckBox("optional")
            skip_bar.addWidget(self.skip_fuzz_ck)
            skip_bar.addWidget(self.skip_repair_ck)
            skip_bar.addWidget(self.skip_optional_ck)
            skip_bar.addStretch(1)
            layout.addLayout(skip_bar)

            stats = QHBoxLayout()
            self.s_vis = QLabel("visibility —")
            self.s_ans = QLabel("answer —")
            self.s_res = QLabel("resolution —")
            self.s_conf = QLabel("confidence —")
            for w in (self.s_vis, self.s_ans, self.s_res, self.s_conf):
                w.setFont(QFont("Segoe UI", 11, QFont.Bold))
                stats.addWidget(w)
            layout.addLayout(stats)

            split = QSplitter(Qt.Vertical)
            self.stages = QTableWidget(0, 5)
            self.stages.setHorizontalHeaderLabels(["stage", "status", "records", "seconds", "note"])
            self.stages.horizontalHeader().setSectionResizeMode(QHeaderView.Stretch)
            self.findings = QTableWidget(0, len(FINDING_COLUMNS))
            self.findings.setHorizontalHeaderLabels(list(FINDING_COLUMNS))
            self.findings.horizontalHeader().setSectionResizeMode(QHeaderView.Stretch)
            self.taxonomy = QTableWidget(0, 3)
            self.taxonomy.setHorizontalHeaderLabels(["class", "verdict", "best"])
            self.taxonomy.horizontalHeader().setSectionResizeMode(QHeaderView.Stretch)
            split.addWidget(self.stages)
            split.addWidget(self.findings)
            split.addWidget(self.taxonomy)
            split.setStretchFactor(1, 2)
            layout.addWidget(split, 1)

            self.log = QTextEdit()
            self.log.setReadOnly(True)
            self.log.setMaximumHeight(140)
            layout.addWidget(self.log)

            # Assistant (roadmap 9.4): questions over findings, "explain <id>",
            # "trusted base". Runs the C++ `prism ask`; the answer shows the
            # structured query. It never changes a verdict.
            ask_bar = QHBoxLayout()
            ask_bar.addWidget(QLabel("Assistant"))
            self.ask_edit = QLineEdit()
            self.ask_edit.setPlaceholderText("Ask about findings, or: explain <id> / trusted base")
            self.ask_btn = QPushButton("Ask")
            self.ask_btn.clicked.connect(self._ask)
            self.ask_edit.returnPressed.connect(self._ask)
            ask_bar.addWidget(self.ask_edit, 1)
            ask_bar.addWidget(self.ask_btn)
            layout.addLayout(ask_bar)
            self.chat = QTextEdit()
            self.chat.setReadOnly(True)
            self.chat.setMaximumHeight(160)
            layout.addWidget(self.chat)
            self._load_last()

        def _ask(self) -> None:
            q = self.ask_edit.text().strip()
            if not q:
                return
            self.chat.append(f"> {q}")
            self.chat.append(assistant_reply(q, self._out, use_model=self.llm_ck.isChecked()))

        def _load_last(self) -> None:
            p = Path("prism-out") / "report.json"
            rec = RunReport.load(p)
            if rec is None:
                # Empty scope is 0, never n/a, never a proof, never CLEAN.
                self.s_vis.setText("visibility 0")
                self.s_ans.setText("answer 0")
                self.s_res.setText("resolution 0")
                self.s_conf.setText(MISSING_REPORT_LABEL)
                return
            self._on_done(rec)
            self.log.append(f"loaded {p.resolve()}")

        def _pick(self) -> None:
            d = QFileDialog.getExistingDirectory(self, "Code to test", self.path_lbl.text())
            if d:
                self.path_lbl.setText(d)

        def _run(self) -> None:
            self.run_btn.setEnabled(False)
            self.log.append("running…")
            skip = skip_from_checks(
                self.skip_fuzz_ck.isChecked(),
                self.skip_repair_ck.isChecked(),
                self.skip_optional_ck.isChecked(),
            )
            cfg = Config(
                root=Path(self.path_lbl.text()),
                out=self._out,
                skip=skip,
                llm=self.llm_ck.isChecked(),
                resume=self.resume_ck.isChecked(),
                allow_exec=self.allow_exec_ck.isChecked(),
                fuzz_budget=4.0,
                fuzz_iters=256,
                repair_rounds=1,
            )
            self._timer.start()
            self._thread = QThread()
            self._worker = Worker(cfg)
            self._worker.moveToThread(self._thread)
            self._thread.started.connect(self._worker.run)
            self._worker.done.connect(self._on_done)
            self._worker.failed.connect(self._on_fail)
            self._worker.done.connect(self._thread.quit)
            self._worker.failed.connect(self._thread.quit)
            self._thread.start()

        def _poll_journal(self) -> None:
            recs = journal.read_stages(self._out)
            if not recs:
                return
            self.stages.setRowCount(0)
            for s in recs:
                r = self.stages.rowCount()
                self.stages.insertRow(r)
                vals = [s.name, s.status, str(s.records), f"{s.elapsed:.2f}", s.detail or s.install]
                for c, v in enumerate(vals):
                    self.stages.setItem(r, c, QTableWidgetItem(v))
            last = recs[-1]
            key = f"{last.name}:{last.status}:{last.records}"
            if key != self._last_logged:
                self._last_logged = key
                self.log.append(f"{last.name} {last.status} ({last.records})")

        def _on_fail(self, msg: str) -> None:
            self._timer.stop()
            self.run_btn.setEnabled(True)
            QMessageBox.critical(self, "PRISM", msg)

        def _on_done(self, report: RunReport) -> None:
            self._timer.stop()
            self.run_btn.setEnabled(True)
            prod = confidence_product(report)
            self.s_vis.setText(f"visibility {prod['visibility']}")
            self.s_ans.setText(f"answer {prod['answer']}")
            self.s_res.setText(f"resolution {prod['resolution']}")
            self.s_conf.setText(confidence_label(report))
            self.stages.setRowCount(0)
            for s in report.stages:
                r = self.stages.rowCount()
                self.stages.insertRow(r)
                vals = [s.name, s.status, str(s.records), f"{s.elapsed:.2f}", s.detail or s.install]
                for c, v in enumerate(vals):
                    self.stages.setItem(r, c, QTableWidgetItem(str(v) if v is not None else ""))
            self.findings.setRowCount(0)
            for row in finding_rows(report):
                r = self.findings.rowCount()
                self.findings.insertRow(r)
                for c, key in enumerate(FINDING_COLUMNS):
                    item = QTableWidgetItem(row[key])
                    bg = status_background(row["status"])
                    if bg and c == 0:
                        item.setBackground(QColor(bg))
                    self.findings.setItem(r, c, item)
            self.taxonomy.setRowCount(0)
            for tax in taxonomy_rows(report):
                r = self.taxonomy.rowCount()
                self.taxonomy.insertRow(r)
                vals = [tax["id"], tax["verdict"], tax["best"]]
                for c, v in enumerate(vals):
                    item = QTableWidgetItem(str(v) if v is not None else "")
                    bg = taxonomy_background(tax["verdict"])
                    if bg and c == 1:
                        item.setBackground(QColor(bg))
                    self.taxonomy.setItem(r, c, item)
            notrun = [s.name for s in report.stages if s.status == "NOTRUN"]
            self.log.append(f"done. {confidence_label(report)}. NOTRUN={notrun or 'none'}")

    # Publish the lazily-built Qt classes as module attributes (read back by
    # the module-level __getattr__ below).
    globals().update(Worker=Worker, MainWindow=MainWindow, QApplication=QApplication)
    _qt_ready = True


def __getattr__(name: str) -> Any:
    if name in {"MainWindow", "Worker", "QApplication"}:
        _ensure_qt()
        return globals()[name]
    raise AttributeError(f"module {__name__!r} has no attribute {name!r}")


def launch(args=None) -> int:
    try:
        from PySide6.QtWidgets import QApplication
        _ensure_qt()
    except (ImportError, OSError) as ex:
        return _notrun_gui(
            "PySide6 not installed",
            f"install: pip install PySide6  ({ex})",
        )
    try:
        app = QApplication.instance() or QApplication(sys.argv)
    except Exception as ex:
        return _notrun_gui("no display", str(ex))
    try:
        path = Path(getattr(args, "path", "testdata")).resolve()
        w = sys.modules[__name__].MainWindow(
            path, allow_exec=bool(getattr(args, "allow_exec", False)))
        w.show()
        return app.exec()
    except Exception as ex:
        return _notrun_gui("no display", str(ex))
