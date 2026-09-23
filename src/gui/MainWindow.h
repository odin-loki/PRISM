#pragma once

#include <QMainWindow>
#include <QPlainTextEdit>
#include <QProcess>
#include <QPushButton>
#include <QLineEdit>
#include <QCheckBox>
#include <QTableWidget>
#include <QLabel>

namespace prism {

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(QWidget *parent = nullptr);

private slots:
    void onRun();
    void onDone(int exitCode, QProcess::ExitStatus st);
    // Roadmap 9.4 assistant: questions over findings, "explain <id>", "trusted base".
    void onAsk();

private:
    void loadSameReport(const QString& outDir);
    QString prismBinary() const;

    QLineEdit *path_ = nullptr;
    QPlainTextEdit *log_ = nullptr;
    QPushButton *run_ = nullptr;
    QProcess *proc_ = nullptr;
    QCheckBox *no_llm_ = nullptr;
    QCheckBox *skip_fuzz_ = nullptr;
    QCheckBox *skip_repair_ = nullptr;
    // Law 9: --allow-exec, default off.
    QCheckBox *allow_exec_ = nullptr;
    QTableWidget *tax_ = nullptr;
    QTableWidget *findings_ = nullptr;
    QLabel *vis_ = nullptr;
    QLabel *ans_ = nullptr;
    QLabel *res_ = nullptr;
    QLabel *conf_ = nullptr;
    QString out_dir_;
    // Assistant chat panel (roadmap 9.4). Answers show the structured query;
    // nothing typed here can change a verdict.
    QLineEdit *ask_ = nullptr;
    QPushButton *ask_btn_ = nullptr;
    QPlainTextEdit *chat_ = nullptr;
};

}  // namespace prism
