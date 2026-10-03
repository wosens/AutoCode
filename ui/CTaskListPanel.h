#pragma once

#include <QWidget>
#include <QList>

class QToolButton;
class QTabWidget;
class QListWidget;
class QListWidgetItem;
class QTreeWidget;
class QTreeWidgetItem;
class QPushButton;
class QHBoxLayout;
class QPoint;
class QResizeEvent;
class QSplitter;
class CTaskInfo;
class CProjectExplorerPanel;
class QTimer;

// 左侧任务列表面板：用 Tab 分「当前任务」(未终结) 与「历史任务」(全局 SQLite 库，多列)。
// 每行显示 名称[类型]·阶段·状态·创建时间。任务对象所有权在 CMainWindow，本面板仅持弱引用。
//
// 两表数据来源不同：
//   - 当前表(m_pListCur)：本工程内存中的非终态任务，由 SetTasks 填充；
//   - 历史表(m_pTreeHis)：全局 SQLite history.db 的终态任务(Done/Failed)，由 SetHistoryTasks 填充。
class CTaskListPanel : public QWidget {
    Q_OBJECT

public:
    explicit CTaskListPanel(QWidget* a_pParent = nullptr);

    // 设置「当前任务」表（本工程非终态任务）。
    void SetTasks(const QList<CTaskInfo*>& a_aTasks);
    // 2026.08.15 切工程：清空两表并复位 m_pCur——旧任务对象即将 delete，若不复位，
    //   悬垂指针可能命中回收地址误匹配新任务（幽灵选中），且 UpdateButtons 每次解引用。
    void ClearAll();
    // 设置「历史任务」表（过滤由 CMainWindow::ReloadHistory 完成，面板直接展示传入列表）。
    void SetHistoryTasks(const QList<CTaskInfo*>& a_aTasks);
    void RefreshTask(CTaskInfo* a_pTask);   // 状态变化时重建当前表（任务可能迁出当前表）
    // 2026.08.15 原地刷新指定任务在两表中的行（当前表整行文本；历史表费用/Token/耗时列），
    //   不重建列表、不动选中——供助手消耗归集/历史耗时回填等轻量数值更新。
    void UpdateTaskRow(CTaskInfo* a_pTask);
    // 在当前表查找并选中指定任务（启动任务时自动选中，使其日志立即显示）。
    void SelectTask(CTaskInfo* a_pTask);
    void DeselectAll();   // 2026-10-02 程序化取消选中（走正常信号链；幽灵回工程档用）
    // 2026.09.10 v2.2.153 在历史表查找并选中指定任务：任务终态归档后调用，切「历史任务」
    //   页并选中刚结束的任务（发选中信号 → 显示其任务日志，与 SelectTask 对称）。
    void SelectHistoryTask(CTaskInfo* a_pTask);
    CTaskInfo* CurrentTask() const { return m_pCur; }

    // FR-3：暴露顶部菜单条容器布局，供 CMainWindow::CreateMenus 注入「工程(&P)」「功能(&F)」菜单。
    // 菜单下沉到左栏顶部，替代原窗口顶部 menuBar()。
    QHBoxLayout* MenuBarLayout() { return m_pMenuLayout; }

    // FR-3：切换工程资源管理器根目录（OpenProjectDir 成功后调用）；空串=清空树。
    void SetProjectRoot(const QString& a_sDir);

    // 2026.09.23 资源管理器显示隐藏目录下发（设置保存/热重载/启动应用；透传给面板）。
    void SetExplorerHiddenDirs(const QStringList& a_aNames);

    // 2026.08.18 任务列表高度分隔条：上段(任务Tab+按钮行)/资源管理器之间的垂直 QSplitter
    //   状态存取（CMainWindow 在启动时恢复、关闭时保存，经 CProjectManager 落 [Layout] VSplit）。
    QByteArray VSplitState() const;
    // 2026.08.29 历史任务表列宽（10 列整型串）与左栏 Tab 页（0=当前任务/1=历史任务）
    //   持久化（[Layout] TaskCols/TaskTab；重启不再回默认列宽/当前任务页）。
    QString TaskColsState() const;
    void RestoreTaskCols(const QString& a_sState);
    int TaskTabIndex() const;
    void RestoreTaskTab(int a_iIndex);
    void RestoreVSplitState(const QByteArray& a_stState);

signals:
    void sigNewTask();
    void sigStartTask(CTaskInfo* a_pTask);
    void sigPauseTask(CTaskInfo* a_pTask);
    void sigStopTask(CTaskInfo* a_pTask);
    void sigDeleteTask(CTaskInfo* a_pTask);
    void sigTaskSelected(CTaskInfo* a_pTask);
    void sigReviveTask(CTaskInfo* a_pTask);   // 失败任务加回当前栏续跑（#6）
    void sigRemoveTask(CTaskInfo* a_pTask);   // 当前任务移除到历史（#3）
    void sigViewTask(CTaskInfo* a_pTask);     // FR-5 右键「查看任务内容」→ 只读展示
    void sigRenameTask(CTaskInfo* a_pTask);   // 2026.08.20 右键「重命名」→ 主窗口弹输入框手动改任务名
    void sigAutoNameTask(CTaskInfo* a_pTask); // 2026.08.20 右键「自动命名」→ 复用新建任务自动命名逻辑
    // FR-6：停止按钮右侧「回退」按钮 → 主窗口弹历史版本对话框。
    void sigRollback(CTaskInfo* a_pTask);
    // FR-3：资源管理器「添加到聊天框」→ 注入 adhoc 助手输入框（Q1 默认）。
    void sigAddToChat(const QString& a_sText, const QString& a_sKind);
    // 2026.08.18：资源管理器双击文本文件 → 执行面板「助手」右侧 Tab 内嵌显示（转发）。
    //   2026.08.21 增加切页标志：双击=true 切页；单击=false 只加载内容不切页。
    void sigFileOpen(const QString& a_sAbsPath, bool a_bSwitchTab);
    // 2026-09-30 代码审查（ReviewSdk）：资源管理器右键 → 主窗口弹审查对话框（转发）。
    void sigCodeReview(const QString& a_sAbsPath);
    // FR-9：任务列表（Tab）底部相对本 widget 顶部的 y 变化 → 供执行面板同步阶段表高度，
    //       使「阶段表底部」与「任务列表底部」（不含按钮行）对齐，随字体/DPI 自适应。
    //       2026.08.19 仅窗口 resize 路径上报；左栏分隔条拖动不上报（各拖动条互不影响）。
    void sigTaskListBottom(int a_iBottomY);

protected:
    bool eventFilter(QObject* a_pWatched, QEvent* a_pEvent) override;   // 粘性选择（#5）
    void resizeEvent(QResizeEvent* a_pEvent) override;   // FR-9 上报任务列表底部 y

private slots:
    void OnSelectionChanged(QListWidgetItem* a_pCur, QListWidgetItem* a_pPrev);          // 当前表
    void OnHistorySelectionChanged(QTreeWidgetItem* a_pCur, QTreeWidgetItem* a_pPrev);   // 历史表
    void OnElapsedTimer();   // 2026.08.15 运行中每秒刷新当前表选中项的耗时文本

private:
    void PopulateCurrent();                   // 重建当前表（只放非终态）
    void RestoreSelection(CTaskInfo* a_pTarget);   // 重建后在两表中找回指定任务选中
    void UpdateButtons();                     // 按当前任务状态刷新启动/暂停/停止按钮
    // 2026.08.17 悬垂对账：m_pCur 不在两表数据源里（任务对象已被 CMainWindow delete）则置空。
    //   各 Set* 重建是删除后唯一的同步通知点，必须在此拦住，否则 1s 定时器/按钮刷新会
    //   解引用已释放内存（d7f99cac 01:32:53 崩溃：OnElapsedTimer 读 m_pCur->Status()）。
    void ReconcileCur();
    static QString FormatTokens(qint64 a_iTokens);  // K/M/G 缩写
    // 2026.08.18 费用格式化：≤0 显示 "-"（DSH 后端无计费恒 0；与请求/耗时列缺失值风格一致）。
    static QString FormatCost(double a_dCost);
    // 2026.08.15 当前表行文本：名称 [类型] · 阶段 · 状态 [· 耗时] · 创建时间（PopulateCurrent 与秒级刷新共用）。
    static QString MakeTaskText(const CTaskInfo* a_pTask);
    static CTaskInfo* TaskOf(QListWidgetItem* a_pItem);
    static CTaskInfo* TaskOf(QTreeWidgetItem* a_pItem);
    void OnHistoryContextMenu(const QPoint& a_pos);   // 历史表右键「再次执行」（#6）
    void OnCurrentContextMenu(const QPoint& a_pos);   // 当前表右键「移除」（#3）
    // FR-9：上报任务列表（Tab）底部 y（m_pTab 布局后 geometry 才准，height<=0 跳过）。
    //   2026.08.19 只剩 resizeEvent 与 RestoreVSplitState 收尾两个调用点；左栏分隔条
    //   拖动不再上报（否则右栏阶段/日志分隔条被连带拖动，见 ctor 注释）。
    void ReportTaskListBottom();

    QTabWidget*   m_pTab      = nullptr;
    QSplitter*    m_pVSplit   = nullptr;   // 2026.08.18 任务列表高度分隔条（上=Tab+按钮行，下=资源管理器）
    QHBoxLayout*  m_pMenuLayout = nullptr;  // FR-3 顶部菜单条容器（供主窗口注入菜单）
    QListWidget*  m_pListCur  = nullptr;   // 当前任务（未终结：Pending/Running/Paused/Stopped）
    QTreeWidget*  m_pTreeHis  = nullptr;   // 历史任务（全局 DB，终态：Done/Failed，多列）
    CProjectExplorerPanel* m_pExplorer = nullptr;   // FR-3 工程资源管理器（占列表下方剩余空间）
    QPushButton*  m_pBtnNew   = nullptr;
    QPushButton*  m_pBtnStart = nullptr;
    QPushButton*  m_pBtnPause = nullptr;
    QPushButton*  m_pBtnStop  = nullptr;
    QPushButton*  m_pBtnRollback = nullptr;   // FR-6 停止按钮右侧「回退」
    // 2026.08.22 「回退」右侧▼下拉已删（与「回退」同信号功能重复）
    QList<CTaskInfo*> m_aTasks;            // 当前表数据源（本工程非终态）
    QList<CTaskInfo*> m_aHis;              // 2026.08.17 历史表数据源副本（悬垂对账用）
    QTimer*       m_pElapsedTimer = nullptr;   // 2026.08.15 运行耗时 1s 刷新（当前任务 Running 时跑）
    CTaskInfo*    m_pCur      = nullptr;
    bool          m_bAllowDeselect = false;  // 粘性选择：仅空白点击才允许取消（#5）
    bool          m_bSyncing  = false;        // 互斥同步中（程序清对方表防重入）
};
