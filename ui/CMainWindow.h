#pragma once

#include <QMainWindow>
#include <QList>
#include <QRect>
#include <QPointer>
#include <QMessageBox>
#include <QLocalSocket>   // 2026-10-01 SSbxReq（沙箱异步回包）QPointer 成员需完整类型
#include <QJsonObject>
#include <memory>

class CProjectManager;
class CTaskOrchestrator;
class CTaskListPanel;
class CExecutionPanel;
class CTaskHistoryDB;
class CTaskInfo;
struct SBackupVersion;
class CTitleBar;
class CTitleBarMsgFilter;
class CCodeReviewDialog;   // 2026-09-30 右键代码审查（ReviewSdk）
class QMenuBar;
class QSplitter;
class QLabel;
class QMenu;
class QCloseEvent;
class QShowEvent;
class QLocalServer;
class QLocalSocket;
class QFileSystemWatcher;
class QTimer;

// 主窗口：整合工程管理、任务列表、执行视图、编排器。
class CMainWindow : public QMainWindow {
    Q_OBJECT

public:
    explicit CMainWindow(QWidget* a_pParent = nullptr);
    ~CMainWindow() override;

protected:
    void closeEvent(QCloseEvent* a_pEvent) override;
    // FR-3：Win32 WM_NCHITTEST 拦截——标题栏区返回 HTCAPTION（可拖拽 + 双击最大化），
    //        边缘缩放带返回 HTLEFT/HTTOP/...，按钮区 return false 让 Qt 派发 click。
    bool nativeEvent(const QByteArray& a_sType, void* a_pMsg, qintptr* a_piRes) override;
    // 最大化图标态只由按钮掌管；showEvent 仅负责首帧/从最小化恢复时按真实态初始化图标。
    void showEvent(QShowEvent* a_pEvent) override;
    // frameless 窗口自绘 1px 深色描边（只画边框线，不改变内容区浅色背景）。
    void paintEvent(QPaintEvent* a_pEvent) override;

private slots:
    // 工程菜单（2026.09.22 删 OnNewProject：与 OnOpenProject 除标题外完全相同，
    //   「打开工程目录...」对话框本就支持选空目录从零建工程）
    void OnOpenProject();
    void OnGenerateGuide();
    void OnGuideLog(const QString& a_sLine);
    void OnGuideFinished(bool a_bOk, const QString& a_sMsg);
    void OnSyncGuide();                                         // 需求4 手动「同步 Guide」
    void OnGuideSyncFinished(bool a_bOk, const QString& a_sMsg);   // 需求4 同步结果→状态栏

    // 功能菜单
    void OnInstallEnv();
    // 2026.08.15 设置对话框：编辑 config.ini 全部可调参数，确定后落盘并应用到运行期。
    void OnSettings();
    // 2026.09 后端版本管理对话框（任务 23183ccb）：四后端版本查看/查询/更新入口。
    void OnVersionManage();

    // 帮助菜单（FR-9）
    void OnHelp();
    void OnAbout();

    // 最近打开（2026.09.26 v3.1.239 按钮面板化：子菜单内嵌 QWidgetAction 面板，
    //   旧 20 条 QAction 列表退役；详见 CRecentChipsPanel 头注）
    void OnRecentChipOpen(const QString& a_sPath);   // 面板按钮：先收菜单链再打开工程
    void OnRecentChipMore();                         // 面板「更多...」：先收菜单链再开全量对话框
    void CloseRecentMenus();                         // 收起子菜单与父级工程菜单
    void OnOpenAllRecent();                          // 全量对话框（搜索/多选删除/计数）

    // 任务列表
    void OnNewTask();
    void OnStartTask(CTaskInfo* a_pTask);
    void OnPauseTask(CTaskInfo* a_pTask);
    void OnStopTask(CTaskInfo* a_pTask);
    void OnRollback(CTaskInfo* a_pTask);   // FR-6：打开统一回退界面 → accept 后引擎执行
    // 统一回退（方案 §5.1/§8）：助手侧入口（↶ 右侧▼）与两粒度回退引擎。
    void OnRollbackTurnOpen();
    void OnRollbackTo(CTaskInfo* a_pTask, const SBackupVersion& a_stVer);
    void OnRollbackTurnTo(const SBackupVersion& a_stVer);
    void OnDeleteTask(CTaskInfo* a_pTask);
    void OnRemoveTask(CTaskInfo* a_pTask);   // 当前任务移除到历史（保留记录，可再次执行）（#3）
    void OnReviveTask(CTaskInfo* a_pTask);   // 失败任务加回当前栏续跑（#6）
    void OnTaskSelected(CTaskInfo* a_pTask);

    // 查看任务（FR-5）：右键「查看任务内容」→ 只读对话框
    void OnViewTask(CTaskInfo* a_pTask);

    // 2026.08.20 重命名：右键「重命名」→ 输入框改任务名（清 autoNamed，落 task.json + 历史库）
    void OnRenameTask(CTaskInfo* a_pTask);

    // 2026.08.20 自动命名：右键「自动命名」→ 任务内容+guide.md 拼提示词交 Agent
    //   （AutoNameNow，异步；助手忙时本地启发式兜底），完成后 OnAutoNameDone 镜像/刷新。
    void OnAutoNameTask(CTaskInfo* a_pTask);
    void OnAutoNameDone(CTaskInfo* a_pTask, bool a_bOk, const QString& a_sName);
    void MirrorTaskIfDetached(CTaskInfo* a_pTarget);   // 非活跃副本镜像 task.json+历史库+刷新两表

    // 编排器回调
    void OnStageLog(CTaskInfo* a_pTask, const QString& a_sLine);
    void OnStageEvent(CTaskInfo* a_pTask, const QString& a_sKind, const QString& a_sText);
    void OnTaskChanged(CTaskInfo* a_pTask);
    void OnTaskFinished(CTaskInfo* a_pTask, bool a_bOk);
    // 2026.09.22 评审驳回迭代熔断升级 → 人工介入弹窗（放行/再迭代/保持暂停）。
    void OnReviewEscalated(CTaskInfo* a_pTask);

    // 提示词 agent（面板 ↔ 编排器转发）
    void OnAdhocSubmit(const QString& a_sAsstId, const QString& a_sText, int a_iPerm);
    // 2026-09-22 助手右键「自动重命名」：面板收集的提示词 → 编排器 AutoNameAsstNow；
    //   同步失败（全忙/无会话）也走面板 ApplyAsstAutoName 留事件行。
    void OnAsstAutoRename(const QString& a_sAsstId, const QString& a_sPrompts);
    void OnAdhocUndo(const QString& a_sAsstId);
    void OnAdhocCompact(const QString& a_sAsstId);
    // 2026.08.15 助手消耗归集：一次成功助手调用的费用/输入/输出/耗时/请求数 → 当前档任务。
    void OnAdhocUsage(const QString& a_sAsstId, qint64 a_iIn, qint64 a_iOut, double a_dCost, qint64 a_iElapsedMs, int a_iReqs);

    // FR-3 任务运行中介入：面板介入栏 → 找任务 → 编排器 InjectTaskMessage。
    void OnTaskInject(const QString& a_sTaskId, const QString& a_sText);

    // ask_user MCP：本地管道提问服务（孙进程 MCP server → 本窗口弹窗）
    void OnAskConnection();
    void OnAskReadyRead();
    // 2026-09-26 功能菜单「测试沙箱...」：打开非模态管理对话框。
    void OnSandboxDialog();

    // 标题栏图标按下/标题栏右键 → 弹 Win32 系统菜单（还原/移动/大小/最小化/最大化/关闭）。
    // a_stAt=菜单锚点（屏幕坐标）；传空点 → 自动取图标左下角。
    void OnTitleIconClicked(const QPoint& a_stAt = QPoint());
    void OnMaximizeToggle();     // 最大化/还原翻转（按钮与标题栏双击共用；图标态同源掌管）
    // 系统命令统一入口（图标菜单/任务栏菜单/Alt+F4）：最大化/还原走 OnMaximizeToggle
    // 保持图标态同步；最小化/关闭走 Qt 路径。参数取 SC_*（wParam & 0xFFF0）。
    void OnSysCommand(unsigned int a_uCmd);

private:
    void CreateUi();
    // §8 回退引擎（两入口共用）：残留前置 → 运行守卫 → 破坏性预检 → rollback-pre 快照
    // → RestoreTree → 收尾（任务 Stopped / 对话徽章）+ SetPendingNotice。a_pTask=nullptr 即助手侧。
    void ExecuteRollback(CTaskInfo* a_pTask, const SBackupVersion& a_stVer);
    // 当前助手档 owner（选中任务=任务 id；否则 "global"）。
    QString CurrentAdhocOwner() const;
    void StartAskServer();
    void ActivateAskClient(QLocalSocket* a_pSock);   // 2026-09-12 接管一条提问连接（当前位空时）

    // 2026-10-01 沙箱命令异步化：入队投线程池（MTA COM）→ 结果/超时回 UI 写回管道。
    void EnqueueSbx(QLocalSocket* a_pSock, const QString& a_sCmd, const QJsonObject& a_oArgs);
    void DispatchSbx();                              // 无在飞且队非空 → 投池执行队首
    void OnSbxDone(qint64 a_iSeq, bool a_bOk, const QString& a_sText);   // 池线程完成（queued 回 UI）
    void FinishSbx(qint64 a_iSeq, bool a_bOk, const QString& a_sText);   // 写回+断连+出队
    void OnSbxWatchdog();                           // 5s 检查：150s 未回包 → 错误包出队
    // 2026.08.20 多实例配置同步：config.ini（exe 同级全局）回填运行期静态配置。
    //   a_bStartup=true 仅启动调用（含 DSH/Pi 不可用警告弹窗）；热重载 false 静默。
    void ApplyAgentConfigToRuntime(bool a_bStartup);
    // 2026.09.19 后端切换清会话链（设置保存/热重载/任务档重应用三路统一）：编排器+面板
    //   两侧同步清 sid/撤销栈（后端私有），对话历史/统计保留，状态栏提示。
    void ClearAssistantSessionsOnBackendSwitch();
    // 2026.09.10 公共模型配置：四张模型表（当前设置参数源范围）合并为一份共享表
    //   （列表+当前项，规则见 CModelEntry.h MergeSharedModelTables）后统一应用到
    //   四个 runner——切换后端即用同一份列表/当前项，无需重配。
    void ApplySharedModelsToRunners();
    // 2026.09.09 v2.1.143 任务级配置覆盖：随有效选中任务切换设置参数源（任务档优先/
    //   未选回全局），重应用运行期配置。选中变化、删除/移除任务、切工程时调用。
    void ApplyTaskConfigOverlay(CTaskInfo* a_pTask);
    void OnConfigFileChanged();            // QFileSystemWatcher 触发（外部实例保存）
    void CreateMenus();
    void SetStatus(const QString& a_sText);
    // 迭代熔断（escalation）弹模态对话框：阻塞详情 + 打开任务目录 / 重置迭代并继续 / 忽略。
    void OnEscalation(CTaskInfo* a_pTask, const QString& a_sEventText);
    // 2026.09.07 v2.1.123 多实例工程锁：他实例持锁时弹阻断式确认（默认 No）；
    //   2026.09.09 v2.1.137 a_bAutoRestore 参数已随「启动自动恢复上次工程」移除而删除。
    void OpenProjectDir(const QString& a_sDir);
    // 2026.09.07 标题=工程名-AutoCode 版本（未开工程=AutoCode 版本）：窗口标题供任务
    //   管理器/Alt-Tab 辨认多实例各开的工程；自绘标题栏同步刷左上角标题文本。
    void UpdateWindowTitle();
    void SaveTask(CTaskInfo* a_pTask);   // 持久化任务到 .autodev/tasks/<id>/task.json
    void ClearTasks();                   // 清理内存任务对象（切工程时调用）
    void ReloadHistory();                // 从全局 SQLite 库重载历史任务并刷新历史页
    void SaveAssistantState();           // 序列化助手档 → 选中任务存 tasks/<id>/assistant.json，否则工程级（#1）
    void LoadAssistantState();           // 读对应档 → 恢复助手会话链与对话流；无档显式清空（#1）
    // 2026.08.22 多助手会话换载：编排器单会话 ↔ 面板各助手会话态互转。
    void SyncOrchToPanel(const QString& a_sAsstId);   // 编排器 sid/栈/guideHash → 回填该助手
    void LoadOrchFromPanel(const QString& a_sAsstId); // 该助手会话态 → 换载进编排器
    // 2026.08.22 多助手事件：切助手换载会话；一轮结束回填运行助手并按需换载激活助手。
    void OnAsstSwitched(const QString& a_sOldId, const QString& a_sNewId);
    void OnAsstAdded(const QString& a_sId);
    void OnAsstRemoved(const QString& a_sId, const QString& a_sNewActive);
    void OnAdhocRunFinished(const QString& a_sAsstId);
    // 助手档切换原语（跟着任务走）：停跑 → 旧档落盘 → 切 m_pCur → 恢复新档。
    //   所有改变 m_pCur 的路径必须经此函数，禁止裸赋值（防旧档丢失 / 串档）。
    void SwitchAssistantArchive(CTaskInfo* a_pNewTask);

    // 依据 RecentProjects() 刷新「最近打开」入口态（2026.09.26 面板化：条目内容
    //   由面板在菜单 aboutToShow 时自取，此处只管空历史置灰）。
    void RebuildRecentMenu();
    // OpenProjectDir 成功后调用：记录历史并刷新子菜单。
    void RecordRecent(const QString& a_sDir);

    // 判断几何矩形是否落在任一屏幕的可视区内（屏幕外回退判定）。
    static bool IsGeometryOnScreen(const QRect& a_stGeo);

    // —— FR3 弹窗字体放大辅助 ——
    // 统一构造并执行放大字体的 QMessageBox（正文 + 按钮均设 s_kiMsgPt，避免部分风格按钮不继承）。
    static void ExecMsgBox(QMessageBox::Icon a_eIcon, const QString& a_sTitle,
                           const QString& a_sText, QWidget* a_pParent);
    // 放大字体的 Yes/No 询问框，返回用户选择（语义同 QMessageBox::question 的 Yes|No）。
    static QMessageBox::StandardButton ExecQuestion(const QString& a_sTitle,
                                                    const QString& a_sText,
                                                    QWidget* a_pParent);
    // 弹窗目标字号（FR3，Q13=14pt，全局默认 11pt，+3pt 显著但不夸张）。
    static constexpr int s_kiMsgPt = 14;

    CProjectManager*   m_pPm        = nullptr;
    CTaskOrchestrator* m_pOrch      = nullptr;
    CTaskListPanel*    m_pTaskPanel = nullptr;
    CExecutionPanel*   m_pExecPanel = nullptr;
    // 2026-09-30 手动代码审查对话框（非模态单例，右键资源管理器触发；关闭自动销毁）。
    QPointer<CCodeReviewDialog> m_pReviewDlg;
    // 挂起的助手档切换（原 v2.3.174 挂起的取消选中切档；2026.09.23 v3.0.220 推广口径：
    //   助手运行中点任何目标——空白/任意任务/启动任务——档切换均挂起不停跑助手，
    //   全部助手结束后 OnAdhocRunFinished 自动完成；目标任务经 QPointer 防删悬垂）。
    CTitleBar*         m_pTitleBar  = nullptr;   // FR-3 自绘标题栏
    // 应用级原生消息过滤器（图标/标题栏右键弹系统菜单；比 nativeEvent 更底层，Qt 吞不掉消息）
    CTitleBarMsgFilter* m_pMsgFilter = nullptr;
    QMenuBar*          m_pMenuBar   = nullptr;   // FR-3 菜单下沉到左栏顶部的菜单条
    QSplitter*         m_pMainSplit = nullptr;   // FR-4 左右主分隔条（比例持久化）
    QLabel*            m_pLblStatus = nullptr;

    std::unique_ptr<CTaskHistoryDB> m_pHistoryDb;   // 当前工程历史库（<工程>/.autodev/history.db；2026.08.20 按工程隔离）
    QList<CTaskInfo*>  m_aHistoryTasks;             // 历史页数据源（DB 加载副本，本窗口拥有）
    QList<CTaskInfo*>  m_aTasks;
    CTaskInfo*         m_pCur          = nullptr;
    bool               m_bGuideBuilding = false;
    // 启动任务时 guide.md 缺失：登记该任务，待 guide 生成完成后自动续跑（初始任务）。
    // QPointer：任务对象销毁时自动置空，避免悬垂。
    QPointer<CTaskInfo> m_pTaskPendingGuide;

    QMenu*             m_pMenuRecent = nullptr;
    class CRecentChipsPanel* m_pRecentPanel = nullptr;   // 2026.09.26 按钮面板（内嵌子菜单）

    // ask_user 提问服务：孙进程 MCP server 经此管道发来提问，弹窗作答后回包。
    QLocalServer*      m_pAskServer = nullptr;
    QLocalSocket*      m_pAskClient = nullptr;   // 当前提问连接（一次只弹一个对话框）
    QList<QLocalSocket*> m_aAskQueue;            // 2026-09-12 排队提问连接（多助手并发手工审批；对端断连自动离队）
    QByteArray         m_bufAsk;                 // 提问连接行缓冲
    bool               m_bAskDialogOpen = false; // 2026.09.28 ask 弹窗重入门禁：模态 exec 期间
                                                 //   m_pAskClient 已置空，新连接须排队防嵌套弹窗

    // 2026-09-26 内置测试沙箱：Agent 后代 GUI 窗口自动幽灵化收纳 + MP4 录像 +
    //   sandbox_* 命令通道（MCP 工具与 --sandbox CLI 同桥转发到这里）。
    class CSandboxManager* m_pSbx = nullptr;
    class CSandboxDialog*  m_pSbxDlg = nullptr;   // 非模态管理对话框（功能菜单入口）

    // 2026-10-01 沙箱命令异步化（修「MeloTTS 实例 UI+助手全卡死」）：sbx 命令的
    //   叶子操作（UIA 控件树/PrintWindow/同步点击）可能无限阻塞（hung 窗口、
    //   输入法钩子），旧实现在 UI 线程同步执行 Execute——一旦阻塞 UI 死锁（UIA
    //   的 STA 回调又需 UI 泵消息，循环等待）。改为投线程池（MTA COM，无 STA
    //   泵依赖），结果回 UI 线程写回管道；150s 看门狗保证对端（MCP/CLI 均有
    //   180s 上限）总能得到回包；卡死的池线程泄漏退出（仅极端 hung 场景）。
    struct SSbxReq {
        qint64                iSeq = 0;
        QPointer<QLocalSocket> pSock;      // 结果写回后断连（一连接一命令）
        qint64                iT0Ms = 0;   // 入队时刻（看门狗计时基点）
        QString               sCmd;
        QJsonObject           oArgs;
    };
    QList<SSbxReq>     m_qSbx;             // FIFO：队首=在飞（线程池执行中）
    qint64             m_iSbxSeq = 0;
    QTimer*            m_pSbxWd = nullptr; // 看门狗（5s 检查，150s 超时回错误包）
    // 2026.08.20 多实例配置同步：盯 config.ini（其他实例保存 → 热重载）。
    QFileSystemWatcher* m_pCfgWatcher  = nullptr;
    QTimer*             m_pCfgReloadTm = nullptr;   // 防抖合并（1s 窗口多次触发一次生效）

    // 2026-10-02 助手运行态孤儿看门狗（修「busy 永卡」：结束信号丢失时面板 bRunning
    //   无限残留——图标转不停/输入栏锁「追加指令」）：周期交叉核对面板运行中助手
    //   与编排器 IsAsstAdhocRunning，连续两次不健康即自动恢复并记事件。
    QTimer*             m_pAdhocHealthTm = nullptr;
    QSet<QString>       m_aAdhocSuspect;             // 首次不健康待复确认（防同轮瞬态空档）
    void OnAdhocHealthCheck();
    void OnAsstForeignActivated(const QString& a_sOwnerKey);   // 2026-10-02 幽灵 Tab 点击 → 切回归属
    void OnAsstForeignFinished(const QString& a_sAsstId);      // 2026-10-02 幽灵轮次结束 → 落盘除名
    qint64              m_iCfgSelfSaveMs = 0;       // 自身最近一次保存时刻（ms；<2s 内的变更跳过）
};
