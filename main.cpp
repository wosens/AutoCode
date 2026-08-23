#include <QApplication>
#include <QCoreApplication>
#include <QDir>
#include <QFont>
#include <QIcon>
#include <cstring>

#include "ui/CMainWindow.h"
#include "agent/CMcpAskServer.h"
#include "core/CCrashDump.h"

int main(int argc, char* argv[]) {
    // 2026.08.22 崩溃转储：崩溃/abort/purecall/std::terminate 等异常退出时
    //   在 <exe 目录>\dump\ 落 MiniDump（最先装，覆盖后续所有路径，含 --mcp-ask-server 模式）。
    CCrashDump::Install();
    // MCP ask-user server 模式：claude 经 --mcp-config 以子进程拉起本 exe，
    // 跑 stdio JSON-RPC（暴露 ask_user 工具），不启动 GUI。
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--mcp-ask-server") == 0) {
            QCoreApplication app(argc, argv);
            CMcpAskServer server;
            server.Start();
            return app.exec();
        }
    }

    QApplication app(argc, argv);
    app.setApplicationName(QStringLiteral("AutoCode"));
    app.setOrganizationName(QStringLiteral("AutoCode"));

    // FR-10：应用图标（任务栏 / 标题栏 / 自绘标题栏 / exe 资源 四入口）。
    app.setWindowIcon(QIcon(QStringLiteral(":/icons/app.ico")));

    // 全局字体设为 11 号（保留系统默认字体族）
    QFont fntApp = app.font();
    fntApp.setPointSize(11);
    app.setFont(fntApp);

    // 全局 QMenu 浅色样式：Windows「深色应用模式」下 QTextBrowser/QPlainTextEdit 的
    // 默认右键菜单（复制/全选）会继承深色调，文字与背景都偏暗、不可读。此处统一把
    // 所有弹出/右键菜单（含菜单栏下拉）强制浅色，保证可读。QMenuBar 本体仍保持原生。
    app.setStyleSheet(QStringLiteral(
        "QMenu { background-color: #ffffff; color: #1f2937;"
        "        border: 1px solid #d1d5db; padding: 4px 0; }"
        "QMenu::item { background-color: transparent; padding: 5px 26px 5px 22px;"
        "              border-radius: 3px; }"
        "QMenu::item:selected { background-color: #dbeafe; color: #1e40af; }"
        "QMenu::item:disabled { color: #9ca3af; }"
        "QMenu::separator { height: 1px; background: #e5e7eb; margin: 4px 8px; }"
        // QMenuBar 本体统一浅色（系统深色模式下原生菜单栏会深底灰字不可读，与 QMenu 一致强制浅底深字）。
        "QMenuBar { background-color: #ffffff; color: #1f2937; }"
        "QMenuBar::item { background-color: transparent; padding: 5px 10px; border-radius: 3px; }"
        "QMenuBar::item:selected { background-color: #dbeafe; color: #1e40af; }"));

    CMainWindow window;
    window.show();
    return app.exec();
}
