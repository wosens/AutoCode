#include "CRecentProjectsDialog.h"

#include <QContextMenuEvent>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileInfo>
#include <QFont>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QVBoxLayout>

CRecentProjectsDialog::CRecentProjectsDialog(const QStringList& a_aRecent, QWidget* a_pParent)
    : QDialog(a_pParent) {
    setWindowTitle(QStringLiteral("最近打开的工程"));
    resize(620, 420);

    // 2026.08.20 整窗 11 号字体（标题栏随系统，客户区含按钮统一）。
    QFont stFont = font();
    stFont.setPointSize(11);
    setFont(stFont);

    // 搜索框（列表最上方）：内容变化实时过滤列表（名称或路径包含即命中，不区分大小写）。
    m_pSearch = new QLineEdit(this);
    m_pSearch->setFont(stFont);
    m_pSearch->setPlaceholderText(QStringLiteral("搜索工程（名称 / 路径）…"));
    m_pSearch->setClearButtonEnabled(true);
    connect(m_pSearch, &QLineEdit::textChanged,
            this, &CRecentProjectsDialog::OnSearchChanged);

    m_pList = new QListWidget(this);
    m_pList->setFont(stFont);
    m_pList->setSelectionMode(QAbstractItemView::ExtendedSelection);   // 多选
    m_pList->setContextMenuPolicy(Qt::CustomContextMenu);             // 右键菜单
    connect(m_pList, &QListWidget::customContextMenuRequested,
            this, &CRecentProjectsDialog::OnContextMenu);

    // 全量快照入 m_aAll，列表内容统一经 RebuildList 生成（搜索过滤复用同一路径）。
    m_aAll = a_aRecent;
    RebuildList(QString());

    m_pBtns = new QDialogButtonBox(
        QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    connect(m_pBtns, &QDialogButtonBox::accepted, this, [this]() {
        // OK：优先当前项，其次第一个选中项；都无则保持空（由调用方判空）。
        ApplyPath(m_pList->currentItem());
        if (m_sSelected.isEmpty()) {
            ApplyPath(m_pList->selectedItems().isEmpty()
                          ? nullptr : m_pList->selectedItems().first());
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
    pV->addWidget(m_pSearch);
    pV->addWidget(m_pList);
    pV->addWidget(m_pBtns);
}

QString CRecentProjectsDialog::SelectedPath() const {
    return m_sSelected;
}

QStringList CRecentProjectsDialog::RemovedPaths() const {
    return m_aRemoved;
}

void CRecentProjectsDialog::ApplyPath(QListWidgetItem* a_pItem) {
    m_sSelected = (a_pItem == nullptr)
                      ? QString()
                      : a_pItem->data(Qt::UserRole).toString();
}

void CRecentProjectsDialog::RebuildList(const QString& a_sFilter) {
    // 依据过滤词重建列表：每项文本「目录名（完整路径）」，UserRole 存绝对路径。
    // 已被右键删除的路径不再出现（防止过滤重建时复活已删条目）。
    m_pList->clear();
    for (const QString& sPath : m_aAll) {
        if (m_aRemoved.contains(sPath)) {
            continue;
        }
        const QString sName = QFileInfo(sPath).fileName();
        if (!a_sFilter.isEmpty()) {
            // 大小写不敏感：名称或完整路径包含关键词即命中。
            if (!sName.contains(a_sFilter, Qt::CaseInsensitive) &&
                !sPath.contains(a_sFilter, Qt::CaseInsensitive)) {
                continue;
            }
        }
        const QString sText = QStringLiteral("%1（%2）").arg(sName, sPath);
        auto* pItem = new QListWidgetItem(sText, m_pList);
        pItem->setData(Qt::UserRole, sPath);
    }
    // 重建后确认路径可能已不在可视列表 → 取第一条兜底为当前项（不改 m_sSelected）。
    if (m_pList->count() > 0) {
        m_pList->setCurrentRow(0);
    }
}

void CRecentProjectsDialog::OnSearchChanged(const QString& a_sText) {
    RebuildList(a_sText.trimmed());
}

void CRecentProjectsDialog::OnItemDoubleClicked() {
    // 双击当前项 → 记录路径并 accept。
    ApplyPath(m_pList->currentItem());
    accept();
}

void CRecentProjectsDialog::OnContextMenu(const QPoint& a_pos) {
    // 右键：光标处若有项则并入选中集，再弹「删除选中」菜单。
    if (auto* pAt = m_pList->itemAt(a_pos)) {
        if (!pAt->isSelected()) {
            m_pList->setCurrentItem(pAt);   // 未选中的右键项 → 置为当前（单选语义）
        }
    }
    if (m_pList->selectedItems().isEmpty()) {
        return;   // 空白处右键且无选中 → 不弹菜单
    }
    QMenu stMenu(this);
    stMenu.setFont(font());
    QAction* pActDel = stMenu.addAction(
        QStringLiteral("删除选中（%1）").arg(m_pList->selectedItems().size()));
    connect(pActDel, &QAction::triggered, this, &CRecentProjectsDialog::OnDeleteSelected);
    stMenu.exec(m_pList->mapToGlobal(a_pos));
}

void CRecentProjectsDialog::OnDeleteSelected() {
    // 从列表移除并记录路径；只动列表副本，MRU 持久化由调用方经 RemovedPaths() 处理。
    const QList<QListWidgetItem*> aSel = m_pList->selectedItems();
    for (QListWidgetItem* pItem : aSel) {
        const QString sPath = pItem->data(Qt::UserRole).toString();
        if (!sPath.isEmpty() && !m_aRemoved.contains(sPath)) {
            m_aRemoved.append(sPath);
        }
        delete pItem;   // QListWidgetItem 析构自动从列表摘除
    }
    // 确认路径若刚被删除 → 清空，防止 OK 取到已删条目。
    if (!m_sSelected.isEmpty() && m_aRemoved.contains(m_sSelected)) {
        m_sSelected.clear();
    }
}
