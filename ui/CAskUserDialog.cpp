#include "CAskUserDialog.h"

#include <QFontMetrics>
#include <QGuiApplication>
#include <QLabel>
#include <QListWidget>
#include <QPlainTextEdit>
#include <QDialogButtonBox>
#include <QScreen>
#include <QVBoxLayout>

// 选项列表末尾固定的“其他”项文本
static const QString g_sOtherOptionText = QStringLiteral("其他（自行输入）");
// 2026-09-13 「其他（自行输入）」输入框固定高度（显隐时窗口高度同步增减同量）
static const int k_iOtherEditH = 96;

CAskUserDialog::CAskUserDialog(const QString& a_sQuestion,
                               const QStringList& a_aOptions,
                               QWidget* a_pParent)
    : QDialog(a_pParent) {
    setWindowTitle(QStringLiteral("助手提问"));
    resize(480, 360);

    m_pLblQuestion = new QLabel(this);
    m_pLblQuestion->setWordWrap(true);
    m_pLblQuestion->setTextInteractionFlags(Qt::TextSelectableByMouse);   // 支持选中复制
    m_pLblQuestion->setText(a_sQuestion);
    // 2026-09-13 字体改显式 QFont（原 stylesheet 字体要在 polish 后才生效，
    //   构造期 heightForWidth 会按旧字体算高度导致估算偏小）。
    QFont fntQ = m_pLblQuestion->font();
    fntQ.setPointSize(12);
    m_pLblQuestion->setFont(fntQ);

    auto* pV = new QVBoxLayout(this);
    pV->addWidget(m_pLblQuestion);

    if (!a_aOptions.isEmpty()) {
        // 选项模式：单选列表
        m_pList = new QListWidget(this);
        m_pList->setSelectionMode(QAbstractItemView::SingleSelection);
        QFont fntList = m_pList->font();
        fntList.setPointSize(11);   // 列表字体 11 号
        m_pList->setFont(fntList);
        for (const QString& sOpt : a_aOptions) {
            m_pList->addItem(sOpt);
        }
        m_pList->addItem(g_sOtherOptionText);   // 末尾固定“其他”项，允许自行输入
        if (m_pList->count() > 0) {
            m_pList->setCurrentRow(0);   // 默认选中首项，便于直接回车确认
        }
        connect(m_pList, &QListWidget::itemDoubleClicked,
                this, &CAskUserDialog::OnItemDoubleClicked);
        pV->addWidget(m_pList, 1);

        // “其他”项对应的自由输入框，仅在选中“其他”时可用
        m_pEditOther = new QPlainTextEdit(this);
        m_pEditOther->setPlaceholderText(QStringLiteral("请输入你的回答..."));
        m_pEditOther->setEnabled(false);
        m_pEditOther->setFixedHeight(k_iOtherEditH);   // 固定高度：显隐时窗口增减量精确对齐
        pV->addWidget(m_pEditOther);
        connect(m_pList, &QListWidget::currentRowChanged, this, [this](int a_iRow) {
            const bool bOther = (m_pList->item(a_iRow) != nullptr)
                                && (m_pList->item(a_iRow)->text() == g_sOtherOptionText);
            m_pEditOther->setEnabled(bOther);
            // 2026-09-13 「其他」显隐时同步增减窗口高度（±输入框固定高度），
            //   防固定窗口下问题区被挤压截断；高度钳位屏幕可用区。
            const bool bShowing = bOther && m_pEditOther->isHidden();
            const bool bHiding  = !bOther && !m_pEditOther->isHidden();
            if (bShowing || bHiding) {
                const QRect rcAvail = QGuiApplication::primaryScreen()->availableGeometry();
                const int iNewH = height() + (bShowing ? k_iOtherEditH : -k_iOtherEditH);
                resize(width(), qBound(240, iNewH, rcAvail.height() - 40));
            }
            m_pEditOther->setVisible(bOther);
            if (bOther) {
                m_pEditOther->setFocus();
            }
        });
        m_pEditOther->hide();
    } else {
        // 自由输入模式
        m_pEdit = new QPlainTextEdit(this);
        m_pEdit->setPlaceholderText(QStringLiteral("在此输入你的回答..."));
        pV->addWidget(m_pEdit, 1);
    }

    m_pBtns = new QDialogButtonBox(
        QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    connect(m_pBtns, &QDialogButtonBox::accepted, this, [this]() {
        if (m_pList != nullptr) {
            if (auto* pCur = m_pList->currentItem()) {
                if (pCur->text() == g_sOtherOptionText) {
                    // “其他”：取输入框文本，未输入则不关闭对话框
                    const QString sCustom = m_pEditOther->toPlainText().trimmed();
                    if (sCustom.isEmpty()) {
                        m_pEditOther->setFocus();
                        return;
                    }
                    m_sAnswer = sCustom;
                } else {
                    m_sAnswer = pCur->text();
                }
            }
        } else if (m_pEdit != nullptr) {
            m_sAnswer = m_pEdit->toPlainText().trimmed();
        }
        accept();
    });
    connect(m_pBtns, &QDialogButtonBox::rejected, this, [this]() {
        m_sAnswer.clear();
        reject();
    });
    pV->addWidget(m_pBtns);

    // —— 2026-09-13 窗口尺寸自适应（用户需求：尽量完整显示提问，放不下用 Tooltip）——
    //   高度 = 问题标签在给定宽度下的完整显示高度（QLabel wordWrap 支持 heightForWidth）
    //   + 其余部分最小高度；超长问题先加宽（480→760）减少换行行数，仍放不下则钳到
    //   屏幕可用区 3/4，并对问题标签设富文本 Tooltip（自动换行，悬停看全文）。
    const QMargins stMg = pV->contentsMargins();
    auto fnNeedH = [this, &stMg, &pV](int a_iW) -> int {
        const int iLabelW = a_iW - stMg.left() - stMg.right() - 4;
        int iNeed = m_pLblQuestion->heightForWidth(iLabelW);
        if (iNeed <= 0) {   // 理论不达（wordWrap 已开）：兑底按字体度量测换行高度
            const QFontMetrics fm(m_pLblQuestion->font());
            iNeed = fm.boundingRect(0, 0, iLabelW, 100000,
                                    Qt::TextWordWrap, m_pLblQuestion->text()).height();
        }
        return iNeed + pV->spacing() + 4;   // 标签与下一行的间距 + 余量
    };
    int iW = 480;
    int iNeedH = fnNeedH(iW);
    if (iNeedH > 240) {           // 问题较高：加宽窗口减少换行行数
        iW = 760;
        iNeedH = fnNeedH(iW);
    }
    const int iRestMin = (m_pList != nullptr ? 190 : 110)   // 选项列表 / 自由输入框
                       + 34                                  // 按钮行
                       + pV->spacing() * 2 + stMg.top() + stMg.bottom();
    int iH = iNeedH + iRestMin;
    const QRect rcAvail = QGuiApplication::primaryScreen()->availableGeometry();
    const int iMaxH = qMax(320, rcAvail.height() * 3 / 4);
    if (iH > iMaxH) {
        // 放不下：钳高度 + 富文本 Tooltip（自动换行）展示完整问题。问题标签高度
        //   钉死为窗口内可见配额（min=max）：wordWrap 标签的 heightForWidth 会把布局
        //   最小高度顶到全文高度，若只设 min 不设 max，布局激活后会把已钳位的窗口
        //   重新撑大到全文高度（实测 after-show 810 → 布局激活后 1192 的元凶）。
        iH = iMaxH;
        m_pLblQuestion->setToolTip(QStringLiteral("<html>%1</html>")
                                       .arg(a_sQuestion.toHtmlEscaped()));
        const int iLblH = qMax(140, iH - iRestMin);
        m_pLblQuestion->setMinimumHeight(iLblH);
        m_pLblQuestion->setMaximumHeight(iLblH);
    } else {
        m_pLblQuestion->setMinimumHeight(iNeedH);   // 强制布局给足问题完整高度
    }
    resize(iW, qMax(iH, 240));
}

QString CAskUserDialog::AnswerText() const {
    return m_sAnswer;
}

void CAskUserDialog::OnItemDoubleClicked() {
    if (m_pList != nullptr) {
        if (auto* pCur = m_pList->currentItem()) {
            if (pCur->text() == g_sOtherOptionText && m_pEditOther != nullptr) {
                m_pEditOther->setFocus();   // 双击“其他”→ 聚焦输入框，不直接确认
                return;
            }
            m_sAnswer = pCur->text();
        }
    }
    accept();
}
