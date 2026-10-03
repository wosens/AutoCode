#pragma once

#include <QWidget>
#include <QHash>
#include <QList>
#include <QSet>
#include <QJsonArray>
#include <QJsonObject>
#include <QColor>
#include <QElapsedTimer>

#include "ui/CMessageBlock.h"   // 块控件（SBlockData 现居 CStreamPresenter.h）
#include "ui/CStreamPresenter.h"   // 2026.09.03 对话流呈现器（任务/助手日志共用；SBlockData/BlockToJson 亦在此）
#include "ui/CStageBar.h"          // 2026.09.04 阶段横向图标条

class QLabel;
class QTabWidget;
class QSplitter;
class QListWidget;
class QListWidgetItem;
class QPlainTextEdit;
class QPushButton;
class QProgressBar;
class QToolButton;
class QComboBox;   // 2026-09-11 权限下拉（自动/手工/只读）
class QTimer;
class CBusyIndicator;
class CAsstTabBar;   // 2026-09-22 多行 Tab 栏（自 HarnessAgent/CAgentTabBar 迁移）
class CAsstTabWidget;   // 同上：宿主 QTabWidget（解禁 setTabBar，构造期换装多行栏）
class CArtifactView;
class CTaskInfo;
class CMessageView;
class CFilePreviewWidget;   // 2026.08.25 文件预览 Tab（md/html/hex 多视图容器）
class CPromptFlyout;   // FR-3：助手日志提示词下拉浮层（半透明按钮 hover 弹出）

// 右侧执行面板：上方阶段进度、中间 Tab（日志对话流 / 阶段产物 .md）、底部 cost/token 统计。
// 对话流由 CMessageView 承放 CMessageBlock；多任务按 id 缓存结构化 block 数据，切换时重建。
// 「阶段产物」页按选中任务的流程阶段，列出 .autodev/tasks/<id>/ 下已生成的 .md 产物供查看。
class CExecutionPanel : public QWidget {
    Q_OBJECT

public:
    explicit CExecutionPanel(QWidget* a_pParent = nullptr);
    ~CExecutionPanel() override;   // 2026.09.03 释放全部任务流（CStreamPresenter 堆对象；助手流随 SAsst/shared_ptr 自清）

    // —— 结构化对话流（直接接编排器信号）——
    // 末尾 a_sViewId 为评审多视角并行的视角标识（空=单 runner 串行路径，向后兼容）。
    void OnAssistantText(CTaskInfo* a_pTask, const QString& a_sText, const QString& a_sViewId = QString());
    void OnToolUse(CTaskInfo* a_pTask, const QString& a_sTool, const QString& a_sInputJson, const QString& a_sViewId = QString());
    void OnToolResult(CTaskInfo* a_pTask, const QString& a_sResult, const QString& a_sViewId = QString());

    // —— 经 MainWindow 转发的通用入口 ——
    void AppendLog(const QString& a_sTaskId, const QString& a_sLine);             // 系统/原始行
    void AppendEvent(const QString& a_sTaskId, const QString& a_sKind,
                     const QString& a_sText);                                      // 阶段生命周期事件

    void SetCurrentTask(CTaskInfo* a_pTask);   // 切换显示任务（重建对话流 + 刷新阶段表）
    // 2026.09.09 有效选中观察口：面板 m_pCurTask 始终跟随用户可见选中（含取消选中
    //   当轮即时置空）。与 CMainWindow::m_pCur（助手档 key，取消选中时若助手在跑会
    //   保留旧任务到本轮结束，见 SwitchAssistantArchive 早退）不同源——助手提示词的
    //   任务信息注入（memory.md/经验规则）须以本值为准：没选任务就绝不带任务信息。
    CTaskInfo* CurrentTask() const { return m_pCurTask; }
    // 2026.09.09 彻底删除助手核心层（无弹窗；观察口：DeleteAsst UI 确认后调它，
    //   测试直调本层验证删除效果）。与 CloseAsst（软关闭可恢复）对应。
    void DeleteAsstCore(const QString& a_sId);
    void ShowTaskLogPage();
    // 2026.08.29 上下文进度条菜单（右键/左键共用）：立即压缩 + 设置上下文大小。
    //   2026.09.05 per-助手控件：目标助手 id 显式传入（该助手自己的进度条/会话）。
    void ShowContextMenu(const QString& a_sAsstId, const QPoint& a_stGlobalPos);
    // 2026.08.29 阶段产物页分割条持久化（[Layout] ArtifactsSplit，base64 saveState）。
    QByteArray ArtifactsSplitState() const;
    void RestoreArtifactsSplit(const QByteArray& a_stState);
    void ShowActiveAssistantPage();           // 2026.08.26 切到当前激活助手 Tab（新建任务后立即看到助手日志）
    void RefreshTask(CTaskInfo* a_pTask);       // 状态变化时刷新阶段表/cost
    // FR-5：阶段产物落盘后由主窗口在 stage_done 事件调用——重扫产物目录并保留当前选中态
    //（重扫前记录选中产物绝对路径，重扫后若该文件仍存在则重新选中同一行，避免查看内容被清空）。
    void RefreshStageOutputs(CTaskInfo* a_pTask);

    // —— 提示词 agent（「助手」Tab，独立于任务的对话流）——
    // 2026.09.03 多助手并发：槽首参 = 产出助手 id，各流直投各的助手 Tab；
    //   运行态 per-助手（SAsst::bRunning），跨助手发送立即执行不再排队。
    void OnAdhocAssistantText(const QString& a_sAsstId, const QString& a_sText);
    void OnAdhocToolUse(const QString& a_sAsstId, const QString& a_sTool, const QString& a_sInputJson);
    void OnAdhocToolResult(const QString& a_sAsstId, const QString& a_sResult);
    void OnAdhocLog(const QString& a_sAsstId, const QString& a_sLine);
    void OnAdhocEvent(const QString& a_sAsstId, const QString& a_sKind, const QString& a_sText);
    // 2026.09.18 暂存提示词生命周期（编排器 sigAdhocPendingStarted/Dropped 连接；主窗口接线）。
    void OnAdhocPendingStarted(const QString& a_sAsstId, const QString& a_sText);
    void OnAdhocPendingDropped(const QString& a_sAsstId);
    void OnAdhocUndo(const QString& a_sAsstId);   // 撤销：移除该助手最后一条 prompt 块 + 其回复/工具块并重建
    void OnAdhocFinished(const QString& a_sAsstId);   // 该助手一次调用结束：恢复其输入态 + 默认名自动改名（2026.09.26）
    // 2026-09-22 助手自动重命名回填（编排器 sigAsstAutoNameDone → 主窗口转这里；同步失败
    //   路径也走它留事件行）：成功改 sName/sDesc + 刷 Tab 标题/tooltip + 落盘，失败记事件行。
    void ApplyAsstAutoName(const QString& a_sAsstId, bool a_bOk,
                           const QString& a_sName, const QString& a_sDesc);
    // 2026.09.26 工作结束自动改名：名称仍是原始名（助手N）时收尾自动触发自动重命名
    //   （复用 sigAsstAutoRename 链路）。每轮用户提问最多自动尝试一次（防命名失败
    //   后收尾信号重入死循环），用户再发提示词时清零重试。
    void MaybeAutoNameOnFinish(const QString& a_sAsstId);
    // 2026-10-03 用户请求「自动重命名」统一入口（右键菜单/挂起收尾共用）：收集素材后
    //   发射 sigAsstAutoRename；助手运行中 → 置 bAutoNameQueued 挂起并在流内留事件行，
    //   本轮收尾（OnAdhocFinished）再执行（用户需求：运行期间请求的自动命名放到
    //   当前指令完成后进行）。返回 false = 无素材/无该助手（调用方提示）。
    bool TriggerAsstAutoRename(const QString& a_sAsstId);
    // 2026.08.19 运行态对外置位：编排器自动续发排队指令起新轮时广播（见 sigAdhocRunning），
    //   手动发送路径面板已自置、此处幂等。原 private，升 public 供主窗口连接。
    //   2026.09.03 多助手并发：置位/清除均按助手 id，互不影响其他助手的运行态。
    void SetAsstRunning(const QString& a_sAsstId, bool a_bRunning);
    bool AsstRunning(const QString& a_sAsstId) const;      // 指定助手是否运行中
    bool AnyAsstRunning() const;                            // 是否有任一助手运行中（旧 m_bAdhocRunning 语义）
    QStringList RunningAsstIds() const;                     // 2026-10-02 运行中助手 id 表（孤儿 busy 看门狗用）

    // —— 2026-10-02 跨档幽灵（任务助手不停跑跨档保留）——
    // 视图 key（当前展示的存档：任务 id 或空=工程全局档）：与本档不一致的助手为外档
    //   幽灵（运行中才存在）。切档前由宿主调 SetViewKey；SaveAssistantState 按归属分组落盘。
    void SetViewKey(const QString& a_sKey) { m_sViewKey = a_sKey; }
    QString ViewKey() const { return m_sViewKey; }
    // 幽灵结束除名（宿主落盘后调）：拆 Tab+除列表项。
    void RemoveAsstEntry(const QString& a_sAsstId);
    // 外档归属查询（tooltip/切回路由用）：非外档返回 false。
    bool AsstForeignById(const QString& a_sAsstId, QString* a_psOwnerKey = nullptr) const;
    // 2026-10-02 全量快照（含外档幽灵；分组落盘用——遍历期间面板可变更，取 COW 副本）。
    // 按归属 key 过滤的序列化（分组落盘用；不嵌 owner 字段——档文件即归属）。
    QJsonArray AssistantsJsonOf(const QString& a_sOwnerKey) const;
    void SetAdhocContext(const QString& a_sAsstId, qint64 a_iFill);   // 更新该助手上下文快照+进度条（<0 视为未知）
    void OnAdhocModel(const QString& a_sAsstId, const QString& a_sModel);   // 该助手收到模型名 → 查表更新窗口分母
    void SetUserContextLimit(qint64 a_iLimit);    // 右键设窗口上限（>0 覆盖探测；0/负回退自动）
    void SetAdhocEnabled(bool a_bEnabled);  // 工程未打开时禁用输入栏
    // FR-3：资源管理器「添加到聊天框」→ 把文本追加到 adhoc 助手输入框末尾。
    void AppendToAdhocInput(const QString& a_sText);
    // 2026.08.18 资源管理器双击文本文件 → 「助手」右侧单实例 Tab 内嵌显示文件内容
    //   （QPlainTextEdit 只读；双击其他文件复用同一 Tab 换内容与标题）。
    //   2026.08.21 增加 a_bSwitchTab：false=资源管理器单击预览——只加载内容/换
    //   标题不切页（用户停留当前 Tab）；默认 true 保持双击切页原行为。
    //   2026-09-30 增加 a_iLine：>0 时加载后定位到该 1-based 行（代码审查发现定位）。
    void ShowFileTab(const QString& a_sAbsPath, bool a_bSwitchTab = true, int a_iLine = 0);
    // FR-9：按左栏任务列表（Tab）底部 y 同步阶段表高度，使两者底部对齐（任务列表不含按钮行）。


    // —— 对话流持久化（#1 助手 / #6 任务）：CMainWindow 在存盘/恢复时调用 ——
    // 2026.08.22 多助手：助手对话流改为按「助手」分条存取，Tab 标题即助手名。
    QJsonArray AssistantsToJson() const;              // 全部助手（含 name/blocks/ctxFill/会话态）→ JSON 数组
    void LoadAssistantsFromJson(const QJsonArray& a_aAssts, const QString& a_sActiveId);   // 恢复（重建各 Tab）
    // 2026.08.22 会话态（sid/撤销栈/guideHash）回填：主窗口在切换/落盘前把编排器现值写入该助手。
    void UpdateAsstSession(const QString& a_sId, const QString& a_sSid,
                           const QString& a_sGuideHash, const QJsonArray& a_aStack);
    // 2026.09.20 粘性后端镜像回填（存档用；编排器为权威）。
    void UpdateAsstBackend(const QString& a_sId, const QString& a_sBackend);
    QString AsstBackend(const QString& a_sId) const;   // 2026.09.20 粘性后端读取（空=未绑定）
    QString AsstSid(const QString& a_sId) const;            // 该助手会话 sid（无则空）
    QJsonArray AsstStack(const QString& a_sId) const;       // 该助手撤销栈 JSON
    QString AsstGuideHash(const QString& a_sId) const;      // 该助手 guide.md 投喂哈希
    qint64 AsstCtxFill(const QString& a_sId) const;         // 该助手上下文占用快照
    // 2026.08.27 助手独立流量记录：未选任务时助手消耗不再丢弃，累计到该助手自身
    //   （随 assistant.json 持久化）。选了任务仍归集到任务（原行为不变，历史记录不丢）。
    void AddAsstUsage(const QString& a_sId, qint64 a_iIn, qint64 a_iOut,
                      double a_dCost, qint64 a_iMs, int a_iReqs);
    // 2026.08.27 本轮实时计量（任务阶段/助手轮次通用）：显示在费用栏右侧“本轮 ↑x ↓y”，
    //   阶段/轮次开始清零、每完成一次 API 请求刷新；(0,0) 隐藏。
    void SetLiveUsage(qint64 a_iIn, qint64 a_iOut);
    void ApplyLiveUsage(qint64 a_iIn, qint64 a_iOut);   // 限频到点实际上屏
    // 2026.09.05 助手「本轮 ↑↓」per-助手路由（原全局单值在多助手并发下互相覆盖）：
    //   编排器按会话累计后带助手 id 上抛，面板刷该助手页内自己的实时标签。
    void OnAdhocLiveUsage(const QString& a_sAsstId, qint64 a_iIn, qint64 a_iOut);
    void FlushAsstLiveUsage();      // 限频到点：逐助手冲刷 m_aPendingAsstLive 到各自标签
    // 2026.09.05 v2.0.120 执行态指示器观察口（测试/诊断）：per-助手页内指示器（统计栏
    //   费用前，转圈=该助手运行中）与任务侧底部指示器；BusyIndicatorSpinning 判转圈态
    //   （CBusyIndicator 定义在 .cpp、无 Q_OBJECT，经 QWidget* 透出避免头文件依赖）。
    QWidget* AsstBusyIndicator(const QString& a_sId) const;
    QWidget* TaskBusyIndicator() const;
    // 2026.09.23 v3.1.227 阶段条显隐观察口（测试/诊断）：无任务选中时阶段条自隐藏、
    //   日志 Tab 区顶上移到原任务进度位置（SetCurrentTask/RefreshTask 驱动）。
    bool StageBarVisible() const { return m_pStageBar != nullptr && m_pStageBar->isVisible(); }
    static bool BusyIndicatorSpinning(const QWidget* a_pW);
    QString ActiveAsstId() const { return m_sActiveAsst; }  // 当前激活（显示中）的助手 id
    // 2026.09.02 跨轮上下文回灌：提取指定助手最近对话记录（"用户：…"/"助手：…"
    //   交替，时间升序，已去时间戳前缀并按预算截断），供不支持会话续接的后端
    //   （DSH/Codex/Hermes）每轮把历史回灌进 prompt。a_sCurText = 本次即将提交的
    //   提问（末尾周着它的 user 块已上屏，剔除防重复）；助手不存在/无历史返回空。
    QStringList BuildAsstTranscript(const QString& a_sAsstId, const QString& a_sCurText) const;
    //   2026.09.07 v2.1.124 连发助手段只留末条（终答）：一轮内 Hermes v2.0.116 中间段
    //   叙述块会以独立 Assistant 块落流，逐条回灌会让历史像"未完成的现行任务"——
    //   实测「你现在用的是什么模型？」被回灌历史带偏成继续执行 record.md 追加
    //   （用户报障：回答与需求毫无关系）；段内叙述丢弃只留终答（回归 asstTranscript）。
    // 2026.08.27 升 public 供测试驱动（test_execpanel 用例）：发送当前输入。
    // 2026.09.03 多助手并发：激活助手在跑 → 注入/续发（同助手追加）；空闲 → 立即提交
    // 该助手自己的会话（其他助手在跑与否无关）。
    // 2026.09.05 per-助手输入栏：目标助手 id 显式传入（空 = 激活助手，测试兼容）。
    void DoAdhocSend(const QString& a_sAsstId = QString());
    void ResetAssistants();                                 // 复位为单个默认助手（清空会话/对话流）
    void ClearAsstTabs();                                   // 拆除全部助手 Tab + 清模型（不建默认助手；载档/复位共用，含「+」页再入防护）
    int AsstCount() const { return m_aAssts.size(); }
    // 2026.09.18 测试观察口：指定助手当前暂存提示词数（右下角折叠框数据源）。
    int StagedCount(const QString& a_sId) const;
    // 2026.09.20 暂存提示词编辑/删除（未发送的排队指令；索引按暂存顺序）：
    //   Remove/ApplyEdit 同步发信号让编排器侧 aPendingInjects 同删除/改写；
    //   EditStagedPrompt 弹多行编辑框（UI 入口，确认后走 ApplyStagedEdit）。
    void RemoveStagedPrompt(const QString& a_sId, int a_iIndex);
    void ApplyStagedEdit(const QString& a_sId, int a_iIndex, const QString& a_sNewText);
    void EditStagedPrompt(const QString& a_sId, int a_iIndex);
    // 2026.09.18 退出确认用：按 id 取助手显示名（未命中回退 id 本身）。
    QString AsstDisplayName(const QString& a_sId) const;
    QJsonArray TaskBlocksToJson(const QString& a_sTaskId) const;
    void LoadTaskBlocksFromJson(const QString& a_sTaskId, const QJsonArray& a_aBlocks);
    bool HasTaskBlocks(const QString& a_sTaskId) const { return m_aStreams.contains(a_sTaskId); }
    void ClearAllBlocks();   // 切工程：清空全部内存对话流（任务 + 各助手）

signals:
    // 2026.09.03 多助手并发：提交/撤销/压缩/停止均带目标助手 id（编排器按 id 路由会话）。
    // 2026-09-11 权限三态化：a_iPerm＝0 自动审批（缺省）/1 手工审批/2 只读
    //   （CTaskOrchestrator::EAdhocPerm，原 a_bReadOnly 布尔三态化）。
    void sigAdhocSubmit(const QString& a_sAsstId, const QString& a_sText, int a_iPerm);
    // 2026.09.20 未发送排队指令的删除/改写：面板暂存框操作 → 主窗口转接编排器
    //   RemoveQueuedAdhoc/EditQueuedAdhoc（aPendingInjects 与暂存框双份，按原文本匹配同步）。
    void sigAdhocQueuedRemove(const QString& a_sAsstId, const QString& a_sText);
    void sigAdhocQueuedEdit(const QString& a_sAsstId, const QString& a_sOldText,
                            const QString& a_sNewText);
    void sigAdhocUndo(const QString& a_sAsstId);
    void sigAdhocCompact(const QString& a_sAsstId);
    void sigAdhocStop(const QString& a_sAsstId);
    // 2026-10-02 跨档幽灵：外档运行中助手被点击（切回归属任务/工程档）与结束（落盘后除幽灵）。
    void sigAsstForeignActivated(const QString& a_sOwnerKey);
    void sigAsstForeignFinished(const QString& a_sAsstId);
    void sigAdhocBlocksChanged();                         // 助手流变化（#1 存盘）
    // 2026.08.22 多助手事件：新增/删除/切换/重命名/拖动排序 → 主窗口换载编排器会话或落盘。
    void sigAsstAdded(const QString& a_sId);                          // 新增助手（已激活）
    void sigAsstRemoved(const QString& a_sId, const QString& a_sNewActive);   // 删除助手
    void sigAsstSwitched(const QString& a_sOldId, const QString& a_sNewId);   // 切换激活助手（点选 Tab）
    void sigAsstLayoutChanged();                                      // 重命名 / 拖动排序（需落盘）
    // 2026-09-22 右键「自动重命名」：面板收集该助手全部用户提示词（去时间戳、尾部限量），
    //   经主窗口转编排器 AutoNameAsstNow（借用会话交 Agent 总结名称+说明）。
    void sigAsstAutoRename(const QString& a_sAsstId, const QString& a_sPrompts);
    void sigTaskChatChanged(const QString& a_sTaskId);    // 任务流变化（#6 去抖存盘）
    // 右键「设置上下文大小」→ 上抛用户设的上下文窗口上限（token；0=回退自动探测）。
    void sigAdhocContextLimitChanged(qint64 a_iLimit);
    // FR-3：任务对话流介入输入栏 → 主窗口 → 编排器 InjectTaskMessage。
    void sigTaskInject(const QString& a_sTaskId, const QString& a_sText);
    // 需求3：阶段表右键「再次执行」→ 主窗口 → 编排器 RunSingleStage（仅该阶段，不推进后续）。
    void sigRerunStage(CTaskInfo* a_pTask, int a_eStage);
    // 2026.08.23 死代码清理：sigRollbackTurn 已删——↶ 右侧▼（方案 §5.1 助手侧代码回退
    //   入口）v1.0.224 移除后无发射点；代码回退唯一入口 = 任务列表「回退」按钮。

protected:
    bool eventFilter(QObject* a_pWatched, QEvent* a_pEvent) override;   // 助手输入框 Ctrl+Enter 发送

    // 2026.08.22 日志行时间前缀：流里已有日期 → 只加 [HH:mm:ss]；否则连日期一起
    QString WithLogTime(const QString& a_sTaskId, const QString& a_sText);
    QSet<QString> m_aDatedLogTasks;   // 已确认带日期（或已写入日期）的任务
    // 2026.08.22 与上一次已输出的时间戳间隔 <60s 时不输出时间（返回空串），否则记录并返回前缀
    QString StampPrefix(const QString& a_sKey, bool a_bFullDate);
    QHash<QString, qint64> m_aLastStampSecs;   // 各流（taskId/助手流哨兵键）最后输出的时间戳

private:
    struct SAsst;   // 前置声明（定义在下方；AsstStream 等签名需先引用）

    // —— 对话流呈现（2026.09.03 统一收拢：任务日志与各助手日志共用 CStreamPresenter，
    //    数据/并行件/反向分页前沿/分帧重建状态全部在流对象内，面板只做路由与生命周期）——
    // 任务侧：每任务一个流（堆上指针，QHash 重哈希不移动指向体）；共享同一个 m_pView，
    //    当前流 Bind+Rebuild，切出流 Unbind（弃件保数据）。
    CStreamPresenter* EnsureStream(const QString& a_sTaskId);   // 取/建任务流（fnDirty→MarkTaskDirty）
    QHash<QString, CStreamPresenter*> m_aStreams;   // taskId → 任务流（数据常驻，不随切换丢失）
    CStreamPresenter* m_pCurStream = nullptr;       // 当前任务流（绑 m_pView；空=无选中任务）

    void AppendData(const QString& a_sTaskId, const SBlockData& a_stData);   // 入流数据 + 当前任务则上屏（路由到 EnsureStream(id)->Append）
    void RebuildView();        // 重建当前任务流视图（m_pCurStream->Rebuild；无当前流时空操作）
    void AppendAdhocBlock(const SBlockData& a_stData, const QString& a_sTargetId = QString());   // ad-hoc 块入助手流 + 上屏（目标 id 空=运行中助手，否则指定助手；2026.08.27 增 id 直投——跨助手排队徽标要落到指定助手）
    void RebuildAdhocView();   // 按激活助手数据重建其对话流（撤销/切档后调用）
    void RebuildAsstView(const QString& a_sId);   // 2026.08.23 按指定助手重建（多助手各自独立控件，加载后预建防切换卡顿）
    CStreamPresenter* AsstStream(SAsst& a_stAsst);   // 取/建助手流（fnDirty→sigAdhocBlocksChanged；懒建，SAsst 值拷贝后共享同一流）
    void UpdateAdhocInputState(const QString& a_sAsstId = QString());   // 按 m_bAdhocEnabled/各助手运行态刷新其输入栏可用性（2026.09.05 per-助手；空 id=全量刷新）
    // 输入框高度自适应通用版：按字体度量算单行高、按 blockCount 线性增长、封顶 a_iMaxLines 行。
    // 介入栏与助手栏共用，保证两框初始高度与增长行为完全一致（#3）。
    static void AdjustInputHeight(QPlainTextEdit* a_pEdit, int a_iMaxLines);
    // FR-3：助手日志右上角半透明提示词按钮 hover/leave → 显示/隐藏提示词下拉浮层。
    void OnPromptBtnHovered();
    void OnPromptBtnLeaved();
    // 2026.09.13 浮层收起判定：全局鼠标位是否仍在「浮层 ∪ 触发按钮 ∪ 条目全文 tip 窗」
    //   区域内（含 8px 容差）。鼠标常会移到条目 tip（贴光标在浮层外沿展开）上读全文，
    //   旧规则 Leave 即收会把浮层连同 tip 一起带走；新规则 500ms 后按位置判定，
    //   在 tip 窗上则续命重查。
    bool IsCursorInsideFlyoutRegion();
    // FR-3：点击下拉中某条提示词 → 把对应 User 提问块滚到助手日志最顶 + 横纵滚动条归零。
    void OnPromptPicked(int a_iBlockIdx);
    // FR-3：按历次 User 提问重建下拉浮层列表（块变化 / 弹出前刷新）。
    void RebuildPromptList();
    void UpdateBusyIndicator();     // 刷新执行态指示器（任务/助手运行→转圈，否则状态色点）
    // 2026.09.05 per-助手控件组：上下文进度条/统计标签随助手走，入参 = 目标助手。
    void UpdateContextButton(const SAsst& a_stAsst);   // 该助手上下文进度条占比/颜色/tooltip
    void UpdateAsstCostLabel(const SAsst& a_stAsst);   // 该助手统计栏（费用/token/请求/耗时）
    qint64 AsstContextLimit(const SAsst& a_stAsst) const;   // 该助手窗口分母（用户上限>模型探测）
    void CreateAsstWidgets(SAsst& a_stAsst);   // 建助手页容器（日志视图+输入栏+统计栏）并接信号
    void SyncRunTimer();            // 2026.09.05 耗时定时器统一开关（任务或任一助手在跑即跑）
    void UpdateTaskInjectState();   // FR-3 按当前任务 Running 态刷新介入输入栏可用性
    void UpdateCostLabel();         // 2026.08.15 刷新底部费用/状态栏（2026.09.05 起仅任务侧；助手统计在各自页内）
    void OnRunTimer();              // 2026.08.15 运行中每秒刷新耗时显示
    static qint64 ContextLimitForModel(const QString& a_sModel);  // 模型名 → 上下文窗口 token 上限
    void MarkTaskDirty(const QString& a_sTaskId);   // 标脏 + 启动去抖 timer（#6）
    void FlushTaskChat();                            // 去抖到期：逐脏 id 发 sigTaskChatChanged
    void LoadStageOutputs(CTaskInfo* a_pTask);  // 「阶段产物」页：按任务流程阶段列出已生成的 .md
    void OnOutputSelected(int a_iRow);          // 选中某阶段产物 → 读取文件内容显示
    void OnStagesContextMenu(const QPoint& a_stPos);   // 需求3：阶段表右键「再次执行」

    CStageBar*      m_pStageBar = nullptr;   // 2026.09.04 阶段横向图标条（替代 QTreeWidget 阶段表）
    // （m_pStageSplit 已删：阶段条固定高移出 splitter，FR-9 高度对齐随图标条化移除。）
    CAsstTabWidget* m_pTabs    = nullptr;   // 日志 / 阶段产物 / 助手 /「+」页 / 文件预览（多行 Tab 栏：宽度不足自动换行）
    CAsstTabBar*    m_pAsstBar = nullptr;   // 2026-09-22 多行 Tab 栏本体（= m_pTabs->tabBar()；右键命中用 TabAtPos、运行态用 SetPageRunning）
    QWidget*        m_pAddTabPage = nullptr;  // 「+」新增助手占位页（选中即新增并弹回，参考 DSHAgentManage）
    QWidget*        m_pArtifactsPage = nullptr;  // 「阶段产物」页本体（Tab 布局管制时识别非助手页用）
    QSplitter*      m_pArtifactsSplit = nullptr;  // 2026.08.29 阶段清单|Markdown 分割条（持久化）
    CFilePreviewWidget* m_pFileView = nullptr;  // 2026.08.25 文件预览 Tab（单实例复用，内部分发纯文本/md/html/hex）
    CMessageView*   m_pView    = nullptr;   // Tab1：对话流日志
    QPlainTextEdit* m_pTaskInput = nullptr; // FR-3 任务对话流下方的介入输入栏
    QPushButton*    m_pBtnInject = nullptr; // FR-3 介入发送按钮
    QListWidget*    m_pOutputs = nullptr;   // Tab2 左：阶段产物清单
    CArtifactView* m_pArtifactView = nullptr;  // Tab2 右：选中产物的 Markdown 渲染
    QLabel*         m_pLblCost = nullptr;
    QLabel*         m_pLblLive = nullptr;   // 2026.08.27 本轮实时 token（运行中“本轮 ↑x ↓y”直观反馈）
    QTimer*         m_pLiveTimer = nullptr; // 限频上屏定时器（200ms 单次）
    qint64          m_iPendingLiveIn = 0;   // 暂存的最新流式值
    qint64          m_iPendingLiveOut = 0;
    CBusyIndicator* m_pBusy    = nullptr;   // 执行态动画指示器（费用文本前，#4）
    QTimer*         m_pRunTimer = nullptr;  // 2026.08.15 运行耗时 1s 刷新定时器（任务 Running 或任一助手运行中跑；2026.09.05 统一 SyncRunTimer 开关）
    bool            m_bTaskRunning = false; // 当前选中任务是否运行中（#4 指示器用）
    QColor          m_colIdle;             // 指示器空闲态颜色（随任务状态）
    // （2026.09.05 per-助手化已删：m_tAdhocClock/m_iContextLimit/m_iLastContextFill/
    //   m_sAdhocModel/m_pAdhocInput/m_pComboPerm/m_pBtnContext/m_pBtnSend/m_pBtnUndo——
    //   耗时钟/窗口分母/进度条/输入栏全部随助手存在 SAsst 内，切换页即切实例。）
    qint64          m_iUserContextLimit = 0;       // 用户设的窗口上限（>0 覆盖按模型探测；0=自动；全局配置对各助手一致）

    QString                          m_sCurId;
    CTaskInfo*                       m_pCurTask = nullptr;  // 需求3：当前显示任务（供阶段表右键取）
    // 对话流状态已在 CStreamPresenter 内（2026.09.03 收拢）：任务侧见上方 m_aStreams/m_pCurStream，
    //   助手侧见 SAsst::pStream。渐进重建/反向分页前沿/并行件列表不再散落面板成员。

    // —— 提示词 agent（独立对话流，按「助手」分 Tab，不受任务切换影响）——
    // 2026.08.22 多助手：每个助手一个 Tab（标题=助手名，可关闭/拖动排序/双击重命名），
    //   各自持有独立对话流；2026.09.03 流本体收拢为 CStreamPresenter（与任务日志共用）。
public:
    struct SAsst {
        QString               sId;            // 助手 id（uuid，跨重命名稳定）
        QString               sName;          // 助手名（Tab 标题）
        // 2026-10-02 跨档幽灵（任务助手不停跑跨档保留）：该助手归属的存档 key
        //   （任务 id；空=工程全局档）——与当前视图 key（m_sViewKey）不一致即为
        //   外档幽灵（粗体 Tab + 任务名 tooltip，输入禁用，点击切回归属任务）。
        QString               sOwnerKey;
        QString               sOwnerName;     // 归属任务名（tooltip 展示；工程档为空）
        QString               sDesc;          // 2026-09-22 助手说明（自动重命名生成，Tab tooltip 首行）
        bool                  bAutoNameTried = false;  // 2026.09.26 本轮已自动改名尝试过（防收尾重入循环；用户再发提示词时清零）
        bool                  bAutoNameQueued = false;  // 2026-10-03 运行中请求的自动重命名挂起位（本轮收尾 OnAdhocFinished 执行）
        QString               sBackend;       // 2026.09.20 粘性后端名（"" = 未绑定；档持久化）
        QString               sSid;           // 会话 sid（编排器会话换载时回填）
        QString               sGuideHash;     // guide.md 投喂哈希基线
        QJsonArray            aStack;         // 撤销栈 JSON（prompt,sid 链）
        qint64                iCtxFill = 0;   // 上下文占用快照（恢复进度条用）
        bool                  bRunning = false;  // 2026.09.03 多助手并发：该助手运行中（Tab ●、按钮态）
        QString               sModel;            // 2026.09.03 该助手最近所用模型名（切 Tab 还原上下文窗口显示）
        // 2026.08.27 未选任务时的独立消耗累计（选任务时消耗归任务，不进这里）
        double                dCost = 0.0;
        qint64                iInTok = 0;
        qint64                iOutTok = 0;
        int                   iReqs = 0;
        qint64                iElapsedMs = 0;
        // 2026.09.03 对话流收拢：数据/件/前沿/重建状态全在流对象内（shared：SAsst 值拷贝
        //   搬移场景（ReopenAsst 等）共享同一流，不产生双份）。懒建（AsstStream），未建时为空。
        CStreamPresenterPtr   pStream;
        // 2026.09.05 per-助手控件组（用户需求：提示词/发送按钮/统计信息全部随助手 Tab 页切换，
        //   独立实例）：Tab 页本体 = pPage 容器（日志视图 + 输入栏 + 统计栏）；关闭后全部置空，
        //   重开重建（统计数值随 SAsst 持久化字段还原，不随控件丢）。
        QWidget*              pPage = nullptr;        // Tab 页容器（日志+输入栏+统计栏）
        CMessageView*         pView = nullptr;    // 该助手的对话流视图（pPage 内；关闭后为 null，重开重建）
        QPlainTextEdit*       pInput = nullptr;   // 该助手指令输入框（Ctrl+Enter 发送）
        QComboBox*            pComboPerm = nullptr;   // 2026-09-11 权限下拉：0 自动审批（缺省）/1 手工审批/2 只读（原 pBtnReadOnly 勾选钮）
        int                   iPerm = 0;              // 权限三态持久化字段（assistant.json "perm"；闭页后控件重建回显）
        QProgressBar*         pBtnContext  = nullptr;   // 上下文占比进度条（点击/右键菜单）
        QPushButton*          pBtnUndo     = nullptr;   // ↶ 撤销
        QPushButton*          pBtnSend     = nullptr;   // 发送/追加/停止
        QLabel*               pLblCost = nullptr;      // 该助手统计栏（费用/token/请求/耗时）
        QLabel*               pLblLive = nullptr;      // 该助手本轮实时 ↑↓
        CBusyIndicator*       pBusy = nullptr;   // 2026.09.05 v2.0.120 该助手执行态指示器（统计栏费用前：运行转圈/空闲灰点）
        QElapsedTimer         tClock;                  // 本轮起始钟（耗时实时跳动，per-助手：并发助手各自计时）
        QElapsedTimer         tRound;                  // 2026.09.19 当前提示词轮起点（每轮 StartAdhocStage 广播 SetAsstRunning(true) 时重锚，含自动续发/压缩；done 事件读它算本轮耗时）
        QToolButton*          pBtnPrompts = nullptr;  // FR-3 右上角提示词按钮（parent=pView）
        QToolButton*          pBtnBottom  = nullptr;  // 提示词按钮正下方「↓」按钮（点击滚动到日志最底）
        // 2026.09.18 运行中暂存提示词折叠框（日志区右下角，parent=pView）：
        //   运行中发送的提示词先进这里（不上屏），实际开始处理（注入立即生效/排队轮启动）
        //   才渲染为正式用户块；缺省折叠只显示第一条，左侧 8×8 按钮展开/折叠全部。
        //   aStaged 元素=(展示文本含时间戳, 原始文本)；框为瞬态显示，不入 assistant.json。
        QFrame*               pQueueBox = nullptr;    // 折叠框本体（无暂存时隐藏）
        QToolButton*          pQueueBtn = nullptr;    // 展开/折叠按钮（8×8 图标）
        QLabel*               pQueueHead = nullptr;   // 首条提示词（省略号截断）
        QWidget*              pQueueList = nullptr;   // 展开态的全部条目容器
        bool                  bQueueExpanded = false;
        QList<QPair<QString, QString>> aStaged;   // 暂存队列：(展示文本, 原始文本)
        bool                  bClosed = false;   // 2026.08.23 已关闭（Tab 摘除但数据保留，可从「▾」菜单恢复）
    };

public:
    // 2026-10-02 档重建前摘下全部运行中助手（跨档幽灵 + 本档运行者；块在 pStream
    //   共享指针内随值拷贝存活）——摘除 Tab 与页控件，列表项移除并返回深拷贝；
    //   挂回=追加列表+建 Tab（外档者粗体+tooltip，本档者转正常）。
    //   会话保留集=全部运行 id（ResetAdhocSessions 据此不杀在跑轮次——切回归属档
    //   也不能杀，v3.2.259 E2E 实测回归切档会误杀）。
    QList<SAsst> TakeRunningForReload();
    void AttachForeignAssistants(QList<SAsst> a_aList, const QString& a_sTaskNameOf);
    // 按 id 除列表项（档重载去重：文件版让位存活版；宿主在载档后、挂回前调用）。
    void DropAsstEntries(const QStringList& a_aIds);
    bool AsstIsForeign(const SAsst& a_st) const { return a_st.sOwnerKey != m_sViewKey; }
    QList<SAsst> AllAsstsSnapshot() const { return m_aAssts; }

private:
    SAsst* AsstById(const QString& a_sId);
    const SAsst* AsstById(const QString& a_sId) const;
    SAsst* ActiveAsst();              // 当前激活助手（无助手时 nullptr）
    // （RunningTargetAsst 已删：编排器信号带助手 id，直投对应助手流。）
    SAsst* AsstByPage(QWidget* a_pPage);   // 按助手页容器反查助手（Tab 事件定位；2026.09.05 Tab 页本体=pPage）
    // 2026.09.03 反向分页（助手流版，依赖 SAsst 故在此声明）：滚动到顶补建更早一段；
    //   BuildOlderChunk 为两流共用实现——补 [frontier-100, frontier) 入临时列表（块内
    //   合并仍生效），再前插对齐外层并行列表并推进前沿；EnsureAsstCoverage 供提示词
    //   跳转目标早于前沿时分轮补建至覆盖后滚动到位。
    void PrependAsstOlder(SAsst* a_pAsst);
    void BuildOlderChunk(const QList<SBlockData>& aBlocks, QList<CMessageBlock*>& aWidgets,
                         int& a_ioFrontier, QList<CMessageBlock*>& a_aNewWidgets);
    void EnsureAsstCoverage(const QString& a_sId, int a_iTargetIdx);
    // 2026.09.03 跨助手排队机制已删（多助手并发下不再排队，激活助手空闲即发）；
    //   同助手运行中追加的排队在编排器会话内（aPendingInjects，续发语义不变）。
    QString AsstTabText(const SAsst& a_st) const;  // Tab 标题（恒纯名；运行标记=Tab 栏自绘闪点）
    void RefreshAsstTabTexts();      // 统一刷全部助手 Tab 标题 + 运行闪点同步 + 闪点时钟启停
    // 2026.09.19 Tab 运行态闪点：运行中的助手 Tab 在标题旁画彩色闪烁 ●（绿→青→蓝→紫，
    //   500ms/拍）。2026-09-22 起随多行 Tab 栏（CAsstTabBar）改为栏内自绘（旧 QLabel 经
    //   QTabBar::setTabButton 挂点方案在多行布局下会错位，已废）——本函数转投运行态给
    //   栏（SetPageRunning，页指针键），变色节拍/色序由栏自管。
    void SyncAsstTabDot(SAsst& a_stAsst, int a_iIdx);   // 运行→栏内画点；空闲→撤点
    // FR-3：为指定助手日志右上角创建半透明提示词按钮（hover 弹出、移开收起、点击跳转）。
    void CreatePromptButton(SAsst& a_stAsst);
    void SetupTabsInteraction();      // 可移动 / 原生「x」/「+」页 /「▾」角落按钮（构造一次，参考 DSHAgentManage）
    void UpdateTabCloseButtons();     // 非助手页与「+」页隐藏关闭按钮；「+」页钉在最后助手 Tab 之后
    void OnAsstTabCloseRequested(int a_iIdx);   // 原生「x」：关闭对应助手
    CMessageView* CreateAsstTab(SAsst& a_stAsst, int a_iPos = -1);   // 建视图 + 提示词按钮 + 插 Tab
    void RemoveAsstTab(SAsst& a_stAsst);                            // 摘 Tab（视图交 parent 回收）
    void CloseAsst(const QString& a_sId);       // 点「x」：关闭该助手（Tab 摘除、数据保留，可经「▾」恢复）
    // 2026.09.09 右键彻底删除 UI 层：守卫（运行中/最后一个打开助手）+ 二次确认弹窗，
    //   确认后交 public DeleteAsstCore 执行；编排器会话经 sigAsstRemoved 由主窗口清理。
    void DeleteAsst(const QString& a_sId);
    void OnAsstMenuClicked();                  // 点「▾」：弹出已关闭助手菜单（选择恢复）
    void ReopenAsst(const QString& a_sId);     // 从菜单恢复已关闭助手（重建 Tab 并激活）
    void OnAsstAddClicked();                 // 点「+」：新增助手并激活（名字自动编号）
    void OnAsstTabRenamed(int a_iIdx);       // 双击助手 Tab → 改名
    void OnAsstTabMoved(int a_iFrom, int a_iTo);   // 拖动 → 模型顺序同步 Tab 相对顺序
    void OnAsstTabCurrentChanged(int a_iIdx);      // 切 Tab：激活助手切换 + 状态栏同步
    void OnAsstTabContextMenu(const QPoint& a_stPos);   // 右键助手 Tab：重命名 + 彻底删除入口
    void SyncAsstsOrderToTabs();             // 拖动后按 Tab 顺序重排 m_aAssts
    // 2026.08.25 助手是否有用：对话流中存在用户发送的提示词（Event/user 块）；
    //   无任何用户提示词 = 空助手（不入「▾」历史，关闭时直接舍弃）。
    static bool AsstHasUserPrompt(const SAsst& a_st);
    // 2026-10-03 收集助手全部用户提示词并组装命名素材（右键「自动重命名」/收尾自动
    //   命名/运行中挂起命名三处同口径：Event/user 块、去时间戳前缀、编号拼接、
    //   总量截尾跟最近 6000 字）。返回空串 = 无素材。
    QString BuildAsstAutoNameMaterial(SAsst* a_pAsst) const;
    static QString AsstStampKey(const QString& a_sId);   // 时间戳哨兵键（@@adhoc@@+id，兼容旧流）
    // 2026.09.18 暂存折叠框刷新：重建内容/展开态/右下角定位/可见性（无暂存隐藏；
    //   仅该助手 Tab 当前可见时显示）。CreateAsstWidgets 建框、Resize/Show/切 Tab 路径定位。
    void RefreshQueueBox(const QString& a_sId);
    CPromptFlyout*         m_pPromptFlyout = nullptr;  // FR-3 提示词下拉浮层（单实例，hover 弹出、移开收起；仅当前助手页可见，锚定其按钮）
    QTimer*                m_pFlyoutHideTimer = nullptr;   // 2026.09.13 浮层收起定时器：鼠标离开区域 500ms 后才收（位置判定，见 IsCursorInsideFlyoutRegion）
    QList<SAsst>           m_aAssts;                   // 全部助手（顺序 = Tab 顺序；已关闭的排在末尾，bClosed=true）
    QString                m_sViewKey;                 // 2026-10-02 当前视图存档 key（任务 id/空=工程档）：SAsst::sOwnerKey 与之不等即外档幽灵
    QString                m_sActiveAsst;              // 当前激活助手 id
    // （m_sRunningAsst 已删：运行态 per-助手存 SAsst::bRunning。）
    // （m_aAdhocQueue 已删：跨助手排队机制随多助手并发移除。）
    QList<int>             m_aPromptIdx;               // FR-3 激活助手流中「用户提问」块下标（旧→新，供下拉跳转）
    bool                   m_bAdhocEnabled = false;    // 工程是否已打开（各助手输入栏总开关）
    // （m_bAdhocRunning 已删：以 AnyAsstRunning() 按 SAsst::bRunning 聚合。）
    bool                   m_bAsstTabsResetting = false;   // ResetAssistants 拆 Tab 期：忽略程序性 currentChanged（防「+」页自动激活被当新增 → 死循环）
    // 2026.09.05 per-助手「本轮 ↑↓」限频暂存：id → 最新流式值（含 (0,0) 隐藏指令），
    //   共享 200ms 单次定时器到点统一上屏（与任务侧 m_iPendingLive* 同一限频口径）。
    QHash<QString, QPair<qint64, qint64>> m_aPendingAsstLive;

    // —— 任务对话流去抖落盘（#6）——
    QTimer*                m_pChatSaveTimer = nullptr; // 去抖写定时器（500ms 合并高频输出）
    QSet<QString>          m_aDirtyTaskIds;            // 待落盘任务 id 集合
};
