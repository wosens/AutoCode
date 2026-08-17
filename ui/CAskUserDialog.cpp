#include "CAskUserDialog.h"

#include <QLabel>
#include <QListWidget>
#include <QPlainTextEdit>
#include <QDialogButtonBox>
#include <QVBoxLayout>

// 选项列表末尾固定的“其他”项文本
static const QString g_sOtherOptionText = QStringLiteral("其他（自行输入）");

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
    m_pLblQuestion->setStyleSheet(QStringLiteral("font-size: 12pt;"));

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
        pV->addWidget(m_pEditOther);
        connect(m_pList, &QListWidget::currentRowChanged, this, [this](int a_iRow) {
            const bool bOther = (m_pList->item(a_iRow) != nullptr)
                                && (m_pList->item(a_iRow)->text() == g_sOtherOptionText);
            m_pEditOther->setEnabled(bOther);
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
