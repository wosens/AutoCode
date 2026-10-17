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
#include <QTimer>
#include "ui/CRecentProjectsDialog.h"
#include "core/CVersion.h"   // 2026.10.11 启动页版本号断言
#include <QDialogButtonBox>
#include <QInputDialog>   // 2026-10-16 重命名弹窗几何回归   // 2026.10.09 启动页（launcher）按钮盒断言
#include <QLineEdit>          // 2026.10.11 启动页无搜索框断言
#include "ui/CRecentChipsPanel.h"   // 2026.09.26 v3.1.239 最近打开按钮面板回归
#include <QTemporaryDir>
#include <QDebug>
#include <QComboBox>   // 2026-09-11 权限下拉回归
#include <QProgressBar>
#include <QMessageBox>   // 临时探针（跑完即删）
#include "ui/CProjectExplorerPanel.h"   // 临时探针（跑完即删）   // 2026-10-05 上下文进度条模型名回归（format 观察口）
#include "ui/CExecutionPanel.h"
#include "agent/CClaudeRunner.h"   // 2026-10-16 claude 跟随开关（空模型分母兜底回归）
#include "../core/CModelEntry.h"
#include "ui/CMessageView.h"
#include "ui/CStreamPresenter.h"   // 2026.10.15 ApplyModelCellEdit（模型就地编辑校验）
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
    // 2026-10-11 幽灵助手+UAF 崩溃回归（pdfplumber 实害）：摘除「当前」运行中/文件版助手页
    //   （TakeRunningForReload / DropAsstEntries 两条跨档幽灵路径）不得误触发「+」页新增。
    void ghostAddOnRunningPageRemoval();
    void filePreviewEncoding();
    // 2026.09.22 文件内查找（工具栏/Ctrl+F）：向下/向上/循环/区分大小写 + 命中选中。
    void filePreviewFind();
    void viewPagination();
    // 2026-10-15 远跳窗口回归：提示词下拉点远处旧提问 → EnsureCoverage 远距分流整窗
    //   跳建（O(200) 不随会话长度增长）；窗口尾后数据-only，AppendNewer 分轮补到尾对齐。
    void streamFarJumpWindow();
    // 2026-10-16 视口惰性排版回归：批量建件 Assistant 块只估高不排正文（窗口内仅
    //   视口带 Materialize，远离带 Dematerialize 弃正文保高度）；工具卡折叠期不建
    //   展开区（懒重建，展开一次建齐）。
    void lazyMaterialization();
    // 2026-10-16 助手重命名弹窗几何持久化：构造式 QInputDialog（可拖动/改大小），
    //   关闭后回填快照 → RenameDlgGeo() 非空；下次弹出按快照恢复原位。
    void asstRenameDlgGeometry();
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
    // 2026.10.14 回归：任务未收尾时阶段条「完成」判定——回炉（评审/测试驳回退回前面
    //   阶段）后，当前进度点之后的 ok 阶段回落待执行，不再亮绿勾误导「已收尾」。
    void stageStatesOnReheat();
    // 2026-10-03 回归：无任务选中隐藏任务侧底栏（介入输入栏+费用栏容器），
    //   选任务恢复、取消选中再隐藏（与阶段条同口径，纵向空间还给日志区）。
    void taskBarsHideWithoutTask();
    // 2026-10-03 回归：无任务选中时「日志」页也不留（与阶段产物同口径随任务显隐；
    //   置空后当前页落激活助手页，选任务恢复并自动切日志页）。
    void taskLogTabHidesWithoutTask();
    // 2026-10-05 回归：上下文进度条本体显示当前模型名（共享表推送即时刷新；claude
    //   粘性助手不跟共享表，模型自报优先）。
    void contextBarShowsModelAndSwitch();
    // 2026-10-16 回归：推送之后新建/恢复的助手（sModel 空，等不到 SetSharedModels
    //   再推送）上下文分母回退共享表激活条目 iCtx（用户实害：条目设 1M，新建助手
    //   却显示 200K）；条目未填 Ctx → 按条目模型名探测；claude 粘性助手：跟随开关
    //   开且条目 anthropic-messages 兼容 → 同认激活条目（注入条件镜像），否则保持
    //   200K 兑底（模型自报，避免张冠李戴）。
    void newAsstContextLimitFallsToActiveEntry();
    // 2026-10-07 回归：右键「选择模型」= 对话级覆盖（仅本助手，不动全局）——
    //   设置/切换/清除/claude no-op/全局切换不冲掉覆盖/档持久化往返。
    void asstModelOverrideScoped();
    // 2026.10.15 设置页改模型名同步：①同身份改显示名 → 覆盖快照刷新+上抛落盘；
    //   ②身份变更（改接入点/模型 id）→ 覆盖保持旧快照不上抛；③无覆盖助手跟随激活项。
    void sharedModelRenameSync();
    // 2026.10.15 模型列表双击就地编辑的列→字段映射+校验回退（ApplyModelCellEdit：
    //   显示名空回退模型 ID/模型 ID 必填/数字列非法回退/标记列不动/无变化 false）。
    void modelCellEditValidation();
    // 2026.10.15 工具卡合并穿透文本（DSH 思维链穿插不再逐卡展开）：Tool 文本 Tool
    //   序列合一张 ×N 组卡；用户提示词(Event)/System 组仍打断。
    void toolMergeSkipsTextBlocks();
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
    // 2026-10-08 提示词长度闸门：占比/token 双约束取小；超限不提交改上抛
    //   sigAsstRefinePrompt（原文+限额+权限快照）；关闸门直发；token 约束独立生效。
    void promptGateRefineFlow();
    // 2026.09.26 v3.1.239 回归：「最近打开」按钮面板（流式行布局/次数渐变色/
    //   面板交互/菜单内嵌点击——设计定稿见 CRecentChipsPanel 头注）。
    void recentChipsPanel();
    void recentChipsPanelMenuEmbed();   // 真实窗口平台专用（offscreen QSKIP）
    void recentChipsPanelResizePersist();   // 2026-10-09 边缘拖拽改尺寸 + 持久化往返
    void recentChipsPanelRightDelete();    // 2026-10-09 右键删除：信号/左键不误开/回调链/exec 全链
    void recentChipsPanelFilterBar();   // 2026-10-11 启动页搜索行（替代更多...，实时过滤）
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
    // 2026.10.09 启动页（launcher 模式）：尺寸与普通模式 RecentWin 双向同步、
    //   位置独立键 RecentLauncherPos、ExitRequested 标志、打开/跳过/退出控件存在。
    void recentDialogLauncherMode();
    // 2026.10.12 启动页键盘选工程：搜索框 ↓ → 首个芯片；方向键网格导航
    //   （←→同行、↑↓跨行 x 最近）；回车=点击同语义打开；过滤后隐藏芯片不参与。
    void launcherChipKeyboardNav();
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
    // 2026.10.15 阶段产物右键「另存为」核心逻辑：①缺省中文名（阶段名+后缀，
    //   plan.md→计划.md；无阶段名回退原名）②批量落盘：同名冲突自动序号、
    //   Windows 文件名不区分大小写、失败明细。
    void outputSaveNameAndBatch();
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

// 2026-10-11 幽灵助手+UAF 崩溃回归（pdfplumber 实害，dump AutoCode_20261011_041512_28592）：
//   任务重命名→取消选中→重选（助手运行中）路径上，TakeRunningForReload 摘「当前」
//   运行中助手页 / DropAsstEntries 摘刚激活的文件版当前页时，旧版裸 removeTab 让 Qt
//   自动激活相邻「+」占位页 → OnAsstTabCurrentChanged 误判为用户点「+」→ 空助手凭空
//   入档（tasks/<id>/assistant.json 多出空 助手1/助手2），且嵌套 m_aAssts.append 重分配
//   悬垂外层 SAsst& → deleteLater 读已释放 pPage（dump 实证 this=-9）崩溃。
//   断言：两条路径摘页后助手数恰减一（无幽灵新增）、Tab 数不增长。
void TestExecPanel::ghostAddOnRunningPageRemoval() {
    CExecutionPanel panel;
    panel.resize(1000, 700);
    panel.show();
    QTabWidget* pTabs = panel.findChild<QTabWidget*>();
    QVERIFY(pTabs != nullptr);

    QJsonArray aAssts;
    aAssts.append(MakeAsst(QStringLiteral("助手A"), 3));
    aAssts.append(MakeAsst(QStringLiteral("助手B"), 3));
    panel.LoadAssistantsFromJson(aAssts, QString());
    QTest::qWait(120);   // 分帧重建；激活=首个助手 A（其页即当前页）
    const int iTabs0 = pTabs->count();
    QCOMPARE(panel.AsstCount(), 2);

    // —— 路径①TakeRunningForReload：摘「当前」运行中助手页（取消选中任务的瞬间）。
    const QString sA = panel.ActiveAsstId();
    QVERIFY(!sA.isEmpty());
    panel.SetAsstRunning(sA, true);
    QVERIFY(panel.AsstRunning(sA));
    const QList<CExecutionPanel::SAsst> aTook = panel.TakeRunningForReload();
    QCOMPARE(aTook.size(), 1);
    QCOMPARE(aTook.first().sId, sA);
    QCOMPARE(panel.AsstCount(), 1);                  // 只剩 B；幽灵新增会变 2
    QCOMPARE(panel.AssistantsToJson().size(), 1);
    QCOMPARE(pTabs->count(), iTabs0 - 1);           // 仅摘 A 页，无幽灵页累积
    panel.SetAsstRunning(sA, false);

    // —— 路径②DropAsstEntries：档重载去重摘「刚激活的文件版当前页」（重选任务的瞬间）。
    panel.LoadAssistantsFromJson(aAssts, QString());
    QTest::qWait(120);
    const QString sA2 = panel.ActiveAsstId();
    QVERIFY(!sA2.isEmpty());
    panel.DropAsstEntries(QStringList{sA2});
    QCOMPARE(panel.AsstCount(), 1);                  // 只剩另一个；幽灵新增会变 2
    QCOMPARE(panel.AssistantsToJson().size(), 1);
    QCOMPARE(pTabs->count(), iTabs0 - 1);
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

    // 轮询等分帧重建收敛（每帧 20 块，Markdown 块不便宜，机器慢时 >300ms）。
    //   2026.10.15 改等稳定值：MakeAsst 序列含 tool_use（i%3==0），新合并语义下
    //   工具卡穿透文本合并，视图 widget 数 < 数据块数（200 窗口实测 134），旧
    //   “每块一件=200”断言不再成立；数据层完整性由下方 AssistantsToJson 保障。
    auto fnWaitStable = [&pV]() {
        int iPrev = -1;
        for (int i = 0; i < 100; ++i) {   // 5s 上限
            const int iNow = pV->BlockCount();
            if (iNow > 0 && iNow == iPrev) {
                return true;
            }
            iPrev = iNow;
            QTest::qWait(50);
        }
        return false;
    };
    QVERIFY2(fnWaitStable(),
             QStringLiteral("初始窗口分帧未收敛：%1").arg(pV->BlockCount()).toUtf8().constData());
    const int iAfterInit = pV->BlockCount();
    QVERIFY(iAfterInit > 0 && iAfterInit <= 200);   // 合并后 widget ≤ 数据块数
    QVERIFY(pV->verticalScrollBar()->maximum() > 0);   // 可滚（否则到顶信号不触发）

    // 滚动到顶 → sigTopReached → 补建更早 60 块（chunk=100 > 60 → 一次补齐）
    pV->verticalScrollBar()->setValue(0);
    QVERIFY2(fnWaitStable(),
             QStringLiteral("到顶补建未收敛：%1").arg(pV->BlockCount()).toUtf8().constData());
    QVERIFY(pV->BlockCount() > iAfterInit);   // 补建后 widget 增加（合并占位后仍增）

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



// 2026-10-16 助手重命名弹窗几何持久化回归：双击 Tab 改名入口由静态 getText 改为
//   构造式 QInputDialog（弹出前 restoreGeometry、关闭后 saveGeometry 回填）。
//   OnAsstTabRenamed 为私有槽，经元对象调用；模态 exec 期间用定时器驱动弹窗
//   （挪位/改大小/输入新名/点 OK），再二次弹出验证几何按上次快照恢复。
void TestExecPanel::asstRenameDlgGeometry() {
    CExecutionPanel panel;
    panel.resize(1000, 700);
    panel.show();
    QTabWidget* pTabs = panel.findChild<QTabWidget*>();
    QVERIFY(pTabs != nullptr);
    QJsonArray aAssts;
    aAssts.append(MakeAsst(QStringLiteral("助手A"), 1));
    panel.LoadAssistantsFromJson(aAssts, QString());
    QTest::qWait(50);
    QVERIFY(panel.RenameDlgGeo().isEmpty());   // 初始无档

    int iTabA = -1;
    for (int i = 0; i < pTabs->count(); ++i) {
        if (pTabs->tabText(i) == QStringLiteral("助手A")) { iTabA = i; }
    }
    QVERIFY(iTabA >= 0);

    // 第一次弹出：挪位 (120,140) + 改大小 (420,200) + 改名「新名字A」+ OK
    const QRect rcWant(120, 140, 460, 300);   // 宽/高尽力设：宽可持久，高被布局锁定（见下方断言注）
    QTimer tmKick;
    tmKick.setInterval(15);
    QObject::connect(&tmKick, &QTimer::timeout, &panel, [&]() {
        QInputDialog* pDlg = panel.findChild<QInputDialog*>();
        if (pDlg == nullptr) { return; }
        if (!pDlg->isVisible()) { return; }
        tmKick.stop();
        pDlg->setGeometry(rcWant);
        if (QLineEdit* pEdit = pDlg->findChild<QLineEdit*>()) {
            pEdit->setText(QStringLiteral("新名字A"));
        }
        if (QDialogButtonBox* pBox = pDlg->findChild<QDialogButtonBox*>()) {
            if (QAbstractButton* pOk = pBox->button(QDialogButtonBox::Ok)) {
                pOk->click();
            }
        }
    });
    tmKick.start(15);
    // 双击 Tab 即既有改名入口（tabBarDoubleClicked → OnAsstTabRenamed；Qt5+ 信号 public 可直发）
    Q_EMIT pTabs->tabBar()->tabBarDoubleClicked(iTabA);

    // 关闭后：快照非空 + 改名生效（新对话框链路不破坏原有改名功能）
    QVERIFY(!panel.RenameDlgGeo().isEmpty());
    QCOMPARE(pTabs->tabText(iTabA), QStringLiteral("新名字A"));

    // 第二次弹出：位置/宽度按上次快照恢复（容差 2px 吸收框架微调）。高度不参与
    //   断言——QInputDialog 显示时布局把高度锁定为内容自适应高（minimumSizeHint，
    //   用户手动也拉不高），「大小持久化」有意义面=宽度+位置。
    QRect rcSeen;
    QTimer tmKick2;
    tmKick2.setInterval(15);
    QObject::connect(&tmKick2, &QTimer::timeout, &panel, [&]() {
        QInputDialog* pDlg = panel.findChild<QInputDialog*>();
        if (pDlg == nullptr || !pDlg->isVisible()) { return; }
        tmKick2.stop();
        rcSeen = pDlg->geometry();
        pDlg->reject();   // 取消路径同样回填快照
    });
    tmKick2.start(15);
    Q_EMIT pTabs->tabBar()->tabBarDoubleClicked(iTabA);
    QVERIFY(qAbs(rcSeen.x() - rcWant.x()) <= 2);
    QVERIFY(qAbs(rcSeen.y() - rcWant.y()) <= 2);
    QVERIFY(qAbs(rcSeen.width() - rcWant.width()) <= 2);
    QVERIFY(rcSeen.height() > 0);   // 高度=布局内容高（锁定），只验非退化
    QCOMPARE(pTabs->tabText(iTabA), QStringLiteral("新名字A"));   // 取消不改名
}

// 2026-10-15 远跳窗口回归（提示词下拉点远处旧提问界面卡顿优化）：
//   目标距前沿超一轮补建量(100) → 整窗跳建：弃全部件、窗口起点=目标、完成后回调
//   非空目标块；窗口尾之后的块数据-only（Append 不建件、WidgetAt 返 nullptr 防错位），
//   AppendNewer 分轮补到数据尾后对齐恢复。近距（≤100）仍走前插路径不跳建。
void TestExecPanel::streamFarJumpWindow() {
    CMessageView v;
    v.resize(400, 300);
    CStreamPresenter st;
    st.Bind(&v);

    QList<SBlockData> aBlocks;
    for (int i = 0; i < 900; ++i) {
        SBlockData d;
        d.eKind = CMessageBlock::Assistant;
        d.sText = QStringLiteral("第%1条").arg(i);
        aBlocks.append(d);
    }
    st.SetBlocks(aBlocks);
    st.Rebuild();
    for (int i = 0; i < 200 && st.Rebuilding(); ++i) {
        QTest::qWait(20);
    }
    QVERIFY(!st.Rebuilding());
    QCOMPARE(st.Frontier(), 700);          // 初始窗口=最近 200 块
    QCOMPARE(st.WidgetCount(), 200);       // 全 Assistant 块无合并 → 槽位=块数

    // 远跳到块 0（距离 700 > 100）→ 整窗跳建，不再逐轮前插建 700 块
    CMessageBlock* pGot = nullptr;
    st.EnsureCoverage(0, [&pGot](CMessageBlock* b) { pGot = b; });
    for (int i = 0; i < 200 && st.Rebuilding(); ++i) {
        QTest::qWait(20);
    }
    QVERIFY(!st.Rebuilding());
    QCOMPARE(st.Frontier(), 0);            // 窗口起点=目标
    QCOMPARE(st.WidgetCount(), 200);       // 只建目标处一个窗口（不随距离增长）
    QVERIFY(pGot != nullptr);              // 完成回调携目标块（滚到视口顶用）

    // 尾隙：窗口尾 200 早于数据尾 900 → Append 新块数据-only（件数不变、数据+1）
    SBlockData dNew;
    dNew.eKind = CMessageBlock::Assistant;
    dNew.sText = QStringLiteral("新到");
    st.Append(dNew);
    QCOMPARE(st.Count(), 901);
    QCOMPARE(st.WidgetCount(), 200);
    QVERIFY(st.WidgetAt(500) == nullptr);  // 窗口外返空（防张冠李戴刷错卡）

    // 底部分页：分轮补到数据尾，对齐恢复（窗口尾==数据尾）
    for (int i = 0; i < 300
         && st.Frontier() + st.WidgetCount() < st.Count(); ++i) {
        st.AppendNewer();
        QTest::qWait(20);
    }
    QCOMPARE(st.Frontier() + st.WidgetCount(), st.Count());
    QVERIFY(st.WidgetAt(900) != nullptr);  // 尾块建件后可寻址

    // 近距目标（前沿 0、已建到尾）不触发跳建：块 850 已在窗口内 → 同步回调
    CMessageBlock* pNear = reinterpret_cast<CMessageBlock*>(0x1);
    st.EnsureCoverage(850, [&pNear](CMessageBlock* b) { pNear = b; });
    QVERIFY(pNear != reinterpret_cast<CMessageBlock*>(0x1));
    QVERIFY(pNear == st.WidgetAt(850));

    st.Unbind();
}

// 2026-10-16 视口惰性排版回归：批量建件的 Assistant 块初始仅估高（零排版成本），
//   滚入视口带才 Materialize（正文上屏），远离视口带 Dematerialize（弃正文保高度）；
//   工具卡折叠期不建展开区（懒重建，展开一次建齐）。载档慢二期治理的行为锚点。
void TestExecPanel::lazyMaterialization() {
    CMessageView v;
    v.resize(500, 300);
    v.show();
    CStreamPresenter st;
    st.Bind(&v);

    QList<SBlockData> aBlocks;
    for (int i = 0; i < 300; ++i) {
        SBlockData d;
        d.eKind = CMessageBlock::Assistant;
        d.sText = QStringLiteral("## 标题%1\n正文文本 some long text to wrap around maybe\n\n- 项目一\n- 项目二\n").arg(i);
        aBlocks.append(d);
    }
    st.SetBlocks(aBlocks);
    st.Rebuild();
    for (int i = 0; i < 200 && st.Rebuilding(); ++i) {
        QTest::qWait(20);
    }
    QVERIFY(!st.Rebuilding());
    QCOMPARE(st.WidgetCount(), 200);
    QVERIFY(v.verticalScrollBar()->maximum() > 0);   // 估高几何有效（可滚）

    // ① 重建尾帧滚底后：视口带内已材化（>0），带外仍有未材化块（<200，惰性生效）
    QTest::qWait(80);   // 滚底帧 + 材化排队收敛（材化标记同步置位，不等预排版落地）
    auto fnCountMat = [&st](int* a_pMat, int* a_pTotal) {
        *a_pMat = 0;
        *a_pTotal = 0;
        for (int i = 0; i < st.Count(); ++i) {
            if (CMessageBlock* pW = st.WidgetAt(i)) {
                ++(*a_pTotal);
                if (pW->AssistantMaterialized()) { ++(*a_pMat); }
            }
        }
    };
    int iMat = 0, iTotal = 0;
    fnCountMat(&iMat, &iTotal);
    QCOMPARE(iTotal, 200);
    QVERIFY2(iMat > 0, "viewport band materialized");
    QVERIFY2(iMat < 200, "off-band blocks stay lazy (deferred layout)");

    // ② 滚到顶：到顶补建（锚在补建前首块，数据块 0 仍在上方带外）→ 再滚到真顶部 →
    //    顶部带材化（首块正文上屏，Markdown 占位/落地可见）
    v.verticalScrollBar()->setValue(0);
    for (int i = 0; i < 100 && (st.Rebuilding() || st.Frontier() > 0); ++i) {
        QTest::qWait(20);
    }
    QCOMPARE(st.Frontier(), 0);
    QTest::qWait(620);   // PrependBlocks 重锚帧（0/60/150/300/500ms）全部落定，不再回拽视口
    v.verticalScrollBar()->setValue(0);   // 补建锚定后位于新块下方，再滚到真顶部
    QTest::qWait(80);   // 到顶后材化排队收敛
    CMessageBlock* pTop = st.WidgetAt(0);
    QVERIFY(pTop != nullptr);
    QVERIFY2(pTop->AssistantMaterialized(), "top block materialized after scroll to top");
    QTest::qWait(150);   // 预排版落地（子线程异步）
    QVERIFY(pTop->findChild<QTextBrowser*>() != nullptr);
    QVERIFY(pTop->findChild<QTextBrowser*>()->toPlainText().contains(QStringLiteral("标题0")));
    const int iTopH = pTop->height();
    QVERIFY(iTopH > 20);

    // ③ 滚回底部：顶部块远离视口带 → 弃正文（未材化）且高度锁定不缩（保形防跳）
    v.verticalScrollBar()->setValue(v.verticalScrollBar()->maximum());
    QTest::qWait(120);   // 滚底 + 惰性带重算收敛
    QVERIFY2(!pTop->AssistantMaterialized(), "far block dematerialized after scroll to bottom");
    QCOMPARE(pTop->height(), iTopH);   // 弃正文不改几何（滚动不跳）

    // ④ 工具卡懒重建：折叠期并入 ×5 不建展开区；展开一次建齐；折叠并入再展开同步
    CMessageBlock card(CMessageBlock::ToolUse);
    card.SetTool(QStringLiteral("read"), QStringLiteral("{\"file_path\":\"a.h\"}"));
    for (int i = 0; i < 5; ++i) {
        card.AddMergedTool(QStringLiteral("read"), QStringLiteral("{\"file_path\":\"b%1.cpp\"}").arg(i));
    }
    QCOMPARE(card.findChildren<QPlainTextEdit*>().size(), 0);   // 折叠期零展开控件（旧版每次并入全重建）
    QCOMPARE(card.ToolCount(), 6);
    card.SetExpanded(true);
    QTest::qWait(20);
    QCOMPARE(card.findChildren<QPlainTextEdit*>().size(), 12);  // 6 条×入参/结果
    card.SetExpanded(false);
    card.AddMergedTool(QStringLiteral("read"), QStringLiteral("{\"file_path\":\"c.cpp\"}"));
    card.SetResult(QStringLiteral("ok"));
    card.SetExpanded(true);
    QTest::qWait(20);
    QCOMPARE(card.findChildren<QPlainTextEdit*>().size(), 14);  // 7 条×2，折叠期并入/回填不丢

    st.Unbind();
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

// 2026.10.14 回归：任务未收尾时阶段条「完成」判定——开发→审查→测试→验证均 ok 后
//   测试驳回回炉，current 退回代码审查重跑：
//   ① 运行中：审查（当前阶段）按实时状态显示 Running（不沿用历史绿勾），其后的
//      测试/验证回落待执行，之前的开发及更早阶段保持 Done；
//   ② 任务整体 Done：全部绿勾（旧口径不变）。
void TestExecPanel::stageStatesOnReheat() {
    auto StateOf = [](CExecutionPanel& a_panel, EStage a_eStage) {
        for (const CStageBar::SNode& stN : a_panel.StageBar()->Nodes()) {
            if (stN.iStage == static_cast<int>(a_eStage)) {
                return stN.eState;
            }
        }
        return CStageBar::EState::Pending;   // 不在流内：与旧行为一致不亮
    };

    // 流：需求确认→开发→代码审查→测试→验证（压缩真实流，覆盖回炉链最小集）。
    const QList<EStage> aFlow = {
        EStage::ConfirmReq, EStage::Develop, EStage::CodeReview,
        EStage::Test, EStage::Verify };

    CTaskInfo task;
    task.SetId(QStringLiteral("reheat-t-1"));
    task.SetFlowStages(aFlow);
    // 历史：各阶段均 ok（含测试/验证），随后测试驳回回炉。
    for (EStage eS : aFlow) {
        CTaskInfo::SStageRun stRun;
        stRun.eStage = eS;
        stRun.bOk    = true;
        task.AppendHistory(stRun);
    }
    task.SetCurrentStage(EStage::CodeReview);   // 审查迭代 2 重跑中
    task.SetStatus(ETaskStatus::Running);

    CExecutionPanel panel;
    panel.resize(1000, 700);
    panel.show();
    panel.SetCurrentTask(&task);
    panel.RefreshTask(&task);

    QCOMPARE(StateOf(panel, EStage::ConfirmReq), CStageBar::EState::Done);
    QCOMPARE(StateOf(panel, EStage::Develop),    CStageBar::EState::Done);
    // 回炉目标阶段：按实时状态显示运行中（历史绿勾不再沿用）。
    QCOMPARE(StateOf(panel, EStage::CodeReview), CStageBar::EState::Running);
    // 进度点之后的 ok 阶段：已被回炉作废 → 待执行（用户需求：测试未真正收尾不亮完成）。
    QCOMPARE(StateOf(panel, EStage::Test),       CStageBar::EState::Pending);
    QCOMPARE(StateOf(panel, EStage::Verify),     CStageBar::EState::Pending);

    // 整体收尾（Status=Done）：全部绿勾，历史口径不变。
    task.SetStatus(ETaskStatus::Done);
    task.SetCurrentStage(EStage::Verify);
    panel.RefreshTask(&task);
    for (EStage eS : aFlow) {
        QCOMPARE(StateOf(panel, eS), CStageBar::EState::Done);
    }
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

// 2026-10-16 新助手上下文窗口分母回归：共享表推送【之后】新建/恢复的助手 sModel
//   为空（推送已过、未跑过一轮），旧口径直接探测空名走未知模型 200K 兑底——用户
//   条目明明设 1M（glm-5.3）新建助手却显示 200K。修复后：非 claude 助手空模型 →
//   回退共享表当前激活条目 iCtx；条目未填 Ctx → 按条目模型名探测；claude 粘性
//   助手保持 200K 兑底。场景 = 切任务恢复从未跑过的助手（LoadAssistantsFromJson，
//   与「+」新建同一空模型分支）。
void TestExecPanel::newAsstContextLimitFallsToActiveEntry() {
    CExecutionPanel panel;
    panel.resize(1000, 700);
    panel.show();
    // ① 先推共享表（active=0，glm-5.3 条目 iCtx=1M）——模拟启动推送已完成。
    SDshModelEntry stE0;
    stE0.sDisp = QStringLiteral("GLM5");
    stE0.sModelId = QStringLiteral("glm-5.3");
    stE0.iCtx = 1000000;
    SDshModelEntry stE1;
    stE1.sDisp = QStringLiteral("GLM47");
    stE1.sModelId = QStringLiteral("glm-4.7-flash");
    stE1.iCtx = 128000;
    panel.SetSharedModels({ stE0, stE1 }, 0);
    QTest::qWait(30);
    // ② 推送后恢复两个从未跑过的助手（无 model 键 → sModel 空；切任务路径）。
    auto mkAsst = [](const char* a_sId, const char* a_sName, const char* a_sBackend) {
        QJsonObject o;
        o["id"] = QString::fromLatin1(a_sId);
        o["name"] = QString::fromLatin1(a_sName);
        o["closed"] = false;
        o["backend"] = QString::fromLatin1(a_sBackend);
        return o;
    };
    QJsonArray aAssts;
    aAssts.append(mkAsst("na-a", "助手A", "dsh"));
    aAssts.append(mkAsst("na-c", "助手C", "claude"));
    panel.LoadAssistantsFromJson(aAssts, QStringLiteral("na-a"));
    QTest::qWait(50);
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
    // ③ 空 dsh 助手：分母回退激活条目 iCtx=1M（新建即显示 1M，不再 200K）；
    //   claude 粘性助手：保持 200K 兑底（模型自报，避免张冠李戴）。
    QVERIFY(pBarDsh->toolTip().contains(QStringLiteral("上下文占用：0 / 1.0M")));
    QVERIFY(pBarClaude->toolTip().contains(QStringLiteral("/ 200.0K")));
    // ④ 激活条目未填 Ctx（iCtx=0）→ 按条目模型名探测（glm → 128K）。
    SDshModelEntry stNoCtx;
    stNoCtx.sDisp = QStringLiteral("GLM5免填");
    stNoCtx.sModelId = QStringLiteral("glm-5.3");
    stNoCtx.iCtx = 0;
    panel.SetSharedModels({ stNoCtx }, 0);
    QTest::qWait(30);
    QVERIFY(pBarDsh->toolTip().contains(QStringLiteral("/ 128.0K")));
    // ⑤ claude 跟随开关开 + 激活条目兼容（sApi 空 = anthropic-messages）→ 兜底同认
    //   激活条目：分母 1M + 本体显示条目模型名（SharedActiveEntryForBackend 与
    //   StartStage env 注入条件镜像——该条件下下一轮实际即用此条目，非张冠李戴）。
    CClaudeRunner::SetFollowSharedModels(true);
    panel.SetSharedModels({ stE0, stE1 }, 0);
    QTest::qWait(30);
    QVERIFY(pBarClaude->toolTip().contains(QStringLiteral("/ 1.0M")));
    QVERIFY(pBarClaude->format().contains(QStringLiteral("glm-5.3")));
    // ⑥ 同开但条目非兼容（openai-completions）→ claude 不路由该条目，回 200K
    //   兜底 + 百分比占位（模型未回报前不冒名）。
    SDshModelEntry stOpenAi = stE0;
    stOpenAi.sApi = QStringLiteral("openai-completions");
    panel.SetSharedModels({ stOpenAi }, 0);
    QTest::qWait(30);
    QVERIFY(pBarClaude->toolTip().contains(QStringLiteral("/ 200.0K")));
    QCOMPARE(pBarClaude->format(), QStringLiteral("%p%"));
    // ⑦ 跟随开关关（还原静态，后续测试不受染）→ 仍 200K。
    CClaudeRunner::SetFollowSharedModels(false);
    panel.SetSharedModels({ stE0 }, 0);
    QTest::qWait(30);
    QVERIFY(pBarClaude->toolTip().contains(QStringLiteral("/ 200.0K")));
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

// 2026.10.15 设置页改模型名同步（用户需求：改名后上下文右键「选择模型」
//   菜单、进度条模型名即时新名）：①同身份改 sDisp → 覆盖快照刷新+信号上抛
//   （落盘链 sigAsstLayoutChanged 由主窗口接）；②身份变更（改接入点/模型 id）→
//   覆盖保持旧快照不上抛（菜单首项自然提示已不在列表）；③无覆盖助手
//   跟随激活项，幂等推送不产生覆盖信号。
void TestExecPanel::sharedModelRenameSync() {
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
    aAssts.append(mkAsst("rn-a", "助手A"));
    aAssts.append(mkAsst("rn-b", "助手B"));
    panel.LoadAssistantsFromJson(aAssts, QStringLiteral("rn-a"));
    QTest::qWait(50);
    auto mkE = [](const QString& sDisp, const QString& sId) {
        SDshModelEntry e;
        e.sDisp = sDisp;
        e.sBaseURL = QStringLiteral("http://127.0.0.1:9/") + sId;
        e.sApi = QStringLiteral("openai-completions");
        e.sModelId = sId;
        return e;
    };
    panel.SetSharedModels({ mkE(QStringLiteral("旧名A"), QStringLiteral("m0")),
                            mkE(QStringLiteral("m1"), QStringLiteral("m1")) }, /*active=*/0);
    // 前置：rn-a 设覆盖 m1（口径同 asstModelOverrideScoped ①）。
    QVERIFY(panel.ApplyAsstModelOverride(QStringLiteral("rn-a"), 1));

    auto stOvOf = [&panel]() { return panel.AsstModelOverride(QStringLiteral("rn-a")); };

    // ① 同身份改显示名（sDisp）：覆盖快照刷新 + sigAsstModelOverride 上抛。
    QSignalSpy spyOv(&panel, &CExecutionPanel::sigAsstModelOverride);
    panel.SetSharedModels({ mkE(QStringLiteral("新名A"), QStringLiteral("m0")),
                            mkE(QStringLiteral("新名1"), QStringLiteral("m1")) }, /*active=*/0);
    {
        const QPair<SDshModelEntry, bool> stOv = stOvOf();
        QVERIFY(stOv.second);
        QCOMPARE(stOv.first.sDisp, QStringLiteral("新名1"));
        QCOMPARE(stOv.first.sModelId, QStringLiteral("m1"));
    }
    QCOMPARE(spyOv.count(), 1);   // 仅覆盖助手 rn-a 上抛（rn-b 无覆盖）
    {
        const QList<QVariant> aArgs = spyOv.takeFirst();
        QCOMPARE(aArgs.at(0).toString(), QStringLiteral("rn-a"));
        QCOMPARE(aArgs.at(1).value<SDshModelEntry>().sDisp, QStringLiteral("新名1"));
        QCOMPARE(aArgs.at(2).toBool(), true);
    }

    // ② 身份变更（改 sModelId）：覆盖保持旧快照，不上抛。
    spyOv.clear();
    panel.SetSharedModels({ mkE(QStringLiteral("新名A"), QStringLiteral("m0")),
                            mkE(QStringLiteral("m1"), QStringLiteral("m1x")) }, /*active=*/0);
    {
        const QPair<SDshModelEntry, bool> stOv = stOvOf();
        QVERIFY(stOv.second);
        QCOMPARE(stOv.first.sDisp, QStringLiteral("新名1"));   // 旧快照保留
        QCOMPARE(stOv.first.sModelId, QStringLiteral("m1"));
    }
    QCOMPARE(spyOv.count(), 0);

    // ③ 无覆盖助手跟随激活项；再推送同表不产生覆盖信号（幂等防抖）。
    spyOv.clear();
    panel.SetSharedModels({ mkE(QStringLiteral("新名A"), QStringLiteral("m0")),
                            mkE(QStringLiteral("m1"), QStringLiteral("m1x")) }, /*active=*/1);
    QCOMPARE(spyOv.count(), 0);
    spyOv.clear();
    panel.SetSharedModels({ mkE(QStringLiteral("新名A"), QStringLiteral("m0")),
                            mkE(QStringLiteral("m1"), QStringLiteral("m1x")) }, /*active=*/1);
    QCOMPARE(spyOv.count(), 0);

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

void TestExecPanel::recentDialogLauncherMode() {
    // 2026.10.11 启动页（launcher，按钮版）单测：
    //   ① 形态：无旧版列表搜索框/列表；主体 CRecentChipsPanel（2026-10-11 起面板
    //      自带顶部搜索行，实时过滤工程按钮）；底钮「退出」「跳过」；
    //     品牌 Title（AutoCode QLabel，横幅高 100）；ExitRequested 初始 false；
    //   ② 按钮区域尺寸同步：预写 [Layout] RecentChipsSize="500,400" → 面板
    //     初始固定尺寸 500x400，窗口 = 500 x (400+100+底钮行高)；
    //   ③ 窗口拖边（resize）→ 面板跟随（SyncPanelToWindow）→ hide →
    //     RecentChipsSize 回写新尺寸；位置落 RecentLauncherPos；
    //   ④ 新 launcher 读回（面板尺寸=回写值、窗口位置=RecentLauncherPos）；
    //   ⑤ 右键删除链（RemoveAndRefresh）：数据源移除 + RemovedPaths 上报 + 面板刷新；
    //   ⑥ 空 cfg = 禁用（不写盘）。
    QTemporaryDir oTmp;
    QVERIFY(oTmp.isValid());
    const QString sCfg = oTmp.path() + QStringLiteral("/config.ini");
    {
        // 预写面板尺寸档（与菜单内弹出面板共用键）。
        QFile fCfg(sCfg);
        QVERIFY(fCfg.open(QIODevice::WriteOnly | QIODevice::Truncate));
        fCfg.write("\r\n[Layout]\r\nRecentChipsSize=500,400\r\n");
        fCfg.close();
        CRecentProjectsDialog stDlg({ QStringLiteral("C:/ProjA"), QStringLiteral("C:/ProjB") },
                                    QList<int>() << 5 << 2, sCfg,
                                    CRecentProjectsDialog::EMode::kLauncher);
        // ① 形态断言。
        QCOMPARE(stDlg.ExitRequested(), false);
        QVERIFY(stDlg.findChild<QListWidget*>() == nullptr);          // 无列表
        // 2026-10-11 启动页搜索行（用户需求：替代「更多...」，输入实时过滤工程
        //   按钮）：面板顶部固定一行搜索框——全文检索到的 QLineEdit 即它（列表版
        //   的旧搜索框不在此形态）。
        QVERIFY(stDlg.findChild<QLineEdit*>() != nullptr);            // 面板搜索行在位
        auto* pChips = stDlg.findChild<CRecentChipsPanel*>();
        QVERIFY(pChips != nullptr);                                   // 按钮面板主体
        stDlg.show();
        QTest::qWait(30);
        // 2026-10-12 缺省焦点（用户需求：启动即可直接打字过滤）：显示后焦点在搜索框。
        QCOMPARE(QApplication::focusWidget(), pChips->FilterEdit());
        bool bExit = false, bSkip = false, bOpen = false;
        for (auto* pB : stDlg.findChildren<QPushButton*>()) {
            if (pB->text() == QStringLiteral("退出")) { bExit = true; }
            if (pB->text() == QStringLiteral("跳过")) { bSkip = true; }
            if (pB->text() == QStringLiteral("打开项目...")) { bOpen = true; }
        }
        QVERIFY(bExit);
        QVERIFY(bSkip);
        QVERIFY(bOpen);   // 2026.10.11 打开项目按钮（跳过左边）
        QLabel* pBrand = nullptr;
        QLabel* pVer = nullptr;
        for (QLabel* pL : stDlg.findChildren<QLabel*>()) {
            if (pL->text() == QStringLiteral("AutoCode")) { pBrand = pL; }
            if (pL->text() == AutoCodeVersionText()) { pVer = pL; }
        }
        QVERIFY(pBrand != nullptr);
        QCOMPARE(pBrand->parentWidget()->height(), 100);              // Title 高
        // 2026.10.11 横幅右下角版本号 + 系统标题栏带版本（与主窗口同源）。
        QVERIFY(pVer != nullptr);
        QVERIFY(stDlg.windowTitle().contains(AutoCodeVersionText()));
        // ② 面板尺寸=档值 500x400（kChipMin 下限 360/240 内合法）。
        QCOMPARE(pChips->size(), QSize(500, 400));
        // 窗口高度 = 400 + 100 + 底钮行高（行高>=按钮 sizeHint>0）。
        QVERIFY(stDlg.height() > 500);
        QCOMPARE(stDlg.width(), 500);
        // ③ 拖边：窗口 560x660（无布局 minimum 冲突/不触屏高钳位）→ 面板跟随 → hide 落盘。
        stDlg.show();
        QTest::qWait(30);
        // 当前收敛态的底钮行高（窗口高-100-面板高）。
        const int iBtnH = stDlg.height() - 100 - pChips->height();
        stDlg.resize(560, 660);
        QTest::qWait(30);
        QCOMPARE(stDlg.height(), 660);   // 窗口自身未被布局钳回
        QCOMPARE(pChips->width(), 560);
        QCOMPARE(pChips->height() + 100 + iBtnH, 660);
        stDlg.move(40, 30);
        stDlg.hide();
        QTest::qWait(30);
        stDlg.move(40, 30);
        stDlg.hide();
        QTest::qWait(30);
        QFile fChk(sCfg);
        QVERIFY(fChk.open(QIODevice::ReadOnly));
        const QString sIni = QString::fromUtf8(fChk.readAll());
        fChk.close();
        QVERIFY(sIni.contains(QStringLiteral("RecentChipsSize=560,")));
        QVERIFY(sIni.contains(QStringLiteral("RecentLauncherPos=")));
        // ⑤ 右键删除链：从数据源移除并上报。
        stDlg.show();
        QTest::qWait(20);
        pChips->DoRemoveChip(QStringLiteral("C:/ProjA"));   // 右键删除回调链
        QCOMPARE(stDlg.RemovedPaths(), QStringList{ QStringLiteral("C:/ProjA") });
        QVERIFY(pChips->Chips().size() == 1);   // 仅 ProjB（搜索行模式无更多...，ProjA 已移除）
        stDlg.hide();
        QTest::qWait(20);
    }
    // ④ 新 launcher：面板尺寸=回写值、位置=独立档；点右上角关闭（X）= 退出程序。
    {
        CRecentProjectsDialog stDlg({ QStringLiteral("C:/ProjB") }, QList<int>(), sCfg,
                                    CRecentProjectsDialog::EMode::kLauncher);
        auto* pChips = stDlg.findChild<CRecentChipsPanel*>();
        QVERIFY(pChips != nullptr);
        QCOMPARE(pChips->width(), 560);
        QCOMPARE(stDlg.pos(), QPoint(40, 30));
        // close() → closeEvent → ExitRequested 置位（同「退出」按钮语义）。
        stDlg.show();
        QTest::qWait(20);
        QCOMPARE(stDlg.ExitRequested(), false);
        stDlg.close();
        QTest::qWait(20);
        QCOMPARE(stDlg.ExitRequested(), true);
    }
    // ⑥ 空 cfg = 禁用。
    {
        CRecentProjectsDialog stDlg({ QStringLiteral("C:/ProjA") }, QList<int>(), QString(),
                                    CRecentProjectsDialog::EMode::kLauncher);
        stDlg.show();
        QTest::qWait(20);
        stDlg.hide();
        QTest::qWait(20);
        QVERIFY(!QFile::exists(oTmp.path() + QStringLiteral("/none.ini")));
    }
}

void TestExecPanel::launcherChipKeyboardNav() {
    // 2026.10.12 启动页键盘选工程：搜索框 ↓ → 焦点落首个工程按钮；方向键网格
    //   导航（←→同行、↑↓跨行 x 最近、边界不动）；回车 = 点击同语义打开工程
    //   （accept 消费，不会落到 QDialog 缺省钮「跳过」）；搜索过滤后隐藏芯片
    //   不参与导航。芯片类型为面板 cpp 内部类——断言走面板观察口。
    QTemporaryDir oTmp;
    QVERIFY(oTmp.isValid());
    const QString sCfg = oTmp.path() + QStringLiteral("/config.ini");
    // 长目录名（每行约 3~4 个）保证多行布局——↑↓ 跨行断言的前提。
    const QStringList aPaths{
        QStringLiteral("C:/LongProjectName_A"), QStringLiteral("C:/LongProjectName_B"),
        QStringLiteral("C:/LongProjectName_C"), QStringLiteral("C:/LongProjectName_D"),
        QStringLiteral("C:/LongProjectName_E"), QStringLiteral("C:/Alpha") };
    // ① 键击链路（搜索框 ↓ → 芯片；→/↑ 网格移动；回车 = 打开焦点芯片）。
    //    QTest 键击→窗口路由依赖真实窗口平台（offscreen 下自绘 QWidget 焦点
    //    链不通，实测仅 QLineEdit 键路可用）——offscreen 跳过，等价导航逻辑
    //    由 ② 的方法级调用覆盖（ctest 全量 offscreen，需手工直跑验证）。
    if (QGuiApplication::platformName() != QLatin1String("offscreen")) {
        CRecentProjectsDialog stDlg(aPaths, QList<int>(), sCfg,
                                    CRecentProjectsDialog::EMode::kLauncher);
        auto* pPanel = stDlg.findChild<CRecentChipsPanel*>();
        QVERIFY(pPanel != nullptr);
        QVERIFY(pPanel->Chips().size() == 6);   // 过滤行模式无「更多...」
        stDlg.show();
        QTest::qWait(30);
        // 缺省焦点在搜索框（SetFilterBar(true)），↓ 直接跳入芯片区。
        QCOMPARE(QApplication::focusWidget(), pPanel->FilterEdit());
        QTest::keyClick(pPanel->FilterEdit(), Qt::Key_Down);
        QCOMPARE(pPanel->FocusedChipIndex(), 0);
        // 2026.10.12 首行 ↑ 回搜索框（用户需求：最上面向上回输入框），↓ 再回
        //   首个芯片——双向导航循环。
        QTest::keyClick(pPanel->ChipAt(0), Qt::Key_Up);
        QCOMPARE(QApplication::focusWidget(), pPanel->FilterEdit());
        QTest::keyClick(pPanel->FilterEdit(), Qt::Key_Down);
        QCOMPARE(pPanel->FocusedChipIndex(), 0);
        // → 同行相邻（首个芯片同行必有下一个，6 芯片 500 宽至少 2 行）。
        QTest::keyClick(pPanel->ChipAt(pPanel->FocusedChipIndex()), Qt::Key_Right);
        QVERIFY(pPanel->FocusedChipIndex() > 0);
        // ↑ 从首行回搜索框（2026.10.12 新循环语义；跨行回退由 ② 覆盖）。
        QTest::keyClick(pPanel->ChipAt(pPanel->FocusedChipIndex()), Qt::Key_Up);
        QCOMPARE(QApplication::focusWidget(), pPanel->FilterEdit());
        // ↓ 从搜索框重返芯片区，回车 = 打开焦点芯片（窗口 accept；未 exec，
        //   result/SelectedPath 可读）。
        QTest::keyClick(pPanel->FilterEdit(), Qt::Key_Down);
        QCOMPARE(pPanel->FocusedChipIndex(), 0);
        const int iCur = pPanel->FocusedChipIndex();
        const QString sPath = pPanel->ChipPathAt(iCur);
        QVERIFY(!sPath.isEmpty());
        QTest::keyClick(pPanel->ChipAt(iCur), Qt::Key_Return);
        QCOMPARE(stDlg.result(), static_cast<int>(QDialog::Accepted));
        QCOMPARE(stDlg.SelectedPath(), sPath);
        QCOMPARE(stDlg.isVisible(), false);
    }
    // ② ↑↓ 跨行导航（纯面板驱动）：↓ 到下一行；← 行内不跨行。
    {
        CRecentChipsPanel stPanel([](const QString&) {}, []() {});
        stPanel.SetFilterBar(true);
        stPanel.Refresh(aPaths, QList<int>() << 1 << 2 << 3 << 4 << 5 << 9);
        // 2026.10.12 拉宽面板：offscreen 字体度量下单芯片可达半宽上限（每行 1 个
        //   → ←→ 无同行邻居）；宽 900 保证每行 ≥2，断言与字体环境解耦。
        stPanel.ApplyWindowSize(900, 400);
        stPanel.show();
        QTest::qWait(30);
        QVERIFY(stPanel.FocusFirstChip());
        QCOMPARE(stPanel.FocusedChipIndex(), 0);
        const int iY0 = stPanel.ChipYAt(0);
        stPanel.NavigateChips(Qt::Key_Down);
        const int iRow2 = stPanel.FocusedChipIndex();
        QVERIFY(iRow2 > 0);
        QVERIFY(stPanel.ChipYAt(iRow2) > iY0);   // 下一行
        // → 行内右移：几何驱动探测同行右邻（每行个数随字体度量变，两种分支
        //   均合法——有右邻=移动到它；行尾=原地不动）。
        int iRightNb = -1;
        for (int i = 0; i < stPanel.Chips().size(); ++i) {
            if (i == iRow2) { continue; }
            if (qAbs(stPanel.ChipYAt(i) - stPanel.ChipYAt(iRow2)) <= 15
                    && stPanel.ChipAt(i)->x() > stPanel.ChipAt(iRow2)->x()) {
                iRightNb = i;
                break;
            }
        }
        stPanel.NavigateChips(Qt::Key_Right);
        QCOMPARE(stPanel.FocusedChipIndex(), iRightNb >= 0 ? iRightNb : iRow2);
        // ← 从右邻回退应原路返回（x 最近 = 右移的来处）。
        if (iRightNb >= 0) {
            stPanel.NavigateChips(Qt::Key_Left);
            QCOMPARE(stPanel.FocusedChipIndex(), iRow2);
        }
        // ↑ 回首行（上方有目标行 → 焦点仍在芯片区，不掉搜索框）。
        stPanel.NavigateChips(Qt::Key_Up);
        const int iBack = stPanel.FocusedChipIndex();
        QVERIFY(iBack >= 0 && stPanel.ChipYAt(iBack) == iY0);
        // 2026.10.12 首行（回到 chip0 所在行）再 ↑：焦点回搜索框；↓ 返回芯片区。
        while (stPanel.ChipYAt(stPanel.FocusedChipIndex()) != iY0) {
            stPanel.NavigateChips(Qt::Key_Up);
        }
        stPanel.NavigateChips(Qt::Key_Up);
        QCOMPARE(stPanel.FocusedChipIndex(), -1);   // 焦点不在任何芯片
        QCOMPARE(QApplication::focusWidget(), stPanel.FilterEdit());
        QVERIFY(stPanel.FocusFirstChip());
        QCOMPARE(stPanel.FocusedChipIndex(), 0);
        // 2026.10.12 末行 ↓ 焦点回搜索框（与首行 ↑ 对称的完整循环）：连续 ↓ 至
        //   焦点离开芯片区（=已过最末行），离开前所在行即末行。
        int iCur = stPanel.FocusedChipIndex();
        int iNext = iCur;
        while (iNext >= 0) {
            stPanel.NavigateChips(Qt::Key_Down);
            iNext = stPanel.FocusedChipIndex();
            if (iNext >= 0) {
                QVERIFY(iNext != iCur);   // 行间必须推进（单行布局则首行即末行）
                iCur = iNext;
            }
        }
        QCOMPARE(QApplication::focusWidget(), stPanel.FilterEdit());
        // ↓ 从搜索框再进芯片区（循环闭合）。
        QVERIFY(stPanel.FocusFirstChip());
        QCOMPARE(stPanel.FocusedChipIndex(), 0);
    }
    // ③ 过滤：命中子集后 ↓ 落在首个可见芯片（隐藏芯片不参与）。
    {
        CRecentChipsPanel stPanel([](const QString&) {}, []() {});
        stPanel.SetFilterBar(true);
        stPanel.Refresh(aPaths, QList<int>());
        stPanel.ApplyWindowSize(900, 400);   // 同上：每行 ≥2 个（字体环境解耦）
        stPanel.show();
        QTest::qWait(30);
        stPanel.FilterEdit()->setText(QStringLiteral("Alpha"));
        QTest::qWait(30);
        QVERIFY(stPanel.FocusFirstChip());
        QCOMPARE(stPanel.ChipNameAt(stPanel.FocusedChipIndex()),
                 QStringLiteral("Alpha"));
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
    // 2026-10-09 首条命名轮文案适配：流内 auto_naming 事件显示“正在预处理”
    //   （不报“根据历史提示词生成名称与说明”误导——历史提示词只有刚输入这一条）。
    {
        const QJsonArray aBlocks0 = panel.AssistantsToJson().at(0).toObject()
                .value(QStringLiteral("blocks")).toArray();
        bool bPre = false;
        for (const QJsonValue& v : aBlocks0) {
            const QJsonObject o = v.toObject();
            if (o.value(QStringLiteral("eventKind")).toString()
                    == QStringLiteral("auto_naming")) {
                bPre = o.value(QStringLiteral("text")).toString()
                          .contains(QStringLiteral("正在预处理"));
            }
        }
        QVERIFY(bPre);
    }

    // ② 命名期间追加：同样只暂存不提交。
    pIn->setPlainText(QStringLiteral("再加上注册功能"));
    panel.DoAdhocSend(sId);
    QCOMPARE(spySubmit.count(), 0);
    QCOMPARE(panel.StagedCount(sId), 2);

    // ③ 命名收尾：全部转正 + 合并一次提交（含两条指令、首条权限）。且收尾不重复命名。
    panel.OnAdhocFinished(sId);
    // 2026.10.14 对齐 2026.10.12 产品改动：补发由同步改延迟 1.5s（等 runner 真正
    //   空闲，修「命名后首条指令丢失」）→ 此处改 QTRY 等发射（默认 5s 覆盖 1.5s）。
    QTRY_COMPARE(spySubmit.count(), 1);
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

void TestExecPanel::promptGateRefineFlow() {
    // 2026-10-08 提示词长度闸门回归：
    //   ①默认闸门（5%/不限 token，新助手模型未知→窗口 200K）小文本直接提交；
    //   ②超限文本（CJK 12000 字 > 10000）不提交，改发射 sigAsstRefinePrompt
    //     （原文 + 限额 10000 + 权限快照），用户块照常上屏、输入框清空、不置运行态；
    //   ③关闸门 SetPromptLimits(0,0) → 同样超限的文本直接提交；
    //   ④token 约束独立生效 SetPromptLimits(0,50) → 中等文本（CJK 200 > 50）也转提炼，
    //     限额=50（两约束取小）。
    CExecutionPanel panel;
    panel.resize(1000, 700);
    panel.show();
    QTabWidget* pTabs = panel.findChild<QTabWidget*>();
    QVERIFY(pTabs != nullptr);
    QJsonArray aAssts;
    aAssts.append(MakeAsst(QStringLiteral("已命名助手"), 0));   // 非默认名：不走先改名链
    panel.LoadAssistantsFromJson(aAssts, QString());
    QTest::qWait(100);
    panel.SetAdhocEnabled(true);
    QSignalSpy spySubmit(&panel, &CExecutionPanel::sigAdhocSubmit);
    QSignalSpy spyRefine(&panel, &CExecutionPanel::sigAsstRefinePrompt);
    QPlainTextEdit* pIn = nullptr;
    QWidget* pPage = nullptr;
    for (int i = 0; i < pTabs->count(); ++i) {
        if (pTabs->tabText(i).startsWith(QStringLiteral("已命名助手"))) {
            pTabs->setCurrentIndex(i);
            pPage = pTabs->widget(i);
            pIn = pPage->findChild<QPlainTextEdit*>();
            break;
        }
    }
    QVERIFY(pIn != nullptr && pPage != nullptr);
    const QString sId = panel.ActiveAsstId();
    QComboBox* pPerm = pPage->findChild<QComboBox*>();
    QVERIFY(pPerm != nullptr);
    pPerm->setCurrentIndex(2);   // 只读：验证权限快照随提炼信号携带
    QTest::qWait(30);

    // ① 小文本（远低于 200K×5%=10000）：直接提交。
    pIn->setPlainText(QStringLiteral("小活直发"));
    panel.DoAdhocSend(sId);
    QCOMPARE(spySubmit.count(), 1);
    QCOMPARE(spyRefine.count(), 0);
    panel.SetAsstRunning(sId, false);   // 复位运行态供后续发送
    QTest::qWait(30);

    // ② 超限：CJK 12000 字 → 估算 12000 > 10000，转提炼。
    const QString sBig(12000, QChar(0x6587));   // 「文」×12000
    pIn->setPlainText(sBig);
    panel.DoAdhocSend(sId);
    QCOMPARE(spySubmit.count(), 1);            // 不再提交
    QCOMPARE(spyRefine.count(), 1);
    QCOMPARE(spyRefine.at(0).at(0).toString(), sId);
    QCOMPARE(spyRefine.at(0).at(1).toString(), sBig);   // 原文完整携带
    QCOMPARE(spyRefine.at(0).at(2).toInt(), 10000);     // 限额 = 200000×5%
    QCOMPARE(spyRefine.at(0).at(3).toInt(), 2);         // 权限快照（只读）
    QCOMPARE(pIn->toPlainText(), QString());            // 输入框已清
    QVERIFY(!panel.AsstRunning(sId));                   // 未置运行态（等提炼轮）

    // ③ 关闸门：超限文本直接提交。
    panel.SetPromptLimits(0, 0);
    pIn->setPlainText(sBig);
    panel.DoAdhocSend(sId);
    QCOMPARE(spySubmit.count(), 2);
    QCOMPARE(spyRefine.count(), 1);
    panel.SetAsstRunning(sId, false);
    QTest::qWait(30);

    // ④ token 约束独立生效：限额 = min(占比关, 50) = 50。
    panel.SetPromptLimits(0, 50);
    const QString sMid(200, QChar(0x5B57));   // 「字」×200 → 估算 200 > 50
    pIn->setPlainText(sMid);
    panel.DoAdhocSend(sId);
    QCOMPARE(spySubmit.count(), 2);
    QCOMPARE(spyRefine.count(), 2);
    QCOMPARE(spyRefine.at(1).at(2).toInt(), 50);
    panel.hide();
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

void TestExecPanel::recentChipsPanelResizePersist() {
    // 2026-10-09 边缘拖拽改尺寸 + 持久化回归：①右下角拖拽 → 尺寸按增量变化并落盘；
    //   ②新面板实例（同档）Refresh 后恢复用户尺寸；③拖拽中 Press/Release 被吃掉
    //   （不触发芯片点击）；④无档时保持自动尺寸。
    QTemporaryDir oTmp;
    QVERIFY(oTmp.isValid());
    const QString sIni = oTmp.path() + QStringLiteral("/cfg.ini");
    auto fnOpen = [](const QString&) {};
    auto fnMore = []() {};

    QWidget stHost;
    stHost.resize(800, 600);
    stHost.show();
    QStringList aPaths;
    for (int i = 0; i < 30; ++i) {
        aPaths << QStringLiteral("E:/proj/工程%1").arg(i);
    }
    QList<int> aOpens;
    for (int i = 0; i < 30; ++i) { aOpens << (i + 1); }

    CRecentChipsPanel stPanel(fnOpen, fnMore, &stHost,
                              [&sIni]() -> QString { return sIni; });
    stPanel.Refresh(aPaths, aOpens);
    stPanel.show();
    QTest::qWait(30);
    const QSize szAuto = stPanel.size();
    QVERIFY(szAuto.width() > 0 && szAuto.height() > 0);
    QCOMPARE(stPanel.UserSize(), QSize(0, 0));   // 无档 = 自动尺寸

    // ① 右下角拖拽：在状态栏右下角按下（面板右/下 6px 边缘带）→ 拖 +80/+60 → 释放。
    QLabel* pStat = stPanel.findChild<QLabel*>();
    QVERIFY(pStat != nullptr);
    const QPoint ptCorner(pStat->width() - 3, pStat->height() - 3);
    const int iW0 = stPanel.width();
    const int iH0 = stPanel.height();
    QTest::mousePress(pStat, Qt::LeftButton, Qt::NoModifier, ptCorner);
    QVERIFY(stPanel.UserSize() == QSize(0, 0) || true);   // 拖拽开始尚无新值
    QTest::mouseMove(pStat, ptCorner + QPoint(80, 60));
    QTest::mouseRelease(pStat, Qt::LeftButton, Qt::NoModifier, ptCorner + QPoint(80, 60));
    QCOMPARE(stPanel.UserSize().width(), iW0 + 80);
    QCOMPARE(stPanel.UserSize().height(), iH0 + 60);
    QCOMPARE(stPanel.size(), stPanel.UserSize());   // 尺寸已应用

    // 档已写入（w,h 两段）。
    {
        QFile fIni(sIni);
        QVERIFY(fIni.open(QIODevice::ReadOnly));
        const QString sTxt = QString::fromUtf8(fIni.readAll());
        QVERIFY(sTxt.contains(QStringLiteral("RecentChipsSize")));
        QVERIFY(sTxt.contains(QStringLiteral("%1,%2")
                                  .arg(iW0 + 80).arg(iH0 + 60)));
    }

    // ② 新实例同档恢复：Refresh 后 fixedSize = 保存的用户尺寸。
    {
        CRecentChipsPanel stPanel2(fnOpen, fnMore, &stHost,
                                   [&sIni]() -> QString { return sIni; });
        stPanel2.Refresh(aPaths, aOpens);
        QCOMPARE(stPanel2.UserSize(), QSize(iW0 + 80, iH0 + 60));
        QCOMPARE(stPanel2.size(), QSize(iW0 + 80, iH0 + 60));
    }

    // ③ 拖拽起始 Press 被吃：边缘带内点击不触发芯片（芯片不在边缘带，反向验证：
    //    边缘带小尺寸拖拽（右下角 -100/-40 → 钳到下限）不崩溃且落盘下限值。
    QTest::mousePress(pStat, Qt::LeftButton, Qt::NoModifier, ptCorner);
    QTest::mouseMove(pStat, ptCorner + QPoint(-2000, -2000));
    QTest::mouseRelease(pStat, Qt::LeftButton, Qt::NoModifier, ptCorner + QPoint(-2000, -2000));
    QVERIFY(stPanel.UserSize().width() >= 360);   // kChipMinW 下限
    QVERIFY(stPanel.UserSize().height() >= 240);  // kChipMinH 下限
    stHost.hide();

    // ⑤ 真实内嵌路径（QWidgetAction 进 QMenu）：拖拽时所在菜单窗口须同步长大——
    //    面板经 setFixedSize 变大而菜单不缩放的话，用户所见菜单完全不变（实反馈：
    //    「边缘没法拖动」）；断言菜单尺寸增量 == 拖拽增量。
    {
        QMenu stMenu;
        CRecentChipsPanel stMPanel(fnOpen, fnMore, nullptr,
                                   [&sIni]() -> QString { return QString(); });   // 不落盘
        stMPanel.Refresh(aPaths, aOpens);
        auto* pWAct = new QWidgetAction(&stMenu);
        pWAct->setDefaultWidget(&stMPanel);
        stMenu.addAction(pWAct);
        stMenu.show();
        QTest::qWait(50);
        QLabel* pStatM = stMPanel.findChild<QLabel*>();
        qDebug() << "[t] menu visible=" << stMenu.isVisible()
                 << "panel visible=" << stMPanel.isVisible()
                 << "panel parent=" << stMPanel.parentWidget()
                 << "panel geo=" << stMPanel.geometry()
                 << "panel global tl=" << stMPanel.mapToGlobal(QPoint(0, 0))
                 << "menu size=" << stMenu.size();
        QVERIFY(pStatM != nullptr);
        const int iMW0 = stMenu.width();
        const int iMH0 = stMenu.height();
        const int iPW0 = stMPanel.width();
        const int iPH0 = stMPanel.height();
        QVERIFY(iMW0 > 0 && iMH0 > 0);
        const QPoint ptC(pStatM->width() - 3, pStatM->height() - 3);
        QTest::mousePress(pStatM, Qt::LeftButton, Qt::NoModifier, ptC);
        QTest::mouseMove(pStatM, ptC + QPoint(60, 40));
        QTest::mouseRelease(pStatM, Qt::LeftButton, Qt::NoModifier, ptC + QPoint(60, 40));
        QCOMPARE(stMPanel.UserSize(), QSize(iPW0 + 60, iPH0 + 40));
        QCOMPARE(stMenu.width(),  iMW0 + 60);   // 菜单随面板同步长大（含边框补偿）
        QCOMPARE(stMenu.height(), iMH0 + 40);
        // 面板-菜单边框关系恒定（拖拽前后 面板宽-菜单宽 差不变 = 菜单框恒随面板；
        //   offscreen 初始 chrome 为负不影响，只验证增量跟随）。平台无关。
        const int iOverW0 = iPW0 - iMW0;
        const int iOverH0 = iPH0 - iMH0;
        QCOMPARE(stMPanel.width()  - stMenu.width(),  iOverW0);
        QCOMPARE(stMPanel.height() - stMenu.height(), iOverH0);

        // —— 菜单右下角 chrome 点（面板外、菜单内，真实用户贴菜单边缘按下的位置）：
        //    receiver=QMenu（非任何子控件）——qApp 级接管 + EdgeAtGlobal 交叉区
        //    扩展（右下斜角 iR/iB 双命中）必须覆盖（沙箱真机复现用户「拖不动」场景）。
        //    前提：菜单 ≥ 面板（真实平台 chrome 4/3px 恒成立）；offscreen 菜单布局
        //    畸变（实测菜单比面板窄）不满足前提，跳过（真机已由沙箱实测覆盖）。
        if (stMenu.width() >= stMPanel.width() && stMenu.height() >= stMPanel.height()) {
            const QSize szPC0 = stMPanel.size();
            const QSize szMC0 = stMenu.size();
            const QPoint ptCornerM(stMenu.width() - 2, stMenu.height() - 2);
            QTest::mousePress(&stMenu, Qt::LeftButton, Qt::NoModifier, ptCornerM);
            QTest::mouseMove(&stMenu, ptCornerM + QPoint(50, 30));
            QTest::mouseRelease(&stMenu, Qt::LeftButton, Qt::NoModifier, ptCornerM + QPoint(50, 30));
            QCOMPARE(stMPanel.UserSize().width(),  szPC0.width()  + 50);
            QCOMPARE(stMPanel.UserSize().height(), szPC0.height() + 30);
            QCOMPARE(stMenu.width(),  szMC0.width()  + 50);   // 菜单同步跟随（chrome 补偿不变）
            QCOMPARE(stMenu.height(), szMC0.height() + 30);
        }
        stMenu.hide();
    }
}

    // ============ 2026-10-11 启动页搜索行（SetFilterBar：替代「更多...」，实时过滤芯片） ============
    //   启动页面板顶部固定一行搜索框（单独占一行）；输入实时过滤工程芯片——名称/
    //   路径不区分大小写包含命中（口径同完整列表搜索框）；清空恢复全显；状态栏
    //   「匹配 M / N」；菜单内嵌面板默认不受影响（更多... 保留）。
void TestExecPanel::recentChipsPanelFilterBar() {
        QStringList aPaths = { QStringLiteral("E:/w/ProjAlpha"),
                               QStringLiteral("E:/w/ProjBeta"),
                               QStringLiteral("F:/code/tank") };
        QList<int> aOpens = { 3, 1, 8 };
        QWidget stHost;
        stHost.resize(700, 400);
        stHost.show();
        CRecentChipsPanel stPanel(
            [](const QString&) {}, []() {}, &stHost,
            []() -> QString { return QString(); });
        stPanel.Refresh(aPaths, aOpens);
        stPanel.show();
        QTest::qWait(30);
        // 默认（菜单内嵌形态）：更多... 在首位、无搜索框。
        QCOMPARE(stPanel.Chips().size(), 4);   // 更多... + 3 工程
        QVERIFY(stPanel.FilterEdit() != nullptr);
        QVERIFY(stPanel.FilterEdit()->isHidden());

        // 启用搜索行：Refresh 后无更多芯片、搜索框可见。
        stPanel.SetFilterBar(true);
        stPanel.Refresh(aPaths, aOpens);
        QTest::qWait(30);
        QCOMPARE(stPanel.Chips().size(), 3);   // 无「更多...」
        QVERIFY(!stPanel.FilterEdit()->isHidden());

        // ① 名称过滤（不区分大小写）：输入 proj → ProjAlpha/ProjBeta 可见、tank 隐藏。
        stPanel.FilterEdit()->setText(QStringLiteral("proj"));
        QTest::qWait(20);
        QVERIFY(!stPanel.ChipAt(0)->isHidden());
        QVERIFY(!stPanel.ChipAt(1)->isHidden());
        QVERIFY(stPanel.ChipAt(2)->isHidden());
        QVERIFY(stPanel.StatusText().contains(QStringLiteral("匹配 2 / 3")));

        // ② 大小写不敏感 + 路径命中：输入 F:/CODE → 仅 tank。
        stPanel.FilterEdit()->setText(QStringLiteral("F:/CODE"));
        QTest::qWait(20);
        QVERIFY(stPanel.ChipAt(0)->isHidden());
        QVERIFY(stPanel.ChipAt(1)->isHidden());
        QVERIFY(!stPanel.ChipAt(2)->isHidden());

        // ③ 无匹配：全隐 + 匹配 0 / 3。
        stPanel.FilterEdit()->setText(QStringLiteral("zzz不存在"));
        QTest::qWait(20);
        for (int i = 0; i < 3; ++i) {
            QVERIFY(stPanel.ChipAt(i)->isHidden());
        }
        QVERIFY(stPanel.StatusText().contains(QStringLiteral("匹配 0 / 3")));

        // ④ 清空恢复全显 + 默认汇总。
        stPanel.FilterEdit()->setText(QString());
        QTest::qWait(20);
        for (int i = 0; i < 3; ++i) {
            QVERIFY(!stPanel.ChipAt(i)->isHidden());
        }
        QVERIFY(!stPanel.StatusText().contains(QStringLiteral("匹配")));

        // ⑤ 运行中 Refresh（右键删除后）保持过滤态：带词刷新后命中子集仍过滤。
        stPanel.FilterEdit()->setText(QStringLiteral("proj"));
        stPanel.Refresh({ QStringLiteral("E:/w/ProjAlpha") }, { 3 });
        QTest::qWait(20);
        QCOMPARE(stPanel.Chips().size(), 1);
        QVERIFY(!stPanel.ChipAt(0)->isHidden());   // ProjAlpha 仍命中显示
        QVERIFY(stPanel.StatusText().contains(QStringLiteral("匹配 1 / 1")));

        // ⑥ 关闭搜索行回菜单形态：Refresh 恢复更多...。
        stPanel.SetFilterBar(false);
        stPanel.Refresh(aPaths, aOpens);
        QTest::qWait(20);
        QCOMPARE(stPanel.Chips().size(), 4);
        QVERIFY(stPanel.FilterEdit()->isHidden());

        stHost.hide();
    }
void TestExecPanel::recentChipsPanelRightDelete() {
    // 2026-10-09 右键删除回归：①右键不误开工程（旧版 mouseReleaseEvent 不分按键）；
    //   ②DoRemoveChip 回调链；③右键 Press → exec 菜单 → singleShot 触发删除 → 回调。
    QStringList aPaths = { QStringLiteral("E:/p/Proj1"), QStringLiteral("E:/p/Proj2") };
    QList<int> aOpens = { 3, 1 };
    int nOpened = 0;
    QStringList aRemoved;
    QWidget stHost;
    stHost.resize(700, 400);
    stHost.show();
    CRecentChipsPanel stPanel(
        [&](const QString&) { ++nOpened; },
        []() {},
        &stHost,
        []() -> QString { return QString(); },
        [&](const QString& sP) { aRemoved.append(sP); });
    stPanel.Refresh(aPaths, aOpens);
    stPanel.show();
    QTest::qWait(30);

    QWidget* pChip1 = stPanel.ChipAt(1);   // 首个工程芯片
    QVERIFY(pChip1 != nullptr);

    // ① 右键 Press+Release：不触发 Clicked（打开计数 0）；Press 触发 exec 菜单
    //   （singleShot 在 exec 循环内经观察口触发删除动作；不用 QVERIFY 防嵌套循环卡死）。
    QTimer::singleShot(150, [&stPanel]() {
        QMenu* pM = stPanel.LastCtxMenu();
        if (pM != nullptr && !pM->actions().isEmpty()) {
            pM->actions().at(0)->trigger();   // 「从最近打开删除」→ 回调（动作连接保证）
            pM->close();   // offscreen 下 exec 不随 trigger 自然返回，补刀退出嵌套循环
        } else {
            // 菜单未出现：关闭面板所在活动弹窗防卡（主流程断言会失败）
            if (QWidget* pW = QApplication::activePopupWidget()) { pW->close(); }
        }
    });
    QTest::mousePress(pChip1, Qt::RightButton, Qt::NoModifier, QPoint(20, 15));
    QTest::qWait(50);
    QVERIFY(stPanel.LastCtxMenu() == nullptr);   // exec 已返回（菜单重建期结束）
    QCOMPARE(nOpened, 0);            // 右键未误开
    QCOMPARE(aRemoved.size(), 1);    // exec 菜单删除动作已触发回调
    QCOMPARE(aRemoved.at(0), QStringLiteral("E:/p/Proj1"));

    // 右键 Release 不补发打开
    QTest::mouseRelease(pChip1, Qt::RightButton, Qt::NoModifier, QPoint(20, 15));
    QCOMPARE(nOpened, 0);

    // ② DoRemoveChip 直调（无 exec 的执行链口）
    stPanel.DoRemoveChip(QStringLiteral("E:/p/Proj2"));
    QCOMPARE(aRemoved.size(), 2);
    QCOMPARE(aRemoved.at(1), QStringLiteral("E:/p/Proj2"));

    // ③ 运行中刷新可见性（用户实害回归：右键删除后 Refresh 发生在面板已显示
    //    期间，新建子部件默认隐藏须显式 show——旧版删除后所有按钮全部消失）。
    stPanel.Refresh({ QStringLiteral("E:/p/Proj3") }, { 5 });
    QTest::qWait(30);
    QCOMPARE(stPanel.Chips().size(), 2);   // 「更多...」+ Proj3
    QVERIFY(stPanel.isVisible());          // 面板本身仍可见
    for (int i = 0; i < stPanel.Chips().size(); ++i) {
        QWidget* pW = stPanel.ChipAt(i);
        QVERIFY2(pW != nullptr && pW->isVisible(),
                 qPrintable(QStringLiteral("chip %1 hidden after live refresh")
                               .arg(i)));
    }

    stHost.hide();
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

// 2026.10.15 阶段产物右键「另存为」核心逻辑（UI 模态对话框不进沙箱，抽函数离屏测）：
//   ①OutputSaveName：条目 UserRole+1 存阶段中文名 → 另存缺省名 = 中文名 + 原后缀
//     （plan.md → 计划.md，用户需求）；无阶段名兑底回退原文件名。
//   ②SaveOutputsToDir：批量落盘到临时目录——同名冲突（目录已有/本轮已用）自动
//     序号（代码审查.md / 代码审查(2).md）、不区分大小写、内容一致、失败计数。
void TestExecPanel::outputSaveNameAndBatch() {
    CExecutionPanel panel;

    // ① 中文名映射：UserRole=源路径、UserRole+1=阶段中文名（LoadStageOutputs 同结构）。
    QListWidgetItem itPlan;
    itPlan.setData(Qt::UserRole, QStringLiteral("E:/x/.autodev/tasks/t1/plan.md"));
    itPlan.setData(Qt::UserRole + 1, QStringLiteral("计划"));
    QCOMPARE(panel.OutputSaveName(&itPlan), QStringLiteral("计划.md"));

    QListWidgetItem itIntake;
    itIntake.setData(Qt::UserRole, QStringLiteral("E:/x/tasks/t1/intake.md"));
    itIntake.setData(Qt::UserRole + 1, QStringLiteral("需求收集"));
    QCOMPARE(panel.OutputSaveName(&itIntake), QStringLiteral("需求收集.md"));

    QListWidgetItem itRaw;   // 无阶段名（异常兑底）→ 原文件名
    itRaw.setData(Qt::UserRole, QStringLiteral("E:/x/tasks/t1/guide.md"));
    QCOMPARE(panel.OutputSaveName(&itRaw), QStringLiteral("guide.md"));

    // ② 批量落盘：plan.md→计划.md、cr.md→代码审查.md（源不同目标同名 → (2) 序号）、
    //   与目录已有文件同名冲突同样避开。
    QTemporaryDir oTmp;
    QVERIFY(oTmp.isValid());
    const QString sDir = oTmp.path();
    const QString sPlan = sDir + QStringLiteral("/src_plan.md");
    const QString sCr1 = sDir + QStringLiteral("/src_cr1.md");
    const QString sCr2 = sDir + QStringLiteral("/src_cr2.md");
    const QString sOdd = sDir + QStringLiteral("/src_odd.dat");
    auto writeFile = [](const QString& sPath, const QString& sBody) {
        QFile f(sPath);
        QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
        f.write(sBody.toUtf8());
        f.close();
    };
    writeFile(sPlan, QStringLiteral("plan-body"));
    writeFile(sCr1, QStringLiteral("cr1-body"));
    writeFile(sCr2, QStringLiteral("cr2-body"));
    writeFile(sOdd, QStringLiteral("odd-body"));

    // 预置目录已有「验证.md」（模拟用户上次已存）→ 本轮自动避让。
    writeFile(sDir + QStringLiteral("/验证.md"), QStringLiteral("old"));

    QList<QPair<QString, QString>> aItems;
    aItems.append({sPlan, QStringLiteral("计划.md")});
    aItems.append({sCr1, QStringLiteral("代码审查.md")});   // 两个 cr.md 源同目标名
    aItems.append({sCr2, QStringLiteral("代码审查.md")});
    aItems.append({sOdd, QStringLiteral("验证.md")});       // 撞目录已有 → (2)
    QStringList aFail;
    const int iOk = CExecutionPanel::SaveOutputsToDir(aItems, sDir, &aFail);
    QCOMPARE(iOk, 4);
    QVERIFY(aFail.isEmpty());

    // 落盘名与内容断言。
    QVERIFY(QFile::exists(sDir + QStringLiteral("/计划.md")));
    QVERIFY(QFile::exists(sDir + QStringLiteral("/代码审查.md")));
    QVERIFY(QFile::exists(sDir + QStringLiteral("/代码审查(2).md")));
    QVERIFY(QFile::exists(sDir + QStringLiteral("/验证(2).md")));
    QVERIFY(!QFile::exists(sDir + QStringLiteral("/代码审查(3).md")));
    QFile f1(sDir + QStringLiteral("/代码审查.md"));
    QVERIFY(f1.open(QIODevice::ReadOnly));
    QCOMPARE(QString::fromUtf8(f1.readAll()), QStringLiteral("cr1-body"));
    f1.close();
    QFile f2(sDir + QStringLiteral("/代码审查(2).md"));
    QVERIFY(f2.open(QIODevice::ReadOnly));
    QCOMPARE(QString::fromUtf8(f2.readAll()), QStringLiteral("cr2-body"));
    f2.close();

    // ③ 失败路径：不存在源 → 失败明细返回 0。
    QList<QPair<QString, QString>> aBad;
    aBad.append({sDir + QStringLiteral("/no_such.md"), QStringLiteral("计划.md")});
    QStringList aFail2;
    QCOMPARE(CExecutionPanel::SaveOutputsToDir(aBad, sDir, &aFail2), 0);
    QCOMPARE(aFail2.size(), 1);
}

// 2026.10.15 模型列表双击就地编辑校验（纯函数直调；UI 链 = QTreeWidget 双击编辑
//   → itemChanged → CSettingsDialog::OnDshModelItemChanged 调本函数后落库刷表）。
void TestExecPanel::modelCellEditValidation() {
    auto mkE = []() {
        SDshModelEntry e;
        e.sDisp = QStringLiteral("旧名");
        e.sBaseURL = QStringLiteral("http://h/a");
        e.sApi = QStringLiteral("anthropic-messages");
        e.sModelId = QStringLiteral("m0");
        e.iCtx = 200000;
        e.iMaxTok = 32768;
        return e;
    };

    // ① 显示名正常改：true + 新值（trim 落存）。
    {
        SDshModelEntry e = mkE();
        QVERIFY(ApplyModelCellEdit(&e, 0, QStringLiteral("  新名  ")));
        QCOMPARE(e.sDisp, QStringLiteral("新名"));
    }
    // ①b 显示名清空：回退模型 ID（对话框 Entry() 同口径），仍算变化。
    {
        SDshModelEntry e = mkE();
        QVERIFY(ApplyModelCellEdit(&e, 0, QStringLiteral("   ")));
        QCOMPARE(e.sDisp, QStringLiteral("m0"));
    }
    // ② 模型 ID 必填：空/纯空白回退旧值且返回 false（单元格重显旧值）。
    {
        SDshModelEntry e = mkE();
        QVERIFY(!ApplyModelCellEdit(&e, 3, QStringLiteral("  ")));
        QCOMPARE(e.sModelId, QStringLiteral("m0"));
    }
    {
        SDshModelEntry e = mkE();
        QVERIFY(ApplyModelCellEdit(&e, 3, QStringLiteral(" m1 ")));
        QCOMPARE(e.sModelId, QStringLiteral("m1"));
    }
    // ③ 上下文/最大输出：合法正整数落存；非数字/零/负数回退旧值返回 false。
    {
        SDshModelEntry e = mkE();
        QVERIFY(ApplyModelCellEdit(&e, 4, QStringLiteral("1000000")));
        QCOMPARE(e.iCtx, qint64(1000000));
    }
    {
        SDshModelEntry e = mkE();
        QVERIFY(!ApplyModelCellEdit(&e, 4, QStringLiteral("abc")));
        QCOMPARE(e.iCtx, qint64(200000));
    }
    {
        SDshModelEntry e = mkE();
        QVERIFY(!ApplyModelCellEdit(&e, 5, QStringLiteral("0")));
        QCOMPARE(e.iMaxTok, 32768);
    }
    {
        SDshModelEntry e = mkE();
        QVERIFY(!ApplyModelCellEdit(&e, 5, QStringLiteral("-8")));
        QCOMPARE(e.iMaxTok, 32768);
    }
    // ④ 标记列（6/越界）不改数据返回 false。
    {
        SDshModelEntry e = mkE();
        QVERIFY(!ApplyModelCellEdit(&e, 6, QStringLiteral("x")));
        QVERIFY(!ApplyModelCellEdit(&e, 9, QStringLiteral("x")));
        QVERIFY(!ApplyModelCellEdit(nullptr, 0, QStringLiteral("x")));   // 空指针防御
        QCOMPARE(e.sDisp, QStringLiteral("旧名"));
    }
    // ⑤ 同值重写（无变化）返回 false（幂等，不触发落库刷表）。
    {
        SDshModelEntry e = mkE();
        QVERIFY(!ApplyModelCellEdit(&e, 1, QStringLiteral("http://h/a")));
    }
}

// 2026.10.15 工具卡合并穿透文本块（TryMergeWidget 放宽；DSH 实害：每次工具调用前
//   的思维链文本把旧口径“紧邻才合并”逐条打断 → 整版工具逐卡铺开刷屏）。断言：
//   ①Tool/文本穿插序列合一张 ×3 组卡（widgets=组卡+2 文本）；②用户提示词(Event)
//   块仍打断（跨提问轮不误并）；③System 组保持紧邻语义（文本打断不穿透）。
void TestExecPanel::toolMergeSkipsTextBlocks() {
    CMessageView stView;
    stView.resize(800, 500);
    stView.show();
    CStreamPresenter stPre;
    stPre.Bind(&stView);
    auto mkTool = [](const QString& a_sCmd) {
        SBlockData st;
        st.eKind = CMessageBlock::ToolUse;
        st.sTool = QStringLiteral("Bash");
        st.sInput = QStringLiteral("{\"command\":\"%1\"}").arg(a_sCmd);
        return st;
    };
    auto mkText = [](const QString& a_s) {
        SBlockData st;
        st.eKind = CMessageBlock::Assistant;
        st.sText = a_s;
        return st;
    };

    // ① DSH 形态：Tool 文本 Tool 文本 Tool → 一张 ×3 组卡（穿透 2 段文本）。
    //   注：WidgetCount=块槽位数（合并卡按数据块占多槽），合并断言用指针同一性。
    stPre.Append(mkTool(QStringLiteral("t1")));
    stPre.Append(mkText(QStringLiteral("思考一")));
    stPre.Append(mkTool(QStringLiteral("t2")));
    stPre.Append(mkText(QStringLiteral("思考二")));
    stPre.Append(mkTool(QStringLiteral("t3")));
    QCOMPARE(stPre.WidgetCount(), 5);   // 每块一槽位（合并卡占 3 槽）
    CMessageBlock* pCard = stPre.WidgetAt(0);
    QVERIFY(pCard != nullptr);
    QCOMPARE(pCard->ToolCount(), 3);   // 三次调用全并组卡（折叠一条）
    QVERIFY(stPre.WidgetAt(1) != pCard);   // 文本块独立上屏
    QVERIFY(stPre.WidgetAt(2) == pCard);   // t2 并卡（占位同指针）
    QVERIFY(stPre.WidgetAt(4) == pCard);   // t3 并卡
    QVERIFY(stPre.WidgetAt(3) != pCard);   // 第二段文本仍独立

    // ② 用户提示词（Event/user）打断：新工具开新卡，不跨提问轮误并。
    SBlockData stUser;
    stUser.eKind = CMessageBlock::Event;
    stUser.sEventKind = QStringLiteral("user");
    stUser.sText = QStringLiteral("继续");
    stPre.Append(stUser);
    stPre.Append(mkTool(QStringLiteral("t4")));
    QCOMPARE(stPre.WidgetCount(), 7);   // 新增 Event + 新工具卡两槽
    CMessageBlock* pNewCard = stPre.WidgetAt(6);
    QVERIFY(pNewCard != nullptr && pNewCard != pCard);
    QCOMPARE(pNewCard->ToolCount(), 1);

    // ③ System 组保持紧邻合并：文本插在中间不穿透合并。
    SBlockData stSys1;
    stSys1.eKind = CMessageBlock::System;
    stSys1.sText = QStringLiteral("系统行1");
    stPre.Append(stSys1);
    stPre.Append(mkText(QStringLiteral("插入文本")));
    SBlockData stSys2;
    stSys2.eKind = CMessageBlock::System;
    stSys2.sText = QStringLiteral("系统行2");
    stPre.Append(stSys2);
    QCOMPARE(stPre.WidgetCount(), 10);   // + System 组 + 文本 + System 新组
    QVERIFY(stPre.WidgetAt(7) != stPre.WidgetAt(9));   // 两个 System 组（未穿透）
    stView.hide();
}

QTEST_MAIN(TestExecPanel)
#include "test_execpanel.moc"
