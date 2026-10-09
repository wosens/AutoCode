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
//     工程外清标记；用户主动还原清标记；add 收编崩溃残留。
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
#include <functional>
#include <windows.h>

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

} // namespace

class TestSandbox : public QObject {
    Q_OBJECT

private slots:
    void SkipCheck() {
        if (QGuiApplication::platformName() != QLatin1String("windows")) {
            QSKIP("需要真实 Win32 窗口（windows 平台）");
        }
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
