//=============================================================================
// main.cpp — Buck HIL 上位机入口
//
// Qt 6 Widgets 应用，跨平台 (Windows/Linux/macOS)
// 三线程模型: GUI + Comm + DataProcess
//=============================================================================
#include <QApplication>
#include <QSurfaceFormat>
#include "MainWindow.h"

int main(int argc, char *argv[])
{
    // 设置 OpenGL 兼容性 (QCustomPlot 默认用 raster，无需 OpenGL)
    QSurfaceFormat fmt;
    fmt.setSamples(0);  // 不需要多重采样
    QSurfaceFormat::setDefaultFormat(fmt);

    QApplication app(argc, argv);
    app.setApplicationName(QStringLiteral("BuckHIL"));
    app.setApplicationVersion(QStringLiteral("1.0.0"));
    app.setOrganizationName(QStringLiteral("BuckHIL"));

    MainWindow window;
    window.setWindowTitle(QStringLiteral("Buck HIL — ZU3EG 实时仿真"));
    window.resize(1400, 900);
    window.show();

    return app.exec();
}
