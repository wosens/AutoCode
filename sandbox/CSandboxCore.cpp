// 内置测试沙箱核心能力层实现（说明见同名 .h）。
#include "CSandboxCore.h"

#include <QJsonObject>
#include <QRect>
#include <QSet>
#include <QFileInfo>
#include <QDir>       // 2026-10-09 IsProtectedAppPath：路径规范化
#include <QElapsedTimer>
#include <QCoreApplication>
#include <QRegularExpression>
#include <tlhelp32.h>
#include <dwmapi.h>
// 全局 WIN32_LEAN_AND_MEAN 裁掉了 ole2，UIA/任务栏接口需先补 COM 基础头。
#include <objbase.h>
#include <ole2.h>
#include <oleauto.h>
#include <shobjidl_core.h>
#include <UIAutomation.h>
#include <shobjidl.h>

#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "oleaut32.lib")

// 幽灵化标记（SetProp；崩溃后新实例可据此还原）。2026-10-09 移入 sbx 并解除
//   文件内限定：单元测试需直接访问 prop 名伪造跨实例场景（越权保护/标记超龄）。
namespace sbx {
const wchar_t* kPropGhost   = L"AutoCodeSbxGhost.1";   // 值 = 收纳者 PID
const wchar_t* kPropOrigEx  = L"AutoCodeSbxOrigEx.1";  // 值 = 原 GWL_EXSTYLE
// 2026-10-08 「曾收纳」标记：AutoCode 退出还原在册窗口时保留（区别于用户主动
//   还原——那会连此标记一并清）。新实例启动/切工程时扫到它：窗口进程 exe 在
//   当前工程目录内 → 续收纳重新隐形（否则窗口裸奔在用户桌面，Compositer 实测）。
// 2026-10-09 值改存打标/还原时刻 GetTickCount()（原恒为 1 无时效）：续收纳只认
//   「最近还原」的窗口——超龄说明 Agent 早不再测它，用户可能正在用，不得再收
//   （用户实测：工程内软件窗口反复自动消失/出现，就是无时效标记被反复续收）。
const wchar_t* kPropAdopt   = L"AutoCodeSbxAdopt.1";
} // namespace sbx

namespace {

// 永不收纳的窗口类（控制台 host / 提示 / 阴影等辅助窗口）。
bool IsDeniedWndClass(const QString& a_sCls) {
    static const QStringList aDeny = {
        QStringLiteral("ConsoleWindowClass"), QStringLiteral("tooltips_class32"),
        QStringLiteral("tooltips_class"), QStringLiteral("SysShadow"),
        QStringLiteral("MSCTFIME UI"), QStringLiteral("ComboLBox"),
    };
    return aDeny.contains(a_sCls, Qt::CaseInsensitive);
}

// 永不收纳的系统宿主进程名（小写）。
bool IsDeniedHostExe(const QString& a_sExeBaseLower) {
    static const QSet<QString> sDeny = {
        QStringLiteral("explorer.exe"), QStringLiteral("applicationframehost.exe"),
        QStringLiteral("dllhost.exe"), QStringLiteral("svchost.exe"),
        QStringLiteral("conhost.exe"), QStringLiteral("runtimebroker.exe"),
        QStringLiteral("searchhost.exe"), QStringLiteral("startmenuexperiencehost.exe"),
        QStringLiteral("shellexperiencehost.exe"), QStringLiteral("textinputhost.exe"),
        QStringLiteral("taskhostw.exe"), QStringLiteral("sihost.exe"),
        QStringLiteral("ctfmon.exe"), QStringLiteral("dwm.exe"),
        QStringLiteral("fontdrvhost.exe"), QStringLiteral("pythonw.exe"),
    };
    return sDeny.contains(a_sExeBaseLower);
}

// WM_KEYDOWN/UP 的 lParam（扫描码 + 重复/转换位）。
LPARAM KeyLParam(UINT a_uVk, bool a_bUp) {
    const UINT uScan = MapVirtualKeyW(a_uVk, MAPVK_VK_TO_VSC);
    LPARAM lp = static_cast<LPARAM>(uScan) << 16 | 1;   // repeat=1
    if (a_bUp) {
        lp |= (1 << 30) | (1 << 31);   // KF_UP + previous state
    }
    return lp;
}

} // namespace

namespace sbx {

// ---------------------------------------------------------------------------
// 进程树归属
// ---------------------------------------------------------------------------
bool BuildParentMap(QHash<DWORD, DWORD>& a_oMap, QHash<DWORD, qint64>& a_oCreated) {
    a_oMap.clear();
    a_oCreated.clear();
    HANDLE hSnap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (hSnap == INVALID_HANDLE_VALUE) {
        return false;
    }
    // 2026-10-14 进程创建时刻（FILETIME 100ns/1601 纪元 → Unix ms）。逐 pid
    //   OpenProcess+GetProcessTimes（几百进程量级毫秒级开销；表 500ms 才重建
    //   一次）；拿不到（System 等保护进程）记 0 = 校验时视为不可得，保持旧行为。
    const auto fnCreateMs = [](DWORD a_dwPid) -> qint64 {
        if (a_dwPid == 0) {
            return 0;
        }
        HANDLE hProc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, a_dwPid);
        if (hProc == nullptr) {
            return 0;
        }
        FILETIME ftCreate = {}, ftExit = {}, ftKernel = {}, ftUser = {};
        qint64 iMs = 0;
        if (GetProcessTimes(hProc, &ftCreate, &ftExit, &ftKernel, &ftUser)) {
            LARGE_INTEGER li;
            li.HighPart = ftCreate.dwHighDateTime;
            li.LowPart = ftCreate.dwLowDateTime;
            iMs = (li.QuadPart - 116444736000000000LL) / 10000;
        }
        CloseHandle(hProc);
        return iMs;
    };
    PROCESSENTRY32W stPe = {};
    stPe.dwSize = sizeof(stPe);
    if (Process32FirstW(hSnap, &stPe)) {
        do {
            a_oMap.insert(stPe.th32ProcessID, stPe.th32ParentProcessID);
            a_oCreated.insert(stPe.th32ProcessID, fnCreateMs(stPe.th32ProcessID));
        } while (Process32NextW(hSnap, &stPe));
    }
    CloseHandle(hSnap);
    return !a_oMap.isEmpty();
}

bool IsParentAlive(DWORD a_dwPid, const QHash<DWORD, DWORD>& a_oMap) {
    // 见 CSandboxCore.h 2026-10-16 注：父 pid 仍是表键 = 父活着；值非 0 但键缺席
    //   = 断链（父死）；本体缺席 = 快照竞态，按断链口径交上层年龄闸。
    const DWORD dwPpid = a_oMap.value(a_dwPid, 0);
    return dwPpid != 0 && a_oMap.contains(dwPpid);
}

bool IsDescendantOf(DWORD a_dwPid, DWORD a_dwRoot, const QHash<DWORD, DWORD>& a_oMap,
                    const QHash<DWORD, qint64>& a_oCreated) {
    if (a_dwRoot == 0 || a_dwPid == 0) {
        return false;
    }
    DWORD dwCur = a_dwPid;
    qint64 iCurMs = a_oCreated.value(dwCur, 0);
    for (int i = 0; i < 64 && dwCur != 0; ++i) {   // 防环上限
        if (dwCur == a_dwRoot) {
            return true;
        }
        const DWORD dwNext = a_oMap.value(dwCur, 0);
        if (dwNext == 0) {
            break;
        }
        // 2026-10-14 PID 复用防护：父必须不晚于子出生。双方创建时刻都可得且父
        //   更年轻 = 当前持有该父 PID 的是复用者（真父已死，子是孤儿），此链是
        //   假归属 → 立即判否。任一端不可得（0：保护进程/权限）保持旧行为，
        //   不影响正常归属判定（真父创建恒早于子，无假阴性）。
        const qint64 iNextMs = a_oCreated.value(dwNext, 0);
        if (iCurMs > 0 && iNextMs > 0 && iNextMs > iCurMs) {
            return false;
        }
        dwCur = dwNext;
        iCurMs = iNextMs;
    }
    return false;
}

QString ExePathOfPid(DWORD a_dwPid) {
    if (a_dwPid == 0) {
        return QString();
    }
    HANDLE hProc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, a_dwPid);
    if (hProc == nullptr) {
        return QString();
    }
    wchar_t wsBuf[MAX_PATH] = {0};
    DWORD dwLen = MAX_PATH;
    QString sOut;
    if (QueryFullProcessImageNameW(hProc, 0, wsBuf, &dwLen) && dwLen > 0) {
        sOut = QString::fromWCharArray(wsBuf, static_cast<int>(dwLen));
    }
    CloseHandle(hProc);
    return sOut;
}

// ---------------------------------------------------------------------------
// 窗口信息
// ---------------------------------------------------------------------------
QString WndTitle(HWND a_hWnd) {
    const int iLen = GetWindowTextLengthW(a_hWnd);
    if (iLen <= 0) {
        return QString();
    }
    std::wstring ws(static_cast<size_t>(iLen) + 1, L'\0');
    const int iGot = GetWindowTextW(a_hWnd, &ws[0], iLen + 1);
    return QString::fromWCharArray(ws.c_str(), qMax(iGot, 0));
}

QString WndClass(HWND a_hWnd) {
    wchar_t wsBuf[256] = {0};
    const int iGot = GetClassNameW(a_hWnd, wsBuf, 256);
    return (iGot > 0) ? QString::fromWCharArray(wsBuf, iGot) : QString();
}

// ---------------------------------------------------------------------------
// 幽灵化 / 还原
// ---------------------------------------------------------------------------
bool GhostWindow(HWND a_hWnd) {
    if (!IsWindow(a_hWnd) || IsGhosted(a_hWnd)) {
        return false;
    }
    const LONG_PTR lEx = GetWindowLongPtrW(a_hWnd, GWL_EXSTYLE);
    if (!SetPropW(a_hWnd, kPropGhost,
                  reinterpret_cast<HANDLE>(static_cast<intptr_t>(GetCurrentProcessId())))) {
        return false;
    }
    SetPropW(a_hWnd, kPropOrigEx, reinterpret_cast<HANDLE>(static_cast<intptr_t>(lEx)));
    SetPropW(a_hWnd, kPropAdopt, reinterpret_cast<HANDLE>(
        static_cast<intptr_t>(static_cast<ULONG>(GetTickCount()))));   // 曾收纳（时刻；退出还原时刷新）
    const LONG_PTR lNew = lEx | WS_EX_LAYERED | WS_EX_TRANSPARENT
                        | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW;
    SetLastError(0);   // SetWindowLongPtr 成功可合法返回 0（原值即 0），先清防残留误判
    SetWindowLongPtrW(a_hWnd, GWL_EXSTYLE, lNew);
    if (GetLastError() != 0) {
        RemovePropW(a_hWnd, kPropGhost);
        RemovePropW(a_hWnd, kPropOrigEx);
        return false;   // 提权/跨会话等改不动：安全侧不收纳
    }
    SetLayeredWindowAttributes(a_hWnd, 0, 1, LWA_ALPHA);   // alpha=1：视觉不可见仍重绘
    // 已在任务栏上的按钮摘除（新建窗口因 TOOLWINDOW 不会再上任务栏/Alt-Tab）。
    if (ITaskbarList* pList = nullptr;
        SUCCEEDED(CoCreateInstance(CLSID_TaskbarList, nullptr, CLSCTX_INPROC_SERVER,
                                   IID_PPV_ARGS(&pList))) && pList != nullptr) {
        pList->DeleteTab(a_hWnd);
        pList->Release();
    }
    return true;
}

bool RestoreWindow(HWND a_hWnd, bool a_bKeepAdoptMark) {
    if (!IsWindow(a_hWnd)) {
        return false;
    }
    const HANDLE hVal = GetPropW(a_hWnd, kPropGhost);
    if (hVal == nullptr) {
        return false;   // 非本产品收纳的窗口
    }
    const LONG_PTR lOrig = reinterpret_cast<LONG_PTR>(
        GetPropW(a_hWnd, kPropOrigEx));
    SetWindowLongPtrW(a_hWnd, GWL_EXSTYLE, lOrig);
    // 2026.09.28 修复条件写反：原已分层窗口还原后 alpha 仍停在幽灵化的 1（持续不可见）；
    //   原本非分层窗口则无 layered 位、SetLayeredWindowAttributes 本就无效。
    if ((lOrig & WS_EX_LAYERED) != 0) {
        // 原本就是分层窗口：恢复不透明（否则 alpha=1 残留，还原后仍看不见）。
        SetLayeredWindowAttributes(a_hWnd, 0, 255, LWA_ALPHA);
    }
    RemovePropW(a_hWnd, kPropGhost);
    RemovePropW(a_hWnd, kPropOrigEx);
    // 2026-10-08 用户主动还原（默认）连「曾收纳」标记一并清——之后新实例不会再
    //   自动续收它；AutoCode 退出路径传 true 保留，供继任实例识别续收。
    if (!a_bKeepAdoptMark) {
        RemovePropW(a_hWnd, kPropAdopt);
    } else {
        // 退出还原：刷新「曾收纳」时刻——继任实例的时效窗从还原起算（收纳后
        //   AutoCode 连续跑数小时再退出，旧时刻已超龄会被误判成「用户在用」）。
        SetPropW(a_hWnd, kPropAdopt, reinterpret_cast<HANDLE>(
            static_cast<intptr_t>(static_cast<ULONG>(GetTickCount()))));
    }
    if (ITaskbarList* pList = nullptr;
        SUCCEEDED(CoCreateInstance(CLSID_TaskbarList, nullptr, CLSCTX_INPROC_SERVER,
                                   IID_PPV_ARGS(&pList))) && pList != nullptr) {
        pList->AddTab(a_hWnd);
        pList->Release();
    }
    return true;
}

struct SRestoreCtx {
    int  iCount = 0;
    bool bKeepAdopt = false;
};

BOOL CALLBACK RestoreMarkedProc(HWND a_hWnd, LPARAM a_lParam) {
    auto* pCtx = reinterpret_cast<SRestoreCtx*>(a_lParam);
    if (GetPropW(a_hWnd, kPropGhost) != nullptr) {
        RestoreWindow(a_hWnd, pCtx->bKeepAdopt);
        ++pCtx->iCount;
    }
    return TRUE;
}

int RestoreAllMarked(bool a_bKeepAdoptMark) {
    SRestoreCtx stCtx;
    stCtx.bKeepAdopt = a_bKeepAdoptMark;
    EnumWindows(RestoreMarkedProc, reinterpret_cast<LPARAM>(&stCtx));
    return stCtx.iCount;
}

bool HasAdoptMark(HWND a_hWnd) {
    return IsWindow(a_hWnd) && GetPropW(a_hWnd, kPropAdopt) != nullptr;
}

// 2026-10-09 「曾收纳」时刻（GetTickCount 快照；无标记返回 0）。年龄判定用无符号
//   减法（now - tick）自动处理 49.7 天回绕。
ULONG AdoptMarkTick(HWND a_hWnd) {
    if (!IsWindow(a_hWnd)) {
        return 0;
    }
    const HANDLE hVal = GetPropW(a_hWnd, kPropAdopt);
    return hVal != nullptr ? static_cast<ULONG>(
        static_cast<ULONG_PTR>(reinterpret_cast<intptr_t>(hVal))) : 0;
}

// 2026-10-09 幽灵中窗口的收纳者 PID（kPropGhost 值；非幽灵返回 0）——多实例
//   互斥用：别的活实例正在管理的窗口，本实例不得越权还原/续收。
DWORD GhostOwnerPid(HWND a_hWnd) {
    if (!IsWindow(a_hWnd)) {
        return 0;
    }
    const HANDLE hVal = GetPropW(a_hWnd, kPropGhost);
    return hVal != nullptr ? static_cast<DWORD>(
        static_cast<ULONG_PTR>(reinterpret_cast<intptr_t>(hVal))) : 0;
}

void ClearAdoptMark(HWND a_hWnd) {
    if (IsWindow(a_hWnd)) {
        RemovePropW(a_hWnd, kPropAdopt);
    }
}

bool IsGhosted(HWND a_hWnd) {
    return IsWindow(a_hWnd) && GetPropW(a_hWnd, kPropGhost) != nullptr;
}

bool IsGhostableShape(HWND a_hWnd) {
    if (!IsWindow(a_hWnd) || !IsWindowVisible(a_hWnd)) {
        return false;
    }
    if (GetAncestor(a_hWnd, GA_ROOT) != a_hWnd) {
        return false;   // 只收纳顶层窗口
    }
    // 属主窗口必须同进程（模态对话框属主即主窗口；跨进程属主的极少见，跳过）。
    if (const HWND hOwner = GetWindow(a_hWnd, GW_OWNER); hOwner != nullptr) {
        DWORD dwSelf = 0, dwOwner = 0;
        GetWindowThreadProcessId(a_hWnd, &dwSelf);
        GetWindowThreadProcessId(hOwner, &dwOwner);
        if (dwSelf != dwOwner) {
            return false;
        }
    }
    DWORD dwPid = 0;
    GetWindowThreadProcessId(a_hWnd, &dwPid);
    if (dwPid == 0 || dwPid == GetCurrentProcessId()) {
        return false;   // 本进程（主界面/弹窗）绝不收纳
    }
    if (IsDeniedWndClass(WndClass(a_hWnd))) {
        return false;
    }
    if (IsGhosted(a_hWnd)) {
        return false;
    }
    RECT rc = {};
    if (!GetWindowRect(a_hWnd, &rc)) {
        return false;
    }
    const int iW = rc.right - rc.left, iH = rc.bottom - rc.top;
    if (iW < 40 || iH < 28 || static_cast<qint64>(iW) * iH > 8 * 1024 * 1024) {
        return false;   // 太小（碎片/提示）或超大异常
    }
    // DWM cloaked（UWP 挂起态/虚拟桌面）不收纳。
    DWORD dwCloaked = 0;
    if (SUCCEEDED(DwmGetWindowAttribute(a_hWnd, DWMWA_CLOAKED,
                                        &dwCloaked, sizeof(dwCloaked))) && dwCloaked != 0) {
        return false;
    }
    return true;
}

bool IsSystemHostPid(DWORD a_dwPid) {
    const QString sExe = ExePathOfPid(a_dwPid);
    if (sExe.isEmpty()) {
        return false;
    }
    const QString sBase = QFileInfo(sExe).fileName().toLower();
    return IsDeniedHostExe(sBase);
}

// 2026-10-09 受保护应用目录（惰性初始化；环境变量在进程生命周期内不变）。
//   前缀比较统一补 '/'，防「C:\Windows」误吞「C:\Windows98\x.exe」这类近邻路径。
QStringList ProtectedAppRoots() {
    static const QStringList s_aRoots = [] {
        QStringList aOut;
        const auto fnAdd = [&aOut](const QString& sDir) {
            QString s = QDir::fromNativeSeparators(sDir).toLower();
            while (s.endsWith(QLatin1Char('/'))) {
                s.chop(1);
            }
            if (!s.isEmpty()) {
                aOut << s;
            }
        };
        fnAdd(qEnvironmentVariable("SystemRoot"));          // C:\Windows（记事本/注册表工具…）
        fnAdd(qEnvironmentVariable("windir"));              // 兼容别名
        fnAdd(qEnvironmentVariable("ProgramFiles"));        // C:\Program Files
        fnAdd(qEnvironmentVariable("ProgramFiles(x86)"));   // C:\Program Files (x86)
        fnAdd(qEnvironmentVariable("LocalAppData")
              + QStringLiteral("\\Microsoft\\WindowsApps")); // UWP 执行别名（Win11 记事本等）
        return aOut;
    }();
    return s_aRoots;
}

bool IsProtectedAppPath(const QString& a_sExe) {
    if (a_sExe.isEmpty()) {
        return false;
    }
    QString sExe = QDir::fromNativeSeparators(a_sExe).toLower();
    while (sExe.endsWith(QLatin1Char('/'))) {
        sExe.chop(1);
    }
    for (const QString& sRoot : ProtectedAppRoots()) {
        if (sExe == sRoot || sExe.startsWith(sRoot + QLatin1Char('/'))) {
            return true;
        }
    }
    return false;
}

// ---------------------------------------------------------------------------
// 离屏截图
// ---------------------------------------------------------------------------
QImage CaptureWindow(HWND a_hWnd) {
    if (!IsWindow(a_hWnd)) {
        return QImage();
    }
    RECT rc = {};
    if (!GetWindowRect(a_hWnd, &rc)) {
        return QImage();
    }
    const int iW = qMin<qint64>(rc.right - rc.left, 8192);
    const int iH = qMin<qint64>(rc.bottom - rc.top, 8192);
    if (iW <= 0 || iH <= 0) {
        return QImage();
    }
    HDC hScreen = GetDC(nullptr);
    if (hScreen == nullptr) {
        return QImage();
    }
    HDC hMem = CreateCompatibleDC(hScreen);
    BITMAPINFO stBi = {};
    stBi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    stBi.bmiHeader.biWidth = iW;
    stBi.bmiHeader.biHeight = -iH;   // top-down
    stBi.bmiHeader.biPlanes = 1;
    stBi.bmiHeader.biBitCount = 32;
    stBi.bmiHeader.biCompression = BI_RGB;
    void* pBits = nullptr;
    HBITMAP hBmp = CreateDIBSection(hScreen, &stBi, DIB_RGB_COLORS, &pBits, nullptr, 0);
    if (hBmp == nullptr || pBits == nullptr) {
        DeleteDC(hMem);
        ReleaseDC(nullptr, hScreen);
        return QImage();
    }
    const HGDIOBJ hOld = SelectObject(hMem, hBmp);
    // PW_RENDERFULLCONTENT：DirectComposition/DX 内容也能取到；失败回退窗口 DC 位拷。
    if (!PrintWindow(a_hWnd, hMem, PW_RENDERFULLCONTENT)) {
        HDC hWnd = GetWindowDC(a_hWnd);
        if (hWnd != nullptr) {
            BitBlt(hMem, 0, 0, iW, iH, hWnd, 0, 0, SRCCOPY | CAPTUREBLT);
            ReleaseDC(a_hWnd, hWnd);
        }
    }
    SelectObject(hMem, hOld);
    // BGRA → QImage（ARGB32 内存序一致），copy() 脱离 DIB 生命周期。
    QImage img = QImage(static_cast<const uchar*>(pBits), iW, iH, QImage::Format_ARGB32)
                     .copy();
    DeleteObject(hBmp);
    DeleteDC(hMem);
    ReleaseDC(nullptr, hScreen);
    return img;
}

// ---------------------------------------------------------------------------
// 消息级输入
// ---------------------------------------------------------------------------
bool ClickWindow(HWND a_hWnd, int a_iX, int a_iY, bool a_bDouble, bool a_bRight) {
    if (!IsWindow(a_hWnd)) {
        return false;
    }
    const LPARAM lp = MAKELPARAM(a_iX, a_iY);
    PostMessageW(a_hWnd, WM_MOUSEMOVE, 0, lp);
    if (a_bRight) {
        PostMessageW(a_hWnd, WM_RBUTTONDOWN, MK_RBUTTON, lp);
        Sleep(40);
        PostMessageW(a_hWnd, WM_RBUTTONUP, 0, lp);
        return true;
    }
    PostMessageW(a_hWnd, WM_LBUTTONDOWN, MK_LBUTTON, lp);
    Sleep(45);
    PostMessageW(a_hWnd, WM_LBUTTONUP, 0, lp);
    if (a_bDouble) {
        Sleep(30);
        PostMessageW(a_hWnd, WM_LBUTTONDBLCLK, MK_LBUTTON, lp);
        Sleep(45);
        PostMessageW(a_hWnd, WM_LBUTTONUP, 0, lp);
    }
    return true;
}

bool DragWindow(HWND a_hWnd, int a_iX1, int a_iY1, int a_iX2, int a_iY2, int a_iMs) {
    if (!IsWindow(a_hWnd)) {
        return false;
    }
    PostMessageW(a_hWnd, WM_MOUSEMOVE, 0, MAKELPARAM(a_iX1, a_iY1));
    PostMessageW(a_hWnd, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(a_iX1, a_iY1));
    const int iSteps = qBound(4, a_iMs / 16, 60);
    for (int i = 1; i <= iSteps; ++i) {
        const int iX = a_iX1 + (a_iX2 - a_iX1) * i / iSteps;
        const int iY = a_iY1 + (a_iY2 - a_iY1) * i / iSteps;
        PostMessageW(a_hWnd, WM_MOUSEMOVE, MK_LBUTTON, MAKELPARAM(iX, iY));
        Sleep(qMax(1, a_iMs / iSteps));
    }
    Sleep(30);
    PostMessageW(a_hWnd, WM_LBUTTONUP, 0, MAKELPARAM(a_iX2, a_iY2));
    return true;
}

bool TypeText(HWND a_hWnd, const QString& a_sText) {
    if (!IsWindow(a_hWnd)) {
        return false;
    }
    // 逐 UTF-16 码元投 WM_CHAR（代理对拆两帧；多数输入控件直接消费）。
    QString sNorm = a_sText;
    sNorm.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
    for (const QChar ch : sNorm) {
        const WCHAR w = (ch == QChar('\n')) ? WCHAR('\r') : ch.unicode();
        PostMessageW(a_hWnd, WM_CHAR, w, 0);
        Sleep(2);
    }
    return true;
}

quint32 VkOfName(const QString& a_sName) {
    const QString s = a_sName.trimmed().toLower();
    if (s.isEmpty()) {
        return 0;
    }
    static const QHash<QString, quint32> sMap = {
        { QStringLiteral("ctrl"),   VK_CONTROL }, { QStringLiteral("control"), VK_CONTROL },
        { QStringLiteral("shift"),  VK_SHIFT },   { QStringLiteral("alt"),     VK_MENU },
        { QStringLiteral("win"),    VK_LWIN },    { QStringLiteral("meta"),    VK_LWIN },
        { QStringLiteral("enter"),  VK_RETURN },  { QStringLiteral("return"),  VK_RETURN },
        { QStringLiteral("tab"),    VK_TAB },     { QStringLiteral("esc"),     VK_ESCAPE },
        { QStringLiteral("escape"), VK_ESCAPE },  { QStringLiteral("space"),   VK_SPACE },
        { QStringLiteral("left"),   VK_LEFT },    { QStringLiteral("right"),   VK_RIGHT },
        { QStringLiteral("up"),     VK_UP },      { QStringLiteral("down"),    VK_DOWN },
        { QStringLiteral("del"),    VK_DELETE },  { QStringLiteral("delete"),  VK_DELETE },
        { QStringLiteral("back"),   VK_BACK },    { QStringLiteral("backspace"), VK_BACK },
        { QStringLiteral("home"),   VK_HOME },    { QStringLiteral("end"),     VK_END },
        { QStringLiteral("pgup"),   VK_PRIOR },   { QStringLiteral("pageup"),  VK_PRIOR },
        { QStringLiteral("pgdn"),   VK_NEXT },    { QStringLiteral("pagedown"), VK_NEXT },
        { QStringLiteral("insert"), VK_INSERT },  { QStringLiteral("ins"),     VK_INSERT },
        { QStringLiteral("apps"),   VK_APPS },
        { QStringLiteral("f1"), VK_F1 },  { QStringLiteral("f2"), VK_F2 },
        { QStringLiteral("f3"), VK_F3 },  { QStringLiteral("f4"), VK_F4 },
        { QStringLiteral("f5"), VK_F5 },  { QStringLiteral("f6"), VK_F6 },
        { QStringLiteral("f7"), VK_F7 },  { QStringLiteral("f8"), VK_F8 },
        { QStringLiteral("f9"), VK_F9 },  { QStringLiteral("f10"), VK_F10 },
        { QStringLiteral("f11"), VK_F11 }, { QStringLiteral("f12"), VK_F12 },
        { QStringLiteral("capslock"), VK_CAPITAL }, { QStringLiteral("numlock"), VK_NUMLOCK },
        { QStringLiteral(","), VK_OEM_COMMA }, { QStringLiteral("."), VK_OEM_PERIOD },
    };
    if (sMap.contains(s)) {
        return sMap.value(s);
    }
    if (s.size() == 1) {
        const SHORT sVk = VkKeyScanW(s.at(0).unicode());
        if (sVk != -1) {
            return static_cast<quint32>(LOBYTE(sVk));
        }
    }
    return 0;
}

bool SendKeys(HWND a_hWnd, const QString& a_sCombo) {
    if (!IsWindow(a_hWnd)) {
        return false;
    }
    QList<quint32> aVks;
    const QStringList aParts = a_sCombo.split(QLatin1Char('+'), Qt::SkipEmptyParts);
    for (const QString& sPart : aParts) {
        const quint32 uVk = VkOfName(sPart);
        if (uVk == 0) {
            return false;
        }
        aVks.append(uVk);
    }
    if (aVks.isEmpty()) {
        return false;
    }
    for (const quint32 uVk : aVks) {
        PostMessageW(a_hWnd, WM_KEYDOWN, uVk, KeyLParam(uVk, false));
        Sleep(10);
    }
    for (int i = aVks.size() - 1; i >= 0; --i) {
        PostMessageW(a_hWnd, WM_KEYUP, aVks.at(i), KeyLParam(aVks.at(i), true));
        Sleep(6);
    }
    return true;
}

bool SendWmCommand(HWND a_hWnd, int a_iCmd) {
    if (!IsWindow(a_hWnd)) {
        return false;
    }
    return PostMessageW(a_hWnd, WM_COMMAND,
                        MAKEWPARAM(static_cast<WORD>(a_iCmd), 0),
                        reinterpret_cast<LPARAM>(a_hWnd)) != FALSE;
}

bool ClickButton(HWND a_hCtrl) {
    if (!IsWindow(a_hCtrl)) {
        return false;
    }
    return PostMessageW(a_hCtrl, BM_CLICK, 0, 0) != FALSE;
}

// ---------------------------------------------------------------------------
// UIA 控件树转储
// ---------------------------------------------------------------------------
namespace {

QString CtrlTypeName(int a_iType) {
    switch (a_iType) {
        case UIA_ButtonControlTypeId:    return QStringLiteral("Button");
        case UIA_EditControlTypeId:      return QStringLiteral("Edit");
        case UIA_TextControlTypeId:      return QStringLiteral("Text");
        case UIA_CheckBoxControlTypeId:  return QStringLiteral("CheckBox");
        case UIA_RadioButtonControlTypeId: return QStringLiteral("Radio");
        case UIA_ComboBoxControlTypeId:  return QStringLiteral("ComboBox");
        case UIA_ListControlTypeId:      return QStringLiteral("List");
        case UIA_ListItemControlTypeId:  return QStringLiteral("ListItem");
        case UIA_MenuControlTypeId:      return QStringLiteral("Menu");
        case UIA_MenuItemControlTypeId:  return QStringLiteral("MenuItem");
        case UIA_TabControlTypeId:       return QStringLiteral("Tab");
        case UIA_TabItemControlTypeId:   return QStringLiteral("TabItem");
        case UIA_TreeControlTypeId:      return QStringLiteral("Tree");
        case UIA_TreeItemControlTypeId:  return QStringLiteral("TreeItem");
        case UIA_WindowControlTypeId:    return QStringLiteral("Window");
        case UIA_PaneControlTypeId:      return QStringLiteral("Pane");
        case UIA_DocumentControlTypeId:  return QStringLiteral("Document");
        case UIA_HyperlinkControlTypeId: return QStringLiteral("Link");
        case UIA_ImageControlTypeId:     return QStringLiteral("Image");
        case UIA_GroupControlTypeId:     return QStringLiteral("Group");
        case UIA_SliderControlTypeId:    return QStringLiteral("Slider");
        case UIA_SpinnerControlTypeId:   return QStringLiteral("Spinner");
        case UIA_ToolBarControlTypeId:   return QStringLiteral("ToolBar");
        case UIA_TitleBarControlTypeId:  return QStringLiteral("TitleBar");
        case UIA_DataGridControlTypeId:  return QStringLiteral("DataGrid");
        case UIA_CustomControlTypeId:    return QStringLiteral("Custom");
        default:                         return QStringLiteral("Ctl");
    }
}

struct STreeCtx {
    IUIAutomation*      pAuto = nullptr;
    IUIAutomationTreeWalker* pWalker = nullptr;
    HWND                hRoot = nullptr;
    QString             sOut;
    int                 iNodes = 0;
    static constexpr int kcMaxNodes = 1600;
    static constexpr int kcMaxBytes = 16 * 1024;
};

void DumpElement(STreeCtx& a_Ctx, IUIAutomationElement* a_pEl, int a_iDepth) {
    if (a_pEl == nullptr || a_iDepth > 24
            || a_Ctx.iNodes >= STreeCtx::kcMaxNodes
            || a_Ctx.sOut.toUtf8().size() >= STreeCtx::kcMaxBytes) {
        return;
    }
    ++a_Ctx.iNodes;
    BSTR bsName = nullptr, bsCls = nullptr;
    a_pEl->get_CurrentName(&bsName);
    a_pEl->get_CurrentClassName(&bsCls);
    RECT rc = {};
    BOOL bEmpty = TRUE;
    a_pEl->get_CurrentBoundingRectangle(&rc);
    a_pEl->get_CurrentIsOffscreen(&bEmpty);
    int iType = 0;
    a_pEl->get_CurrentControlType(&iType);
    QString sName = (bsName != nullptr)
        ? QString::fromWCharArray(bsName, SysStringLen(bsName)) : QString();
    QString sCls = (bsCls != nullptr)
        ? QString::fromWCharArray(bsCls, SysStringLen(bsCls)) : QString();
    if (bsName != nullptr) SysFreeString(bsName);
    if (bsCls != nullptr) SysFreeString(bsCls);
    if (!bEmpty && rc.right > rc.left && rc.bottom > rc.top) {
        // 中心点 → 客户区坐标（sandbox_click 直接可用）。
        POINT pt { (rc.left + rc.right) / 2, (rc.top + rc.bottom) / 2 };
        ScreenToClient(a_Ctx.hRoot, &pt);
        a_Ctx.sOut += QString(QStringLiteral("%1[%2] %3 (%4) [%5x%6] click=(%7,%8)\n"))
            .arg(QString(a_iDepth * 2, QChar(' ')), CtrlTypeName(iType),
                 sName.isEmpty() ? QStringLiteral("-") : sName,
                 sCls.isEmpty() ? QStringLiteral("-") : sCls)
            .arg(rc.right - rc.left).arg(rc.bottom - rc.top).arg(pt.x).arg(pt.y);
    } else {
        a_Ctx.sOut += QString(QStringLiteral("%1[%2] %3 (%4) [offscreen]\n"))
            .arg(QString(a_iDepth * 2, QChar(' ')), CtrlTypeName(iType),
                 sName.isEmpty() ? QStringLiteral("-") : sName,
                 sCls.isEmpty() ? QStringLiteral("-") : sCls);
    }
    // 子控件遍历（控件视图：跳过纯装饰原始节点）。
    IUIAutomationElement* pChild = nullptr;
    if (SUCCEEDED(a_Ctx.pWalker->GetFirstChildElement(a_pEl, &pChild)) && pChild != nullptr) {
        IUIAutomationElement* pCur = pChild;
        while (pCur != nullptr
               && a_Ctx.iNodes < STreeCtx::kcMaxNodes
               && a_Ctx.sOut.toUtf8().size() < STreeCtx::kcMaxBytes) {
            IUIAutomationElement* pNext = nullptr;
            a_Ctx.pWalker->GetNextSiblingElement(pCur, &pNext);
            DumpElement(a_Ctx, pCur, a_iDepth + 1);
            pCur->Release();
            pCur = pNext;
        }
    }
}

} // namespace

QString DumpUiTree(HWND a_hWnd, int a_iMaxDepth) {
    if (!IsWindow(a_hWnd)) {
        return QStringLiteral("[窗口无效]");
    }
    IUIAutomation* pAuto = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_CUIAutomation, nullptr, CLSCTX_INPROC_SERVER,
                                  IID_PPV_ARGS(&pAuto));
    if (FAILED(hr) || pAuto == nullptr) {
        return QStringLiteral("[UIA 不可用]");
    }
    QString sOut;
    IUIAutomationTreeWalker* pWalker = nullptr;
    IUIAutomationElement* pRoot = nullptr;
    if (SUCCEEDED(pAuto->get_ControlViewWalker(&pWalker)) && pWalker != nullptr
            && SUCCEEDED(pAuto->ElementFromHandle(a_hWnd, &pRoot)) && pRoot != nullptr) {
        STreeCtx stCtx;
        stCtx.pAuto = pAuto;
        stCtx.pWalker = pWalker;
        stCtx.hRoot = a_hWnd;
        DumpElement(stCtx, pRoot, 0);
        sOut = stCtx.sOut;
        pRoot->Release();
    } else {
        sOut = QStringLiteral("[无法获取控件树]");
    }
    if (pWalker != nullptr) pWalker->Release();
    pAuto->Release();
    Q_UNUSED(a_iMaxDepth);
    return sOut.trimmed();
}

// ---------------------------------------------------------------------------
// 杂项
// ---------------------------------------------------------------------------
bool ParseHwnd(const QString& a_sText, HWND* a_phOut) {
    if (a_phOut == nullptr) {
        return false;
    }
    const QString s = a_sText.trimmed();
    if (s.isEmpty()) {
        return false;
    }
    bool bOk = false;
    const qulonglong u = s.startsWith(QStringLiteral("0x"), Qt::CaseInsensitive)
        ? s.toULongLong(&bOk, 16) : s.toULongLong(&bOk, 10);
    if (!bOk || u == 0) {
        return false;
    }
    *a_phOut = reinterpret_cast<HWND>(static_cast<uintptr_t>(u));
    return true;
}

bool ParseCli(const QStringList& a_aArgs, QString* a_psCmd, QJsonObject* a_pOut) {
    if (a_psCmd == nullptr || a_pOut == nullptr || a_aArgs.size() < 3) {
        return false;
    }
    const QString sCmd = a_aArgs.at(2).toLower();
    const int iN = a_aArgs.size();
    auto fnArg = [&a_aArgs](int a_i, const QString& a_sDef = QString()) -> QString {
        return (a_i < a_aArgs.size()) ? a_aArgs.at(a_i) : a_sDef;
    };
    auto fnH = [&fnArg](int a_i) { return fnArg(a_i); };   // hwnd 文本原样传
    QJsonObject o;
    if (sCmd == QStringLiteral("windows") || sCmd == QStringLiteral("status")
            || sCmd == QStringLiteral("record_on") || sCmd == QStringLiteral("record_off")
            || sCmd == QStringLiteral("restore-all") || sCmd == QStringLiteral("restore_all")) {
        *a_psCmd = (sCmd == QLatin1String("restore-all")) ? QStringLiteral("restore_all") : sCmd;
        *a_pOut = o;
        return true;
    }
    if (sCmd == QStringLiteral("capture")) {
        if (iN < 4) return false;
        o.insert(QStringLiteral("hwnd"), fnArg(3));
        if (iN >= 5) o.insert(QStringLiteral("path"), fnArg(4));
    } else if (sCmd == QStringLiteral("click")) {
        if (iN < 6) return false;
        o.insert(QStringLiteral("hwnd"), fnH(3));
        o.insert(QStringLiteral("x"), fnArg(4).toInt());
        o.insert(QStringLiteral("y"), fnArg(5).toInt());
        const QString sMode = (iN >= 7) ? fnArg(6).toLower() : QString();
        o.insert(QStringLiteral("dbl"), sMode == QLatin1String("dbl")
                                     || sMode == QLatin1String("double"));
        o.insert(QStringLiteral("right"), sMode == QLatin1String("right"));
    } else if (sCmd == QStringLiteral("drag")) {
        if (iN < 8) return false;
        o.insert(QStringLiteral("hwnd"), fnH(3));
        o.insert(QStringLiteral("x1"), fnArg(4).toInt());
        o.insert(QStringLiteral("y1"), fnArg(5).toInt());
        o.insert(QStringLiteral("x2"), fnArg(6).toInt());
        o.insert(QStringLiteral("y2"), fnArg(7).toInt());
        if (iN >= 9) o.insert(QStringLiteral("ms"), fnArg(8).toInt());
    } else if (sCmd == QStringLiteral("type")) {
        if (iN < 5) return false;
        o.insert(QStringLiteral("hwnd"), fnH(3));
        o.insert(QStringLiteral("text"), QStringList(a_aArgs.mid(4)).join(QLatin1Char(' ')));
    } else if (sCmd == QStringLiteral("launch")) {
        // --sandbox launch <exe> [参数...]：主进程代启动被测程序（2026-10-14
        //   断链漏收治理：Start-Process/cmd start 中间人秒退会切断进程树）。
        if (iN < 4) return false;
        o.insert(QStringLiteral("exe"), fnArg(3));
        if (iN >= 5) {
            // 余参重建成命令行串：含空格/制表的元素补引号（Qt 拆参已剥一层）。
            QStringList aRest;
            for (int k = 4; k < iN; ++k) {
                const QString s = a_aArgs.at(k);
                aRest.append(s.contains(QLatin1Char(' '))
                                 || s.contains(QLatin1Char('\t'))
                                 || s.isEmpty()
                             ? QStringLiteral("\"%1\"").arg(s) : s);
            }
            o.insert(QStringLiteral("args"), aRest.join(QLatin1Char(' ')));
        }
    } else if (sCmd == QStringLiteral("keys")) {
        if (iN < 5) return false;
        o.insert(QStringLiteral("hwnd"), fnH(3));
        o.insert(QStringLiteral("keys"), fnArg(4));
    } else if (sCmd == QStringLiteral("command")) {
        if (iN < 5) return false;
        o.insert(QStringLiteral("hwnd"), fnH(3));
        o.insert(QStringLiteral("cmd"), fnArg(4).toInt());
    } else if (sCmd == QStringLiteral("button")) {
        if (iN < 4) return false;
        o.insert(QStringLiteral("hwnd"), fnArg(3));
    } else if (sCmd == QStringLiteral("tree") || sCmd == QStringLiteral("add")
            || sCmd == QStringLiteral("restore") || sCmd == QStringLiteral("close")) {
        if (iN < 4) return false;
        o.insert(QStringLiteral("hwnd"), fnArg(3));
    } else {
        return false;   // 未知命令
    }
    *a_psCmd = sCmd;
    *a_pOut = o;
    return true;
}

} // namespace sbx
