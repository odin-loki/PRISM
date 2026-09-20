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
    QApplication app(argc, argv);
    prism::MainWindow w;
    w.show();
    return app.exec();
}
