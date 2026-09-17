// 单元测试（test_execpanel）：多助手 Tab「点击任务卡死」回归——模拟任务切换路径
// SwitchAssistantArchive → LoadAssistantState → CExecutionPanel::LoadAssistantsFromJson
// 重复切档（点任务）不应：① 死循环挂死；② 泄漏幽灵助手 Tab；③ 崩溃。
#include <QtTest/QtTest>
#include <QApplication>
#include <QTabWidget>
#include <QJsonArray>
#include <QJsonObject>
#include <QScrollBar>
#include <QLabel>
#include <QToolButton>
#include <QPushButton>
#include <QPlainTextEdit>
#include <QComboBox>   // 2026-09-11 权限下拉回归
#include "ui/CExecutionPanel.h"
#include "core/CTaskInfo.h"   // 2026.09.05 v2.0.120 asstBusyIndicator：构造任务刷底部指示器
#include "ui/CFilePreviewWidget.h"
#include "ui/CMessageView.h"
#include "ui/CAskUserDialog.h"   // 2026-09-13 助手提问对话框尺寸自适应回归
#include <QListWidget>
#include <QHelpEvent>
#include "ui/CTipStyle.h"   // 2026.09.10 v2.2.156 tooltip 字体/换行回归
#include <QToolTip>

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
    // 2026.09.04 v2.0.112 Hermes 工具卡：纯文本 preview 入参的组头摘要（OCR
    //   实测组头仅“… ×N”疑摘要丢失；ToolSummary 对非 JSON 输入应兑底原文）。
    void toolSummaryPlainText();
    // 2026.09.05 per-助手控件隔离：输入框/发送按钮/统计栏随助手 Tab 页切换（独立实例），
    //   助手消耗不再并入任务统计（面板底栏仅任务侧）。
    void perAsstInputAndStats();
    // 2026.09.05 v2.0.120 per-助手执行态指示器：统计栏费用前的转圈指示器只随【该助手】
    //   起停（A 跑 B 不转）；任务侧底部指示器只看任务运行态（助手跑不转，防误读）。
    void asstBusyIndicator();
    // 2026.09.07 v2.1.124 回灌转录折叠：一轮内连续多条 Assistant 块（Hermes v2.0.116
    //   中间段叙述）只留末条（终答）——防回灌历史像"未完成的现行任务"带偏模型。
    void asstTranscript();
    // 2026.09.09 有效选中观察口：CurrentTask 严格跟随 SetCurrentTask（选中/换选/
    //   取消选中均即时）——助手提示词任务信息注入以此为源头，防取消选中后滞留旧任务。
    void currentTaskFollowsSelection();
    // 2026.09.09 右键彻底删除助手（DeleteAsstCore 无弹窗核心层）：被删者从档出局
    //   （AssistantsToJson 不再含，区别于软关闭 closed=true 保留）；其余助手对话/
    //   Tab/激活完整；删除未知 id 无副作用。
    void asstHardDelete();
    // 2026.09.10 v2.2.152 自动命名选行修复回归（CTaskInfo::ExtractNameFromRequest）。
    void testTaskNameExtraction();
    // 2026.09.10 v2.2.153 回归：「载任务=任务日志」统一口径——SetCurrentTask(非空) 自动
    //   选中「日志」页。背景：载助手档（LoadAssistantsFromJson）会激活首个助手 Tab，
    //   再次执行/启动任务后用户停在助手日志（用户反馈逻辑不通）；修复统一收在
    //   SetCurrentTask，任何路径（启动/再次执行/点选任务列表）载任务后都应落在任务日志页。
    void setCurrentTaskSelectsLogPage();
    // 2026.09.10 v2.2.156 回归：全局 tooltip 11 号字体 + 长文本自动换行（用户需求：
    //   设置页提示字体太小统一 11 号、长文本自动换行）。①WrapText 转义/换行/边距归零；
    //   ②ApplyGlobal 后 QToolTip::font()==11pt；③实测 QTipLabel：长富文本提示宽度
    //   有界（自动换行），纯文本则一行拉满不受限。
    void toolTipFontAndWrap();
    // 2026.09.10 田档自愈+编号回归：遗留「已关闭且空」助手（「▾」菜单不可见）不占号——
    //   载入即丢弃并回写净化档；点「+」按可见助手取最小缺号（实害：只见 助手1，
    //   新增却出了 助手7）。
    void asstAddNumberingSkipsLegacyEmptyClosed();
    void asstPermCombo();   // 2026-09-11 权限下拉：三态缺省/独立/提交携带/持久化
    void askUserDialogSizing();   // 2026-09-13 助手提问对话框：高度自适应/Tooltip/其他项增减高度
    void promptFlyoutHideRule();   // 2026-09-13 历史提问浮层：鼠标位置+500ms 容忍收起（不再 Leave 即收）
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
                     [&](const QString& sId, const QString& sText, int) {
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

    // 加载后当前页 = 第一个助手页容器（2026.09.05 Tab 页本体 = 容器：日志视图+输入栏+统计栏）
    QWidget* pPageW = pTabs->currentWidget();
    QVERIFY(pPageW != nullptr);
    CMessageView* pV = pPageW->findChild<CMessageView*>();
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

void TestExecPanel::toolSummaryPlainText() {
    // Hermes v3 事件的 preview 为脱敏纯文本（如 "config.yaml L501"），非 JSON——
    // 组头摘要应兑底原文（经真实 UI 控件文本验证，不依赖 OCR）。
    auto fnHead = [](CMessageBlock* pBlk) -> QString {
        QToolButton* pHead = pBlk->findChild<QToolButton*>(QString(), Qt::FindDirectChildrenOnly);
        return pHead ? pHead->text() : QString();
    };
    CMessageBlock stBlk(CMessageBlock::ToolUse);
    stBlk.SetTool(QStringLiteral("read_file"), QStringLiteral("ChartView.h"));
    stBlk.AddMergedTool(QStringLiteral("read_file"), QStringLiteral("main.cpp"));
    stBlk.show();
    const QString sHead = fnHead(&stBlk);
    QVERIFY2(sHead.contains(QStringLiteral("ChartView.h")) && sHead.contains(QStringLiteral("×2")),
             qPrintable(sHead));
    CMessageBlock stBlk2(CMessageBlock::ToolUse);
    stBlk2.SetTool(QStringLiteral("terminal"), QStringLiteral("cmd /c dir /b *.cpp"));
    stBlk2.show();
    QVERIFY2(fnHead(&stBlk2).contains(QStringLiteral("dir")),
             qPrintable(fnHead(&stBlk2)));
}

// 2026.09.05 per-助手控件隔离回归：每个助手 Tab 页内独立输入框/发送按钮/统计栏——
//   ① 各助手输入至不串（切页保留各向草稿）；② A 运行只锁 A 的发送钮（B 仍「发送」）；
//   ③ AddAsstUsage 只进目标助手统计栏；④ 面板底部费用栏不再显示助手统计（任务专用）。
void TestExecPanel::perAsstInputAndStats() {
    CExecutionPanel panel;
    panel.resize(1000, 700);
    panel.show();
    QTabWidget* pTabs = panel.findChild<QTabWidget*>();
    QVERIFY(pTabs != nullptr);

    QJsonArray aAssts;
    aAssts.append(MakeAsst(QStringLiteral("助手A"), 2));
    aAssts.append(MakeAsst(QStringLiteral("助手B"), 2));
    panel.LoadAssistantsFromJson(aAssts, QString());
    QTest::qWait(100);
    panel.SetAdhocEnabled(true);

    // 逐 Tab 页取输入框/发送钮/统计标签（页容器内各一个）。
    auto fnPageWidgets = [pTabs](const QString& sName)
            -> QPair<QWidget*, QPlainTextEdit*> {
        for (int i = 0; i < pTabs->count(); ++i) {
            if (pTabs->tabText(i).startsWith(sName)) {
                QWidget* pPage = pTabs->widget(i);
                return qMakePair(pPage, pPage->findChild<QPlainTextEdit*>());
            }
        }
        return qMakePair(nullptr, nullptr);
    };
    auto fnSend = [pTabs](const QString& sName) -> QPushButton* {
        for (int i = 0; i < pTabs->count(); ++i) {
            if (pTabs->tabText(i).startsWith(sName)) {
                // 页内有两个 QPushButton（撤销在前）：发送钮按文本三态匹配。
                const QList<QPushButton*> aBtns = pTabs->widget(i)->findChildren<QPushButton*>();
                for (QPushButton* pB : aBtns) {
                    if (pB->text() == QStringLiteral("发送")
                        || pB->text() == QStringLiteral("⏹ 停止")
                        || pB->text() == QStringLiteral("➤ 追加指令")) {
                        return pB;
                    }
                }
            }
        }
        return nullptr;
    };
    auto fnCost = [pTabs](const QString& sName) -> QString {
        for (int i = 0; i < pTabs->count(); ++i) {
            if (pTabs->tabText(i).startsWith(sName)) {
                const QList<QLabel*> aLbls = pTabs->widget(i)->findChildren<QLabel*>();
                for (QLabel* pL : aLbls) {
                    if (pL->text().startsWith(QStringLiteral("费用"))) {
                        return pL->text();
                    }
                }
            }
        }
        return QString();
    };

    QWidget* pPageA = nullptr; QWidget* pPageB = nullptr;
    QPlainTextEdit* pInA = nullptr;  QPlainTextEdit* pInB = nullptr;
    { const auto prA = fnPageWidgets(QStringLiteral("助手A")); pPageA = prA.first; pInA = prA.second;
      const auto prB = fnPageWidgets(QStringLiteral("助手B")); pPageB = prB.first; pInB = prB.second; }
    QVERIFY(pPageA != nullptr && pInA != nullptr);
    QVERIFY(pPageB != nullptr && pInB != nullptr);
    QVERIFY(pInA != pInB);   // 独立实例

    // ① 输入草稿互不串（切页后各自保留）。
    pInA->setPlainText(QStringLiteral("给A的草稿"));
    pInB->setPlainText(QStringLiteral("给B的草稿"));
    pTabs->setCurrentWidget(pPageB);
    pTabs->setCurrentWidget(pPageA);
    QCOMPARE(pInA->toPlainText(), QStringLiteral("给A的草稿"));
    QCOMPARE(pInB->toPlainText(), QStringLiteral("给B的草稿"));

    // ② A 运行只锁 A 的发送钮；B 的仍可发送。A 有草稿 → 「➤ 追加指令」；清空 → 「⏹ 停止」。
    const QString sA = panel.ActiveAsstId();
    panel.SetAsstRunning(sA, true);
    QPushButton* pBtnA = fnSend(QStringLiteral("助手A"));
    QPushButton* pBtnB = fnSend(QStringLiteral("助手B"));
    QVERIFY(pBtnA != nullptr && pBtnB != nullptr);
    QCOMPARE(pBtnA->text(), QStringLiteral("➤ 追加指令"));   // A 有草稿且在跑
    pInA->clear();
    QTest::qWait(30);
    QCOMPARE(pBtnA->text(), QStringLiteral("⏹ 停止"));
    QCOMPARE(pBtnB->text(), QStringLiteral("发送"));   // B 不受 A 运行影响

    // ③ 消耗只进目标助手统计栏。
    QString sB;
    for (int i = 0; i < pTabs->count(); ++i) {
        if (pTabs->tabText(i).startsWith(QStringLiteral("助手B"))) {
            pTabs->setCurrentWidget(pTabs->widget(i));
            sB = panel.ActiveAsstId();
        }
    }
    QVERIFY(sA != sB);
    panel.AddAsstUsage(sB, 1000, 500, 0.25, 3000, 2);
    QVERIFY2(fnCost(QStringLiteral("助手B")).contains(QStringLiteral("1.0K")),
             qPrintable(fnCost(QStringLiteral("助手B"))));   // FormatTokens：1000 → 1.0K
    QVERIFY(!fnCost(QStringLiteral("助手A")).contains(QStringLiteral("1.0K")));
    panel.AddAsstUsage(sA, 7, 3, 0.0, 100, 1);
    QVERIFY(fnCost(QStringLiteral("助手A")).contains(QStringLiteral("7")));

    // ④ 面板底部费用栏不再显示助手统计（任务专用；当前无任务 → 置空/不含助手数字）。
    const QList<QLabel*> aPanelLbls = panel.findChildren<QLabel*>(QString(), Qt::FindDirectChildrenOnly);
    for (QLabel* pL : aPanelLbls) {
        QVERIFY2(!pL->text().contains(QStringLiteral("输入:1000"))
                 && !pL->text().contains(QStringLiteral("输入:7 ")) || pL->text().isEmpty(),
                 qPrintable(pL->text()));
    }
    panel.hide();
}

// 2026.09.05 v2.0.120 per-助手执行态指示器回归：①每个助手页统计栏费用前有独立指示器；
//   ②指示器只随【该助手】运行态起停（A 跑 B 不转）；③任务侧底部指示器只看任务运行态
//   （助手跑不转，任务跑才转——统计与指示器成对，防「助手跑任务闲」误读为任务在跑）。
void TestExecPanel::asstBusyIndicator() {
    CExecutionPanel panel;
    panel.resize(1000, 700);
    panel.show();
    QTabWidget* pTabs = panel.findChild<QTabWidget*>();
    QVERIFY(pTabs != nullptr);

    const QJsonObject oA = MakeAsst(QStringLiteral("助手A"), 2);
    const QJsonObject oB = MakeAsst(QStringLiteral("助手B"), 2);
    QJsonArray aAssts;
    aAssts.append(oA);
    aAssts.append(oB);
    panel.LoadAssistantsFromJson(aAssts, oA["id"].toString());
    QTest::qWait(100);
    const QString sIdA = oA["id"].toString();
    const QString sIdB = oB["id"].toString();

    // ① 页内各有指示器，初始均不转；且位于统计栏费用标签之前（同页坐标系 x 更小）。
    QWidget* pBusyA = panel.AsstBusyIndicator(sIdA);
    QWidget* pBusyB = panel.AsstBusyIndicator(sIdB);
    QVERIFY(pBusyA != nullptr);
    QVERIFY(pBusyB != nullptr);
    QVERIFY(!CExecutionPanel::BusyIndicatorSpinning(pBusyA));
    QVERIFY(!CExecutionPanel::BusyIndicatorSpinning(pBusyB));
    QTest::qWait(50);   // 布局激活后取几何
    QLabel* pLblCostA = nullptr;
    for (int i = 0; i < pTabs->count() && pLblCostA == nullptr; ++i) {
        if (pTabs->tabText(i).startsWith(QStringLiteral("助手A"))) {
            const QList<QLabel*> aLbls = pTabs->widget(i)->findChildren<QLabel*>();
            for (QLabel* pL : aLbls) {
                if (pL->text().startsWith(QStringLiteral("费用"))) {
                    pLblCostA = pL;
                    break;
                }
            }
        }
    }
    QVERIFY2(pLblCostA != nullptr, "A 页统计栏费用标签缺失");
    QVERIFY2(pBusyA->x() < pLblCostA->x(), "指示器应在费用标签之前");

    // ② A 起跑：只有 A 的指示器转；任务侧底部指示器不受助手影响（当前无任务）。
    panel.SetAsstRunning(sIdA, true);
    QVERIFY(CExecutionPanel::BusyIndicatorSpinning(pBusyA));
    QVERIFY(!CExecutionPanel::BusyIndicatorSpinning(pBusyB));
    QVERIFY(!CExecutionPanel::BusyIndicatorSpinning(panel.TaskBusyIndicator()));
    // A 停：归空闲。
    panel.SetAsstRunning(sIdA, false);
    QVERIFY(!CExecutionPanel::BusyIndicatorSpinning(pBusyA));

    // ③ 任务 Running：底部指示器转；此时 A 也跑 → 底部仍只看任务（已 Running 恒转），
    //   任务离开运行态后即使 A 在跑底部也不转（旧口径 AnyAsstRunning 驱动会转，勿回退）。
    CTaskInfo task;
    task.SetId(QStringLiteral("busy-t-1"));
    task.SetStatus(ETaskStatus::Running);
    panel.SetCurrentTask(&task);
    panel.RefreshTask(&task);
    QVERIFY(CExecutionPanel::BusyIndicatorSpinning(panel.TaskBusyIndicator()));
    panel.SetAsstRunning(sIdB, true);   // 助手 B 同时在跑
    QVERIFY(CExecutionPanel::BusyIndicatorSpinning(pBusyB));
    task.SetStatus(ETaskStatus::Done);  // 任务完成：底部停转（助手仍在跑也不许带动它）
    panel.RefreshTask(&task);
    QVERIFY(!CExecutionPanel::BusyIndicatorSpinning(panel.TaskBusyIndicator()));
    QVERIFY(CExecutionPanel::BusyIndicatorSpinning(pBusyB));   // B 页内照转
    panel.SetAsstRunning(sIdB, false);
    panel.SetCurrentTask(nullptr);
    panel.hide();
}

// 2026.09.07 v2.1.124 回灌转录折叠回归：①一轮内连续多条 Assistant 块只留末条（终答），
//   中间段叙述（Hermes v2.0.116）不进回灌历史；②用户块分段正确（每轮一段）；
//   ③末尾本轮提问（a_sCurText）仍被剔除；④时间戳前缀剥离不变。
// 背景：实测 .claude 工程用户问「你现在用的是什么模型？」，回灌历史里上一轮的
//   record.md 追加叙述让模型把「追加记录」当现行任务执行、完全无视提问。
void TestExecPanel::asstTranscript() {
    CExecutionPanel panel;
    panel.resize(1000, 700);
    panel.show();
    QTabWidget* pTabs = panel.findChild<QTabWidget*>();
    QVERIFY(pTabs != nullptr);

    // 手工构造块序：user(hi) → asst(叙述1) → asst(叙述2) → asst(终答1)
    //              → user(问模型) → asst(带偏的追加叙述) → asst(终答2) → user(本轮新提问)
    QJsonArray aBlocks;
    auto userB = [&aBlocks](const QString& s) {
        QJsonObject o; o["kind"] = "event"; o["eventKind"] = "user"; o["text"] = s;
        aBlocks.append(o);
    };
    auto asstB = [&aBlocks](const QString& s) {
        QJsonObject o; o["kind"] = "assistant"; o["text"] = s;
        aBlocks.append(o);
    };
    userB(QStringLiteral("[2026-09-07 23:20:00] hi"));
    asstB(QStringLiteral("中间叙述一：我先读取 record.md"));
    asstB(QStringLiteral("中间叙述二：date 取当前时刻"));
    asstB(QStringLiteral("终答一：你好！有什么可以帮忙的？"));
    userB(QStringLiteral("[2026-09-07 23:32:50] 你现在用的是什么模型？"));
    asstB(QStringLiteral("中间叙述：我已读取 .autodev/record.md，下面追加一行"));
    asstB(QStringLiteral("终答二：我是 GLM 系模型。"));
    const QString sCur = QStringLiteral("再问一个问题");
    userB(sCur);   // 本轮即将提交的提问（应被剔除）

    QJsonObject oAsst;
    oAsst["id"] = QUuid::createUuid().toString(QUuid::WithoutBraces);
    oAsst["name"] = QStringLiteral("助手A");
    oAsst["closed"] = false;
    oAsst["blocks"] = aBlocks;
    QJsonArray aAssts; aAssts.append(oAsst);
    panel.LoadAssistantsFromJson(aAssts, oAsst["id"].toString());
    QTest::qWait(100);

    const QStringList aLines = panel.BuildAsstTranscript(oAsst["id"].toString(), sCur);
    // ①折叠：共 2 轮 → 2 用户行 + 2 助手行 = 4 条（旧实现叙述全入=7 条）。
    QCOMPARE(aLines.size(), 4);
    // ②每轮只留终答：叙述文本绝不出现在回灌里。
    for (const QString& s : aLines) {
        QVERIFY2(!s.contains(QStringLiteral("中间叙述")), qPrintable(s));
    }
    // ③终答在位、时间戳已剥。
    QVERIFY(aLines.contains(QStringLiteral("用户：hi")));
    QVERIFY(aLines.contains(QStringLiteral("用户：你现在用的是什么模型？")));
    QVERIFY(aLines.contains(QStringLiteral("助手：终答一：你好！有什么可以帮忙的？")));
    QVERIFY(aLines.contains(QStringLiteral("助手：终答二：我是 GLM 系模型。")));
    // ④本轮提问剔除。
    QVERIFY(!aLines.join(QLatin1Char('|')).contains(sCur));
    panel.hide();
}

// 2026.09.10 v2.2.153/155 回归：切换口径全句「选任务=任务日志；无任务选中=助手日志」
// ——SetCurrentTask(非空)自动选中「日志」页；清空（nullptr）切当前激活助手页。
// 复现用户报告链：载助手档把当前页硬跳到激活助手 Tab 后，再次执行/启动任务
// （链路终点 SetCurrentTask）应把用户拉回任务日志，而不是停在助手日志；
// 反向：取消选中/移除/删除任务后应落到助手日志，不停在已清空的任务日志页。
void TestExecPanel::setCurrentTaskSelectsLogPage() {
    CExecutionPanel panel;
    panel.resize(1000, 700);
    panel.show();
    QTabWidget* pTabs = panel.findChild<QTabWidget*>();
    QVERIFY(pTabs != nullptr);

    // 前置：载一份双助手档——LoadAssistantsFromJson 会把当前页切到激活助手 Tab
    // （旧版再次执行后停在助手日志的根源）。
    auto mkAsst = [](const char* a_sId, const char* a_sName) {
        QJsonObject o;
        o["id"] = QString::fromLatin1(a_sId);
        o["name"] = QString::fromLatin1(a_sName);
        o["closed"] = false;
        return o;
    };
    QJsonArray aAssts;
    aAssts.append(mkAsst("log-a", "助手A"));
    aAssts.append(mkAsst("log-b", "助手B"));
    panel.LoadAssistantsFromJson(aAssts, QStringLiteral("log-b"));
    QTest::qWait(50);
    QVERIFY(pTabs->currentIndex() >= 0);
    QVERIFY(pTabs->tabText(pTabs->currentIndex()) != QStringLiteral("日志"));   // 停在助手页

    // ① 载任务：自动切「日志」页（启动/再次执行/点选任务的共同收口）。
    CTaskInfo task;
    task.SetId(QStringLiteral("log-t-1"));
    task.SetStatus(ETaskStatus::Running);
    panel.SetCurrentTask(&task);
    QCOMPARE(pTabs->tabText(pTabs->currentIndex()), QStringLiteral("日志"));

    // ② 再被助手页抢走后重复载任务：仍应拉回「日志」（同 id 重复 SetCurrentTask
    //    也切页，与 SelectTask 重复调用链兼容）。
    panel.ShowActiveAssistantPage();
    QVERIFY(pTabs->tabText(pTabs->currentIndex()) != QStringLiteral("日志"));
    panel.SetCurrentTask(&task);
    QCOMPARE(pTabs->tabText(pTabs->currentIndex()), QStringLiteral("日志"));

    // ③ 清空任务反向切助手页：从「日志」页置空 → 落到激活助手（log-b）页
    //    （取消选中/移除/删除任务后不再停在已清空的任务日志）。
    panel.SetCurrentTask(&task);
    QCOMPARE(pTabs->tabText(pTabs->currentIndex()), QStringLiteral("日志"));
    panel.SetCurrentTask(nullptr);
    QVERIFY(pTabs->tabText(pTabs->currentIndex()) != QStringLiteral("日志"));
    panel.hide();
}

// 2026.09.09 有效选中观察口回归：CurrentTask() 必须严格跟随 SetCurrentTask
// （选中即时指向、取消选中即时置空）。背景：助手提示词的任务信息注入以本值为准
// （CMainWindow::OnAdhocSubmit），若它与用户可见选中脱钩（如取消选中后仍滞留旧任务），
// 未选任务的新提示词仍会携带旧任务的【任务经验记忆】等任务信息。
void TestExecPanel::currentTaskFollowsSelection() {
    CExecutionPanel panel;
    panel.resize(900, 600);
    panel.show();
    QVERIFY(panel.CurrentTask() == nullptr);   // 初始未选

    CTaskInfo taskA;
    taskA.SetId(QStringLiteral("cur-t-a"));
    panel.SetCurrentTask(&taskA);
    QVERIFY(panel.CurrentTask() == &taskA);   // 选中即时生效

    CTaskInfo taskB;
    taskB.SetId(QStringLiteral("cur-t-b"));
    panel.SetCurrentTask(&taskB);
    QVERIFY(panel.CurrentTask() == &taskB);   // 换选即时切换

    panel.SetCurrentTask(nullptr);
    QVERIFY(panel.CurrentTask() == nullptr);   // 取消选中即时置空（勿滞后）
    panel.hide();
}

// 2026.09.09 右键彻底删除助手回归（DeleteAsstCore 无弹窗核心层；UI 层守卫/确认弹窗
// 不在测试范围）：①被删者永久出局（档 JSON 不再含，vs 软关闭 closed=true 保留）；
// ②其余助手 Tab/对话/激活完整；③删未知 id 无副作用。
void TestExecPanel::asstHardDelete() {
    CExecutionPanel panel;
    panel.resize(1000, 700);
    panel.show();
    QTabWidget* pTabs = panel.findChild<QTabWidget*>();
    QVERIFY(pTabs != nullptr);

    // 三个助手：A（含用户提问块，非空）、B、C。
    const QJsonObject oA = MakeAsst(QStringLiteral("助手A"), 3);
    const QJsonObject oB = MakeAsst(QStringLiteral("助手B"), 2);
    const QJsonObject oC = MakeAsst(QStringLiteral("助手C"), 1);
    QJsonArray aAssts; aAssts.append(oA); aAssts.append(oB); aAssts.append(oC);
    const QString sIdA = oA["id"].toString();
    const QString sIdB = oB["id"].toString();
    panel.LoadAssistantsFromJson(aAssts, sIdA);   // 激活 A
    QTest::qWait(50);
    const int iTabsBefore = pTabs->count();
    QVERIFY(panel.ActiveAsstId() == sIdA);

    // ③未知 id：无副作用。
    panel.DeleteAsstCore(QStringLiteral("no-such-id"));
    QCOMPARE(pTabs->count(), iTabsBefore);

    // ①删除激活中的 A（最严路径：RemoveAsstTab 预切 + 激活校正）。
    panel.DeleteAsstCore(sIdA);
    const QJsonArray aAfter = panel.AssistantsToJson();
    QStringList aIdsAfter;
    for (const QJsonValue& v : aAfter) {
        aIdsAfter.append(v.toObject()["id"].toString());
    }
    QVERIFY(!aIdsAfter.contains(sIdA));   // 永久出局（软关闭会以 closed=true 保留）
    QVERIFY(aIdsAfter.contains(sIdB));    // 其余完好
    QCOMPARE(pTabs->count(), iTabsBefore - 1);

    // ②激活校正：激活 id 有效且指向存在的助手；当前页为助手页（非「+」/日志页）。
    const QString sAct = panel.ActiveAsstId();
    QVERIFY(!sAct.isEmpty() && sAct != sIdA);
    QVERIFY(sAct == sIdB || sAct == oC["id"].toString());

    panel.hide();
}


// 2026.09.10 v2.2.152 自动命名选行修复回归：首个非空行（总起句）优先 + 编号/引导语
// 剥离 + 首子句截断；首行泛型退最长行。实害：Git 工具任务 11 条编号需求，旧「最长行
// 优先」选中 79 字压线的第 7 条细节条款，抠出「资源管理器右键菜单增加」丢掉主旨。
void TestExecPanel::testTaskNameExtraction() {
    // ① 实害案例（本次用户任务描述）：首行总起句→剥引导语「我想开发一个」→首子句
    const QString sGit = QStringLiteral(
        "我想开发一个自动拉取代码、提交代码、推送代码的工具，如果发现冲突，需要自动修复。\n"
        "1.主界面左边显示资源管理器，参照：AutoCode\n"
        "2.主界面右边显示Tab页\n"
        "7.资源管理器右键菜单增加：拉取、提交和推送。拉取过程中如果出现冲突，需要自动处理冲突。点击提交时自动填写修改的日志并且提交。推送时弹出对话框输入远端分支名称");
    QCOMPARE(CTaskInfo::ExtractNameFromRequest(sGit),
             QStringLiteral("自动拉取代码、提交代码、推送代码的工具"));
    // ② 单行短需求：剥引导语保留本体
    QCOMPARE(CTaskInfo::ExtractNameFromRequest(
                 QStringLiteral("创建一个网页版飞机大战游戏")),
             QStringLiteral("网页版飞机大战游戏"));
    // ③ 首行泛型（如题）：退「最长且 ≤80 字」行（编号剥离）
    const QString sGenericHead = QStringLiteral(
        "如题\n"
        "1.把主界面的登录按钮换成圆角样式，并且加上记住密码功能");
    QCOMPARE(CTaskInfo::ExtractNameFromRequest(sGenericHead),
             QStringLiteral("把主界面的登录按钮换成圆角样式"));   // 首子句≥10 字截断（旧口径不变）
    // ④ 引导语剥后 <4 字回退剥前原文（防过度剥离）
    QCOMPARE(CTaskInfo::ExtractNameFromRequest(QStringLiteral("帮我修一下 bug")),
             QStringLiteral("修一下 bug"));   // 剥「帮我」后 6 字 ≥ 4，采用剥离结果
    // ⑤ 空需求/全空白行
    QVERIFY(CTaskInfo::ExtractNameFromRequest(QString()).isEmpty());
    QVERIFY(CTaskInfo::ExtractNameFromRequest(QStringLiteral(" \n \n ")).isEmpty());
}

// 2026.09.10 v2.2.156 全局 tooltip 样式回归：11 号字体 + 长文本自动换行。
//   ①WrapText 字符串层；②ApplyGlobal 字体生效；③真实 QTipLabel 宽度实测——
//   富文本提示自动换行（宽度有界），纯文本一行拉满不受限（对照组）。
void TestExecPanel::toolTipFontAndWrap() {
    // ① 字符串层：富文本结构 + HTML 转义 + \n→<br> + 段落边距归零。
    const QString sWrapped = CTipStyle::WrapText(
        QStringLiteral("第一行\n第二行 <b>非标签</b> & 专治"));
    QVERIFY(Qt::mightBeRichText(sWrapped));
    QVERIFY(sWrapped.contains(QStringLiteral("<br>")));
    QVERIFY(!sWrapped.contains(QStringLiteral("<b>非标签</b>")));   // <>& 已转义
    QVERIFY(sWrapped.contains(QStringLiteral("margin-top:0")));

    // ② 全局字体：ApplyGlobal 后 tooltip 类字体 11 号（点数，随 DPI 缩放）。
    CTipStyle::ApplyGlobal();
    QCOMPARE(QToolTip::font().pointSize(), 11);

    // ③ 真实提示窗实测：长文本（300+ 字）纯文本 vs 富文本提示宽度。
    CExecutionPanel panel;   // 提示需要一个可见父场景（offscreen 下照常建窗）
    panel.resize(900, 600);
    panel.show();
    const QString sLong = QStringLiteral(
        "设置页提示文本很长需要自动换行的完整测试句子，持续追加到足够长以验证宽度有界。").repeated(12);
    auto fnTipWidth = [&panel](const QString& a_sText) -> int {
        QToolTip::showText(panel.mapToGlobal(QPoint(20, 20)), a_sText);
        int iW = 0;
        const QWidgetList aTop = QApplication::topLevelWidgets();
        for (QWidget* pW : aTop) {
            if ((pW->windowFlags() & Qt::ToolTip) && pW->inherits("QLabel")) {
                iW = pW->width();   // QTipLabel：Qt 自绘提示 QLabel
            }
        }
        QToolTip::hideText();
        return iW;
    };
    const int iPlain = fnTipWidth(sLong);            // 对照：纯文本一行拉满
    const int iRich = fnTipWidth(CTipStyle::WrapText(sLong));   // 富文本自动换行
    QVERIFY2(iPlain > 0, "offscreen 下未找到 QTipLabel（环境变化需重审本用例）");
    QVERIFY2(iRich > 0, "offscreen 下未找到 QTipLabel（环境变化需重审本用例）");
    // 富文本宽度有界（自动换行生效）：显著窄于纯文本拉满宽度，且不超屏宽合理上限。
    QVERIFY2(iRich < iPlain, qPrintable(QStringLiteral("富文本(%1) 应窄于纯文本(%2)").arg(iRich).arg(iPlain)));
    QVERIFY2(iRich <= 800, qPrintable(QStringLiteral("富文本宽度超限: %1").arg(iRich)));
    panel.hide();
}

// 2026.09.10 田档自愈+编号回归（实害：HarnessAgent tasks/c22ce112 田里助手2~助手6 均为
//   closed 且 0 块的旧版遗留，「▾」菜单因空助手被过滤只显示 助手1，而「+」编号把
//   隐形空项也数进去 → 新增出 助手7）：①载入即丢弃遗留空关闭项并回写净化档；
//   ②点「+」按可见助手（打开的 + 有用户提示词的已关闭）取最小缺号 → 助手2。
void TestExecPanel::asstAddNumberingSkipsLegacyEmptyClosed() {
    CExecutionPanel panel;
    panel.resize(1000, 700);
    panel.show();
    QTabWidget* pTabs = panel.findChild<QTabWidget*>();
    QVERIFY(pTabs != nullptr);
    QSignalSpy aSpyHeal(&panel, &CExecutionPanel::sigAsstLayoutChanged);

    // 用户提示词块（Event/user）：助手“非空”的判定依据。
    QJsonObject oUserEvt;
    oUserEvt[QStringLiteral("kind")]      = QStringLiteral("event");
    oUserEvt[QStringLiteral("eventKind")] = QStringLiteral("user");
    oUserEvt[QStringLiteral("text")]      = QStringLiteral("帮我做点事");

    // 复刻实害田：打开的改名助手 + 助手1（closed、有提示词，菜单可见）+
    //   助手2~助手6（closed、0 块，旧版遗留，菜单不可见却占号）。
    QJsonObject oOpen = MakeAsst(QStringLiteral("开发"), 0);
    {
        QJsonArray aBlocks;
        aBlocks.append(oUserEvt);
        oOpen[QStringLiteral("blocks")] = aBlocks;
    }
    QJsonObject oClosed1 = MakeAsst(QStringLiteral("助手1"), 0);
    {
        QJsonArray aBlocks;
        aBlocks.append(oUserEvt);
        oClosed1[QStringLiteral("blocks")] = aBlocks;
        oClosed1[QStringLiteral("closed")] = true;
    }
    QJsonArray aAssts;
    aAssts.append(oOpen);
    aAssts.append(oClosed1);
    for (int iN = 2; iN <= 6; ++iN) {
        QJsonObject oLegacy = MakeAsst(QStringLiteral("助手%1").arg(iN), 0);
        oLegacy[QStringLiteral("closed")] = true;   // 0 块：遗留空关闭项
        aAssts.append(oLegacy);
    }
    panel.LoadAssistantsFromJson(aAssts, oOpen[QStringLiteral("id")].toString());
    QTest::qWait(50);

    // ① 田自愈：空关闭项被丢弃（助手总数 = 2），且触发净化田回写信号。
    QCOMPARE(panel.AsstCount(), 2);
    QVERIFY(aSpyHeal.count() >= 1);
    QStringList aNames;
    for (const QJsonValue& v : panel.AssistantsToJson()) {
        aNames.append(v.toObject()[QStringLiteral("name")].toString());
    }
    QCOMPARE(aNames.count(), 2);
    QVERIFY(aNames.contains(QStringLiteral("助手1")));    // 有提示词的已关闭助手保留
    QVERIFY(!aNames.contains(QStringLiteral("助手3")));   // 遗留空项已出局

    // ② 点「+」：编号取可见助手中缺失的最小号 → 助手2（旧行为数到隐形空项 → 助手7）。
    int iAddTab = -1;
    for (int i = 0; i < pTabs->count(); ++i) {
        if (pTabs->tabText(i) == QStringLiteral("+")) {
            iAddTab = i;
            break;
        }
    }
    QVERIFY(iAddTab >= 0);
    pTabs->setCurrentIndex(iAddTab);   // → OnAsstTabCurrentChanged → OnAsstAddClicked
    QTest::qWait(30);
    aNames.clear();
    for (const QJsonValue& v : panel.AssistantsToJson()) {
        aNames.append(v.toObject()[QStringLiteral("name")].toString());
    }
    QVERIFY(aNames.contains(QStringLiteral("助手2")));   // 不再跳号
    QCOMPARE(panel.AsstCount(), 3);
    panel.hide();
}

// 2026-09-11 权限下拉（原「只读」勾选钮三态化）：
// ① 每助手页一个 QComboBox，三态文本正确，缺省 0＝自动审批（旧档无 perm 键兼容）；
// ② 各助手独立切换互不影响；③ 提交携带所选权限值（sigAdhocSubmit 第三参）；
// ④ 选择随助手持久化（AssistantsToJson 带 perm；重载档后下拉回显）。
void TestExecPanel::asstPermCombo() {
    CExecutionPanel panel;
    panel.resize(1000, 700);
    panel.show();
    QTabWidget* pTabs = panel.findChild<QTabWidget*>();
    QVERIFY(pTabs != nullptr);
    QJsonArray aAssts;
    aAssts.append(MakeAsst(QStringLiteral("助手A"), 2));   // 旧档无 perm 键 → 应回缺省 0
    aAssts.append(MakeAsst(QStringLiteral("助手B"), 2));
    panel.LoadAssistantsFromJson(aAssts, QString());
    QTest::qWait(50);
    panel.SetAdhocEnabled(true);

    // 按页取权限下拉（页内仅一个 QComboBox）。
    auto fnCombo = [pTabs](const QString& sName) -> QComboBox* {
        for (int i = 0; i < pTabs->count(); ++i) {
            if (pTabs->tabText(i).startsWith(sName)) {
                return pTabs->widget(i)->findChild<QComboBox*>();
            }
        }
        return nullptr;
    };
    QComboBox* pCmA = fnCombo(QStringLiteral("助手A"));
    QComboBox* pCmB = fnCombo(QStringLiteral("助手B"));
    QVERIFY(pCmA != nullptr && pCmB != nullptr);
    // ① 三态文本 + 缺省自动审批（旧档兼容）
    QCOMPARE(pCmA->count(), 3);
    QCOMPARE(pCmA->itemText(0), QStringLiteral("自动审批"));
    QCOMPARE(pCmA->itemText(1), QStringLiteral("手工审批"));
    QCOMPARE(pCmA->itemText(2), QStringLiteral("只读"));
    QCOMPARE(pCmA->currentIndex(), 0);
    QCOMPARE(pCmB->currentIndex(), 0);

    // ② 各自切换：A→只读(2)、B→手工审批(1)，互不影响
    pCmA->setCurrentIndex(2);
    pCmB->setCurrentIndex(1);
    QTest::qWait(30);
    QCOMPARE(pCmA->currentIndex(), 2);
    QCOMPARE(pCmB->currentIndex(), 1);

    // ③ 提交携带权限值：切到 B 页后发送（激活助手的下拉值随提交携带）
    int iGotPerm = -1;
    QObject::connect(&panel, &CExecutionPanel::sigAdhocSubmit,
                     [&](const QString&, const QString&, int a_iPerm) {
                         iGotPerm = a_iPerm;
                     });
    for (int i = 0; i < pTabs->count(); ++i) {
        if (pTabs->tabText(i).startsWith(QStringLiteral("助手B"))) {
            pTabs->setCurrentIndex(i);
            break;
        }
    }
    QTest::qWait(30);
    panel.AppendToAdhocInput(QStringLiteral("测权限"));
    panel.DoAdhocSend();
    QTest::qWait(30);
    QCOMPARE(iGotPerm, 1);   // B＝手工审批

    // ④ 持久化：ToJson 带 perm；新面板重载后下拉回显各自选择
    const QJsonArray aSaved = panel.AssistantsToJson();
    bool bSawA = false, bSawB = false;
    for (const QJsonValue& v : aSaved) {
        const QJsonObject o = v.toObject();
        if (o[QStringLiteral("name")].toString() == QStringLiteral("助手A")) {
            bSawA = true;
            QCOMPARE(o[QStringLiteral("perm")].toInt(), 2);
        } else if (o[QStringLiteral("name")].toString() == QStringLiteral("助手B")) {
            bSawB = true;
            QCOMPARE(o[QStringLiteral("perm")].toInt(), 1);
        }
    }
    QVERIFY(bSawA && bSawB);
    CExecutionPanel panel2;
    panel2.resize(1000, 700);
    panel2.show();
    panel2.LoadAssistantsFromJson(aSaved, QString());
    QTest::qWait(50);
    QTabWidget* pTabs2 = panel2.findChild<QTabWidget*>();
    auto fnCombo2 = [pTabs2](const QString& sName) -> QComboBox* {
        for (int i = 0; i < pTabs2->count(); ++i) {
            if (pTabs2->tabText(i).startsWith(sName)) {
                return pTabs2->widget(i)->findChild<QComboBox*>();
            }
        }
        return nullptr;
    };
    QCOMPARE(fnCombo2(QStringLiteral("助手A"))->currentIndex(), 2);
    QCOMPARE(fnCombo2(QStringLiteral("助手B"))->currentIndex(), 1);
    panel.hide();
    panel2.hide();
}

// 2026-09-13 助手提问对话框（ask_user / --ask 共用）尺寸自适应：
// ① 短问题：窗口紧致、无 Tooltip；② 超长问题：高度撑到屏幕上限并截断 + 标签 Tooltip
//   含完整问题（富文本）；③ 选「其他（自行输入）」→ 窗口高度增加输入框高度（96），
//   切回普通选项 → 高度收回。
void TestExecPanel::askUserDialogSizing() {
    const QStringList aOpts = { QStringLiteral("同意"), QStringLiteral("拒绝") };

    // ① 短问题：紧致 + 无 Tooltip（Tooltip 只在截断时设在问题标签上）
    CAskUserDialog d1(QStringLiteral("确认执行删除操作？"), aOpts);
    d1.show();
    QVERIFY(QTest::qWaitForWindowExposed(&d1));
    QVERIFY(d1.height() <= 420);            // 短问题不撑高（旧固定 360；现在 ≤ 上限内紧致）
    QVERIFY(d1.findChild<QLabel*>()->toolTip().isEmpty());   // 完整显示 → 无 Tooltip

    // ③ 其他项高度增减（短问题窗口有余量，不受屏幕上限干扰）
    QListWidget* pList = d1.findChild<QListWidget*>();
    QVERIFY(pList != nullptr);
    const int iH0 = d1.height();
    pList->setCurrentRow(pList->count() - 1);   // 「其他（自行输入）」
    QTest::qWait(30);
    QVERIFY(d1.height() >= iH0 + 90);           // 窗口加高 ≈ 输入框高度 96
    QPlainTextEdit* pEditOther = d1.findChild<QPlainTextEdit*>();
    QVERIFY(pEditOther != nullptr && pEditOther->isVisible());
    pList->setCurrentRow(0);                     // 切回普通选项
    QTest::qWait(30);
    QVERIFY(d1.height() <= iH0 + 5);             // 高度收回
    d1.close();

    // ② 超长问题：高度撑到上限 + Tooltip 含完整问题
    QString sLong;
    for (int i = 0; i < 120; ++i) {
        sLong += QStringLiteral("这是一段很长的提问内容，用来验证窗口高度自适应与 Tooltip 兑底。编号%1。").arg(i);
    }
    CAskUserDialog d2(sLong, aOpts);
    d2.show();
    QVERIFY(QTest::qWaitForWindowExposed(&d2));
    QVERIFY(d2.height() >= 400);            // 拉高（钳在屏幕可用区 3/4）
    QLabel* pLbl = d2.findChild<QLabel*>();
    QVERIFY(pLbl != nullptr);
    QVERIFY(!pLbl->toolTip().isEmpty());    // 截断 → Tooltip 兑底（设在问题标签上）
    QVERIFY(pLbl->toolTip().contains(QStringLiteral("编号119")));   // 富文本里含末尾全文
    // 布局激活后高度仍稳定在屏幕上限内（wordWrap hfw 布局会把窗口最小高度顶到全文
    //   高度的回归：标签须 min=max 钉死，否则 after-show 钳位后又被撑大）
    QTest::qWait(150);
    const int iBound = QGuiApplication::primaryScreen()->availableGeometry().height() * 3 / 4 + 60;
    QVERIFY(d2.height() <= iBound);
    QVERIFY(d2.height() >= 400);
    d2.close();
}

// 2026-09-13 历史提问浮层（≡按钮下拉）收起规则回归：鼠标位置 + 500ms 容忍。
//   旧规则：浮层 Leave 即收——鼠标移到条目 Tooltip（贴光标在浮层外沿展开）上读全文时
//   浮层连同 Tooltip 一起消失；新规则：离开区域满 500ms 才收，在 浮层∪按钮∪Tooltip
//   区域内则续命重查。
void TestExecPanel::promptFlyoutHideRule() {
    CExecutionPanel panel;
    panel.move(20, 20);
    panel.resize(700, 600);
    panel.show();
    QVERIFY(QTest::qWaitForWindowExposed(&panel));
    panel.SetAdhocEnabled(true);
    const QString sId = panel.ActiveAsstId();
    for (int i = 0; i < 3; ++i) {
        panel.OnAdhocEvent(sId, QStringLiteral("user"),
                           QStringLiteral("提问%1：这是一条比较长的历史提问内容，用于浮层条目与 tooltip。").arg(i));
    }
    // 浮层（无 Q_OBJECT，已设 objectName 供寻址）与触发按钮（按 tooltip 文本找）
    QWidget* pFly = panel.findChild<QWidget*>(QStringLiteral("PromptFlyout"));
    QVERIFY(pFly != nullptr);
    QToolButton* pBtn = nullptr;
    for (QToolButton* pB : panel.findChildren<QToolButton*>()) {
        if (pB->toolTip().contains(QStringLiteral("历史提问"))) {
            pBtn = pB;
            break;
        }
    }
    QVERIFY(pBtn != nullptr);

    QEvent evEnter(QEvent::Enter), evLeave(QEvent::Leave);
    QApplication::sendEvent(pBtn, &evEnter);   // hover 按钮 → 弹浮层
    QVERIFY(pFly->isVisible());

    // ① hover 列表项（合成 ToolTip 事件）→ 立即显示自建全文 tip 窗（鼠标穿透）；
    //    鼠标移到 tip 上读全文：旧规则 Leave 即收；新规则 500ms 检查点在 tip 区域
    //    内 → 浮层与 tip 都续命不收（QTipLabel 方案会被 app 级 Leave 规则 ~300ms 自毁）。
    QListWidget* pList = pFly->findChild<QListWidget*>();
    QVERIFY(pList != nullptr);
    const QRect rcFly = pFly->frameGeometry();
    const QPoint ptItem = pList->visualItemRect(pList->item(0)).center();
    QCursor::setPos(pList->viewport()->mapToGlobal(ptItem));
    QHelpEvent evTip(QEvent::ToolTip, ptItem, QCursor::pos());
    QApplication::sendEvent(pList->viewport(), &evTip);   // 懒创建：先发事件再取窗
    QWidget* pTipW = pFly->findChild<QLabel*>(QStringLiteral("PromptFlyoutTip"));
    QVERIFY(pTipW != nullptr && pTipW->isVisible());   // 自建 tip 窗已显
    QCursor::setPos(pTipW->frameGeometry().center());   // 鼠标移到 tip 上（浮层外）
    QApplication::sendEvent(pFly, &evLeave);
    QTest::qWait(300);
    QVERIFY(pFly->isVisible());                // <500ms：不收
    QVERIFY(pTipW->isVisible());
    QTest::qWait(450);                         // 过 500ms 检查点：在 tip 区域 → 续命
    QVERIFY(pFly->isVisible());
    QVERIFY(pTipW->isVisible());               // 自建穿透 tip 不受 Leave 规则影响

    // ② 鼠标离开 tip/全部区域 → 满 500ms 后收起，且 tip 窗一并藏掉（防孤儿滞留）
    QCursor::setPos(rcFly.bottomRight() + QPoint(-120, 150));   // 浮层下方远处（屏内）
    QTest::qWait(900);
    QVERIFY(!pFly->isVisible());
    QVERIFY(!pTipW->isVisible());

    // ③ 无 Tooltip、光标远在别处：Leave 后 <500ms 不收、>500ms 收
    QApplication::sendEvent(pBtn, &evEnter);
    QVERIFY(pFly->isVisible());
    QCursor::setPos(rcFly.bottomRight() + QPoint(-120, 150));   // 浮层下方远处（屏内）
    QApplication::sendEvent(pFly, &evLeave);
    QTest::qWait(250);
    QVERIFY(pFly->isVisible());
    QTest::qWait(400);
    QVERIFY(!pFly->isVisible());

    // ④ Enter 浮层取消计时：Leave 后马上又 Enter（光标在浮层内）→ 不收
    QApplication::sendEvent(pBtn, &evEnter);
    QVERIFY(pFly->isVisible());
    QCursor::setPos(pFly->frameGeometry().center());
    QApplication::sendEvent(pFly, &evLeave);
    QApplication::sendEvent(pFly, &evEnter);
    QTest::qWait(900);
    QVERIFY(pFly->isVisible());
    pFly->hide();
}

QTEST_MAIN(TestExecPanel)
#include "test_execpanel.moc"
