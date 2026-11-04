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
        // 2026-11-02 物理输入拦截：Agent 运行期间吞掉合成鼠标/键盘事件
        //   （SendInput/pyautogui 等带 INJECTED 标志）——提示词铁律只能约束
        //   自觉的模型，机制性拦截才能根治“Agent 抢占用户鼠标键盘”。
        bool bBlockSynth = true;   // [Sandbox] BlockSynth 配置开关（默认开）
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
//   2026-10-14 断链兜底（Compositer 工程实测「测试窗口一直弹出」根因）：Agent 用
//   powershell Start-Process / cmd start 等中间人方式启动被测程序，中间 shell
//   执行完立即退出，被测程序主窗口弹出时父进程已死——IsDescendantOf 沿 ppid
//   爬链第一跳即断，TreePid 判否 → 不收纳，窗口正常可见弹到用户桌面还抢焦点。
//   双对策：① ConsiderWindowImpl 树判定失败后按「exe 在工程根内、进程新建且
//   父已死」兑底收纳（IsProjectTestPid；2026-10-16 条件③收紧：父活着（explorer
//   双击/QuickTool/托盘等任意启动器）一律不兑底，修用户经 QuickTool 打开
//   HarnessAgent 被误幽灵化「窗口消失」）；② 新增 sandbox launch 命令
//   （LaunchAut）：主进程亲自 CreateProcess + AddManualRootPid 登记根，进程链
//   永不断（提示词铁律同步禁断链启动方式）。
//   2026-10-26 出生登记（QuickTool 工程实测「%TEMP% 副本测试窗口裸弹」根因）：
//   Agent 把被测 exe 拷到工程外（%TEMP%\dsh-verify51 等）再经中间 shell 启动——
//   ①工程根兑底认不出（exe 不在根内），②中间 shell 退出断链后进程树判定也
//   认不出，两道全破 → 窗口裸弹用户桌面（QuickTool 本身置顶磁吸，格外扰人）。
//   对策 OnBirthScan（250ms）：趁链还活着（Agent 脚本 Start-Process 后多带
//   Start-Sleep，链活数秒）就发现新出生进程，沿 ppid 爬链到本进程的登记进
//   m_aAgentBorn（出生登记表）；链断后窗口弹出，ConsiderWindowImpl 按登记
//   兜底收纳（IsAgentBornPid，与树内同零宽限直收）。直接子（父=本进程）不
//   登记——那是用户经 UI ShellExecuteEx 启动的形态（与宽限期启发同口径）。
//   exe 位置不再限定工程根：Agent 拷到哪里启动都能收住。
    void SetProjectRoot(const QString& a_sRoot);   // 打开/切换工程时刷新
    void ReadoptProjectWindows();                  // 扫描带标记窗口，工程内的续收纳

    // 出生登记查询（诊断/测试用）：进程是否在链活期间被登记为 Agent 后代。
    bool IsAgentBornPid(DWORD a_dwPid) const;
    // 出生登记 janitor 兑底宽限（仅测试用）：默认 10 分钟（根 runner 退出后，
    //   其遗留被测/拉起进程超时自动终结——QuickTool 实测记事本残留不清理）。
    void SetBornJanitorGraceForTest(int a_iMs) { m_iBornJanitorGraceMs = a_iMs; }

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
    void OnBirthScan();      // 出生登记扫描（250ms）：链活期间发现 Agent 后代新进程
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
    // 2026-10-14 断链兑底判定：树外窗口进程 exe 在当前工程根目录内、进程为新建
    //   （<10 分钟）且父进程已死（断链；2026-10-16 收紧：父活着=用户经任意启动器
    //   主动打开，不兑底）→ 视为 Agent 启动的被测工程产物。须在 TreePid 之后调
    //   （复用其刚刷新的 pid→创建时刻表；自带 500ms 防陈旧重建守卫）。
    bool IsProjectTestPid(DWORD a_dwPid);
    // 2026-11-02 %TEMP% 产物兑底（QuickTool 实害链收口）：Agent 把被测 exe 拷到
    //   系统临时目录/exe 同级 ~Temperary 再经中间 shell 启动——IsProjectTestPid
    //   （exe 须在工程根内）与出生登记（中间人秒退 <250ms 漏逮）双双落空，窗口
    //   裸弹用户桌面。四重限定压误伤：① Agent 活跃（agent_active 置位，空闲期
    //   用户自己跑临时程序不收）；② exe 在 %TEMP% 或 ~Temperary 下（正常软件
    //   绝不装这里）；③ 父进程已死（explorer 双击等用户启动父活着）；④ 进程
    //   新建 <10 分钟（陈旧残留视为用户手动运行）。
    bool IsTempArtifactPid(DWORD a_dwPid);
    // 2026-10-14 sandbox launch：主进程 CreateProcess 启动被测程序并登记手工根。
    bool LaunchAut(const QJsonObject& a_oArgs, QString& a_sOut);
    void RestoreEntriesOfPid(DWORD a_dwPid);               // 登记豁免时回扫还原（兑底网）
    void RestoreEntriesOfPidLocked(DWORD a_dwPid);         // 持锁实现（tryLock 外壳内部用）
    void GhostAndTrack(HWND a_hWnd);
    bool TreePid(DWORD a_dwPid);           // 属本进程树（或手工根）
    DWORD ParentOfPid(DWORD a_dwPid) const;   // 2026-10-26 pid 父查询（锁内读表）
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
    // 2026-11-02 物理输入拦截：宿主 agent_active 命令置位（助手/任务任一在跑），
    //   与 cfg.bBlockSynth 相与后写全局原子（钩子线程读，回调内只查标志）。
    bool                       m_bAgentActive = false; // 宿主告知的 Agent 运行态
    QTimer                     m_tSweep;
    QTimer                     m_tBirth;              // 出生登记扫描器（2026-10-26）
    QHash<DWORD, qint64>       m_aAgentBorn;          // pid→创建时刻（出生登记表）
    QHash<DWORD, DWORD>        m_aBornRoot;           // pid→根 runner pid（出生归属，2026-10-26 janitor）
    int                        m_iBornJanitorGraceMs = 10 * 60 * 1000;  // 根死后遗留兑底宽限（测试可缩短）
    QSet<DWORD>                m_aSeenPids;           // 上轮全量 pid（差分找新增）
    mutable QMutex             m_mtxBorn;             // m_aAgentBorn 跨线程（UI写/钩子读）
    QHash<DWORD, DWORD>        m_oParentMap;           // pid→ppid 快照（500ms 缓存）
    QHash<DWORD, qint64>        m_oParentCreated;       // 2026-10-14 pid→创建时刻（PID 复用防护，见 CSandboxCore.h）
    qint64                     m_iMapBuiltMs = 0;
    mutable QMutex             m_mtxPidMap;            // 2026-10-26 并发写防护：钩子/UI/工作线程共写 pid 表（QHash 并发写会损坏结构死循环）
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
