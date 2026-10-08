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

#include <algorithm>
#include <vector>

namespace {
void SetPt11(QWidget* a_pW) {
    QFont f = a_pW->font();
    f.setPointSize(11);
    a_pW->setFont(f);
}
// 变化区域底色（Beyond Compare 风格；浅色适配白底黑字）。
const QColor kClrOld(QStringLiteral("#ffd6d6"));   // 删除行（旧侧有、新侧无）
const QColor kClrNew(QStringLiteral("#d6ffd6"));   // 新增行（新侧有、旧侧无）
const QColor kClrPad(QStringLiteral("#f2f2f2"));   // 对齐占位行（该侧缺失）
// 行对齐占位哨兵（区别于真实空行：真实空行是公共行不着色，占位行灰底）。
const QChar kPad = QChar(0x01);
// LCS 行对齐上限：O(n·m) 内存/耗时保护，超限退化前后缀整块对齐（保可用性）。
constexpr int k_iLcsMaxLines = 6000;

// 按通用换行拆行（吞 \r，保留空行结构——空行参与对齐）。
QList<QString> SplitLines(const QByteArray& a_buf) {
    const QString s = QString::fromUtf8(a_buf);
    QList<QString> aLines;
    int iStart = 0;
    for (int i = 0; i < s.size(); ++i) {
        if (s.at(i) == QLatin1Char('\n')) {
            int iEnd = i;
            if (iEnd > iStart && s.at(iEnd - 1) == QLatin1Char('\r')) {
                --iEnd;
            }
            aLines.append(s.mid(iStart, iEnd - iStart));
            iStart = i + 1;
        }
    }
    aLines.append(s.mid(iStart));
    return aLines;
}

// LCS 按行对齐（2026.09.04 对齐 HarnessAgent HistoryDiffDialog 口径）：逐行求最长
//   公共子序列，缺失侧插入占位行——只有真正增/删/改的行着色，公共行不着色。
//   旧实现用「公共前缀+公共后缀」把中间整块标色：改动一行会带红/绿几十行上下文，
//   看不出实质改了什么（用户反馈不如 HarnessAgent 的根源）。
void AlignLinesLcs(const QList<QString>& a_a, const QList<QString>& a_b,
                   QList<QString>& a_aOut, QList<QString>& a_bOut) {
    a_aOut.clear();
    a_bOut.clear();
    const int iN = a_a.size(), iM = a_b.size();
    if (iN > k_iLcsMaxLines || iM > k_iLcsMaxLines) {
        // 超限退化：直接整块并排（不插占位，着色按整块区间）。
        a_aOut = a_a;
        a_bOut = a_b;
        return;
    }
    std::vector<std::vector<int>> vvDp(static_cast<size_t>(iN) + 1,
                                       std::vector<int>(static_cast<size_t>(iM) + 1, 0));
    for (int i = iN - 1; i >= 0; --i) {
        for (int j = iM - 1; j >= 0; --j) {
            vvDp[i][j] = (a_a.at(i) == a_b.at(j))
                ? vvDp[i + 1][j + 1] + 1
                : std::max(vvDp[i + 1][j], vvDp[i][j + 1]);
        }
    }
    int i = 0, j = 0;
    while (i < iN && j < iM) {
        if (a_a.at(i) == a_b.at(j)) {
            a_aOut.append(a_a.at(i));   // 公共行
            a_bOut.append(a_b.at(j));
            ++i; ++j;
        } else if (vvDp[i + 1][j] >= vvDp[i][j + 1]) {
            a_aOut.append(a_a.at(i));   // 旧侧删除行
            a_bOut.append(QString(kPad));
            ++i;
        } else {
            a_aOut.append(QString(kPad));   // 新侧新增行
            a_bOut.append(a_b.at(j));
            ++j;
        }
    }
    while (i < iN) { a_aOut.append(a_a.at(i)); a_bOut.append(QString(kPad)); ++i; }
    while (j < iM) { a_aOut.append(QString(kPad)); a_bOut.append(a_b.at(j)); ++j; }
}
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
    setMinimumSize(640, 420);
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
                    // 2026-10-08 修「每次弹出往上跑」：setGeometry 改 move()+resize()
                    //   （同 CRecentProjectsDialog/CRollbackDialog/CSettingsDialog
                    //   2026.09.25 修复——Qt6.8.3 顶层窗口 setGeometry 客户区落点恒比
                    //   请求 y 高 31px，hideEvent 存回偏移值 → 逐次上漂；move()+
                    //   resize() 走精确客户区路径不漂）。
                    move(QPoint(qBound(rcDesk.left(), iX, rcDesk.right() - iW + 1),
                                qBound(rcDesk.top(), iY, rcDesk.bottom() - iH + 1)));
                    resize(iW, iH);
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

void CDiffViewDialog::Fill() {
    m_pLblOld->setText(m_sLabelOld);
    m_pLblNew->setText(m_sLabelNew);

    // 单侧缺失（新增/删除文件）：缺失侧占位说明，另一侧全文高亮。
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

    // 2026.09.04 LCS 行级对齐（对齐 HarnessAgent HistoryDiffDialog）：逐行最长公共
    //   子序列，缺失侧插占位行；只有真正变更的行着色（删除=旧侧红 / 新增=新侧绿 /
    //   占位=灰），公共行不着色——替代旧「前后缀夹逼整块标色」（改一行红绿几十行，
    //   看不出实质修改）。
    const QList<QString> aOld = SplitLines(m_bufOld);
    const QList<QString> aNew = SplitLines(m_bufNew);
    QList<QString> aOldAl, aNewAl;
    AlignLinesLcs(aOld, aNew, aOldAl, aNewAl);

    // 占位行显示为空行（哨兵不出现在可见文本里）。
    QList<QString> aOldShow, aNewShow;
    for (const QString& s : aOldAl) { aOldShow.append(s.contains(kPad) ? QString() : s); }
    for (const QString& s : aNewAl) { aNewShow.append(s.contains(kPad) ? QString() : s); }
    m_pTxtOld->setPlainText(aOldShow.join(QLatin1Char('\n')));
    m_pTxtNew->setPlainText(aNewShow.join(QLatin1Char('\n')));

    // 行级着色：本侧占位=灰；本侧行与对侧同位不同（或对侧占位）=变更色；完全相同=不着色。
    auto fnMark = [this](QPlainTextEdit* a_pEdit, const QList<QString>& aSelf,
                         const QList<QString>& aOther, const QColor& a_clrChange) {
        QList<QTextEdit::ExtraSelection> aSel;
        QTextBlock stBlk = a_pEdit->document()->firstBlock();
        for (int k = 0; k < aSelf.size() && stBlk.isValid(); ++k, stBlk = stBlk.next()) {
            const bool bPadSelf  = aSelf.at(k).contains(kPad);
            const bool bPadOther = (k < aOther.size()) && aOther.at(k).contains(kPad);
            const bool bSame     = !bPadSelf && !bPadOther
                                   && (k < aOther.size()) && aSelf.at(k) == aOther.at(k);
            if (bSame) {
                continue;   // 公共行不着色（正是 LCS 对齐的价值所在）
            }
            QTextEdit::ExtraSelection stEs;
            stEs.cursor = QTextCursor(stBlk);
            stEs.cursor.movePosition(QTextCursor::EndOfBlock, QTextCursor::KeepAnchor);
            stEs.format.setBackground(bPadSelf ? kClrPad : a_clrChange);
            aSel.append(stEs);
        }
        a_pEdit->setExtraSelections(aSel);
    };
    fnMark(m_pTxtOld, aOldAl, aNewAl, kClrOld);
    fnMark(m_pTxtNew, aNewAl, aOldAl, kClrNew);

    // 定位到第一处变更（行号 = 首个非公共位；两侧同步置光标并居中视口）。
    int iFirst = -1;
    const int iRows = qMin(aOldAl.size(), aNewAl.size());
    for (int k = 0; k < iRows; ++k) {
        const bool bPadSelf  = aOldAl.at(k).contains(kPad);
        const bool bPadOther = aNewAl.at(k).contains(kPad);
        if (bPadSelf || bPadOther || aOldAl.at(k) != aNewAl.at(k)) {
            iFirst = k;
            break;
        }
    }
    if (iFirst < 0 && aOldAl.size() != aNewAl.size()) {
        iFirst = iRows;   // 尾部纯增删
    }
    if (iFirst > 0) {
        for (QPlainTextEdit* pTxt : {m_pTxtOld, m_pTxtNew}) {
            QTextCursor stCur(pTxt->document()->findBlockByNumber(iFirst));
            pTxt->setTextCursor(stCur);
            pTxt->centerCursor();
        }
    }
}

void CDiffViewDialog::showEvent(QShowEvent* a_pEvent) {
    QDialog::showEvent(a_pEvent);
    // 2026.09.04 标题栏防超屏兜底（用户实测标题栏被顶出屏幕外不可见——恢复几何来自
    //   坏档/异 DPI/异屏数会话时，构建期 qBound 钳制仍可能被窗口管理器布局覆盖）：
    //   显示后按「标题栏中点所在屏」校正——不在任何屏内则整体钳回该屏可用区。
    const QRect rcFrame = frameGeometry();
    const QPoint ptTitle(rcFrame.center().x(), rcFrame.top() + 8);   // 标题栏中点
    QScreen* pScr = QGuiApplication::screenAt(ptTitle);
    if (pScr == nullptr) {
        pScr = QGuiApplication::primaryScreen();
    }
    if (pScr != nullptr) {
        const QRect rcAvail = pScr->availableGeometry();
        const int iW = rcFrame.width(), iH = rcFrame.height();
        const int iX = qBound(rcAvail.left(), rcFrame.left(),
                              rcAvail.right() - iW + 1);
        const int iY = qBound(rcAvail.top(), rcFrame.top(),
                              rcAvail.bottom() - iH + 1);
        if (iX != rcFrame.left() || iY != rcFrame.top()) {
            move(iX, iY);
        }
    }
}

void CDiffViewDialog::hideEvent(QHideEvent* a_pEvent) {
    // 2026.08.29 关闭时保存窗口几何（exec 结束 hide 必经；析构不重复写）。
    // 2026.09.04 最大化/最小化状态不存（存的是还原几何，且部分 WM 下含阴影 frame
    //   负偏移——恢复后正好把标题栏顶出屏幕，即用户反馈的超屏来源之一）。
    if (!m_sCfgPath.isEmpty() && !isMaximized() && !isMinimized()) {
        const QString sVal = QStringLiteral("%1,%2,%3,%4")
                                 .arg(QString::number(x()), QString::number(y()),
                                      QString::number(width()), QString::number(height()));
        WritePrivateProfileStringW(L"Layout", L"DiffWin", sVal.toStdWString().c_str(),
                                   m_sCfgPath.toStdWString().c_str());
    }
    QDialog::hideEvent(a_pEvent);
}
