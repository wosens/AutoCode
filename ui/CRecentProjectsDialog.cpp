#include "CRecentProjectsDialog.h"

#include <QListWidget>
#include <QDialogButtonBox>
#include <QVBoxLayout>
#include <QDir>
#include <QFileInfo>

CRecentProjectsDialog::CRecentProjectsDialog(const QStringList& a_aRecent, QWidget* a_pParent)
    : QDialog(a_pParent) {
    setWindowTitle(QStringLiteral("最近打开的工程"));
    resize(620, 420);

    m_pList = new QListWidget(this);
    m_pList->setSelectionMode(QAbstractItemView::SingleSelection);

    // 填列表：每项文本「目录名（完整路径）」，UserRole 存绝对路径以便精确取回。
    for (const QString& sPath : a_aRecent) {
        const QString sName = QFileInfo(sPath).fileName();
        const QString sText = QStringLiteral("%1（%2）").arg(sName, sPath);
        auto* pItem = new QListWidgetItem(sText, m_pList);
        pItem->setData(Qt::UserRole, sPath);
    }

    m_pBtns = new QDialogButtonBox(
        QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    connect(m_pBtns, &QDialogButtonBox::accepted, this, [this]() {
        // OK：取当前选中项；无选中则 m_sSelected 保持空但仍 accept（由调用方判空）。
        if (auto* pCur = m_pList->currentItem()) {
            m_sSelected = pCur->data(Qt::UserRole).toString();
        } else {
            m_sSelected.clear();
        }
        accept();
    });
    connect(m_pBtns, &QDialogButtonBox::rejected, this, [this]() {
        m_sSelected.clear();
        reject();
    });

    // 双击 = 确认该项。
    connect(m_pList, &QListWidget::itemDoubleClicked,
            this, &CRecentProjectsDialog::OnItemDoubleClicked);

    auto* pV = new QVBoxLayout(this);
    pV->addWidget(m_pList);
    pV->addWidget(m_pBtns);
}

QString CRecentProjectsDialog::SelectedPath() const {
    return m_sSelected;
}

void CRecentProjectsDialog::OnItemDoubleClicked() {
    // 双击当前项 → 记录路径并 accept。
    if (auto* pCur = m_pList->currentItem()) {
        m_sSelected = pCur->data(Qt::UserRole).toString();
    } else {
        m_sSelected.clear();
    }
    accept();
}
