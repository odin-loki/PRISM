#include "MainWindow.h"

#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QFileDialog>
#include <QLabel>
#include <QWidget>

namespace helix {

MainWindow::MainWindow(QWidget *parent) : QMainWindow(parent) {
    setWindowTitle(QStringLiteral("Helix"));
    resize(960, 640);
    auto *root = new QWidget(this);
    setCentralWidget(root);
    auto *v = new QVBoxLayout(root);
    auto *h = new QHBoxLayout();
    path_ = new QLineEdit(QStringLiteral("testdata"));
    auto *browse = new QPushButton(QStringLiteral("Open…"));
    run_ = new QPushButton(QStringLiteral("Run pipeline"));
    h->addWidget(new QLabel(QStringLiteral("Path")));
    h->addWidget(path_, 1);
    h->addWidget(browse);
    h->addWidget(run_);
    v->addLayout(h);
    log_ = new QPlainTextEdit;
    log_->setReadOnly(true);
    v->addWidget(log_, 1);
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
}

void MainWindow::onRun() {
    run_->setEnabled(false);
    log_->appendPlainText(QStringLiteral("running python -m helix …"));
    proc_->setProgram(QStringLiteral("python"));
    proc_->setArguments({QStringLiteral("-m"), QStringLiteral("helix"), path_->text(),
                         QStringLiteral("--no-llm")});
    proc_->start();
}

void MainWindow::onDone(int exitCode, QProcess::ExitStatus) {
    run_->setEnabled(true);
    log_->appendPlainText(QStringLiteral("exit %1").arg(exitCode));
}

}  // namespace helix
