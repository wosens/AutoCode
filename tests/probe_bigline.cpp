// probe_bigline.cpp —— 大单行 JSON（.autodev chat.json/assistant.json，1.4MB/0 换行）
//   经查找结果点击 → CFilePreviewWidget::ShowFile（纯文本页）耗时量化：
//   setPlainText 首开 / 二开 / GotoPos(1,1,3)（整行选中 1.4M 字符？）各计毫秒。
//   手动运行：Execute\probe_bigline.exe；退出码恒 0，数值即结论。
#include <QApplication>
#include <QDateTime>
#include <QtTest/QtTest>

#include "ui/CFilePreviewWidget.h"

static qint64 Now() { return QDateTime::currentMSecsSinceEpoch(); }

int main(int argc, char** argv) {
    QApplication a(argc, argv);
    const QString sJson = QStringLiteral(
        "E:/wosens/Programs/VisualC++/插件框架/HotNews/.autodev/tasks/12d2892f/chat.json");
    CFilePreviewWidget w;
    w.resize(900, 600);
    w.show();
    QTest::qWait(300);

    qint64 t0 = Now();
    w.ShowFile(sJson);
    printf("open1Ms=%lld\n", static_cast<long long>(Now() - t0));
    QTest::qWait(800);   // 让首帧绘制/布局落地

    t0 = Now();
    w.ShowFile(sJson);
    printf("open2Ms=%lld\n", static_cast<long long>(Now() - t0));
    QTest::qWait(800);

    t0 = Now();
    w.GotoPos(1, 1, 3);
    QApplication::processEvents();
    printf("gotoMs=%lld\n", static_cast<long long>(Now() - t0));
    QTest::qWait(500);

    // 交互模拟：连续换同一个文件 5 次（模拟连点多行）总耗时。
    t0 = Now();
    for (int i = 0; i < 5; ++i) {
        w.ShowFile(sJson);
        QApplication::processEvents();
    }
    printf("fiveClicksMs=%lld\n", static_cast<long long>(Now() - t0));
    printf("probe rc=0\n");
    return 0;
}
