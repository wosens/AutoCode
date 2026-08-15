#include "CAskUserDialog.h"

#include <QLabel>
#include <QListWidget>
#include <QPlainTextEdit>
#include <QDialogButtonBox>
#include <QVBoxLayout>

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
        for (const QString& sOpt : a_aOptions) {
            m_pList->addItem(sOpt);
        }
        if (m_pList->count() > 0) {
            m_pList->setCurrentRow(0);   // 默认选中首项，便于直接回车确认
        }
        connect(m_pList, &QListWidget::itemDoubleClicked,
                this, &CAskUserDialog::OnItemDoubleClicked);
        pV->addWidget(m_pList, 1);
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
                m_sAnswer = pCur->text();
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
            m_sAnswer = pCur->text();
        }
    }
    accept();
}
