#pragma once

#include <QMainWindow>
#include <QList>
#include <QRect>
#include <QPointer>
#include <QMessageBox>
#include <memory>

class CProjectManager;
class CTaskOrchestrator;
class CTaskListPanel;
class CExecutionPanel;
class CTaskHistoryDB;
class CTaskInfo;
class CTitleBar;
class CTitleBarMsgFilter;
class QMenuBar;
class QSplitter;
class QLabel;
class QMenu;
class QCloseEvent;
class QShowEvent;
class QLocalServer;
class QLocalSocket;

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
    // 工程菜单
    void OnNewProject();
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

    // 帮助菜单（FR-9）
    void OnHelp();
    void OnAbout();

    // 最近打开
    void OnRecentProject();        // 点子菜单内具体历史项
    void OnOpenAllRecent();        // 点「...」→ 开全量对话框

    // 任务列表
    void OnNewTask();
    void OnStartTask(CTaskInfo* a_pTask);
    void OnPauseTask(CTaskInfo* a_pTask);
    void OnStopTask(CTaskInfo* a_pTask);
    void OnRollback(CTaskInfo* a_pTask);   // FR-6：弹历史版本对话框 → 原子恢复源码 → 任务置 Stopped
    void OnDeleteTask(CTaskInfo* a_pTask);
    void OnRemoveTask(CTaskInfo* a_pTask);   // 当前任务移除到历史（保留记录，可再次执行）（#3）
    void OnReviveTask(CTaskInfo* a_pTask);   // 失败任务加回当前栏续跑（#6）
    void OnTaskSelected(CTaskInfo* a_pTask);

    // 查看任务（FR-5）：右键「查看任务内容」→ 只读对话框
    void OnViewTask(CTaskInfo* a_pTask);

    // 编排器回调
    void OnStageLog(CTaskInfo* a_pTask, const QString& a_sLine);
    void OnStageEvent(CTaskInfo* a_pTask, const QString& a_sKind, const QString& a_sText);
    void OnTaskChanged(CTaskInfo* a_pTask);
    void OnTaskFinished(CTaskInfo* a_pTask, bool a_bOk);

    // 提示词 agent（面板 ↔ 编排器转发）
    void OnAdhocSubmit(const QString& a_sText, bool a_bReadOnly);
    void OnAdhocUndo();
    void OnAdhocCompact();
    // 2026.08.15 助手消耗归集：一次成功助手调用的费用/输入/输出/耗时/请求数 → 当前档任务。
    void OnAdhocUsage(qint64 a_iIn, qint64 a_iOut, double a_dCost, qint64 a_iElapsedMs, int a_iReqs);

    // FR-3 任务运行中介入：面板介入栏 → 找任务 → 编排器 InjectTaskMessage。
    void OnTaskInject(const QString& a_sTaskId, const QString& a_sText);

    // ask_user MCP：本地管道提问服务（孙进程 MCP server → 本窗口弹窗）
    void OnAskConnection();
    void OnAskReadyRead();

    // 标题栏图标按下/标题栏右键 → 弹 Win32 系统菜单（还原/移动/大小/最小化/最大化/关闭）。
    // a_stAt=菜单锚点（屏幕坐标）；传空点 → 自动取图标左下角。
    void OnTitleIconClicked(const QPoint& a_stAt = QPoint());
    void OnMaximizeToggle();     // 最大化/还原翻转（按钮与标题栏双击共用；图标态同源掌管）
    // 系统命令统一入口（图标菜单/任务栏菜单/Alt+F4）：最大化/还原走 OnMaximizeToggle
    // 保持图标态同步；最小化/关闭走 Qt 路径。参数取 SC_*（wParam & 0xFFF0）。
    void OnSysCommand(unsigned int a_uCmd);

private:
    void CreateUi();
    void StartAskServer();
    void CreateMenus();
    void SetStatus(const QString& a_sText);
    // 迭代熔断（escalation）弹模态对话框：阻塞详情 + 打开任务目录 / 重置迭代并继续 / 忽略。
    void OnEscalation(CTaskInfo* a_pTask, const QString& a_sEventText);
    void OpenProjectDir(const QString& a_sDir);
    void SaveTask(CTaskInfo* a_pTask);   // 持久化任务到 .autodev/tasks/<id>/task.json
    void ClearTasks();                   // 清理内存任务对象（切工程时调用）
    void ReloadHistory();                // 从全局 SQLite 库重载历史任务并刷新历史页
    void SaveAssistantState();           // 序列化助手档 → 选中任务存 tasks/<id>/assistant.json，否则工程级（#1）
    void LoadAssistantState();           // 读对应档 → 恢复助手会话链与对话流；无档显式清空（#1）
    // 助手档切换原语（跟着任务走）：停跑 → 旧档落盘 → 切 m_pCur → 恢复新档。
    //   所有改变 m_pCur 的路径必须经此函数，禁止裸赋值（防旧档丢失 / 串档）。
    void SwitchAssistantArchive(CTaskInfo* a_pNewTask);

    // 依据 RecentProjects() 重建 m_pMenuRecent 的 QAction。
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
    CTitleBar*         m_pTitleBar  = nullptr;   // FR-3 自绘标题栏
    // 应用级原生消息过滤器（图标/标题栏右键弹系统菜单；比 nativeEvent 更底层，Qt 吞不掉消息）
    CTitleBarMsgFilter* m_pMsgFilter = nullptr;
    QMenuBar*          m_pMenuBar   = nullptr;   // FR-3 菜单下沉到左栏顶部的菜单条
    QSplitter*         m_pMainSplit = nullptr;   // FR-4 左右主分隔条（比例持久化）
    QLabel*            m_pLblStatus = nullptr;

    std::unique_ptr<CTaskHistoryDB> m_pHistoryDb;   // 全局历史库（exe 同级 history.db）
    QList<CTaskInfo*>  m_aHistoryTasks;             // 历史页数据源（DB 加载副本，本窗口拥有）
    QList<CTaskInfo*>  m_aTasks;
    CTaskInfo*         m_pCur          = nullptr;
    bool               m_bGuideBuilding = false;
    // 启动任务时 guide.md 缺失：登记该任务，待 guide 生成完成后自动续跑（初始任务）。
    // QPointer：任务对象销毁时自动置空，避免悬垂。
    QPointer<CTaskInfo> m_pTaskPendingGuide;

    QMenu*             m_pMenuRecent = nullptr;
    // 子菜单显示上限（可调常量，FR-4.1）。
    static constexpr int s_kiMaxRecentMenu = 10;

    // ask_user 提问服务：孙进程 MCP server 经此管道发来提问，弹窗作答后回包。
    QLocalServer*      m_pAskServer = nullptr;
    QLocalSocket*      m_pAskClient = nullptr;   // 当前提问连接（串行，一次一个）
    QByteArray         m_bufAsk;                 // 提问连接行缓冲
};
