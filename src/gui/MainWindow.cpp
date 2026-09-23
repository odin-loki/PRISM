#include "MainWindow.h"

#include "prism/models.hpp"
#include "prism/taxonomy.hpp"

#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QFileDialog>
#include <QLabel>
#include <QWidget>
#include <QHeaderView>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QColor>
#include <QBrush>
#include <QStringList>

#include <filesystem>

namespace {

bool takes_value(const QString& a) {
    return a == QLatin1String("--out") || a == QLatin1String("--stage") ||
           a == QLatin1String("--skip") || a == QLatin1String("--unwind") ||
           a == QLatin1String("--fuzz-budget") || a == QLatin1String("--fuzz-iters") ||
           a == QLatin1String("--repair-rounds") || a == QLatin1String("--jobs") ||
           a == QLatin1String("-j") || a == QLatin1String("--tool");
}

struct CliLaunch {
    QString path;
    QString out;
    bool no_llm = false;
    bool skip_fuzz = false;
    bool skip_repair = false;
    bool allow_exec = false;
    QStringList extra_skip;
    QStringList extra;
    bool from_cli = false;
};

CliLaunch parse_cli_launch() {
    CliLaunch c;
    const QStringList argv = QCoreApplication::arguments();
    for (int i = 1; i < argv.size(); ++i) {
        const QString a = argv[i];
        auto next = [&]() -> QString {
            if (i + 1 < argv.size()) return argv[++i];
            return {};
        };
        if (a == QLatin1String("--gui")) {
            c.from_cli = true;
            continue;
        }
        if (a == QLatin1String("--no-llm")) {
            c.no_llm = true;
            c.from_cli = true;
            continue;
        }
        if (a == QLatin1String("--out")) {
            c.out = next();
            c.from_cli = true;
            continue;
        }
        if (a == QLatin1String("--skip")) {
            c.from_cli = true;
            for (const auto& part : next().split(QLatin1Char(','))) {
                const QString t = part.trimmed();
                if (t == QLatin1String("fuzz")) c.skip_fuzz = true;
                else if (t == QLatin1String("repair")) c.skip_repair = true;
                else if (!t.isEmpty()) c.extra_skip << t;
            }
            continue;
        }
        if (takes_value(a)) {
            c.extra << a << next();
            c.from_cli = true;
            continue;
        }
        if (a == QLatin1String("--allow-exec")) {
            c.allow_exec = true;
            c.from_cli = true;
            continue;
        }
        if (a == QLatin1String("--resume")) {
            c.extra << a;
            c.from_cli = true;
            continue;
        }
        if (!a.startsWith(QLatin1Char('-'))) {
            if (c.path.isEmpty()) c.path = a;
            c.from_cli = true;
            continue;
        }
        if (a.startsWith(QLatin1String("-"))) continue;
    }
    return c;
}

QColor findingStatusBg(const QString& status) {
    // Same palette as prism/gui.py. CLEAN is blue, never proof-green.
    if (status == QLatin1String("PROVED-UNBOUNDED")) return QColor(0x1f, 0x6f, 0x3a);
    if (status == QLatin1String("PROVED")) return QColor(0x2a, 0x81, 0x48);
    if (status == QLatin1String("PROVED-ASSUMING")) return QColor(0x3a, 0x7a, 0x4a);
    if (status == QLatin1String("BOUNDED")) return QColor(0x6b, 0x6b, 0x2a);
    if (status == QLatin1String("FAILED")) return QColor(0x8b, 0x2e, 0x2e);
    if (status == QLatin1String("CRASH")) return QColor(0xaa, 0x11, 0x11);
    if (status == QLatin1String("CLEAN")) return QColor(0x2a, 0x4a, 0x6b);
    if (status == QLatin1String("HYPOTHESIS")) return QColor(0x5a, 0x3a, 0x7a);
    if (status == QLatin1String("NOTRUN")) return QColor(0x6b, 0x5a, 0x2a);
    if (status == QLatin1String("ERROR")) return QColor(0x5a, 0x5a, 0x5a);
    if (status == QLatin1String("NEEDS-HARNESS")) return QColor(0x5a, 0x4a, 0x2a);
    if (status == QLatin1String("TIMEOUT") || status == QLatin1String("UNKNOWN"))
        return QColor(0x4a, 0x4a, 0x4a);
    return QColor();
}

QColor taxonomyVerdictBg(const QString& verdict) {
    // COVERED is a class hit (PROVED green). GAP is NOTRUN brown.
    // CLEAN is never COVERED and never a proof.
    if (verdict == QLatin1String("COVERED")) return QColor(0x2a, 0x81, 0x48);
    if (verdict == QLatin1String("GAP")) return QColor(0x6b, 0x5a, 0x2a);
    if (verdict == QLatin1String("PARTIAL")) return QColor(0x6b, 0x6b, 0x2a);
    if (verdict == QLatin1String("CLEAN")) return QColor(0x2a, 0x4a, 0x6b);
    return QColor();
}

void fillTaxonomyTable(QTableWidget* tax, const QJsonArray& arr) {
    tax->setRowCount(0);
    for (const auto& v : arr) {
        const auto o = v.toObject();
        QString verdict = o.value(QStringLiteral("verdict")).toString();
        if (verdict == QLatin1String("CLEAN"))
            verdict = QStringLiteral("GAP");
        const int row = tax->rowCount();
        tax->insertRow(row);
        tax->setItem(row, 0, new QTableWidgetItem(o.value(QStringLiteral("id")).toString()));
        tax->setItem(row, 1, new QTableWidgetItem(verdict));
        tax->setItem(row, 2, new QTableWidgetItem(o.value(QStringLiteral("best")).toString()));
        const auto bg = taxonomyVerdictBg(verdict);
        if (bg.isValid())
            if (auto *it = tax->item(row, 1)) it->setBackground(QBrush(bg));
    }
}

}  // namespace

namespace prism {

MainWindow::MainWindow(QWidget *parent) : QMainWindow(parent) {
    setWindowTitle(QStringLiteral("PRISM"));
    resize(1280, 800);
    auto *root = new QWidget(this);
    setCentralWidget(root);
    auto *v = new QVBoxLayout(root);
    auto *h = new QHBoxLayout();
    path_ = new QLineEdit(QStringLiteral("testdata"));
    auto *browse = new QPushButton(QStringLiteral("Open…"));
    run_ = new QPushButton(QStringLiteral("Run pipeline"));
    no_llm_ = new QCheckBox(QStringLiteral("--no-llm"));
    no_llm_->setChecked(true);
    skip_fuzz_ = new QCheckBox(QStringLiteral("skip fuzz"));
    skip_repair_ = new QCheckBox(QStringLiteral("skip repair"));
    allow_exec_ = new QCheckBox(QStringLiteral("Allow executing scanned code"));
    allow_exec_->setChecked(false);
    allow_exec_->setToolTip(QStringLiteral(
        "--allow-exec: sanitizer/fuzz/diff harnesses, perl -c, cargo clippy, eslint, "
        "ParanoidBSD modules, LLM programs. Only on code you trust; off = NOTRUN."));
    h->addWidget(new QLabel(QStringLiteral("Path")));
    h->addWidget(path_, 1);
    h->addWidget(browse);
    h->addWidget(no_llm_);
    h->addWidget(skip_fuzz_);
    h->addWidget(skip_repair_);
    h->addWidget(allow_exec_);
    h->addWidget(run_);
    v->addLayout(h);
    auto *stats = new QHBoxLayout();
    vis_ = new QLabel(QStringLiteral("visibility 0"));
    ans_ = new QLabel(QStringLiteral("answer 0"));
    res_ = new QLabel(QStringLiteral("resolution 0"));
    conf_ = new QLabel(QStringLiteral("confidence 0  (vis 0 x ans 0 x res 0)"));
    stats->addWidget(vis_);
    stats->addWidget(ans_);
    stats->addWidget(res_);
    stats->addWidget(conf_, 1);
    v->addLayout(stats);
    log_ = new QPlainTextEdit;
    log_->setReadOnly(true);
    findings_ = new QTableWidget(0, 7);
    findings_->setHorizontalHeaderLabels({
        QStringLiteral("status"), QStringLiteral("stage"), QStringLiteral("cls"),
        QStringLiteral("file"), QStringLiteral("line"), QStringLiteral("function"),
        QStringLiteral("message")});
    findings_->horizontalHeader()->setStretchLastSection(true);
    tax_ = new QTableWidget(0, 3);
    tax_->setHorizontalHeaderLabels({QStringLiteral("class"), QStringLiteral("verdict"), QStringLiteral("best")});
    tax_->horizontalHeader()->setStretchLastSection(true);
    v->addWidget(log_, 2);
    v->addWidget(findings_, 2);
    v->addWidget(tax_, 1);
    proc_ = new QProcess(this);
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
    out_dir_ = QDir::current().absoluteFilePath(QStringLiteral("prism-out-gui"));
    const auto cli = parse_cli_launch();
    if (!cli.path.isEmpty())
        path_->setText(QFileInfo(cli.path).absoluteFilePath());
    if (!cli.out.isEmpty())
        out_dir_ = QDir::current().absoluteFilePath(cli.out);
    if (cli.from_cli)
        no_llm_->setChecked(cli.no_llm);
    if (cli.skip_fuzz) skip_fuzz_->setChecked(true);
    if (cli.skip_repair) skip_repair_->setChecked(true);
    if (cli.allow_exec) allow_exec_->setChecked(true);
    if (QFileInfo::exists(out_dir_ + QStringLiteral("/report.json")))
        loadSameReport(out_dir_);
}

QString MainWindow::prismBinary() const {
    const QString dir = QCoreApplication::applicationDirPath();
    QString cand = dir + QStringLiteral("/prism");
#ifdef Q_OS_WIN
    cand += QStringLiteral(".exe");
#endif
    if (QFileInfo::exists(cand)) return cand;
    return QStringLiteral("prism");
}

void MainWindow::onRun() {
    run_->setEnabled(false);
    const auto cli = parse_cli_launch();
    if (!cli.out.isEmpty())
        out_dir_ = QDir::current().absoluteFilePath(cli.out);
    else
        out_dir_ = QDir::current().absoluteFilePath(QStringLiteral("prism-out-gui"));
    log_->appendPlainText(QStringLiteral("running prism …"));
    proc_->setProgram(prismBinary());
    QStringList args{path_->text(), QStringLiteral("--out"), out_dir_};
    if (no_llm_->isChecked()) args << QStringLiteral("--no-llm");
    if (allow_exec_->isChecked()) args << QStringLiteral("--allow-exec");
    QStringList skip;
    if (skip_fuzz_->isChecked()) skip << QStringLiteral("fuzz");
    if (skip_repair_->isChecked()) skip << QStringLiteral("repair");
    skip << cli.extra_skip;
    if (!skip.isEmpty()) args << QStringLiteral("--skip") << skip.join(QLatin1Char(','));
    args << cli.extra;
    proc_->setArguments(args);
    proc_->start();
    if (!proc_->waitForStarted(4000)) {
        run_->setEnabled(true);
        log_->appendPlainText(QStringLiteral("NOTRUN gui: prism binary not found — not a clean window"));
        log_->appendPlainText(QStringLiteral("  install: build prism with WSL clang++ (never MinGW)"));
    }
}

void MainWindow::loadSameReport(const QString& outDir) {
    QJsonArray taxArr;
    QFile reportFile(outDir + QStringLiteral("/report.json"));
    if (reportFile.open(QIODevice::ReadOnly)) {
        const auto doc = QJsonDocument::fromJson(reportFile.readAll());
        const auto obj = doc.object();
        const double vis = obj.value(QStringLiteral("visibility")).toDouble();
        const double ans = obj.value(QStringLiteral("answer")).toDouble();
        const double res = obj.value(QStringLiteral("resolution")).toDouble();
        const double conf = obj.value(QStringLiteral("confidence")).toDouble();
        vis_->setText(QStringLiteral("visibility %1").arg(vis));
        ans_->setText(QStringLiteral("answer %1").arg(ans));
        res_->setText(QStringLiteral("resolution %1").arg(res));
        conf_->setText(QStringLiteral("confidence %1  (vis %2 x ans %3 x res %4)")
                           .arg(conf).arg(vis).arg(ans).arg(res));
        findings_->setRowCount(0);
        const auto stages = obj.value(QStringLiteral("stages")).toArray();
        for (const auto& sVal : stages) {
            const auto s = sVal.toObject();
            const auto name = s.value(QStringLiteral("name")).toString();
            const auto recs = s.value(QStringLiteral("findings")).toArray();
            for (const auto& fVal : recs) {
                const auto f = fVal.toObject();
                const auto status = f.value(QStringLiteral("status")).toString();
                if ((status == QLatin1String("NOTRUN") || status == QLatin1String("CLEAN")) &&
                    (name == QLatin1String("inventory") || name == QLatin1String("classify") ||
                     name == QLatin1String("unify")))
                    continue;
                const int row = findings_->rowCount();
                findings_->insertRow(row);
                const QStringList cols{
                    status,
                    f.value(QStringLiteral("stage")).toString(name),
                    f.value(QStringLiteral("cls")).toString(),
                    f.value(QStringLiteral("file")).toString(),
                    f.value(QStringLiteral("line")).isNull()
                        ? QString()
                        : QString::number(f.value(QStringLiteral("line")).toInt()),
                    f.value(QStringLiteral("function")).toString(),
                    f.value(QStringLiteral("message")).toString().left(200),
                };
                for (int c = 0; c < cols.size(); ++c) {
                    auto *item = new QTableWidgetItem(cols[c]);
                    if (c == 0) {
                        const auto bg = findingStatusBg(status);
                        if (bg.isValid()) item->setBackground(QBrush(bg));
                    }
                    findings_->setItem(row, c, item);
                }
            }
        }
        taxArr = obj.value(QStringLiteral("taxonomy")).toArray();
        // Python engine recomputes COVERED/GAP from report.json findings. report.json
        // has no taxonomy key; a CLEAN unify line is never COVERED.
        const auto loaded = prism::RunReport::load(
            std::filesystem::path(outDir.toStdString()) / "report.json");
        if (loaded) {
            taxArr = QJsonArray();
            for (const auto& r : prism::coverage_from_report(*loaded)) {
                QJsonObject row;
                row.insert(QStringLiteral("id"), QString::fromStdString(r.id));
                std::string verdict_s = r.verdict;
                if (verdict_s == "CLEAN" ||
                    (verdict_s != "COVERED" && verdict_s != "PARTIAL" && verdict_s != "GAP"))
                    verdict_s = "GAP";
                verdict_s = prism::refuse_llm_cover(*loaded, r.id, verdict_s, r.best);
                row.insert(QStringLiteral("verdict"), QString::fromStdString(verdict_s));
                row.insert(QStringLiteral("best"), QString::fromStdString(r.best));
                taxArr.append(row);
            }
        }
    } else {
        vis_->setText(QStringLiteral("visibility 0"));
        ans_->setText(QStringLiteral("answer 0"));
        res_->setText(QStringLiteral("resolution 0"));
        conf_->setText(QStringLiteral("confidence 0  (vis 0 x ans 0 x res 0) — report.json missing; not a proof"));
    }

    // Do not fall back to a stale taxonomy.json (COVERED can linger after
    // findings were recomputed as GAP).
    if (!taxArr.isEmpty())
        fillTaxonomyTable(tax_, taxArr);
    else
        tax_->setRowCount(0);
}

void MainWindow::onDone(int exitCode, QProcess::ExitStatus) {
    run_->setEnabled(true);
    log_->appendPlainText(QStringLiteral("exit %1").arg(exitCode));
    if (!out_dir_.isEmpty()) loadSameReport(out_dir_);
}

}  // namespace prism
