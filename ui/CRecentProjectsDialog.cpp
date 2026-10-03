#include "CRecentProjectsDialog.h"
#include "ui/CMenuIcon.h"

#include <QContextMenuEvent>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileInfo>
#include <QFont>
#include <QGuiApplication>
#include <QHideEvent>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QRect>
#include <QScreen>
#include <QVBoxLayout>

// 2026.09.20 几何持久化：profile 读写（同 CRollbackDialog 单一写入者家族）。
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

CRecentProjectsDialog::CRecentProjectsDialog(const QStringList& a_aRecent, QWidget* a_pParent)
    : CRecentProjectsDialog(a_aRecent, QList<int>(), QString(), a_pParent) {}

CRecentProjectsDialog::CRecentProjectsDialog(const QStringList& a_aRecent,
                                             const QList<int>& a_aOpens,
                                             QWidget* a_pParent)
    : CRecentProjectsDialog(a_aRecent, a_aOpens, QString(), a_pParent) {}

CRecentProjectsDialog::CRecentProjectsDialog(const QStringList& a_aRecent,
                                             const QList<int>& a_aOpens,
                                             const QString& a_sCfgPath,
                                             QWidget* a_pParent)
    : QDialog(a_pParent), m_sCfgPath(a_sCfgPath) {
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
    // 2026.09.20 打开次数对齐（缺省/旧档 → 1；异常长表截齐）。
    m_aOpens = a_aOpens;
    while (m_aOpens.size() > m_aAll.size()) {
        m_aOpens.removeLast();
    }
    while (m_aOpens.size() < m_aAll.size()) {
        m_aOpens.append(1);
    }
    RebuildList(QString());
    RestoreGeometryFromConfig();   // 2026.09.20 几何持久化恢复（无档=缺省 620×420）

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
    // 依据过滤词重建列表：每项文本「目录名（完整路径）— N 次」（N=打开次数，
    //   2026.09.20 工程使用/修改频率统计），UserRole 存绝对路径。
    // 已被右键删除的路径不再出现（防止过滤重建时复活已删条目）。
    m_pList->clear();
    for (int i = 0; i < m_aAll.size(); ++i) {
        const QString& sPath = m_aAll.at(i);
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
        const int iOpens = (i < m_aOpens.size() && m_aOpens.at(i) > 0)
                               ? m_aOpens.at(i) : 1;
        const QString sText = QStringLiteral("%1（%2）— %3 次")
                                  .arg(sName, sPath)
                                  .arg(iOpens);
        auto* pItem = new QListWidgetItem(sText, m_pList);
        pItem->setData(Qt::UserRole, sPath);
        pItem->setToolTip(QStringLiteral("已打开 %1 次（每次成功打开工程时累加 1）")
                              .arg(iOpens));
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
        MenuIcon(QStringLiteral("ow-delete")), QStringLiteral("删除选中（%1）").arg(m_pList->selectedItems().size()));
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

// —— 2026.09.20 几何持久化（[Layout] RecentWin=x,y,w,h；口径同 CRollbackDialog：显式
//    整型串 + 屏幕可用区限幅 + profile 写缓存冲刷；空 cfg 路径=禁用，测试不写盘）——

namespace {
// 恰好 4 个整数（允许负数——多屏左侧坐标）才通过；空/个数不符/非数字返回空表。
QList<int> SplitFourInts(const QString& a_s) {
    if (a_s.isEmpty()) {
        return {};
    }
    const QStringList aParts = a_s.split(QLatin1Char(','));
    if (aParts.size() != 4) {
        return {};
    }
    QList<int> v;
    for (const QString& sP : aParts) {
        bool bOk = false;
        const int i = sP.toInt(&bOk);
        if (!bOk) {
            return {};
        }
        v.append(i);
    }
    return v;
}
} // namespace

void CRecentProjectsDialog::RestoreGeometryFromConfig() {
    if (m_sCfgPath.isEmpty()) {
        return;
    }
    wchar_t szBuf[64] = {0};
    GetPrivateProfileStringW(L"Layout", L"RecentWin", L"", szBuf, 64,
                             (LPCWSTR)m_sCfgPath.utf16());
    const QList<int> aWin = SplitFourInts(QString::fromWCharArray(szBuf));
    if (aWin.size() != 4) {
        return;   // 无档/坏档：保持构造缺省 620x420
    }
    // 限幅到虚拟桌面可用区内（宽 ≥420、高 ≥300、不超屏；坐标移入可见区——
    //   多屏/分辨率变化后不出屏外窗口；负坐标在屏内合法放行）。
    const QRect rcDesk = QGuiApplication::primaryScreen()->availableVirtualGeometry();
    const int iW = qBound(420, aWin[2], rcDesk.width());
    const int iH = qBound(300, aWin[3], rcDesk.height());
    // 2026.09.25 修「每开一次上漂一个标题栏高度」：setGeometry 改 move()+resize()。
    //   本机实测（build/probe_recentdrift4 四变体对照）：Qt6.8.3 顶层窗口 setGeometry
    //   的客户区落点恒比请求 y 高 31px（构造期/showEvent/显示后调用均同），
    //   hideEvent 把偏移值存回 → 下次继续偏、逐次上漂；move()+resize() 走精确
    //   客户区路径不漂（回归：test_execpanel recentDialogGeometryNoDrift）。
    move(QPoint(qBound(rcDesk.left(), aWin[0], rcDesk.right() - iW + 1),
                qBound(rcDesk.top(), aWin[1], rcDesk.bottom() - iH + 1)));
    resize(iW, iH);
}

void CRecentProjectsDialog::SaveGeometryToConfig() {
    if (m_sCfgPath.isEmpty()) {
        return;
    }
    const QString sVal = QStringLiteral("%1,%2,%3,%4")
                             .arg(x()).arg(y()).arg(width()).arg(height());
    WritePrivateProfileStringW(L"Layout", L"RecentWin",
                               (LPCWSTR)sVal.utf16(), (LPCWSTR)m_sCfgPath.utf16());
    // 冲刷 profile 写缓存（v1.3.78 教训：延迟落盘会让下一次打开读到空 → 「没持久化」）。
    WritePrivateProfileStringW(nullptr, nullptr, nullptr, (LPCWSTR)m_sCfgPath.utf16());
}

void CRecentProjectsDialog::hideEvent(QHideEvent* a_pEv) {
    QDialog::hideEvent(a_pEv);
    // 隐藏（含 accept/reject/done 关闭路径——QDialog::done 内部走 hide）时落盘。
    SaveGeometryToConfig();
}
