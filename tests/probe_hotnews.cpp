// probe_hotnews.cpp —— HotNews 工程实测三报复现（2026-11-06）：
//   ①"new" 7000+ 命中连点数行 → 卡死？②ArticleExtract.cpp 经搜索打开乱码+错位
//   （直开正常）？③关搜索列表后直开源文件仍卡？
// 手动运行：Execute\probe_hotnews.exe，退出码 0=未复现 / 3=复现。
#include <QtTest/QtTest>
#include <QApplication>
#include <QDir>
#include <QFile>
#include <QTableWidget>
#include <QDateTime>
#include <windows.h>
#include <windowsx.h>

#include "ui/CExecutionPanel.h"
#include "ui/CFindResultsPage.h"
#include "ui/CFilePreviewWidget.h"
#include "ui/CCodeSyntaxView.h"
#include "core/CFileSearcher.h"

static HWND g_hSci = nullptr;
static BOOL CALLBACK FnFindSci(HWND h, LPARAM) {
    wchar_t sz[64] = { 0 };
    ::GetClassNameW(h, sz, 64);
    if (QString::fromWCharArray(sz).contains(QStringLiteral("Scintilla"))) {
        g_hSci = h;
        return FALSE;
    }
    return TRUE;
}

// 从 Scintilla 回读全文（裸 SendMessage 指针参数可用），与文件字节比对。
static QByteArray SciReadAll() {
    if (g_hSci == nullptr) {
        return QByteArray();
    }
    const LRESULT iLen = ::SendMessage(g_hSci, 2006 /*SCI_GETLENGTH*/, 0, 0);
    QByteArray aBuf(static_cast<int>(iLen) + 1, 0);
    const LRESULT iGot = ::SendMessage(g_hSci, 2182 /*SCI_GETTEXT*/,
                                       static_cast<WPARAM>(iLen + 1),
                                       reinterpret_cast<LPARAM>(aBuf.data()));
    aBuf.truncate(static_cast<int>(iGot > 0 ? iGot : 0));
    return aBuf;
}

int main(int argc, char** argv) {
    QApplication a(argc, argv);
    const QString sProj = QStringLiteral(
        "E:/wosens/Programs/VisualC++/插件框架/HotNews");
    const QString sFile = sProj + QStringLiteral("/ArticleExtract.cpp");
    if (!QFile::exists(sFile)) {
        printf("[SKIP] 文件不在：%s\n", qUtf8Printable(sFile));
        return 4;
    }
    QByteArray aFileBytes;
    {
        QFile f(sFile);
        f.open(QIODevice::ReadOnly);
        aFileBytes = f.readAll();
    }

    // ③先行对照收进同一视图链路：本探针不再预建直开预览（上版实验发现进程内
    //   二次 CvcCreate 会失败回退纯文本页——POC 单实例限制，真 App 单预览不受影
    //   响）；代码视图名额留给面板，乱码/耗时都在真链路上验。

    // ①+②：真链路搜索 "new" → 等填表 → 连点 ArticleExtract.cpp 的多行（计时）。
    CExecutionPanel panel;
    panel.resize(1000, 700);
    panel.show();
    QTest::qWait(200);

    // 基线：先直开一次（面板自己的预览，代码视图就位）——耗时+内容正确性。
    {
        CFilePreviewWidget* pPrev0 = nullptr;
        // 面板预览页惰性：轻点第一行前不存在——用 ShowFileTab 不可直调（私有），
        // 改由搜索后第一次点击自然创建；此处先空过，直开对照在点击后补。
        Q_UNUSED(pPrev0);
    }
    SFindTask stTask;
    stTask.m_sFind = QStringLiteral("new");
    stTask.m_aRoots = QStringList{sProj};
    stTask.m_sProjectDir = sProj;
    stTask.m_aExcludeDirs = CFileSearcher::LogFileNames();   // 占位非空（不影响）
    stTask.m_aExcludeDirs = QStringList{QStringLiteral(".git")};
    const qint64 tS0 = QDateTime::currentMSecsSinceEpoch();
    panel.StartFileSearch(stTask);
    CFindResultsPage* pPage = panel.findChild<CFindResultsPage*>();
    QTableWidget* pTable = pPage ? pPage->findChild<QTableWidget*>() : nullptr;
    if (pTable == nullptr) {
        printf("[FAIL] 结果窗不可得\n");
        return 3;
    }
    // 等搜索收尾（7000+ 行；上限 60s 防挂）。
    for (int i = 0; i < 3000 && pPage->SearchRunning(); ++i) {
        QTest::qWait(20);
    }
    QTest::qWait(300);
    printf("searchMs=%lld rows=%d\n",
           static_cast<long long>(QDateTime::currentMSecsSinceEpoch() - tS0),
           pTable->rowCount());

    // 收集 ArticleExtract.cpp 的行号。
    QList<int> aRows;
    for (int i = 0; i < pTable->rowCount() && aRows.size() < 6; ++i) {
        if (pTable->item(i, 1)->text().endsWith(QStringLiteral("ArticleExtract.cpp"))) {
            aRows.append(i);
        }
    }
    printf("articleRows=%d\n", aRows.size());

    // 连点：每行点击后不等待（模拟用户连点），最后统一计时+验内容。
    const qint64 tC0 = QDateTime::currentMSecsSinceEpoch();
    int iClickIdx = 0;
    for (int iRow : aRows) {
        const QRect stCell = pTable->visualItemRect(pTable->item(iRow, 0));
        QTest::mouseClick(pTable->viewport(), Qt::LeftButton, Qt::NoModifier,
                          stCell.center());
        QApplication::processEvents();   // 不 qWait：让事件堆积如真实连点
        ++iClickIdx;
        // 逐次视图长度（首次点击后预览/代码视图才存在）。
        if (g_hSci == nullptr) {
            CCodeSyntaxView* pCv = panel.findChild<CCodeSyntaxView*>();
            if (pCv != nullptr) {
                ::EnumChildWindows(reinterpret_cast<HWND>(pCv->winId()),
                                   FnFindSci, 0);
            }
        }
        if (g_hSci != nullptr) {
            printf("click#%d viewLen=%ld (file=48503)\n", iClickIdx,
                   static_cast<long>(::SendMessage(g_hSci, 2006, 0, 0)));
            // 对照：第二次点击后双通道清空对比（窗口层 vs CodeView 直调层）。
            if (iClickIdx == 2) {
                const LRESULT iRo0 = ::SendMessage(g_hSci, 2140 /*GETREADONLY*/, 0, 0);
                ::SendMessage(g_hSci, 2191 /*SETREADONLY*/, 0, 0);
                const LRESULT iRo1 = ::SendMessage(g_hSci, 2140, 0, 0);
                ::SendMessage(g_hSci, 2180 /*CLEARALL*/, 0, 0);
                const LRESULT iLenA = ::SendMessage(g_hSci, 2006, 0, 0);
                // 真空串 SETTEXT（DLL 原意的正确写法）。
                char szEmpty[1] = { 0 };
                ::SendMessage(g_hSci, 2181 /*SETTEXT*/, 0,
                              reinterpret_cast<LPARAM>(szEmpty));
                const LRESULT iLenB = ::SendMessage(g_hSci, 2006, 0, 0);
                printf("  wndProc: ro=%ld->%ld clearAll=%ld setEmpty=%ld\n",
                       static_cast<long>(iRo0), static_cast<long>(iRo1),
                       static_cast<long>(iLenA), static_cast<long>(iLenB));
                // 直调层：CvcSendMsg(host, SETREADONLY 0 + CLEARALL)。
                {
                    const QString sDll = QDir(QCoreApplication::applicationDirPath())
                        .filePath(QStringLiteral("CodeView.dll"));
                    const QString sDllUp = QDir(QCoreApplication::applicationDirPath()
                        + QStringLiteral("/..")).filePath(
                            QStringLiteral("CodeView.dll"));
                    HMODULE hDll = ::LoadLibraryW(
                        (QFile::exists(sDll) ? sDll : sDllUp).toStdWString().c_str());
                    if (hDll != nullptr) {
                        using FnSend = intptr_t (*)(void*, unsigned int,
                                                    uintptr_t, intptr_t);
                        auto pfn = reinterpret_cast<FnSend>(
                            ::GetProcAddress(hDll, "CvcSendMsg"));
                        // hView = CodeViewHost（CvcCreate 返回值）：Scintilla 的父即 host。
                        HWND hHost = ::GetParent(g_hSci);
                        wchar_t szCls[32] = { 0 };
                        ::GetClassNameW(hHost, szCls, 32);
                        if (pfn != nullptr && hHost != nullptr) {
                            const intptr_t iRoA = pfn(hHost, 2140, 0, 0);
                            pfn(hHost, 2191, 0, 0);
                            const intptr_t iRoB = pfn(hHost, 2140, 0, 0);
                            printf("  direct: ro %ld->%ld\n",
                                   static_cast<long>(iRoA),
                                   static_cast<long>(iRoB));
                            pfn(hHost, 2191, 0, 0);
                            pfn(hHost, 2180, 0, 0);
                            printf("  direct(%ls): clearAll -> len=%ld\n", szCls,
                                   static_cast<long>(
                                       ::SendMessage(g_hSci, 2006, 0, 0)));
                            char szE[1] = { 0 };
                            pfn(hHost, 2181, 0, reinterpret_cast<intptr_t>(szE));
                            printf("  direct: setEmpty -> len=%ld\n",
                                   static_cast<long>(
                                       ::SendMessage(g_hSci, 2006, 0, 0)));
                            const LRESULT iL = ::SendMessage(g_hSci, 2006, 0, 0);
                            pfn(hHost, 2640 /*SCI_DELETERANGE*/, 0,
                                static_cast<intptr_t>(iL));
                            printf("  direct: deleteRange -> len=%ld\n",
                                   static_cast<long>(
                                       ::SendMessage(g_hSci, 2006, 0, 0)));
                        }
                    }
                }
            }
        }
    }
    const qint64 iClickMs = QDateTime::currentMSecsSinceEpoch() - tC0;
    // 排空事件队列（若卡死在此不动，外部超时杀）。
    for (int i = 0; i < 50; ++i) {
        QApplication::processEvents();
        QTest::qWait(20);
    }
    printf("clicksMs=%lld (6 次连点+排空)\n", static_cast<long long>(iClickMs));

    // 直开对照（点击链已建预览/代码视图后，从资源管理器同源入口再开一次）。
    //   CExecutionPanel::ShowFileTab 是公有入口——直调验证不经搜索路径的内容。
    {
        const qint64 t1 = QDateTime::currentMSecsSinceEpoch();
        // 直接打开（无定位）：模拟资源管理器双击（不带 line/col）。
        panel.ShowFileTab(sFile, true);
        const qint64 iMs1 = QDateTime::currentMSecsSinceEpoch() - t1;
        QApplication::processEvents();
        QTest::qWait(200);
        printf("directAfter: showMs=%lld\n", static_cast<long long>(iMs1));
    }

    // CPU 走势：代码视图活着时采样（上一版误测回退纯文本页）。
    {
        FILETIME ftI0, ftK0, ftU0, ftE0;
        ::GetProcessTimes(::GetCurrentProcess(), &ftI0, &ftK0, &ftU0, &ftE0);
        QTest::qWait(1500);
        FILETIME ftI1, ftK1, ftU1, ftE1;
        ::GetProcessTimes(::GetCurrentProcess(), &ftI1, &ftK1, &ftU1, &ftE1);
        const auto fnUs = [](const FILETIME& x) {
            return (static_cast<qint64>(x.dwHighDateTime) << 32) | x.dwLowDateTime;
        };
        printf("cpuIdle=%lldms/1500ms (>1200=消息自激实锤)\n",
               static_cast<long long>((fnUs(ftU1) - fnUs(ftU0)) / 10000));
    }

    // ②验证：经搜索打开后视图内容 vs 文件字节（乱码=不一致）。
    int rc = 0;
    g_hSci = nullptr;
    {
        CCodeSyntaxView* pCv = panel.findChild<CCodeSyntaxView*>();
        CFilePreviewWidget* pPrev = panel.findChild<CFilePreviewWidget*>();
        printf("viaSearch: codeViewWidget=%p isOk=%d preview=%p\n",
               static_cast<void*>(pCv), (pCv && pCv->IsOk()) ? 1 : 0,
               static_cast<void*>(pPrev));
        if (pPrev != nullptr) {
            printf("viaSearch: previewCodeActive=%d\n",
                   pPrev->IsCodeViewActive() ? 1 : 0);
        }
        if (pCv != nullptr) {
            ::EnumChildWindows(reinterpret_cast<HWND>(pCv->winId()), FnFindSci, 0);
        }
    }
    if (g_hSci != nullptr) {
        const QByteArray aView = SciReadAll();
        const bool bEq = (aView == aFileBytes);
        printf("viaSearch: viewLen=%lld fileLen=%lld equal=%d%s\n",
               static_cast<long long>(aView.size()),
               static_cast<long long>(aFileBytes.size()),
               bEq ? 1 : 0,
               bEq ? "" : "  ← 乱码/内容不一致实锤");
        if (!bEq) {
            rc = 3;
            const int n = qMin(aView.size(), aFileBytes.size());
            for (int i = 0; i < n; ++i) {
                if (aView.at(i) != aFileBytes.at(i)) {
                    printf("firstDiff@%d view=%02X file=%02X\n", i,
                           uchar(aView.at(i)), uchar(aFileBytes.at(i)));
                    break;
                }
            }
        }
        // 逐次点击后选区验证：末次点击的行/列 vs 实际选区。
        if (aRows.size() >= 2) {
            const QString sLast = pTable->item(aRows.last(), 0)->text();
            const int iColon = sLast.indexOf(QLatin1Char(':'));
            const int iLine = sLast.left(iColon).toInt();
            const LRESULT iSelS = ::SendMessage(g_hSci, 2143, 0, 0);
            const LRESULT iSelLine = ::SendMessage(g_hSci, 2166, iSelS, 0);
            printf("lastClick: wantLine=%d gotSelLine=%ld\n", iLine,
                   static_cast<long>(iSelLine) + 1);
        }
    } else {
        printf("viaSearch: Scintilla 不可得（代码视图创建失败——探针环境污染或真缺陷）\n");
    }
    if (iClickMs > 5000) {
        printf("[FAIL] 连点 %lldms（接近卡死）\n", static_cast<long long>(iClickMs));
        rc = 3;
    }
    printf("probe rc=%d\n", rc);
    return rc;
}
