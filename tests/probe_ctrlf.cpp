// probe_ctrlf.cpp —— Ctrl+F 快捷键作用域裁决探针（2026-11-07）：
//   预览页（CFilePreviewWidget）已有 WidgetWithChildrenShortcut 的 Ctrl+F（文件内
//   查找栏，v3.0.206）；现在要给主窗「编辑→查找...」加同键。验证两种上下文组合
//   下焦点在预览内/外时各自谁触发（歧义=都不触发）。
//   手动运行：offscreen 即可。退出码 0=两场景各有裁决 / 3=存在吞键场景。
#include <QApplication>
#include <QDir>
#include <QFile>
#include <QLineEdit>
#include <QShortcut>
#include <QTabWidget>
#include <QTextBrowser>
#include <QTextEdit>
#include <QVBoxLayout>
#include <QtTest/QtTest>

#include "ui/CFilePreviewWidget.h"

static int g_iWindowFired = 0;
static int g_iAppFired = 0;

int main(int argc, char** argv) {
    setvbuf(stdout, nullptr, _IONBF, 0);   // 沙箱排障：无缓冲逐行落盘
    printf("[T] main enter\n");
    QApplication a(argc, argv);
    printf("[T] qapp ok\n");

    QWidget w;
    w.resize(800, 600);
    auto* pLay = new QVBoxLayout(&w);
    auto* pTabs = new QTabWidget(&w);
    pLay->addWidget(pTabs);

    auto* pPrev = new CFilePreviewWidget(&w);
    printf("[T] preview built\n");
    pTabs->addTab(pPrev, QStringLiteral("预览"));
    auto* pOther = new QTextEdit(&w);
    pTabs->addTab(pOther, QStringLiteral("其他页"));
    w.show();

    // 组合一：主窗 WindowShortcut（QAction 菜单快捷键的缺省上下文）。
    auto* pScWin = new QShortcut(QKeySequence::Find, &w);
    QObject::connect(pScWin, &QShortcut::activated, [] { ++g_iWindowFired; });
    // 组合二：ApplicationShortcut。
    auto* pScApp = new QShortcut(QKeySequence::Find, &w);
    pScApp->setContext(Qt::ApplicationShortcut);
    QObject::connect(pScApp, &QShortcut::activated, [] { ++g_iAppFired; });
    // 预览内部 Ctrl+F 的可观察面：查找栏（含 QLineEdit 的隐藏容器）变为可见。
    const auto aEdits = pPrev->findChildren<QLineEdit*>();
    QWidget* pBar = aEdits.isEmpty() ? nullptr : aEdits.front()->parentWidget();

    // —— 场景 1：焦点在预览的纯文本视图里 ——
    pTabs->setCurrentWidget(pPrev);
    const QString sTmp = QDir::temp().absoluteFilePath(QStringLiteral(
        "probe_ctrlf_%1.txt").arg(QCoreApplication::applicationPid()));
    {
        QFile f(sTmp);
        f.open(QIODevice::WriteOnly);
        f.write("hello ctrl f");
        f.close();
    }
    pPrev->ShowFile(sTmp);
    printf("[T] showfile done\n");
    QApplication::processEvents();
    QApplication::setActiveWindow(&w);
    w.activateWindow();
    // 纯文本页视图（QTextBrowser）显式持焦（offscreen 下无窗口激活链，不设焦
    //   focusWidget 恒 null，keyClick 无性投递）。
    if (QTextBrowser* pView = pPrev->findChild<QTextBrowser*>()) {
        pView->setFocus();
    }
    QApplication::processEvents();
    QWidget* pFocus = pPrev->focusWidget() != nullptr ? pPrev->focusWidget() : pPrev;
    printf("scene1 focus=%s\n", pFocus ? pFocus->metaObject()->className() : "(null)");
    QTest::keyClick(pFocus, Qt::Key_F, Qt::ControlModifier);
    QApplication::processEvents();
    const int iPrev1 = (pBar != nullptr && !pBar->isHidden()) ? 1 : 0;
    const int iWin1 = g_iWindowFired, iApp1 = g_iAppFired;
    printf("scene1: previewBar=%d windowFired=%d appFired=%d\n", iPrev1, iWin1, iApp1);

    // —— 场景 2：焦点在预览外（其他页 QTextEdit）——
    pTabs->setCurrentWidget(pOther);
    QApplication::setActiveWindow(&w);
    pOther->setFocus();
    QApplication::processEvents();
    QTest::keyClick(pOther, Qt::Key_F, Qt::ControlModifier);
    QApplication::processEvents();
    const int iPrev2 = ((pBar != nullptr && !pBar->isHidden()) ? 1 : 0) - iPrev1;
    const int iWin2 = g_iWindowFired - iWin1, iApp2 = g_iAppFired - iApp1;
    printf("scene2: previewBar=%d windowFired=%d appFired=%d\n", iPrev2, iWin2, iApp2);

    int rc = 0;
    if (iPrev1 + iWin1 + iApp1 == 0 || iPrev2 + iWin2 + iApp2 == 0) {
        printf("[FAIL] 存在无触发场景（歧义/吞键）\n");
        rc = 3;
    }
    QFile::remove(sTmp);
    printf("probe rc=%d\n", rc);
    return rc;
}
