#pragma once

#include <QMainWindow>
#include <QPlainTextEdit>
#include <QProcess>
#include <QPushButton>
#include <QLineEdit>

namespace helix {

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(QWidget *parent = nullptr);

private slots:
    void onRun();
    void onDone(int exitCode, QProcess::ExitStatus st);

private:
    QLineEdit *path_ = nullptr;
    QPlainTextEdit *log_ = nullptr;
    QPushButton *run_ = nullptr;
    QProcess *proc_ = nullptr;
};

}  // namespace helix
