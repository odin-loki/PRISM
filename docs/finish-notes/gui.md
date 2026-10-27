# GUI stages slice

Checked `src/gui` at `e59a8b3df` against the first three `prism/gui.py` gaps in
`docs/CPP_PORT_PLAN.md`. That appendix row is the 2026-09-30 audit: it still
says the stages table, the journal poll, and the resume checkbox are missing.
The status block at the top of the same plan already records Qt GUI parity
(the model, the stages table, and the journal poll) as merged. This slice
did not add a second table, timer, or checkbox.

The GUI was not launched. No display, and no `cmake` / `ninja`. The
`--smoke-screenshot` path in `src/gui/main.cpp` was not edited. It still
removes that flag from the window's launch arguments, runs the pipeline
through `MainWindow::runSmoke`, and exits 0 only when the findings table has
rows and the screenshot was written.

## Stages table

`MainWindow` builds a `QTableWidget` named `stages` with columns stage,
status, records, seconds, note (`prism::gui::STAGE_COLUMNS`). `loadReport`
fills it from `report.stages` through `stage_rows`: status is copied as
written, seconds are two decimal places, and the note is the stage detail
or the install hint. The status cell uses the same palette as findings
(CLEAN stays blue). A missing or stale `report.json` leaves confidence at 0
and does not invent a proof. While a run is in flight the same table is
replaced from the journal (below); when the run finishes and a report
exists, the report rows replace the journal rows.

## Live progress

A `QTimer` on the window uses `prism::gui::POLL_MS` (400). It starts after
the `prism` process has started and stops when that process finishes.
Each tick calls `journal_read_stages` on the output directory, keeps this
run's rows (`live_stages`: a row older than the run start is hidden unless
Resume is checked), and writes them into the stages table. The log gets one
line, `<stage> <status> (<records>)`, when the last row changes.

The journal reader is already linked. `prism_gui_window` links `prism_core`,
and `src/prism/journal.cpp` is in that library's source list. Nothing extra
had to be added to the GUI target.

## Resume last report

The checkbox is labeled "Resume last report", object name `resume`, and it
starts unchecked. A launch argument `--resume` checks it; ticking it is what
puts `--resume` on the next `prism` command (`gui_run_args`). That does not
change the CLI default: `prism` still resumes only when the flag is passed.

The run launcher has no `Config` object. It starts the `prism` binary with
`QProcess` and the argument list above. `Config.resume` is therefore not set
on the scan path. The assistant panel builds its own `Config` and sets
`resume` true so `ai_audit.jsonl` is appended; that flag is the audit log,
not this checkbox.

## Left as they are

The same appendix cell continues with an optional-skip checkbox and then
stops at "(+9 more)". The optional checkbox is already on the window
(object name `skip_optional`, default off, passed as `--skip optional`).
The rest of that cell was never stored past the truncation, so it is not
reconstructed here.

The last `prism/gui.py` (`d16a3017`) had two dialogs: the directory picker
("Open…") and a critical box when the run failed. Both are already in
`MainWindow` (the failure box is non-modal, object name `run_failure`, so
an offscreen test can keep running). No remaining Python window behavior
needs a new dialog. Not copied, and not dialogs: the Python window ran the
pipeline in-process on a `Config`; this window runs the `prism` CLI. The
Python default output directory was `prism-out`; this window uses
`prism-out-gui`.
