#include <QApplication>
#include <QCoreApplication>
#include <QDir>
#include <QFont>
#include <QIcon>
#include <cstring>

#include "ui/CMainWindow.h"
#include "agent/CMcpAskServer.h"
#include "core/CCrashDump.h"
#include "sandbox/CSandboxCore.h"   // 2026-09-26 --sandbox 命令解析（sbx::ParseCli）
#include <windows.h>   // v2.2.146 诊断模式 ExitProcess 直退（析构拆序段错误兑底）
#include "ui/CSettingsDialog.h"
#include "project/CProjectManager.h"
#include <QTabWidget>
#include <QTreeWidget>
#include <QLabel>
#include <QFile>
#include <QHeaderView>   // 2026.09.11 --diag-settings 转储列宽/resize mode
#include <QTextStream>
#include <QLocalSocket>   // 2026-09-12 --ask 弹窗确认（手工审批全后端通道）
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QElapsedTimer>
#include <iostream>

// 2026.09.09 v2.2.146 诊断模式（隐藏，不进帮助）：--diag-settings [工程目录 [任务id]]
//   用真实 CProjectManager（+可选任务级配置覆盖）构造设置对话框，转储标题/Tab 页/
//   模型表行数/顶全部件到 <exe>\dump\settings_diag.txt 并截图 settings_diag.png
//   （不 exec，构造完即退）。用户报「设置里多了个 tab/模型清空」的实机复现工具。
static int RunSettingsDiag(int argc, char* argv[]) {
    QApplication app(argc, argv);
    const QString sProj = (argc >= 3) ? QString::fromLocal8Bit(argv[2]) : QString();
    const QString sTask = (argc >= 4) ? QString::fromLocal8Bit(argv[3]) : QString();
    CProjectManager stPm;
    if (!sProj.isEmpty()) {
        stPm.Open(sProj);
    }
    if (!sTask.isEmpty()) {
        stPm.SetTaskConfigOverlay(sTask);
    }
    CSettingsDialog stDlg(&stPm, nullptr, {}, sTask.isEmpty() ? QString() : sTask);
    QString sOut;
    QTextStream ts(&sOut);
    ts << "title: " << stDlg.windowTitle() << "\n";
    ts << "overlay: " << (stPm.TaskConfigOverlayActive() ? stPm.TaskConfigFile() : "(off)") << "\n";
    if (auto* pTabs = stDlg.findChild<QTabWidget*>()) {
        ts << "tabs(" << pTabs->count() << "): ";
        for (int i = 0; i < pTabs->count(); ++i) {
            ts << "[" << pTabs->tabText(i) << "] ";
        }
        ts << "\n";
    }
    ts << "PiModels=" << stDlg.PiModels().size()
       << " DshModels=" << stDlg.DshModels().size()
       << " CodexModels=" << stDlg.CodexModels().size()
       << " HermesModels=" << stDlg.HermesModels().size() << "\n";
    const auto aTrees = stDlg.findChildren<QTreeWidget*>();
    for (QTreeWidget* pTree : aTrees) {
        ts << "tree: rows=" << pTree->topLevelItemCount()
           << " headers=" << pTree->headerItem()->text(0)
           << " visible=" << pTree->isVisible();
        // 2026.09.11 模型表列宽可调诊断：逐列转储 resize mode + 实际宽度
        //   （全列应为 Interactive、末列 stretch；宽度即持久化恢复后的值）。
        ts << " modes=";
        for (int i = 0; i < pTree->columnCount(); ++i) {
            ts << (i > 0 ? "," : QString())
               << int(pTree->header()->sectionResizeMode(i));
        }
        ts << " widths=";
        for (int i = 0; i < pTree->columnCount(); ++i) {
            ts << (i > 0 ? "," : QString()) << pTree->columnWidth(i);
        }
        ts << " stretchLast=" << pTree->header()->stretchLastSection();
        ts << " parentChain=";
        for (QWidget* pW = pTree->parentWidget(); pW != nullptr; pW = pW->parentWidget()) {
            QString sName = pW->objectName();
            QString sCls = pW->metaObject()->className();
            if (auto* pTabW = qobject_cast<QTabWidget*>(pW)) {
                sName = QStringLiteral("tab:%1").arg(pTabW->tabText(
                    pTabW->indexOf(pTree->parentWidget() == pTabW
                        ? pTree : qobject_cast<QWidget*>(pTree->parent()))));
            }
            ts << sCls << "(" << sName << ") <- ";
        }
        ts << "\n";
    }
    const auto aAllTabs = stDlg.findChildren<QTabWidget*>();
    ts << "QTabWidget count: " << aAllTabs.size() << "\n";
    for (QTabWidget* pTabW : aAllTabs) {
        ts << "  tabwidget tabs:";
        for (int i = 0; i < pTabW->count(); ++i) {
            ts << " [" << pTabW->tabText(i) << "]";
        }
        ts << "\n";
    }
    // 顶层布局首件（找神秘 tab/提示条）
    const auto aLabels = stDlg.findChildren<QLabel*>();
    for (QLabel* pLbl : aLabels) {
        const QString sTxt = pLbl->text();
        if (!sTxt.isEmpty() && (sTxt.contains(QLatin1String("任务"))
                                || sTxt.contains(QLatin1String("◆")))) {
            ts << "label: " << sTxt.left(80) << "\n";
        }
    }
    QDir().mkpath(QCoreApplication::applicationDirPath() + "/dump");
    const QString sBase = QCoreApplication::applicationDirPath() + "/dump/settings_diag";
    QFile fTxt(sBase + ".txt");
    fTxt.open(QIODevice::WriteOnly | QIODevice::Truncate);
    fTxt.write(sOut.toUtf8());
    fTxt.close();
    stDlg.resize(980, 700);
    stDlg.grab().save(sBase + ".png");
    fTxt.flush();
    // 诊断模式直退（栈上 CProjectManager 的后台线程/静态 runner 拆序在 main 尾声
    //   有确定性段错误——诊断只取控件树与截图，不走完整析构）。
    ::ExitProcess(0);
    return 0;
}

// 2026-09-12 --ask 弹窗确认模式（实现说明见 main() 内分派处注释）。
//   协议同 CMcpAskServer::HandleAskUser：一行 JSON {type:ask,question,options}\n 发到
//   AUTOCODE_ASK_PIPE 指向的主实例 QLocalServer，同步等 {type:answer|cancel}\n 回行
//   （500ms 轮询累计超时）。stdout 只写结果文本（UTF-8），诊断走 stderr。
static int RunAskMode(int argc, char* argv[]) {
    QCoreApplication app(argc, argv);   // 仅用 QLocalSocket，无控件（弹窗在主实例）
    const QStringList aArgs = app.arguments();   // Qt 经 GetCommandLineW 解码，中文安全
    if (aArgs.size() < 4) {
        std::cout << "[提问服务不可用：用法 AutoCode.exe --ask <超时秒> <问题文本>]\n";
        return 4;
    }
    bool bOk = false;
    const int iTimeoutSec = aArgs.at(2).toInt(&bOk);
    if (!bOk || iTimeoutSec <= 0 || iTimeoutSec > 24 * 3600) {
        std::cout << "[提问服务不可用：非法超时秒数]\n";
        return 4;
    }
    const QString sQuestion = aArgs.mid(3).join(QLatin1Char(' '));
    if (sQuestion.trimmed().isEmpty()) {
        std::cout << "[提问服务不可用：问题文本为空]\n";
        return 4;
    }
    const QString sPipe = qEnvironmentVariable("AUTOCODE_ASK_PIPE");
    if (sPipe.isEmpty()) {
        std::cout << "[提问服务不可用：未配置 AUTOCODE_ASK_PIPE（须在 AutoCode 的助手进程链内运行）]\n";
        return 4;
    }
    QLocalSocket sock;
    sock.connectToServer(sPipe);
    if (!sock.waitForConnected(3000)) {
        std::cout << "[提问服务不可用：无法连接 AutoCode 主窗口（可能已关闭）]\n";
        return 4;
    }
    QJsonObject oAsk;
    oAsk.insert(QStringLiteral("type"), QStringLiteral("ask"));
    oAsk.insert(QStringLiteral("question"), sQuestion);
    oAsk.insert(QStringLiteral("options"), QJsonArray{});
    sock.write(QJsonDocument(oAsk).toJson(QJsonDocument::Compact) + '\n');
    if (!sock.waitForBytesWritten(3000)) {
        std::cout << "[提问服务不可用：发送提问失败]\n";
        return 4;
    }
    // 同步等答复（口径同 CMcpAskServer：500ms 轮询累计超时）
    QElapsedTimer t;
    t.start();
    QString sReply;
    bool bGot = false;
    bool bCancelled = false;
    while (!bGot && t.elapsed() < qint64(iTimeoutSec) * 1000) {
        if (sock.state() == QLocalSocket::UnconnectedState && sock.bytesAvailable() == 0) {
            break;   // 主窗口主动断开
        }
        while (sock.canReadLine()) {
            const QByteArray aLine = sock.readLine().trimmed();
            if (aLine.isEmpty()) {
                continue;
            }
            const QJsonDocument stDoc = QJsonDocument::fromJson(aLine);
            if (!stDoc.isObject()) {
                continue;
            }
            const QJsonObject o = stDoc.object();
            const QString sType = o.value(QStringLiteral("type")).toString();
            if (sType == QStringLiteral("answer")) {
                sReply = o.value(QStringLiteral("text")).toString();
                bGot = true;
                break;
            }
            if (sType == QStringLiteral("cancel")) {
                bCancelled = true;
                bGot = true;
                break;
            }
        }
        if (bGot) {
            break;
        }
        sock.waitForReadyRead(500);
    }
    sock.disconnectFromServer();
    if (sock.state() != QLocalSocket::UnconnectedState) {
        sock.waitForDisconnected(1000);
    }
    if (bCancelled) {
        std::cout << "[用户未确认（取消/关闭弹窗）]\n";
        return 2;
    }
    if (!bGot) {
        std::cout << "[等待用户确认超时（" << iTimeoutSec << " 秒）]\n";
        return 3;
    }
    std::cout << "用户已答复：" << sReply.toUtf8().toStdString() << "\n";
    return 0;
}

// 2026-09-26 --sandbox 沙箱命令模式（内置测试沙箱，全后端通用入口）。
//   Agent 经 Bash 运行："<AutoCode.exe 全路径>" --sandbox <cmd> [参数...]，本进程经
//   AUTOCODE_ASK_PIPE 连到所属主实例，转发 {type:sbx,cmd,args} 并同步等
//   {type:sbxres,ok,text} 回行（200ms 轮询累计超时 180s）。结果文本写 stdout（UTF-8），
//   退出码 0=成功 / 1=失败 / 4=服务不可用/用法错误。子命令与参数解析见
//   sbx::ParseCli（windows/status/capture/click/drag/type/keys/command/button/
//   tree/add/restore/restore-all/close/record_on/record_off）。
static int RunSandboxMode(int argc, char* argv[]) {
    QCoreApplication app(argc, argv);   // 仅 QLocalSocket，无控件（操作在主实例）
    const QStringList aArgs = app.arguments();   // Qt 经 GetCommandLineW 解码，中文安全
    QString sCmd;
    QJsonObject oArgs;
    if (!sbx::ParseCli(aArgs, &sCmd, &oArgs)) {
        std::cout << "[沙箱不可用：用法 AutoCode.exe --sandbox <命令> [参数]，命令集 "
                     "windows/status/capture/click/drag/type/keys/command/button/tree/"
                     "add/restore/restore-all/close/record_on/record_off]\n";
        return 4;
    }
    const QString sPipe = qEnvironmentVariable("AUTOCODE_ASK_PIPE");
    if (sPipe.isEmpty()) {
        std::cout << "[沙箱不可用：未配置 AUTOCODE_ASK_PIPE（须在 AutoCode 的助手进程链内运行）]\n";
        return 4;
    }
    QLocalSocket sock;
    sock.connectToServer(sPipe);
    if (!sock.waitForConnected(3000)) {
        std::cout << "[沙箱不可用：无法连接主窗口]\n";
        return 4;
    }
    QJsonObject oReq;
    oReq.insert(QStringLiteral("type"), QStringLiteral("sbx"));
    oReq.insert(QStringLiteral("cmd"), sCmd);
    oReq.insert(QStringLiteral("args"), oArgs);
    sock.write(QJsonDocument(oReq).toJson(QJsonDocument::Compact) + "\n");
    if (!sock.waitForBytesWritten(3000)) {
        std::cout << "[沙箱不可用：发送命令失败]\n";
        return 4;
    }
    QElapsedTimer t;
    t.start();
    bool bGot = false, bOk = false;
    QString sText;
    while (!bGot && t.elapsed() < 180000) {
        if (sock.state() == QLocalSocket::UnconnectedState && sock.bytesAvailable() == 0) {
            break;
        }
        while (sock.canReadLine()) {
            const QByteArray aLine = sock.readLine().trimmed();
            if (aLine.isEmpty()) {
                continue;
            }
            const QJsonDocument stDoc = QJsonDocument::fromJson(aLine);
            if (!stDoc.isObject()) {
                continue;
            }
            const QJsonObject o = stDoc.object();
            if (o.value(QStringLiteral("type")).toString() == QStringLiteral("sbxres")) {
                bOk = o.value(QStringLiteral("ok")).toBool();
                sText = o.value(QStringLiteral("text")).toString();
                bGot = true;
                break;
            }
        }
        if (bGot) {
            break;
        }
        sock.waitForReadyRead(200);
    }
    sock.disconnectFromServer();
    if (!bGot) {
        std::cout << "[沙箱命令超时未响应]\n";
        return 4;
    }
    std::cout << sText.toUtf8().toStdString() << "\n";
    return bOk ? 0 : 1;
}

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
        // 2026-09-12 --ask 弹窗确认模式（手工审批，DSH/Pi/Codex/Hermes 全后端通用）：
        //   助手经 Bash 运行 "%AUTOCODE_ASK_EXE%" --ask <超时秒> "问题"（环境由各
        //   runner InjectAskEnv 注入），本进程经 AUTOCODE_ASK_PIPE 连到所属主实例的
        //   AskServer（同 claude ask_user MCP 的转发协议），弹窗由主实例弹（CAskUserDialog），
        //   答复回写本进程 stdout（UTF-8）。退出码：0=已答复 / 2=取消或关窗 /
        //   3=超时 / 4=服务不可用。用法：AutoCode.exe --ask 600 "要确认的问题"。
        if (std::strcmp(argv[i], "--ask") == 0) {
            return RunAskMode(argc, argv);
        }
        // 2026-09-26 --sandbox 沙箱命令模式（内置测试沙箱，全后端通用）：
        //   "<AutoCode.exe>" --sandbox windows / capture <hwnd> / click <hwnd> <x> <y> /
        //   type <hwnd> <文本> / tree <hwnd> / restore <hwnd> / restore-all ...
        //   （环境 AUTOCODE_ASK_PIPE 由 runner InjectAskEnv 注入，同 --ask 通道）。
        if (std::strcmp(argv[i], "--sandbox") == 0) {
            return RunSandboxMode(argc, argv);
        }
        // 2026.09.09 v2.2.146 隐藏诊断模式（见 RunSettingsDiag 注释）。须在 QApplication
        //   构造前分派：RunSettingsDiag 自建 QApplication，Qt 进程内仅允许一个实例，
        //   后分派会二次构造（断言构建直接 abort / Release 覆盖 self）。
        if (std::strcmp(argv[i], "--diag-settings") == 0) {
            return RunSettingsDiag(argc, argv);
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
        // 2026.09.17 菜单图标列右移 8px（offscreen 实测：Qt 样式表下图标恒贴左 x≈3，
        //   item 的 padding-left 只推文字不推图标，唯 QMenu::icon{left:N} 精确可控）。
        "QMenu::icon { left: 8px; }"
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
