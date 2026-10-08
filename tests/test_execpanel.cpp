// 单元测试（test_execpanel）：多助手 Tab「点击任务卡死」回归——模拟任务切换路径
// SwitchAssistantArchive → LoadAssistantState → CExecutionPanel::LoadAssistantsFromJson
// 重复切档（点任务）不应：① 死循环挂死；② 泄漏幽灵助手 Tab；③ 崩溃。
#include <QtTest/QtTest>
#include <QApplication>
#include <QTabWidget>
#include <QTabBar>   // 2026.09.19 Tab 运行闪点回归；2026-09-22 起闪点由 CAsstTabBar 自绘（观察口改 PageRunning）
#include <QJsonArray>
#include <QJsonObject>
#include <QScrollBar>
#include <QEnterEvent>     // 2026.09.26 按钮面板悬停回归（手工发 Enter/Leave）
#include <QWheelEvent>     // 菜单内嵌滚轮回归
#include <QMenu>
#include <QWidgetAction>
#include <QAbstractScrollArea>
#include <numeric>          // std::accumulate（行宽求和断言）
#include <QLabel>
#include <QToolButton>
#include <QPushButton>
#include <QPlainTextEdit>
#include <QFrame>
#include <QSignalSpy>
#include "ui/CRecentProjectsDialog.h"
#include "ui/CRecentChipsPanel.h"   // 2026.09.26 v3.1.239 最近打开按钮面板回归
#include <QTemporaryDir>
#include <QDebug>
#include <QComboBox>   // 2026-09-11 权限下拉回归
#include <QProgressBar>   // 2026-10-05 上下文进度条模型名回归（format 观察口）
#include "ui/CExecutionPanel.h"
#include "ui/CAsstTabBar.h"   // 2026-09-22 多行 Tab 栏（闪点观察口 PageRunning）
#include "core/CTaskInfo.h"   // 2026.09.05 v2.0.120 asstBusyIndicator：构造任务刷底部指示器
#include "ui/CFilePreviewWidget.h"
#include <QTextBrowser>   // 2026.09.22 文件内查找：选中验证
#include <QTextCursor>
#include <QTemporaryFile>
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
    // 2026.09.22 文件内查找（工具栏/Ctrl+F）：向下/向上/循环/区分大小写 + 命中选中。
    void filePreviewFind();
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
    // 2026.09.23 v3.1.227 回归：无任务选中时阶段条自隐藏（日志 Tab 顶上移到原任务
    //   进度位置）；载入任务（流非空）恢复显示。
    void stageBarHidesWithoutTask();
    // 2026-10-03 回归：无任务选中隐藏任务侧底栏（介入输入栏+费用栏容器），
    //   选任务恢复、取消选中再隐藏（与阶段条同口径，纵向空间还给日志区）。
    void taskBarsHideWithoutTask();
    // 2026-10-03 回归：无任务选中时「日志」页也不留（与阶段产物同口径随任务显隐；
    //   置空后当前页落激活助手页，选任务恢复并自动切日志页）。
    void taskLogTabHidesWithoutTask();
    // 2026-10-05 回归：上下文进度条本体显示当前模型名（共享表推送即时刷新；claude
    //   粘性助手不跟共享表，模型自报优先）。
    void contextBarShowsModelAndSwitch();
    // 2026-10-07 回归：右键「选择模型」= 对话级覆盖（仅本助手，不动全局）——
    //   设置/切换/清除/claude no-op/全局切换不冲掉覆盖/档持久化往返。
    void asstModelOverrideScoped();
    // v3.3.273 重入加固回归：落盘信号链内助手数组搬家（载档重排）不得令
    //   ApplyAsstModelOverride 崩溃/上抛悬垂快照（2026-10-07 23:14 用户实崩）。
    void asstModelOverrideReentrantReload();
    // 2026.09.19 后端切换清会话链（面板镜像侧回归）：ClearAsstSessions 清 sid/撤销栈、
    //   保留对话块（PixelPorter 实害：pi 切 claude 后旧 sid 被带给 claude --resume）。
    void backendSwitchClearsMirror();
    // 2026.09.23 v3.1.228 回归：助手输出块说话人标签取该助手粘性后端（非全局）。
    void asstBlockBackendUsesSticky();
    // 2026.09.26 v3.1.235 回归：自动改名——名称仍为原始名（助手N）时首条发送即触发
    //   sigAsstAutoRename（2026-10-08 改「先改名后执行」）；防重入（每轮一次）；
    //   非默认名不触发；改名失败后再发送（已有提示词）→ 收尾重试（旧语义兜底）。
    void asstAutoNameOnFinish();
    // 2026-10-03 运行中请求自动重命名 → 挂起到本轮收尾执行（用户需求）。
    void asstAutoNameDeferredWhileRunning();
    // 2026-10-08 首条提示词「先改名后执行」：默认名首条发送 → 立即命名 + 提示词
    //   挂起不提交；命名期间追加只暂存；命名收尾统一转正补发（多条合并）；
    //   停止撤销挂起退回输入框。
    void asstAutoNameRenameFirstOnSend();
    // 2026.09.26 v3.1.239 回归：「最近打开」按钮面板（流式行布局/次数渐变色/
    //   面板交互/菜单内嵌点击——设计定稿见 CRecentChipsPanel 头注）。
    void recentChipsPanel();
    void recentChipsPanelMenuEmbed();   // 真实窗口平台专用（offscreen QSKIP）
    // 2026.09.26 v3.1.241 回归：右键「添加到聊天框」注入长文本后输入框自动长高
    //   （软换行视觉行计高；封顶 10 行；清空回单行；宽度变窄重算更高）。
    void adhocInputAutoGrowOnInject();
    void foreignPhantomLifecycle();
    // 2026.09.20 最近打开对话框几何持久化（[Layout] RecentWin 往返/坏档/出屏收口）。
    void recentDialogGeometryPersist();
    // 2026.09.25 v3.1.230 回归：窗口「每开一次上漂一个标题栏高度」——构造期
    //   setGeometry 在原生窗口创建时客户区落点恒比请求 y 高 31px（本机 Qt6.8.3
    //   实测，probe_recentdrift4 四变体对照：仅 move()+resize() 精确），hideEvent
    //   把偏移值存回 → 下次继续偏。修复=恢复改 move()+resize()。需真实窗口平台。
    void recentDialogGeometryNoDrift();
    // 2026.09.18 运行中暂存提示词：不直接上屏、进右下角折叠框；injected 转正上屏；
    //   排队轮启动（OnAdhocPendingStarted）转正；队列作废（OnAdhocPendingDropped）清空。
    void stagedPromptsBox();
    // 2026.09.20 未发送排队指令的删除/编辑：RemoveStagedPrompt/ApplyStagedEdit 同步
    //   发信号（编排器侧按文本匹配）并保留时间戳前缀；✕ 按钮实际删除条目。
    void stagedPromptsEditDelete();
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
    void asstRenameSafety();       // 2026-09-22 助手改名安全：id 路由/会话链/存档往返不受改名影响
    void asstTabBarMultiRow();     // 2026-09-22 多行 Tab 栏：换行布局/命中/关闭热区/运行闪点/双击/拖动
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
    //   B 的指令应【立即提交】（不再排队等待 A），A/B 同时进入运行态（各 ● 闪点标记），
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
    // 2026.09.19 ● 运行标记从 Tab 文本（静态小黑点）迁为标题旁彩色闪点；
    //   2026-09-22 随多行 Tab 栏改为 CAsstTabBar 栏内自绘（旧 QLabel tabButton
    //   方案已废），运行态断言改为查询栏的运行页集合。
    auto bRunDot = [&pTabs](int i) -> bool {
        auto* pBar = qobject_cast<CAsstTabBar*>(pTabs->tabBar());
        return pBar != nullptr && pBar->PageRunning(pTabs->widget(i));
    };
    QVERIFY(bRunDot(iB));
    QVERIFY(bRunDot(iA));                     // 双闪点并发

    // A 轮结束：只清 A 的运行态，B 不受影响（旧实现全局单运行位会把 B 也带停）。
    panel.OnAdhocFinished(sA);
    QVERIFY(!panel.AsstRunning(sA));
    QVERIFY(panel.AsstRunning(sB));
    QVERIFY(bRunDot(iB));
    QVERIFY(!bRunDot(iA));

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

// 2026.09.22 文件内查找：纯文本视图上验证向下/向上/循环/区分大小写 + 命中选中。
//   （MD/HTML 走同一 QTextBrowser::find 路径，hex 走 ASCII 列同包装，不逐型重测。）
void TestExecPanel::filePreviewFind() {
    QTemporaryFile oFile;   // 无后缀 → 纯文本分支
    QVERIFY(oFile.open());
    const QString sBody = QStringLiteral(
        "alpha beta gamma\nBETA upper\nsecond beta end\n");
    oFile.write(sBody.toUtf8());
    oFile.close();

    CFilePreviewWidget w;
    w.resize(600, 400);
    w.show();
    w.ShowFile(oFile.fileName());

    // 取当前可见的 QTextBrowser（纯文本视图；stack 只有 current 可见）。
    QTextBrowser* pView = nullptr;
    const auto aViews = w.findChildren<QTextBrowser*>();
    for (QTextBrowser* p : aViews) {
        if (p->isVisible()) {
            pView = p;
            break;
        }
    }
    QVERIFY(pView != nullptr);
    QCOMPARE(pView->toPlainText(), sBody);

    // ① 向下逐次命中：三次 beta（大小写不敏感含 BETA），每次命中即选中。
    QVERIFY(w.FindText(QStringLiteral("beta"), false, false, false));
    QCOMPARE(pView->textCursor().selectedText(), QStringLiteral("beta"));
    QVERIFY(w.FindText(QStringLiteral("beta"), false, false, false));
    QCOMPARE(pView->textCursor().selectedText(), QStringLiteral("BETA"));
    QVERIFY(w.FindText(QStringLiteral("beta"), false, false, false));
    QCOMPARE(pView->textCursor().selectedText(), QStringLiteral("beta"));
    // ② 到底后无循环 → 未找到。
    QVERIFY(!w.FindText(QStringLiteral("beta"), false, false, false));
    // ③ 循环开启 → 回卷命中第一个 beta。
    QVERIFY(w.FindText(QStringLiteral("beta"), false, false, true));
    QCOMPARE(pView->textCursor().selectedText(), QStringLiteral("beta"));
    QVERIFY(pView->textCursor().position() < 20);

    // ④ 区分大小写：从当前位置（首个 beta 之后）只剩末尾一处 beta（不含 BETA）；
    //    再向下到底未命中。
    QVERIFY(w.FindText(QStringLiteral("beta"), false, true, false));
    QCOMPARE(pView->textCursor().selectedText(), QStringLiteral("beta"));
    QVERIFY(!w.FindText(QStringLiteral("beta"), false, true, false));

    // ⑤ 向上：从当前（第 2 个 beta）向上回第 1 个，再向上循环关时到顶未命中。
    QVERIFY(w.FindText(QStringLiteral("beta"), true, true, false));
    QCOMPARE(pView->textCursor().selectedText(), QStringLiteral("beta"));
    QVERIFY(!w.FindText(QStringLiteral("beta"), true, true, false));
    // ⑥ 向上循环 → 回卷到末尾命中。
    QVERIFY(w.FindText(QStringLiteral("beta"), true, true, true));
    QCOMPARE(pView->textCursor().selectedText(), QStringLiteral("beta"));
    QVERIFY(pView->textCursor().position() > 30);

    // ⑦ 不存在串恒未命中；空串安全返回 false。
    QVERIFY(!w.FindText(QStringLiteral("zzz-none"), false, false, true));
    QVERIFY(!w.FindText(QString(), false, false, true));
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
    // 组头摘要应兑底原文（经真实 UI 控件验证，不依赖 OCR）。
    // 2026.09.22 适配 v3.0.208 标题单行化：宽度不足时 text 按中间省略号截断
    //   （保住头部工具名与尾部 ×N，ElideMiddle 可能吃掉中部文件名），全文在
    //   tooltip——内容断言改验 tooltip 全文，另保留可见文本的尾部计数检查。
    auto fnHeadText = [](CMessageBlock* pBlk) -> QString {
        QToolButton* pHead = pBlk->findChild<QToolButton*>(
            QString(), Qt::FindDirectChildrenOnly);
        return pHead ? pHead->text() : QString();
    };
    auto fnHeadFull = [](CMessageBlock* pBlk) -> QString {
        QToolButton* pHead = pBlk->findChild<QToolButton*>(
            QString(), Qt::FindDirectChildrenOnly);
        return pHead ? pHead->toolTip() : QString();
    };
    CMessageBlock stBlk(CMessageBlock::ToolUse);
    stBlk.SetTool(QStringLiteral("read_file"), QStringLiteral("ChartView.h"));
    stBlk.AddMergedTool(QStringLiteral("read_file"), QStringLiteral("main.cpp"));
    stBlk.show();
    qApp->processEvents();
    QVERIFY2(fnHeadFull(&stBlk).contains(QStringLiteral("ChartView.h"))
                 && fnHeadFull(&stBlk).contains(QStringLiteral("×2")),
             qPrintable(fnHeadFull(&stBlk)));
    QVERIFY2(fnHeadText(&stBlk).contains(QStringLiteral("×2")),
             qPrintable(fnHeadText(&stBlk)));   // 尾部计数不受 ElideMiddle 影响
    CMessageBlock stBlk2(CMessageBlock::ToolUse);
    stBlk2.SetTool(QStringLiteral("terminal"), QStringLiteral("cmd /c dir /b *.cpp"));
    stBlk2.show();
    qApp->processEvents();
    QVERIFY2(fnHeadFull(&stBlk2).contains(QStringLiteral("dir")),
             qPrintable(fnHeadFull(&stBlk2)));
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

// 2026.09.23 v3.1.227 回归：无任务选中时阶段条自隐藏（用户需求：日志 Tab 控件的 top
//   移到任务进度的顶部）——SetCurrentTask(nullptr)/初始空流隐藏，载任务恢复显示。
void TestExecPanel::stageBarHidesWithoutTask() {
    CExecutionPanel panel;
    panel.resize(1000, 700);
    panel.show();
    // ① 初始（未载任何任务）：阶段条隐藏，不占 58px 布局位。
    QVERIFY(!panel.StageBarVisible());

    // ② 载任务（RefreshTask 建非空阶段流）：阶段条恢复显示。
    CTaskInfo task;
    task.SetId(QStringLiteral("sb-t-1"));
    task.SetStatus(ETaskStatus::Pending);
    panel.SetCurrentTask(&task);
    panel.RefreshTask(&task);
    QVERIFY(panel.StageBarVisible());

    // ③ 取消选中：再次隐藏，Tab 区顶回原任务进度位置。
    panel.SetCurrentTask(nullptr);
    QVERIFY(!panel.StageBarVisible());

    // ④ 同任务重复 SetCurrentTask（重复点选/SelectTask 二次触发）：保持显示。
    panel.SetCurrentTask(&task);
    panel.RefreshTask(&task);
    panel.SetCurrentTask(&task);
    QVERIFY(panel.StageBarVisible());
    panel.hide();
}

// 2026-10-03 无任务隐藏任务侧底栏回归：介入输入栏与费用栏只对当前任务有意义，
//   取消选中后整体隐藏不占空间；选任务恢复。锁死与阶段条同口径的显隐驱动。
void TestExecPanel::taskBarsHideWithoutTask() {
    CExecutionPanel panel;
    panel.resize(1000, 700);
    panel.show();
    // ① 初始（未选任务）：介入栏与费用栏均隐藏。
    QVERIFY(!panel.TaskBarsVisible());

    // ② 选任务：两栏恢复显示。
    CTaskInfo task;
    task.SetId(QStringLiteral("tb-t-1"));
    task.SetStatus(ETaskStatus::Pending);
    panel.SetCurrentTask(&task);
    panel.RefreshTask(&task);
    QVERIFY(panel.TaskBarsVisible());

    // ③ 取消选中：再次隐藏（任务运行态由任务列表图标呈现，不丢观测）。
    panel.SetCurrentTask(nullptr);
    QVERIFY(!panel.TaskBarsVisible());

    // ④ 重复选同一任务：保持显示；介入输入栏隐藏期不受 UpdateTaskInjectState 干扰。
    panel.SetCurrentTask(&task);
    panel.SetCurrentTask(&task);
    QVERIFY(panel.TaskBarsVisible());
    panel.hide();
}

// 2026-10-03 无任务隐藏「日志」页回归：任务对话流只属于任务，没选任务连页都不留
//（用户口径）；与阶段产物页同口径由 SetTaskTabsVisible 统一驱动。
void TestExecPanel::taskLogTabHidesWithoutTask() {
    CExecutionPanel panel;
    panel.resize(1000, 700);
    panel.show();
    QTabWidget* pTabs = panel.findChild<QTabWidget*>();
    QVERIFY(pTabs != nullptr);
    // 前置：载双助手档（取消选中后的落点=激活助手页）。
    auto mkAsst = [](const char* a_sId, const char* a_sName) {
        QJsonObject o;
        o["id"] = QString::fromLatin1(a_sId);
        o["name"] = QString::fromLatin1(a_sName);
        o["closed"] = false;
        return o;
    };
    QJsonArray aAssts;
    aAssts.append(mkAsst("lt-a", "助手A"));
    aAssts.append(mkAsst("lt-b", "助手B"));
    panel.LoadAssistantsFromJson(aAssts, QStringLiteral("lt-b"));
    QTest::qWait(50);

    // ① 初始（未选任务）：日志页在 Tab 栏不可见。
    QVERIFY(!panel.TaskLogTabVisible());
    QVERIFY(pTabs->tabText(pTabs->currentIndex()) != QStringLiteral("日志"));

    // ② 选任务：日志页恢复可见且成为当前页。
    CTaskInfo task;
    task.SetId(QStringLiteral("lt-t-1"));
    task.SetStatus(ETaskStatus::Running);
    panel.SetCurrentTask(&task);
    QVERIFY(panel.TaskLogTabVisible());
    QCOMPARE(pTabs->tabText(pTabs->currentIndex()), QStringLiteral("日志"));

    // ③ 取消选中：日志页再隐藏，当前页落回激活助手（lt-b）。
    panel.SetCurrentTask(nullptr);
    QVERIFY(!panel.TaskLogTabVisible());
    QVERIFY(pTabs->tabText(pTabs->currentIndex()) != QStringLiteral("日志"));

    // ④ 重复选同一任务：保持可见；阶段产物页同口径随行（v2026-10-02 行为不回退）。
    panel.SetCurrentTask(&task);
    panel.SetCurrentTask(&task);
    QVERIFY(panel.TaskLogTabVisible());
    panel.hide();
}

// 2026-10-05 上下文进度条模型名回归：①无共享表/未回报 → 百分比占位；②SetSharedModels
//   推送（主窗口启动/热重载/设置保存/右键切换四路）→ 非 claude 助手条立即显示当前
//   模型名，claude 粘性助手不跟（模型自报）；③实际回报（OnAdhocModel）优先；④再切换
//   当前项即时跟上。锁死「进度条=当前模型」口径与后端分流规则。
void TestExecPanel::contextBarShowsModelAndSwitch() {
    CExecutionPanel panel;
    panel.resize(1000, 700);
    panel.show();
    auto mkAsst = [](const char* a_sId, const char* a_sName, const char* a_sBackend) {
        QJsonObject o;
        o["id"] = QString::fromLatin1(a_sId);
        o["name"] = QString::fromLatin1(a_sName);
        o["closed"] = false;
        o["backend"] = QString::fromLatin1(a_sBackend);
        return o;
    };
    QJsonArray aAssts;
    aAssts.append(mkAsst("cb-a", "助手A", "dsh"));
    aAssts.append(mkAsst("cb-b", "助手B", "claude"));
    panel.LoadAssistantsFromJson(aAssts, QStringLiteral("cb-a"));
    QTest::qWait(50);

    // 按粘性后端定位各自的进度条（tooltip 带「后端：DSH/Claude」）。
    auto barOf = [&panel](const QString& a_sKey) -> QProgressBar* {
        const QList<QProgressBar*> apBars = panel.findChildren<QProgressBar*>();
        for (QProgressBar* pBar : apBars) {
            if (pBar->toolTip().contains(a_sKey)) {
                return pBar;
            }
        }
        return nullptr;
    };
    QProgressBar* pBarDsh = barOf(QStringLiteral("后端：DSH"));
    QProgressBar* pBarClaude = barOf(QStringLiteral("后端：Claude"));
    QVERIFY(pBarDsh != nullptr);
    QVERIFY(pBarClaude != nullptr);

    // ① 共享表未推/未回报：回落百分比占位。
    QCOMPARE(pBarDsh->format(), QStringLiteral("%p%"));

    // ② 推共享表（active=1）：非 claude 助手条立即显示当前模型名；claude 条不跟。
    SDshModelEntry stE0;
    stE0.sDisp = QStringLiteral("GLM5");
    stE0.sModelId = QStringLiteral("glm-5.3");
    SDshModelEntry stE1;
    stE1.sDisp = QStringLiteral("GLM47");
    stE1.sModelId = QStringLiteral("glm-4.7-flash");
    panel.SetSharedModels({ stE0, stE1 }, 1);
    QVERIFY(pBarDsh->format().contains(QStringLiteral("glm-4.7-flash")));
    QCOMPARE(pBarClaude->format(), QStringLiteral("%p%"));

    // ③ 实际回报优先：OnAdhocModel 覆盖共享表兜底。
    panel.OnAdhocModel(QStringLiteral("cb-a"), QStringLiteral("dsv4"));
    QVERIFY(pBarDsh->format().contains(QStringLiteral("dsv4")));

    // ④ 再切当前项（active=0）：非 claude 助手条即时跟上。
    panel.SetSharedModels({ stE0, stE1 }, 0);
    QVERIFY(pBarDsh->format().contains(QStringLiteral("glm-5.3")));

    panel.hide();
}

// 2026-10-07 对话级模型覆盖回归（用户需求：右键选择模型不改缺省配置、仅本对话生效）：
//   ①设置覆盖 → 该助手条显示覆盖模型 + tooltip 标注「仅本助手」，信号上抛 (id,entry,true)；
//   ②claude 粘性助手 2026-10-08 起同样支持覆盖；③全局当前项切换不冲掉覆盖显示；
//   ④assistant.json 持久化往返（modelOv 键）；⑤清除（-1）→ 回跟全局当前项。
void TestExecPanel::asstModelOverrideScoped() {
    CExecutionPanel panel;
    panel.resize(1000, 700);
    panel.show();
    auto mkAsst = [](const char* a_sId, const char* a_sName, const char* a_sBackend) {
        QJsonObject o;
        o["id"] = QString::fromLatin1(a_sId);
        o["name"] = QString::fromLatin1(a_sName);
        o["closed"] = false;
        o["backend"] = QString::fromLatin1(a_sBackend);
        return o;
    };
    QJsonArray aAssts;
    aAssts.append(mkAsst("ov-a", "助手A", "dsh"));
    aAssts.append(mkAsst("ov-c", "助手C", "claude"));
    panel.LoadAssistantsFromJson(aAssts, QStringLiteral("ov-a"));
    QTest::qWait(50);
    auto mkE = [](const char* a_sId) {
        SDshModelEntry e;
        e.sDisp = QString::fromLatin1(a_sId);
        e.sBaseURL = QStringLiteral("http://127.0.0.1:9/") + QString::fromLatin1(a_sId);
        e.sModelId = QString::fromLatin1(a_sId);
        return e;
    };
    panel.SetSharedModels({ mkE("m0"), mkE("m1"), mkE("m2") }, /*active=*/0);

    auto barOf = [&panel](const QString& a_sKey) -> QProgressBar* {
        const QList<QProgressBar*> apBars = panel.findChildren<QProgressBar*>();
        for (QProgressBar* pBar : apBars) {
            if (pBar->toolTip().contains(a_sKey)) {
                return pBar;
            }
        }
        return nullptr;
    };
    QProgressBar* pBarDsh = barOf(QStringLiteral("后端：DSH"));
    QProgressBar* pBarClaude = barOf(QStringLiteral("后端：Claude"));
    QVERIFY(pBarDsh != nullptr);
    QVERIFY(pBarClaude != nullptr);
    QVERIFY(pBarDsh->format().contains(QStringLiteral("m0")));   // 初始=全局当前 m0
    QCOMPARE(pBarClaude->format(), QStringLiteral("%p%"));

    // ① 设置覆盖（下标 2 → m2）：条即时显示 m2，tooltip 标注仅本助手，信号上抛。
    QSignalSpy spyOv(&panel, &CExecutionPanel::sigAsstModelOverride);
    QVERIFY(panel.ApplyAsstModelOverride(QStringLiteral("ov-a"), 2));
    QVERIFY(pBarDsh->format().contains(QStringLiteral("m2")));
    QVERIFY(pBarDsh->toolTip().contains(QStringLiteral("仅本助手")));
    QCOMPARE(spyOv.count(), 1);
    {
        const QList<QVariant> aArgs = spyOv.takeFirst();
        QCOMPARE(aArgs.at(0).toString(), QStringLiteral("ov-a"));
        QCOMPARE(aArgs.at(2).toBool(), true);
    }

    // ② 2026-10-08 起 claude 粘性助手同样支持覆盖（env 注入，面板行为与其他后端一致）。
    QVERIFY(panel.ApplyAsstModelOverride(QStringLiteral("ov-c"), 1));
    QVERIFY(pBarClaude->format().contains(QStringLiteral("m1")));
    QVERIFY(pBarClaude->toolTip().contains(QStringLiteral("仅本助手")));

    // ③ 全局当前项切换（active 0→1）：覆盖助手不被冲掉（仍 m2）。
    panel.SetSharedModels({ mkE("m0"), mkE("m1"), mkE("m2") }, /*active=*/1);
    QVERIFY(pBarDsh->format().contains(QStringLiteral("m2")));

    // ④ 持久化往返：modelOv 键随 assistant.json 落档并恢复。
    {
        const QJsonArray aSaved = panel.AssistantsToJson();
        CExecutionPanel panel2;
        panel2.resize(1000, 700);
        panel2.show();
        panel2.LoadAssistantsFromJson(aSaved, QStringLiteral("ov-a"));
        QTest::qWait(50);
        const QPair<SDshModelEntry, bool> stOv =
            panel2.AsstModelOverride(QStringLiteral("ov-a"));
        QVERIFY(stOv.second);
        QCOMPARE(stOv.first.sModelId, QStringLiteral("m2"));
        panel2.hide();
    }

    // ⑤ 清除（-1）：回跟全局当前项（此时 m1）。
    QVERIFY(panel.ApplyAsstModelOverride(QStringLiteral("ov-a"), -1));
    QVERIFY(pBarDsh->format().contains(QStringLiteral("m1")));
    QVERIFY(!pBarDsh->toolTip().contains(QStringLiteral("仅本助手")));

    // 2026-10-07 显示口径：runner 回报带 provider/进程名前缀（autocode-2/glm-4.6、
    //   codex/gpt-5）→ 条上只显示模型本体，tooltip 同口径。
    panel.OnAdhocModel(QStringLiteral("ov-a"), QStringLiteral("autocode-2/glm-4.6"));
    QVERIFY(pBarDsh->format().contains(QStringLiteral("glm-4.6")));
    QVERIFY(!pBarDsh->format().contains(QStringLiteral("autocode")));
    QVERIFY(pBarDsh->toolTip().contains(QStringLiteral("模型: glm-4.6")));

    panel.hide();
}

// v3.3.273 重入加固回归：ApplyAsstModelOverride 的落盘信号（sigAsstLayoutChanged）
    // 链内若发生载档重排（m_aAssts 清空重建，QList 搬家），旧实现链后再读
    // pAsst->stModelOv 上抛 = 读 freed 内存（用户实崩栈：SetAsstModelOverride→
    // QHash 插键拷贝 d=-1 的 QString）。加固后：链前完成全部解引用，链后只用 id+
    // 链前快照——上抛条目恒为链前值，面板重建后继续可用。
void TestExecPanel::asstModelOverrideReentrantReload() {
    CExecutionPanel panel;
    panel.resize(1000, 700);
    panel.show();
    auto mkAsst = [](const char* a_sId, const char* a_sName) {
        QJsonObject o;
        o["id"] = QString::fromLatin1(a_sId);
        o["name"] = QString::fromLatin1(a_sName);
        o["closed"] = false;
        o["backend"] = QStringLiteral("dsh");
        return o;
    };
    QJsonArray aAssts;
    aAssts.append(mkAsst("re-a", "助手A"));
    aAssts.append(mkAsst("re-b", "助手B"));
    panel.LoadAssistantsFromJson(aAssts, QStringLiteral("re-a"));
    QTest::qWait(50);
    auto mkE = [](const char* a_sId) {
        SDshModelEntry e;
        e.sDisp = QString::fromLatin1(a_sId);
        e.sBaseURL = QStringLiteral("http://127.0.0.1:9/") + QString::fromLatin1(a_sId);
        e.sModelId = QString::fromLatin1(a_sId);
        return e;
    };
    panel.SetSharedModels({ mkE("m0"), mkE("m1") }, /*active=*/0);

    // 钩子：落盘信号 → 模拟载档重排（同步链内清空重建 m_aAssts，旧数组即 freed）。
    int iReloads = 0;
    const QMetaObject::Connection cHook = QObject::connect(
        &panel, &CExecutionPanel::sigAsstLayoutChanged, &panel, [&]() {
            ++iReloads;
            panel.LoadAssistantsFromJson(aAssts, QStringLiteral("re-a"));
        });
    // 手动收流（不经 QSignalSpy 的 QVariant，直捕结构体副本）。
    SDshModelEntry stGot;
    bool bGotOn = false;
    int iOvSignals = 0;
    const QMetaObject::Connection cCap = QObject::connect(
        &panel, &CExecutionPanel::sigAsstModelOverride, &panel,
        [&](const QString&, const SDshModelEntry& a_stE, bool a_bOn) {
            stGot = a_stE;
            bGotOn = a_bOn;
            ++iOvSignals;
        });
    QVERIFY(panel.ApplyAsstModelOverride(QStringLiteral("re-a"), 1));   // 不得崩溃
    QObject::disconnect(cHook);
    QObject::disconnect(cCap);
    QCOMPARE(iReloads, 1);          // 重排确实发生在链内
    QCOMPARE(iOvSignals, 1);
    QVERIFY(bGotOn);
    QCOMPARE(stGot.sModelId, QStringLiteral("m1"));   // 上抛链前快照，非悬垂内存

    // 链内重排后：重建面板继续可用（重建版覆盖丢失=载档语义，功能不受损）。
    QVERIFY(panel.ApplyAsstModelOverride(QStringLiteral("re-a"), 0));
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
    // ⑥ 2026.09.21 实害（HarnessGatewayPlugin 任务）：裸动词开头保留「开发」+
    //    行尾冒号剥离——旧版产出「大模型 Harness网关插件:」，用户期望
    //    「开发大模型Harness网关插件」。
    const QString sHgp = QStringLiteral(
        "开发大模型 Harness网关插件:\n"
        "插件参照：\n"
        "E:\\x\\GraphContainer\n"
        "业务参照：\nE:\\y\\HarnessGateway");
    QCOMPARE(CTaskInfo::ExtractNameFromRequest(sHgp),
             QStringLiteral("开发大模型 Harness网关插件"));
    // ⑦ 「动词+一个」仍剥（裸动词不剥的两面性）
    QCOMPARE(CTaskInfo::ExtractNameFromRequest(QStringLiteral("做一个贪吃蛇游戏")),
             QStringLiteral("贪吃蛇游戏"));
    // ⑧ TrimNamePunct：两侧引号书名号与句尾冒号（嵌套循环剥）
    QCOMPARE(CTaskInfo::TrimNamePunct(QStringLiteral("「大模型网关插件：」")),
             QStringLiteral("大模型网关插件"));
    QCOMPARE(CTaskInfo::TrimNamePunct(QStringLiteral("开发XX插件:")),
             QStringLiteral("开发XX插件"));
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

// 2026-09-22 助手改名安全回归（手动双击改名与右键自动重命名同一变异点：只改 sName[+sDesc]）：
//   全部路由（激活/发送/Tab 查找/「▾」恢复）与持久化键均在 sId 上——改名后：
//   ①发送仍路由到同一助手 id；②会话链 sid/撤销栈/粘性后端/权限/统计随档完整往返；
//   ③说明 sDesc 入 tooltip 首行且随档持久化；④失败路径原名保持。
void TestExecPanel::asstRenameSafety() {
    CExecutionPanel panel;
    panel.resize(1000, 700);
    panel.show();
    QTabWidget* pTabs = panel.findChild<QTabWidget*>();
    QVERIFY(pTabs != nullptr);

    // A：带会话链/撤销栈/粘性后端/权限/统计（改名最怕丢的就是这些）；B：陷钉。
    QJsonObject oA = MakeAsst(QStringLiteral("助手A"), 3);
    oA[QStringLiteral("sid")]     = QStringLiteral("sess-abc-111");
    oA[QStringLiteral("backend")] = QStringLiteral("pi");
    oA[QStringLiteral("perm")]    = 2;   // 只读
    {
        QJsonObject oPair;
        oPair[QStringLiteral("prompt")] = QStringLiteral("旧提问");
        oPair[QStringLiteral("sid")]    = QStringLiteral("sess-abc-111");
        QJsonArray aStack; aStack.append(oPair);
        oA[QStringLiteral("stack")] = aStack;
        QJsonObject oUsage;
        oUsage[QStringLiteral("cost")] = 1.25;
        oUsage[QStringLiteral("in")]   = 100.0;
        oUsage[QStringLiteral("out")]  = 50.0;
        oUsage[QStringLiteral("reqs")] = 3;
        oUsage[QStringLiteral("ms")]   = 45000.0;
        oA[QStringLiteral("usage")] = oUsage;
    }
    QJsonArray aAssts;
    aAssts.append(oA);
    aAssts.append(MakeAsst(QStringLiteral("助手B"), 1));
    panel.LoadAssistantsFromJson(aAssts, QString());
    QTest::qWait(100);   // 分帧重建跑完；激活=第一个助手 A
    panel.SetAdhocEnabled(true);
    const QString sId0 = panel.ActiveAsstId();
    QVERIFY(!sId0.isEmpty());

    // —— 自动改名成功路径：只动 sName/sDesc ——
    panel.ApplyAsstAutoName(sId0, true, QStringLiteral("调试登录页"),
                            QStringLiteral("定位并修复登录页键盘焦点"));
    QCOMPARE(panel.ActiveAsstId(), sId0);   // 激活路由不变（id 锚定）
    bool bTitle = false;
    for (int i = 0; i < pTabs->count(); ++i) {
        if (pTabs->tabText(i) == QStringLiteral("调试登录页")) {
            bTitle = true;
            QVERIFY(pTabs->tabToolTip(i).contains(QStringLiteral("键盘焦点")));   // 说明进 tooltip 首行
        }
    }
    QVERIFY(bTitle);

    // —— 失败路径：原名/原说明保持不变 ——
    panel.ApplyAsstAutoName(sId0, false, QString(), QStringLiteral("Agent 未给出有效名称"));
    {
        const QJsonObject oChk = panel.AssistantsToJson().at(0).toObject();
        QCOMPARE(oChk[QStringLiteral("name")].toString(), QStringLiteral("调试登录页"));
        QCOMPARE(oChk[QStringLiteral("desc")].toString(), QStringLiteral("定位并修复登录页键盘焦点"));
    }

    // —— 改名后发送路由：仍提交到同一助手 id ——
    int iSubmits = 0;
    QString sLastId;
    QObject::connect(&panel, &CExecutionPanel::sigAdhocSubmit,
                     [&](const QString& sId, const QString&, int) { ++iSubmits; sLastId = sId; });
    panel.AppendToAdhocInput(QStringLiteral("改名后的新指令"));
    panel.DoAdhocSend();
    QCOMPARE(iSubmits, 1);
    QCOMPARE(sLastId, sId0);

    // —— 档往返：name/desc 已更新，id/sid/stack/backend/perm/usage 原样保留 ——
    const QJsonArray aOut = panel.AssistantsToJson();
    QCOMPARE(aOut.size(), 2);
    const QJsonObject oA2 = aOut.at(0).toObject();
    QCOMPARE(oA2[QStringLiteral("id")].toString(), sId0);
    QCOMPARE(oA2[QStringLiteral("name")].toString(), QStringLiteral("调试登录页"));
    QCOMPARE(oA2[QStringLiteral("desc")].toString(), QStringLiteral("定位并修复登录页键盘焦点"));
    QCOMPARE(oA2[QStringLiteral("sid")].toString(), QStringLiteral("sess-abc-111"));
    QCOMPARE(oA2[QStringLiteral("backend")].toString(), QStringLiteral("pi"));
    QCOMPARE(oA2[QStringLiteral("perm")].toInt(), 2);
    QCOMPARE(oA2[QStringLiteral("stack")].toArray().size(), 1);
    QCOMPARE(oA2[QStringLiteral("usage")].toObject()[QStringLiteral("cost")].toDouble(), 1.25);

    // —— 重载档（切档等价路径）：说明/会话链仍在 ——
    panel.LoadAssistantsFromJson(aOut, QString());
    QTest::qWait(50);
    const QJsonObject oA3 = panel.AssistantsToJson().at(0).toObject();
    QCOMPARE(oA3[QStringLiteral("desc")].toString(), QStringLiteral("定位并修复登录页键盘焦点"));
    QCOMPARE(oA3[QStringLiteral("sid")].toString(), QStringLiteral("sess-abc-111"));
    QCOMPARE(oA3[QStringLiteral("backend")].toString(), QStringLiteral("pi"));

    // —— 2026-09-22 运行中改名（本问核心时序）：bRunning=true 期间改名只动显示字段 ——
    //   手动双击与自动重命名共用同一变异点；运行点(SyncAsstTabDot)/暂存队列(aStaged)/
    //   消耗统计均按 SAsst 结构锚定，与 sName 正交。
    panel.SetAsstRunning(sId0, true);   // 闪点挂载（tabButton）
    panel.ApplyAsstAutoName(sId0, true, QStringLiteral("运行期改名"),
                            QStringLiteral("运行中改名不触碰运行态"));
    QCOMPARE(panel.ActiveAsstId(), sId0);   // 运行中激活路由仍不变
    bTitle = false;
    for (int i = 0; i < pTabs->count(); ++i) {
        if (pTabs->tabText(i) == QStringLiteral("运行期改名")) {
            bTitle = true;
            QVERIFY(pTabs->tabToolTip(i).contains(QStringLiteral("运行中改名不触碰运行态")));
        }
    }
    QVERIFY(bTitle);   // 运行中 Tab 标题/tooltip 照常刷新
    // 运行中发送 → 暂存队列（aStaged）+ 仍按 id 提交（编排器注入/排队）：改名不影响。
    panel.AppendToAdhocInput(QStringLiteral("运行中排队指令"));
    panel.DoAdhocSend();
    QCOMPARE(iSubmits, 2);
    QCOMPARE(sLastId, sId0);
    QCOMPARE(panel.StagedCount(sId0), 1);   // 进的是改名后助手的队列
    // 轮结束：运行态清零后标题保持新名、说明不丢（RefreshAsstTabTexts 按 sName 重算）。
    panel.SetAsstRunning(sId0, false);
    QCOMPARE(pTabs->tabText(pTabs->currentIndex()), QStringLiteral("运行期改名"));
    QCOMPARE(panel.AssistantsToJson().at(0).toObject()
             .value(QStringLiteral("desc")).toString(),
             QStringLiteral("运行中改名不触碰运行态"));

    panel.hide();
}

// 2026-09-22 多行 Tab 栏回归（自 HarnessAgent/CAgentTabBar 迁移）：
//   ① 宽度足够单行、变窄自动换行（行数>1），第二行及以后的 Tab 命中/定位
//      （TabRectAt/TabAtPos）仍准确；② 运行闪点随 SetAsstRunning 起/停（页指针键，
//      拖动/改名不失效）；③ 关闭「x」只在助手页（点非助手页右缘不触发）；
//   后半段用独立裸栏（无面板连接，避免双击改名弹模态框卡测试）验证：
//   ④ 双击发 tabBarDoubleClicked（按多行布局命中）；⑤ 拖动换位发 tabMoved。
void TestExecPanel::asstTabBarMultiRow() {
    CExecutionPanel panel;
    panel.resize(1100, 700);
    panel.show();
    QVERIFY(QTest::qWaitForWindowExposed(&panel));
    QTabWidget* pTabs = panel.findChild<QTabWidget*>();
    QVERIFY(pTabs != nullptr);
    auto* pBar = qobject_cast<CAsstTabBar*>(pTabs->tabBar());
    QVERIFY(pBar != nullptr);

    // 先验「宽度足够 = 单行」（与原生观感一致）：2 个助手远小于栏宽。
    QJsonArray aFew;
    aFew.append(MakeAsst(QStringLiteral("单行助手一"), 1));
    aFew.append(MakeAsst(QStringLiteral("单行助手二"), 1));
    panel.LoadAssistantsFromJson(aFew, QString());
    QTest::qWait(120);
    QCOMPARE(pBar->TabRowCount(), 1);

    // 40 个长名助手：窗口压窄后必换行。
    QJsonArray aAssts;
    for (int i = 0; i < 40; ++i) {
        aAssts.append(MakeAsst(QStringLiteral("多行回归助手%1号").arg(i), 1));
    }
    panel.LoadAssistantsFromJson(aAssts, QString());
    QTest::qWait(120);   // 分帧重建 + 「+」页插入
    QCOMPARE(pTabs->count(), 2 + 40 + 1);   // 日志+产物+40 助手+「+」
    QVERIFY(pBar->TabRowCount() >= 2);      // 名多宽不够 → 已换行（宽窗口下同样会叠行）

    panel.resize(360, 700);
    QTest::qWait(120);   // 几何传播 + resizeEvent 重分行
    QVERIFY(pBar->TabRowCount() >= 2);      // 变窄 → 行数只增不减
    // 每个 Tab 的 rect 中心 TabAtPos 命中自身（含第二行及以后的行；基类单行
    // tabAt 在多行下必错，这里验的是自管布局的完整性）。
    for (int i = 0; i < pTabs->count(); ++i) {
        // 2026-10-02 阶段产物 Tab 无任务选中时隐藏（setTabVisible）——隐藏页无布局
        //   rect（CAsstTabBar 跳过不可见项），跳过其 rect/命中断言。
        if (!pTabs->tabBar()->isTabVisible(i)) {
            continue;
        }
        const QRect rc = pBar->TabRectAt(i);
        QVERIFY(rc.isValid());
        QVERIFY(rc.top() >= 0 && rc.bottom() < pBar->TabRowCount() * rc.height());
        QCOMPARE(pBar->TabAtPos(rc.center()), i);
    }

    // ② 运行闪点：SetAsstRunning 起/停均同步到栏（页指针键）。
    const QString sId = panel.ActiveAsstId();
    QVERIFY(!sId.isEmpty());
    const int iAsstIdx = pTabs->indexOf(pTabs->currentWidget());
    panel.SetAsstRunning(sId, true);
    QVERIFY(pBar->PageRunning(pTabs->widget(iAsstIdx)));
    panel.SetAsstRunning(sId, false);
    QVERIFY(!pBar->PageRunning(pTabs->widget(iAsstIdx)));

    // ③ 关闭「x」只在助手页：点助手页关闭热区 → tabCloseRequested；点「日志」
    //   右缘同位置 → 不触发（选中日志页而已）。
    QSignalSpy aSpyClose(pTabs, &QTabWidget::tabCloseRequested);
    const int iLastAsst = pTabs->count() - 2;   // 最后一个助手（「+」前）
    QTest::mouseClick(pBar, Qt::LeftButton, Qt::NoModifier,
                      pBar->CloseRectOf(pBar->TabRectAt(iLastAsst)).center());
    QCOMPARE(aSpyClose.count(), 1);
    QCOMPARE(aSpyClose.takeFirst().at(0).toInt(), iLastAsst);
    QTest::qWait(50);   // 软关闭（CloseAsst）摘 Tab
    QCOMPARE(pTabs->count(), 2 + 40);            // 少一个助手
    QTest::mouseClick(pBar, Qt::LeftButton, Qt::NoModifier,
                      pBar->CloseRectOf(pBar->TabRectAt(0)).center());   // 「日志」页右缘：无「x」热区语义
    QCOMPARE(aSpyClose.count(), 0);
    QCOMPARE(pTabs->count(), 2 + 40);            // 无页被关
    panel.hide();

    // —— ④⑤ 裸栏（无面板连接）：双击/拖动 ——
    CAsstTabWidget stTabs;
    auto* pBare = new CAsstTabBar;
    stTabs.setTabBar(pBare);
    stTabs.setMovable(true);   // 拖动换位需显式开（面板路径在 SetupTabsInteraction 开）
    for (int i = 0; i < 3; ++i) {
        stTabs.addTab(new QWidget(&stTabs), QStringLiteral("裸栏页%1").arg(i));
    }
    stTabs.resize(260, 200);
    stTabs.show();
    QVERIFY(QTest::qWaitForWindowExposed(&stTabs));
    QTest::qWait(50);
    // 双击按多行布局命中 → tabBarDoubleClicked（面板接它做改名，不能在面板上试；
    //   Qt6 QTest 无 mouseDblClick，直接发 MouseButtonDblClick 事件）。
    QSignalSpy aSpyDbl(pBare, &QTabBar::tabBarDoubleClicked);
    const int iDblIdx = 2;
    const QRect rcDbl = pBare->TabRectAt(iDblIdx);
    QMouseEvent evDbl(QEvent::MouseButtonDblClick, QPointF(rcDbl.center()),
                      QPointF(pBare->mapToGlobal(rcDbl.center())),
                      Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(pBare, &evDbl);
    QCOMPARE(aSpyDbl.count(), 1);
    QCOMPARE(aSpyDbl.takeFirst().at(0).toInt(), iDblIdx);
    // 拖动换位：按住页1拖到页2位置 → tabMoved(1,2)，文本序互换。
    QSignalSpy aSpyMoved(pBare, &QTabBar::tabMoved);
    const QPoint ptA = pBare->TabRectAt(1).center();
    const QPoint ptB = pBare->TabRectAt(2).center();
    QTest::mousePress(pBare, Qt::LeftButton, Qt::NoModifier, ptA);
    for (int s = 1; s <= 12; ++s) {   // 分步越过拖起阈值，模拟真实拖拽
        QTest::mouseMove(pBare, ptA + (ptB - ptA) * s / 12);
        QTest::qWait(10);
    }
    QTest::mouseRelease(pBare, Qt::LeftButton, Qt::NoModifier, ptB);
    QCOMPARE(aSpyMoved.count(), 1);
    QCOMPARE(aSpyMoved.takeFirst().at(0).toInt(), 1);
    QCOMPARE(pBare->tabText(1), QStringLiteral("裸栏页2"));
    QCOMPARE(pBare->tabText(2), QStringLiteral("裸栏页1"));
    stTabs.hide();
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

void TestExecPanel::stagedPromptsBox() {
    // 2026.09.18 运行中发送的提示词暂存链路（右下角折叠框数据源）：
    //   ①运行中发送 → 不上屏、StagedCount=1、queueBox 可见；
    //   ②注入生效事件（injected）→ 暂存转正为用户块、框收空隐藏；
    //   ③再暂存两条 → OnAdhocPendingStarted 按文本匹配转正首条；
    //   ④OnAdhocPendingDropped 清空；⑤空闲发送仍直接上屏。
    CExecutionPanel panel;
    panel.resize(1000, 700);
    panel.show();
    QTabWidget* pTabs = panel.findChild<QTabWidget*>();
    QVERIFY(pTabs != nullptr);
    QJsonArray aAssts;
    aAssts.append(MakeAsst(QStringLiteral("助手A"), 2));
    panel.LoadAssistantsFromJson(aAssts, QString());
    QTest::qWait(50);
    panel.SetAdhocEnabled(true);

    // 首个助手的 id/输入框（页容器内首个 QPlainTextEdit）。
    QWidget* pPage = nullptr;
    for (int i = 0; i < pTabs->count(); ++i) {
        if (pTabs->tabText(i).startsWith(QStringLiteral("助手A"))) {
            pPage = pTabs->widget(i);
            break;
        }
    }
    QVERIFY(pPage != nullptr);
    auto* pInput = pPage->findChild<QPlainTextEdit*>();
    QVERIFY(pInput != nullptr);
    const QJsonArray aBefore = panel.AssistantsToJson();
    const QString sId = aBefore.at(0).toObject().value(QStringLiteral("id")).toString();
    const int iBlocksBefore = aBefore.at(0).toObject()
            .value(QStringLiteral("blocks")).toArray().size();

    // ①运行中发送：暂存不上屏。
    panel.SetAsstRunning(sId, true);
    pInput->setPlainText(QStringLiteral("排队第一条"));
    panel.DoAdhocSend(sId);
    QCOMPARE(panel.StagedCount(sId), 1);
    QCOMPARE(panel.AssistantsToJson().at(0).toObject()
             .value(QStringLiteral("blocks")).toArray().size(), iBlocksBefore);
    auto* pQBox = pPage->findChild<QFrame*>(QStringLiteral("queueBox"));
    QVERIFY(pQBox != nullptr);
    QVERIFY(pQBox->isVisible());

    // ②注入生效：转正上屏 + 框收空。
    panel.OnAdhocEvent(sId, QStringLiteral("injected"),
                       QStringLiteral("已注入：排队第一条"));
    QCOMPARE(panel.StagedCount(sId), 0);
    QVERIFY(!pQBox->isVisible());
    const QJsonArray aAfterInj = panel.AssistantsToJson();
    // +2：用户块（先）+「已注入」事件行（后，通用路径追加）。
    QCOMPARE(aAfterInj.at(0).toObject()
             .value(QStringLiteral("blocks")).toArray().size(), iBlocksBefore + 2);
    QCOMPARE(aAfterInj.at(0).toObject()
             .value(QStringLiteral("blocks")).toArray().at(iBlocksBefore)
             .toObject().value(QStringLiteral("eventKind")).toString(),
             QStringLiteral("user"));

    // ③再暂存两条 → 排队轮启动转正首条。
    pInput->setPlainText(QStringLiteral("排队第二条"));
    panel.DoAdhocSend(sId);
    pInput->setPlainText(QStringLiteral("排队第三条"));
    panel.DoAdhocSend(sId);
    QCOMPARE(panel.StagedCount(sId), 2);
    panel.OnAdhocPendingStarted(sId, QStringLiteral("排队第二条"));
    QCOMPARE(panel.StagedCount(sId), 1);
    QCOMPARE(panel.AssistantsToJson().at(0).toObject()
             .value(QStringLiteral("blocks")).toArray().size(), iBlocksBefore + 3);

    // ④队列作废：清空。
    panel.OnAdhocPendingDropped(sId);
    QCOMPARE(panel.StagedCount(sId), 0);

    // ⑤空闲发送：直接上屏（原行为）。
    panel.SetAsstRunning(sId, false);
    pInput->setPlainText(QStringLiteral("空闲直发"));
    panel.DoAdhocSend(sId);
    QCOMPARE(panel.StagedCount(sId), 0);
    QCOMPARE(panel.AssistantsToJson().at(0).toObject()
             .value(QStringLiteral("blocks")).toArray().size(), iBlocksBefore + 4);
    panel.hide();
}

void TestExecPanel::backendSwitchClearsMirror() {
    // 2026.09.20 v2.4.193 粘性后端（取代清链语义）：per-assistant backend 字段在
    //   面板 JSON 往返中保持——档载入读入、序列化写回（旧档无键 → 不写/空串）。
    CExecutionPanel panel;
    QTabWidget* pTabs = panel.findChild<QTabWidget*>();
    QVERIFY(pTabs != nullptr);
    QJsonArray aAssts;
    QJsonObject oA = MakeAsst(QStringLiteral("助手A"), 3);
    oA[QStringLiteral("backend")] = QStringLiteral("pi");
    oA[QStringLiteral("sid")] = QStringLiteral("pi-session-0001");
    QJsonObject oB = MakeAsst(QStringLiteral("助手B"), 2);   // 旧档无 backend 键
    aAssts.append(oA);
    aAssts.append(oB);
    panel.LoadAssistantsFromJson(aAssts, QString());

    const QString sIdA = panel.AssistantsToJson().at(0).toObject()
                             .value(QStringLiteral("id")).toString();
    const QString sIdB = panel.AssistantsToJson().at(1).toObject()
                             .value(QStringLiteral("id")).toString();
    QCOMPARE(panel.AsstBackend(sIdA), QStringLiteral("pi"));
    QCOMPARE(panel.AsstBackend(sIdB), QString());

    const QJsonArray aOut = panel.AssistantsToJson();
    QCOMPARE(aOut.at(0).toObject().value(QStringLiteral("backend")).toString(),
             QStringLiteral("pi"));
    QVERIFY(aOut.at(1).toObject().value(QStringLiteral("backend")).toString().isEmpty());
}

// 2026.09.23 v3.1.228 回归：助手输出块说话人标签取该助手粘性后端（v2.4.193），
//   而非渲染时刻的全局后端——用户报告：助手绑 PiAgent、全局切 CodexHarness 后，
//   日志块仍标 CodexHarness，与 sticky 提示「本助手继续使用 PIAgent」自相矛盾。
void TestExecPanel::asstBlockBackendUsesSticky() {
    CExecutionPanel panel;
    panel.resize(1000, 700);
    panel.show();
    QTabWidget* pTabs = panel.findChild<QTabWidget*>();
    QVERIFY(pTabs != nullptr);
    QJsonArray aAssts;
    QJsonObject oA = MakeAsst(QStringLiteral("助手A"), 0);
    oA[QStringLiteral("backend")] = QStringLiteral("pi");   // 粘性绑定 PiAgent
    aAssts.append(oA);
    panel.LoadAssistantsFromJson(aAssts, QString());
    const QString sIdA = panel.AssistantsToJson().at(0).toObject()
                             .value(QStringLiteral("id")).toString();
    QCOMPARE(panel.AsstBackend(sIdA), QStringLiteral("pi"));

    // 注入一条助手输出：块标签应为 PIAgent（粘性），而非当前全局（测试进程工厂缺省 Claude）。
    panel.OnAdhocAssistantText(sIdA, QStringLiteral("粘性后端块测试"));
    QTest::qWait(50);

    // 在该助手页的对话流里找说话人标签（“⏺ ” 前缀的 QLabel）。
    QWidget* pPage = nullptr;
    for (int i = 0; i < pTabs->count(); ++i) {
        if (pTabs->tabText(i).startsWith(QStringLiteral("助手A"))) {
            pPage = pTabs->widget(i);
            break;
        }
    }
    QVERIFY(pPage != nullptr);
    QString sSpeaker;
    const QList<QLabel*> aLabels = pPage->findChildren<QLabel*>();
    for (const QLabel* pL : aLabels) {
        const QString s = pL->text();
        if (s.startsWith(QStringLiteral("⏺ "))) {
            sSpeaker = s;
            break;
        }
    }
    QCOMPARE(sSpeaker, QStringLiteral("⏺ PIAgent"));
    panel.hide();
}

void TestExecPanel::stagedPromptsEditDelete() {
    // 2026.09.20 未发送排队指令的删除/编辑（面板侧）：信号同步编排器 aPendingInjects、
    //   展示文本保留时间戳前缀、✕ 按钮（queueDelBtn）实际删除条目。
    CExecutionPanel panel;
    panel.resize(1000, 700);
    panel.show();
    QTabWidget* pTabs = panel.findChild<QTabWidget*>();
    QVERIFY(pTabs != nullptr);
    QJsonArray aAssts;
    aAssts.append(MakeAsst(QStringLiteral("助手A"), 2));
    panel.LoadAssistantsFromJson(aAssts, QString());
    QTest::qWait(50);
    panel.SetAdhocEnabled(true);
    QWidget* pPage = nullptr;
    for (int i = 0; i < pTabs->count(); ++i) {
        if (pTabs->tabText(i).startsWith(QStringLiteral("助手A"))) {
            pPage = pTabs->widget(i);
            break;
        }
    }
    QVERIFY(pPage != nullptr);
    auto* pInput = pPage->findChild<QPlainTextEdit*>();
    QVERIFY(pInput != nullptr);
    const QString sId = panel.AssistantsToJson().at(0).toObject()
                             .value(QStringLiteral("id")).toString();

    // 运行中暂存两条。
    panel.SetAsstRunning(sId, true);
    pInput->setPlainText(QStringLiteral("排队甲"));
    panel.DoAdhocSend(sId);
    pInput->setPlainText(QStringLiteral("排队乙"));
    panel.DoAdhocSend(sId);
    QCOMPARE(panel.StagedCount(sId), 2);

    QSignalSpy aSpyRemove(&panel, &CExecutionPanel::sigAdhocQueuedRemove);
    QSignalSpy aSpyEdit(&panel, &CExecutionPanel::sigAdhocQueuedEdit);

    // 编辑首条（首条带 [yyyy-MM-dd HH:mm:ss] 时间戳——第二条距首条 <60s 被去抖无前缀）：
    //   原始文本换新、展示文本保留时间戳前缀、信号带（旧→新）。
    panel.ApplyStagedEdit(sId, 0, QStringLiteral("排队甲（改）"));
    QCOMPARE(panel.StagedCount(sId), 2);
    QCOMPARE(aSpyEdit.size(), 1);
    QCOMPARE(aSpyEdit.at(0).at(1).toString(), QStringLiteral("排队甲"));
    QCOMPARE(aSpyEdit.at(0).at(2).toString(), QStringLiteral("排队甲（改）"));
    // 展示文本（转正时上屏的用户块文本）保留时间戳前缀且含新文本。
    panel.OnAdhocPendingStarted(sId, QStringLiteral("排队甲（改）"));
    const QJsonObject oLast = panel.AssistantsToJson().at(0).toObject()
            .value(QStringLiteral("blocks")).toArray().last().toObject();
    QVERIFY(oLast.value(QStringLiteral("eventKind")).toString()
            == QStringLiteral("user"));
    const QString sUserText = oLast.value(QStringLiteral("text")).toString();
    QVERIFY(sUserText.contains(QStringLiteral("排队甲（改）")));
    QVERIFY(sUserText.startsWith(QStringLiteral("[20")));   // 时间戳前缀保留

    // 空白编辑拒改。
    panel.ApplyStagedEdit(sId, 0, QStringLiteral("   "));
    QCOMPARE(aSpyEdit.size(), 1);

    // 删除余下的「排队乙」（转正已消费改过的首条）：信号按原文本同步。
    panel.RemoveStagedPrompt(sId, 0);
    QCOMPARE(panel.StagedCount(sId), 0);
    QCOMPARE(aSpyRemove.size(), 1);
    QCOMPARE(aSpyRemove.at(0).at(1).toString(), QStringLiteral("排队乙"));

    // 越界/未知助手：零副作用。
    panel.RemoveStagedPrompt(sId, 9);
    panel.RemoveStagedPrompt(QStringLiteral("no-such"), 0);
    QCOMPARE(aSpyRemove.size(), 1);

    // UI 路径：再暂存一条，展开后 ✕ 按钮删除该条。
    pInput->setPlainText(QStringLiteral("排队丙"));
    panel.DoAdhocSend(sId);
    QCOMPARE(panel.StagedCount(sId), 1);
    auto* pExpand = pPage->findChild<QToolButton*>(QStringLiteral("queueExpandBtn"));
    QVERIFY(pExpand != nullptr);
    QTest::mouseClick(pExpand, Qt::LeftButton);
    QTest::qWait(30);
    auto aDelBtns = pPage->findChildren<QToolButton*>(QStringLiteral("queueDelBtn"));
    QCOMPARE(aDelBtns.size(), 1);
    QTest::mouseClick(aDelBtns.at(0), Qt::LeftButton);
    QCOMPARE(panel.StagedCount(sId), 0);
    QCOMPARE(aSpyRemove.size(), 2);
    QCOMPARE(aSpyRemove.at(1).at(1).toString(), QStringLiteral("排队丙"));
    panel.hide();
}

void TestExecPanel::recentDialogGeometryPersist() {
    // 2026.09.20 最近打开对话框几何持久化：show→move/resize→hide 落
    //   [Layout] RecentWin；新实例读回同几何；坏档回缺省；空 cfg 路径不写盘。
    QTemporaryDir oTmp;
    QVERIFY(oTmp.isValid());
    const QString sCfg = oTmp.path() + QStringLiteral("/config.ini");
    {
        CRecentProjectsDialog stDlg({ QStringLiteral("C:/ProjA"), QStringLiteral("C:/ProjB") },
                                     QList<int>() << 5 << 2, sCfg);
        stDlg.show();
        QTest::qWait(30);
        // offscreen 虚拟桌面 800×600：取 600×400@（100,60）不出屏（760 宽会触发贴边收口）。
        stDlg.move(100, 60);
        stDlg.resize(600, 400);
        QTest::qWait(30);
        stDlg.hide();   // hideEvent → 落盘
        QTest::qWait(30);
        QFile fDbg(sCfg);
        if (fDbg.open(QIODevice::ReadOnly)) {
            qDebug() << "CFG:" << QString::fromUtf8(fDbg.readAll());
            fDbg.close();
        }
    }
    {
        CRecentProjectsDialog stDlg({ QStringLiteral("C:/ProjA") }, QList<int>(), sCfg);
        QCOMPARE(stDlg.geometry(), QRect(100, 60, 600, 400));
        // 计数显示（同序快照）。
        QVERIFY(stDlg.findChild<QListWidget*>() != nullptr);
        QListWidget* pList = stDlg.findChild<QListWidget*>();
        QCOMPARE(pList->count(), 1);
        QVERIFY(pList->item(0)->text().contains(QStringLiteral("— 1 次")));
    }
    // 坏档（非数字/个数不符）→ 缺省 620×420 不炸（重写新档，非追加——追加时首个
    //   同名键仍命中旧好值，profile 取首次出现）。
    {
        QFile fCfg(sCfg);
        QVERIFY(fCfg.open(QIODevice::WriteOnly | QIODevice::Truncate));
        fCfg.write("\r\n[Layout]\r\nRecentWin=garbage,xx\r\n");
        fCfg.close();
        CRecentProjectsDialog stDlg({ QStringLiteral("C:/ProjA") }, QList<int>(), sCfg);
        QCOMPARE(stDlg.size(), QSize(620, 420));
    }
    // 空 cfg 路径 = 禁用（不写盘；hide 无副作用）。
    {
        CRecentProjectsDialog stDlg({ QStringLiteral("C:/ProjA") });
        stDlg.show();
        QTest::qWait(20);
        stDlg.hide();
        QTest::qWait(20);
        QVERIFY(!QFile::exists(oTmp.path() + QStringLiteral("/none.ini")));
    }
}

void TestExecPanel::recentDialogGeometryNoDrift() {
    // 窗口几何断言依赖真实窗口平台（offscreen 无原生窗口，漂移不发生，跳过）。
    if (QGuiApplication::platformName() == QLatin1String("offscreen"))
        QSKIP("window geometry drift requires a real window platform");
    // 完整生命周期循环：构造（恢复几何）→ show → hide（落盘）→ 销毁，三轮。
    //   修复前每轮 y 恒 -31（Qt6.8.3 setGeometry 客户区落点比请求高一个标题栏），
    //   累漂到顶。修复后 move()+resize() 精确，三轮 y 必须逐轮相等。
    QTemporaryDir oTmp;
    QVERIFY(oTmp.isValid());
    const QString sCfg = oTmp.path() + QStringLiteral("/config.ini");
    int iPrevY = -1;
    for (int i = 0; i < 3; ++i) {
        CRecentProjectsDialog stDlg({ QStringLiteral("C:/ProjA") }, QList<int>(), sCfg);
        stDlg.show();
        QTest::qWait(80);
        const int iY = stDlg.y();
        stDlg.hide();   // hideEvent → 落盘
        QTest::qWait(40);
        if (iPrevY >= 0) {
            QCOMPARE(iY, iPrevY);
        }
        iPrevY = iY;
    }
}

void TestExecPanel::asstAutoNameOnFinish() {
    // 2026.09.26 自动改名回归，2026-10-08 起时序改「先改名后执行」：
    //   ①默认名（助手N）+从未有提示词 → 【发送时】即触发 sigAsstAutoRename（素材含
    //     提示词；测试无编排器监听 → 命名轮未起 → 退回直接执行旧路径）；
    //   ②收尾不重复触发（bAutoNameTried）；③非默认名不触发；
    //   ④改名失败后再发送（已有提示词，名称仍默认）→ 回旧语义：收尾重试。
    CExecutionPanel panel;
    panel.resize(1000, 700);
    panel.show();
    QTabWidget* pTabs = panel.findChild<QTabWidget*>();
    QVERIFY(pTabs != nullptr);
    QJsonArray aAssts;
    aAssts.append(MakeAsst(QStringLiteral("助手1"), 0));      // 默认名
    aAssts.append(MakeAsst(QStringLiteral("装修达人"), 0));    // 非默认名
    panel.LoadAssistantsFromJson(aAssts, QString());
    QTest::qWait(100);
    panel.SetAdhocEnabled(true);
    QSignalSpy spyRename(&panel, &CExecutionPanel::sigAsstAutoRename);

    auto fnSwitchTo = [pTabs](const QString& sPrefix) -> QPlainTextEdit* {
        for (int i = 0; i < pTabs->count(); ++i) {
            if (pTabs->tabText(i).startsWith(sPrefix)) {
                pTabs->setCurrentIndex(i);
                return pTabs->widget(i)->findChild<QPlainTextEdit*>();
            }
        }
        return nullptr;
    };
    auto fnFinish = [&panel](const QString& sId) {
        panel.OnAdhocFinished(sId);
    };

    // ① 默认名首条：发送即触发改名（先改名），素材含提示词。
    QPlainTextEdit* pIn = fnSwitchTo(QStringLiteral("助手1"));
    QVERIFY(pIn != nullptr);
    const QString sId1 = panel.ActiveAsstId();
    pIn->setPlainText(QStringLiteral("帮我实现一个登录页面"));
    panel.DoAdhocSend(sId1);
    QCOMPARE(spyRename.count(), 1);   // 发送即触发（旧版：收尾才触发）
    QCOMPARE(spyRename.at(0).at(0).toString(), sId1);
    QVERIFY(spyRename.at(0).at(1).toString().contains(QStringLiteral("登录页面")));

    // ② 收尾（含命名轮收尾重入/失败场景）→ 不再触发，防死循环。
    fnFinish(sId1);
    QCOMPARE(spyRename.count(), 1);
    fnFinish(sId1);
    QCOMPARE(spyRename.count(), 1);

    // ③ 非默认名助手：发送+收尾 → 不触发。
    pIn = fnSwitchTo(QStringLiteral("装修达人"));
    QVERIFY(pIn != nullptr);
    const QString sId2 = panel.ActiveAsstId();
    QVERIFY(sId2 != sId1);
    pIn->setPlainText(QStringLiteral("随便干点啥"));
    panel.DoAdhocSend(sId2);
    fnFinish(sId2);
    QCOMPARE(spyRename.count(), 1);

    // ④ 默认名但已有提示词（改名失败重试场景）：发送不触发（非首条）→ 收尾触发（旧语义）。
    pIn = fnSwitchTo(QStringLiteral("助手1"));
    QVERIFY(pIn != nullptr);
    pIn->setPlainText(QStringLiteral("再加上注册功能"));
    panel.DoAdhocSend(sId1);
    QCOMPARE(spyRename.count(), 1);   // 非首条：发送不触发
    fnFinish(sId1);
    QCOMPARE(spyRename.count(), 2);   // 收尾重试
    panel.hide();
}

void TestExecPanel::asstAutoNameDeferredWhileRunning() {
    // 2026-10-03 运行中请求「自动重命名」→ 挂起到当前指令完成后执行（用户需求）：
    //   ①运行中请求：不发射、置挂起位（流内留事件行）；
    //   ②本轮收尾（OnAdhocFinished）：清挂起位 + 发射 sigAsstAutoRename（素材含
    //     已完成轮的提示词）+ 运行态已归零；
    //   ③挂起命名让位收尾自动命名（bAutoNameTried）：不双跑；再次收尾无残留；
    //   ④空闲时请求：立即发射。
    //   注：助手用非默认名（装修达人）——隔离 2026-10-08 首条「先改名后执行」路径
    //   （本测只验证右键挂起机制；首条发送立即改名见 asstAutoNameRenameFirstOnSend）。
    CExecutionPanel panel;
    panel.resize(1000, 700);
    panel.show();
    QTabWidget* pTabs = panel.findChild<QTabWidget*>();
    QVERIFY(pTabs != nullptr);
    QJsonArray aAssts;
    aAssts.append(MakeAsst(QStringLiteral("装修达人"), 0));   // 非默认名
    panel.LoadAssistantsFromJson(aAssts, QString());
    QTest::qWait(100);
    panel.SetAdhocEnabled(true);
    QSignalSpy spyRename(&panel, &CExecutionPanel::sigAsstAutoRename);
    QPlainTextEdit* pIn = nullptr;
    for (int i = 0; i < pTabs->count() && pIn == nullptr; ++i) {
        if (pTabs->tabText(i).startsWith(QStringLiteral("装修达人"))) {
            pTabs->setCurrentIndex(i);
            pIn = pTabs->widget(i)->findChild<QPlainTextEdit*>();
        }
    }
    QVERIFY(pIn != nullptr);
    const QString sId = panel.ActiveAsstId();

    // 发送提示词（素材入流）+ 置运行态（= 编排器 sigAdhocRunning 语义）
    pIn->setPlainText(QStringLiteral("帮我实现一个登录页面"));
    panel.DoAdhocSend(sId);
    panel.SetAsstRunning(sId, true);
    QVERIFY(panel.AsstRunning(sId));

    // ① 运行中请求 → 挂起，不发射
    QVERIFY(panel.TriggerAsstAutoRename(sId));
    QCOMPARE(spyRename.count(), 0);
    QVERIFY(panel.AsstRunning(sId));   // 挂起不打断当前轮

    // ② 本轮收尾 → 执行挂起命名（发射 + 运行态清零后再起新命名轮前可见）
    panel.OnAdhocFinished(sId);
    QCOMPARE(spyRename.count(), 1);
    QCOMPARE(spyRename.at(0).at(0).toString(), sId);
    QVERIFY(spyRename.at(0).at(1).toString().contains(QStringLiteral("登录页面")));
    QVERIFY(!panel.AsstRunning(sId));   // 测试环境无编排器回置运行态：收尾即空闲

    // ③ 再次收尾：挂起位已清 + bAutoNameTried 拦截收尾自动命名 → 不再发射
    panel.OnAdhocFinished(sId);
    QCOMPARE(spyRename.count(), 1);

    // ④ 空闲时请求 → 立即发射（原右键路径行为保持）
    QVERIFY(panel.TriggerAsstAutoRename(sId));
    QCOMPARE(spyRename.count(), 2);
    panel.hide();
}

void TestExecPanel::asstAutoNameRenameFirstOnSend() {
    // 2026-10-08 首条提示词「先改名后执行」回归：
    //   ①默认名（助手N）首条发送 → 立即发射 sigAsstAutoRename（素材含提示词）+
    //     命名轮同步起（模拟编排器链：auto_naming 事件 + 运行态）；提示词暂存不提交；
    //   ②命名期间追加 → 同样只暂存（不 emit sigAdhocSubmit，防注入污染命名轮）；
    //   ③命名收尾（OnAdhocFinished）→ 暂存全部转正上屏（用户块含时间戳前缀）+
    //     多条合并一次提交（格式同编排器 MergeQueuedPrompts，携带首条权限）；
    //   ④改名回填生效（Tab 标题更新）；
    //   ⑤停止撤销挂起：暂存退回输入框、命名收尾不补发。
    CExecutionPanel panel;
    panel.resize(1000, 700);
    panel.show();
    QTabWidget* pTabs = panel.findChild<QTabWidget*>();
    QVERIFY(pTabs != nullptr);
    QJsonArray aAssts;
    aAssts.append(MakeAsst(QStringLiteral("助手1"), 0));   // 默认名、无提问
    panel.LoadAssistantsFromJson(aAssts, QString());
    QTest::qWait(100);
    panel.SetAdhocEnabled(true);
    QSignalSpy spyRename(&panel, &CExecutionPanel::sigAsstAutoRename);
    QSignalSpy spySubmit(&panel, &CExecutionPanel::sigAdhocSubmit);
    // 模拟编排器命名链（同步）：AutoNameAsstNow → auto_naming 事件 + StartAdhocStage
    //   的 sigAdhocRunning（置运行态）。DoAdhocSend emit 返回后凭 bRunning 判定挂起。
    QObject::connect(&panel, &CExecutionPanel::sigAsstAutoRename,
                     &panel, [&panel](const QString& sId, const QString&) {
        panel.OnAdhocEvent(sId, QStringLiteral("auto_naming"),
                           QStringLiteral("⚙ 正在根据提示词生成助手名称…"));
        panel.SetAsstRunning(sId, true);
    });

    QPlainTextEdit* pIn = nullptr;
    QWidget* pPage = nullptr;
    for (int i = 0; i < pTabs->count(); ++i) {
        if (pTabs->tabText(i).startsWith(QStringLiteral("助手1"))) {
            pTabs->setCurrentIndex(i);
            pPage = pTabs->widget(i);
            pIn = pPage->findChild<QPlainTextEdit*>();
            break;
        }
    }
    QVERIFY(pIn != nullptr && pPage != nullptr);
    const QString sId = panel.ActiveAsstId();
    // 权限切到只读（验证挂起快照随补发携带）。
    QComboBox* pPerm = pPage->findChild<QComboBox*>();
    QVERIFY(pPerm != nullptr);
    pPerm->setCurrentIndex(2);
    QTest::qWait(30);

    // ① 首条发送：立即命名 + 挂起（不提交）。
    pIn->setPlainText(QStringLiteral("帮我实现一个登录页面"));
    panel.DoAdhocSend(sId);
    QCOMPARE(spyRename.count(), 1);   // 先改名（发送时立即触发）
    QVERIFY(spyRename.at(0).at(1).toString().contains(QStringLiteral("登录页面")));
    QCOMPARE(spySubmit.count(), 0);   // 未提交：等命名完成
    QCOMPARE(panel.StagedCount(sId), 1);   // 暂存中
    QVERIFY(panel.AsstRunning(sId));       // 命名轮运行态
    QCOMPARE(pIn->toPlainText(), QString());   // 输入框已清

    // ② 命名期间追加：同样只暂存不提交。
    pIn->setPlainText(QStringLiteral("再加上注册功能"));
    panel.DoAdhocSend(sId);
    QCOMPARE(spySubmit.count(), 0);
    QCOMPARE(panel.StagedCount(sId), 2);

    // ③ 命名收尾：全部转正 + 合并一次提交（含两条指令、首条权限）。且收尾不重复命名。
    panel.OnAdhocFinished(sId);
    QCOMPARE(spySubmit.count(), 1);
    QCOMPARE(spySubmit.at(0).at(0).toString(), sId);
    QVERIFY(spySubmit.at(0).at(1).toString().contains(QStringLiteral("登录页面")));
    QVERIFY(spySubmit.at(0).at(1).toString().contains(QStringLiteral("注册功能")));
    QVERIFY(spySubmit.at(0).at(1).toString().contains(QStringLiteral("【指令 1】")));
    QCOMPARE(spySubmit.at(0).at(2).toInt(), 2);   // 挂起时的权限快照
    QCOMPARE(panel.StagedCount(sId), 0);
    QVERIFY(panel.AsstRunning(sId));              // 补发轮持有运行态
    QCOMPARE(spyRename.count(), 1);              // 收尾不重复命名
    // 转正用户块上屏：两条 user 块，首条带日期时间戳前缀。
    {
        const QJsonArray aBlocks = panel.AssistantsToJson().at(0).toObject()
                .value(QStringLiteral("blocks")).toArray();
        int iUsers = 0;
        bool bFirstStamped = false;
        for (const QJsonValue& v : aBlocks) {
            const QJsonObject o = v.toObject();
            if (o.value(QStringLiteral("eventKind")).toString()
                    == QStringLiteral("user")) {
                ++iUsers;
                if (iUsers == 1) {
                    bFirstStamped = o.value(QStringLiteral("text")).toString()
                            .startsWith(QStringLiteral("[20"));
                }
            }
        }
        QCOMPARE(iUsers, 2);
        QVERIFY(bFirstStamped);
    }

    // ④ 改名回填：Tab 标题更新（先改名后执行的「改名」落地）。
    panel.ApplyAsstAutoName(sId, true, QStringLiteral("登录页助手"),
                            QStringLiteral("实现登录页面与注册"));
    bool bTitled = false;
    for (int i = 0; i < pTabs->count(); ++i) {
        if (pTabs->tabText(i).startsWith(QStringLiteral("登录页助手"))) {
            bTitled = true;
        }
    }
    QVERIFY(bTitled);

    // ⑤ 停止撤销挂起：新面板重搭一个干净的默认名助手。
    CExecutionPanel panel2;
    panel2.resize(1000, 700);
    panel2.show();
    QTabWidget* pTabs2 = panel2.findChild<QTabWidget*>();
    QVERIFY(pTabs2 != nullptr);
    QJsonArray aAssts2;
    aAssts2.append(MakeAsst(QStringLiteral("助手1"), 0));
    panel2.LoadAssistantsFromJson(aAssts2, QString());
    QTest::qWait(100);
    panel2.SetAdhocEnabled(true);
    QSignalSpy spySubmit2(&panel2, &CExecutionPanel::sigAdhocSubmit);
    QSignalSpy spyStop2(&panel2, &CExecutionPanel::sigAdhocStop);
    QObject::connect(&panel2, &CExecutionPanel::sigAsstAutoRename,
                     &panel2, [&panel2](const QString& sId2, const QString&) {
        panel2.SetAsstRunning(sId2, true);
    });
    QPlainTextEdit* pIn2 = nullptr;
    QWidget* pPage2 = nullptr;
    for (int i = 0; i < pTabs2->count(); ++i) {
        if (pTabs2->tabText(i).startsWith(QStringLiteral("助手1"))) {
            pTabs2->setCurrentIndex(i);
            pPage2 = pTabs2->widget(i);
            pIn2 = pPage2->findChild<QPlainTextEdit*>();
            break;
        }
    }
    QVERIFY(pIn2 != nullptr && pPage2 != nullptr);
    const QString sId2 = panel2.ActiveAsstId();
    pIn2->setPlainText(QStringLiteral("先别执行"));
    panel2.DoAdhocSend(sId2);
    QCOMPARE(spySubmit2.count(), 0);
    QCOMPARE(panel2.StagedCount(sId2), 1);
    // 运行中且输入空 → 发送按钮即「停止」：点它撤销挂起。
    QPushButton* pBtnStop = nullptr;
    const auto aBtns = pPage2->findChildren<QPushButton*>();
    for (QPushButton* pB : aBtns) {
        if (pB->text().contains(QStringLiteral("停止"))) { pBtnStop = pB; }
    }
    QVERIFY(pBtnStop != nullptr);
    QTest::mouseClick(pBtnStop, Qt::LeftButton);
    QCOMPARE(spyStop2.count(), 1);        // 停命名轮
    QCOMPARE(spySubmit2.count(), 0);      // 不补发
    QCOMPARE(panel2.StagedCount(sId2), 0);// 暂存清空
    QCOMPARE(pIn2->toPlainText(), QStringLiteral("先别执行"));   // 退回输入框
    // 命名轮被停的收尾：无挂起位 → 不补发。
    panel2.OnAdhocFinished(sId2);
    QCOMPARE(spySubmit2.count(), 0);
    panel.hide();
    panel2.hide();
}

void TestExecPanel::recentChipsPanel() {
    // 2026.09.26 v3.1.239 「最近打开」按钮面板回归（纯函数 + 面板交互）。
    const QFontMetrics stFm{QFont{}};
    constexpr int kiAvail = 400, kiSpacing = 6, kiHPad = 10;

    // —— ① LayoutChips：等名列表 —— 行满占满整宽（无 cap 时误差≤1）、宽度≤半宽上限
    {
        QStringList aNames;
        for (int i = 0; i < 20; ++i) { aNames << QStringLiteral("工程%1").arg(i); }
        const auto aRows = CRecentChipsPanel::LayoutChips(aNames, stFm, kiAvail, kiSpacing, kiHPad);
        QVERIFY(aRows.size() >= 2);
        int iTotal = 0;
        for (const auto& stRow : aRows) {
            int iSum = -kiSpacing * (stRow.aBtnW.size() - 1) - kiSpacing;   // 先减全部间距
            for (int w : stRow.aBtnW) {
                iSum += w + kiSpacing;
                QVERIFY(w <= kiAvail / 2);            // 用户规则 3：单按钮宽上限=半宽
                QVERIFY(w >= 1);
            }
            iTotal += stRow.aBtnW.size();
            // 行满：Σw+间距 = avail（整数化零头归最宽者，误差≤1）；单按钮行被 cap 钳位除外
            if (stRow.aBtnW.size() > 1) {
                const int iRowW = std::accumulate(stRow.aBtnW.begin(), stRow.aBtnW.end(), 0)
                                  + kiSpacing * (stRow.aBtnW.size() - 1);
                QVERIFY(qAbs(iRowW - kiAvail) <= 1);
            }
        }
        QCOMPARE(iTotal, 20);   // 无丢失无重复
    }

    // —— ② LayoutChips：单按钮行被半宽上限钳位；独行超长同样封顶（省略号兑底）
    {
        const auto aRows = CRecentChipsPanel::LayoutChips(
            { QStringLiteral("唯一工程") }, stFm, kiAvail, kiSpacing, kiHPad);
        QCOMPARE(aRows.size(), 1);
        QCOMPARE(aRows.at(0).aBtnW.at(0), kiAvail / 2);   // 钳到半宽（用户规则 3）
    }

    // —— ③ LayoutChips：长短名混排 —— 行内个数不同、多按钮行每按钮宽≥其最小宽（标题完整显示）
    {
        QStringList aNames = { QStringLiteral("a"), QStringLiteral("很长很长的工程名称测试用例"),
                               QStringLiteral("b"), QStringLiteral("c"),
                               QStringLiteral("另一个超长工程名称样例数据"), QStringLiteral("d") };
        const auto aRows = CRecentChipsPanel::LayoutChips(aNames, stFm, kiAvail, kiSpacing, kiHPad);
        QVERIFY(aRows.size() >= 2);   // 长名必然断行 → 每行个数不同
        bool bSawDiff = false;
        for (const auto& stRow : aRows) {
            for (int k = 0; k < stRow.aBtnW.size(); ++k) {
                const int iMinW = stFm.horizontalAdvance(aNames.at(stRow.iFirst + k))
                                  + 2 * kiHPad + 2;
                if (stRow.aBtnW.size() > 1) {
                    QVERIFY(stRow.aBtnW.at(k) >= iMinW);   // 完整显示：宽≥最小宽
                }
            }
            if (stRow.aBtnW.size() != aRows.at(0).aBtnW.size()) { bSawDiff = true; }
        }
        QVERIFY(bSawDiff);
    }

    // —— ④ ChipColorForCount：1~5 基色 → 100=(255,0,0) → 255=(100,0,0) → >255 恒定；
    //      5→100 R 单调升 G/B 降；100→255 R 单调降
    {
        const QColor clBase = CRecentChipsPanel::ChipColorForCount(1);
        QCOMPARE(CRecentChipsPanel::ChipColorForCount(5), clBase);
        QCOMPARE(clBase, QColor(225, 230, 238));
        QCOMPARE(CRecentChipsPanel::ChipColorForCount(100), QColor(255, 0, 0));
        QCOMPARE(CRecentChipsPanel::ChipColorForCount(255), QColor(100, 0, 0));
        QCOMPARE(CRecentChipsPanel::ChipColorForCount(300), QColor(100, 0, 0));
        const QColor cl6 = CRecentChipsPanel::ChipColorForCount(6);
        const QColor cl50 = CRecentChipsPanel::ChipColorForCount(50);
        const QColor cl99 = CRecentChipsPanel::ChipColorForCount(99);
        QVERIFY(cl50.red() > cl6.red() && cl99.red() > cl50.red() && cl99.red() < 255);
        QVERIFY(cl50.green() < cl6.green() && cl50.blue() < cl6.blue());
        const QColor cl150 = CRecentChipsPanel::ChipColorForCount(150);
        QVERIFY(cl150.green() == 0 && cl150.blue() == 0);
        QVERIFY(cl150.red() < 255 && cl150.red() > 100);
    }

    // —— ⑤ 面板交互：首个按钮=更多...；悬停状态栏显路径/离开回汇总；点击回调带路径
    {
        QStringList aOpened;
        bool bMore = false;
        CRecentChipsPanel stPanel(
            [&aOpened](const QString& sP) { aOpened.append(sP); },
            [&bMore]() { bMore = true; });
        stPanel.Refresh({ QStringLiteral("C:/ProjA"), QStringLiteral("D:/x/ProjB") },
                         { 3, 120 });
        stPanel.show();
        QVERIFY(QTest::qWaitForWindowExposed(&stPanel));
        QTest::qWait(50);
        QCOMPARE(stPanel.Chips().size(), 3);   // 更多... + 2 工程

        // 悬停工程按钮 → 状态栏显路径；离开 → 回汇总
        QWidget* pChipB = stPanel.ChipAt(2);
        QEnterEvent stEnter(QPointF(5, 5), QPointF(5, 5), QPointF(0, 0));
        QApplication::sendEvent(pChipB, &stEnter);
        QVERIFY(stPanel.StatusText().contains(QStringLiteral("ProjB")));
        QEvent stLeave(QEvent::Leave);
        QApplication::sendEvent(pChipB, &stLeave);
        QVERIFY(stPanel.StatusText().contains(QStringLiteral("共 2 个工程")));

        // 点击：工程按钮回调带路径；首个按钮=更多...
        QTest::mouseClick(stPanel.ChipAt(1), Qt::LeftButton);
        QCOMPARE(aOpened, QStringList({ QStringLiteral("C:/ProjA") }));
        QTest::mouseClick(stPanel.ChipAt(0), Qt::LeftButton);
        QVERIFY(bMore);
        QCOMPARE(aOpened.size(), 1);   // 更多不进工程回调

        // 面板渲染不崩溃（含悬停态假粗体 2x2 四次绘制路径）
        QImage stImg(stPanel.size(), QImage::Format_ARGB32);
        stPanel.render(&stImg);
        QEnterEvent stEnter2(QPointF(3, 3), QPointF(3, 3), QPointF(0, 0));
        QApplication::sendEvent(stPanel.ChipAt(1), &stEnter2);
        stPanel.render(&stImg);
        stPanel.hide();
    }
}

void TestExecPanel::recentChipsPanelMenuEmbed() {
    // 菜单内嵌（QWidgetAction）真实窗口验证：QMenu 打开期间面板按钮可点击/滚轮可滚。
    //   offscreen 平台 QSKIP（口径同 recentDialogGeometryNoDrift）。
    if (QGuiApplication::platformName() == QLatin1String("offscreen")) {
        QSKIP("menu embed interaction requires a real window platform");
    }
    QStringList aOpened;
    bool bMore = false;
    auto* pPanel = new CRecentChipsPanel(
        [&aOpened](const QString& sP) { aOpened.append(sP); },
        [&bMore]() { bMore = true; });
    QStringList aPaths;
    QList<int> aOpens;
    for (int i = 0; i < 60; ++i) {
        aPaths << QStringLiteral("C:/LongDir/ProjectLongName%1").arg(i);   // 长目录名 → 每行 3~4 个 → 必溢出 9 行封顶
        aOpens << (i % 120);
    }

    QMenu stMenu;
    auto* pWgtAct = new QWidgetAction(&stMenu);
    pWgtAct->setDefaultWidget(pPanel);
    stMenu.addAction(pWgtAct);
    stMenu.show();
    QVERIFY(QTest::qWaitForWindowExposed(&stMenu));
    QTest::qWait(50);
    // 与生产同序：菜单已显示（aboutToShow 语义）后刷新——真实宽度下布局
    pPanel->Refresh(aPaths, aOpens);
    QTest::qWait(80);

    // 点击第 3 个按钮（Proj2）——验证 QMenu 打开期间内嵌面板收到点击（关键决策：
    //   独立弹窗收不到，QWidgetAction 内嵌可以）
    QWidget* pChip = pPanel->ChipAt(3);
    QTest::mouseClick(pChip, Qt::LeftButton);
    QVERIFY(aOpened.contains(QStringLiteral("C:/LongDir/ProjectLongName2")));

    // 滚轮滚动：60 项超出 9 行封顶 → 滚轮后垂直滚动值前移（明确取垂直条）
    QScrollBar* pBar = nullptr;
    for (QScrollBar* pB : pPanel->findChildren<QScrollBar*>()) {
        if (pB->orientation() == Qt::Vertical) { pBar = pB; }
    }
    QVERIFY(pBar != nullptr);
    QVERIFY(pBar->maximum() > 0);   // 60 项必超 9 行可视上限
    const int iVal0 = pBar->value();
        QWheelEvent stWheel(QPointF(10, 10), QPointF(10, 10), QPoint(0, -120), QPoint(0, -120),
                            Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
        QApplication::sendEvent(pPanel->findChild<QAbstractScrollArea*>()->viewport(), &stWheel);
        QTest::qWait(80);
        QVERIFY(pBar->value() > iVal0);
    stMenu.hide();
}


void TestExecPanel::foreignPhantomLifecycle() {
    // 2026-10-02 跨档幽灵（任务助手不停跑跨档保留）核心逻辑：
    //   ①视图 key 切换后 TakeRunningForReload 摘下全部运行者（含外档）；
    //   ②AttachForeignAssistants 挂回；外档者粗体标记 + AssistantsJsonOf 分组过滤；
    //   ③外档页输入禁用；④DropAsstEntries 按 id 去重；⑤轮次结束除名后不再外档。
    CExecutionPanel panel;
    panel.resize(1100, 700);
    panel.show();
    QTabWidget* pTabs = panel.findChild<QTabWidget*>();
    QVERIFY(pTabs != nullptr);

    // 视图 = 任务档 "taskA"：载两个助手（A1 运行中由 SetAsstRunning 模拟）
    panel.SetViewKey(QStringLiteral("taskA"));
    QJsonArray aAssts;
    aAssts.append(MakeAsst(QStringLiteral("任务助手A1"), 3));
    aAssts.append(MakeAsst(QStringLiteral("任务助手A2"), 1));
    panel.LoadAssistantsFromJson(aAssts, QString());
    QTest::qWait(80);
    const QString sIdA1 = panel.AllAsstsSnapshot().at(0).sId;
    panel.SetAsstRunning(sIdA1, true);
    QCOMPARE(panel.RunningAsstIds().size(), 1);

    // 切视图到工程档（""）：载工程助手 B1；运行中的 A1 应被摘下并可挂回
    panel.SetViewKey(QString());
    QList<CExecutionPanel::SAsst> aTaken = panel.TakeRunningForReload();
    QCOMPARE(aTaken.size(), 1);                       // 只摘运行者（A1）
    QCOMPARE(aTaken.at(0).sId, sIdA1);
    QCOMPARE(aTaken.at(0).sOwnerKey, QStringLiteral("taskA"));
    QJsonArray aProj;
    aProj.append(MakeAsst(QStringLiteral("工程助手B1"), 0));
    panel.LoadAssistantsFromJson(aProj, QString());
    QTest::qWait(80);
    panel.AttachForeignAssistants(aTaken, QStringLiteral("某任务"));
    QTest::qWait(80);

    // 外档判定 + 分组落盘过滤
    QString sOwner;
    QVERIFY(panel.AsstForeignById(sIdA1, &sOwner));
    QCOMPARE(sOwner, QStringLiteral("taskA"));
    const QList<CExecutionPanel::SAsst> aSnap = panel.AllAsstsSnapshot();
    QCOMPARE(aSnap.size(), 2);                        // 工程助手B1 + 幽灵A1（文件版同 id 已被 Drop 语义避免）
    QCOMPARE(panel.AssistantsToJson().size(), 1);     // 视图档只含工程助手B1
    QCOMPARE(panel.AssistantsJsonOf(QStringLiteral("taskA")).size(), 1);   // 幽灵归 taskA 组

    // 外档页输入禁用（防跨档误操作）——按 Tab 标题找页
    QWidget* pForeignPage = nullptr;
    for (int i = 0; i < pTabs->count(); ++i) {
        if (pTabs->tabText(i) == QStringLiteral("任务助手A1")) {
            pForeignPage = pTabs->widget(i);
            break;
        }
    }
    QVERIFY(pForeignPage != nullptr);
    QPlainTextEdit* pForeignInput = pForeignPage->findChild<QPlainTextEdit*>();
    QVERIFY(pForeignInput != nullptr);
    QVERIFY(!pForeignInput->isEnabled());

    // DropAsstEntries：载入同 id 文件版让位（不产生重复 Tab）
    panel.LoadAssistantsFromJson(aProj, QString());   // 再载一次（模拟切回工程重复载）
    panel.DropAsstEntries(QStringList{ sIdA1 });      // 若文件版含 A1 则移除
    QCOMPARE(panel.AllAsstsSnapshot().size(), 1);     // 只有工程助手B1

    // 幽灵结束除名
    panel.AttachForeignAssistants(aTaken, QStringLiteral("某任务"));
    panel.RemoveAsstEntry(sIdA1);
    QCOMPARE(panel.AllAsstsSnapshot().size(), 1);
    QVERIFY(!panel.AsstForeignById(sIdA1));
}

void TestExecPanel::adhocInputAutoGrowOnInject() {
    // 2026.09.26 右键「添加到聊天框」注入长文本 → 输入框自动往上扩展高度：
    //   ①注入无回车长文本后高度增长（软换行视觉行计高，旧版 blockCount 恒 1 行不涨）；
    //   ②封顶 10 行；③清空回单行；④窗口变窄 → 软换行变多 → 高度重算更高。
    CExecutionPanel panel;
    panel.resize(1100, 700);
    panel.show();
    QTabWidget* pTabs = panel.findChild<QTabWidget*>();
    QVERIFY(pTabs != nullptr);
    QJsonArray aAssts;
    aAssts.append(MakeAsst(QStringLiteral("助手A"), 0));
    panel.LoadAssistantsFromJson(aAssts, QString());
    QTest::qWait(100);
    panel.SetAdhocEnabled(true);
    QPlainTextEdit* pInput = pTabs->currentWidget()->findChild<QPlainTextEdit*>();
    QVERIFY(pInput != nullptr);
    QTest::qWait(80);   // 首次布局安定

    const int iH0 = pInput->height();
    const QFontMetrics stFm = pInput->fontMetrics();
    const int iLineH = stFm.lineSpacing();
    QVERIFY(iH0 < iLineH * 2 + 40);   // 空输入=单行高度

    // ① 无回车长文本（右键注入路径的典型形态）→ 高度按视觉行增长
    panel.AppendToAdhocInput(QString(240, QLatin1Char('x')));   // 单块、无回车
    QTest::qWait(80);
    const int iH1 = pInput->height();
    QVERIFY(iH1 > iH0 + iLineH);          // 至少多出 2 行（240 字符远超一行）
    QVERIFY(iH1 <= iH0 + 9 * iLineH);     // 封顶 10 行

    // ② 超长文本封顶后不再无限增长
    panel.AppendToAdhocInput(QString(2000, QLatin1Char('y')));
    QTest::qWait(80);
    QVERIFY(pInput->height() <= iH0 + 9 * iLineH);

    // ③ 清空（textChanged）回单行
    pInput->clear();
    QTest::qWait(80);
    QCOMPARE(pInput->height(), iH0);

    // ④ 宽度变窄 → 同样文本软换行变多 → 高度重算更高（Resize 重算链）
    panel.AppendToAdhocInput(QString(160, QLatin1Char('z')));
    QTest::qWait(80);
    const int iWide = pInput->height();
    panel.resize(520, 700);
    QTest::qWait(120);
    const int iNarrow = pInput->height();
    QVERIFY(iNarrow > iWide);
    panel.hide();
}

QTEST_MAIN(TestExecPanel)
#include "test_execpanel.moc"
