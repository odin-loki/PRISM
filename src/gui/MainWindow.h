#pragma once

#include "prism/gui_model.hpp"

#include <QCheckBox>
#include <QLabel>
#include <QLineEdit>
#include <QMainWindow>
#include <QPlainTextEdit>
#include <QProcess>
#include <QPushButton>
#include <QStringList>
#include <QTableWidget>
#include <QTimer>

namespace prism {

// The PRISM window. Every row it shows comes from prism::gui (gui_model.hpp,
// tested headless); this class only copies those rows into widgets and runs
// the `prism` CLI. Widgets carry object names so tests find them offscreen.
class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    // `launch`: the window's own argv without argv[0] (prism_gui [PATH] [flags]).
    explicit MainWindow(const QStringList& launch = {}, QWidget *parent = nullptr);
    // Headless smoke test (QT_QPA_PLATFORM=offscreen): run the pipeline
    // through this window, and when the report has loaded save the window to
    // `screenshot` and exit 0 if findings were shown, 3 if none (never a
    // clean pass on an empty table).
    void runSmoke(const QString& screenshot);
    // Load <outDir>/report.json into the stats, stage, finding and taxonomy
    // views (a missing report is confidence 0, never a proof). With
    // `notBefore` (epoch seconds) a report written before then is stale and
    // is treated as missing.
    void loadReport(const QString& outDir, double notBefore = 0);
    // One journal poll (<out>/stages.jsonl): the stage table and a log line
    // when the last stage changed. Runs every POLL_MS while a run is live.
    void pollJournal();
    // The `prism` program the window runs (default: beside prism_gui, else PATH).
    void setPrismProgram(const QString& program) { program_ = program; }
    QString outDir() const { return out_dir_; }
    // The argv (without the program) the next run passes to prism.
    QStringList runArguments() const;

private slots:
    void onRun();
    void onDone(int exitCode, QProcess::ExitStatus st);
    // Roadmap 9.4 assistant: questions over findings, "explain <id>", "trusted base".
    void onAsk();

private:
    void fillStages(const std::vector<StageResult>& stages);
    QString prismBinary() const;

    prism::gui::Launch launch_;
    QLineEdit *path_ = nullptr;
    QPlainTextEdit *log_ = nullptr;
    QPushButton *run_ = nullptr;
    QProcess *proc_ = nullptr;
    QTimer *poll_ = nullptr;
    QCheckBox *llm_ = nullptr;
    QCheckBox *resume_ = nullptr;
    QCheckBox *skip_fuzz_ = nullptr;
    QCheckBox *skip_repair_ = nullptr;
    QCheckBox *skip_optional_ = nullptr;
    // Law 9: --allow-exec, default off.
    QCheckBox *allow_exec_ = nullptr;
    QTableWidget *stages_ = nullptr;
    QTableWidget *tax_ = nullptr;
    QTableWidget *findings_ = nullptr;
    QLabel *vis_ = nullptr;
    QLabel *ans_ = nullptr;
    QLabel *res_ = nullptr;
    QLabel *conf_ = nullptr;
    QString out_dir_;
    QString program_;
    double run_started_ = 0;
    bool run_resume_ = false;
    prism::gui::ProgressTracker progress_;
    // Assistant chat panel (roadmap 9.4). Answers show the structured query;
    // nothing typed here can change a verdict.
    QLineEdit *ask_ = nullptr;
    QPushButton *ask_btn_ = nullptr;
    QPlainTextEdit *chat_ = nullptr;
};

}  // namespace prism
