// probe_reviewtabs.cpp —— 审查窗口 Tab 改造验证（临时，验证后删除）。
// 步骤（argv[2]）：
//   1=inspect  开窗即查（Tab 数/下拉项/全量开关/清单行+复选/状态列）
//   2=run      全量关闭下点「开始审查」→ 等完成（真后端 LLM）→ 打印发现
//   3=reopen   重开：未勾全量清单应空（基线已前移）；勾全量后应全量列出（无变化）
//   4=incr     改一个文件后开窗：只列该文件（修改）→ 审查 → 增量口径+基线前移
//   5=filter   下拉切 Python（应空）/ C/C++（应全列）
//   6=fixprompt 取消勾选一条发现 → 按建议修复提示词应只含剩余勾选行
#include <QApplication>
#include <QTimer>
#include <QTabWidget>
#include <QTreeWidget>
#include <QComboBox>
#include <QCheckBox>
#include <QPushButton>
#include <QLabel>
#include <QDir>
#include <QFileInfo>
#include <cstdio>

#include "CCodeReviewDialog.h"

static CCodeReviewDialog* g_pDlg = nullptr;

static QTreeWidget* TreeOf(int a_iCols) {
    const auto aTrees = g_pDlg->findChildren<QTreeWidget*>();
    for (QTreeWidget* pT : aTrees) {
        if (pT->columnCount() == a_iCols) {
            return pT;
        }
    }
    return nullptr;
}

static void DumpFiles(const char* a_szTag) {
    QTreeWidget* pT = TreeOf(5);
    if (pT == nullptr) {
        printf("%s: NO FILE TREE\n", a_szTag);
        return;
    }
    printf("%s: fileRows=%d\n", a_szTag, pT->topLevelItemCount());
    for (int i = 0; i < pT->topLevelItemCount(); ++i) {
        const QTreeWidgetItem* pItem = pT->topLevelItem(i);
        printf("  [%s] %s  %s  %s\n",
               pItem->checkState(0) == Qt::Checked ? "x" : " ",
               qPrintable(pItem->text(1)), qPrintable(pItem->text(2)),
               qPrintable(pItem->text(3)));
    }
    fflush(stdout);
}

static void DumpFindings(const char* a_szTag) {
    QTreeWidget* pT = TreeOf(7);
    if (pT == nullptr) {
        printf("%s: NO FIND TREE\n", a_szTag);
        return;
    }
    printf("%s: findRows=%d\n", a_szTag, pT->topLevelItemCount());
    for (int i = 0; i < pT->topLevelItemCount(); ++i) {
        const QTreeWidgetItem* pItem = pT->topLevelItem(i);
        printf("  [%s] %s:%s %s\n",
               pItem->checkState(0) == Qt::Checked ? "x" : " ",
               qPrintable(pItem->text(2)), qPrintable(pItem->text(3)),
               qPrintable(pItem->text(5).left(46)));
    }
    fflush(stdout);
}

static QLabel* StatusLabel() {
    const auto aLbls = g_pDlg->findChildren<QLabel*>();
    for (QLabel* pL : aLbls) {
        if (pL->text().contains(QStringLiteral("审查")) || pL->text().contains(QStringLiteral("就绪"))) {
            return pL;
        }
    }
    return aLbls.isEmpty() ? nullptr : aLbls.last();
}

static QPushButton* BtnStart() {
    const auto aBtns = g_pDlg->findChildren<QPushButton*>();
    for (QPushButton* pB : aBtns) {
        if (pB->text() == QStringLiteral("开始审查")) {
            return pB;
        }
    }
    return nullptr;
}

static void WaitReviewDone(int a_iMaxSec) {
    auto* pTimer = new QTimer(QApplication::instance());
    int iTicks = 0;
    QObject::connect(pTimer, &QTimer::timeout, [&]() {
        ++iTicks;
        QPushButton* pBtn = BtnStart();
        const bool bDone = (pBtn != nullptr && pBtn->isEnabled() && iTicks > 3);
        if (bDone || iTicks > a_iMaxSec / 2) {
            QLabel* pS = StatusLabel();
            printf("review-%s ticks=%d status=%s\n", bDone ? "done" : "TIMEOUT", iTicks,
                   pS ? qPrintable(pS->text().left(160)) : "?");
            DumpFindings("after");
            pTimer->stop();
            g_pDlg->close();
            QTimer::singleShot(300, &QApplication::quit);
        }
    });
    pTimer->start(2000);
}

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    const QString sTarget = QString::fromLocal8Bit(argv[1]);
    const int iStep = atoi(argv[2]);

    g_pDlg = new CCodeReviewDialog(sTarget, nullptr);
    g_pDlg->show();

    if (iStep == 1) {
        QTimer::singleShot(1200, [&]() {
            const auto aTabs = g_pDlg->findChildren<QTabWidget*>();
            printf("tabs=%d", aTabs.isEmpty() ? 0 : aTabs.first()->count());
            if (!aTabs.isEmpty()) {
                for (int i = 0; i < aTabs.first()->count(); ++i) {
                    printf(" [%s]", qPrintable(aTabs.first()->tabText(i)));
                }
            }
            printf("\n");
            QComboBox* pCb = g_pDlg->findChild<QComboBox*>();
            if (pCb != nullptr) {
                printf("combo:");
                for (int i = 0; i < pCb->count(); ++i) {
                    printf(" %s", qPrintable(pCb->itemText(i)));
                }
                printf(" (cur=%s)\n", qPrintable(pCb->currentText()));
            }
            QCheckBox* pChk = g_pDlg->findChild<QCheckBox*>();
            printf("fullChecked=%d\n", pChk != nullptr && pChk->isChecked() ? 1 : 0);
            DumpFiles("inspect");
            DumpFindings("inspect");
            g_pDlg->close();
            QTimer::singleShot(200, &QApplication::quit);
        });
    } else if (iStep == 2 || iStep == 4) {
        QTimer::singleShot(1500, [&]() {
            DumpFiles(iStep == 2 ? "before-run" : "before-incr");
            QPushButton* pBtn = BtnStart();
            if (pBtn == nullptr) {
                printf("NO START BTN\n");
                QTimer::singleShot(200, &QApplication::quit);
                return;
            }
            pBtn->click();
            WaitReviewDone(600);
        });
    } else if (iStep == 3) {
        QTimer::singleShot(1500, [&]() {
            DumpFiles("reopen-nofull");
            QCheckBox* pChk = g_pDlg->findChild<QCheckBox*>();
            if (pChk != nullptr) {
                pChk->setChecked(true);   // 触发 toggled → 重建清单
            }
            QTimer::singleShot(600, [&]() {
                DumpFiles("reopen-full");
                DumpFindings("reopen");
                g_pDlg->close();
                QTimer::singleShot(200, &QApplication::quit);
            });
        });
    } else if (iStep == 5) {
        QTimer::singleShot(1500, [&]() {
            QComboBox* pCb = g_pDlg->findChild<QComboBox*>();
            pCb->setCurrentText(QStringLiteral("Python"));
            QTimer::singleShot(400, [&]() {
                DumpFiles("py");
                g_pDlg->findChild<QComboBox*>()->setCurrentText(QStringLiteral("C/C++"));
                QTimer::singleShot(400, [&]() {
                    DumpFiles("cpp");
                    g_pDlg->close();
                    QTimer::singleShot(200, &QApplication::quit);
                });
            });
        });
    } else if (iStep == 6) {
        // 命中缓存回填后：取消勾选第一条发现 → 按建议修复提示词应只含剩余勾选行。
        QString sFix;
        QObject::connect(g_pDlg, &CCodeReviewDialog::sigFixToChat,
                         [&sFix](const QString& a_sP) { sFix = a_sP; });
        QTimer::singleShot(1500, [&]() {
            QTreeWidget* pT = TreeOf(7);
            printf("findRows=%d\n", pT ? pT->topLevelItemCount() : -1);
            if (pT != nullptr && pT->topLevelItemCount() >= 2) {
                pT->topLevelItem(0)->setCheckState(0, Qt::Unchecked);
            }
            QTimer::singleShot(400, [&]() {
                QPushButton* pFix = nullptr;
                for (QPushButton* pB : g_pDlg->findChildren<QPushButton*>()) {
                    if (pB->text() == QStringLiteral("按建议修复")) {
                        pFix = pB;
                    }
                }
                printf("fix-enabled=%d\n", pFix != nullptr && pFix->isEnabled() ? 1 : 0);
                if (pFix != nullptr && pFix->isEnabled()) {
                    pFix->click();
                    printf("fix-prompt-lines=%d\n", sFix.count(QLatin1Char('\n')));
                }
                g_pDlg->close();
                QTimer::singleShot(200, &QApplication::quit);
            });
        });
    }
    return app.exec();
}
