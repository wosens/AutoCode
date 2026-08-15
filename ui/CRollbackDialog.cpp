#include "CRollbackDialog.h"

#include <QListWidget>
#include <QListWidgetItem>
#include <QDialogButtonBox>
#include <QVBoxLayout>
#include <QLabel>
#include <QFont>
#include <QPushButton>

CRollbackDialog::CRollbackDialog(const QList<SBackupVersion>& a_aVersions,
                                 QWidget* a_pParent)
    : QDialog(a_pParent), m_aVersions(a_aVersions) {
    setWindowTitle(QStringLiteral("代码回退 — 选择历史版本"));
    setMinimumSize(520, 360);

    auto* pV = new QVBoxLayout(this);

    auto* pLblHint = new QLabel(
        QStringLiteral("选择要恢复到的源码版本（双击或选中后点「恢复」）："), this);
    QFont fHint = pLblHint->font();
    fHint.setPointSize(11);
    pLblHint->setFont(fHint);
    pV->addWidget(pLblHint);

    m_pList = new QListWidget(this);
    QFont fList = m_pList->font();
    fList.setPointSize(11);
    m_pList->setFont(fList);
    // 倒序展示（m_aVersions 调用方已按序号倒序传入）。
    for (const SBackupVersion& st : m_aVersions) {
        // 摘要：首次版本（summary 空）显示「（首次备份）」；否则显示摘要（过长截断）。
        QString sSum = st.sSummary.trimmed();
        if (sSum.isEmpty()) {
            sSum = QStringLiteral("（首次备份，无修改记录）");
        } else if (sSum.length() > 60) {
            sSum = sSum.left(60) + QStringLiteral("…");
        }
        // 时间美化：ISO → 去掉 T 与秒以下。
        QString sTime = st.sTime;
        sTime.replace(QLatin1Char('T'), QLatin1Char(' '));
        if (sTime.length() > 16) {
            sTime = sTime.left(16);
        }
        const QString sText = QStringLiteral("#%1  %2  [%3]  %4")
            .arg(st.iSeq)
            .arg(st.sStage.isEmpty() ? QStringLiteral("改码阶段") : st.sStage)
            .arg(sTime)
            .arg(sSum);
        auto* pItem = new QListWidgetItem(sText, m_pList);
        pItem->setData(Qt::UserRole, st.iSeq);
    }
    if (m_pList->count() > 0) {
        m_pList->setCurrentRow(0);   // 默认选最新版本
    }
    connect(m_pList, &QListWidget::currentItemChanged,
            this, &CRollbackDialog::OnItemChanged);
    connect(m_pList, &QListWidget::itemDoubleClicked,
            this, &CRollbackDialog::OnItemDoubleClicked);
    pV->addWidget(m_pList, 1);

    m_pButtons = new QDialogButtonBox(
        QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    // 文案改为「恢复 / 取消」，更贴合回退语义。
    m_pButtons->button(QDialogButtonBox::Ok)->setText(QStringLiteral("恢复"));
    m_pButtons->button(QDialogButtonBox::Cancel)->setText(QStringLiteral("取消"));
    QFont fBtn = m_pButtons->font();
    fBtn.setPointSize(11);
    m_pButtons->setFont(fBtn);
    connect(m_pButtons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(m_pButtons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    pV->addWidget(m_pButtons);
}

void CRollbackDialog::OnItemChanged(QListWidgetItem* a_pCur, QListWidgetItem* /*a_pPrev*/) {
    if (a_pCur == nullptr) {
        m_iSelected = -1;
        return;
    }
    m_iSelected = a_pCur->data(Qt::UserRole).toInt();
}

void CRollbackDialog::OnItemDoubleClicked(QListWidgetItem* a_pItem) {
    // 双击 = 选中并直接确认（与「恢复」按钮等价）。
    if (a_pItem != nullptr) {
        m_iSelected = a_pItem->data(Qt::UserRole).toInt();
        accept();
    }
}
