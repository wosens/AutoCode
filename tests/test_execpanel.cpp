// 单元测试（test_execpanel）：多助手 Tab「点击任务卡死」回归——模拟任务切换路径
// SwitchAssistantArchive → LoadAssistantState → CExecutionPanel::LoadAssistantsFromJson
// 重复切档（点任务）不应：① 死循环挂死；② 泄漏幽灵助手 Tab；③ 崩溃。
#include <QtTest/QtTest>
#include <QApplication>
#include <QTabWidget>
#include <QJsonArray>
#include <QJsonObject>
#include <QScrollBar>
#include "ui/CExecutionPanel.h"
#include "ui/CFilePreviewWidget.h"
#include "ui/CMessageView.h"

#ifdef Q_OS_WIN
#include <windows.h>
#endif

namespace {
QJsonObject MakeAsst(const QString& a_sName, int a_iBlocks) {
    QJsonArray aBlocks;
    for (int i = 0; i < a_iBlocks; ++i) {
        QJsonObject o;
        o["kind"] = (i % 3 == 0) ? "tool_use" : "assistant";
        o["text"] = QStringLiteral("第%1条").arg(i);
        o["tool"] = "Read";
        o["input"] = "{}";
        o["result"] = "ok";
        aBlocks.append(o);
    }
    QJsonObject o;
    o["id"] = QUuid::createUuid().toString(QUuid::WithoutBraces);
    o["name"] = a_sName;
    o["closed"] = false;
    o["blocks"] = aBlocks;
    return o;
}
QJsonArray RoundTrip(const QJsonArray& a_aAssts) {
    // 直接构造等价输入（面板 AssistantsToJson → 再次 Load 即一次切档）
    return a_aAssts;
}
} // namespace

class TestExecPanel : public QObject {
    Q_OBJECT
private slots:
    void taskSwitchLoop();
    void crossAssistantQueue();
    void filePreviewEncoding();
    void viewPagination();
};

void TestExecPanel::taskSwitchLoop() {
    CExecutionPanel panel;
    panel.resize(1000, 700);
    panel.show();
    QTabWidget* pTabs = panel.findChild<QTabWidget*>();
    QVERIFY(pTabs != nullptr);

    // 初始载入：两个助手（一个带历史块，一个空），激活第一个。
    QJsonArray aAssts;
    aAssts.append(MakeAsst(QStringLiteral("助手A"), 45));   // 45 块 → 跨 3 个分帧
    aAssts.append(MakeAsst(QStringLiteral("助手B"), 8));
    panel.LoadAssistantsFromJson(aAssts, QString());
    QTest::qWait(200);   // 分帧重建跑完

    // 关键回归：反复「点任务」= 反复切档（拆除重建 + 载入）。
    // 旧实现：RemoveAsstTab 摘除当前助手页时 Qt 自动把「+」占位页设为当前页 →
    // OnAsstTabCurrentChanged → OnAsstAddClicked 在拆除循环遍历 m_aAssts 期间
    // append 修改容器（迭代器失效 UB）且清一个生一个 → 死循环卡死；载入路径走
    // ResetAssistants 还会每切一次多出一个空「助手1」。
    const int iTabs0 = pTabs->count();
    for (int i = 0; i < 25; ++i) {
        panel.LoadAssistantsFromJson(RoundTrip(aAssts), QString());
        QTest::qWait(30);
    }
    // 断言：Tab 数不随切档次数增长（无幽灵页累积）。
    QCOMPARE(pTabs->count(), iTabs0);
    // 断言：助手数量保持 2（不含内部复位的默认助手）。
    QCOMPARE(panel.AsstCount(), 2);
    // 断言：序列化往返仍为 2 个助手。
    QCOMPARE(panel.AssistantsToJson().size(), 2);
    panel.hide();
}

// 2026.08.27 跨助手排队回归：助手 A 运行中，切到/新建助手 B 后发送——指令不得
//   误注入 A 的运行轮，应排队到 B；A 轮结束后自动发送到 B（换载 B 会话后提交）。
void TestExecPanel::crossAssistantQueue() {
    // 2026.09.03 多助手并发回归：每个助手是独立 Agent——A 运行中切到 B 发送，
    //   B 的指令应【立即提交】（不再排队等待 A），A/B 同时进入运行态（各 ● 标记），
    //   A 结束不影响 B 的运行态；同助手运行中再发仍为追加提交（注入语义）。
    CExecutionPanel panel;
    panel.resize(1000, 700);
    panel.show();
    QTabWidget* pTabs = panel.findChild<QTabWidget*>();
    QVERIFY(pTabs != nullptr);

    QJsonArray aAssts;
    aAssts.append(MakeAsst(QStringLiteral("助手A"), 2));
    aAssts.append(MakeAsst(QStringLiteral("助手B"), 2));
    panel.LoadAssistantsFromJson(aAssts, QString());
    QTest::qWait(100);   // 分帧重建跑完；激活=第一个助手 A
    panel.SetAdhocEnabled(true);

    // A 运行中（激活助手即 A → 运行归属 A）。
    const QString sA = panel.ActiveAsstId();
    panel.SetAsstRunning(sA, true);
    QVERIFY(panel.AsstRunning(sA));
    // 用户切到 B（新建助手后即在 B 上输入发送的等价场景）。
    int iB = -1, iA = -1;
    for (int i = 0; i < pTabs->count(); ++i) {
        if (pTabs->tabText(i).startsWith(QStringLiteral("助手B"))) iB = i;
        if (pTabs->tabText(i).startsWith(QStringLiteral("助手A"))) iA = i;
    }
    QVERIFY(iA >= 0 && iB >= 0);
    pTabs->setCurrentWidget(pTabs->widget(iB));
    const QString sB = panel.ActiveAsstId();
    QVERIFY(sA != sB);

    int iSubmits = 0;
    QString sLastId, sLastText;
    QObject::connect(&panel, &CExecutionPanel::sigAdhocSubmit,
                     [&](const QString& sId, const QString& sText, bool) {
                         ++iSubmits; sLastId = sId; sLastText = sText;
                     });

    // B 上发送：立即提交到 B（不排队），B 同时进入运行态。
    panel.AppendToAdhocInput(QStringLiteral("给B的指令"));
    panel.DoAdhocSend();
    QCOMPARE(iSubmits, 1);
    QCOMPARE(sLastId, sB);
    QCOMPARE(sLastText, QStringLiteral("给B的指令"));
    QVERIFY(panel.AsstRunning(sB));
    QVERIFY(panel.AsstRunning(sA));           // A 仍在跑（并发互不影响）
    QVERIFY(pTabs->tabText(iB).contains(QStringLiteral("●")));
    QVERIFY(pTabs->tabText(iA).contains(QStringLiteral("●")));   // 双 ● 并发

    // A 轮结束：只清 A 的运行态，B 不受影响（旧实现全局单运行位会把 B 也带停）。
    panel.OnAdhocFinished(sA);
    QVERIFY(!panel.AsstRunning(sA));
    QVERIFY(panel.AsstRunning(sB));
    QVERIFY(pTabs->tabText(iB).contains(QStringLiteral("●")));
    QVERIFY(!pTabs->tabText(iA).contains(QStringLiteral("●")));

    // 同助手追加语义不变：B 运行中在 B 上再发 → 直接提交（注入运行轮）。
    panel.AppendToAdhocInput(QStringLiteral("继续"));
    panel.DoAdhocSend();
    QCOMPARE(iSubmits, 2);
    QCOMPARE(sLastId, sB);
    QVERIFY(panel.AsstRunning(sB));

    // B 结束：运行态清零。
    panel.OnAdhocFinished(sB);
    QVERIFY(!panel.AsstRunning(sB));
    panel.hide();
}

// 2026.08.26 编码探测回归：GBK（如中文 bat/老源码）不得再乱码。
//   覆盖：UTF-8 BOM / UTF-16LE BOM / 纯 UTF-8 / GBK（含中文路径的 bat 内容）/
//   纯 ASCII（UTF-8 分支命中）/ 二进制判定。
void TestExecPanel::filePreviewEncoding() {
    CFilePreviewWidget w;
    QString sText;
    bool bBinary = false;

    // ① UTF-8 BOM
    {
        const QByteArray a = QByteArray("\xEF\xBB\xBF") + QStringLiteral("你好")
                                 .toUtf8();
        QCOMPARE(w.DetectAndDecode(a, sText, bBinary), QStringLiteral("UTF-8 BOM"));
        QCOMPARE(sText, QStringLiteral("你好"));
    }
    // ② UTF-16LE BOM
    {
        QString s = QChar(0xFEFF);
        s += QStringLiteral("中文");
        const QByteArray a((const char*)s.utf16(), s.size() * 2);
        QCOMPARE(w.DetectAndDecode(a, sText, bBinary), QStringLiteral("UTF-16LE BOM"));
        QCOMPARE(sText, QStringLiteral("中文"));
    }
    // ③ 纯 UTF-8（无 BOM）
    {
        const QByteArray a = QStringLiteral("rem 中文注释")
                                 .toUtf8();
        QCOMPARE(w.DetectAndDecode(a, sText, bBinary), QStringLiteral("UTF-8"));
        QCOMPARE(sText, QStringLiteral("rem 中文注释"));
    }
    // ④ GBK（典型中文 bat：mklink 指向中文路径）
    {
        const QString sSrc = QStringLiteral(
            "mklink /j \"Include\" \"E:\\proj\\40.三化\\模块化\\lib\\Include\"");
        const char* pc = nullptr;   // QString→GBK：无 Qt 内建，用 Windows API
        int n = 0;
        #ifdef Q_OS_WIN
        const wchar_t* pw = (const wchar_t*)sSrc.utf16();
        n = WideCharToMultiByte(936, 0, (LPCWSTR)pw, -1, nullptr, 0, nullptr, nullptr);
        QByteArray aGbk(n - 1, Qt::Uninitialized);
        WideCharToMultiByte(936, 0, (LPCWSTR)pw, -1, aGbk.data(), n, nullptr, nullptr);
        pc = aGbk.constData();
        #endif
        Q_UNUSED(pc)
        #ifdef Q_OS_WIN
        QCOMPARE(w.DetectAndDecode(aGbk, sText, bBinary), QStringLiteral("GB18030/GBK"));
        QCOMPARE(sText, sSrc);   // 解回原文，不再乱码
        #endif
    }
    // ⑤ 纯 ASCII（合法 UTF-8，走 ③ 分支）
    {
        const QByteArray a = QByteArrayLiteral("mklink /j \"Inc\" \"E:\\x\\y\"");
        QCOMPARE(w.DetectAndDecode(a, sText, bBinary), QStringLiteral("UTF-8"));
    }
    // ⑥ Latin1 兜底：非法 UTF-8 且非法 GB18030 的字节（如独立续字节 0x81）不丢字节
    //    注：空字节密集的二进制判定由 ShowFile 调用前预检负责（NUL 是合法 UTF-8/GB 字节，
    //    函数内无法单独判，避免双重逻辑）。bBinary 传入预检结果。
    {
        bBinary = false;
        QByteArray a(1, char(0x81));
        QCOMPARE(w.DetectAndDecode(a, sText, bBinary), QStringLiteral("Latin1"));
        QCOMPARE(sText, QString(QChar(0x81)));
    }
}

// 2026.09.03 反向分页回归：助手流 260 块 → 初始只建最近 200 块；滚动到顶自动补建
//   更早一段（60 块一次补齐）→ 活件数从全量降为窗口+翻页量；数据层（落盘 JSON）不变。
void TestExecPanel::viewPagination() {
    CExecutionPanel panel;
    panel.resize(1000, 700);
    panel.show();
    QTabWidget* pTabs = panel.findChild<QTabWidget*>();
    QVERIFY(pTabs != nullptr);

    QJsonArray aAssts;
    aAssts.append(MakeAsst(QStringLiteral("助手A"), 260));   // 260 块 > 初始窗口 200
    panel.LoadAssistantsFromJson(aAssts, QString());

    // 加载后当前页 = 第一个助手视图（LoadAssistantsFromJson 激活逻辑）
    CMessageView* pV = qobject_cast<CMessageView*>(pTabs->currentWidget());
    QVERIFY(pV != nullptr);

    // 轮询等分帧重建收敛到初始窗口 200（每帧 20 块，Markdown 块不便宜，机器慢时 >300ms）
    auto fnWaitCount = [&pV](int a_iExpect) {
        for (int i = 0; i < 100; ++i) {   // 5s 上限
            if (pV->BlockCount() == a_iExpect) {
                return true;
            }
            QTest::qWait(50);
        }
        return pV->BlockCount() == a_iExpect;
    };
    QVERIFY2(fnWaitCount(200),
             QStringLiteral("初始窗口未收敛到 200：%1").arg(pV->BlockCount()).toUtf8().constData());
    QVERIFY(pV->verticalScrollBar()->maximum() > 0);   // 可滚（否则到顶信号不触发）

    // 滚动到顶 → sigTopReached → 补建更早 60 块（chunk=100 > 60 → 一次补齐）
    pV->verticalScrollBar()->setValue(0);
    QVERIFY2(fnWaitCount(260),
             QStringLiteral("到顶补建未收敛到 260：%1").arg(pV->BlockCount()).toUtf8().constData());

    // 数据层完整无损（分页只影响建件，不影响落盘）
    QJsonArray aOut = panel.AssistantsToJson();
    QCOMPARE(aOut.size(), 1);
    QCOMPARE(aOut.at(0).toObject().value(QStringLiteral("blocks")).toArray().size(), 260);

    // 任务侧流路由（2026.09.03 收拢后同一 CStreamPresenter 承载）：公开 API roundtrip。
    QJsonArray aTask;
    for (int i = 0; i < 300; ++i) {
        QJsonObject oBlock;
        oBlock[QStringLiteral("kind")] = QStringLiteral("assistant");
        oBlock[QStringLiteral("text")] = QStringLiteral("第%1条").arg(i);
        aTask.append(oBlock);
    }
    panel.LoadTaskBlocksFromJson(QStringLiteral("task-t1"), aTask);
    QVERIFY(panel.HasTaskBlocks(QStringLiteral("task-t1")));
    QCOMPARE(panel.TaskBlocksToJson(QStringLiteral("task-t1")).size(), 300);
    panel.hide();
}

QTEST_MAIN(TestExecPanel)
#include "test_execpanel.moc"
