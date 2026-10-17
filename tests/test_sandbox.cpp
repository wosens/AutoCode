// 单元测试（test_sandbox）：内置测试沙箱核心链路回归。
//  ① CLI 参数解析 sbx::ParseCli（--sandbox 各子命令 → cmd+args JSON）；
//  ② 幽灵化/还原：真实 QWidget → GhostWindow（样式位/标记）→ RestoreWindow 复原；
//  ③ 消息级输入：ClickWindow 投递到按钮（eventFilter 计数）、TypeText 进 QLineEdit、
//     SendKeys backspace 删字符；
//  ④ 离屏截图：CaptureWindow 尺寸匹配、像素非全零；
//  ⑤ UIA 树：DumpUiTree 含按钮文本；
//  ⑥ 管理器命令分派：add/windows/capture/status/restore_all 全链路（AddWindow 显式
//     收纳不做进程树判定，本进程窗口可测）；
//  ⑧ 用户启动豁免：RegisterUserRoot 后 ConsiderWindow 不收纳；清除后宽限期
//     (1.2s) 满才收纳；对已收纳窗口登记兑底网立即还原；
//  ⑦ MP4 录像：CSandboxRecorder 跑 2 秒产出非空 mp4（ftyp 头）；
//  ⑧ 跨实例续收纳：退出还原留 adopt 标记 → 继任实例 Start 对工程内窗口续收纳，
//     工程外清标记；用户主动还原清标记；add 收编崩溃残留；标记超龄不再自动收
//     （修用户软件窗口反复消失/出现）；收纳者是活实例时互斥不越权（2026-10-09）；
//  ⑨ 2026-10-16 断链兜底收紧：IsParentAlive 纯逻辑（父活任意进程/断链/本体缺席）
//     + 工程根内新进程「父活（QuickTool 形态）不收纳 / 父死（cmd start 断链）收纳」
//     两组真窗实测（修 QuickTool 打开 HarnessAgent 被误幽灵化）。
// 需要真实 Win32 窗口：不进 offscreen 清单；无桌面会话环境自动 QSKIP。
// 2026.09.27 测试宿主窗口一律屏外定位（见 CHost 构造注释）：ctest 全量回归频繁
// 执行本用例，此前窗口在屏内会闪现用户桌面（"沙箱宿主"字样，用户反馈）。
#include <QtTest/QtTest>
#include <QApplication>
#include <QPushButton>
#include <QLineEdit>
#include <QLabel>
#include <QVBoxLayout>
#include <QTemporaryDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QProcess>        // 2026-10-09 NotepadNotGhosted：受保护应用实测
#include <QStandardPaths>  // 2026-10-09 ProtectedAppPath：TEMP 路径断言
#include <QDir>            // 2026-10-16 断链兜底实测：cmd start 参数原生分隔符
#include <functional>
#include <windows.h>
#include <tlhelp32.h>   // 2026-10-09 场景 F：找 explorer pid 伪活实例（需 windows.h 后置）
#include <shobjidl_core.h>   // 2026-10-16 MakeShortcut：IShellLinkW 建 .lnk（windows.h 后置）

#include "sandbox/CSandboxCore.h"
#include "sandbox/CSandboxManager.h"
#include "sandbox/CSandboxRecorder.h"

namespace {

// 等待条件成立（事件循环驱动投递消息落地）。
bool WaitCond(int a_iMs, const std::function<bool()>& a_fn) {
    QElapsedTimer t;
    t.start();
    while (t.elapsed() < a_iMs) {
        QApplication::processEvents(QEventLoop::AllEvents, 50);
        if (a_fn()) {
            return true;
        }
        Sleep(20);
    }
    return a_fn();
}

// 测试宿主窗口：按钮 + 输入框（native HWND）。
// 2026.09.27 不可见双轨（用户反馈：ctest 回归时桌面闪现"沙箱宿主"窗口）：
//   缺省屏外定位（Offscreen）——样式位/消息投递/UIA 树等不需要像素渲染的用例；
//   ShowGhosted()——需要 PrintWindow 像素的用例（截帧/录像）：屏内 + 先设
//   alpha=1 分层再 show（幽灵化先于首帧绘制，全程不可见）。注意 DWM 只合成
//   屏内窗口，屏外即使分层 PrintWindow 也拿黑帧（实测），故截帧用例必须屏内。
class CHost : public QWidget {
public:
    explicit CHost(QWidget* a_pParent = nullptr) : QWidget(a_pParent) {
        auto* pLay = new QVBoxLayout(this);
        m_pBtn = new QPushButton(QStringLiteral("点我测试"), this);
        m_pEdit = new QLineEdit(this);
        m_pLbl = new QLabel(QStringLiteral("沙箱宿主"), this);
        pLay->addWidget(m_pLbl);
        pLay->addWidget(m_pBtn);
        pLay->addWidget(m_pEdit);
        resize(320, 200);
        // 缺省屏外兑底：新增用例忘记选不可见策略也不会闪现用户桌面；
        // 屏外不影响被测行为（样式位/PostMessage 客户区坐标/UIA/IsWindowVisible 均不变）。
        move(-32000, -32000);
        m_iClicks = 0;
        connect(m_pBtn, &QPushButton::clicked, this, [this]() { ++m_iClicks; });
    }
    // 回屏内 + 立即幽灵化 + 显示：给截帧/录像用例（需要像素）。
    void ShowGhosted() {
        move(80, 80);                                                // 回屏内
        sbx::GhostWindow(reinterpret_cast<HWND>(winId()));           // 先 alpha=1 分层
        show();                                                      // 首帧即隐形
    }
    QPushButton* m_pBtn = nullptr;
    QLineEdit*   m_pEdit = nullptr;
    QLabel*      m_pLbl = nullptr;
    int          m_iClicks = 0;
};

// 按 pid 找子进程主窗口（UserRootExempt 用；EnumWindows 回调需函数指针）。
HWND  g_hFoundChild = nullptr;
DWORD g_dwFindPid = 0;
// 2026-10-08 ZeroGhostGrace 用例：孙窗口查找（排除自身/中间层两个 pid）。
DWORD g_dwSelfPidForFind = 0;
DWORD g_dwMidPidForFind = 0;
static BOOL CALLBACK FindWndByPidProc(HWND a_hWnd, LPARAM) {
    DWORD dwPid = 0;
    GetWindowThreadProcessId(a_hWnd, &dwPid);
    if (dwPid == g_dwFindPid && IsWindowVisible(a_hWnd)
            && GetAncestor(a_hWnd, GA_ROOT) == a_hWnd
            && GetWindowTextLengthW(a_hWnd) > 0) {
        g_hFoundChild = a_hWnd;
        return FALSE;
    }
    return TRUE;
}

// 2026-10-16 断链兜底实测：按「窗口进程 exe 规范化全路径」精确找窗（临时工程根
//   内的 notepad 副本——与真实被测程序同形态：exe 在工程根内、非保护路径）。
//   规范化（canonicalFilePath）解析 QTemporaryDir 的 8.3 短名（ADMINI~1）到
//   全长真实路径，与 QueryFullProcessImageNameW 口径一致才能对上。
QString g_sFindExePath;
static BOOL CALLBACK FindWndByExeProc(HWND a_hWnd, LPARAM) {
    if (!IsWindowVisible(a_hWnd) || GetAncestor(a_hWnd, GA_ROOT) != a_hWnd
            || GetWindowTextLengthW(a_hWnd) <= 0) {
        return TRUE;
    }
    DWORD dwPid = 0;
    GetWindowThreadProcessId(a_hWnd, &dwPid);
    if (dwPid == GetCurrentProcessId()) {
        return TRUE;   // 排除测试宿主自身窗口
    }
    const QString sExe = QDir::fromNativeSeparators(sbx::ExePathOfPid(dwPid)).toLower();
    if (sExe == g_sFindExePath) {
        g_hFoundChild = a_hWnd;
        return FALSE;
    }
    return TRUE;
}

// 2026-10-16 ProjectFallbackUserLaunchNotGhosted：把测试 exe + Qt 最小运行集
//   （Core/Gui/Widgets/Test、platforms\qwindows.dll、opengl32sw.dll）复制进目标
//   目录——explorer 启动的子进程环境无本工程 PATH，须目录内自足。
bool DeploySelfHost(const QString& a_sDir) {
    const QString sDllSrc = QDir(QCoreApplication::applicationDirPath())
                                .absoluteFilePath(QStringLiteral(".."));
    const QStringList aFiles = {
        QStringLiteral("Qt6Core.dll"), QStringLiteral("Qt6Gui.dll"),
        QStringLiteral("Qt6Widgets.dll"), QStringLiteral("Qt6Test.dll"),
        QStringLiteral("opengl32sw.dll"),
    };
    if (!QDir().mkpath(a_sDir + QStringLiteral("/platforms"))) {
        return false;
    }
    // exe 与 Qt DLL 分居两目录（exe=本进程文件；DLL=输出根），分列来源。
    if (!QFile::copy(QCoreApplication::applicationFilePath(),
                     a_sDir + QLatin1Char('/') + QFileInfo(
                         QCoreApplication::applicationFilePath()).fileName())) {
        return false;
    }
    // Qt DLL/平台插件尽力而为：拷成更稳（目录内自足）；失败不致命——explorer
    //   环境的用户 PATH 常含 Qt bin（本机即如此，实测无部署亦能起窗）。
    for (const QString& s : aFiles) {
        QFile::copy(sDllSrc + QLatin1Char('/') + s, a_sDir + QLatin1Char('/') + s);
    }
    QFile::copy(sDllSrc + QStringLiteral("/platforms/qwindows.dll"),
                a_sDir + QStringLiteral("/platforms/qwindows.dll"));
    return true;
}

// 2026-10-16 ProjectFallbackUserLaunchNotGhosted：建 .lnk（带参数）供 explorer
//   打开——目标进程由长驻 explorer 实例启动（父活、非本树），用户双击同构。
bool MakeShortcut(const QString& a_sExe, const QString& a_sArgs, const QString& a_sLnk) {
    HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    const bool bNeedUninit = SUCCEEDED(hr);
    IShellLinkW* pLnk = nullptr;
    IPersistFile* pFile = nullptr;
    bool bOk = false;
    if (SUCCEEDED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER,
                                   IID_IShellLinkW, reinterpret_cast<void**>(&pLnk)))) {
        pLnk->SetPath(reinterpret_cast<const wchar_t*>(a_sExe.utf16()));
        pLnk->SetArguments(reinterpret_cast<const wchar_t*>(a_sArgs.utf16()));
        if (SUCCEEDED(pLnk->QueryInterface(IID_IPersistFile,
                                           reinterpret_cast<void**>(&pFile)))) {
            bOk = SUCCEEDED(pFile->Save(
                reinterpret_cast<const wchar_t*>(a_sLnk.utf16()), TRUE));
            pFile->Release();
        }
        pLnk->Release();
    }
    if (bNeedUninit) {
        CoUninitialize();
    }
    return bOk;
}

// 2026-10-16 收尾：树杀子宿主进程（taskkill /T /F，与 CAgentRunnerBase 同口径；
//   child-host 的 app.exec() 不会自退）。
void KillPidTree(DWORD a_dwPid) {
    QProcess::execute(QStringLiteral("taskkill"),
                      { QStringLiteral("/PID"), QString::number(a_dwPid),
                        QStringLiteral("/T"), QStringLiteral("/F") });
}

} // namespace

class TestSandbox : public QObject {
    Q_OBJECT

private slots:
    void SkipCheck() {
        if (QGuiApplication::platformName() != QLatin1String("windows")) {
            QSKIP("需要真实 Win32 窗口（windows 平台）");
        }
    }

    // 2026-10-09 收纳范围收紧：受保护应用路径判定单元断言。
    void ProtectedAppPath() {
        // 系统/公共应用目录 → 保护（不收纳）。
        QVERIFY(sbx::IsProtectedAppPath(
            qEnvironmentVariable("SystemRoot") + QStringLiteral("\\notepad.exe")));
        QVERIFY(sbx::IsProtectedAppPath(
            qEnvironmentVariable("ProgramFiles") + QStringLiteral("\\Mozilla Firefox\\firefox.exe")));
        QVERIFY(sbx::IsProtectedAppPath(
            qEnvironmentVariable("ProgramFiles(x86)") + QStringLiteral("\\Microsoft\\Edge\\msedge.exe")));
        QVERIFY(sbx::IsProtectedAppPath(
            qEnvironmentVariable("LocalAppData") + QStringLiteral("\\Microsoft\\WindowsApps\\Notepad.exe")));
        // 大小写/斜杠方向不敏感。
        QString sWin = qEnvironmentVariable("SystemRoot");
        sWin.replace(QLatin1Char('\\'), QLatin1Char('/'));
        QVERIFY(sbx::IsProtectedAppPath(sWin.toUpper() + QStringLiteral("/NOTEPAD.EXE")));
        // 前缀近邻陷阱：目录名恰好以保护根开头不算保护（C:\Windows98 不是 C:\Windows）。
        QVERIFY(!sbx::IsProtectedAppPath(
            qEnvironmentVariable("SystemRoot") + QStringLiteral("98\\tool.exe")));
        // 工程/临时/用户目录 → 非保护（正常收纳范围）。
        QVERIFY(!sbx::IsProtectedAppPath(QCoreApplication::applicationFilePath()));
        QVERIFY(!sbx::IsProtectedAppPath(
            QStandardPaths::writableLocation(QStandardPaths::TempLocation) + QStringLiteral("\\x.exe")));
        QVERIFY(!sbx::IsProtectedAppPath(QString()));
    }

    // 2026-10-14 PID 复用防护（TuKan 误收养实害）：孤儿进程的真父退出后父 PID
    //   被 Agent 树内新进程复用——旧版仅凭 pid→ppid 快照判归属，链上无出生
    //   校验，会把孤儿误认成复用者（进而 Agent 树）的后代而幽灵化。纯哈希
    //   单测覆盖三分支：复用判伪 / 合法链放行 / 创建时刻不可得回退旧行为。
    void PidReuseAncestry() {
        const qint64 iT0 = 1770000000000LL;   // 任意基准时刻
        const DWORD dwRoot = 100, dwHolder = 200, dwChild = 300, dwMid = 400;
        QHash<DWORD, DWORD> aMap;
        QHash<DWORD, qint64> aCreated;
        // 场景①：子(300) 于 T0 出生，真父(旧 200) 已死；PID 200 被树内新进程
        //   复用（T0+5min 出生）→ 链 300→200→100 判伪，不得认定是 root 后代。
        aMap = { {dwChild, dwHolder}, {dwHolder, dwRoot} };
        aCreated = { {dwChild, iT0}, {dwHolder, iT0 + 5 * 60 * 1000}, {dwRoot, iT0 - 60 * 60 * 1000} };
        QVERIFY(!sbx::IsDescendantOf(dwChild, dwRoot, aMap, aCreated));
        // 场景②：合法链（父恒早于子出生）→ 放行（含多跳）。
        aMap = { {dwChild, dwMid}, {dwMid, dwRoot} };
        aCreated = { {dwChild, iT0 + 2000}, {dwMid, iT0 + 1000}, {dwRoot, iT0} };
        QVERIFY(sbx::IsDescendantOf(dwChild, dwRoot, aMap, aCreated));
        QVERIFY(sbx::IsDescendantOf(dwMid, dwRoot, aMap, aCreated));
        // 场景③：任一端创建时刻不可得（0：保护进程/权限）→ 回退旧行为不误伤。
        aCreated = { {dwChild, 0}, {dwMid, iT0 + 1000}, {dwRoot, 0} };
        QVERIFY(sbx::IsDescendantOf(dwChild, dwRoot, aMap, aCreated));
        // 场景④：空时刻表（旧签名缺省参）→ 完全旧行为。
        QVERIFY(sbx::IsDescendantOf(dwChild, dwRoot, aMap));
        // 场景⑤：链后段复用同样判伪（300→mid 合法，mid 的父 200 是复用者）。
        aMap = { {dwChild, dwMid}, {dwMid, dwHolder}, {dwHolder, dwRoot} };
        aCreated = { {dwChild, iT0 + 3000}, {dwMid, iT0 + 2000},
                     {dwHolder, iT0 + 5 * 60 * 1000}, {dwRoot, iT0} };
        QVERIFY(!sbx::IsDescendantOf(dwChild, dwRoot, aMap, aCreated));
        // 场景⑥：活表自检（真实系统快照含创建时刻且非空）。
        QHash<DWORD, DWORD> aLiveMap;
        QHash<DWORD, qint64> aLiveCreated;
        QVERIFY(sbx::BuildParentMap(aLiveMap, aLiveCreated));
        QVERIFY(aLiveMap.size() == aLiveCreated.size());
        QVERIFY(aLiveCreated.contains(GetCurrentProcessId()));
        QVERIFY(aLiveCreated.value(GetCurrentProcessId()) > 0);   // 自身时刻可得
        // 自身 → 自身直接命中。
        QVERIFY(sbx::IsDescendantOf(GetCurrentProcessId(), GetCurrentProcessId(),
                                   aLiveMap, aLiveCreated));
    }

    // 2026-10-16 断链兜底收紧（IsProjectTestPid 条件③）：由「父非 explorer」改
    //   「父已死才兑底」——用户经 QuickTool/托盘等任何非 explorer 启动器打开的
    //   工程内程序不得幽灵化（实害：QuickTool 打开 HarnessAgent.exe → 窗口
    //   alpha=1+点击穿透+无任务栏，「消失」但进程存活）。IsParentAlive 纯哈希
    //   单测覆盖：父活（任意进程，含 explorer）/ 断链（父不在表）/ 本体缺席竞态。
    void ParentAliveFallback() {
        const DWORD dwSelf = 300, dwQuickTool = 200, dwDeadParent = 250, dwExplorer = 101;
        // 场景①：父活着且非 explorer（QuickTool 等启动器）→ 兜底必须关。
        QHash<DWORD, DWORD> aMap = { {dwSelf, dwQuickTool}, {dwQuickTool, 100} };
        QVERIFY(sbx::IsParentAlive(dwSelf, aMap));
        // 场景②：断链——快照记录的父 pid 已不在表（父死，Agent 中间 shell 秒退）→ 允许兜底。
        aMap = { {dwSelf, dwDeadParent} };
        QVERIFY(!sbx::IsParentAlive(dwSelf, aMap));
        // 场景③：本体不在表（快照早于进程创建的竞态）→ 按断链口径（上层年龄闸把关）。
        QVERIFY(!sbx::IsParentAlive(999, aMap));
        // 场景④：explorer 父照常判活（并入「父活一律不兜底」统一口径，行为不回归）。
        aMap = { {dwSelf, dwExplorer}, {dwExplorer, 100} };
        QVERIFY(sbx::IsParentAlive(dwSelf, aMap));
        // 场景⑤：活表自检——自身父（ctest/测试宿主）必在表，判活。
        QHash<DWORD, DWORD> aLiveMap;
        QHash<DWORD, qint64> aLiveCreated;
        QVERIFY(sbx::BuildParentMap(aLiveMap, aLiveCreated));
        QVERIFY(sbx::IsParentAlive(GetCurrentProcessId(), aLiveMap));
    }

    // 2026-10-16 断链兜底收紧实测①（用户启动形态）：把测试 exe 连同 Qt 最小
    //   运行集部署进临时工程根（explorer 启动的子进程无本工程 PATH，须自足），
    //   .lnk（带 --sandbox-child-host 参数）交 explorer 打开 → 目标进程父 =
    //   长驻 explorer（活、非本树）→ 走「父活不兜底」分支：不得收纳。
    //   注：QuickTool 形态（活父且非 explorer）无法从树内伪造（测试所起的一切
    //   进程皆本树后代），由 ParentAliveFallback 纯逻辑覆盖；2026-10-16 真机
    //   现场（QuickTool→HarnessAgent 被吞→修后不吞）已实证该分支。
    void ProjectFallbackUserLaunchNotGhosted() {
        if (QGuiApplication::platformName() != QLatin1String("windows")) {
            QSKIP("需要真实 Win32 窗口（windows 平台）");
        }
        QTemporaryDir oDir;                       // 临时工程根（含被测 exe 副本）
        QVERIFY(DeploySelfHost(oDir.path()));
        const QString sExe = oDir.path() + QStringLiteral("/")
                             + QFileInfo(QCoreApplication::applicationFilePath()).fileName();
        const QString sLnk = oDir.path() + QStringLiteral("/probe.lnk");
        QVERIFY(MakeShortcut(sExe, QStringLiteral("--sandbox-child-host"), sLnk));
        CSandboxManager stMgr;
        CSandboxManager::SSbxCfg stCfg;
        stCfg.bEnable = true;
        stCfg.bAutoGhost = true;
        stCfg.bRecord = false;
        stMgr.SetConfig(stCfg);
        stMgr.SetProjectRoot(oDir.path());
        stMgr.Start();

        QVERIFY(QProcess::startDetached(QStringLiteral("explorer.exe"),
                               { QDir::toNativeSeparators(sLnk) }));
        HWND hChild = nullptr;
        QVERIFY(WaitCond(15000, [&]() {
            g_hFoundChild = nullptr;
            g_sFindExePath = QFileInfo(sExe).canonicalFilePath().toLower();
            EnumWindows(FindWndByExeProc, 0);
            return g_hFoundChild != nullptr;
        }));
        hChild = g_hFoundChild;
        QVERIFY(hChild != nullptr);
        DWORD dwPid = 0;
        GetWindowThreadProcessId(hChild, &dwPid);

        stMgr.ConsiderWindow(hChild);             // 钩子同口径（非终判，留宽限复判）
        QTest::qWait(2500);                       // 覆盖宽限期（若误兜底此处会收走）
        QVERIFY2(!sbx::IsGhosted(hChild), "父活（用户启动形态）+ 工程根内新进程不得兜底收纳");
        QCOMPARE(stMgr.GhostedCount(), 0);

        stMgr.Shutdown();
        KillPidTree(dwPid);
    }

    // 2026-10-16 断链兜底收紧实测②（Agent 通道回归）：cmd start 中间人秒退断链
    //   （父已死）+ exe 在工程根内 + 新建 → 兜底收纳照常生效（修复不回归）。
    void ProjectFallbackOrphanChainGhosted() {
        if (QGuiApplication::platformName() != QLatin1String("windows")) {
            QSKIP("需要真实 Win32 窗口（windows 平台）");
        }
        const QString sExe = QCoreApplication::applicationFilePath();
        CSandboxManager stMgr;
        CSandboxManager::SSbxCfg stCfg;
        stCfg.bEnable = true;
        stCfg.bAutoGhost = true;
        stCfg.bRecord = false;
        stMgr.SetConfig(stCfg);
        stMgr.SetProjectRoot(QCoreApplication::applicationDirPath());
        stMgr.Start();

        QProcess stMid;                        // 中间人：执行完即退 → 被测进程父死断链
        stMid.setProgram(QStringLiteral("cmd.exe"));
        stMid.setArguments({ QStringLiteral("/c"), QStringLiteral("start"), QString(),
                             QDir::toNativeSeparators(sExe),
                             QStringLiteral("--sandbox-child-host") });
        stMid.start();
        QVERIFY(stMid.waitForFinished(8000));  // cmd start 立即返回退出
        HWND hChild = nullptr;
        QVERIFY(WaitCond(12000, [&]() {
            g_hFoundChild = nullptr;
            g_sFindExePath = QFileInfo(sExe).canonicalFilePath().toLower();
            EnumWindows(FindWndByExeProc, 0);
            return g_hFoundChild != nullptr;
        }));
        hChild = g_hFoundChild;
        QVERIFY(hChild != nullptr);
        DWORD dwPid = 0;
        GetWindowThreadProcessId(hChild, &dwPid);

        stMgr.ConsiderWindow(hChild, true);    // 终判：断链兑底应收纳
        QVERIFY2(WaitCond(5000, [&]() { return sbx::IsGhosted(hChild); }),
                 "断链（父死）+ 工程根内新进程应兑底收纳");
        QVERIFY(stMgr.GhostedCount() >= 1);
        stMgr.Shutdown();                      // 还原窗口停录像
        QVERIFY(!sbx::IsGhosted(hChild));
        KillPidTree(dwPid);
    }

    // 2026-10-09 收纳范围收紧实测：系统应用（notepad，树内子进程启动）不得被
    //   自动收纳，也不得被 sandbox add 收纳——旧逻辑会立即收走（用户实测
    //   「Agent 开报告/浏览器给用户看时窗口凭空消失」）。
    void NotepadNotGhosted() {
        if (QGuiApplication::platformName() != QLatin1String("windows")) {
            QSKIP("需要真实 Win32 窗口（windows 平台）");
        }
        const QString sNotepad = qEnvironmentVariable("SystemRoot")
                                 + QStringLiteral("\\notepad.exe");
        if (!QFileInfo::exists(sNotepad)) {
            QSKIP("notepad.exe 不存在");
        }
        CSandboxManager stMgr;
        CSandboxManager::SSbxCfg stCfg;
        stCfg.bEnable = true;
        stCfg.bAutoGhost = true;
        stCfg.bRecord = false;
        stMgr.SetConfig(stCfg);
        stMgr.Start();

        QProcess stProc;
        stProc.setProgram(sNotepad);
        stProc.start();
        QVERIFY(stProc.waitForStarted(5000));
        const DWORD dwPid = static_cast<DWORD>(stProc.processId());

        // 等它出主窗口（可见顶层且属该 pid）。
        HWND hNp = nullptr;
        QVERIFY(WaitCond(10000, [&]() {
            g_hFoundChild = nullptr;
            g_dwFindPid = dwPid;
            EnumWindows(FindWndByPidProc, 0);
            return g_hFoundChild != nullptr;
        }));
        hNp = g_hFoundChild;
        QVERIFY(hNp != nullptr);

        // 自动收纳路径（钩子/扫描 + 宽限终判）：2.5 秒内不得幽灵化。
        QTest::qWait(2500);
        QVERIFY2(!sbx::IsGhosted(hNp), "系统应用（notepad）不得被自动收纳");

        // 显式 add：受保护应用必须拒绝。
        QString sOut;
        QJsonObject oAdd;
        oAdd.insert(QStringLiteral("hwnd"),
                    QStringLiteral("0x%1").arg(reinterpret_cast<quintptr>(hNp), 0, 16));
        QVERIFY2(!stMgr.Execute(QStringLiteral("add"), oAdd, sOut),
                 "sandbox add 必须拒绝受保护应用");
        QVERIFY(!sbx::IsGhosted(hNp));

        stProc.terminate();
        stProc.waitForFinished(3000);
        if (stProc.state() == QProcess::Running) {
            stProc.kill();
            stProc.waitForFinished(2000);
        }
        stMgr.Shutdown();
    }

    void ParseCliCases() {
        QString sCmd;
        QJsonObject o;
        QVERIFY(sbx::ParseCli({ QStringLiteral("AutoCode.exe"), QStringLiteral("--sandbox"),
                                QStringLiteral("windows") }, &sCmd, &o));
        QCOMPARE(sCmd, QStringLiteral("windows"));

        QVERIFY(sbx::ParseCli({ QStringLiteral("AutoCode.exe"), QStringLiteral("--sandbox"),
                                QStringLiteral("click"), QStringLiteral("0x1A2B"),
                                QStringLiteral("10"), QStringLiteral("20"), QStringLiteral("dbl") },
                              &sCmd, &o));
        QCOMPARE(sCmd, QStringLiteral("click"));
        QCOMPARE(o.value(QStringLiteral("hwnd")).toString(), QStringLiteral("0x1A2B"));
        QCOMPARE(o.value(QStringLiteral("x")).toInt(), 10);
        QCOMPARE(o.value(QStringLiteral("y")).toInt(), 20);
        QCOMPARE(o.value(QStringLiteral("dbl")).toBool(), true);

        QVERIFY(sbx::ParseCli({ QStringLiteral("AutoCode.exe"), QStringLiteral("--sandbox"),
                                QStringLiteral("type"), QStringLiteral("1234"),
                                QStringLiteral("你好"), QStringLiteral("世界") }, &sCmd, &o));
        QCOMPARE(o.value(QStringLiteral("text")).toString(), QStringLiteral("你好 世界"));

        QVERIFY(sbx::ParseCli({ QStringLiteral("AutoCode.exe"), QStringLiteral("--sandbox"),
                                QStringLiteral("restore-all") }, &sCmd, &o));
        QCOMPARE(sCmd, QStringLiteral("restore_all"));

        // 2026-10-14 launch：exe + 余参重建成 args 串（含空格元素补引号）。
        QVERIFY(sbx::ParseCli({ QStringLiteral("AutoCode.exe"), QStringLiteral("--sandbox"),
                                QStringLiteral("launch"), QStringLiteral("C:\\a b\\app.exe"),
                                QStringLiteral("--mode"), QStringLiteral("fast run") },
                              &sCmd, &o));
        QCOMPARE(sCmd, QStringLiteral("launch"));
        QCOMPARE(o.value(QStringLiteral("exe")).toString(), QStringLiteral("C:\\a b\\app.exe"));
        QCOMPARE(o.value(QStringLiteral("args")).toString(),
                 QStringLiteral("--mode \"fast run\""));
        QVERIFY(!sbx::ParseCli({ QStringLiteral("AutoCode.exe"), QStringLiteral("--sandbox"),
                                 QStringLiteral("launch") }, &sCmd, &o));

        QVERIFY(!sbx::ParseCli({ QStringLiteral("AutoCode.exe"), QStringLiteral("--sandbox"),
                                 QStringLiteral("不存在命令") }, &sCmd, &o));
        QVERIFY(!sbx::ParseCli({ QStringLiteral("AutoCode.exe"), QStringLiteral("--sandbox") },
                               &sCmd, &o));
        // hwnd 十进制/十六进制解析。
        HWND h = nullptr;
        QVERIFY(sbx::ParseHwnd(QStringLiteral("65536"), &h));
        QCOMPARE(reinterpret_cast<quintptr>(h), quintptr(65536));
        QVERIFY(sbx::ParseHwnd(QStringLiteral("0xFFFF"), &h));
        QCOMPARE(reinterpret_cast<quintptr>(h), quintptr(0xFFFF));
        QVERIFY(!sbx::ParseHwnd(QStringLiteral("abc"), &h));
    }

    void GhostRestore() {
        CHost stHost;
        stHost.show();
        QVERIFY(WaitCond(2000, [&]() { return stHost.isVisible(); }));
        const HWND h = reinterpret_cast<HWND>(stHost.winId());

        const LONG_PTR lOrig = GetWindowLongPtrW(h, GWL_EXSTYLE);
        QVERIFY(!sbx::IsGhosted(h));
        QVERIFY(sbx::GhostWindow(h));
        QVERIFY(sbx::IsGhosted(h));
        const LONG_PTR lGhost = GetWindowLongPtrW(h, GWL_EXSTYLE);
        QVERIFY((lGhost & WS_EX_LAYERED) != 0);
        QVERIFY((lGhost & WS_EX_TRANSPARENT) != 0);
        QVERIFY((lGhost & WS_EX_NOACTIVATE) != 0);
        QVERIFY((lGhost & WS_EX_TOOLWINDOW) != 0);
        // 幽灵化幂等。
        QVERIFY(!sbx::GhostWindow(h));

        QVERIFY(sbx::RestoreWindow(h));
        QVERIFY(!sbx::IsGhosted(h));
        QCOMPARE(GetWindowLongPtrW(h, GWL_EXSTYLE), lOrig);
        // 未收纳窗口还原失败。
        QVERIFY(!sbx::RestoreWindow(h));
    }

    void InputClick() {
        CHost stHost;
        stHost.show();
        QVERIFY(WaitCond(2000, [&]() { return stHost.isVisible(); }));
        const HWND h = reinterpret_cast<HWND>(stHost.winId());

        // 按钮中心点（客户区坐标）后台点击。
        const QPoint ptBtn = stHost.m_pBtn->rect().center();
        const QPoint ptTop = stHost.m_pBtn->mapTo(&stHost, QPointF(ptBtn)).toPoint();
        QVERIFY(sbx::ClickWindow(h, ptTop.x(), ptTop.y(), false, false));
        QVERIFY(WaitCond(2000, [&]() { return stHost.m_iClicks >= 1; }));
        QCOMPARE(stHost.m_iClicks, 1);

        // 双击 = 两次 clicked。
        QVERIFY(sbx::ClickWindow(h, ptTop.x(), ptTop.y(), true, false));
        QVERIFY(WaitCond(2000, [&]() { return stHost.m_iClicks >= 3; }));

        // 文本输入：先点输入框获得 Qt 内部焦点（WM_CHAR 路由到焦点控件，
        // 与真人操作顺序一致——Agent 用法同样是先 click 后 type），再输入。
        const QPoint ptEdit = stHost.m_pEdit->mapTo(&stHost,
            QPointF(stHost.m_pEdit->rect().center())).toPoint();
        QVERIFY(sbx::ClickWindow(h, ptEdit.x(), ptEdit.y(), false, false));
        QVERIFY(WaitCond(2000, [&]() {
            return stHost.m_pEdit->hasFocus(); }));
        QVERIFY(sbx::TypeText(h, QStringLiteral("hello")));
        QVERIFY(WaitCond(2000, [&]() { return stHost.m_pEdit->text() == QStringLiteral("hello"); }));
        // backspace 删一字符。
        QVERIFY(sbx::SendKeys(h, QStringLiteral("back")));
        QVERIFY(WaitCond(2000, [&]() { return stHost.m_pEdit->text() == QStringLiteral("hell"); }));
    }

    void CaptureAndTree() {
        CHost stHost;
        stHost.ShowGhosted();   // 屏内 + 先幽灵化再 show：像素用例（见 CHost 注释）
        QVERIFY(WaitCond(2000, [&]() { return stHost.isVisible(); }));
        const HWND h = reinterpret_cast<HWND>(stHost.winId());
        QVERIFY(sbx::IsGhosted(h));   // ShowGhosted 已分层（与生产截屏对象一致）

        const QImage img = sbx::CaptureWindow(h);
        QVERIFY(!img.isNull());
        // PrintWindow 取的是 GetWindowRect（含 DWM 不可见边框），与 frameGeometry 对齐。
        RECT rcWin = {};
        GetWindowRect(reinterpret_cast<HWND>(stHost.winId()), &rcWin);
        QCOMPARE(img.width(), static_cast<int>(rcWin.right - rcWin.left));
        QCOMPARE(img.height(), static_cast<int>(rcWin.bottom - rcWin.top));
        QVERIFY(img.width() >= stHost.width() && img.height() >= stHost.height());
        // 像素非全零（Ghost 后 PrintWindow 仍重绘）。
        long lSum = 0;
        for (int y = 0; y < img.height(); y += 7) {
            for (int x = 0; x < img.width(); x += 7) {
                lSum += img.pixel(x, y) & 0xFFFFFF;
            }
        }
        QVERIFY(lSum > 0);

        const QString sTree = sbx::DumpUiTree(h);
        QVERIFY(sTree.contains(QStringLiteral("点我测试")));
        QVERIFY(sTree.contains(QStringLiteral("click=")));
    }

    void ManagerCommands() {
        CSandboxManager stMgr;
        CSandboxManager::SSbxCfg stCfg;
        stCfg.bEnable = true;
        stCfg.bAutoGhost = false;   // 测试显式收纳路径
        stCfg.bRecord = false;      // 命令链路不启录像（录像单独测）
        stMgr.SetConfig(stCfg);
        stMgr.Start();

        CHost stHost;
        stHost.show();
        QVERIFY(WaitCond(2000, [&]() { return stHost.isVisible(); }));
        const HWND h = reinterpret_cast<HWND>(stHost.winId());
        const QString sH = QStringLiteral("0x%1").arg(reinterpret_cast<quintptr>(h), 0, 16);

        QString sOut;
        // windows：空表。
        QVERIFY(stMgr.Execute(QStringLiteral("windows"), QJsonObject(), sOut));
        QVERIFY(sOut.contains(QStringLiteral("暂无窗口")));

        // add：显式收纳（不做进程树判定）。
        QJsonObject oAdd;
        oAdd.insert(QStringLiteral("hwnd"), sH);
        QVERIFY(stMgr.Execute(QStringLiteral("add"), oAdd, sOut));
        QVERIFY(stMgr.Execute(QStringLiteral("windows"), QJsonObject(), sOut));
        QVERIFY(sOut.contains(sH.right(4)) || sOut.contains(QStringLiteral("点我测试")));
        QCOMPARE(stMgr.GhostedCount(), 1);

        // capture：返回 PNG 路径且文件存在。
        QJsonObject oCap;
        oCap.insert(QStringLiteral("hwnd"), sH);
        QVERIFY(stMgr.Execute(QStringLiteral("capture"), oCap, sOut));
        QVERIFY(sOut.endsWith(QStringLiteral(".png"), Qt::CaseInsensitive));
        QVERIFY(QFile::exists(sOut));

        // click（后台投递）。
        CHost* pHost = &stHost;
        QJsonObject oClick;
        oClick.insert(QStringLiteral("hwnd"), sH);
        const QPoint ptTop = pHost->m_pBtn->mapTo(pHost, QPointF(pHost->m_pBtn->rect().center())).toPoint();
        oClick.insert(QStringLiteral("x"), ptTop.x());
        oClick.insert(QStringLiteral("y"), ptTop.y());
        QVERIFY(stMgr.Execute(QStringLiteral("click"), oClick, sOut));
        QVERIFY(WaitCond(2000, [&]() { return pHost->m_iClicks >= 1; }));

        // status / restore_all。
        QVERIFY(stMgr.Execute(QStringLiteral("status"), QJsonObject(), sOut));
        QVERIFY2(sOut.contains(QStringLiteral("在册窗口 1 个")), qUtf8Printable(sOut));
        QVERIFY(stMgr.Execute(QStringLiteral("restore_all"), QJsonObject(), sOut));
        QCOMPARE(stMgr.GhostedCount(), 0);
        QVERIFY(!sbx::IsGhosted(h));

        // 未知命令。
        QVERIFY(!stMgr.Execute(QStringLiteral("no_such"), QJsonObject(), sOut));

        stMgr.Shutdown();
    }

    void UserRootExempt() {
        // ⑧ 用户启动豁免（2026.09.27）：登记 pid 后 ConsiderWindow 不收纳不幽灵化；
        //    清除豁免后宽限期（1.2s）满才收纳；对已收纳窗口登记兑底网立即还原。
        //    候选窗用真实子进程（TreePid 判后代成立；自身窗口不是自己的后代）。
        //    子进程用测试 exe 自身的 --sandbox-child-host 模式：钩子在登记落地前
        //    触发时（ShellExecuteEx 泵消息竞态同构），宽限期复判必须能等到登记。
        CSandboxManager stMgr;
        CSandboxManager::SSbxCfg stCfg;
        stCfg.bEnable = true;
        stCfg.bAutoGhost = true;
        stCfg.bRecord = false;
        stMgr.SetConfig(stCfg);
        stMgr.Start();

        QProcess stProc;
        stProc.setProcessChannelMode(QProcess::ForwardedChannels);
        stProc.start(QCoreApplication::applicationFilePath(),
                     { QStringLiteral("--sandbox-child-host") });
        QVERIFY(stProc.waitForStarted(5000));
        const DWORD dwChildPid = static_cast<DWORD>(stProc.processId());

        // 等子进程主窗口出现（可见顶层 + 归属子 pid）。
        HWND hChild = nullptr;
        QVERIFY(WaitCond(8000, [&]() {
            g_hFoundChild = nullptr;
            g_dwFindPid = dwChildPid;
            EnumWindows(FindWndByPidProc, 0);
            return g_hFoundChild != nullptr;
        }));
        hChild = g_hFoundChild;
        QVERIFY(hChild != nullptr);
        QVERIFY(!sbx::IsGhosted(hChild));

        // ① 先让判定在登记前跑（竞态同构）→ 再登记 pid → 宽限期复判应豁免。
        QVERIFY(!stMgr.ConsiderWindow(hChild));   // 未登记：入宽限队列（不立即收纳）
        CSandboxManager::RegisterUserRoot(dwChildPid, nullptr);
        QVERIFY(!sbx::IsGhosted(hChild));
        QTest::qWait(1600);   // > kGhostGraceMs(1200)：宽限复判应豁免
        QVERIFY2(!sbx::IsGhosted(hChild), "registered pid must never be ghosted");
        QCOMPARE(stMgr.GhostedCount(), 0);

        // ② 清除豁免 → 宽限排队（不立即收纳）→ 期满后收纳。
        CSandboxManager::ClearUserRoots();
        QVERIFY(!stMgr.ConsiderWindow(hChild));   // 入宽限队列，立即返回 false
        QVERIFY(WaitCond(4000, [&]() { return sbx::IsGhosted(hChild); }));
        QCOMPARE(stMgr.GhostedCount(), 1);

        // ③ 登记已收纳窗口的 pid → 兑底网立即还原（竞态场景）。
        CSandboxManager::RegisterUserRoot(dwChildPid, nullptr);
        QVERIFY(!sbx::IsGhosted(hChild));
        QCOMPARE(stMgr.GhostedCount(), 0);

        stProc.kill();
        QVERIFY(stProc.waitForFinished(3000));
        stMgr.Shutdown();
        CSandboxManager::ClearUserRoots();   // 防污染后续用例
    }

    // 2026-10-08 零闪现收纳回归（SmartStore 工程实测：回归跑批反复启被测程序，
    // 旧逻辑一律先排 1.2s 宽限 → 每个窗口都先裸奔一下，用户桌面反复闪窗）。
    // 孙进程窗口（隔代后代 = 生产 Agent 链形态）首次判定即收纳，不进宽限队列。
    void ZeroGhostGrace() {
        CSandboxManager stMgr;
        CSandboxManager::SSbxCfg stCfg;
        stCfg.bEnable = true;
        stCfg.bAutoGhost = true;
        stCfg.bRecord = false;
        stMgr.SetConfig(stCfg);
        stMgr.Start();

        QProcess stMid;   // 中间层（≈runner），孙窗口的直接父是它、不是测试进程
        stMid.setProcessChannelMode(QProcess::ForwardedChannels);
        stMid.start(QCoreApplication::applicationFilePath(),
                    { QStringLiteral("--sandbox-grand-host") });
        QVERIFY(stMid.waitForStarted(5000));

        // 等孙窗口出现（可见顶层、pid != 测试进程/中间层 = 孙进程）。
        const DWORD dwSelf = static_cast<DWORD>(QCoreApplication::applicationPid());
        const DWORD dwMid = static_cast<DWORD>(stMid.processId());
        g_dwSelfPidForFind = dwSelf;
        g_dwMidPidForFind = dwMid;
        HWND hGrand = nullptr;
        QVERIFY(WaitCond(10000, [&]() {
            g_hFoundChild = nullptr;
            EnumWindows([](HWND h, LPARAM) {
                DWORD dwPid = 0;
                GetWindowThreadProcessId(h, &dwPid);
                if (dwPid != 0 && dwPid != g_dwSelfPidForFind && dwPid != g_dwMidPidForFind
                        && IsWindowVisible(h)) {
                    wchar_t wsTitle[128] = {};
                    GetWindowTextW(h, wsTitle, 128);
                    if (wcsstr(wsTitle, L"sandbox-child-host") != nullptr) {
                        g_hFoundChild = h;
                    }
                }
                return TRUE;
            }, 0);
            return g_hFoundChild != nullptr;
        }));
        hGrand = g_hFoundChild;
        QVERIFY(hGrand != nullptr);

        // 隔代后代零宽限：窗口 SHOW 起，钩子（EVENT_OBJECT_SHOW，毫秒级投递）或
        // 扫描兑底应立即收纳——旧逻辑下首次判定一律排 1.2s 宽限队列（窗口裸奔），
        // 即使钩子即时到达也要等期满；新逻辑首次判定即 GhostAndTrack。
        // 上限 2500ms：钩子即时路径外留扫描兜底（1.2s 周期）+ 事件循环余量。
        QVERIFY2(WaitCond(2500, [&]() { return sbx::IsGhosted(hGrand); }),
                 "隔代后代应在宽限期(800ms)内即被收纳（零闪现）");
        QCOMPARE(stMgr.GhostedCount(), 1);

        DWORD dwGrandPid = 0;
        GetWindowThreadProcessId(hGrand, &dwGrandPid);
        stMgr.Shutdown();
        // 收尾杀整棵测试树：中间层 kill 后孙进程成孤儿不会跟着退（首跑实测挂住
        // 锁 exe），须按 pid 单独终结，否则后续构建 LNK1104。
        stMid.kill();
        stMid.waitForFinished(3000);
        if (dwGrandPid != 0) {
            if (HANDLE hG = OpenProcess(PROCESS_TERMINATE, FALSE, dwGrandPid)) {
                TerminateProcess(hG, 1);
                CloseHandle(hG);
            }
        }
    }

    // 2026-10-14 断链兜底回归（Compositer 工程实测「测试窗口一直弹出」根因）：
    //   Agent 用 powershell Start-Process / cmd start 等中间人方式启动被测程序，
    //   中间 shell 秒退 → 被测程序主窗口弹出时父进程已死，进程树爬链第一跳即断，
    //   旧版不收纳 → 窗口裸奍弹到用户桌面。新逻辑：树外但 exe 在工程根内且进程
    //   新建 → 兑底收纳（工程外仍拒绝）。
    void BrokenChainFallback() {
        // 中间人启动（生产形态）：cmd /c start → cmd 立即退出，被测程序成孤儿。
        const QString sExe = QCoreApplication::applicationFilePath();
        QVERIFY(QProcess::startDetached(QStringLiteral("cmd.exe"),
            { QStringLiteral("/c"), QStringLiteral("start"), QString(),
              sExe, QStringLiteral("--sandbox-child-host") }));

        // 等孤儿窗口出现（可见顶层、标题含 sandbox-child-host）。
        HWND hKid = nullptr;
        QVERIFY(WaitCond(10000, [&]() {
            hKid = nullptr;
            EnumWindows([](HWND h, LPARAM lp) {
                wchar_t wsTitle[128] = {};
                if (GetWindowTextW(h, wsTitle, 128) > 0 && IsWindowVisible(h)
                        && GetAncestor(h, GA_ROOT) == h
                        && wcsstr(wsTitle, L"sandbox-child-host") != nullptr) {
                    *reinterpret_cast<HWND*>(lp) = h;
                    return FALSE;
                }
                return TRUE;
            }, reinterpret_cast<LPARAM>(&hKid));
            return hKid != nullptr;
        }));
        QVERIFY(hKid != nullptr);
        DWORD dwKidPid = 0;
        GetWindowThreadProcessId(hKid, &dwKidPid);

        CSandboxManager stMgr;
        CSandboxManager::SSbxCfg stCfg;
        stCfg.bEnable = true;
        stCfg.bAutoGhost = true;
        stCfg.bRecord = false;
        stMgr.SetConfig(stCfg);

        // 反例：工程根指向无关目录 → 不兑底（窗口保持可见）。
        QTemporaryDir stDir;
        stMgr.SetProjectRoot(stDir.path());
        stMgr.Start();
        QVERIFY2(!stMgr.ConsiderWindow(hKid), "工程外孤儿不得兑底收纳");
        QTest::qWait(1200);
        QVERIFY2(!sbx::IsGhosted(hKid), "工程外孤儿不得被鬼化");

        // 正例：工程根指向 exe 所在目录（生产中即被测工程产物目录）→ 兑底收纳
        //   （宽限 800ms 终判后鬼化；钩子/扫描自动路径同样覆盖）。
        stMgr.SetProjectRoot(QCoreApplication::applicationDirPath());
        QVERIFY2(WaitCond(4000, [&]() { return sbx::IsGhosted(hKid); }),
                 "工程内新建孤儿窗口应被断链兑底收纳");
        QCOMPARE(stMgr.GhostedCount(), 1);

        // 清场：杀孤儿进程（防后续构建 LNK1104）。
        if (dwKidPid != 0) {
            if (HANDLE hP = OpenProcess(PROCESS_TERMINATE, FALSE, dwKidPid)) {
                TerminateProcess(hP, 1);
                CloseHandle(hP);
            }
        }
        stMgr.Shutdown();
    }

    // 2026-10-14 sandbox launch：主进程代启动被测程序 + 手工根登记——窗口弹出
    //   即收纳（TreePid 命中手工根，零宽限直收），不受断链影响。
    void SandboxLaunchCommand() {
        CSandboxManager stMgr;
        CSandboxManager::SSbxCfg stCfg;
        stCfg.bEnable = true;
        stCfg.bAutoGhost = true;
        stCfg.bRecord = false;
        stMgr.SetConfig(stCfg);
        stMgr.Start();

        QString sOut;
        // 反例：exe 不存在。
        QJsonObject oBad;
        oBad.insert(QStringLiteral("exe"), QStringLiteral("C:/__no_such_app__.exe"));
        QVERIFY2(!stMgr.Execute(QStringLiteral("launch"), oBad, sOut),
                 "exe 不存在必须失败");

        // 正例：启动自身 child-host 模式。
        QJsonObject oLaunch;
        oLaunch.insert(QStringLiteral("exe"), QCoreApplication::applicationFilePath());
        oLaunch.insert(QStringLiteral("args"), QStringLiteral("--sandbox-child-host"));
        QVERIFY(stMgr.Execute(QStringLiteral("launch"), oLaunch, sOut));
        QVERIFY2(sOut.contains(QStringLiteral("pid=")), qUtf8Printable(sOut));

        // 窗口弹出即自动收纳（手工根零宽限；上限含进程冷启动）。
        QVERIFY2(WaitCond(8000, [&]() { return stMgr.GhostedCount() >= 1; }),
                 "launch 启动的窗口应即弹即收");
        QVERIFY(stMgr.Execute(QStringLiteral("windows"), QJsonObject(), sOut));
        QVERIFY(sOut.contains(QStringLiteral("sandbox-child-host")));

        // 清场：从在册行拿 pid 终结子进程。
        const QList<CSandboxManager::SRow> aRows = stMgr.Rows();
        for (const CSandboxManager::SRow& st : aRows) {
            if (HANDLE hP = OpenProcess(PROCESS_TERMINATE, FALSE, st.dwPid)) {
                TerminateProcess(hP, 1);
                CloseHandle(hP);
            }
        }
        stMgr.Shutdown();
    }

    // 2026-10-08 跨实例续收纳回归（Compositer 工程实测：AutoCode 实例重启后，
    // 退出还原的测试窗口裸奔用户桌面，且新实例进程树出身判定永不再认它）。
    void ReadoptAcrossRestart() {
        // 场景 C：用户主动还原（默认语义）应连「曾收纳」标记一并清。
        {
            CHost stHost;
            stHost.show();
            QVERIFY(WaitCond(2000, [&]() { return stHost.isVisible(); }));
            const HWND h = reinterpret_cast<HWND>(stHost.winId());
            QVERIFY(sbx::GhostWindow(h));
            QVERIFY(sbx::HasAdoptMark(h));        // 收纳时打上
            QVERIFY(sbx::RestoreWindow(h));       // 用户语义（清 adopt）
            QVERIFY(!sbx::IsGhosted(h));
            QVERIFY(!sbx::HasAdoptMark(h));
        }
        // 场景 A（主场景）：上个实例「退出还原」留 adopt 标记 → 新实例 Start 时
        // 窗口 exe 在工程目录内 → 续收纳重新入册（窗口回到隐形）。
        {
            CHost stHost;
            stHost.show();
            QVERIFY(WaitCond(2000, [&]() { return stHost.isVisible(); }));
            const HWND h = reinterpret_cast<HWND>(stHost.winId());
            QVERIFY(sbx::GhostWindow(h));
            QVERIFY(sbx::RestoreWindow(h, true)); // 退出还原：保留 adopt 标记
            QVERIFY(!sbx::IsGhosted(h) && sbx::HasAdoptMark(h));   // 可见但带标记

            CSandboxManager stMgr;                // 模拟继任实例
            CSandboxManager::SSbxCfg stCfg;
            stCfg.bEnable = true;
            stCfg.bAutoGhost = false;             // 不走钩子，隔离考察续收纳路径
            stCfg.bRecord = false;
            stMgr.SetConfig(stCfg);
            stMgr.SetProjectRoot(QCoreApplication::applicationDirPath()); // exe 本身在此目录
            stMgr.Start();
            QCOMPARE(stMgr.GhostedCount(), 1);     // 续收纳入册
            QVERIFY(sbx::IsGhosted(h));            // 重新隐形
            stMgr.Shutdown();                     // 清场（退出还原又留标记，窗口析构销毁 prop）
        }
        // 场景 B：工程外路径 → 不收纳，仅清 adopt 标记（恢复常态）。
        {
            CHost stHost;
            stHost.show();
            QVERIFY(WaitCond(2000, [&]() { return stHost.isVisible(); }));
            const HWND h = reinterpret_cast<HWND>(stHost.winId());
            QVERIFY(sbx::GhostWindow(h));
            QVERIFY(sbx::RestoreWindow(h, true));
            QVERIFY(sbx::HasAdoptMark(h));

            QTemporaryDir stDir;
            CSandboxManager stMgr;
            CSandboxManager::SSbxCfg stCfg;
            stCfg.bEnable = true;
            stCfg.bAutoGhost = false;
            stCfg.bRecord = false;
            stMgr.SetConfig(stCfg);
            stMgr.SetProjectRoot(stDir.path());   // 与本进程 exe 无关的目录
            stMgr.Start();
            QCOMPARE(stMgr.GhostedCount(), 0);
            QVERIFY(!sbx::IsGhosted(h));
            QVERIFY(!sbx::HasAdoptMark(h));       // 标记清除，窗口恢复常态
            stMgr.Shutdown();
        }
        // 场景 E（2026-10-09 用户实测回归）：「曾收纳」标记超龄 → 不再自动收走。
        //   工程内软件被 Agent 测过一次后，标记永久跟随，此前每次启动/切工程都被
        //   强制隐形、退出又还原 → 用户桌面反复「消失/出现」。时效窗后超龄=清标记。
        {
            CHost stHost;
            stHost.show();
            QVERIFY(WaitCond(2000, [&]() { return stHost.isVisible(); }));
            const HWND h = reinterpret_cast<HWND>(stHost.winId());
            QVERIFY(sbx::GhostWindow(h));
            QVERIFY(sbx::RestoreWindow(h, true));          // 退出还原：标记保留（时刻=now）
            QVERIFY(sbx::HasAdoptMark(h));
            // 伪造超龄标记（20 分钟前）——与 core 的 kPropAdopt 同名直写。
            SetPropW(h, sbx::kPropAdopt, reinterpret_cast<HANDLE>(
                static_cast<intptr_t>(static_cast<ULONG>(
                    GetTickCount() - 20UL * 60 * 1000))));

            CSandboxManager stMgr;                          // 模拟继任实例（启动很晚）
            CSandboxManager::SSbxCfg stCfg;
            stCfg.bEnable = true;
            stCfg.bAutoGhost = false;
            stCfg.bRecord = false;
            stMgr.SetConfig(stCfg);
            stMgr.SetProjectRoot(QCoreApplication::applicationDirPath());  // exe 在工程内
            stMgr.Start();
            QCOMPARE(stMgr.GhostedCount(), 0);              // 超龄：不收
            QVERIFY(!sbx::IsGhosted(h));                    // 用户可见
            QVERIFY(!sbx::HasAdoptMark(h));                 // 标记清除，恢复常态
            stMgr.Shutdown();
        }
        // 场景 F（2026-10-09 多实例互斥）：幽灵中 + 收纳者是活着的其他实例 →
        //   本实例不得越权还原/续收（此前实例 B 的启动扫描会把实例 A 沙箱里的
        //   测试窗口强制还原，打断 A 的 Agent 测试）。
        {
            CHost stHost;
            stHost.show();
            QVERIFY(WaitCond(2000, [&]() { return stHost.isVisible(); }));
            const HWND h = reinterpret_cast<HWND>(stHost.winId());
            QVERIFY(sbx::GhostWindow(h));                   // 收纳者=本测试进程
            QVERIFY(sbx::IsGhosted(h));
            // 伪造「别的活实例收纳中」：kPropGhost 值改成 explorer 的 pid（必活，
            // 且不是本进程）。与 core 的 kPropGhost 同名直写。
            DWORD dwExplorer = 0;
            {
                HANDLE hSnap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
                PROCESSENTRY32W stPe = { sizeof(stPe) };
                if (hSnap != INVALID_HANDLE_VALUE) {
                    if (Process32FirstW(hSnap, &stPe)) {
                        do {
                            if (wcscmp(stPe.szExeFile, L"explorer.exe") == 0) {
                                dwExplorer = stPe.th32ProcessID;
                                break;
                            }
                        } while (Process32NextW(hSnap, &stPe));
                    }
                    CloseHandle(hSnap);
                }
            }
            QVERIFY2(dwExplorer != 0, "找不到 explorer 进程");
            SetPropW(h, sbx::kPropGhost,
                     reinterpret_cast<HANDLE>(static_cast<intptr_t>(dwExplorer)));

            CSandboxManager stMgr;
            CSandboxManager::SSbxCfg stCfg;
            stCfg.bEnable = true;
            stCfg.bAutoGhost = false;
            stCfg.bRecord = false;
            stMgr.SetConfig(stCfg);
            stMgr.SetProjectRoot(QCoreApplication::applicationDirPath());
            stMgr.Start();
            QCOMPARE(stMgr.GhostedCount(), 0);              // 别人在管：不入册
            QVERIFY(sbx::IsGhosted(h));                     // 也不越权还原（保持原状）
            stMgr.Shutdown();
            // 清场：恢复 owner=本进程再正常还原（清标记）。
            SetPropW(h, sbx::kPropGhost, reinterpret_cast<HANDLE>(
                static_cast<intptr_t>(GetCurrentProcessId())));
            QVERIFY(sbx::RestoreWindow(h));
        }
        // 场景 D：Agent 显式 add 收编「已幽灵但不在册」的崩溃残留窗口。
        {
            CHost stHost;
            stHost.show();
            QVERIFY(WaitCond(2000, [&]() { return stHost.isVisible(); }));
            const HWND h = reinterpret_cast<HWND>(stHost.winId());
            QVERIFY(sbx::GhostWindow(h));         // 模拟崩溃残留（隐形、无管理器在册）

            CSandboxManager stMgr;
            CSandboxManager::SSbxCfg stCfg;
            stCfg.bEnable = true;
            stCfg.bAutoGhost = false;
            stCfg.bRecord = false;
            stMgr.SetConfig(stCfg);
            stMgr.Start();
            QString sOut;
            QJsonObject oAdd;
            oAdd.insert(QStringLiteral("hwnd"),
                        QStringLiteral("0x%1").arg(reinterpret_cast<quintptr>(h), 0, 16));
            QVERIFY(stMgr.Execute(QStringLiteral("add"), oAdd, sOut));
            QCOMPARE(stMgr.GhostedCount(), 1);     // 收编入册
            QVERIFY(sbx::IsGhosted(h));
            stMgr.Execute(QStringLiteral("restore_all"), QJsonObject(), sOut); // 清场（清 adopt）
        }
    }

    void HookPostRaceSurvives() {
        // 2026-10-08 崩溃回归（Compositer 实测 ACCESS_VIOLATION）：钩子线程
        //   HookProc→invokeMethod 的 postEvent receiver=管理器对象，load 与投递之间
        //   管理器销毁 → Qt lockThreadPostEventList 读悬垂 threadData 崩溃。
        //   修复后 HookProc 投递 receiver=qApp（永不悬垂）+ lambda 主线程 atomic 复查。
        //   本用例反复「建管理器→SHOW 风暴→立即销毁」逼出竞态窗口：修复前可在
        //   processEvents 中崩；修复后迟到投递被 atomic 复查丢弃，全程存活。
        if (QGuiApplication::platformName() != QLatin1String("windows")) {
            QSKIP("需要真实 Win32 窗口（windows 平台）");
        }
        QTemporaryDir stDir;
        QVERIFY(stDir.isValid());
        for (int iRound = 0; iRound < 4; ++iRound) {
            auto* pMgr = new CSandboxManager(this);
            CSandboxManager::SSbxCfg stCfg;
            stCfg.bEnable    = true;
            stCfg.bAutoGhost = true;    // 自动收纳：HookProc→ConsiderWindow 链路活跃
            stCfg.bRecord    = false;
            pMgr->SetConfig(stCfg);
            pMgr->SetProjectRoot(stDir.path());
            pMgr->Start();
            // SHOW 风暴：3 个窗口 show 触发 EVENT_OBJECT_SHOW → 钩子线程排队 qApp
            QList<CHost*> aW;
            for (int k = 0; k < 3; ++k) {
                auto* pW = new CHost();
                pW->setWindowTitle(QStringLiteral("hookrace-%1-%2").arg(iRound).arg(k));
                aW.append(pW);
                pW->show();
            }
            QApplication::processEvents(QEventLoop::AllEvents, 80);
            // 立即销毁：HookProc 的迟到投递在 processEvents 中被 atomic 复查丢弃
            pMgr->Shutdown();
            delete pMgr;
            QApplication::processEvents(QEventLoop::AllEvents, 300);
            qDeleteAll(aW);
            QApplication::processEvents(QEventLoop::AllEvents, 100);
        }
        QVERIFY(true);
    }

    void RecorderMp4() {
        CHost stHost;
        stHost.ShowGhosted();   // 屏内 + 先幽灵化再 show：像素用例（见 CHost 注释）
        QVERIFY(WaitCond(2000, [&]() { return stHost.isVisible(); }));
        const HWND h = reinterpret_cast<HWND>(stHost.winId());

        QTemporaryDir stDir;
        QVERIFY(stDir.isValid());
        const QString sPath = stDir.path() + QStringLiteral("/rec_test.mp4");
        CSandboxRecorder stRec(h, 10, sPath);
        QElapsedTimer t;
        t.start();
        stRec.start();
        while (t.elapsed() < 2200) {
            QApplication::processEvents(QEventLoop::AllEvents, 50);
        }
        stRec.RequestStop();
        QVERIFY(stRec.wait(8000));
        QVERIFY(QFile::exists(sPath));
        QVERIFY(QFileInfo(sPath).size() > 4096);   // 非空 mp4（ftyp+moov+帧数据）
        // MP4 头校验：ftyp box。
        QFile f(sPath);
        QVERIFY(f.open(QIODevice::ReadOnly));
        const QByteArray aHead = f.read(12);
        f.close();
        QVERIFY(aHead.size() >= 8);
        QVERIFY(aHead.mid(4, 4) == QByteArray("ftyp", 4));
    }
};

// 自定义 main：支持 --sandbox-child-host 子进程模式（UserRootExempt 的被测窗口宿主）；
// --sandbox-grand-host = 再拉一层 child-host（孙进程宿主，ZeroGhostGrace 用例用——
// 生产 Agent 链 AutoCode→runner→bash→python→被测程序 同为「隔代后代」形态）。
// 其余参数走标准 QTest 执行（原 QTEST_MAIN 展开）。
int main(int argc, char** argv) {
    for (int i = 1; i < argc; ++i) {
        if (qstrcmp(argv[i], "--sandbox-grand-host") == 0) {
            QApplication app(argc, argv);
            QProcess stKid;
            stKid.setProcessChannelMode(QProcess::ForwardedChannels);
            stKid.start(QCoreApplication::applicationFilePath(),
                        { QStringLiteral("--sandbox-child-host") });
            if (!stKid.waitForStarted(5000)) {
                return 1;
            }
            return stKid.waitForFinished(-1) ? 0 : 1;
        }
        if (qstrcmp(argv[i], "--sandbox-child-host") == 0) {
            QApplication app(argc, argv);
            CHost stHost;
            stHost.setWindowTitle(QStringLiteral("sandbox-child-host"));
            stHost.show();
            return app.exec();
        }
    }
    QApplication app(argc, argv);
    app.setAttribute(Qt::AA_Use96Dpi, true);
    TestSandbox tc;
    return QTest::qExec(&tc, argc, argv);
}
#include "test_sandbox.moc"
