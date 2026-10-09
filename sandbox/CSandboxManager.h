#pragma once

// 内置测试沙箱管理器（2026-09-26，主实例内）。
//   ① 自动收纳：WinEvent 钩子（EVENT_OBJECT_SHOW）+ 周期扫描，凡属本 AutoCode
//      进程树后代（Agent runner → cmd → 测试程序链）新出现的顶层窗口自动幽灵化
//      （alpha=1 + 点击穿透 + 无任务栏，窗口持续重绘可截图/可投递消息）——用户无感；
//   ② 自动录像：每收纳一个窗口即起 CSandboxRecorder（MP4/H264，带时间戳与点击标记）；
//   ③ 命令分派 Execute()：claude 的 MCP 工具（mcp__autocode__sandbox_*）与其他后端的
//      --sandbox CLI 的统一入口（主窗口 QLocalServer 收 {type:sbx} 转发到这里）。
//   2026-10-01 线程模型（修「沙箱命令卡死整个 UI」）：Execute 的叶子操作（UIA 控件树/
//      PrintWindow 截图/SendMessage 同步点击等）可能无限阻塞（ hung 窗口、输入法钩子），
//      主窗口改为投线程池（MTA COM）调 Execute——故 m_aEntries/录像/还原等状态用
//      m_mtxState 串行化；UI 线程路径（OnSweep/ConsiderWindow/Rows）一律 tryLock 短超时
//      跳过/读缓存，永不在 UI 线程上等这把锁。
// 与 CProjectManager 解耦：配置经 SSbxCfg 值对象注入（主窗口负责读 ini），
// 便于单元测试独立构造。

#include <QObject>
#include <QTimer>
#include <QHash>
#include <QSet>
#include <QMutex>
#include <QPair>
#include <QJsonObject>
#include <windows.h>

class CSandboxRecorder;

class CSandboxManager : public QObject {
    Q_OBJECT

public:
    // 配置值对象（主窗口从 [Sandbox] 节读出后 SetConfig 注入）。
    struct SSbxCfg {
        bool bEnable    = true;    // [Sandbox] Enable     总开关
        bool bAutoGhost = true;    // [Sandbox] AutoGhost  自动收纳新窗口
        bool bRecord    = true;    // [Sandbox] Record     自动录像
        int  iFps       = 5;       // [Sandbox] Fps        录制帧率 1..15
        int  iRetainDays = 7;      // [Sandbox] RetainDays 录像保留天数 1..365
    };

    explicit CSandboxManager(QObject* a_pParent = nullptr);
    ~CSandboxManager() override;

    void SetConfig(const SSbxCfg& a_stCfg);
    void Start();            // 孤儿处理（续收纳/还原）+ 录像保留清理 +（启用时）钩子/扫描
    void Shutdown();         // 停钩子/扫描，全部还原 + 停录像（退出前必调）
    void ReloadConfig();     // 设置保存后热应用（开/停钩子）

    // 2026-10-08 跨实例续收纳：AutoCode 重启（版本更新/用户重开）后，上个实例
    //   收纳中的测试进程窗口（带幽灵标记残留、进程未退出）会被退出还原重新可见，
    //   而本实例的进程树出身判定认不出「已退出实例的后代」→ 永不再收纳，用户桌面
    //   被测试窗口打扰（Compositer 工程实测：16:14 实例重启后窗口一直裸奔）。
    //   对策：窗口进程 exe 在当前工程目录内 → 重新入册保持隐形；无关残留照旧还原。
    void SetProjectRoot(const QString& a_sRoot);   // 打开/切换工程时刷新
    void ReadoptProjectWindows();                  // 扫描带标记窗口，工程内的续收纳

    // 钩子线程句柄（Start 时创建；无 Q_OBJECT 的内部类经指针直调 StopHook）。
    class CSbxHookThread* m_pHookTh = nullptr;

    // 续收纳判定用：当前工程根（空 = 未开工程，孤儿一律还原）。
    QString m_sProjRoot;

    // MCP/CLI 统一命令入口（cmd 小写）。成功返回 true，a_sOut 为多行文本。
    //   阻塞锁（可在工作线程长阻塞；UI 线程勿直调，用 ExecuteTry）。
    bool Execute(const QString& a_sCmd, const QJsonObject& a_oArgs, QString& a_sOut);
    // UI 线程专用：tryLock 短超时——沙箱忙（助手命令在执行）时立即返回友好提示，
    //   永不阻塞 UI（沙箱对话框手动操作用）。
    bool ExecuteTry(const QString& a_sCmd, const QJsonObject& a_oArgs, QString& a_sOut);

    // 沙箱窗口表（管理对话框刷新用）。
    struct SRow {
        HWND    h = nullptr;
        DWORD   dwPid = 0;
        QString sExe, sTitle;
        qint64  iGhostAtMs = 0;
        QString sRecPath;             // 首段录像路径（空=未录）
        bool    bRecording = false;
    };
    QList<SRow> Rows() const;
    QString RecordDir() const;
    QString ShotDir() const;
    bool IsEnabled() const { return m_stCfg.bEnable; }
    bool IsRecordingOn() const { return m_bRecOn; }
    int  Fps() const { return m_stCfg.iFps; }
    int  GhostedCount() const { QMutexLocker oL(&m_mtxCache); return m_aRowsCache.size(); }

    // 显式收纳（--sandbox add / 单元测试）：不做出身判定，仅做形状判定。
    bool AddWindow(HWND a_hWnd);
    // 还原并停录像、摘表。
    bool RestoreWindowEx(HWND a_hWnd);
    void AddManualRootPid(DWORD a_dwPid) { m_aManualRoots.insert(a_dwPid); }

    // 2026.09.26 用户启动豁免（进程级登记表）：UI（资源管理器单击/双击、打开方式）
    //   主动启动的程序不属于 Agent 被测对象——ShellExecuteEx 拿到新进程句柄后调此
    //   登记（句柄所有权移交；进程退出即除名，防 pid 复用误豁免）。命中登记根
    //   （自身或其子进程）的新窗口不收纳不录像；若登记前已被收纳（ShellExecuteEx
    //   内部泵消息致钩子判定先行的竞态）则立即还原停录像。Agent 启动的测试程序
    //   不经此登记，照常收纳录像。
    static void RegisterUserRoot(DWORD a_dwPid, HANDLE a_hProc);
    // 测试用：清空豁免登记表（prod 不调；进程退出句柄随终）。
    static void ClearUserRoots();

signals:
    void sigChanged();       // 窗口表/录像状态变化（对话框刷新）

private slots:
    void OnSweep();          // 周期扫描：补收纳 + 条目健康检查
    void OnRecFinished();    // 录像线程收尾（deleteLater）

public:
    // 钩子/扫描候选窗口的完整判定 + 收纳（public：钩子线程匿名命名空间 lambda 跨
    //   线程投递调用，无友元可达路径）。
    //   2026.09.27 收纳改宽限期：判定通过后延迟 kGhostGraceMs 复判再幽灵化——
    //   ShellExecuteEx 内部可能泵消息，用户点击启动的进程窗口可能在豁免登记
    //   （OpenWithShell 拿 pid 之前）就触发钩子判定，宽限期内的复判能看到登记。
    //   返回是否已（立即）收纳；宽限排队返回 false。
    bool ConsiderWindow(HWND a_hWnd, bool a_bFinal = false);

private:
    bool ConsiderWindowImpl(HWND a_hWnd, bool a_bFinal);   // a_bFinal=true=宽限期满终判
    void RestoreEntriesOfPid(DWORD a_dwPid);               // 登记豁免时回扫还原（兑底网）
    void RestoreEntriesOfPidLocked(DWORD a_dwPid);         // 持锁实现（tryLock 外壳内部用）
    void GhostAndTrack(HWND a_hWnd);
    bool TreePid(DWORD a_dwPid);           // 属本进程树（或手工根）
    HWND ArgHwnd(const QJsonObject& a_oArgs, bool* a_pbOk) const;
    CSandboxRecorder* StartRecorder(HWND a_hWnd, DWORD a_dwPid, const QString& a_sExe,
                                    QString* a_psPathOut);
    void StopRecorderOf(int a_iIdx);
    // 用户启动豁免登记表（进程级静态；RegisterUserRoot 入表）。
    static QList<QPair<DWORD, HANDLE>>& UserRoots();
    static QMutex& UserRootsMtx();
    static void PruneUserRoots();          // 退出的登记根除名（关句柄）

    // 2026-10-01 并发模型：m_aEntries 等状态被工作线程（Execute）、UI 线程（扫描/
    //   对话框）、钩子线程（ConsiderWindow）交叉访问——m_mtxState 串行化；
    //   Rows()/GhostedCount() 返回 m_mtxCache 保护下的快照（UI 无锁等待）。
    mutable QMutex             m_mtxState;
    mutable QMutex             m_mtxCache;
    QList<SRow>                m_aRowsCache;      // Rows() 快照（状态变更时重建）

private:
    SSbxCfg                    m_stCfg;
    bool                       m_bStarted = false;
    bool                       m_bRecOn = true;        // 运行期录像开关（record_on/off）
    QTimer                     m_tSweep;
    QHash<DWORD, DWORD>        m_oParentMap;           // pid→ppid 快照（500ms 缓存）
    qint64                     m_iMapBuiltMs = 0;
    QSet<DWORD>                m_aManualRoots;         // 测试/特殊场景手工根

    struct SEntry {
        HWND               h = nullptr;
        DWORD              dwPid = 0;
        QString            sExe, sTitle;
        qint64             iGhostAtMs = 0;
        CSandboxRecorder*  pRec = nullptr;   // 无父对象：finished 后 deleteLater
        QString            sRecPath;
    };
    QList<SEntry>              m_aEntries;
    QSet<quintptr>             m_aPendingGhosts;   // 宽限期待定窗口（去重，见 ConsiderWindow）

    void RebuildCacheLocked();   // 持 m_mtxState 时调用：重建 Rows 快照
    bool ExecuteLocked(const QString& a_sCmd, const QJsonObject& a_oArgs, QString& a_sOut);
};
