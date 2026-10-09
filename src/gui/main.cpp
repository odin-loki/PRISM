#include "MainWindow.h"

#include "prism/gui_model.hpp"

#include <QApplication>
#include <cstdio>
#include <cstdlib>
#include <cstring>

int main(int argc, char **argv) {
    // No display (and not QT_QPA_PLATFORM=offscreen) is NOTRUN, exit 0, as
    // `prism --gui` without prism_gui: a missing window is never a clean
    // result and never a fake window.
    if (!prism::gui::display_available(std::getenv("DISPLAY"), std::getenv("WAYLAND_DISPLAY"),
                                       std::getenv("QT_QPA_PLATFORM"))) {
        std::fputs(prism::gui::notrun_display_text().c_str(), stdout);
        return 0;
    }
    // --smoke-screenshot FILE: headless smoke test (see MainWindow::runSmoke).
    // Removed from argv so the window's own launch parsing never sees it.
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
    QStringList launch = QCoreApplication::arguments();
    if (!launch.isEmpty()) launch.removeFirst();
    prism::MainWindow w(launch);
    w.show();
    if (!smoke.isEmpty()) w.runSmoke(smoke);
    return app.exec();
}
