#include "CDiffViewDialog.h"

#include <QGuiApplication>
#include <QScreen>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QPlainTextEdit>
#include <QSplitter>
#include <QTextBlock>
#include <QTextCursor>
#include <QFontDatabase>
#include <QScrollBar>

namespace {
void SetPt11(QWidget* a_pW) {
    QFont f = a_pW->font();
    f.setPointSize(11);
    a_pW->setFont(f);
}
// 变化区域底色（Beyond Compare 风格：旧=红、新=绿；浅色适配白底黑字）。
const QColor kClrOld(QStringLiteral("#ffd6d6"));
const QColor kClrNew(QStringLiteral("#d6ffd6"));
}   // namespace

CDiffViewDialog::CDiffViewDialog(const QString& a_sPath,
                                 const QByteArray& a_bufOld, const QByteArray& a_bufNew,
                                 bool a_bOldMissing, bool a_bNewMissing,
                                 const QString& a_sLabelOld, const QString& a_sLabelNew,
                                 QWidget* a_pParent, const QString& a_sCfgPath)
    : QDialog(a_pParent), m_sPath(a_sPath), m_bufOld(a_bufOld), m_bufNew(a_bufNew),
      m_bOldMissing(a_bOldMissing), m_bNewMissing(a_bNewMissing),
      m_sLabelOld(a_sLabelOld), m_sLabelNew(a_sLabelNew), m_sCfgPath(a_sCfgPath) {
    BuildUi();
    Fill();
}

void CDiffViewDialog::BuildUi() {
    setWindowTitle(QStringLiteral("查看变更 — %1").arg(m_sPath));
    resize(1100, 680);
    // 2026.08.29 恢复上次窗口几何（[Layout] DiffWin="x,y,w,h"；限幅屏幕可用区，
    //   坏档/无记录用默认 1100x680）。profile API 读写（与 CRollbackDialog 同口径）。
    if (!m_sCfgPath.isEmpty()) {
        wchar_t szBuf[128] = {0};
        if (GetPrivateProfileStringW(L"Layout", L"DiffWin", L"", szBuf, 128,
                                     m_sCfgPath.toStdWString().c_str()) > 0) {
            const QStringList aParts = QString::fromWCharArray(szBuf)
                                           .split(QLatin1Char(','));
            bool bOk[4] = {false, false, false, false};
            if (aParts.size() == 4) {
                const int iX = aParts[0].toInt(&bOk[0]);
                const int iY = aParts[1].toInt(&bOk[1]);
                const int iW = aParts[2].toInt(&bOk[2]);
                const int iH = aParts[3].toInt(&bOk[3]);
                const QRect rcDesk = QGuiApplication::primaryScreen()
                                         ->availableVirtualGeometry();
                if (bOk[0] && bOk[1] && bOk[2] && bOk[3]
                    && iW >= 640 && iH >= 420 && iW <= rcDesk.width()
                    && iH <= rcDesk.height()) {
                    setGeometry(QRect(qBound(rcDesk.left(), iX, rcDesk.right() - iW + 1),
                                      qBound(rcDesk.top(), iY, rcDesk.bottom() - iH + 1),
                                      iW, iH));
                }
            }
        }
    }

    auto* pV = new QVBoxLayout(this);

    // —— 顶部：左/右版本标签（各占一半，与下方编辑器对齐）——
    auto* pHdr = new QHBoxLayout();
    m_pLblOld = new QLabel(this);
    m_pLblNew = new QLabel(this);
    for (QLabel* pLbl : {m_pLblOld, m_pLblNew}) {
        SetPt11(pLbl);
        pLbl->setStyleSheet(QStringLiteral("color:#6b7280;"));
        pLbl->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
        pLbl->setMinimumWidth(0);
    }
    pHdr->addWidget(m_pLblOld);
    pHdr->addWidget(m_pLblNew);
    pV->addLayout(pHdr);

    // —— 双栏并排（等宽只读 + 滚动联动）——
    auto* pSplit = new QSplitter(Qt::Horizontal, this);
    SetPt11(pSplit);
    auto fnMakePane = [this]() {
        auto* pTxt = new QPlainTextEdit(this);
        SetPt11(pTxt);
        QFont fMono = QFontDatabase::systemFont(QFontDatabase::FixedFont);
        fMono.setPointSize(10);
        pTxt->setFont(fMono);
        pTxt->setReadOnly(true);
        pTxt->setLineWrapMode(QPlainTextEdit::NoWrap);
        pTxt->setWordWrapMode(QTextOption::NoWrap);
        return pTxt;
    };
    m_pTxtOld = fnMakePane();
    m_pTxtNew = fnMakePane();
    pSplit->addWidget(m_pTxtOld);
    pSplit->addWidget(m_pTxtNew);
    pSplit->setStretchFactor(0, 1);
    pSplit->setStretchFactor(1, 1);
    pSplit->setChildrenCollapsible(false);
    // 50:50 等分且禁拖歪（对齐视图两栏必须同宽）。
    pSplit->setSizes({1, 1});
    pV->addWidget(pSplit, 1);

    // —— 底部按钮行 ——
    auto* pH = new QHBoxLayout();
    pH->addStretch(1);
    auto* pBtnClose = new QPushButton(QStringLiteral("关闭"), this);
    SetPt11(pBtnClose);
    connect(pBtnClose, &QPushButton::clicked, this, &QDialog::reject);
    pH->addWidget(pBtnClose);
    pV->addLayout(pH);

    // 滚动联动：任一侧滚动 → 同步另一侧（防递归：同步期间断开对端信号）。
    auto fnBind = [](QPlainTextEdit* a, QPlainTextEdit* b) {
        connect(a->verticalScrollBar(), &QScrollBar::valueChanged, a, [a, b](int a_iVal) {
            if (b->verticalScrollBar()->value() != a_iVal) {
                b->verticalScrollBar()->setValue(a_iVal);
            }
        });
    };
    fnBind(m_pTxtOld, m_pTxtNew);
    fnBind(m_pTxtNew, m_pTxtOld);
}

void CDiffViewDialog::AlignSides(const QList<QString>& a_aOld, const QList<QString>& a_aNew,
                                 QList<QString>& a_aOldOut, QList<QString>& a_aNewOut,
                                 int& a_iOldFrom, int& a_iOldTo,
                                 int& a_iNewFrom, int& a_iNewTo) {
    // 公共前缀。
    int iPre = 0;
    while (iPre < a_aOld.size() && iPre < a_aNew.size()
           && a_aOld[iPre] == a_aNew[iPre]) {
        ++iPre;
    }
    // 公共后缀（不与前缀重叠）。
    int iSuf = 0;
    while (iSuf < a_aOld.size() - iPre && iSuf < a_aNew.size() - iPre
           && a_aOld[a_aOld.size() - 1 - iSuf] == a_aNew[a_aNew.size() - 1 - iSuf]) {
        ++iSuf;
    }
    const int iMidOld = a_aOld.size() - iPre - iSuf;
    const int iMidNew = a_aNew.size() - iPre - iSuf;
    const int iPadOld = qMax(0, iMidNew - iMidOld);   // 旧侧缺的行数 → 旧侧补空行
    const int iPadNew = qMax(0, iMidOld - iMidNew);

    a_aOldOut = a_aOld;
    a_aNewOut = a_aNew;
    for (int i = 0; i < iPadOld; ++i) {
        a_aOldOut.insert(iPre + iMidOld, QString());
    }
    for (int i = 0; i < iPadNew; ++i) {
        a_aNewOut.insert(iPre + iMidNew, QString());
    }
    // 变化区间（补位后的行号，含端点；区间为空表示两侧内容完全一致）。
    a_iOldFrom = iPre;
    a_iOldTo   = iPre + iMidOld - 1;
    a_iNewFrom = iPre;
    a_iNewTo   = iPre + iMidNew - 1;
}

void CDiffViewDialog::Fill() {
    m_pLblOld->setText(m_sLabelOld);
    m_pLblNew->setText(m_sLabelNew);

    // 单侧缺失（新增/删除）：缺失侧占位说明，另一侧全文高亮。
    if (m_bOldMissing || m_bNewMissing) {
        if (m_bOldMissing) {
            m_pTxtOld->setPlainText(QStringLiteral("（该记录点中不存在此文件）"));
        }
        if (m_bNewMissing) {
            m_pTxtNew->setPlainText(QStringLiteral("（该记录点中不存在此文件）"));
        }
        QPlainTextEdit* pFull = m_bOldMissing ? m_pTxtNew : m_pTxtOld;
        pFull->setPlainText(QString::fromUtf8(m_bOldMissing ? m_bufNew : m_bufOld));
        QList<QTextEdit::ExtraSelection> aSel;
        QTextCursor stCur(pFull->document());
        stCur.movePosition(QTextCursor::Start);
        stCur.movePosition(QTextCursor::End, QTextCursor::KeepAnchor);
        QTextEdit::ExtraSelection st;
        st.cursor = stCur;
        st.format.setBackground(m_bOldMissing ? kClrNew : kClrOld);
        aSel.append(st);
        pFull->setExtraSelections(aSel);
        return;
    }

    // 二进制内容（含 NUL）：不做文本对比，给出说明。
    if (m_bufOld.contains('\0') || m_bufNew.contains('\0')) {
        const QString sMsg = QStringLiteral(
            "（二进制文件，不进行文本对比）\n\n旧 %1 字节 → 新 %2 字节")
            .arg(m_bufOld.size()).arg(m_bufNew.size());
        m_pTxtOld->setPlainText(sMsg);
        m_pTxtNew->setPlainText(sMsg);
        return;
    }

    const QList<QString> aOld = QString::fromUtf8(m_bufOld).split('\n');
    const QList<QString> aNew = QString::fromUtf8(m_bufNew).split('\n');
    QList<QString> aOldAl, aNewAl;
    int iOldFrom = 0, iOldTo = -1, iNewFrom = 0, iNewTo = -1;
    AlignSides(aOld, aNew, aOldAl, aNewAl, iOldFrom, iOldTo, iNewFrom, iNewTo);

    m_pTxtOld->setPlainText(aOldAl.join(QLatin1Char('\n')));
    m_pTxtNew->setPlainText(aNewAl.join(QLatin1Char('\n')));

    // 变化区域底色（行号 → 文本块整行选区；区间为空=无变化不上色）。
    auto fnHighlight = [](QPlainTextEdit* a_pTxt, int a_iFrom, int a_iTo, const QColor& a_clr) {
        if (a_iFrom > a_iTo) {
            return;
        }
        QList<QTextEdit::ExtraSelection> aSel;
        for (int i = a_iFrom; i <= a_iTo && i < a_pTxt->document()->blockCount(); ++i) {
            QTextBlock stBlk = a_pTxt->document()->findBlockByNumber(i);
            if (!stBlk.isValid()) {
                continue;
            }
            QTextCursor stCur(stBlk);
            stCur.movePosition(QTextCursor::EndOfBlock, QTextCursor::KeepAnchor);
            QTextEdit::ExtraSelection st;
            st.cursor = stCur;
            st.format.setBackground(a_clr);
            aSel.append(st);
        }
        a_pTxt->setExtraSelections(aSel);
    };
    fnHighlight(m_pTxtOld, iOldFrom, iOldTo, kClrOld);
    fnHighlight(m_pTxtNew, iNewFrom, iNewTo, kClrNew);

    // 定位到第一处变化。
    const int iFirst = qMax(0, qMin(iOldFrom, iNewFrom) - 2);
    for (QPlainTextEdit* pTxt : {m_pTxtOld, m_pTxtNew}) {
        QTextCursor stCur(pTxt->document()->findBlockByNumber(iFirst));
        pTxt->setTextCursor(stCur);
    }
}

void CDiffViewDialog::hideEvent(QHideEvent* a_pEvent) {
    // 2026.08.29 关闭时保存窗口几何（exec 结束 hide 必经；析构不重复写）。
    if (!m_sCfgPath.isEmpty()) {
        const QString sVal = QStringLiteral("%1,%2,%3,%4")
                                 .arg(QString::number(x()), QString::number(y()),
                                      QString::number(width()), QString::number(height()));
        WritePrivateProfileStringW(L"Layout", L"DiffWin", sVal.toStdWString().c_str(),
                                   m_sCfgPath.toStdWString().c_str());
    }
    QDialog::hideEvent(a_pEvent);
}
