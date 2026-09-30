#include "MainWindow.h"

#include <cstdio>

#include "prism/ai.hpp"
#include "prism/ai_assist.hpp"
#include "prism/config.hpp"
#include "prism/journal.hpp"
#include "prism/models.hpp"

#include <QBrush>
#include <QColor>
#include <QCoreApplication>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QFont>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QMessageBox>
#include <QSplitter>
#include <QVBoxLayout>
#include <QWidget>

#include <chrono>
#include <filesystem>
#include <optional>

namespace {

namespace pg = prism::gui;

QString qs(std::string_view s) {
    return QString::fromUtf8(s.data(), static_cast<qsizetype>(s.size()));
}

std::filesystem::path fs_path(const QString& s) {
    return std::filesystem::path(s.toStdU16String());
}

double now_secs() {
    using clock = std::chrono::system_clock;
    return std::chrono::duration<double>(clock::now().time_since_epoch()).count();
}

QStringList header(const auto& columns) {
    QStringList h;
    for (auto c : columns) h << qs(c);
    return h;
}

QTableWidget* table(const char* name, const QStringList& columns) {
    auto *t = new QTableWidget(0, static_cast<int>(columns.size()));
    t->setObjectName(QLatin1String(name));
    t->setHorizontalHeaderLabels(columns);
    t->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    return t;
}

void setRow(QTableWidget* t, int row, const std::vector<std::string>& cells) {
    for (int c = 0; c < static_cast<int>(cells.size()); ++c)
        t->setItem(row, c, new QTableWidgetItem(qs(cells[c])));
}

void paint(QTableWidget* t, int row, int col, const std::string& hex) {
    if (hex.empty()) return;
    if (auto *it = t->item(row, col)) it->setBackground(QBrush(QColor(qs(hex))));
}

}  // namespace

namespace prism {

MainWindow::MainWindow(const QStringList& launch, QWidget *parent) : QMainWindow(parent) {
    std::vector<std::string> args;
    for (const auto& a : launch) args.push_back(a.toStdString());
    launch_ = pg::parse_launch(args);

    setWindowTitle(qs(pg::WINDOW_TITLE));
    resize(1280, 800);
    auto *root = new QWidget(this);
    setCentralWidget(root);
    auto *v = new QVBoxLayout(root);

    auto *bar = new QHBoxLayout();
    path_ = new QLineEdit(QStringLiteral("testdata"));
    path_->setObjectName(QStringLiteral("path"));
    auto *browse = new QPushButton(QStringLiteral("Open…"));
    // The LLM stage is on by default: its output is HYPOTHESIS/READS and
    // never covers a class (Law 4); without a model it is NOTRUN.
    llm_ = new QCheckBox(QStringLiteral("LLM hypotheses"));
    llm_->setObjectName(QStringLiteral("llm"));
    llm_->setChecked(true);
    resume_ = new QCheckBox(QStringLiteral("Resume last report"));
    resume_->setObjectName(QStringLiteral("resume"));
    allow_exec_ = new QCheckBox(QStringLiteral("Allow executing scanned code"));
    allow_exec_->setObjectName(QStringLiteral("allow_exec"));
    allow_exec_->setChecked(false);
    allow_exec_->setToolTip(QStringLiteral(
        "--allow-exec: sanitizer/fuzz/diff harnesses, perl -c, cargo clippy, eslint, "
        "ParanoidBSD modules, LLM programs. Only on code you trust; off = NOTRUN."));
    run_ = new QPushButton(QStringLiteral("Run pipeline"));
    run_->setObjectName(QStringLiteral("run"));
    bar->addWidget(new QLabel(QStringLiteral("Path")));
    bar->addWidget(path_, 1);
    bar->addWidget(browse);
    bar->addWidget(llm_);
    bar->addWidget(resume_);
    bar->addWidget(allow_exec_);
    bar->addWidget(run_);
    v->addLayout(bar);

    auto *skipBar = new QHBoxLayout();
    skipBar->addWidget(new QLabel(QStringLiteral("Skip:")));
    skip_fuzz_ = new QCheckBox(QStringLiteral("fuzz"));
    skip_fuzz_->setObjectName(QStringLiteral("skip_fuzz"));
    skip_repair_ = new QCheckBox(QStringLiteral("repair"));
    skip_repair_->setObjectName(QStringLiteral("skip_repair"));
    skip_optional_ = new QCheckBox(QStringLiteral("optional"));
    skip_optional_->setObjectName(QStringLiteral("skip_optional"));
    skipBar->addWidget(skip_fuzz_);
    skipBar->addWidget(skip_repair_);
    skipBar->addWidget(skip_optional_);
    skipBar->addStretch(1);
    v->addLayout(skipBar);

    auto *stats = new QHBoxLayout();
    stats->setSpacing(24);
    vis_ = new QLabel;
    ans_ = new QLabel;
    res_ = new QLabel;
    conf_ = new QLabel;
    vis_->setObjectName(QStringLiteral("s_vis"));
    ans_->setObjectName(QStringLiteral("s_ans"));
    res_->setObjectName(QStringLiteral("s_res"));
    conf_->setObjectName(QStringLiteral("s_conf"));
    QFont bold = font();
    bold.setPointSize(11);
    bold.setBold(true);
    for (auto *l : {vis_, ans_, res_, conf_}) {
        l->setFont(bold);
        stats->addWidget(l, l == conf_ ? 1 : 0);
    }
    v->addLayout(stats);

    stages_ = table("stages", header(pg::STAGE_COLUMNS));
    findings_ = table("findings", header(pg::FINDING_COLUMNS));
    tax_ = table("taxonomy", {QStringLiteral("class"), QStringLiteral("verdict"),
                              QStringLiteral("best")});
    auto *split = new QSplitter(Qt::Vertical);
    split->addWidget(stages_);
    split->addWidget(findings_);
    split->addWidget(tax_);
    split->setStretchFactor(1, 2);
    v->addWidget(split, 3);

    log_ = new QPlainTextEdit;
    log_->setObjectName(QStringLiteral("log"));
    log_->setReadOnly(true);
    v->addWidget(log_, 1);

    // Assistant (roadmap 9.4): "unproved memory safety in module net",
    // "explain bmc#3", "trusted base". Double-click a finding to explain it.
    auto *chatRow = new QHBoxLayout();
    ask_ = new QLineEdit;
    ask_->setObjectName(QStringLiteral("ask"));
    ask_->setPlaceholderText(QStringLiteral("Ask about findings, or: explain <id> / trusted base"));
    ask_btn_ = new QPushButton(QStringLiteral("Ask"));
    chatRow->addWidget(new QLabel(QStringLiteral("Assistant")));
    chatRow->addWidget(ask_, 1);
    chatRow->addWidget(ask_btn_);
    v->addLayout(chatRow);
    chat_ = new QPlainTextEdit;
    chat_->setObjectName(QStringLiteral("chat"));
    chat_->setReadOnly(true);
    v->addWidget(chat_, 1);
    connect(ask_btn_, &QPushButton::clicked, this, &MainWindow::onAsk);
    connect(ask_, &QLineEdit::returnPressed, this, &MainWindow::onAsk);
    connect(findings_, &QTableWidget::cellDoubleClicked, this, [this](int row, int) {
        if (auto *it = findings_->item(row, 0)) {
            ask_->setText(QStringLiteral("explain ") + it->data(Qt::UserRole).toString());
            onAsk();
        }
    });

    proc_ = new QProcess(this);
    poll_ = new QTimer(this);
    poll_->setInterval(pg::POLL_MS);
    connect(poll_, &QTimer::timeout, this, &MainWindow::pollJournal);
    connect(browse, &QPushButton::clicked, this, [this] {
        auto d = QFileDialog::getExistingDirectory(this, QStringLiteral("Code"), path_->text());
        if (!d.isEmpty()) path_->setText(d);
    });
    connect(run_, &QPushButton::clicked, this, &MainWindow::onRun);
    connect(proc_, qOverload<int, QProcess::ExitStatus>(&QProcess::finished),
            this, &MainWindow::onDone);
    connect(proc_, &QProcess::readyReadStandardOutput, this, [this] {
        log_->appendPlainText(QString::fromUtf8(proc_->readAllStandardOutput()));
    });
    connect(proc_, &QProcess::readyReadStandardError, this, [this] {
        log_->appendPlainText(QString::fromUtf8(proc_->readAllStandardError()));
    });

    // Launch flags set their boxes; the rest ride along to the CLI.
    if (!launch_.path.empty())
        path_->setText(QFileInfo(qs(launch_.path)).absoluteFilePath());
    if (launch_.no_llm) llm_->setChecked(false);
    resume_->setChecked(launch_.resume);
    allow_exec_->setChecked(launch_.allow_exec);
    skip_fuzz_->setChecked(launch_.skip_fuzz);
    skip_repair_->setChecked(launch_.skip_repair);
    skip_optional_->setChecked(launch_.skip_optional);
    for (const auto& ig : launch_.ignored)
        log_->appendPlainText(QStringLiteral("ignored launch flag (not a scan): ") + qs(ig));
    out_dir_ = QDir::current().absoluteFilePath(
        qs(launch_.out.empty() ? std::string(pg::DEFAULT_OUT) : launch_.out));
    loadReport(out_dir_);
}

QString MainWindow::prismBinary() const {
    if (!program_.isEmpty()) return program_;
    const QString dir = QCoreApplication::applicationDirPath();
    QString cand = dir + QStringLiteral("/prism");
#ifdef Q_OS_WIN
    cand += QStringLiteral(".exe");
#endif
    if (QFileInfo::exists(cand)) return cand;
    return QStringLiteral("prism");
}

QStringList MainWindow::runArguments() const {
    pg::RunOptions o;
    o.path = path_->text().toStdString();
    o.out = out_dir_.toStdString();
    o.llm = llm_->isChecked();
    o.resume = resume_->isChecked();
    o.allow_exec = allow_exec_->isChecked();
    o.skip_fuzz = skip_fuzz_->isChecked();
    o.skip_repair = skip_repair_->isChecked();
    o.skip_optional = skip_optional_->isChecked();
    o.extra_skip = launch_.extra_skip;
    o.extra = launch_.extra;
    QStringList args;
    for (const auto& a : pg::gui_run_args(o)) args << qs(a);
    return args;
}

void MainWindow::onRun() {
    run_->setEnabled(false);
    const QStringList args = runArguments();
    log_->appendPlainText(QStringLiteral("running prism ") + args.join(QLatin1Char(' ')));
    run_started_ = now_secs();
    run_resume_ = resume_->isChecked();
    progress_.reset();
    stages_->setRowCount(0);
    proc_->setProgram(prismBinary());
    proc_->setArguments(args);
    proc_->start();
    if (!proc_->waitForStarted(4000)) {
        run_->setEnabled(true);
        log_->appendPlainText(QStringLiteral("NOTRUN gui: prism binary not found — not a clean window"));
        log_->appendPlainText(QStringLiteral("  install: build prism with WSL clang++ (never MinGW)"));
        return;
    }
    poll_->start();
}

void MainWindow::fillStages(const std::vector<StageResult>& stages) {
    stages_->setRowCount(0);
    for (const auto& r : pg::stage_rows(stages)) {
        const int row = stages_->rowCount();
        stages_->insertRow(row);
        setRow(stages_, row, {r.stage, r.status, r.records, r.seconds, r.note});
        paint(stages_, row, 1, pg::status_background(r.status));
    }
}

void MainWindow::pollJournal() {
    const auto rows = pg::live_stages(prism::journal_read_stages(fs_path(out_dir_)),
                                      run_started_, run_resume_);
    if (rows.empty()) return;
    fillStages(rows);
    if (auto line = progress_.update(rows)) log_->appendPlainText(qs(*line));
}

void MainWindow::loadReport(const QString& outDir, double notBefore) {
    findings_->setRowCount(0);
    tax_->setRowCount(0);
    const QString file = outDir + QStringLiteral("/report.json");
    std::optional<RunReport> report;
    // A report.json older than this run is the previous run's result: the
    // window must not present it as this run's verdict.
    const QFileInfo info(file);
    const bool stale = notBefore > 0 && info.exists() &&
                       info.lastModified().toMSecsSinceEpoch() / 1000.0 < notBefore - 1.0;
    if (stale)
        log_->appendPlainText(QStringLiteral("report.json predates this run; not shown"));
    else
        report = RunReport::load(fs_path(file));
    if (!report) {
        // Empty scope is 0, never n/a, never a proof, never CLEAN.
        vis_->setText(QStringLiteral("visibility 0"));
        ans_->setText(QStringLiteral("answer 0"));
        res_->setText(QStringLiteral("resolution 0"));
        conf_->setText(qs(pg::MISSING_REPORT_LABEL));
        return;
    }
    const auto c = pg::confidence_product(*report);
    vis_->setText(QStringLiteral("visibility ") + qs(pg::format_number(c.visibility)));
    ans_->setText(QStringLiteral("answer ") + qs(pg::format_number(c.answer)));
    res_->setText(QStringLiteral("resolution ") + qs(pg::format_number(c.resolution)));
    conf_->setText(qs(pg::confidence_label(c)));
    fillStages(report->stages);
    for (const auto& f : pg::finding_rows(*report)) {
        const int row = findings_->rowCount();
        findings_->insertRow(row);
        std::vector<std::string> cells;
        for (auto col : pg::FINDING_COLUMNS) cells.push_back(f.column(col));
        setRow(findings_, row, cells);
        // Status is copied verbatim from report.json; the colour follows it.
        paint(findings_, row, 0, pg::status_background(f.status));
        findings_->item(row, 0)->setData(Qt::UserRole, qs(f.id));
    }
    // COVERED / PARTIAL / GAP recomputed from the findings, never a stale
    // taxonomy.json; the LLM cannot cover a class.
    for (const auto& t : pg::taxonomy_rows(*report)) {
        const int row = tax_->rowCount();
        tax_->insertRow(row);
        setRow(tax_, row, {t.id, t.verdict, t.best});
        paint(tax_, row, 1, pg::taxonomy_background(t.verdict));
    }
}

void MainWindow::onAsk() {
    const QString q = ask_->text().trimmed();
    if (q.isEmpty()) return;
    chat_->appendPlainText(QStringLiteral("> ") + q);
    const auto path = fs_path(out_dir_) / "report.json";
    const auto report = prism::RunReport::load(path);
    if (!report) {
        chat_->appendPlainText(QStringLiteral("NOTRUN assistant: no report.json in ") + out_dir_ +
                               QStringLiteral(" (run the pipeline first)"));
        return;
    }
    // A session so a model (when reachable and the LLM box is on) can translate
    // the question; it appends to ai_audit.jsonl and never truncates it.
    prism::Config cfg = prism::default_config();
    cfg.out = fs_path(out_dir_);
    cfg.root = report->root;
    cfg.resume = true;
    cfg.llm = llm_->isChecked();
    prism::ai::Session session(cfg);
    const auto reply = prism::ai::assistant_reply(*report, q.toStdString(), cfg.llm);
    chat_->appendPlainText(QString::fromStdString(reply));
}

void MainWindow::runSmoke(const QString& screenshot) {
    // Connected after onDone (constructor), so the report is loaded first.
    connect(proc_, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this,
            [this, screenshot](int, QProcess::ExitStatus) {
                const int rows = findings_->rowCount();
                const bool saved = grab().save(screenshot);
                std::fprintf(stderr, "gui smoke: %d finding rows, screenshot %s\n", rows,
                             saved ? "saved" : "NOT saved");
                QCoreApplication::exit(rows > 0 && saved ? 0 : 3);
            });
    onRun();
    if (proc_->state() == QProcess::NotRunning) QCoreApplication::exit(2);  // prism not found: NOTRUN
}

void MainWindow::onDone(int exitCode, QProcess::ExitStatus st) {
    poll_->stop();
    pollJournal();
    run_->setEnabled(true);
    loadReport(out_dir_, run_started_);
    const auto report = RunReport::load(fs_path(out_dir_) / "report.json");
    const bool crashed = st == QProcess::CrashExit;
    if (auto why = pg::run_failure(exitCode, crashed)) {
        log_->appendPlainText(qs(*why));
        // Not modal-blocking: the window stays live (and testable offscreen).
        auto *box = new QMessageBox(QMessageBox::Critical, QStringLiteral("PRISM"), qs(*why),
                                    QMessageBox::Ok, this);
        box->setObjectName(QStringLiteral("run_failure"));
        box->setAttribute(Qt::WA_DeleteOnClose);
        box->open();
        return;
    }
    if (!report || conf_->text() == qs(pg::MISSING_REPORT_LABEL)) {
        log_->appendPlainText(QStringLiteral("done. ") + qs(pg::MISSING_REPORT_LABEL));
        return;
    }
    log_->appendPlainText(qs(pg::done_summary(pg::confidence_product(*report), report->stages)));
}

}  // namespace prism
