// 内置测试沙箱管理器实现（说明见同名 .h）。
#include "CSandboxManager.h"
#include "CSandboxCore.h"
#include "CSandboxRecorder.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QThread>
#include <QCoreApplication>
#include <QPointer>
#include <atomic>
#include <vector>

// —— WinEvent 钩子线程：EVENT_OBJECT_SHOW 到达即把候选 HWND 投递回主线程 ——
// SetWinEventHook(OUTOFCONTEXT) 要求安装线程跑消息循环；钩子回调无用户参数，
// 用文件内静态指针路由到管理器（单实例；多 AutoCode 实例各持各的进程内指针）。
// 2026.09.28 改原子指针：主线程 Start/Shutdown 写、钩子线程 HookProc 读原本无同步，
//   管理器析构期间钩子线程可读到旧指针对销毁中的对象 invokeMethod（use-after-free 窗口）。
static std::atomic<CSandboxManager*> g_pSbxMgrForHook { nullptr };

class CSbxHookThread : public QThread {
public:
    using QThread::QThread;
    std::atomic<DWORD> m_dwTid { 0 };

    void StopHook() {
        const DWORD dwT = m_dwTid.load();
        if (dwT != 0) {
            PostThreadMessageW(dwT, WM_QUIT, 0, 0);
        }
    }

protected:
    void run() override {
        // 2026.09.28 先强制建消息队列（PeekMessage PM_NOREMOVE）再发布 tid：否则
        //   StopHook 可能在队列创建前 PostThreadMessageW（投递失败丢 WM_QUIT，线程永不退出）。
        MSG stMsgPre;
        PeekMessageW(&stMsgPre, nullptr, WM_USER, WM_USER, PM_NOREMOVE);
        m_dwTid = GetCurrentThreadId();
        HWINEVENTHOOK hHook = SetWinEventHook(
            EVENT_OBJECT_SHOW, EVENT_OBJECT_SHOW, nullptr, &CSbxHookThread::HookProc,
            0, 0, WINEVENT_OUTOFCONTEXT);
        MSG stMsg;
        while (GetMessageW(&stMsg, nullptr, 0, 0) > 0) {
            TranslateMessage(&stMsg);
            DispatchMessageW(&stMsg);
        }
        if (hHook != nullptr) {
            UnhookWinEvent(hHook);
        }
        m_dwTid = 0;
    }

    static void CALLBACK HookProc(HWINEVENTHOOK, DWORD, HWND a_hWnd,
                                  LONG a_lIdObject, LONG a_lIdChild, DWORD, DWORD) {
        if (a_lIdObject != OBJID_WINDOW || a_lIdChild != CHILDID_SELF) {
            return;
        }
        CSandboxManager* pMgr = g_pSbxMgrForHook.load(std::memory_order_acquire);
        if (pMgr == nullptr) {
            return;
        }
        const quintptr hVal = reinterpret_cast<quintptr>(a_hWnd);
        // 2026-10-08 崩溃修复（Compositer 实测 ACCESS_VIOLATION）：原先 postEvent 的
        //   receiver=管理器对象——原子指针只保证指针读取本身原子，load 与
        //   invokeMethod 之间若管理器销毁/析构推进，钩子线程仍会踩悬垂对象
        //   （Qt lockThreadPostEventList 读悬垂 threadData → 访问违例 0xFFFFFFFFFFFFFFFF，
        //   dump 实证）。改为投递到 qApp（进程内永活、threadData 稳定）：postEvent
        //   阶段不可能悬垂；lambda 在主线程执行时与析构天然串行，二次 atomic 复查
        //   丢弃过期窗口（Shutdown/换实例后的迟到窗口）。
        QMetaObject::invokeMethod(QCoreApplication::instance(), [pMgr, hVal]() {
            if (g_pSbxMgrForHook.load(std::memory_order_acquire) != pMgr) {
                return;   // 已 Shutdown/实例已替换：窗口事件过期丢弃
            }
            pMgr->ConsiderWindow(reinterpret_cast<HWND>(hVal));
        }, Qt::QueuedConnection);
    }
};

// ---------------------------------------------------------------------------
// 2026.09.26 用户启动豁免登记表（进程级静态）：UI 主动 ShellExecuteEx 启动的
//   程序（资源管理器单击/双击、打开方式）登记 pid，ConsiderWindow 对命中根
//   （自身或子进程）的新窗口不收纳不录像；句柄用 WaitForSingleObject 轮询除名，
//   防止 pid 复用后误豁免后续 Agent 窗口。
QList<QPair<DWORD, HANDLE>>& CSandboxManager::UserRoots() {
    static QList<QPair<DWORD, HANDLE>> s_aRoots;
    return s_aRoots;
}

QMutex& CSandboxManager::UserRootsMtx() {
    static QMutex s_oMtx;
    return s_oMtx;
}

void CSandboxManager::PruneUserRoots() {
    // 调用方持 UserRootsMtx（ConsiderWindow）。句柄已信号 = 进程已退：关句柄除名。
    //   nullptr 句柄 = 无生命周期跟踪（测试/无句柄登记）＝永久豁免，不除名。
    auto& aRoots = UserRoots();
    for (int i = aRoots.size() - 1; i >= 0; --i) {
        const HANDLE h = aRoots.at(i).second;
        if (h != nullptr && WaitForSingleObject(h, 0) == WAIT_OBJECT_0) {
            CloseHandle(h);
            aRoots.removeAt(i);
        }
    }
}

void CSandboxManager::RegisterUserRoot(DWORD a_dwPid, HANDLE a_hProc) {
    // 句柄所有权移交登记表（退出/除名时关闭）；重复 pid 不重复登记（多余句柄就地关闭）。
    if (a_dwPid == 0) {
        if (a_hProc != nullptr) {
            CloseHandle(a_hProc);
        }
        return;
    }
    {
        QMutexLocker oL(&UserRootsMtx());
        for (const auto& pr : std::as_const(UserRoots())) {
            if (pr.first == a_dwPid) {
                if (a_hProc != nullptr) {
                    CloseHandle(a_hProc);
                }
                return;
            }
        }
        UserRoots().append({a_dwPid, a_hProc});
    }
    // 兑底网：登记前窗口已被收纳（ShellExecuteEx 泵消息竞态致判定
    //   先行）→ 立即还原停录像，把窗口还给用户。复用钩子路由的实例静态指针。
    if (CSandboxManager* pMgr = g_pSbxMgrForHook.load(std::memory_order_acquire)) {
        pMgr->RestoreEntriesOfPid(a_dwPid);
    }
}

void CSandboxManager::ClearUserRoots() {
    QMutexLocker oL(&UserRootsMtx());
    for (const auto& pr : std::as_const(UserRoots())) {
        if (pr.second != nullptr) {
            CloseHandle(pr.second);
        }
    }
    UserRoots().clear();
}

void CSandboxManager::RestoreEntriesOfPid(DWORD a_dwPid) {
    // 2026-10-01 UI 线程调用（RegisterUserRoot 兑底网）：tryLock 短等待——沙箱命令在
    //   工作线程执行时可能持锁很久，UI 不能等；拿不到锁延迟重试一次，再不行交扫描兜底。
    if (!m_mtxState.tryLock(500)) {
        QTimer::singleShot(2000, this, [this, a_dwPid]() {
            if (m_mtxState.tryLock(500)) {
                RestoreEntriesOfPidLocked(a_dwPid);
                m_mtxState.unlock();
            } else {
                qWarning("[CSandboxManager] 兑底还原 pid=%lu 未获锁（沙箱忙），交周期扫描处理", a_dwPid);
            }
        });
        return;
    }
    RestoreEntriesOfPidLocked(a_dwPid);
    m_mtxState.unlock();
}

void CSandboxManager::RestoreEntriesOfPidLocked(DWORD a_dwPid) {
    // 快照 hwnd 后逐个还原（RestoreWindowEx 内部 removeAt 会移动下标）。
    //   调用方持 m_mtxState。
    QList<HWND> aHit;
    for (const SEntry& st : std::as_const(m_aEntries)) {
        if (st.dwPid == a_dwPid) {
            aHit.append(st.h);
        }
    }
    for (HWND h : aHit) {
        RestoreWindowEx(h);
    }
}

// ---------------------------------------------------------------------------
CSandboxManager::CSandboxManager(QObject* a_pParent)
    : QObject(a_pParent) {
    m_tSweep.setInterval(1200);
    connect(&m_tSweep, &QTimer::timeout, this, &CSandboxManager::OnSweep);
}

CSandboxManager::~CSandboxManager() {
    Shutdown();
}

void CSandboxManager::SetConfig(const SSbxCfg& a_stCfg) {
    m_stCfg = a_stCfg;
    m_stCfg.iFps = qBound(1, m_stCfg.iFps, 15);
    m_stCfg.iRetainDays = qBound(1, m_stCfg.iRetainDays, 365);
}

QString CSandboxManager::RecordDir() const {
    return QCoreApplication::applicationDirPath() + QStringLiteral("/sandbox/rec");
}

QString CSandboxManager::ShotDir() const {
    return QCoreApplication::applicationDirPath() + QStringLiteral("/sandbox/shot");
}

void CSandboxManager::SetProjectRoot(const QString& a_sRoot) {
    m_sProjRoot = a_sRoot;
}

// 2026-10-08 跨实例续收纳扫描：枚举全部带幽灵标记的顶层窗口（上个 AutoCode
//   实例收纳中残留——正常退出时在册窗口被还原，但还原后的窗口标记已清；这里
//   处理的是「标记仍在」的少数：上个实例崩溃被强杀、或本实例启动早于其退出
//   还原竞态等）。窗口进程 exe 在当前工程目录内 → GhostAndTrack 续收纳（窗口
//   保持隐形，Agent 可继续 sandbox_* 操控）；其余（与本工程无关/拿不准的）
//   还原成可见由用户处置——替代旧的无差别 RestoreAllMarked（会把刚续收纳的
//   窗口又还原掉）。
// 2026-10-09 修「用户软件窗口反复自动消失/出现」（本机制两处过度行为实测暴露）：
//   ①「曾收纳」标记无时效——工程内软件只要被 Agent 测过一次，此后每次启动/
//     切工程都被强制隐形、退出又还原，用户一天重启多次就反复消失/出现。现标记
//     值=还原时刻，续收纳仅认最近 kAdoptFreshMs（10 分钟）内的窗口——超龄=Agent
//     早不再测它、用户可能正在用 → 清标记恢复常态，永不再自动收走。
//   ②多实例越权——实例 B 的本扫描会把实例 A 正在沙箱管理的窗口强制还原（A 的
//     内存表仍认为在册，Agent 测试现场被打断）。现按 kPropGhost 值（收纳者 PID）
//     互斥：收纳者是活着的其他实例 → 跳过不动（各管各的）；死了才是崩溃残留。
void CSandboxManager::ReadoptProjectWindows() {
    constexpr ULONG kAdoptFreshMs = 10 * 60 * 1000;   // 续收纳时效窗：最近还原 10 分钟内
    const DWORD dwSelf = GetCurrentProcessId();
    struct SCtx {
        QVector<QPair<HWND, DWORD>> aMarked;
    } stCtx;
    struct SEnum {
        static BOOL CALLBACK Proc(HWND a_hWnd, LPARAM a_lParam) {
            auto* pCtx = reinterpret_cast<SCtx*>(a_lParam);
            // 幽灵中（崩溃/强杀残留）或带「曾收纳」标记（上个实例退出还原后留存）
            // 都算候选——后者正是跨实例主场景：窗口已被还原成可见。
            if (sbx::IsGhosted(a_hWnd) || sbx::HasAdoptMark(a_hWnd)) {
                DWORD dwPid = 0;
                GetWindowThreadProcessId(a_hWnd, &dwPid);
                pCtx->aMarked.append({ a_hWnd, dwPid });
            }
            return TRUE;
        }
    };
    EnumWindows(SEnum::Proc, reinterpret_cast<LPARAM>(&stCtx));
    // 前缀根规范化：去尾斜杠再统一补一个，防「E:/x/」+"/"="E:/x//" 前缀误判。
    QString sRoot = QDir::fromNativeSeparators(m_sProjRoot).toLower();
    if (!sRoot.isEmpty()) {
        while (sRoot.endsWith(QLatin1Char('/'))) {
            sRoot.chop(1);
        }
        sRoot += QLatin1Char('/');
    }
    const ULONG dwNow = GetTickCount();
    for (const auto& pr : std::as_const(stCtx.aMarked)) {
        const HWND h = pr.first;
        // 实例互斥：幽灵中且收纳者是活着的其他实例（本窗口归它管）→ 不越权
        //   动它。OpenProcess 成功即活着（已退 pid 返回失败）；极小概率 pid 被
        //   无关进程复用 → 误判「别人在管」而跳过，方向保守（不动），无害。
        if (sbx::IsGhosted(h)) {
            const DWORD dwOwner = sbx::GhostOwnerPid(h);
            if (dwOwner != 0 && dwOwner != dwSelf) {
                if (HANDLE hO = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, dwOwner)) {
                    CloseHandle(hO);
                    continue;   // 别的活实例正在管理：跳过
                }
            }
        }
        const bool bInProject = !sRoot.isEmpty()
            && QDir::fromNativeSeparators(sbx::ExePathOfPid(pr.second)).toLower().startsWith(sRoot);
        if (sbx::IsGhosted(h)) {
            // 崩溃/强杀残留（收纳者已死）：保持隐形逻辑——工程内续收纳入册供
            //   Agent 接续操控；工程外还原可见由用户处置。
            if (bInProject) {
                GhostAndTrack(h);   // 在册查重幂等（见 GhostAndTrack 头部）
            } else {
                sbx::RestoreWindow(h);
            }
        } else if (bInProject
                   && (dwNow - sbx::AdoptMarkTick(h)) < kAdoptFreshMs) {
            // 可见 + 带标记 + 刚还原不久 + 工程内 = Agent 会话还在继续（实例重启
            //   间隙通常几十秒）→ 续收纳重新隐形。
            GhostAndTrack(h);
        } else {
            // 超龄标记 / 工程外：Agent 早不再测它（用户可能在用）→ 清标记恢复
            //   常态，不再自动收走。此前无时效版本会把用户正在用的工程内软件
            //   反复收走又还原（每次启动/切工程一轮）。
            sbx::ClearAdoptMark(h);
        }
    }
}

void CSandboxManager::Start() {
    if (m_bStarted) {
        return;
    }
    m_bStarted = true;
    m_bRecOn = m_stCfg.bRecord;
    // ① 孤儿处理（ReadoptProjectWindows 一体承担）：工程内残留窗口续收纳保持
    //   隐形，其余带标记的（崩溃残留/无关进程）保守还原成可见由用户处置。
    ReadoptProjectWindows();
    // ② 录像保留清理：超期 MP4 删除（启动一次即够）。
    QDir stRecDir(RecordDir());
    if (stRecDir.exists()) {
        const qint64 iCut = QDateTime::currentMSecsSinceEpoch()
                          - qint64(m_stCfg.iRetainDays) * 24 * 3600 * 1000;
        for (const QFileInfo& stFi : stRecDir.entryInfoList(
                 QStringList() << QStringLiteral("*.mp4"), QDir::Files)) {
            if (stFi.lastModified().toMSecsSinceEpoch() < iCut) {
                QFile::remove(stFi.absoluteFilePath());
            }
        }
    }
    // ③ 钩子 + 周期扫描（总开关开才收纳；关时仅保留命令通道与孤儿还原）。
    if (m_stCfg.bEnable) {
        g_pSbxMgrForHook.store(this, std::memory_order_release);
        if (m_stCfg.bAutoGhost) {
            auto* pHook = new CSbxHookThread(this);
            pHook->setObjectName(QStringLiteral("sbxHook"));
            m_pHookTh = pHook;
            // 2026-10-08 同崩溃修复同类：捕获 this 的 lambda 若晚于管理器析构执行
            //   （finished 排队到主线程时管理器已亡）→ 同类悬垂。改 QPointer 守护。
            QPointer<CSandboxManager> pSelf(this);
            connect(pHook, &QThread::finished, pHook, [pSelf, pHook]() {
                if (!pSelf.isNull() && pSelf->m_pHookTh == pHook) {
                    pSelf->m_pHookTh = nullptr;
                }
                pHook->deleteLater();
            });
            pHook->start();
        }
        m_tSweep.start();
    }
}

void CSandboxManager::Shutdown() {
    m_tSweep.stop();
    g_pSbxMgrForHook.store(nullptr, std::memory_order_release);
    m_aPendingGhosts.clear();   // 宽限期待定作废（定时器 lambda 有窗口有效性复判）
    // 停钩子线程（SetWinEventHook 要求安装线程自己 Unhook）。
    // 2026.09.28 有限等待线程退出：不等得重雪旧实现（QThread 父子托管）
    //   ——线程仍在跑时对象被析构会 qFatal 直接终止进程。
    if (m_pHookTh != nullptr) {
        auto* pHook = static_cast<CSbxHookThread*>(m_pHookTh);
        pHook->StopHook();
        if (!pHook->wait(2000)) {
            qWarning("[CSandboxManager] 钩子线程未在 2s 内退出（可能卡在 UnhookWinEvent）");
        }
        m_pHookTh = nullptr;   // finished→deleteLater 已挂，对象由事件循环回收
    }
    // 全部还原 + 停录像。2026-10-08 退出还原保留「曾收纳」标记（kPropAdopt）：
    //   继任实例（版本更新/重开）扫描到它 + 窗口进程 exe 在工程目录内 → 续收纳
    //   重新隐形；否则该窗口被还原成可见且新实例的进程树出身判定永不再认它
    //   （祖先是已退出实例的后代），Agent 继续测试就裸奔在用户桌面（Compositer
    //   工程实测）。用户主动还原（对话框/restore 命令）仍走清标记语义。
    if (m_mtxState.tryLock(2000)) {
        // 2026-10-01 tryLock：工作线程卡在长 UIA 操作时退出不能无限等，拿不到锁跳过
        //   内存表还原——SetProp 幽灵标记由下方 RestoreAllMarked 兑底。
        while (!m_aEntries.isEmpty()) {
            const HWND h = m_aEntries.first().h;
            StopRecorderOf(0);
            sbx::RestoreWindow(h, true);   // 退出还原：保留 adopt 标记
            m_aEntries.removeAt(0);
        }
        RebuildCacheLocked();
        m_mtxState.unlock();
    } else {
        qWarning("[CSandboxManager] Shutdown 未获状态锁（沙箱命令仍在执行），内存表还原跳过");
    }
    sbx::RestoreAllMarked(true);   // 兑底（含命令通道手工收纳的；同样保留 adopt 标记）
    m_bStarted = false;
}

void CSandboxManager::ReloadConfig() {
    if (!m_bStarted) {
        Start();
        return;
    }
    if (m_stCfg.bEnable) {
        if (!m_tSweep.isActive()) {
            m_tSweep.start();
        }
    } else {
        m_tSweep.stop();
    }
    m_bRecOn = m_stCfg.bRecord;
}

// ---------------------------------------------------------------------------
// 进程树与收纳
// ---------------------------------------------------------------------------
bool CSandboxManager::TreePid(DWORD a_dwPid) {
    if (m_aManualRoots.contains(a_dwPid)) {
        return true;
    }
    const qint64 iNow = QDateTime::currentMSecsSinceEpoch();
    if (m_oParentMap.isEmpty() || iNow - m_iMapBuiltMs > 500) {
        sbx::BuildParentMap(m_oParentMap, m_oParentCreated);
        m_iMapBuiltMs = iNow;
    }
    return sbx::IsDescendantOf(a_dwPid, GetCurrentProcessId(), m_oParentMap,
                               m_oParentCreated);
}

bool CSandboxManager::IsProjectTestPid(DWORD a_dwPid) {
    // 断链兑底（见 ConsiderWindowImpl 2026-10-14 注）：树外但像「Agent 刚启动的
    //   被测工程产物」的窗口进程。三重限定压误伤：
    //   ① 工程根非空且 exe 路径在工程根内（被测程序 = 自研产物，与 2026-10-08
    //     续收纳/显式 add 的工程目录白名单同一哲学）；
    //   ② 进程新建 <10 分钟（用户手动打开的应用多已运行较久；拿不到创建时刻
    //     保守不兑底）；
    //   ③ 父进程已死（断链）才兑底——Agent 经 powershell Start-Process / cmd
    //     start 等中间人启动、中间 shell 执行完即退，被测程序主窗口弹出时父
    //     已死，这是要救的场景；父进程活着（无论 explorer 双击、QuickTool/
    //     托盘/计划任务/任何非 explorer 启动器）= 用户主动打开，一律不兑底。
    //     2026-10-16 修：旧口径只豁免「父=explorer.exe」，用户经 QuickTool 打开
    //     HarnessAgent.exe 被误幽灵化（窗口 alpha=1+点击穿透+无任务栏，「消失」
    //     但进程存活，实测 ex=0x80801A0）——收紧为父活即拒（IsParentAlive，
    //     按「父 pid 仍是快照表键」判活，见 CSandboxCore.h 注）。
    if (m_sProjRoot.isEmpty() || a_dwPid == 0) {
        return false;
    }
    QString sRoot = QDir::fromNativeSeparators(m_sProjRoot).toLower();
    while (sRoot.endsWith(QLatin1Char('/'))) {
        sRoot.chop(1);
    }
    sRoot += QLatin1Char('/');
    const QString sExe = QDir::fromNativeSeparators(sbx::ExePathOfPid(a_dwPid)).toLower();
    if (sExe.isEmpty() || !sExe.startsWith(sRoot)) {
        return false;
    }
    // 父表防陈旧（与 TreePid 同一守卫）：表早于进程创建会把活父判成断链（值查
    //   不到→误兑底），判否前超 500ms 先重建再查。
    const qint64 iNow = QDateTime::currentMSecsSinceEpoch();
    if (m_oParentMap.isEmpty() || iNow - m_iMapBuiltMs > 500) {
        sbx::BuildParentMap(m_oParentMap, m_oParentCreated);
        m_iMapBuiltMs = iNow;
    }
    if (sbx::IsParentAlive(a_dwPid, m_oParentMap)) {
        return false;   // 父进程活着：用户主动打开（任意启动器），不兑底
    }
    const qint64 iCreated = m_oParentCreated.value(a_dwPid, 0);
    if (iCreated <= 0) {
        return false;   // 创建时刻不可得：保守不兑底
    }
    const qint64 iAgeMs = QDateTime::currentMSecsSinceEpoch() - iCreated;
    return iAgeMs >= 0 && iAgeMs < 10LL * 60 * 1000;
}

bool CSandboxManager::LaunchAut(const QJsonObject& a_oArgs, QString& a_sOut) {
    // sandbox launch <exe> [args...]：主进程亲自启动被测程序并登记手工根——
    //   窗口弹出即收纳（TreePid 命中手工根 + 零宽限直收），不受 Agent shell
    //   中间人断链影响（Compositer 实测 Start-Process 秒退断链漏收的治本通道）。
    QString sExe = QDir::fromNativeSeparators(
        a_oArgs.value(QStringLiteral("exe")).toString().trimmed());
    if (sExe.isEmpty()) {
        a_sOut = QStringLiteral("缺少 exe 参数");
        return false;
    }
    if (!QFileInfo::exists(sExe)) {
        a_sOut = QStringLiteral("exe 不存在：%1").arg(sExe);
        return false;
    }
    if (sbx::IsProtectedAppPath(sExe)) {
        a_sOut = QStringLiteral("系统/公共应用不允许由沙箱启动");
        return false;
    }
    QString sWorkdir = a_oArgs.value(QStringLiteral("workdir")).toString().trimmed();
    if (sWorkdir.isEmpty()) {
        sWorkdir = QFileInfo(sExe).absolutePath();
    }
    const QString sCmdLine = QStringLiteral("\"%1\" %2")
        .arg(QDir::toNativeSeparators(sExe),
             a_oArgs.value(QStringLiteral("args")).toString().trimmed());
    std::vector<wchar_t> vCmd(static_cast<size_t>(sCmdLine.size()) + 1, L'\0');
    sCmdLine.toWCharArray(vCmd.data());
    const std::wstring wsDir = QDir::toNativeSeparators(sWorkdir).toStdWString();
    STARTUPINFOW stSi = {};
    stSi.cb = sizeof(stSi);
    PROCESS_INFORMATION stPi = {};
    if (!CreateProcessW(nullptr, vCmd.data(), nullptr, nullptr, FALSE, 0,
                        nullptr, wsDir.c_str(), &stSi, &stPi)) {
        a_sOut = QStringLiteral("启动失败（GetLastError=%1）").arg(GetLastError());
        return false;
    }
    AddManualRootPid(stPi.dwProcessId);
    CloseHandle(stPi.hThread);   // 句柄即关，防泄露（进程存活不依赖句柄）
    CloseHandle(stPi.hProcess);
    a_sOut = QStringLiteral("已由沙箱代启动 pid=%1（窗口弹出即自动收纳）\n"
                            "查询窗口：sandbox windows；截图：sandbox capture <hwnd>")
                 .arg(stPi.dwProcessId);
    return true;
}

bool CSandboxManager::ConsiderWindow(HWND a_hWnd, bool a_bFinal) {
    // 2026-10-01 跨线程（钩子线程/宽限 lambda）入口：tryLock 短等待；沙箱忙则放弃
    //   本次判定——返回 false 走宽限/扫描兑底，不阻塞调用线程。
    if (!m_mtxState.tryLock(150)) {
        return false;
    }
    const bool bOk = ConsiderWindowImpl(a_hWnd, a_bFinal);
    m_mtxState.unlock();
    return bOk;
}

bool CSandboxManager::ConsiderWindowImpl(HWND a_hWnd, bool a_bFinal) {
    if (!m_stCfg.bEnable || !m_bStarted) {
        return false;   // 停用/已 Shutdown：宽限定时器晚到也不可再收纳
    }
    for (const SEntry& st : std::as_const(m_aEntries)) {
        if (st.h == a_hWnd) {
            return false;   // 已在册
        }
    }
    if (!sbx::IsGhostableShape(a_hWnd)) {
        return false;
    }
    DWORD dwPid = 0;
    GetWindowThreadProcessId(a_hWnd, &dwPid);
    // 2026-10-14 断链兑底（Compositer 实测漏收根因）：Agent 用 powershell
    //   Start-Process / cmd start 等中间人方式启动被测程序时，中间 shell 执行完
    //   立即退出，被测程序主窗口弹出时父进程已死——IsDescendantOf 沿 ppid 爬链
    //   第一跳即断（快照无此 pid），TreePid 判否 → 不收纳，窗口正常可见弹到
    //   用户桌面还抢焦点。兑底：exe 在当前工程根目录内且进程新建 → 视为 Agent
    //   启动的被测工程产物，继续收纳判定（其后仍过豁免登记/系统宿主/保护路径
    //   三道闸，用户经 AutoCode UI 打开的工程内程序照常豁免）。2026-10-16 收紧：
    //   兑底另须父已死（断链）——父活着=用户经 QuickTool/explorer 等任意启动器
    //   主动打开，不兑底（IsProjectTestPid 条件③，修 QuickTool 打开工程产物被
    //   误幽灵化）。
    bool bProjFallback = false;
    if (!TreePid(dwPid)) {
        bProjFallback = IsProjectTestPid(dwPid);
        if (!bProjFallback) {
            return false;
        }
    }
    // 2026.09.26 用户启动豁免（TreePid 之后判定，复用刚刷新的父表）：命中登记根
    //   （自身或其子进程）不收纳——用户在 UI 主动打开的程序要自己用，不是 Agent
    //   的被测对象；幽灵化会让窗口凭空消失，录像还可能（本机硬编缺陷）崩进程。
    {
        QMutexLocker oL(&UserRootsMtx());
        PruneUserRoots();
        for (const auto& pr : std::as_const(UserRoots())) {
            if (dwPid == pr.first
                || sbx::IsDescendantOf(dwPid, pr.first, m_oParentMap, m_oParentCreated)) {
                return false;
            }
        }
    }
    if (sbx::IsSystemHostPid(dwPid)) {
        return false;   // 树内系统宿主（explorer 等被 agent shell 打开等极端情形）
    }
    // 2026-10-09 收纳范围收紧：系统/公共应用目录（Windows、Program Files、
    //   WindowsApps）下的 exe 不收纳——树内进程启动它们几乎都是「Agent 打开给
    //   用户看」（浏览器看报告、默认程序看文档/图片）或系统组件，绝非被测工程
    //   程序；自动收纳它们会让用户正在查看的窗口凭空消失（用户实测）。被测程序
    //   = 自研代码产物，都在工程/输出目录；确需测试的特殊场景走显式 add
    //   （AddWindow 同样受保护目录拦截，双保险）。
    if (sbx::IsProtectedAppPath(sbx::ExePathOfPid(dwPid))) {
        return false;
    }
    // 2026-10-08 零闪现收纳：旧逻辑对所有树内窗口一律先排 kGhostGraceMs 宽限期
    //   （为等用户启动豁免登记落地），代价是 Agent 后代窗口每个都先裸奔 1.2 秒
    //   ——回归跑批反复启被测程序时用户桌面反复闪窗（SmartStore 工程实测）。
    //   区分两类：直接父 == 本 AutoCode 进程 = 用户经资源管理器双击/打开方式/
    //   CMD 运行（ShellExecuteEx 由本进程发起，豁免 pid 在其返回后才登记，需要
    //   宽限等待）；链上隔了 runner（cmd/claude/hermes→bash→python→被测程序）
    //   = Agent 后代，豁免登记永不涉及 → 立即收纳。
    const bool bUserLaunchLike =
        (m_oParentMap.value(dwPid, 0) == GetCurrentProcessId()) || bProjFallback;
    if (!a_bFinal && bUserLaunchLike && m_stCfg.bAutoGhost
            && !m_aManualRoots.contains(dwPid)) {   // launch 登记根：主进程亲自启动，
        // 豁免登记永不涉及，零闪现直收（不走 800ms 宽限）。
        // 800ms：豁免登记在 ShellExecuteExW 返回后立即执行（毫秒级），800 绰绰
        //   有余（旧 1200 过度保守，用户启动的程序也多闪半秒）。
        constexpr int kGhostGraceMs = 800;
        const quintptr hKey = reinterpret_cast<quintptr>(a_hWnd);
        if (!m_aPendingGhosts.contains(hKey)) {
            m_aPendingGhosts.insert(hKey);
            QTimer::singleShot(kGhostGraceMs, this, [this, a_hWnd, hKey]() {
                // 宽限期到：终判（经 ConsiderWindow 拿锁——lambda 在 UI 线程）。
                m_aPendingGhosts.remove(hKey);
                ConsiderWindow(a_hWnd, true);
            });
        }
        return false;   // 宽限中，尚未收纳
    }
    GhostAndTrack(a_hWnd);
    return true;
}

void CSandboxManager::GhostAndTrack(HWND a_hWnd) {
    // 2026.09.28 返回收纳成败（旧实现 GhostWindow 失败时静默 return 仍建不了条目，
    //   AddWindow 却恒报成功，--sandbox add 误报「已收纳」）。
    // 2026-10-08 已带标记窗口（跨实例残留，ReadoptProjectWindows 续收纳路径）
    //   GhostWindow 幂等返回 false 是预期——IsGhosted 前提成立则继续登记入册。
    // 2026-10-09 在册查重：Readopt 在 Start 与每次切工程都跑，已入册窗口直接
    //   幂等返回，否则重复入册（GhostedCount 虚高、restore 重复处理）。
    for (const SEntry& st : std::as_const(m_aEntries)) {
        if (st.h == a_hWnd) {
            return;
        }
    }
    if (!sbx::GhostWindow(a_hWnd) && !sbx::IsGhosted(a_hWnd)) {
        return;
    }
    SEntry st;
    st.h = a_hWnd;
    GetWindowThreadProcessId(a_hWnd, &st.dwPid);
    st.sExe = sbx::ExePathOfPid(st.dwPid);
    st.sTitle = sbx::WndTitle(a_hWnd);
    st.iGhostAtMs = QDateTime::currentMSecsSinceEpoch();
    if (m_bRecOn && m_stCfg.bEnable) {
        RECT rc = {};
        GetWindowRect(a_hWnd, &rc);
        const int iW = rc.right - rc.left, iH = rc.bottom - rc.top;
        if (iW >= 96 && iH >= 64) {
            st.pRec = StartRecorder(a_hWnd, st.dwPid, st.sExe, &st.sRecPath);
        }
    }
    m_aEntries.append(st);
    RebuildCacheLocked();
    emit sigChanged();
}

bool CSandboxManager::AddWindow(HWND a_hWnd) {
    // 显式收纳：仅收「确定与 Agent 测试相关」的窗口，不设防的旧实现已废——
    //   Agent 依提示词「不在列表就 add」操作时猜错 hwnd 会把用户正在用的窗口
    //   收走（用户实测「无关窗口凭空消失」）。2026-10-09 出身白名单：
    //   ①带幽灵/曾收纳标记的残留窗口（崩溃/跨实例残留，此前就在沙箱里）；
    //   ②本 AutoCode 进程树内（Agent 启动的进程——自动收纳的漏网补收）；
    //   ③exe 在当前工程目录内（工程产物）；其余一律拒绝。
    //   系统/公共应用目录（Windows、Program Files：浏览器/Office/记事本等
    //   用户正在用的程序）即使带标记也不重收（还原后由用户处置）。
    for (const SEntry& st : std::as_const(m_aEntries)) {
        if (st.h == a_hWnd) {
            return true;
        }
    }
    if (!IsWindow(a_hWnd) || !IsWindowVisible(a_hWnd)
            || GetAncestor(a_hWnd, GA_ROOT) != a_hWnd) {
        return false;
    }
    const bool bMarked = sbx::IsGhosted(a_hWnd) || sbx::HasAdoptMark(a_hWnd);
    DWORD dwPid = 0;
    GetWindowThreadProcessId(a_hWnd, &dwPid);
    const QString sExe = QDir::fromNativeSeparators(sbx::ExePathOfPid(dwPid));
    if (sbx::IsProtectedAppPath(sExe)) {
        return false;   // 系统/公共应用：用户可能正在用，永不收纳
    }
    if (!bMarked) {
        const bool bInTree = TreePid(dwPid);
        QString sRoot = QDir::fromNativeSeparators(m_sProjRoot).toLower();
        if (!sRoot.isEmpty()) {
            while (sRoot.endsWith(QLatin1Char('/'))) {
                sRoot.chop(1);
            }
            sRoot += QLatin1Char('/');
        }
        const bool bInProject = !sRoot.isEmpty()
            && sExe.toLower().startsWith(sRoot);
        if (!bInTree && !bInProject) {
            return false;   // 树外 + 工程外 + 无标记 = 用户自己的窗口，不收
        }
    }
    GhostAndTrack(a_hWnd);
    // 2026.09.28 收纳成败以在册为准：GhostWindow 失败（改不动样式/已幽灵化等）时
    //   GhostAndTrack 不建条目，不能像旧实现那样恒报成功。
    for (const SEntry& st : std::as_const(m_aEntries)) {
        if (st.h == a_hWnd) {
            return true;
        }
    }
    return false;
}

bool CSandboxManager::RestoreWindowEx(HWND a_hWnd) {
    for (int i = 0; i < m_aEntries.size(); ++i) {
        if (m_aEntries.at(i).h == a_hWnd) {
            StopRecorderOf(i);
            sbx::RestoreWindow(a_hWnd);
            m_aEntries.removeAt(i);
            RebuildCacheLocked();
            emit sigChanged();
            return true;
        }
    }
    return sbx::RestoreWindow(a_hWnd);
}

CSandboxRecorder* CSandboxManager::StartRecorder(HWND a_hWnd, DWORD a_dwPid,
                                                 const QString& a_sExe,
                                                 QString* a_psPathOut) {
    QDir().mkpath(RecordDir());
    // 文件名：<yyyyMMdd_HHmmss>_<exe基名>_<pid>.mp4（非法字符换 _）。
    QString sBase = QFileInfo(a_sExe).completeBaseName();
    if (sBase.isEmpty()) {
        sBase = QStringLiteral("win");
    }
    sBase.replace(QRegularExpression(QStringLiteral("[^A-Za-z0-9_\\-\\u4e00-\\u9fff]")),
                  QStringLiteral("_"));
    const QString sPath = QStringLiteral("%1/%2_%3_%4.mp4")
        .arg(RecordDir(),
             QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd_HHmmss")),
             sBase).arg(a_dwPid);
    auto* pRec = new CSandboxRecorder(a_hWnd, m_stCfg.iFps, sPath);
    // 2026-10-01 创建线程可能是线程池（无 Qt 事件循环，deleteLater 永不执行）或钩子
    //   线程（原生消息循环非 Qt）——统一报到管理器线程（UI，有事件循环），
    //   sigFinished→deleteLater 才能被处理；run() 始终在新起线程执行，不受影响。
    pRec->moveToThread(thread());
    if (a_psPathOut != nullptr) {
        *a_psPathOut = sPath;
    }
    connect(pRec, &CSandboxRecorder::sigFinished, this, &CSandboxManager::OnRecFinished);
    connect(pRec, &CSandboxRecorder::sigFinished, pRec, &QObject::deleteLater);
    pRec->start();
    return pRec;
}

void CSandboxManager::StopRecorderOf(int a_iIdx) {
    if (a_iIdx < 0 || a_iIdx >= m_aEntries.size()) {
        return;
    }
    if (CSandboxRecorder* pRec = m_aEntries.at(a_iIdx).pRec) {
        pRec->RequestStop();   // 收尾由 sigFinished → OnRecFinished 统一处理
        m_aEntries[a_iIdx].pRec = nullptr;
    }
}

void CSandboxManager::OnRecFinished() {
    // 录像线程收尾（UI 线程 queued）：tryLock 重建快照（bRecording 归位）；
    //   沙箱忙则跳过——下轮扫描 1.2s 内补上。
    if (m_mtxState.tryLock(50)) {
        RebuildCacheLocked();
        m_mtxState.unlock();
    }
    emit sigChanged();
}

void CSandboxManager::OnSweep() {
    if (!m_stCfg.bEnable) {
        return;
    }
    // ① 补收纳：钩子事件可能丢失（窗口先显示后钩子才装好等），全表扫描兜底。
    // 2026-10-01 tryLock 跳过本轮：沙箱命令在执行（长 UIA/截图）时放弃扫描，
    //   下轮 1.2s 后再来——UI 线程永不阻塞在沙箱状态锁上。
    if (!m_mtxState.tryLock(150)) {
        return;
    }
    struct SEnumCtx { CSandboxManager* pMgr; };
    SEnumCtx stCtx { this };
    EnumWindows([](HWND a_hWnd, LPARAM a_lParam) -> BOOL {
        auto* pCtx = reinterpret_cast<SEnumCtx*>(a_lParam);
        pCtx->pMgr->ConsiderWindowImpl(a_hWnd, false);
        return TRUE;
    }, reinterpret_cast<LPARAM>(&stCtx));
    // ② 条目健康检查：窗口销毁的摘表停录。
    for (int i = m_aEntries.size() - 1; i >= 0; --i) {
        if (!IsWindow(m_aEntries.at(i).h)) {
            StopRecorderOf(i);
            m_aEntries.removeAt(i);
            emit sigChanged();
        }
    }
    RebuildCacheLocked();
    m_mtxState.unlock();
}

QList<CSandboxManager::SRow> CSandboxManager::Rows() const {
    // 2026-10-01 快照读：m_mtxCache 只在赋值/拷贝瞬间持有（绝不长持有），UI 无锁等待。
    QMutexLocker oL(&m_mtxCache);
    return m_aRowsCache;
}

void CSandboxManager::RebuildCacheLocked() {
    // 调用方持 m_mtxState；锁序恒为 m_mtxState → m_mtxCache。
    QList<SRow> aRows;
    for (const SEntry& st : std::as_const(m_aEntries)) {
        SRow stRow;
        stRow.h = st.h;
        stRow.dwPid = st.dwPid;
        stRow.sExe = st.sExe;
        stRow.sTitle = st.sTitle;
        stRow.iGhostAtMs = st.iGhostAtMs;
        stRow.sRecPath = st.sRecPath;
        stRow.bRecording = (st.pRec != nullptr);
        aRows.append(stRow);
    }
    {
        QMutexLocker oL(&m_mtxCache);
        m_aRowsCache = aRows;
    }
}

// ---------------------------------------------------------------------------
// 命令分派（MCP 工具 / --sandbox CLI 统一入口）
// ---------------------------------------------------------------------------
HWND CSandboxManager::ArgHwnd(const QJsonObject& a_oArgs, bool* a_pbOk) const {
    HWND h = nullptr;
    if (!sbx::ParseHwnd(a_oArgs.value(QStringLiteral("hwnd")).toString(), &h)) {
        // 缺省取唯一在册窗口（agent 常只开一个测试程序）。
        if (m_aEntries.size() == 1) {
            if (a_pbOk != nullptr) *a_pbOk = true;
            return m_aEntries.first().h;
        }
        if (a_pbOk != nullptr) *a_pbOk = false;
        return nullptr;
    }
    if (a_pbOk != nullptr) *a_pbOk = true;
    return h;
}

bool CSandboxManager::Execute(const QString& a_sCmd, const QJsonObject& a_oArgs,
                              QString& a_sOut) {
    // 2026-10-01 阻塞锁入口：供工作线程（主窗口线程池/MTA COM）与 CLI/测试调用；
    //   UI 线程手动操作改走 ExecuteTry（tryLock），永不等长操作。
    QMutexLocker oL(&m_mtxState);
    return ExecuteLocked(a_sCmd, a_oArgs, a_sOut);
}

bool CSandboxManager::ExecuteTry(const QString& a_sCmd, const QJsonObject& a_oArgs,
                                 QString& a_sOut) {
    // UI 线程专用（沙箱对话框手动截图/还原等）：沙箱忙立即回友好提示。
    if (!m_mtxState.tryLock(200)) {
        a_sOut = QStringLiteral("沙箱忙（助手沙箱命令执行中，可能含耗时 UIA/截图），请稍后重试");
        return false;
    }
    const bool bOk = ExecuteLocked(a_sCmd, a_oArgs, a_sOut);
    m_mtxState.unlock();
    return bOk;
}

bool CSandboxManager::ExecuteLocked(const QString& a_sCmd, const QJsonObject& a_oArgs,
                                    QString& a_sOut) {
    // 2026-10-01 阻塞叶子操作无锁化：调用方持 m_mtxState 进入，但 Win32/UIA 叶子
    //   （截图/控件树/同步点击/输入）可能无限阻塞（hung 窗口/输入法钩子）——若持
    //   锁阻塞，后续一切沙箱命令都会饿死在锁上。叶子期间临时解锁（fnLeaf），
    //   完成后重新持锁；hwnd 已在锁内解析为局部变量，不依赖状态。
    auto fnLeaf = [this](auto a_fn) { m_mtxState.unlock(); const auto r = a_fn(); m_mtxState.lock(); return r; };
    const QString sCmd = a_sCmd.toLower();
    a_sOut.clear();
    if (sCmd == QStringLiteral("status")) {
        a_sOut = QStringLiteral("沙箱：%1｜自动收纳：%2｜录像：%3（帧率 %4）｜在册窗口 %5 个\n录像目录：%6")
            .arg(m_stCfg.bEnable ? QStringLiteral("启用") : QStringLiteral("停用"))
            .arg(m_stCfg.bAutoGhost ? QStringLiteral("开") : QStringLiteral("关"))
            .arg(m_bRecOn ? QStringLiteral("开") : QStringLiteral("关"))
            .arg(m_stCfg.iFps).arg(m_aEntries.size()).arg(RecordDir());
        return true;
    }
    if (!m_stCfg.bEnable && sCmd != QStringLiteral("restore_all")) {
        a_sOut = QStringLiteral("沙箱未启用（功能→设置→测试沙箱）");
        return false;
    }
    if (sCmd == QStringLiteral("windows")) {
        if (m_aEntries.isEmpty()) {
            a_sOut = QStringLiteral("（沙箱暂无窗口）");
            return true;
        }
        for (const SEntry& st : m_aEntries) {
            RECT rc = {};
            GetWindowRect(st.h, &rc);
            a_sOut += QStringLiteral("hwnd=0x%1 pid=%2 %3x%4 \"%5\" %6%7\n")
                .arg(reinterpret_cast<quintptr>(st.h), 0, 16)
                .arg(st.dwPid).arg(rc.right - rc.left).arg(rc.bottom - rc.top)
                .arg(st.sTitle, QFileInfo(st.sExe).fileName())
                .arg(st.pRec != nullptr ? QStringLiteral("  [录屏中]")
                                        : QStringLiteral("  [未录屏]"));
        }
        return true;
    }
    if (sCmd == QStringLiteral("record_on") || sCmd == QStringLiteral("record_off")) {
        m_bRecOn = (sCmd == QLatin1String("record_on"));
        a_sOut = QStringLiteral("沙箱录像已%1").arg(m_bRecOn ? QStringLiteral("开启")
                                                             : QStringLiteral("关闭"));
        return true;
    }
    if (sCmd == QStringLiteral("restore_all")) {
        int iN = 0;
        while (!m_aEntries.isEmpty()) {
            RestoreWindowEx(m_aEntries.first().h);
            ++iN;
        }
        iN += sbx::RestoreAllMarked();
        a_sOut = QStringLiteral("已还原 %1 个窗口").arg(iN);
        return true;
    }
    if (sCmd == QStringLiteral("launch")) {
        return LaunchAut(a_oArgs, a_sOut);
    }
    // —— 需要 hwnd 的命令 ——
    bool bHOk = false;
    const HWND h = ArgHwnd(a_oArgs, &bHOk);
    if (!bHOk || h == nullptr || !IsWindow(h)) {
        a_sOut = QStringLiteral("无效 hwnd（先 --sandbox windows 查询）");
        return false;
    }
    if (sCmd == QStringLiteral("capture")) {
        const QImage img = fnLeaf([&]() { return sbx::CaptureWindow(h); });
        if (img.isNull() || img.width() <= 0) {
            a_sOut = QStringLiteral("截图失败");
            return false;
        }
        QDir().mkpath(ShotDir());
        QString sPath = a_oArgs.value(QStringLiteral("path")).toString();
        if (sPath.isEmpty()) {
            sPath = QStringLiteral("%1/cap_%2.png").arg(ShotDir())
                .arg(QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd_HHmmsszzz")));
        }
        if (!img.save(sPath, "PNG")) {
            a_sOut = QStringLiteral("截图保存失败：%1").arg(sPath);
            return false;
        }
        a_sOut = QDir::toNativeSeparators(sPath);
        return true;
    }
    if (sCmd == QStringLiteral("click")) {
        const int iX = a_oArgs.value(QStringLiteral("x")).toInt();
        const int iY = a_oArgs.value(QStringLiteral("y")).toInt();
        const bool bDbl = a_oArgs.value(QStringLiteral("dbl")).toBool();
        const bool bRight = a_oArgs.value(QStringLiteral("right")).toBool();
        const bool bOk = fnLeaf([&]() { return sbx::ClickWindow(h, iX, iY, bDbl, bRight); });
        a_sOut = bOk ? QStringLiteral("已点击 (%1,%2)%3%4").arg(iX).arg(iY)
                         .arg(bDbl ? QStringLiteral(" 双击") : QString())
                         .arg(bRight ? QStringLiteral(" 右键") : QString())
                   : QStringLiteral("点击失败（窗口无效）");
        if (bOk) {
            for (const SEntry& st : std::as_const(m_aEntries)) {
                if (st.h == h && st.pRec != nullptr) {
                    st.pRec->SetClickMarker(iX, iY);
                }
            }
        }
        return bOk;
    }
    if (sCmd == QStringLiteral("drag")) {
        const bool bOk = fnLeaf([&]() {
            return sbx::DragWindow(h,
            a_oArgs.value(QStringLiteral("x1")).toInt(),
            a_oArgs.value(QStringLiteral("y1")).toInt(),
            a_oArgs.value(QStringLiteral("x2")).toInt(),
            a_oArgs.value(QStringLiteral("y2")).toInt(),
            qMax(50, a_oArgs.value(QStringLiteral("ms")).toInt(200)));
        });
        a_sOut = bOk ? QStringLiteral("已拖拽") : QStringLiteral("拖拽失败");
        return bOk;
    }
    if (sCmd == QStringLiteral("type")) {
        const bool bOk = fnLeaf([&]() { return sbx::TypeText(h, a_oArgs.value(QStringLiteral("text")).toString()); });
        a_sOut = bOk ? QStringLiteral("已输入文本") : QStringLiteral("输入失败（窗口无效）");
        return bOk;
    }
    if (sCmd == QStringLiteral("keys")) {
        const bool bOk = fnLeaf([&]() { return sbx::SendKeys(h, a_oArgs.value(QStringLiteral("keys")).toString()); });
        a_sOut = bOk ? QStringLiteral("已发送按键") : QStringLiteral("按键序列无法解析（如 ctrl+s）");
        return bOk;
    }
    if (sCmd == QStringLiteral("command")) {
        const bool bOk = fnLeaf([&]() { return sbx::SendWmCommand(h, a_oArgs.value(QStringLiteral("cmd")).toInt()); });
        a_sOut = bOk ? QStringLiteral("已发 WM_COMMAND") : QStringLiteral("发送失败");
        return bOk;
    }
    if (sCmd == QStringLiteral("button")) {
        const bool bOk = fnLeaf([&]() { return sbx::ClickButton(h); });
        a_sOut = bOk ? QStringLiteral("已发 BM_CLICK") : QStringLiteral("发送失败");
        return bOk;
    }
    if (sCmd == QStringLiteral("tree")) {
        a_sOut = fnLeaf([&]() { return sbx::DumpUiTree(h); });
        return true;
    }
    if (sCmd == QStringLiteral("add")) {
        const bool bOk = AddWindow(h);
        a_sOut = bOk ? QStringLiteral("已收纳") : QStringLiteral("收纳失败（形状/已收纳）");
        return bOk;
    }
    if (sCmd == QStringLiteral("restore")) {
        const bool bOk = RestoreWindowEx(h);
        a_sOut = bOk ? QStringLiteral("已还原（窗口恢复可见）")
                     : QStringLiteral("还原失败（非沙箱窗口）");
        return bOk;
    }
    if (sCmd == QStringLiteral("close")) {
        for (int i = 0; i < m_aEntries.size(); ++i) {
            if (m_aEntries.at(i).h == h) {
                StopRecorderOf(i);
                PostMessageW(h, WM_CLOSE, 0, 0);
                a_sOut = QStringLiteral("已发送关闭");
                return true;
            }
        }
        PostMessageW(h, WM_CLOSE, 0, 0);
        a_sOut = QStringLiteral("已发送关闭（非在册窗口）");
        return true;
    }
    a_sOut = QStringLiteral("未知沙箱命令：%1").arg(sCmd);
    return false;
}
