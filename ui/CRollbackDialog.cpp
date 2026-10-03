#include "CRollbackDialog.h"
#include "ui/CMenuIcon.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QTableWidget>
#include <QHeaderView>
#include <QSplitter>
#include <QGroupBox>
#include <QDate>
#include <QHideEvent>
#include <QShowEvent>
#include <QResizeEvent>
#include <QTimer>
#include <QGuiApplication>
#include <QScreen>
#include <QMenu>
#include <QMessageBox>
#include <algorithm>
#include <windows.h>

#include "project/CProjectManager.h"
#include "CDiffViewDialog.h"

namespace {
// 2026.08.23 布局键改内联字面量（原 QSettings 常量随 QSettings 一并移除）：
// RollbackVerHdr / RollbackFileHdr / RollbackSplit / RollbackGeo，节名 Layout。

// 11pt 字号（与主程序 setPt11 同口径；对话框内逐控件设置）。
void SetPt11(QWidget* a_pW) {
    QFont f = a_pW->font();
    f.setPointSize(11);
    a_pW->setFont(f);
}

// 2026.09.21 设为基准流程消息框（预估失败/确认/截断失败/完成）统一 11pt 字体
//   （用户需求）：QMessageBox 静态接口不便于逐控件设字体，实例化后统一设置。
int BaselineMsgBox(QWidget* a_pParent, QMessageBox::Icon a_eIcon,
                   const QString& a_sText, QMessageBox::StandardButtons a_eButtons,
                   QMessageBox::StandardButton a_eDefault) {
    QMessageBox stBox(a_pParent);
    stBox.setIcon(a_eIcon);
    stBox.setWindowTitle(QStringLiteral("设为基准"));
    stBox.setText(a_sText);
    stBox.setStandardButtons(a_eButtons);
    stBox.setDefaultButton(a_eDefault);
    SetPt11(&stBox);
    return stBox.exec();
}
}   // namespace

CRollbackDialog::CRollbackDialog(const QString& a_sOwner, CProjectManager* a_pPm,
                                 QWidget* a_pParent, const QString& a_sDispName)
    : QDialog(a_pParent), m_sOwner(a_sOwner), m_sDispName(a_sDispName), m_pPm(a_pPm) {
    BuildUi();
    LoadData();
    FillVersionTable();
    RestoreLayoutState();
    if (m_pTableVersions->rowCount() > 0) {
        m_pTableVersions->selectRow(0);   // 默认选最新并联动右表
    } else {
        OnSelectionChanged();   // 空态：按钮禁用 + 占位文案
    }
}

CRollbackDialog::~CRollbackDialog() {
    if (!m_bLayoutSaved) {
        SaveLayoutState();
    }
}

void CRollbackDialog::BuildUi() {
    // 2026.08.22 标题优先显示任务名（旧版直接显示 owner 十六进制 id，用户看不懂）
    const QString sDisp = !m_sDispName.isEmpty() ? m_sDispName
        : (m_sOwner == QStringLiteral("global") ? QStringLiteral("工程助手")
                                                : m_sOwner);
    setWindowTitle(QStringLiteral("代码回退 — %1").arg(sDisp));
    resize(940, 560);

    auto* pV = new QVBoxLayout(this);

    m_pLblSummary = new QLabel(this);
    m_pLblSummary->setStyleSheet(QStringLiteral("color:#6b7280;"));
    SetPt11(m_pLblSummary);
    // 2026.08.22 文案不驱动布局：换文字（记录点数/时间范围变化）不改变窗体最小宽度
    m_pLblSummary->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
    m_pLblSummary->setMinimumWidth(0);
    pV->addWidget(m_pLblSummary);

    m_pSplit = new QSplitter(Qt::Horizontal, this);
    SetPt11(m_pSplit);

    // —— 左：记录点列表（7 列）——
    m_pTableVersions = new QTableWidget(this);
    m_pTableVersions->setColumnCount(7);
    m_pTableVersions->setHorizontalHeaderLabels({
        QStringLiteral("#"), QStringLiteral("时间"), QStringLiteral("类型"),
        QStringLiteral("名称摘要"), QStringLiteral("改"), QStringLiteral("删"),
        QStringLiteral("增")});
    m_pTableVersions->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_pTableVersions->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_pTableVersions->setSelectionMode(QAbstractItemView::SingleSelection);
    m_pTableVersions->verticalHeader()->setVisible(false);
    m_pTableVersions->setAlternatingRowColors(true);
    m_pTableVersions->setSortingEnabled(false);
    auto* pHdrV = m_pTableVersions->horizontalHeader();
    pHdrV->setSectionsMovable(false);
    pHdrV->setSectionResizeMode(QHeaderView::Interactive);
    pHdrV->setMinimumSectionSize(24);
    QList<int> aWidthsV = {54, 118, 72, 300, 40, 40, 40};   // 2026.09.21 #列 40→54：⚓ 基准角标
    for (int i = 0; i < aWidthsV.size(); ++i) {
        m_pTableVersions->setColumnWidth(i, aWidthsV[i]);
    }
    SetPt11(m_pTableVersions);
    m_pSplit->addWidget(m_pTableVersions);

    // —— 右：变动明细（3 列）——
    auto* pRight = new QGroupBox(QStringLiteral("选中记录点的文件变化"), this);
    SetPt11(pRight);
    auto* pRV = new QVBoxLayout(pRight);
    m_pLblFilesCaption = new QLabel(pRight);
    SetPt11(m_pLblFilesCaption);
    // 2026.08.22 文案不驱动布局：选中记录点后说明文字变长/变短（含长 diff 摘要）
    // 不允许撑大 GroupBox/分割条（QSplitter 会按子部件 sizeHint 重新分配）
    m_pLblFilesCaption->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
    m_pLblFilesCaption->setMinimumWidth(0);
    pRV->addWidget(m_pLblFilesCaption);
    m_pTableFiles = new QTableWidget(pRight);
    m_pTableFiles->setColumnCount(3);
    m_pTableFiles->setHorizontalHeaderLabels({
        QStringLiteral("文件路径"), QStringLiteral("变化"), QStringLiteral("大小")});
    m_pTableFiles->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_pTableFiles->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_pTableFiles->setSelectionMode(QAbstractItemView::SingleSelection);
    m_pTableFiles->verticalHeader()->setVisible(false);
    m_pTableFiles->setAlternatingRowColors(true);
    m_pTableFiles->setSortingEnabled(false);
    auto* pHdrF = m_pTableFiles->horizontalHeader();
    pHdrF->setSectionsMovable(false);
    pHdrF->setSectionResizeMode(QHeaderView::Interactive);
    m_pTableFiles->setColumnWidth(0, 420);
    m_pTableFiles->setColumnWidth(1, 56);
    m_pTableFiles->setColumnWidth(2, 120);
    SetPt11(m_pTableFiles);
    pRV->addWidget(m_pTableFiles, 1);
    m_pSplit->addWidget(pRight);
    m_pSplit->setStretchFactor(0, 3);
    m_pSplit->setStretchFactor(1, 2);
    pV->addWidget(m_pSplit, 1);

    // —— 底部按钮行 ——
    auto* pH = new QHBoxLayout();
    m_pBtnRestore = new QPushButton(QStringLiteral("回退到此记录点"), this);
    SetPt11(m_pBtnRestore);
    m_pBtnRestore->setEnabled(false);
    connect(m_pBtnRestore, &QPushButton::clicked, this, &QDialog::accept);
    pH->addWidget(m_pBtnRestore);
    auto* pBtnClose = new QPushButton(QStringLiteral("关闭"), this);
    SetPt11(pBtnClose);
    connect(pBtnClose, &QPushButton::clicked, this, &QDialog::reject);
    pH->addWidget(pBtnClose);
    pH->addStretch(1);
    pV->addLayout(pH);

    connect(m_pTableVersions, &QTableWidget::itemSelectionChanged,
            this, &CRollbackDialog::OnSelectionChanged);
    // 2026.09.21 版本列表右键：「设为基准（删除更早的记录）」——历史太多时选定锚点
    //   截断释放（manifest 全量自包含，删旧版不伤后版；引用版先物化）。
    m_pTableVersions->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_pTableVersions, &QTableWidget::customContextMenuRequested,
            this, &CRollbackDialog::OnVersionsContextMenu);
    // 2026.08.28 取消「双击记录点直接回退」：回退唯一入口=底部「回退到此记录点」按钮，
    //   双击误触直接执行回退风险高（原实现双击即 accept()）。文件列表双击=查看变更，保留。
    // 2026.08.22 用户手拖分割条时更新冻结值（否则下次点选会弹回旧位置）
    connect(m_pSplit, &QSplitter::splitterMoved, this, [this]() {
        m_aSplitSizes = m_pSplit->sizes();
    });

    // 2026.08.24 文件列表右键「查看变更」（Beyond Compare 风格双栏对比）；双击同效。
    m_pTableFiles->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_pTableFiles, &QTableWidget::customContextMenuRequested,
            this, &CRollbackDialog::OnFilesContextMenu);
    connect(m_pTableFiles, &QTableWidget::cellDoubleClicked,
            this, &CRollbackDialog::OnFilesCellDoubleClicked);
}

// 2026.08.24 文件列表右键菜单：对选中行提供「查看变更」。
void CRollbackDialog::OnFilesContextMenu(const QPoint& a_pos) {
    const int iRow = m_pTableFiles->rowAt(a_pos.y());
    if (iRow < 0) {
        return;   // 空白处不出菜单
    }
    m_pTableFiles->selectRow(iRow);
    QMenu stMenu(this);
    QAction* pAct = stMenu.addAction(MenuIcon(QStringLiteral("ow-diff")), QStringLiteral("查看变更"));
    if (stMenu.exec(m_pTableFiles->viewport()->mapToGlobal(a_pos)) == pAct) {
        ShowFileDiff(iRow);
    }
}

void CRollbackDialog::OnFilesCellDoubleClicked(int a_iRow, int /*a_iCol*/) {
    ShowFileDiff(a_iRow);   // 双击文件行 = 查看变更（与右键菜单一致）
}

// 2026.09.21 版本列表右键菜单：「设为基准（删除更早的记录）」。
void CRollbackDialog::OnVersionsContextMenu(const QPoint& a_pos) {
    const int iRow = m_pTableVersions->rowAt(a_pos.y());
    if (iRow < 0 || iRow >= m_aVersions.size()) {
        return;   // 空白处不出菜单
    }
    m_pTableVersions->selectRow(iRow);
    const SBackupVersion& st = m_aVersions[iRow];
    QMenu stMenu(this);
    SetPt11(&stMenu);   // 2026.09.21 右键菜单字体 11pt（与对话框控件同口径）
    QAction* pAct = stMenu.addAction(MenuIcon(QStringLiteral("ow-remove")),
                                     QStringLiteral("设为基准（删除更早的记录）…"));
    // 旧全量快照不在统一库（无 manifest，截断语义不适用）；是否真有更早记录由
    //   dry-run 全库判定（可见列表可能只是 owner 过滤后的子集）。
    pAct->setEnabled(!st.bLegacy);
    if (stMenu.exec(m_pTableVersions->viewport()->mapToGlobal(a_pos)) == pAct) {
        OnSetBaseline();
    }
}

// 2026.09.21 设为基准：确认框（dry-run 预估）→ 截断（引用版先物化+GC）→ 刷新列表。
void CRollbackDialog::OnSetBaseline() {
    if (m_pPm == nullptr || m_pSelected == nullptr || m_pSelected->bLegacy) {
        return;
    }
    const int iBase = m_pSelected->iSeq;
    // 预估（不动盘）。
    int iDel = 0;
    int iDelBlobs = 0;
    qint64 iBytes = 0;
    QString sErr;
    if (!m_pPm->TruncateUnifiedBefore(iBase, /*a_bDryRun=*/true, iDel, iDelBlobs, iBytes, sErr)) {
        BaselineMsgBox(this, QMessageBox::Information, sErr,
                       QMessageBox::Ok, QMessageBox::Ok);
        return;
    }
    // 2026.09.28 已是最早可回退点：无可删内容，不再弹出「永久删除 0 条」的误导性确认框。
    if (iDel <= 0 && iDelBlobs <= 0) {
        BaselineMsgBox(this, QMessageBox::Information,
                       QStringLiteral("该记录点已是最早可回退点，无需设为基准。"),
                       QMessageBox::Ok, QMessageBox::Ok);
        return;
    }
    // 涉及范围（全局序号：跨任务/助手的更早记录一并删除——确认框明示）。
    const QList<SBackupVersion> aAll = m_pPm->ListUnifiedBackups(QString());
    QMap<QString, int> mByOwner;
    int iMin = 0;
    int iMax = 0;
    for (const SBackupVersion& v : aAll) {
        if (v.bLegacy || v.iSeq >= iBase) {
            continue;
        }
        const QString sKey = (v.sOwner == QStringLiteral("global"))
            ? QStringLiteral("工程助手") : v.sOwner;
        mByOwner[sKey] = mByOwner.value(sKey) + 1;
        if (iMin == 0 || v.iSeq < iMin) {
            iMin = v.iSeq;
        }
        if (v.iSeq > iMax) {
            iMax = v.iSeq;
        }
    }
    QStringList aOwnerParts;
    for (auto it = mByOwner.constBegin(); it != mByOwner.constEnd(); ++it) {
        aOwnerParts << QStringLiteral("%1 ×%2").arg(it.key()).arg(it.value());
    }
    const QString sMsg = QStringLiteral(
        "将把记录点 #%1 设为基准，并永久删除更早的 %2 条记录：\n\n"
        "· 范围：#%3 ~ #%4（按全局序号，其他任务/助手中更早的记录一并删除）\n"
        "· 涉及：%5\n"
        "· 预计删除：%6 个记录文件 + %7 个内容块，释放 %8 MB\n\n"
        "⚠ 删除后无法回退到这些时间点（不可撤销）。\n"
        "基准 #%1 及其后的所有记录点不受影响，仍可正常回退——增量备份的每份记录\n"
        "都是完整清单，删早不改伤后续记录的还原。")
        .arg(iBase).arg(iDel).arg(iMin).arg(iMax)
        .arg(aOwnerParts.isEmpty() ? QStringLiteral("—") : aOwnerParts.join(QStringLiteral("、")))
        .arg(iDel)
        .arg(iDelBlobs)
        .arg(iBytes / 1048576.0, 0, 'f', 1);
    if (BaselineMsgBox(this, QMessageBox::Warning, sMsg,
                       QMessageBox::Ok | QMessageBox::Cancel,
                       QMessageBox::Cancel) != QMessageBox::Ok) {
        return;
    }
    // 执行截断。
    iDel = 0;
    iDelBlobs = 0;
    iBytes = 0;
    if (!m_pPm->TruncateUnifiedBefore(iBase, /*a_bDryRun=*/false, iDel, iDelBlobs, iBytes, sErr)) {
        BaselineMsgBox(this, QMessageBox::Warning,
                       QStringLiteral("截断失败：%1").arg(sErr),
                       QMessageBox::Ok, QMessageBox::Ok);
        return;
    }
    // 刷新：重建列表并选中原基准行（m_pSelected 随 m_aVersions 重建，先置空防悬垂）。
    m_pSelected = nullptr;
    m_aDiffBySeq.clear();
    LoadData();
    FillVersionTable();
    int iSelRow = -1;
    for (int i = 0; i < m_aVersions.size(); ++i) {
        if (!m_aVersions[i].bLegacy && m_aVersions[i].iSeq == iBase) {
            iSelRow = i;
            break;
        }
    }
    m_pTableVersions->selectRow(iSelRow >= 0 ? iSelRow : 0);
    // 2026.09.28 无条件重算选中：行集未变（截断 0 条/删除行均在选中行下方）时
    //   selectRow 重选同一选中集不触发 itemSelectionChanged，m_pSelected 停留 nullptr，
    //   表现为行仍显示选中、按钮可用，但 accept 后回退被调用方静默忽略。
    OnSelectionChanged();
    if (iSelRow < 0 && m_pTableVersions->rowCount() == 0) {
        // 空态：按钮禁用 + 占位文案（OnSelectionChanged 幂等，重复调用无害）
    }
    BaselineMsgBox(this, QMessageBox::Information,
        QStringLiteral("已删除 %1 条更早记录：\n"
                        "· 删除文件：%2 个记录文件 + %3 个内容块（共 %4 个）\n"
                        "· 释放空间：%5 MB\n\n"
                        "当前最早可回退点：#%6（列表 ⚓ 标记）")
            .arg(iDel).arg(iDel).arg(iDelBlobs).arg(iDel + iDelBlobs)
            .arg(iBytes / 1048576.0, 0, 'f', 1).arg(iBase),
        QMessageBox::Ok, QMessageBox::Ok);
}

void CRollbackDialog::ShowFileDiff(int a_iRow) {
    if (m_pPm == nullptr || a_iRow < 0 || a_iRow >= m_aShownEntries.size()) {
        return;
    }
    const SDiffEntry& stE = m_aShownEntries[a_iRow];
    const int iRowVer = m_pTableVersions->currentRow();
    if (iRowVer < 0 || iRowVer >= m_aVersions.size()) {
        return;
    }
    const SBackupVersion& stNew = m_aVersions[iRowVer];
    const int iSeqOld = (iRowVer + 1 < m_aVersions.size())
        ? m_aVersions[iRowVer + 1].iSeq : -1;
    // 版本侧标签（序号 + 时间）。
    auto fnTime = [](const QString& a_s) {
        QString s = a_s;
        s.replace(QLatin1Char('T'), QLatin1Char(' '));
        return s.left(16);
    };
    const QString sLblNew = QStringLiteral("#%1 · %2")
        .arg(stNew.iSeq).arg(fnTime(stNew.sTime));
    const QString sLblOld = iSeqOld > 0
        ? QStringLiteral("#%1 · %2").arg(iSeqOld)
            .arg(fnTime(m_aVersions[iRowVer + 1].sTime))
        : QStringLiteral("（无上一记录点）");

    // 读两侧内容：新增→旧侧缺失；删除→新侧缺失；修改→两侧都读（读失败弹错提示）。
    QByteArray bufOld, bufNew;
    QString sErr;
    bool bOldMissing = (stE.sChange == QStringLiteral("add"));
    bool bNewMissing = (stE.sChange == QStringLiteral("del"));
    if (!bOldMissing) {
        if (!m_pPm->ReadUnifiedFile(iSeqOld, stE.sPath, bufOld, sErr)) {
            QMessageBox::warning(this, QStringLiteral("查看变更"),
                QStringLiteral("读取上一记录点 #%1 中该文件失败：\n%2").arg(iSeqOld).arg(sErr));
            return;
        }
    }
    if (!bNewMissing) {
        if (!m_pPm->ReadUnifiedFile(stNew.iSeq, stE.sPath, bufNew, sErr)) {
            QMessageBox::warning(this, QStringLiteral("查看变更"),
                QStringLiteral("读取记录点 #%1 中该文件失败：\n%2").arg(stNew.iSeq).arg(sErr));
            return;
        }
    }
    CDiffViewDialog stDlg(stE.sPath, bufOld, bufNew, bOldMissing, bNewMissing,
                          sLblOld, sLblNew, this,
                          m_pPm != nullptr ? m_pPm->ConfigFilePath() : QString());
    stDlg.exec();
}

void CRollbackDialog::LoadData() {
    if (m_pPm == nullptr) {
        return;
    }
    m_aVersions = m_pPm->ListUnifiedBackups(m_sOwner);
    // 逐相邻对 diff 缓存（§5.7）：i 与 i+1（更新的一条）都是 CAS 版本才算。
    for (int i = 0; i + 1 < m_aVersions.size(); ++i) {
        const SBackupVersion& stNew = m_aVersions[i];
        const SBackupVersion& stOld = m_aVersions[i + 1];
        if (stNew.bLegacy || stOld.bLegacy) {
            continue;
        }
        SDiffResult st;
        if (m_pPm->DiffVersions(stOld.iSeq, stNew.iSeq, st)) {
            m_aDiffBySeq.insert(stNew.iSeq, st);
        }
    }
}

QString CRollbackDialog::KindLabel(const SBackupVersion& a_st) {
    if (a_st.sKind == QStringLiteral("stage")) {
        return a_st.sStage.isEmpty() ? QStringLiteral("阶段") : a_st.sStage;
    }
    if (a_st.sKind == QStringLiteral("turn"))        return QStringLiteral("助手轮");
    if (a_st.sKind == QStringLiteral("rollback-pre")) return QStringLiteral("回退前快照");
    if (a_st.bLegacy || a_st.sKind == QStringLiteral("legacy")) return QStringLiteral("旧全量");
    return a_st.sKind;
}

QString CRollbackDialog::DisplayTitle(const SBackupVersion& a_st) {
    const QString sKind = a_st.bLegacy ? QStringLiteral("legacy") : a_st.sKind;
    if (sKind == QStringLiteral("stage") || sKind == QStringLiteral("legacy")) {
        if (!a_st.sSummary.trimmed().isEmpty()) {
            return a_st.sSummary.left(40) + (a_st.sSummary.size() > 40
                ? QStringLiteral("…") : QString());
        }
        return sKind == QStringLiteral("stage")
            ? QStringLiteral("%1前快照").arg(a_st.sStage)
            : QStringLiteral("旧版全量快照");
    }
    if (sKind == QStringLiteral("turn")) {
        const QString sP = a_st.sPrompt.trimmed().replace(QLatin1Char('\n'), QLatin1Char(' '));
        if (sP.isEmpty()) {
            return QStringLiteral("（无提问存量）");
        }
        return sP.left(40) + (sP.size() > 40 ? QStringLiteral("…") : QString());
    }
    if (sKind == QStringLiteral("rollback-pre")) {
        return QStringLiteral("回退到 #%1 前的现场").arg(a_st.iTarget);
    }
    return QString();
}

QString CRollbackDialog::FormatSize(qint64 a_iBytes) {
    if (a_iBytes < 1024) {
        return QString::number(a_iBytes) + QStringLiteral(" B");
    }
    if (a_iBytes < 1024 * 1024) {
        return QStringLiteral("%1 KB").arg(a_iBytes / 1024.0, 0, 'f', 1);
    }
    return QStringLiteral("%1 MB").arg(a_iBytes / 1048576.0, 0, 'f', 1);
}

void CRollbackDialog::FillVersionTable() {
    // 2026.09.21 基准角标：可见范围内最老的增量记录点（用户截断后即所选基准；
    //   未截断时也是事实上能退到的最早点）。seq 列加 ⚓ 前缀，列宽随之微增。
    int iBaseRow = -1;
    for (int i = m_aVersions.size() - 1; i >= 0; --i) {
        if (!m_aVersions[i].bLegacy) {
            iBaseRow = i;
            break;
        }
    }
    m_pTableVersions->setRowCount(m_aVersions.size());
    for (int i = 0; i < m_aVersions.size(); ++i) {
        const SBackupVersion& st = m_aVersions[i];
        const bool bHasDiff = m_aDiffBySeq.contains(st.iSeq) && !st.bLegacy;
        auto fnItem = [](const QString& a_sText) {
            auto* pItem = new QTableWidgetItem(a_sText);
            pItem->setFlags(pItem->flags() & ~Qt::ItemIsEditable);
            return pItem;
        };
        // #（UserRole 存行索引）。
        auto* pItemSeq = fnItem((i == iBaseRow ? QStringLiteral("⚓") : QString())
                                + QString::number(st.iSeq));
        pItemSeq->setData(Qt::UserRole, i);
        pItemSeq->setToolTip((i == iBaseRow
            ? QStringLiteral("基准（最老可回退点）· ") : QString())
            + QStringLiteral("%1 · %2")
                .arg(st.sTime, st.bLegacy ? QStringLiteral("旧版全量快照（FR-6）")
                                          : st.sKind));
        m_pTableVersions->setItem(i, 0, pItemSeq);
        // 时间（当年省年份）。
        QString sTime = st.sTime;
        sTime.replace(QLatin1Char('T'), QLatin1Char(' '));
        const QDate dToday = QDate::currentDate();
        if (sTime.size() >= 19
            && sTime.left(4).toInt() == dToday.year()) {
            sTime = sTime.mid(5, 11);
        }
        auto* pItemTime = fnItem(sTime);
        pItemTime->setToolTip(st.sTime);
        m_pTableVersions->setItem(i, 1, pItemTime);
        // 类型。
        auto* pItemKind = fnItem(KindLabel(st));
        pItemKind->setToolTip(st.bLegacy
            ? QStringLiteral("旧版全量快照（FR-6）")
            : (st.sKind == QStringLiteral("turn")
               ? QStringLiteral("轮次前快照：每轮非只读投递前捕获")
               : (st.sKind == QStringLiteral("rollback-pre")
                  ? QStringLiteral("回退前现场保护快照：执行回退前自动留底")
                  : QStringLiteral("阶段前快照：该阶段启动前捕获"))));
        m_pTableVersions->setItem(i, 2, pItemKind);
        // 名称摘要（Tooltip 全文）。
        QString sFull;
        if (st.bLegacy || st.sKind == QStringLiteral("stage")) {
            sFull = st.sSummary;
        } else if (st.sKind == QStringLiteral("turn")) {
            sFull = st.sPrompt;
        }
        auto* pItemTitle = fnItem(DisplayTitle(st));
        if (!sFull.isEmpty()) {
            pItemTitle->setToolTip(sFull);
        }
        m_pTableVersions->setItem(i, 3, pItemTitle);
        // 改/删/增（无对比 → "—"；最老一条的"增"= manifest 文件总数）。
        const QString sDash = QStringLiteral("—");
        QString sMod = sDash, sDel = sDash, sAdd = sDash;
        if (bHasDiff) {
            const SDiffResult& stDiff = m_aDiffBySeq.value(st.iSeq);
            sMod = QString::number(stDiff.iChanged);
            sDel = QString::number(stDiff.iRemoved);
            sAdd = QString::number(stDiff.iAdded);
        } else if (!st.bLegacy && i + 1 >= m_aVersions.size()) {
            sAdd = QString::number(st.iFileCount);   // 基线
        }
        const int iPrevSeq = (i + 1 < m_aVersions.size()) ? m_aVersions[i + 1].iSeq : -1;
        const QString sStatTip = bHasDiff
            ? QStringLiteral("修改 %1 · 删除 %2 · 新增 %3（相对上一记录点 #%4）")
                .arg(sMod, sDel, sAdd).arg(iPrevSeq)
            : QStringLiteral("旧格式/基线，无对比数据");
        auto* pItemM = fnItem(sMod);  pItemM->setToolTip(sStatTip);
        auto* pItemD = fnItem(sDel);  pItemD->setToolTip(sStatTip);
        auto* pItemA = fnItem(sAdd);  pItemA->setToolTip(sStatTip);
        pItemM->setTextAlignment(Qt::AlignCenter);
        pItemD->setTextAlignment(Qt::AlignCenter);
        pItemA->setTextAlignment(Qt::AlignCenter);
        m_pTableVersions->setItem(i, 4, pItemM);
        m_pTableVersions->setItem(i, 5, pItemD);
        m_pTableVersions->setItem(i, 6, pItemA);
    }
    // 顶部说明。
    if (m_aVersions.isEmpty()) {
        m_pLblSummary->setText(
            QStringLiteral("暂无记录点（改码阶段/非只读助手轮会自动产生）"));
    } else {
        QString sTimeNew = m_aVersions.first().sTime;
        QString sTimeOld = m_aVersions.last().sTime;
        sTimeNew.replace(QLatin1Char('T'), QLatin1Char(' '));
        sTimeOld.replace(QLatin1Char('T'), QLatin1Char(' '));
        m_pLblSummary->setText(QStringLiteral("共 %1 个记录点 · 最早 %2 · 最新 %3")
            .arg(m_aVersions.size())
            .arg(sTimeOld.left(16), sTimeNew.left(16)));
    }
}

void CRollbackDialog::OnSelectionChanged() {
    const int iRow = m_pTableVersions->currentRow();
    m_pSelected = (iRow >= 0 && iRow < m_aVersions.size()) ? &m_aVersions[iRow] : nullptr;
    m_pBtnRestore->setEnabled(m_pSelected != nullptr);
    FillFilesTable(iRow);
    // 2026.08.22 右表填充/清空会改变子部件 sizeHint，QSplitter 可能把分割条
    // 挪到新分配——强制回贴冻结值，点击记录点不动窗体布局。
    if (!m_aSplitSizes.isEmpty()) {
        m_pSplit->setSizes(m_aSplitSizes);
    }
}

void CRollbackDialog::FillFilesTable(int a_iRow) {
    m_pTableFiles->setRowCount(0);
    m_aShownEntries.clear();   // 2026.08.24 先清（legacy/无 diff 路径不留旧条目）
    if (a_iRow < 0 || a_iRow >= m_aVersions.size()) {
        m_pLblFilesCaption->setText(
            m_aVersions.isEmpty()
                ? QStringLiteral("暂无记录点")
                : QStringLiteral("未选中记录点"));
        return;
    }
    const SBackupVersion& st = m_aVersions[a_iRow];
    if (st.bLegacy) {
        m_pLblFilesCaption->setText(
            QStringLiteral("旧全量快照：无增量明细（可直接回退）"));
        return;
    }
    if (!m_aDiffBySeq.contains(st.iSeq)) {
        const bool bBaseline = (a_iRow + 1 >= m_aVersions.size())
            || m_aVersions[a_iRow + 1].bLegacy;
        m_pLblFilesCaption->setText(bBaseline
            ? QStringLiteral("基线记录点：无上一记录点可对比")
            : QStringLiteral("旧格式上一记录点，无对比数据"));
        return;
    }
    const SDiffResult& stDiff = m_aDiffBySeq.value(st.iSeq);
    const int iPrevSeq = (a_iRow + 1 < m_aVersions.size())
        ? m_aVersions[a_iRow + 1].iSeq : -1;
    m_pLblFilesCaption->setText(QStringLiteral("#%1 相对 #%2 的变化 —— 改 %3 · 删 %4 · 增 %5"
        "（含两记录点之间的一切磁盘改动）")
        .arg(st.iSeq).arg(iPrevSeq)
        .arg(stDiff.iChanged).arg(stDiff.iRemoved).arg(stDiff.iAdded));
    // 排序：变化类型（修改→新增→删除）再路径字典序。
    QList<SDiffEntry> aEntries = stDiff.aEntries;
    auto fnRank = [](const QString& s) {
        if (s == QStringLiteral("mod")) { return 0; }
        if (s == QStringLiteral("add")) { return 1; }
        return 2;
    };
    std::sort(aEntries.begin(), aEntries.end(),
              [&fnRank](const SDiffEntry& a, const SDiffEntry& b) {
                  const int iRa = fnRank(a.sChange), iRb = fnRank(b.sChange);
                  if (iRa != iRb) { return iRa < iRb; }
                  return a.sPath < b.sPath;
              });
    m_aShownEntries = aEntries;   // 2026.08.24 右键「查看变更」行号对齐数据源
    m_pTableFiles->setRowCount(aEntries.size());
    for (int i = 0; i < aEntries.size(); ++i) {
        const SDiffEntry& stE = aEntries[i];
        auto* pItemPath = new QTableWidgetItem(stE.sPath);
        pItemPath->setFlags(pItemPath->flags() & ~Qt::ItemIsEditable);
        pItemPath->setToolTip(stE.sPath);
        const QString sChange = stE.sChange == QStringLiteral("mod")
            ? QStringLiteral("修改")
            : (stE.sChange == QStringLiteral("add") ? QStringLiteral("新增")
                                                    : QStringLiteral("删除"));
        auto* pItemChg = new QTableWidgetItem(sChange);
        pItemChg->setFlags(pItemChg->flags() & ~Qt::ItemIsEditable);
        pItemChg->setTextAlignment(Qt::AlignCenter);
        const QString sSize = stE.sChange == QStringLiteral("add")
            ? QStringLiteral("— → %1").arg(FormatSize(stE.iSizeNew))
            : (stE.sChange == QStringLiteral("del")
                ? QStringLiteral("%1 → —").arg(FormatSize(stE.iSizeOld))
                : QStringLiteral("%1 → %2").arg(FormatSize(stE.iSizeOld),
                                                FormatSize(stE.iSizeNew)));
        auto* pItemSize = new QTableWidgetItem(sSize);
        pItemSize->setFlags(pItemSize->flags() & ~Qt::ItemIsEditable);
        m_pTableFiles->setItem(i, 0, pItemPath);
        m_pTableFiles->setItem(i, 1, pItemChg);
        m_pTableFiles->setItem(i, 2, pItemSize);
    }
}

void CRollbackDialog::hideEvent(QHideEvent* a_pEvent) {
    if (!m_bLayoutSaved) {
        SaveLayoutState();
        m_bLayoutSaved = true;   // 2026.08.22 防析构二次写盘（同内容重复 IO）
    }
    QDialog::hideEvent(a_pEvent);
}

void CRollbackDialog::showEvent(QShowEvent* a_pEvent) {
    QDialog::showEvent(a_pEvent);
    // 2026.08.28 首次显示后布局才真正激活：把冻结值刷成真实分配（构造期
    //   RestoreLayoutState 里 sizes() 拿到的仍是未激活的假值/恢复原始值，
    //   与实际呈现可能有出入；不刷的话首次点选记录点会弹回旧位置）。
    QTimer::singleShot(0, this, [this]() {
        m_aSplitSizes = m_pSplit->sizes();
    });
}

void CRollbackDialog::resizeEvent(QResizeEvent* a_pEvent) {
    QDialog::resizeEvent(a_pEvent);
    // 2026.08.28 窗口 resize 后分割条按比例重分配：同步刷新冻结值，否则
    //   用户拉大窗口后点选记录点会弹回旧尺寸（布局事件后取真值，延迟 0ms）。
    QTimer::singleShot(0, this, [this]() {
        m_aSplitSizes = m_pSplit->sizes();
    });
}

// 2026.08.23 布局存取改用 Win32 profile API（与 CProjectManager 其余 config.ini IO 同口径）。
// 旧实现用 QSettings(IniFormat)：Qt6 按 UTF-8 读写并整文件重写，会把 profile API 写下的
// ANSI(GBK) 中文值（如 [Recent] 最近打开的中文工程路径）按 UTF-8 误读成 U+FFFD 再落盘，
// 永久性打成乱码（“最近打开里面的中文全部乱码”根因）。数值串全 ASCII，两侧等价安全。
static void WriteProfileString(const QString& a_sFile, const wchar_t* a_pwsSect,
                               const wchar_t* a_pwsKey, const QString& a_sVal) {
    WritePrivateProfileStringW(a_pwsSect, a_pwsKey, a_sVal.toStdWString().c_str(),
                               a_sFile.toStdWString().c_str());
}

static QString ReadProfileString(const QString& a_sFile, const wchar_t* a_pwsSect,
                                 const wchar_t* a_pwsKey) {
    wchar_t szBuf[8192] = {0};   // 列宽/几何整型串最长数百字符，8K 富余
    const DWORD dw = GetPrivateProfileStringW(a_pwsSect, a_pwsKey, L"", szBuf, 8192,
                                              a_sFile.toStdWString().c_str());
    return QString::fromWCharArray(szBuf, static_cast<int>(dw));
}

// 2026.08.28 显式数值持久化辅助：严格整数表序列化/反序列化。
//   旧版存 opaque base64（QHeaderView/QSplitter saveState）无任何校验——现场
//   实测坏档静默生效（副本 config 曾存入 71/170 的未激活 sizeHint 假值，恢复后
//   分割条不可用），且人不可读无法核对。改为可读整型串 + 解析/范围校验，坏档自弃。
static QString JoinInts(const QList<int>& a_v) {
    QString s;
    for (const int i : a_v) {
        if (!s.isEmpty()) {
            s += QLatin1Char(',');
        }
        s += QString::number(i);
    }
    return s;
}

static QList<int> SplitInts(const QString& a_s, int a_iExpect) {
    // 恰好 a_iExpect 个整数（允许负数——多屏左侧坐标 x 为负）才通过；
    //   空串/个数不符/任何非数字字符（旧 base64 档混入同名键）返回空表。
    //   用途层各自做范围校验（窗口几何限屏幕内、分割条两侧 ≥60、列宽限幅）。
    if (a_s.isEmpty()) {
        return {};
    }
    const QStringList aParts = a_s.split(QLatin1Char(','));
    if (aParts.size() != a_iExpect) {
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

void CRollbackDialog::SaveLayoutState() {
    if (m_pPm == nullptr) {
        return;
    }
    const QString sCfg = m_pPm->ConfigFilePath();
    if (sCfg.isEmpty()) {
        return;
    }
    const wchar_t* pwsSect = L"Layout";
    // 2026.08.28 显式数值持久化：窗口几何/分割条/两表列宽全部存整型串，
    //   RestoreLayoutState 严格校验；旧四个 base64 键顺带清除防复活。
    WriteProfileString(sCfg, pwsSect, L"RollbackWin",
        JoinInts({ x(), y(), width(), height() }));
    WriteProfileString(sCfg, pwsSect, L"RollbackPane", JoinInts(m_pSplit->sizes()));
    QList<int> aColsV;
    for (int i = 0; i < m_pTableVersions->columnCount(); ++i) {
        aColsV.append(m_pTableVersions->columnWidth(i));
    }
    WriteProfileString(sCfg, pwsSect, L"RollbackColsV", JoinInts(aColsV));
    QList<int> aColsF;
    for (int i = 0; i < m_pTableFiles->columnCount(); ++i) {
        aColsF.append(m_pTableFiles->columnWidth(i));
    }
    WriteProfileString(sCfg, pwsSect, L"RollbackColsF", JoinInts(aColsF));
    ::WritePrivateProfileStringW(pwsSect, L"RollbackVerHdr", nullptr,
                                 (LPCWSTR)sCfg.utf16());
    ::WritePrivateProfileStringW(pwsSect, L"RollbackFileHdr", nullptr,
                                 (LPCWSTR)sCfg.utf16());
    ::WritePrivateProfileStringW(pwsSect, L"RollbackSplit", nullptr,
                                 (LPCWSTR)sCfg.utf16());
    ::WritePrivateProfileStringW(pwsSect, L"RollbackGeo", nullptr,
                                 (LPCWSTR)sCfg.utf16());
    // 2026.08.28 冲刷 profile 写缓存（关键）：WritePrivateProfileStringW 的写入
    //   先进系统缓存、延迟落盘——实测同一进程紧接着读回都不完整（RollbackWin
    //   读到而 RollbackPane 丢，Qt 直读磁盘文件为空串），外部进程/热重载/下次
    //   打开窗口更读到空 → 「布局没持久化」的直接根因。全 NULL + 文件名是该
    //   API 的官方冲刷用法，立即把缓存写穿到磁盘。
    ::WritePrivateProfileStringW(nullptr, nullptr, nullptr, (LPCWSTR)sCfg.utf16());
}

void CRollbackDialog::RestoreLayoutState() {
    if (m_pPm == nullptr) {
        return;
    }
    const QString sCfg = m_pPm->ConfigFilePath();
    if (sCfg.isEmpty()) {
        return;
    }
    const wchar_t* pwsSect = L"Layout";
    // 列宽：个数与列数相符且逐列限幅 [24, 2000] 才应用（防坏档拉出 0 宽/巨宽）。
    const QList<int> aColsV = SplitInts(
        ReadProfileString(sCfg, pwsSect, L"RollbackColsV"),
        m_pTableVersions->columnCount());
    for (int i = 0; i < aColsV.size(); ++i) {
        // 2026.09.21 #列（0）下限抬到 54：旧存档 40 放不下 ⚓ 基准角标前缀。
        const int iMin = (i == 0) ? 54 : 24;
        m_pTableVersions->setColumnWidth(i, qBound(iMin, aColsV[i], 2000));
    }
    const QList<int> aColsF = SplitInts(
        ReadProfileString(sCfg, pwsSect, L"RollbackColsF"),
        m_pTableFiles->columnCount());
    for (int i = 0; i < aColsF.size(); ++i) {
        m_pTableFiles->setColumnWidth(i, qBound(24, aColsF[i], 2000));
    }
    // 分割条：两侧各 ≥60px 才应用（现场坏档 71/170 即未激活假值，两值过小
    //   直接弃用默认分配，不让坏档把窗体挤成一条）。
    const QList<int> aPane = SplitInts(
        ReadProfileString(sCfg, pwsSect, L"RollbackPane"), 2);
    if (aPane.size() == 2 && aPane[0] >= 60 && aPane[1] >= 60) {
        m_pSplit->setSizes(aPane);
        // 2026.08.28 关键：冻结值用「恢复的意图值」，绝不能用 sizes()——构造期
        //   布局未激活，sizes() 返回 sizeHint 级假值（47,47 / 旧版现场 71,170），
        //   ctor 的 selectRow→OnSelectionChanged→setSizes(假值) 会把恢复值当场
        //   顶掉，每次打开分割条都被 sizeHint 比例重置——「布局没持久化」的根因。
        m_aSplitSizes = aPane;
    }
    // 窗口几何：限幅到屏幕可用区内（宽 ≥620、高 ≥420、不超屏；坐标移入可见
    //   区），多屏/分辨率变化后不数出屏外窗口。
    const QList<int> aWin = SplitInts(
        ReadProfileString(sCfg, pwsSect, L"RollbackWin"), 4);
    if (aWin.size() == 4) {
        const QRect rcDesk = QGuiApplication::primaryScreen()
                                 ->availableVirtualGeometry();
        const int iW = qBound(620, aWin[2], rcDesk.width());
        const int iH = qBound(420, aWin[3], rcDesk.height());
        // 2026.09.25 修「逐次上漂」：同 CRecentProjectsDialog——setGeometry 改
        //   move()+resize()（Qt6.8.3 setGeometry 客户区落点恒比请求 y 高 31px，
        //   构造期恢复→隐藏时存偏移值→逐次上漂；实测仅 move()+resize() 精确）。
        move(QPoint(qBound(rcDesk.left(), aWin[0], rcDesk.right() - iW + 1),
                    qBound(rcDesk.top(), aWin[1], rcDesk.bottom() - iH + 1)));
        resize(iW, iH);
    }
    // 2026.08.22 冻结分割条分配：选中记录点（右表填充/清空、说明文字变化）按
    //   冻结值回贴，窗体布局不随选中内容跳动。无恢复档时冻结值留空（跳过回贴），
    //   showEvent 首次显示后刷成真实分配（见彼处注释）；恢复档时上面已设意图值。
    m_pSplit->setChildrenCollapsible(false);
}
