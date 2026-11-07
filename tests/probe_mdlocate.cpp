// probe_mdlocate.cpp —— 真实 md 定位准确性探针（2026-11-07 用户报"md 还是定位不准"）：
//   靶文件 doc/AutoCode改进计划-202611.md（305 行：标题/表格/引用/列表/代码块/链接）。
//   对多个搜索词逐命中点击，校验：①选中文本==命中文本；②选中所在渲染块与源命中行
//   对应（规范化互含）。BAD 行打印源行 vs 渲染块，直指断点机制。
// 手动运行：Execute\Release\probe_mdlocate.exe，退出码 0=全部命中定位正确。
#include <QtTest/QtTest>
#include <QApplication>
#include <QTextBrowser>
#include <QTextBlock>
#include <QTableWidget>
#include <QDateTime>
#include <QFileInfo>
#include <QDir>
#include <QRegularExpression>

#include "ui/CExecutionPanel.h"
#include "ui/CFindResultsPage.h"
#include "ui/CFilePreviewWidget.h"
#include "core/CFileSearcher.h"

// 规范化：剥 md 标记字符/空白/链接语法，比对渲染块与源行。
static QString Norm(QString s) {
    static const QRegularExpression s_reLink(
        QStringLiteral("\\[([^\\]]*)\\]\\([^)]*\\)"));
    s.replace(s_reLink, QStringLiteral("\\1"));
    QString out;
    for (const QChar& c : s) {
        const ushort u = c.unicode();
        if (u == '#') continue;
        if (u == '|') continue;
        if (u == '>') continue;
        if (u == '*') continue;
        if (u == '`') continue;
        if (u == '_') continue;
        if (u == '~') continue;
        if (u == '-') continue;   // 分隔线/列表标记/表格对齐行
        if (c.isSpace()) continue;
        out += c;
    }
    return out;
}

int main(int argc, char** argv) {
    QApplication a(argc, argv);
    const QString sTarget = QStringLiteral(
        "E:/wosens/Programs/VisualC++/QT/AutoCode/doc/AutoCode改进计划-202611.md");
    if (!QFile::exists(sTarget)) {
        printf("[SKIP] %s\n", qUtf8Printable(sTarget));
        return 4;
    }
    const QString sRoot = QFileInfo(sTarget).absolutePath();

    // —— 微测试：直开 md，find/GotoPos 隔离 ——
    {
        CFilePreviewWidget w;
        w.resize(900, 700);
        w.show();
        QTest::qWait(100);
        w.ShowFile(sTarget);
        QApplication::processEvents();
        QTest::qWait(200);
        QTextBrowser* pMd = nullptr;
        const auto aVs = w.findChildren<QTextBrowser*>();
        for (QTextBrowser* p : aVs) {
            printf("  view[%s] visible=%d\n", p->metaObject()->className(),
                   p->isVisible() ? 1 : 0);
            if (p->isVisible() && pMd == nullptr) { pMd = p; }
        }
        if (pMd != nullptr) {
            const QString sAll = pMd->toPlainText();
            const QString sT = QStringLiteral("任务");
            int iCnt = 0, iAt = -1;
            while ((iAt = sAll.indexOf(sT, iAt + 1)) >= 0) { ++iCnt; }
            printf("  plainLen=%d contains=%d count=%d\n",
                   static_cast<int>(sAll.size()),
                   sAll.contains(sT) ? 1 : 0, iCnt);
            QTextCursor c0(pMd->document());
            pMd->setTextCursor(c0);
            const bool bF0 = pMd->find(sT);
            printf("  find#1=%d sel[%.20s]\n", bF0 ? 1 : 0,
                   qUtf8Printable(pMd->textCursor().selectedText()));
            w.GotoPos(3, 41, 2, sT, 0);
            QApplication::processEvents();
            printf("  GotoPos(3,41,2) sel[%.20s] blk[%.40s]\n",
                   qUtf8Printable(pMd->textCursor().selectedText()),
                   qUtf8Printable(pMd->document()->findBlock(
                       pMd->textCursor().selectionStart()).text()));
        }
    }

    int iBad = 0;
    int iTotal = 0;
    const QStringList aTerms = {
        QStringLiteral("任务"), QStringLiteral("回炉"),
        QStringLiteral("pi"),   QStringLiteral("验证"),
        QStringLiteral("token"),
    };
    for (const QString& sTerm : aTerms) {
        CExecutionPanel panel;
        panel.resize(1000, 700);
        panel.show();
        QTest::qWait(150);
        SFindTask stTask;
        stTask.m_sFind = sTerm;
        stTask.m_aRoots = QStringList{sRoot};
        stTask.m_sProjectDir = sRoot;
        stTask.m_aExcludeDirs = QStringList{QStringLiteral(".git")};
        panel.StartFileSearch(stTask);
        CFindResultsPage* pPage = panel.findChild<CFindResultsPage*>();
        QTableWidget* pTable = pPage ? pPage->findChild<QTableWidget*>() : nullptr;
        if (pTable == nullptr) {
            printf("[FAIL] no table\n");
            return 3;
        }
        for (int i = 0; i < 600 && pPage->SearchRunning(); ++i) {
            QTest::qWait(20);
        }
        QTest::qWait(150);
        // 目标文件的全部命中行。
        QList<int> aRows;
        for (int i = 0; i < pTable->rowCount(); ++i) {
            const QString sTip = pTable->item(i, 1)->toolTip();
            if (sTip.startsWith(sTarget + QStringLiteral("\n"))
                    || sTip.startsWith(QDir::toNativeSeparators(sTarget)
                                       + QStringLiteral("\n"))) {
                aRows.append(i);
            }
        }
        printf("== term[%ls] hits=%d ==\n", qUtf8Printable(sTerm), aRows.size());
        for (int iRow : aRows) {
            // tooltip: "path\n行 L:C\nlinetext"
            const QStringList aTip = pTable->item(iRow, 1)->toolTip()
                                         .split(QLatin1Char('\n'));
            const QString sPos = aTip.value(1);   // "行 L:C"
            const QString sLine = aTip.value(2);  // 源命中行
            const QRect stCell = pTable->visualItemRect(pTable->item(iRow, 0));
            QTest::mouseClick(pTable->viewport(), Qt::LeftButton, Qt::NoModifier,
                              stCell.center());
            QApplication::processEvents();
            QTest::qWait(30);
            CFilePreviewWidget* pPrev = panel.findChild<CFilePreviewWidget*>();
            QTextBrowser* pMd = nullptr;
            if (pPrev != nullptr) {
                const auto aViews = pPrev->findChildren<QTextBrowser*>();
                for (QTextBrowser* p : aViews) {
                    if (p->isVisible()) { pMd = p; break; }
                }
            }
            if (pMd == nullptr) {
                printf("  [%ls] %ls BAD: no visible browser\n",
                       qUtf8Printable(sTerm), qUtf8Printable(sPos));
                ++iBad; ++iTotal;
                continue;
            }
            const QString sSel = pMd->textCursor().selectedText();
            const QString sBlk = pMd->document()->findBlock(
                pMd->textCursor().selectionStart()).text();
            const bool bSelOk = (sSel.compare(sTerm, Qt::CaseInsensitive) == 0);
            const QString nB = Norm(sBlk), nL = Norm(sLine);
            const bool bBlkOk = nB.contains(nL) || nL.contains(nB);
            ++iTotal;
            if (!bSelOk || !bBlkOk || iTotal <= 6) {
                printf("  %s %s sel[%.30s] blk[%.50s]\n    src[%.80s]\n",
                       qUtf8Printable(sPos), (bSelOk && bBlkOk) ? "ok" : "BAD",
                       qUtf8Printable(sSel), qUtf8Printable(sBlk),
                       qUtf8Printable(sLine));
                if (!bSelOk || !bBlkOk) {
                    ++iBad;
                }
            }
        }
    }
    printf("total=%d bad=%d\n", iTotal, iBad);
    return iBad == 0 ? 0 : 3;
}
