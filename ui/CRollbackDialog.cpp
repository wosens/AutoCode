#include "CRollbackDialog.h"

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
#include <algorithm>
#include <windows.h>

#include "project/CProjectManager.h"

namespace {
// 2026.08.23 布局键改内联字面量（原 QSettings 常量随 QSettings 一并移除）：
// RollbackVerHdr / RollbackFileHdr / RollbackSplit / RollbackGeo，节名 Layout。

// 11pt 字号（与主程序 setPt11 同口径；对话框内逐控件设置）。
void SetPt11(QWidget* a_pW) {
    QFont f = a_pW->font();
    f.setPointSize(11);
    a_pW->setFont(f);
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
    QList<int> aWidthsV = {40, 118, 72, 300, 40, 40, 40};
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
    connect(m_pTableVersions, &QTableWidget::cellDoubleClicked,
            this, &CRollbackDialog::OnCellDoubleClicked);
    // 2026.08.22 用户手拖分割条时更新冻结值（否则下次点选会弹回旧位置）
    connect(m_pSplit, &QSplitter::splitterMoved, this, [this]() {
        m_aSplitSizes = m_pSplit->sizes();
    });
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
        auto* pItemSeq = fnItem(QString::number(st.iSeq));
        pItemSeq->setData(Qt::UserRole, i);
        pItemSeq->setToolTip(QStringLiteral("%1 · %2")
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
        m_pLblSummary->setText(QStringLiteral("共 %1 个记录点 · 最早 %2 · 最新 %3 · 双击记录点可直接回退")
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

void CRollbackDialog::OnCellDoubleClicked(int a_iRow, int /*a_iCol*/) {
    // 双击 = 选中并直接确认（与「回退到此记录点」等价）。
    if (a_iRow >= 0 && a_iRow < m_aVersions.size()) {
        m_pTableVersions->selectRow(a_iRow);
        accept();
    }
}

void CRollbackDialog::hideEvent(QHideEvent* a_pEvent) {
    if (!m_bLayoutSaved) {
        SaveLayoutState();
        m_bLayoutSaved = true;   // 2026.08.22 防析构二次写盘（同内容重复 IO）
    }
    QDialog::hideEvent(a_pEvent);
}

// 2026.08.23 布局存取改用 Win32 profile API（与 CProjectManager 其余 config.ini IO 同口径）。
// 旧实现用 QSettings(IniFormat)：Qt6 按 UTF-8 读写并整文件重写，会把 profile API 写下的
// ANSI(GBK) 中文值（如 [Recent] 最近打开的中文工程路径）按 UTF-8 误读成 U+FFFD 再落盘，
// 永久性打成乱码（“最近打开里面的中文全部乱码”根因）。base64 值全 ASCII，两侧等价安全。
static void WriteProfileString(const QString& a_sFile, const wchar_t* a_pwsSect,
                               const wchar_t* a_pwsKey, const QString& a_sVal) {
    WritePrivateProfileStringW(a_pwsSect, a_pwsKey, a_sVal.toStdWString().c_str(),
                               a_sFile.toStdWString().c_str());
}

static QString ReadProfileString(const QString& a_sFile, const wchar_t* a_pwsSect,
                                 const wchar_t* a_pwsKey) {
    wchar_t szBuf[8192] = {0};   // base64 几何/表头最长数百字符，8K 富余
    const DWORD dw = GetPrivateProfileStringW(a_pwsSect, a_pwsKey, L"", szBuf, 8192,
                                              a_sFile.toStdWString().c_str());
    return QString::fromWCharArray(szBuf, static_cast<int>(dw));
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
    WriteProfileString(sCfg, pwsSect, L"RollbackVerHdr",
        QString::fromLatin1(m_pTableVersions->horizontalHeader()->saveState().toBase64()));
    WriteProfileString(sCfg, pwsSect, L"RollbackFileHdr",
        QString::fromLatin1(m_pTableFiles->horizontalHeader()->saveState().toBase64()));
    WriteProfileString(sCfg, pwsSect, L"RollbackSplit",
        QString::fromLatin1(m_pSplit->saveState().toBase64()));
    WriteProfileString(sCfg, pwsSect, L"RollbackGeo",
        QString::fromLatin1(saveGeometry().toBase64()));
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
    const QByteArray aVer = QByteArray::fromBase64(
        ReadProfileString(sCfg, pwsSect, L"RollbackVerHdr").toLatin1());
    const QByteArray aFile = QByteArray::fromBase64(
        ReadProfileString(sCfg, pwsSect, L"RollbackFileHdr").toLatin1());
    const QByteArray aSplit = QByteArray::fromBase64(
        ReadProfileString(sCfg, pwsSect, L"RollbackSplit").toLatin1());
    const QByteArray aGeo = QByteArray::fromBase64(
        ReadProfileString(sCfg, pwsSect, L"RollbackGeo").toLatin1());
    if (!aVer.isEmpty()) {
        m_pTableVersions->horizontalHeader()->restoreState(aVer);
    }
    if (!aFile.isEmpty()) {
        m_pTableFiles->horizontalHeader()->restoreState(aFile);
    }
    if (!aSplit.isEmpty()) {
        m_pSplit->restoreState(aSplit);
    }
    if (!aGeo.isEmpty()) {
        restoreGeometry(aGeo);
    }
    // 2026.08.22 冻结分割条分配：此后选中记录点（右表填充/清空、说明文字变化）
    // 都按此处记录的分配强制回贴，窗体布局不再随选中内容跳动。
    m_aSplitSizes = m_pSplit->sizes();
    m_pSplit->setChildrenCollapsible(false);
}
