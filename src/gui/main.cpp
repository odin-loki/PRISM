#include "MainWindow.h"

#include <QApplication>
#include <cstdio>
#include <cstdlib>
#include <cstring>

int main(int argc, char **argv) {
#ifndef Q_OS_WIN
    const char* display = std::getenv("DISPLAY");
    const char* wayland = std::getenv("WAYLAND_DISPLAY");
    const char* platform = std::getenv("QT_QPA_PLATFORM");
    bool offscreen = platform && std::strcmp(platform, "offscreen") == 0;
    if (!offscreen && (!display || !display[0]) && (!wayland || !wayland[0])) {
        std::fprintf(stderr, "NOTRUN gui: no display (not a clean window)\n");
        std::fprintf(stderr, "  install: set DISPLAY or QT_QPA_PLATFORM=offscreen\n");
        return 2;
    }
#endif
    // --smoke-screenshot FILE: headless smoke test (see MainWindow::runSmoke).
    // Removed from argv so the window's own CLI parsing never sees it.
    QString smoke;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--smoke-screenshot") == 0 && i + 1 < argc) {
            smoke = QString::fromLocal8Bit(argv[i + 1]);
            for (int j = i; j + 2 <= argc; ++j) argv[j] = argv[j + 2];
            argc -= 2;
            break;
        }
    }
    QApplication app(argc, argv);
    prism::MainWindow w;
    w.show();
    if (!smoke.isEmpty()) w.runSmoke(smoke);
    return app.exec();
}
