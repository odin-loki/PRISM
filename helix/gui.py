"""Qt 6 GUI (PySide6). Same pipeline as the CLI. Native C++ GUI lives in native/gui."""

from __future__ import annotations

from pathlib import Path
import sys

from PySide6.QtCore import QObject, Qt, QThread, QTimer, Signal
from PySide6.QtGui import QColor, QFont
from PySide6.QtWidgets import (
    QApplication,
    QFileDialog,
    QHBoxLayout,
    QHeaderView,
    QLabel,
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

from helix.config import Config
from helix.pipeline import STAGE_ORDER, run_pipeline
from helix.models import RunReport
from helix.taxonomy import coverage_from_report
from helix import journal


def skip_from_checks(fuzz: bool, repair: bool, optional: bool) -> list[str]:
    skip: list[str] = []
    if fuzz:
        skip.append("fuzz")
    if repair:
        skip.append("repair")
    if optional:
        skip.append("optional")
    return skip


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


class MainWindow(QMainWindow):
    def __init__(self, path: Path | None = None) -> None:
        super().__init__()
        self.setWindowTitle("Helix — hybrid code testing")
        self.resize(1280, 800)
        self._thread: QThread | None = None
        self._worker: Worker | None = None
        self._out = Path("helix-out")
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
        self.run_btn = QPushButton("Run pipeline")
        self.run_btn.clicked.connect(self._run)
        bar.addWidget(self.path_lbl, 1)
        bar.addWidget(pick)
        bar.addWidget(self.llm_ck)
        bar.addWidget(self.resume_ck)
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
        self.findings = QTableWidget(0, 7)
        self.findings.setHorizontalHeaderLabels(
            ["status", "stage", "file", "line", "function", "class", "message"]
        )
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
        self._load_last()

    def _load_last(self) -> None:
        p = Path("helix-out") / "report.json"
        rec = RunReport.load(p)
        if rec is None:
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
        QMessageBox.critical(self, "Helix", msg)

    def _on_done(self, report: RunReport) -> None:
        self._timer.stop()
        self.run_btn.setEnabled(True)
        self.s_vis.setText(f"visibility {report.visibility}")
        self.s_ans.setText(f"answer {report.answer}")
        self.s_res.setText(f"resolution {report.resolution}")
        self.s_conf.setText(f"confidence {report.confidence}")
        self.stages.setRowCount(0)
        for s in report.stages:
            r = self.stages.rowCount()
            self.stages.insertRow(r)
            vals = [s.name, s.status, str(s.records), f"{s.elapsed:.2f}", s.detail or s.install]
            for c, v in enumerate(vals):
                self.stages.setItem(r, c, QTableWidgetItem(str(v) if v is not None else ""))
        self.findings.setRowCount(0)
        for s in report.stages:
            for f in s.findings:
                if f.status in {"CLEAN"} and s.name in {"inventory", "unify"}:
                    continue
                r = self.findings.rowCount()
                self.findings.insertRow(r)
                vals = [f.status, s.name, f.file, str(f.line or ""),
                        f.function or "", f.cls, f.message[:200]]
                for c, v in enumerate(vals):
                    item = QTableWidgetItem(str(v) if v is not None else "")
                    bg = _STATUS_BG.get(f.status)
                    if bg and c == 0:
                        item.setBackground(QColor(bg))
                    self.findings.setItem(r, c, item)
        self.taxonomy.setRowCount(0)
        for row in coverage_from_report(report):
            r = self.taxonomy.rowCount()
            self.taxonomy.insertRow(r)
            vals = [row["id"], row["verdict"], str(row.get("best") or "")]
            for c, v in enumerate(vals):
                item = QTableWidgetItem(str(v) if v is not None else "")
                if row["verdict"] == "COVERED" and c == 1:
                    item.setBackground(QColor(_STATUS_BG["PROVED"]))
                elif row["verdict"] == "GAP" and c == 1:
                    item.setBackground(QColor(_STATUS_BG["NOTRUN"]))
                self.taxonomy.setItem(r, c, item)
        notrun = [s.name for s in report.stages if s.status == "NOTRUN"]
        self.log.append(f"done. confidence={report.confidence}. NOTRUN={notrun or 'none'}")


def launch(args=None) -> int:
    app = QApplication(sys.argv)
    path = Path(getattr(args, "path", "testdata")).resolve()
    w = MainWindow(path)
    w.show()
    return app.exec()
