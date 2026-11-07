// probe_findpos.cpp —— 查找结果点击定位（代码文件）双 bug 探针（2026-11-06）：
//   ①代码文件（CCodeSyntaxView/Scintilla 页）GotoPos 后选区是否落在命中处；
//   ②定位后是否有窗口劫持鼠标捕获（GetCapture 非空=主窗标题栏拖不动）。
// 手动运行：Execute\probe_findpos.exe，退出码 0=全过 / 3=复现失败。
#include <QtTest/QtTest>
#include <QApplication>
#include <QDir>
#include <QFile>
#include <windows.h>

#include "ui/CFilePreviewWidget.h"
#include "ui/CCodeSyntaxView.h"
#include "project/CProjectManager.h"

struct SciEnumCtx {
    HWND hFound = nullptr;   // Scintilla 本体
    HWND hHost  = nullptr;   // CodeViewHost（CvcCreate 返回的包装层）
    QStringList aClasses;
};

static BOOL CALLBACK FnEnumSciChildren(HWND h, LPARAM lp) {
    auto* p = reinterpret_cast<SciEnumCtx*>(lp);
    wchar_t szCls[64] = { 0 };
    ::GetClassNameW(h, szCls, 64);
    const QString sCls = QString::fromWCharArray(szCls);
    p->aClasses << sCls;
    if (p->hFound == nullptr
            && sCls.contains(QStringLiteral("Scintilla"), Qt::CaseInsensitive)) {
        p->hFound = h;
    }
    if (p->hHost == nullptr && sCls == QStringLiteral("CodeViewHost")) {
        p->hHost = h;
    }
    return TRUE;   // 继续枚举全部（打印类名清单）
}

int main(int argc, char** argv) {
    QApplication a(argc, argv);

    // 夹具：.cpp 文件，needle 在第 3 行第 10 列。
    const QString sDir = QDir::temp().absoluteFilePath(QStringLiteral(
        "autocode_probe_findpos_%1").arg(QCoreApplication::applicationPid()));
    QDir().mkpath(sDir);
    const QString sCpp = sDir + QStringLiteral("/probe.cpp");
    {
        QFile f(sCpp);
        f.open(QIODevice::WriteOnly);
        f.write(QByteArrayLiteral("// line1\nint aa = 1;\nint needle_x = needle + 1;\n// tail\n"));
        f.close();
    }
    // LoadCodeSyntax 读全局 config.ini；强制开（复制现场：设置默认开）。
    // （探测读不到就用默认 true。）

    CFilePreviewWidget w;
    w.resize(600, 400);
    w.show();
    w.ShowFile(sCpp);
    QApplication::processEvents();

    const bool bCode = w.IsCodeViewActive();
    printf("codeViewActive=%d\n", bCode ? 1 : 0);

    int rc = 0;
    if (!bCode) {
        printf("[SKIP] CodeStyle 模块不在位，纯文本页与本双 bug 无关\n");
        rc = 4;
    } else {
        CCodeSyntaxView* pCode = w.findChild<CCodeSyntaxView*>();
        SciEnumCtx stEnum;
        if (pCode != nullptr) {
            ::EnumChildWindows(reinterpret_cast<HWND>(pCode->winId()),
                               FnEnumSciChildren,
                               reinterpret_cast<LPARAM>(&stEnum));
        }
        printf("childClasses=%s\n",
               qUtf8Printable(stEnum.aClasses.join(QLatin1Char(','))));
        HWND hSci = stEnum.hFound;
        if (hSci != nullptr) {
            // 验窗口是真 Scintilla：SCI_GETLENGTH(2006) 应答非零文档长。
            const LRESULT iLen = ::SendMessage(hSci, 2006, 0, 0);
            printf("sciLen=%ld (expect 56)\n", static_cast<long>(iLen));
            if (iLen <= 0) { hSci = nullptr; }
        }

        if (hSci == nullptr) {
            printf("[FAIL] 找不到 Scintilla 子窗口\n");
            rc = 3;
        } else {
            // —— 对照 X：直连 CvcSendMsg 逐消息探性（与裸 SendMessage 对拍）——
            //   经 CodeView.dll 导出直接调，验 CvcSendMsg(HOST, msg, wp, lp) 行为。
            {
                const QString sDll = QDir(QCoreApplication::applicationDirPath())
                    .filePath(QStringLiteral("CodeView.dll"));
                const QString sDllUp = QDir(QCoreApplication::applicationDirPath()
                    + QStringLiteral("/..")).filePath(QStringLiteral("CodeView.dll"));
                HMODULE hDll = ::LoadLibraryW(
                    (QFile::exists(sDll) ? sDll : sDllUp).toStdWString().c_str());
                if (hDll != nullptr) {
                    using FnSend = intptr_t (*)(void*, unsigned int, uintptr_t, intptr_t);
                    auto pfn = reinterpret_cast<FnSend>(
                        ::GetProcAddress(hDll, "CvcSendMsg"));
                    if (pfn != nullptr && stEnum.hHost != nullptr) {
                        void* hView = stEnum.hHost;
                        // 位置归零后统一从 GOTOLINE(2) 起测。
                        ::SendMessage(hSci, 2024, 2, 0);
                        const intptr_t iRawCur = ::SendMessage(hSci, 2008, 0, 0);
                        const intptr_t iCsvCur = pfn(hView, 2008, 0, 0);
                        const intptr_t iCsvLine = pfn(hView, 2166, 0, iCsvCur);
                        const intptr_t iCsvStart = pfn(hView, 2167, 2, 0);
                        const intptr_t iCsvEnd = pfn(hView, 2136, 2, 0);
                        const intptr_t iCsvLen = pfn(hView, 2350, 2, 0);
                        printf("csv: cur=%ld(raw %ld) line=%ld start=%ld end=%ld len=%ld"
                               " (expect cur=21 line=2 start=21 end=47 len=27)\n",
                               static_cast<long>(iCsvCur), static_cast<long>(iRawCur),
                               static_cast<long>(iCsvLine), static_cast<long>(iCsvStart),
                               static_cast<long>(iCsvEnd), static_cast<long>(iCsvLen));
                    } else {
                        printf("csv: CvcSendMsg 不可得 host=%p\n",
                               static_cast<void*>(stEnum.hHost));
                    }
                }
            }

            // 对照 0：裸 SendMessage SCI_GOTOLINE(2) 直接打到 Scintilla——验证控件本身响应。
            ::SendMessage(hSci, 2024 /*SCI_GOTOLINE*/, 2, 0);
            QApplication::processEvents();
            const LRESULT iPos0 = ::SendMessage(hSci, 2008 /*SCI_GETCURRENTPOS*/, 0, 0);
            printf("rawGotoline curpos=%ld (expect 21)\n", static_cast<long>(iPos0));

            // 对照 1：旧链路 GotoLine（代码审查定位）——验证 CvcSendMsg 通道本身。
            //   两轮读数：立即 + 300ms 后（验异步投递——延迟后若变为正确值即坐实）。
            w.GotoLine(3);
            QApplication::processEvents();
            {
                const LRESULT iS0 = ::SendMessage(hSci, 2143, 0, 0);
                const LRESULT iE0 = ::SendMessage(hSci, 2145, 0, 0);
                QTest::qWait(300);
                const LRESULT iS = ::SendMessage(hSci, 2143, 0, 0);
                const LRESULT iE = ::SendMessage(hSci, 2145, 0, 0);
                printf("gotoLine selNow=%ld..%ld selAfter300ms=%ld..%ld (expect 21..47)\n",
                       static_cast<long>(iS0), static_cast<long>(iE0),
                       static_cast<long>(iS), static_cast<long>(iE));
            }

            // ① GotoPos(3, 5, 6)：期望选中首个 needle（行1 9 字节+行2 12 字节=行3 起点 21，
            //   「int 」占 4 字符 → 列 5=字节 25，长 6 → 31）。
            w.GotoPos(3, 5, 6);
            QApplication::processEvents();
            const LRESULT iSelStart = ::SendMessage(hSci, 2143 /*SCI_GETSELECTIONSTART*/, 0, 0);
            const LRESULT iSelEnd   = ::SendMessage(hSci, 2145 /*SCI_GETSELECTIONEND*/, 0, 0);
            // 注意：SCI_LINEFROMPOS 的位置在 wParam（本探针曾踩 lParam 坑——与被测
            //   bug 同源，探针侧修正）。
            const LRESULT iCurLine  = ::SendMessage(hSci, 2166 /*SCI_LINEFROMPOS*/, iSelStart, 0);
            // 行 3（1-based）起点字节偏移 21；列 5 → +4 = 25；长 6 → 31。
            printf("selStart=%ld selEnd=%ld line0=%ld (expect start=25 end=31 line0=2)\n",
                   static_cast<long>(iSelStart), static_cast<long>(iSelEnd),
                   static_cast<long>(iCurLine));
            if (iSelStart != 25 || iSelEnd != 31 || iCurLine != 2) {
                printf("[FAIL-1] 代码文件定位失败：选区不在命中串上\n");
                rc = 3;
            } else {
                printf("[PASS-1] 代码文件定位正确\n");
            }
            // ② 鼠标捕获：定位链结束后 GetCapture 应为空（非空=标题栏拖不动的胄）。
            //   再模拟用户点了一下主窗客户区后松开（找胄是否仍持有）。
            const HWND hCap1 = ::GetCapture();
            printf("captureAfterGoto=%p\n", static_cast<void*>(hCap1));
            if (hCap1 != nullptr) {
                printf("[FAIL-2] GotoPos 后有窗口持有鼠标捕获（标题栏拖不动根因）\n");
                rc = (rc == 0) ? 3 : rc;
            } else {
                printf("[PASS-2] GotoPos 后无鼠标捕获残留\n");
            }
        }
    }

    // 夹具清理（失败时保留现场调试）。
    if (rc == 0 || rc == 4) {
        QDir(sDir).removeRecursively();
    }
    printf("probe rc=%d\n", rc);
    return rc;
}
