#pragma once

#include <QWidget>
#include <QHash>
#include <QList>
#include <QSet>
#include <QJsonArray>
#include <QJsonObject>
#include <QColor>

#include "ui/CMessageBlock.h"   // SBlockData 复用 CMessageBlock::EKind

class QTreeWidget;
class QTreeWidgetItem;
class QLabel;
class QTabWidget;
class QSplitter;
class QListWidget;
class QListWidgetItem;
class QPlainTextEdit;
class QPushButton;
class QProgressBar;
class QToolButton;
class QTimer;
class CBusyIndicator;
class CArtifactView;
class CTaskInfo;
class CMessageView;
class CPromptFlyout;   // FR-3：助手日志提示词下拉浮层（半透明按钮 hover 弹出）

// 右侧执行面板：上方阶段进度、中间 Tab（日志对话流 / 阶段产物 .md）、底部 cost/token 统计。
// 对话流由 CMessageView 承放 CMessageBlock；多任务按 id 缓存结构化 block 数据，切换时重建。
// 「阶段产物」页按选中任务的流程阶段，列出 .autodev/tasks/<id>/ 下已生成的 .md 产物供查看。
class CExecutionPanel : public QWidget {
    Q_OBJECT

public:
    explicit CExecutionPanel(QWidget* a_pParent = nullptr);

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
    void RefreshTask(CTaskInfo* a_pTask);       // 状态变化时刷新阶段表/cost
    // FR-5：阶段产物落盘后由主窗口在 stage_done 事件调用——重扫产物目录并保留当前选中态
    //（重扫前记录选中产物绝对路径，重扫后若该文件仍存在则重新选中同一行，避免查看内容被清空）。
    void RefreshStageOutputs(CTaskInfo* a_pTask);

    // —— 提示词 agent（「助手」Tab，独立于任务的对话流）——
    void OnAdhocAssistantText(const QString& a_sText);
    void OnAdhocToolUse(const QString& a_sTool, const QString& a_sInputJson);
    void OnAdhocToolResult(const QString& a_sResult);
    void OnAdhocLog(const QString& a_sLine);
    void OnAdhocEvent(const QString& a_sKind, const QString& a_sText);
    void OnAdhocUndo();                // 撤销：移除最后一条 prompt 块 + 其回复/工具块并重建
    void OnAdhocFinished();            // 一次助手调用结束：重新启用输入栏
    // 2026.08.19 运行态对外置位：编排器自动续发排队指令起新轮时广播（见 sigAdhocRunning），
    //   手动发送路径面板已自置、此处幂等。原 private，升 public 供主窗口连接。
    void SetAdhocRunning(bool a_bRunning);
    void SetAdhocContext(qint64 a_iFill);   // 更新上下文进度条（<0 视为占用未知，进度归零）
    qint64 LastContextFill() const { return m_iLastContextFill; }   // 供主窗口存盘：当前上下文占用快照
    void OnAdhocModel(const QString& a_sModel);   // 收到模型名 → 查表更新上下文窗口分母
    void SetUserContextLimit(qint64 a_iLimit);    // 右键设窗口上限（>0 覆盖探测；0/负回退自动）
    void SetAdhocEnabled(bool a_bEnabled);  // 工程未打开时禁用输入栏
    // FR-3：资源管理器「添加到聊天框」→ 把文本追加到 adhoc 助手输入框末尾。
    void AppendToAdhocInput(const QString& a_sText);
    // 2026.08.18 资源管理器双击文本文件 → 「助手」右侧单实例 Tab 内嵌显示文件内容
    //   （QPlainTextEdit 只读；双击其他文件复用同一 Tab 换内容与标题）。
    //   2026.08.21 增加 a_bSwitchTab：false=资源管理器单击预览——只加载内容/换
    //   标题不切页（用户停留当前 Tab）；默认 true 保持双击切页原行为。
    void ShowFileTab(const QString& a_sAbsPath, bool a_bSwitchTab = true);
    // FR-9：按左栏任务列表（Tab）底部 y 同步阶段表高度，使两者底部对齐（任务列表不含按钮行）。
    void SyncStagesHeight(int a_iTaskListBottomY);

    // —— 对话流持久化（#1 助手 / #6 任务）：CMainWindow 在存盘/恢复时调用 ——
    // 2026.08.22 多助手：助手对话流改为按「助手」分条存取，Tab 标题即助手名。
    QJsonArray AssistantsToJson() const;              // 全部助手（含 name/blocks/ctxFill/会话态）→ JSON 数组
    void LoadAssistantsFromJson(const QJsonArray& a_aAssts, const QString& a_sActiveId);   // 恢复（重建各 Tab）
    // 2026.08.22 会话态（sid/撤销栈/guideHash）回填：主窗口在切换/落盘前把编排器现值写入该助手。
    void UpdateAsstSession(const QString& a_sId, const QString& a_sSid,
                           const QString& a_sGuideHash, const QJsonArray& a_aStack);
    QString AsstSid(const QString& a_sId) const;            // 该助手会话 sid（无则空）
    QJsonArray AsstStack(const QString& a_sId) const;       // 该助手撤销栈 JSON
    QString AsstGuideHash(const QString& a_sId) const;      // 该助手 guide.md 投喂哈希
    qint64 AsstCtxFill(const QString& a_sId) const;         // 该助手上下文占用快照
    QString ActiveAsstId() const { return m_sActiveAsst; }  // 当前激活（显示中）的助手 id
    QString RunningAsstId() const { return m_sRunningAsst; }// 正在执行一轮的助手 id（无则空）
    void ClearRunningAsst();                                // 结束态：清运行中助手（主窗口在读取后调用）
    void ResetAssistants();                                 // 复位为单个默认助手（清空会话/对话流）
    void ClearAsstTabs();                                   // 拆除全部助手 Tab + 清模型（不建默认助手；载档/复位共用，含「+」页再入防护）
    int AsstCount() const { return m_aAssts.size(); }
    QJsonArray TaskBlocksToJson(const QString& a_sTaskId) const;
    void LoadTaskBlocksFromJson(const QString& a_sTaskId, const QJsonArray& a_aBlocks);
    bool HasTaskBlocks(const QString& a_sTaskId) const { return m_aBlocks.contains(a_sTaskId); }
    void ClearAllBlocks();   // 切工程：清空全部内存对话流（任务 + 各助手）

signals:
    void sigAdhocSubmit(const QString& a_sText, bool a_bReadOnly);
    void sigAdhocUndo();
    void sigAdhocCompact();
    void sigAdhocStop();
    void sigAdhocBlocksChanged();                         // 助手流变化（#1 存盘）
    // 2026.08.22 多助手事件：新增/删除/切换/重命名/拖动排序 → 主窗口换载编排器会话或落盘。
    void sigAsstAdded(const QString& a_sId);                          // 新增助手（已激活）
    void sigAsstRemoved(const QString& a_sId, const QString& a_sNewActive);   // 删除助手
    void sigAsstSwitched(const QString& a_sOldId, const QString& a_sNewId);   // 切换激活助手（点选 Tab）
    void sigAsstLayoutChanged();                                      // 重命名 / 拖动排序（需落盘）
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
    // 单条对话流的数据快照（任务切换时据此重建 widget）
    struct SBlockData {
        CMessageBlock::EKind eKind = CMessageBlock::Assistant;
        QString sText;        // Assistant/Event/System/Error 文本
        QString sTool;        // ToolUse 工具名
        QString sInput;       // ToolUse 入参 JSON
        QString sResult;      // 工具结果（到达后回填）
        QString sEventKind;   // Event 的 kind
        QString sViewId;      // 评审多视角并行的视角标识（v1/v2/...；空=串行路径）
    };

    CMessageBlock* CreateWidget(const SBlockData& a_stData);   // 由数据快照构建一个块 widget
    // 2026.08.22 连续工具调用合并上屏：新块与前一块同为 ToolUse 且同视角时并入上一 widget（×N 折叠卡），
    //   返回 false=不满足合并条件（由调用方正常新建 widget）
    bool TryMergeToolWidget(const QList<SBlockData>& aBlocks, QList<CMessageBlock*>& aWidgets,
                            int a_iNewIdx, const SBlockData& a_stData);
    void AppendData(const QString& a_sTaskId, const SBlockData& a_stData);   // 入缓存 + 当前任务则上屏
    void RebuildView();        // 按 m_sCurId 重建整条对话流（分帧渐进，避免数百块同步创建卡死 UI）
    void RebuildChunk(quint32 a_uGen);   // 渐进重建一帧（代际号不符=已被新重建顶替，立即终止）
    void AppendAdhocBlock(const SBlockData& a_stData);   // ad-hoc 块入缓存 + 上屏助手 Tab（运行中→该助手，否则激活助手）
    void RebuildAdhocView();   // 按激活助手数据重建其对话流（分帧渐进，撤销/切档后调用）
    void RebuildAsstView(const QString& a_sId);   // 2026.08.23 按指定助手重建（多助手各自独立控件，加载后预建防切换卡顿）
    void RebuildAdhocChunk(const QString& a_sId, quint32 a_uGen);   // 助手视图渐进重建一帧（id/代际号不符=被顶替，立即终止）
    void UpdateAdhocInputState();   // 按 m_bAdhocEnabled/m_bAdhocRunning 刷新输入栏可用性
    void DoAdhocSend();             // 发送当前输入（Ctrl+Enter / 发送按钮调用）
    void OnAdhocTextChanged();      // 多行输入框高度自适应（按内容行数，上限 10 行）
    // 输入框高度自适应通用版：按字体度量算单行高、按 blockCount 线性增长、封顶 a_iMaxLines 行。
    // 介入栏与助手栏共用，保证两框初始高度与增长行为完全一致（#3）。
    static void AdjustInputHeight(QPlainTextEdit* a_pEdit, int a_iMaxLines);
    // FR-3：助手日志右上角半透明提示词按钮 hover/leave → 显示/隐藏提示词下拉浮层。
    void OnPromptBtnHovered();
    void OnPromptBtnLeaved();
    // FR-3：点击下拉中某条提示词 → 把对应 User 提问块滚到助手日志最顶 + 横纵滚动条归零。
    void OnPromptPicked(int a_iBlockIdx);
    // FR-3：按历次 User 提问重建下拉浮层列表（块变化 / 弹出前刷新）。
    void RebuildPromptList();
    void UpdateBusyIndicator();     // 刷新执行态指示器（任务/助手运行→转圈，否则状态色点）
    void UpdateContextButton();     // 按 fill/limit/model 刷新上下文进度条占比/颜色/tooltip
    void UpdateTaskInjectState();   // FR-3 按当前任务 Running 态刷新介入输入栏可用性
    void UpdateCostLabel();         // 2026.08.15 刷新底部费用/状态栏（含运行耗时）
    void OnRunTimer();              // 2026.08.15 运行中每秒刷新耗时显示
    static qint64 ContextLimitForModel(const QString& a_sModel);  // 模型名 → 上下文窗口 token 上限
    static QJsonObject BlockToJson(const SBlockData& a_stData);   // SBlockData ↔ JSON
    static SBlockData BlockFromJson(const QJsonObject& a_o);
    void MarkTaskDirty(const QString& a_sTaskId);   // 标脏 + 启动去抖 timer（#6）
    void FlushTaskChat();                            // 去抖到期：逐脏 id 发 sigTaskChatChanged
    void LoadStageOutputs(CTaskInfo* a_pTask);  // 「阶段产物」页：按任务流程阶段列出已生成的 .md
    void OnOutputSelected(int a_iRow);          // 选中某阶段产物 → 读取文件内容显示
    void OnStagesContextMenu(const QPoint& a_stPos);   // 需求3：阶段表右键「再次执行」

    QTreeWidget*    m_pStages  = nullptr;
    QSplitter*      m_pStageSplit = nullptr;   // 阶段表/日志Tab 的垂直分隔条（FR-9 对齐经 setSizes 设比例）
    QTabWidget*     m_pTabs    = nullptr;   // 日志 / 阶段产物 / 助手 /「+」页 / 文件预览
    QWidget*        m_pAddTabPage = nullptr;  // 「+」新增助手占位页（选中即新增并弹回，参考 DSHAgentManage）
    QWidget*        m_pArtifactsPage = nullptr;  // 「阶段产物」页本体（Tab 布局管制时识别非助手页用）
    QPlainTextEdit* m_pFileView = nullptr;  // 2026.08.18 文件预览 Tab（双击文本文件，单实例复用）
    CMessageView*   m_pView    = nullptr;   // Tab1：对话流日志
    QPlainTextEdit* m_pTaskInput = nullptr; // FR-3 任务对话流下方的介入输入栏
    QPushButton*    m_pBtnInject = nullptr; // FR-3 介入发送按钮
    QListWidget*    m_pOutputs = nullptr;   // Tab2 左：阶段产物清单
    CArtifactView* m_pArtifactView = nullptr;  // Tab2 右：选中产物的 Markdown 渲染
    QLabel*         m_pLblCost = nullptr;
    CBusyIndicator* m_pBusy    = nullptr;   // 执行态动画指示器（费用文本前，#4）
    QTimer*         m_pRunTimer = nullptr;  // 2026.08.15 运行耗时 1s 刷新定时器（仅当前任务 Running 时跑）
    bool            m_bTaskRunning = false; // 当前选中任务是否运行中（#4 指示器用）
    QColor          m_colIdle;             // 指示器空闲态颜色（随任务状态）
    QString         m_sAdhocModel;                 // 助手当前所用模型名（按钮 tooltip 显示）
    qint64          m_iContextLimit   = 200000;    // 上下文窗口上限（按模型名查表，默认 200K）
    qint64          m_iUserContextLimit = 0;       // 用户设的窗口上限（>0 覆盖按模型探测；0=自动）
    qint64          m_iLastContextFill = 0;        // 最近一次上下文占用（重算分母时复用）

    QString                          m_sCurId;
    CTaskInfo*                       m_pCurTask = nullptr;  // 需求3：当前显示任务（供阶段表右键取）
    QHash<QString, QList<SBlockData>> m_aBlocks;       // 任务 id → block 数据列表
    QList<CMessageBlock*>            m_aCurWidgets;    // 当前任务的块 widget（与 m_aBlocks[m_sCurId] 一一对齐）
    // 渐进重建状态：任务对话流可达数百块（Markdown/工具卡均重），同步重建卡 UI ~10s。
    // 分帧创建（每帧小批量 + 回事件循环），期间新到的当前任务块也走分帧路径防重复上屏。
    int                               m_iRebuildIdx = 0;    // 重建进度（已创建的 block 下标）
    quint32                           m_uRebuildGen = 0;    // 重建代际号（切任务/重建重启时 +1 作废旧帧）
    bool                              m_bRebuilding = false; // 渐进重建中（AppendData 新块交分帧循环按序创建）
    // 2026.08.22 助手视图分帧重建状态移入各助手（SAsst::iRebuildIdx/uGen/bRebuilding）。

    // —— 提示词 agent（独立对话流，按「助手」分 Tab，不受任务切换影响）——
    // 2026.08.22 多助手：每个助手一个 Tab（标题=助手名，可关闭/拖动排序/双击重命名），
    //   各自持有独立对话流与分帧重建状态；「+」角标按钮新增助手。
    struct SAsst {
        QString               sId;            // 助手 id（uuid，跨重命名稳定）
        QString               sName;          // 助手名（Tab 标题）
        QString               sSid;           // 会话 sid（编排器会话换载时回填）
        QString               sGuideHash;     // guide.md 投喂哈希基线
        QJsonArray            aStack;         // 撤销栈 JSON（prompt,sid 链）
        qint64                iCtxFill = 0;   // 上下文占用快照（恢复进度条用）
        QList<SBlockData>     aBlocks;        // 对话流数据快照
        QList<CMessageBlock*> aWidgets;       // 与 aBlocks 一一对齐
        int                   iRebuildIdx = 0;    // 分帧重建进度
        quint32               uGen = 0;           // 分帧重建代际号
        bool                  bRebuilding = false;// 分帧重建中（新块交分帧循环按序创建）
        CMessageView*         pView = nullptr;    // 该助手的对话流视图（Tab 页本体；关闭后为 null，重开重建）
        QToolButton*          pBtnPrompts = nullptr;  // FR-3 右上角提示词按钮（parent=pView）
        QToolButton*          pBtnBottom  = nullptr;  // 提示词按钮正下方「↓」按钮（点击滚动到日志最底）
        bool                  bClosed = false;   // 2026.08.23 已关闭（Tab 摘除但数据保留，可从「▾」菜单恢复）
    };
    SAsst* AsstById(const QString& a_sId);
    const SAsst* AsstById(const QString& a_sId) const;
    SAsst* ActiveAsst();              // 当前激活助手（无助手时 nullptr）
    SAsst* RunningTargetAsst();       // 块归属目标：运行中助手 → 否则激活助手
    SAsst* AsstByView(QWidget* a_pView);   // 按视图反查助手（Tab 事件定位）
    // FR-3：为指定助手日志右上角创建半透明提示词按钮（hover 弹出、移开收起、点击跳转）。
    void CreatePromptButton(SAsst& a_stAsst);
    void SetupTabsInteraction();      // 可移动 / 原生「x」/「+」页 /「▾」角落按钮（构造一次，参考 DSHAgentManage）
    void UpdateTabCloseButtons();     // 非助手页与「+」页隐藏关闭按钮；「+」页钉在最后助手 Tab 之后
    void OnAsstTabCloseRequested(int a_iIdx);   // 原生「x」：关闭对应助手
    CMessageView* CreateAsstTab(SAsst& a_stAsst, int a_iPos = -1);   // 建视图 + 提示词按钮 + 插 Tab
    void RemoveAsstTab(SAsst& a_stAsst);                            // 摘 Tab（视图交 parent 回收）
    void CloseAsst(const QString& a_sId);       // 点「x」：关闭该助手（Tab 摘除、数据保留，可经「▾」恢复）
    void OnAsstMenuClicked();                  // 点「▾」：弹出已关闭助手菜单（选择恢复）
    void ReopenAsst(const QString& a_sId);     // 从菜单恢复已关闭助手（重建 Tab 并激活）
    void OnAsstAddClicked();                 // 点「+」：新增助手并激活（名字自动编号）
    void OnAsstTabRenamed(int a_iIdx);       // 双击助手 Tab → 改名
    void OnAsstTabMoved(int a_iFrom, int a_iTo);   // 拖动 → 模型顺序同步 Tab 相对顺序
    void OnAsstTabCurrentChanged(int a_iIdx);      // 切 Tab：激活助手切换 + 状态栏同步
    void SyncAsstsOrderToTabs();             // 拖动后按 Tab 顺序重排 m_aAssts
    static QString AsstStampKey(const QString& a_sId);   // 时间戳哨兵键（@@adhoc@@+id，兼容旧流）
    QPlainTextEdit*        m_pAdhocInput  = nullptr;   // 指令输入框（多行，Ctrl+Enter 发送）
    QToolButton*           m_pBtnReadOnly = nullptr;   // 只读/完整 切换（checkable）
    QProgressBar*          m_pBtnContext  = nullptr;   // 上下文占比进度条（绿≤70%/红>70%，右键压缩）
    QPushButton*           m_pBtnUndo     = nullptr;   // ↶ 回退（点击→撤销）
    QPushButton*           m_pBtnSend     = nullptr;   // 发送
    CPromptFlyout*         m_pPromptFlyout = nullptr;  // FR-3 提示词下拉浮层（单实例，hover 弹出、移开收起）
    QList<SAsst>           m_aAssts;                   // 全部助手（顺序 = Tab 顺序；已关闭的排在末尾，bClosed=true）
    QString                m_sActiveAsst;              // 当前激活助手 id
    QString                m_sRunningAsst;             // 正在执行一轮的助手 id（空=空闲）
    QList<int>             m_aPromptIdx;               // FR-3 激活助手流中「用户提问」块下标（旧→新，供下拉跳转）
    bool                   m_bAdhocEnabled = false;    // 工程是否已打开（输入栏总开关）
    bool                   m_bAdhocRunning = false;    // 助手是否运行中（运行中禁输入栏）
    bool                   m_bAsstTabsResetting = false;   // ResetAssistants 拆 Tab 期：忽略程序性 currentChanged（防「+」页自动激活被当新增 → 死循环）

    // —— 任务对话流去抖落盘（#6）——
    QTimer*                m_pChatSaveTimer = nullptr; // 去抖写定时器（500ms 合并高频输出）
    QSet<QString>          m_aDirtyTaskIds;            // 待落盘任务 id 集合
};
