// The PRISM window (src/gui) offscreen: QT_QPA_PLATFORM=offscreen, no
// display needed. Built only with PRISM_QT=ON (target prism_gui_tests).
//
// The window must show exactly what report.json says: PROVED-UNBOUNDED stays
// a proof, CLEAN fuzz is not one (blue, never proof-green), a missing tool is
// NOTRUN, an LLM hypothesis never covers a class, a missing report is
// confidence 0. Runs are the `prism` CLI; its live journal feeds the stage
// table, a failed run raises a dialog, a finished run logs the NOTRUN list.

#define DOCTEST_CONFIG_IMPLEMENT
#include <doctest/doctest.h>
#ifdef ERROR
#  undef ERROR
#endif

#include "MainWindow.h"

#include "prism/gui_model.hpp"
#include "prism/journal.hpp"
#include "prism/laws.hpp"
#include "prism/models.hpp"

#include <QApplication>
#include <QCheckBox>
#include <QColor>
#include <QDir>
#include <QElapsedTimer>
#include <QLabel>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QTableWidget>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <set>
#include <string>
#include <vector>

#ifndef _WIN32
#  include <sys/stat.h>
#  include <unistd.h>
#endif

namespace fs = std::filesystem;
namespace pg = prism::gui;
namespace laws = prism::laws;

namespace {

std::string S(std::string_view v) { return std::string(v); }
QString Q(std::string_view v) { return QString::fromUtf8(v.data(), static_cast<qsizetype>(v.size())); }

prism::Finding finding(const std::string& stage, std::string_view status, const std::string& cls,
                       const std::string& message, const std::string& file = "a.c",
                       std::optional<std::string> function = std::string("add"),
                       std::optional<int> line = 3) {
    prism::Finding f;
    f.stage = stage;
    f.status = S(status);
    f.file = file;
    f.function = function;
    f.line = line;
    f.cls = cls;
    f.message = message;
    f.strength = laws::is_proof(status) ? S(laws::STRENGTH_PROVES)
                 : status == laws::NOTRUN ? S(laws::STRENGTH_READS)
                                          : S(laws::STRENGTH_FINDS);
    return f;
}

prism::StageResult stage(const std::string& name, const std::string& status,
                         std::vector<prism::Finding> fs, int records = 0) {
    prism::StageResult s;
    s.name = name;
    s.status = status;
    s.findings = std::move(fs);
    s.records = records;
    return s;
}

prism::RunReport mixed_report() {
    prism::RunReport r;
    r.root = "mem";
    r.visibility = 1.0;
    r.answer = 0.5;
    r.resolution = 0.5;
    r.confidence = 0.25;
    r.stages = {
        stage("bmc", "ok", {finding("bmc", laws::PROVED_UNBOUNDED, "INT-SIGNED-OVF",
                                    "k-induction closed; unbounded")}, 1),
        stage("fuzz", "ok", {finding("fuzz", laws::CLEAN, "", "no crash (not a proof)")}, 1),
        stage("esbmc", "NOTRUN",
              {finding("esbmc", laws::NOTRUN, "", "esbmc not on PATH", "", std::nullopt, std::nullopt)}),
    };
    return r;
}

struct TempDir {
    fs::path path;
    QString old_cwd;
    explicit TempDir(const std::string& tag) {
        path = fs::temp_directory_path() / ("prism-guiw-" + tag + "-" + std::to_string(QCoreApplication::applicationPid()));
        fs::remove_all(path);
        fs::create_directories(path);
        old_cwd = QDir::currentPath();
        QDir::setCurrent(QString::fromStdString(path.string()));
    }
    ~TempDir() {
        QDir::setCurrent(old_cwd);
        std::error_code ec;
        fs::remove_all(path, ec);
    }
};

template <class T>
T* child(prism::MainWindow& w, const char* name) {
    auto* c = w.findChild<T*>(QLatin1String(name));
    REQUIRE_MESSAGE(c != nullptr, name);
    return c;
}

std::vector<std::vector<QString>> cells(QTableWidget* t) {
    std::vector<std::vector<QString>> rows;
    for (int r = 0; r < t->rowCount(); ++r) {
        std::vector<QString> row;
        for (int c = 0; c < t->columnCount(); ++c)
            row.push_back(t->item(r, c) ? t->item(r, c)->text() : QString());
        rows.push_back(row);
    }
    return rows;
}

QStringList headers(QTableWidget* t) {
    QStringList h;
    for (int c = 0; c < t->columnCount(); ++c) h << t->horizontalHeaderItem(c)->text();
    return h;
}

QColor bg(QTableWidget* t, int r, int c) {
    auto* it = t->item(r, c);
    return it ? it->background().color() : QColor();
}

QColor hex(std::string_view status) { return QColor(Q(pg::status_background(status))); }

// Spin the event loop until `done` or the timeout (ms).
template <class F>
bool wait_for(F done, int ms) {
    QElapsedTimer t;
    t.start();
    while (!done() && t.elapsed() < ms) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
    }
    return done();
}

fs::path repo_root() { return fs::path(__FILE__).parent_path().parent_path().parent_path(); }

}  // namespace

TEST_SUITE("gui window") {

TEST_CASE("window constructs with safe defaults and the missing-report label") {
    TempDir td("ctor");
    prism::MainWindow w;
    CHECK_FALSE(w.isVisible());
    CHECK(w.windowTitle() == Q(pg::WINDOW_TITLE));
    CHECK_FALSE(child<QCheckBox>(w, "skip_fuzz")->isChecked());
    CHECK_FALSE(child<QCheckBox>(w, "skip_repair")->isChecked());
    CHECK_FALSE(child<QCheckBox>(w, "skip_optional")->isChecked());
    CHECK_FALSE(child<QCheckBox>(w, "resume")->isChecked());
    CHECK(child<QCheckBox>(w, "llm")->isChecked());
    // Law 9: executing scanned code is opt-in.
    auto* ax = child<QCheckBox>(w, "allow_exec");
    CHECK_FALSE(ax->isChecked());
    CHECK(ax->text() == QStringLiteral("Allow executing scanned code"));

    QStringList fc, sc;
    for (auto c : pg::FINDING_COLUMNS) fc << Q(c);
    for (auto c : pg::STAGE_COLUMNS) sc << Q(c);
    CHECK(headers(child<QTableWidget>(w, "findings")) == fc);
    CHECK(headers(child<QTableWidget>(w, "stages")) == sc);
    CHECK(headers(child<QTableWidget>(w, "taxonomy")) ==
          QStringList{QStringLiteral("class"), QStringLiteral("verdict"), QStringLiteral("best")});

    const auto conf = child<QLabel>(w, "s_conf")->text();
    CHECK(conf == Q(pg::MISSING_REPORT_LABEL));
    CHECK_FALSE(conf.toLower().contains(QStringLiteral("n/a")));
    CHECK_FALSE(conf.contains(QStringLiteral("CLEAN")));
    CHECK(child<QLabel>(w, "s_vis")->text() == QStringLiteral("visibility 0"));
    CHECK(child<QTableWidget>(w, "findings")->rowCount() == 0);
    CHECK(child<QTableWidget>(w, "taxonomy")->rowCount() == 0);

    // Default run: prism-out-gui, GUI budgets, no --allow-exec, LLM on.
    const auto args = w.runArguments();
    CHECK(w.outDir() == QDir::current().absoluteFilePath(QStringLiteral("prism-out-gui")));
    CHECK(args.at(args.indexOf(QStringLiteral("--out")) + 1) == w.outDir());
    CHECK_FALSE(args.contains(QStringLiteral("--allow-exec")));
    CHECK_FALSE(args.contains(QStringLiteral("--no-llm")));
    CHECK(args.at(args.indexOf(QStringLiteral("--fuzz-budget")) + 1) == QStringLiteral("4"));
    CHECK(args.at(args.indexOf(QStringLiteral("--fuzz-iters")) + 1) == QStringLiteral("256"));
    CHECK(args.at(args.indexOf(QStringLiteral("--repair-rounds")) + 1) == QStringLiteral("1"));

    // Ticking the boxes changes the run.
    child<QCheckBox>(w, "skip_optional")->setChecked(true);
    child<QCheckBox>(w, "resume")->setChecked(true);
    child<QCheckBox>(w, "llm")->setChecked(false);
    const auto args2 = w.runArguments();
    CHECK(args2.at(args2.indexOf(QStringLiteral("--skip")) + 1) == QStringLiteral("optional"));
    CHECK(args2.contains(QStringLiteral("--resume")));
    CHECK(args2.contains(QStringLiteral("--no-llm")));
}

TEST_CASE("launch flags set the boxes and ride along to the CLI") {
    TempDir td("launch");
    prism::MainWindow w({QStringLiteral("src"), QStringLiteral("--no-llm"), QStringLiteral("--skip"),
                         QStringLiteral("fuzz,optional,bmc"), QStringLiteral("--allow-exec"),
                         QStringLiteral("--resume"), QStringLiteral("--pir-drafts"),
                         QStringLiteral("--unwind"), QStringLiteral("4"), QStringLiteral("--out"),
                         QStringLiteral("o"), QStringLiteral("--version")});
    CHECK_FALSE(child<QCheckBox>(w, "llm")->isChecked());
    CHECK(child<QCheckBox>(w, "skip_fuzz")->isChecked());
    CHECK_FALSE(child<QCheckBox>(w, "skip_repair")->isChecked());
    CHECK(child<QCheckBox>(w, "skip_optional")->isChecked());
    CHECK(child<QCheckBox>(w, "allow_exec")->isChecked());
    CHECK(child<QCheckBox>(w, "resume")->isChecked());
    CHECK(child<QLineEdit>(w, "path")->text() == QDir::current().absoluteFilePath(QStringLiteral("src")));
    CHECK(w.outDir() == QDir::current().absoluteFilePath(QStringLiteral("o")));
    const auto args = w.runArguments();
    CHECK(args.at(args.indexOf(QStringLiteral("--skip")) + 1) == QStringLiteral("fuzz,optional,bmc"));
    CHECK(args.contains(QStringLiteral("--pir-drafts")));
    CHECK(args.at(args.indexOf(QStringLiteral("--unwind")) + 1) == QStringLiteral("4"));
    CHECK(args.contains(QStringLiteral("--allow-exec")));
    CHECK_FALSE(args.contains(QStringLiteral("--version")));
    CHECK(child<QPlainTextEdit>(w, "log")->toPlainText().contains(QStringLiteral("--version")));
}

TEST_CASE("offscreen window shows the report's statuses verbatim") {
    TempDir td("mixed");
    auto report = mixed_report();
    report.save(td.path / "prism-out-gui" / "report.json");
    prism::MainWindow w;  // loads prism-out-gui/report.json at construction
    auto* findings = child<QTableWidget>(w, "findings");
    const auto expected = pg::finding_rows(report);
    const auto rows = cells(findings);
    REQUIRE(rows.size() == expected.size());
    for (std::size_t r = 0; r < rows.size(); ++r)
        for (std::size_t c = 0; c < pg::FINDING_COLUMNS.size(); ++c)
            CHECK(rows[r][c] == Q(expected[r].column(pg::FINDING_COLUMNS[c])));
    CHECK(rows[0][0] == QStringLiteral("PROVED-UNBOUNDED"));
    CHECK(rows[1][0] == QStringLiteral("CLEAN"));
    CHECK(rows[2][0] == QStringLiteral("NOTRUN"));
    CHECK(rows[2][1] == QStringLiteral("esbmc"));
    CHECK(findings->item(0, 0)->data(Qt::UserRole).toString() == QStringLiteral("bmc#0"));
    CHECK(child<QLabel>(w, "s_conf")->text() == Q(pg::confidence_label(report)));
    CHECK(child<QLabel>(w, "s_vis")->text() == QStringLiteral("visibility 1"));

    const QColor green = hex(laws::PROVED);
    const QColor blue = hex(laws::CLEAN);
    CHECK(green != blue);
    for (int r = 0; r < findings->rowCount(); ++r) {
        const auto st = findings->item(r, 0)->text().toStdString();
        const auto c = bg(findings, r, 0);
        CHECK(c == hex(st));
        if (st == laws::CLEAN) {
            CHECK(c == blue);
            CHECK(c != green);
            CHECK(c != hex(laws::PROVED_UNBOUNDED));
        }
        if (laws::is_proof(st)) CHECK(c != blue);
        if (st == laws::NOTRUN) CHECK_FALSE(pg::is_proof_green(pg::status_background(st)));
    }

    auto* tax = child<QTableWidget>(w, "taxonomy");
    std::set<QString> verdicts;
    for (int r = 0; r < tax->rowCount(); ++r) {
        const auto v = tax->item(r, 1)->text();
        verdicts.insert(v);
        CHECK(v != QStringLiteral("CLEAN"));
        if (v == QStringLiteral("COVERED")) {
            CHECK(bg(tax, r, 1) == green);
            CHECK(bg(tax, r, 1) != blue);
        }
        if (v == QStringLiteral("PARTIAL")) {
            CHECK(bg(tax, r, 1) == hex(laws::BOUNDED));
            CHECK(bg(tax, r, 1) != green);
        }
        if (tax->item(r, 0)->text() == QStringLiteral("INT-SIGNED-OVF")) CHECK(v == QStringLiteral("COVERED"));
    }
    CHECK(verdicts.count(QStringLiteral("COVERED")));
    CHECK(verdicts.count(QStringLiteral("GAP")));

    // The stage table: esbmc is NOTRUN, never shown as proved.
    auto st = cells(child<QTableWidget>(w, "stages"));
    REQUIRE(st.size() == 3);
    CHECK(st[2][0] == QStringLiteral("esbmc"));
    CHECK(st[2][1] == QStringLiteral("NOTRUN"));
    CHECK(st[0][2] == QStringLiteral("1"));
}

TEST_CASE("offscreen: an LLM hypothesis neither covers nor paints proof-green") {
    TempDir td("llm");
    prism::RunReport report;
    report.root = "mem";
    report.stages = {stage("llm", "ok", {finding("llm", laws::HYPOTHESIS, "INT-SIGNED-OVF", "maybe overflow")})};
    report.save(td.path / "out" / "report.json");
    prism::MainWindow w;
    w.loadReport(QDir::current().absoluteFilePath(QStringLiteral("out")));
    CHECK(child<QLabel>(w, "s_conf")->text() == Q(pg::confidence_label(report)));
    CHECK_FALSE(child<QLabel>(w, "s_conf")->text().toLower().contains(QStringLiteral("n/a")));
    auto rows = cells(child<QTableWidget>(w, "findings"));
    REQUIRE(rows.size() == 1);
    CHECK(rows[0][0] == QStringLiteral("HYPOTHESIS"));
    auto* tax = child<QTableWidget>(w, "taxonomy");
    bool saw = false;
    for (int r = 0; r < tax->rowCount(); ++r) {
        const auto v = tax->item(r, 1)->text();
        CHECK(v != QStringLiteral("CLEAN"));
        CHECK(v != QStringLiteral("COVERED"));
        CHECK_FALSE(pg::is_proof_green(pg::taxonomy_background(v.toStdString())));
        if (v == QStringLiteral("PARTIAL")) CHECK(bg(tax, r, 1) == hex(laws::BOUNDED));
        if (tax->item(r, 0)->text() == QStringLiteral("INT-SIGNED-OVF")) {
            saw = true;
            CHECK((v == QStringLiteral("PARTIAL") || v == QStringLiteral("GAP")));
        }
    }
    CHECK(saw);
    // A missing report afterwards resets to 0, not to the old numbers.
    w.loadReport(QDir::current().absoluteFilePath(QStringLiteral("nothing-here")));
    CHECK(child<QLabel>(w, "s_conf")->text() == Q(pg::MISSING_REPORT_LABEL));
    CHECK(child<QTableWidget>(w, "findings")->rowCount() == 0);
    CHECK(child<QTableWidget>(w, "taxonomy")->rowCount() == 0);
}

TEST_CASE("journal poll: live stage table and one progress line per change") {
    TempDir td("poll");
    prism::MainWindow w;
    const fs::path out = w.outDir().toStdString();
    fs::create_directories(out);
    auto s1 = stage("inventory", "ok", {}, 3);
    s1.started = 1e12;  // later than any run start
    prism::journal_append_stage(out, s1);
    w.pollJournal();
    auto* stages = child<QTableWidget>(w, "stages");
    REQUIRE(stages->rowCount() == 1);
    CHECK(stages->item(0, 0)->text() == QStringLiteral("inventory"));
    CHECK(stages->item(0, 2)->text() == QStringLiteral("3"));
    auto* log = child<QPlainTextEdit>(w, "log");
    CHECK(log->toPlainText().count(QStringLiteral("inventory ok (3)")) == 1);
    w.pollJournal();  // unchanged: no second line
    CHECK(log->toPlainText().count(QStringLiteral("inventory ok (3)")) == 1);
    auto s2 = stage("esbmc", "NOTRUN", {});
    s2.started = 1e12;
    s2.install = "install esbmc";
    prism::journal_append_stage(out, s2);
    w.pollJournal();
    REQUIRE(stages->rowCount() == 2);
    CHECK(stages->item(1, 1)->text() == QStringLiteral("NOTRUN"));
    CHECK(stages->item(1, 4)->text() == QStringLiteral("install esbmc"));
    CHECK(log->toPlainText().contains(QStringLiteral("esbmc NOTRUN (0)")));
}

#ifndef _WIN32
// A stand-in `prism` so the run/done path is tested without a scan: it
// writes the journal and report the real CLI would, then exits with $CODE.
void fake_prism(const fs::path& dir, const fs::path& report, int code) {
    const auto p = dir / "fake-prism";
    std::ofstream f(p);
    f << "#!/bin/sh\n"
         "out=\"$3\"\n"
         "mkdir -p \"$out\"\n"
         "cp '" << report.string() << "' \"$out/report.json\"\n"
         "echo \"args: $*\"\n"
         "exit " << code << "\n";
    f.close();
    ::chmod(p.c_str(), 0755);
}

TEST_CASE("a finished run logs the confidence line and the NOTRUN stages") {
    TempDir td("done");
    auto report = mixed_report();
    report.save(td.path / "canned.json");
    fake_prism(td.path, td.path / "canned.json", 0);
    prism::MainWindow w;
    w.setPrismProgram(QString::fromStdString((td.path / "fake-prism").string()));
    auto* run = child<QPushButton>(w, "run");
    run->click();
    CHECK_FALSE(run->isEnabled());
    REQUIRE(wait_for([&] { return run->isEnabled(); }, 20000));
    const auto log = child<QPlainTextEdit>(w, "log")->toPlainText();
    CHECK(log.contains(QStringLiteral("--fuzz-budget 4")));
    CHECK(log.contains(Q(pg::done_summary(pg::confidence_product(report), report.stages))));
    CHECK(log.contains(QStringLiteral("NOTRUN=esbmc")));
    CHECK(child<QTableWidget>(w, "findings")->rowCount() == 3);
    CHECK(w.findChild<QMessageBox*>(QStringLiteral("run_failure")) == nullptr);
}

TEST_CASE("a failed run raises a dialog, never a clean result") {
    TempDir td("fail");
    auto report = mixed_report();
    report.save(td.path / "canned.json");
    fake_prism(td.path, td.path / "canned.json", 2);
    prism::MainWindow w;
    w.setPrismProgram(QString::fromStdString((td.path / "fake-prism").string()));
    auto* run = child<QPushButton>(w, "run");
    run->click();
    REQUIRE(wait_for([&] { return run->isEnabled(); }, 20000));
    auto* box = w.findChild<QMessageBox*>(QStringLiteral("run_failure"));
    REQUIRE(box != nullptr);
    CHECK(box->icon() == QMessageBox::Critical);
    CHECK(box->text().contains(QStringLiteral("not a clean run")));
    const auto log = child<QPlainTextEdit>(w, "log")->toPlainText();
    CHECK_FALSE(log.contains(QStringLiteral("done.")));
    box->close();
}

TEST_CASE("a missing prism binary is NOTRUN in the log") {
    TempDir td("noprism");
    prism::MainWindow w;
    w.setPrismProgram(QStringLiteral("/nonexistent/prism"));
    auto* run = child<QPushButton>(w, "run");
    run->click();
    CHECK(run->isEnabled());
    CHECK(child<QPlainTextEdit>(w, "log")->toPlainText().contains(
        QStringLiteral("NOTRUN gui: prism binary not found — not a clean window")));
    CHECK(child<QLabel>(w, "s_conf")->text() == Q(pg::MISSING_REPORT_LABEL));
}

TEST_CASE("a real run through the window (prism beside the test binary)") {
    const auto prism = fs::path(QCoreApplication::applicationDirPath().toStdString()) / "prism";
    const auto src = repo_root() / "testdata" / "div_param.c";
    if (!fs::exists(prism) || !fs::exists(src)) {
        MESSAGE("NOTRUN: prism binary or testdata not found");
        return;
    }
    const auto abs_src = fs::absolute(src);
    TempDir td("real");
    fs::create_directories(td.path / "src");
    fs::copy_file(abs_src, td.path / "src" / "div_param.c");
    prism::MainWindow w({QStringLiteral("src"), QStringLiteral("--no-llm"), QStringLiteral("--stage"),
                         QStringLiteral("inventory,classify,lints")});
    auto* run = child<QPushButton>(w, "run");
    run->click();
    REQUIRE(wait_for([&] { return run->isEnabled(); }, 300000));
    const auto log = child<QPlainTextEdit>(w, "log")->toPlainText();
    CHECK(log.contains(QStringLiteral("done. confidence")));
    CHECK(child<QTableWidget>(w, "stages")->rowCount() > 0);
    CHECK(child<QTableWidget>(w, "findings")->rowCount() > 0);
    CHECK(child<QLabel>(w, "s_conf")->text().startsWith(QStringLiteral("confidence ")));
    CHECK(w.findChild<QMessageBox*>(QStringLiteral("run_failure")) == nullptr);
}
#endif

}  // TEST_SUITE

int main(int argc, char** argv) {
    // Offscreen: no display needed, and never a real window.
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);
    doctest::Context ctx(argc, argv);
    return ctx.run();
}
