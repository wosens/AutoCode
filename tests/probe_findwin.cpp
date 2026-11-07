// probe_findwin.cpp —— 查找结果独立窗 + 代码文件打开后主窗标题栏拖动探针（2026-11-06）：
//   复现链：搜索 → 真鼠标点击 .cpp 结果行 → 预览开代码页 →
//   ① GetCapture 是否被劫持（标题栏拖不动根因）② 焦点落点 ③ GotoPos 选区。
// 手动运行：Execute\probe_findwin.exe，退出码 0=全过 / 3=复现。
#include <QtTest/QtTest>
#include <QApplication>
#include <QDir>
#include <QFile>
#include <QTableWidget>
#include <QVBoxLayout>
#include <windows.h>
#include <windowsx.h>   // GET_X/Y_LPARAM
#include <QDateTime>
#include <dwmapi.h>
#pragma comment(lib, "dwmapi.lib")

#include "ui/CExecutionPanel.h"
#include "ui/CFindResultsPage.h"
#include "ui/CFilePreviewWidget.h"
#include "ui/CCodeSyntaxView.h"
#include <QFileInfo>

struct SciEnumCtx {
    HWND hFound = nullptr;
    HWND hHost  = nullptr;
};

// 复刻真主窗的无边框拖动机制（CMainWindow::nativeEvent WM_NCHITTEST → 标题带
//   HTCAPTION）：验证点代码结果行前后，标题带命中/窗口使能是否变化。
class CFramelessHost : public QWidget {
public:
    explicit CFramelessHost(QWidget* a_pParent = nullptr) : QWidget(a_pParent) {
        setWindowFlags(Qt::Window | Qt::FramelessWindowHint);
        resize(1000, 700);
        // 补回 Win10/11 系统阴影（同真主窗 CMainWindow，消除与真窗的残差）。
        if (HWND hWnd = reinterpret_cast<HWND>(winId())) {
            MARGINS stMargin { 1, 1, 1, 1 };
            DwmExtendFrameIntoClientArea(hWnd, &stMargin);
        }
    }
protected:
    bool nativeEvent(const QByteArray& a_sType, void* a_pMsg, qintptr* a_piRes) override {
        if (a_sType == "windows_generic_MSG" && a_pMsg != nullptr
                && a_piRes != nullptr) {
            const MSG* pMsg = static_cast<MSG*>(a_pMsg);
            if (pMsg->message == WM_NCHITTEST) {
                const QPoint stGlobal(QPoint(GET_X_LPARAM(pMsg->lParam),
                                             GET_Y_LPARAM(pMsg->lParam)));
                const QPoint stLocal = mapFromGlobal(stGlobal);
                if (stLocal.y() >= 0 && stLocal.y() < 40) {
                    *a_piRes = HTCAPTION;   // 标题带：同真主窗可拖
                    return true;
                }
            }
        }
        return QWidget::nativeEvent(a_sType, a_pMsg, a_piRes);
    }
};

static BOOL CALLBACK FnEnumSci(HWND h, LPARAM lp) {
    auto* p = reinterpret_cast<SciEnumCtx*>(lp);
    wchar_t szCls[64] = { 0 };
    ::GetClassNameW(h, szCls, 64);
    const QString sCls = QString::fromWCharArray(szCls);
    if (p->hFound == nullptr && sCls.contains(QStringLiteral("Scintilla"),
                                               Qt::CaseInsensitive)) {
        p->hFound = h;
    }
    if (p->hHost == nullptr && sCls == QStringLiteral("CodeViewHost")) {
        p->hHost = h;
    }
    return TRUE;
}

int main(int argc, char** argv) {
    QApplication a(argc, argv);

    // 夹具：.cpp（needle 行3 列5）+ .txt 对照。
    const QString sDir = QDir::temp().absoluteFilePath(QStringLiteral(
        "autocode_probe_findwin_%1").arg(QCoreApplication::applicationPid()));
    QDir().mkpath(sDir);
    {
        QFile f(sDir + QStringLiteral("/probe.cpp"));
        f.open(QIODevice::WriteOnly);
        f.write(QByteArrayLiteral("// line1\nint aa = 1;\nint needle_x = needle + 1;\n// tail\n"));
        f.close();
        QFile f2(sDir + QStringLiteral("/note.txt"));
        f2.open(QIODevice::WriteOnly);
        f2.write(QByteArrayLiteral("needle in txt\n"));
        f2.close();
    }

    // 大文件冻结实测：~1MB 生成式 .cpp（搜索带去的那种大文件）同步解析耗时。
    {
        CFilePreviewWidget wBig;
        wBig.resize(600, 400);
        wBig.show();
        const QString sBig = sDir + QStringLiteral("/big.cpp");
        {
            QFile f(sBig);
            f.open(QIODevice::WriteOnly);
            const QByteArray aTpl = QByteArrayLiteral(
                "int fn_%1(int a, const char* b) { // needle big\n"
                "    int r = a * 3 + a % 7; if (b && r > 0) { r -= 2; }\n"
                "    for (int i = 0; i < 32; ++i) { r += i ^ 0x5a; }\n"
                "    return r + static_cast<int>(QString::fromUtf8(b).size()); }\n");
            for (int i = 0; i < 5400; ++i) {   // ≈1MB
                f.write(aTpl);
            }
            f.write(QByteArrayLiteral("// needle big tail\n"));
            f.close();
        }
        const qint64 t0 = QDateTime::currentMSecsSinceEpoch();
        wBig.ShowFile(sBig);
        const qint64 iMs = QDateTime::currentMSecsSinceEpoch() - t0;
        printf("bigCppShowMs=%lld fileBytes=%lld (>800=同步解析明显卡 UI)\n",
               static_cast<long long>(iMs),
               static_cast<long long>(QFileInfo(sBig).size()));
        QApplication::processEvents();
    }   // wBig 出域：CvcDestroy 释放 POC 单视图名额，后续面板自建

    CFramelessHost stHost;   // 无边框宿主（复刻真主窗拖动机制）
    auto* pLay = new QVBoxLayout(&stHost);
    pLay->setContentsMargins(0, 40, 0, 0);   // 顶部 40px=标题带（自绘）
    CExecutionPanel panel(&stHost);
    pLay->addWidget(&panel);
    stHost.show();

    SFindTask stTask;
    stTask.m_sFind = QStringLiteral("needle");
    stTask.m_aRoots = QStringList{sDir};
    stTask.m_sProjectDir = sDir;
    panel.StartFileSearch(stTask);

    CFindResultsPage* pPage = panel.findChild<CFindResultsPage*>();
    QTableWidget* pTable = pPage ? pPage->findChild<QTableWidget*>() : nullptr;
    if (pTable == nullptr) {
        printf("[FAIL] 结果窗/表不可得\n");
        return 3;
    }
    for (int i = 0; i < 200 && pTable->rowCount() < 2; ++i) {
        QTest::qWait(20);
    }
    printf("rows=%d\n", pTable->rowCount());

    // 找 .cpp 行（第 0 行按文件主序=probe.cpp）。
    int iCppRow = -1;
    for (int i = 0; i < pTable->rowCount(); ++i) {
        if (pTable->item(i, 1)->text().endsWith(QStringLiteral(".cpp"))) {
            iCppRow = i;
            break;
        }
    }
    if (iCppRow < 0) {
        printf("[FAIL] 无 .cpp 结果行\n");
        return 3;
    }

    // 命中诊断：无边框宿主标题带的真 NCHITTEST（经 CFramelessHost::nativeEvent，
    //   与真主窗同机制）+ 窗口使能。拖不动=命中非 CAPTION 或窗口被禁用。
    auto fnTitleHit = [&stHost](const char* a_pcTag) {
        const QRect stGeo = stHost.geometry();
        const QPoint stPt(stGeo.center().x(), stGeo.top() + 20);   // 标题带中央
        POINT pt{ static_cast<LONG>(stPt.x()), static_cast<LONG>(stPt.y()) };
        const HWND hHit = ::WindowFromPoint(pt);
        wchar_t szCls[64] = { 0 };
        if (hHit != nullptr) {
            ::GetClassNameW(hHit, szCls, 64);
        }
        const LRESULT iHt = (hHit != nullptr)
            ? ::SendMessage(hHit, WM_NCHITTEST, 0,
                            MAKELPARAM(pt.x, pt.y)) : -1;
        const BOOL bEnabled = (hHit != nullptr) ? ::IsWindowEnabled(hHit) : FALSE;
        printf("%s titleHit: hwnd=%p cls=%ls ht=%ld enabled=%d (2=HTCAPTION 拖得动)\n",
               a_pcTag, static_cast<void*>(hHit), szCls, static_cast<long>(iHt),
               static_cast<int>(bEnabled));
        return iHt == 2 && bEnabled;
    };
    const bool bHitBefore = fnTitleHit("before");

    // 真拖动链路验证：看门狗线程 400ms 后投 WM_NCLBUTTONUP；主线程发
    //   WM_NCLBUTTONDOWN(HTCAPTION) 进入 DefWindowProc 移动模态循环——若
    //   阻塞≈400ms 后返回=拖动链路活着；立即返回=拖不动（模态循环未进入）。
    auto fnDragLoop = [&stHost](const char* a_pcTag) {
        const HWND hWnd = reinterpret_cast<HWND>(stHost.winId());
        const QPoint stPt = stHost.mapToGlobal(QPoint(stHost.width() / 2, 20));
        class CUp {
        public:
            static DWORD WINAPI Run(LPVOID lp) {
                ::Sleep(400);
                ::PostMessageW(static_cast<HWND>(lp), WM_NCLBUTTONUP, HTCAPTION, 0);
                return 0;
            }
        };
        const HANDLE hTh = ::CreateThread(nullptr, 0, CUp::Run, hWnd, 0, nullptr);
        const qint64 t0 = QDateTime::currentMSecsSinceEpoch();
        ::SendMessageW(hWnd, WM_NCLBUTTONDOWN, HTCAPTION,
                       MAKELPARAM(static_cast<WORD>(stPt.x()),
                                  static_cast<WORD>(stPt.y())));
        const qint64 iBlocked = QDateTime::currentMSecsSinceEpoch() - t0;
        if (hTh != nullptr) {
            ::WaitForSingleObject(hTh, 2000);
            ::CloseHandle(hTh);
        }
        printf("%s dragLoop blocked=%lldms (>200=移动循环活着拖得动)\n",
               a_pcTag, static_cast<long long>(iBlocked));
        return iBlocked > 200;
    };
    const bool bDragBefore = fnDragLoop("before");

    // —— 真实点击 .cpp 行（同用户操作：mouseDown+Up 在结果窗表格上）——
    const QRect stCell = pTable->visualItemRect(pTable->item(iCppRow, 0));
    QTest::mouseClick(pTable->viewport(), Qt::LeftButton, Qt::NoModifier,
                      stCell.center());
    QApplication::processEvents();
    QTest::qWait(200);
    const bool bHitAfterCpp = fnTitleHit("afterCpp");

    // CPU 走势：SCN_UPDATEUI→WM_APP_REFRESH→indicator 写→再 UPDATEUI 若自激，
    //   消息泵永不清闲（拖动/点击整体发滞）;采样进程 CPU 时间验证。
    {
        const qint64 t0 = QDateTime::currentMSecsSinceEpoch();
        FILETIME ftIdle0, ftKr0, ftUs0;
        ::GetProcessTimes(::GetCurrentProcess(), &ftIdle0, &ftKr0, &ftUs0, &ftIdle0);
        QTest::qWait(1500);
        FILETIME ftIdle1, ftKr1, ftUs1;
        ::GetProcessTimes(::GetCurrentProcess(), &ftIdle1, &ftKr1, &ftUs1, &ftIdle1);
        const auto fnUs = [](const FILETIME& a) {
            return (static_cast<qint64>(a.dwHighDateTime) << 32) | a.dwLowDateTime;
        };
        const qint64 iUs = (fnUs(ftUs1) - fnUs(ftUs0)) / 10000;   // ms 用户态
        printf("cpuAfterCpp=%lldms/1500ms (>1200=自激消息泵实锤)\n",
               static_cast<long long>(iUs));
    }
    const bool bDragAfterCpp = fnDragLoop("afterCpp");

    // 对照：点击 .txt 行后再测（用户报：txt 正常 / cpp 拖不动）。
    int iTxtRow = -1;
    for (int i = 0; i < pTable->rowCount(); ++i) {
        if (pTable->item(i, 1)->text().endsWith(QStringLiteral(".txt"))) {
            iTxtRow = i;
            break;
        }
    }
    if (iTxtRow >= 0) {
        const QRect stTxt = pTable->visualItemRect(pTable->item(iTxtRow, 0));
        QTest::mouseClick(pTable->viewport(), Qt::LeftButton, Qt::NoModifier,
                          stTxt.center());
        QApplication::processEvents();
        QTest::qWait(200);
        fnTitleHit("afterTxt");
    }

    // 覆盖实验：把结果窗叠到宿主标题带上 → 标题带命中应落到结果窗（拖不动的
    //   唯一可行机制：Qt::Dialog 恒悬于父之上，盖住标题带则点不到主窗 HTCAPTION）。
    {
        const QRect stGeo = stHost.geometry();
        pPage->move(stGeo.left() + stGeo.width() / 2 - 100, stGeo.top() - 10);
        QApplication::processEvents();
        QTest::qWait(100);
        printf("overlapDlg dlgGeo=%d,%d %dx%d hostTitlePt=%d,%d\n",
               pPage->geometry().left(), pPage->geometry().top(),
               pPage->width(), pPage->height(),
               stGeo.center().x(), stGeo.top() + 20);
        fnTitleHit("overlapDlg");
    }

    int rc = 0;
    if (!bHitBefore || !bHitAfterCpp) {
        printf("[FAIL-2] 标题栏命中非 HTCAPTION（拖不动实锤）\n");
        rc = 3;
    } else if (!bDragBefore || !bDragAfterCpp) {
        printf("[FAIL-2] 标题栏移动模态循环未进入（拖不动实锤）\n");
        rc = 3;
    } else {
        printf("[PASS-2] 标题栏命中/移动循环均正常\n");
    }
    // ① 鼠标捕获：点击链结束后应为空。
    const HWND hCap = ::GetCapture();
    wchar_t szCapCls[64] = { 0 };
    if (hCap != nullptr) {
        ::GetClassNameW(hCap, szCapCls, 64);
    }
    printf("capture=%p cls=%ls\n", static_cast<void*>(hCap), szCapCls);
    if (hCap != nullptr) {
        printf("[FAIL-2] 点击代码结果行后有窗口持有鼠标捕获（标题栏拖不动根因）\n");
        rc = 3;
    } else {
        printf("[PASS-2] 点击后无捕获残留\n");
    }
    // ② 焦点落点。
    const HWND hFocus = ::GetFocus();
    wchar_t szFocCls[64] = { 0 };
    if (hFocus != nullptr) {
        ::GetClassNameW(hFocus, szFocCls, 64);
    }
    printf("focus=%p cls=%ls\n", static_cast<void*>(hFocus), szFocCls);

    // ③ 代码页定位：选区应落在 needle（25..31）。
    CCodeSyntaxView* pCode = panel.findChild<CCodeSyntaxView*>();
    SciEnumCtx stEnum;
    if (pCode != nullptr) {
        ::EnumChildWindows(reinterpret_cast<HWND>(pCode->winId()), FnEnumSci,
                           reinterpret_cast<LPARAM>(&stEnum));
    }
    if (stEnum.hFound != nullptr) {
        const LRESULT iS = ::SendMessage(stEnum.hFound, 2143, 0, 0);
        const LRESULT iE = ::SendMessage(stEnum.hFound, 2145, 0, 0);
        printf("codeSel=%ld..%ld (expect 25..31)\n",
               static_cast<long>(iS), static_cast<long>(iE));
        if (iS != 25 || iE != 31) {
            printf("[FAIL-1] 代码文件未定位到命中串\n");
            rc = 3;
        } else {
            printf("[PASS-1] 代码文件定位正确\n");
        }
    } else {
        printf("[FAIL] Scintilla 子窗不可得\n");
        rc = 3;
    }

    if (rc == 0) {
        QDir(sDir).removeRecursively();
    }
    printf("probe rc=%d\n", rc);
    return rc;
}
