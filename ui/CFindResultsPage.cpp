// CFindResultsPage.cpp —— 「查找结果」独立无模式窗口（2026-11-07；同日二改：
//   内嵌 Tab → 悬浮窗，点行开文件时列表仍在，免文件/列表来回切）。
#include "CFindResultsPage.h"

#include <QDir>
#include <QFileInfo>
#include <QFont>
#include <QHeaderView>
#include <QLabel>
#include <QPushButton>
#include <QSet>
#include <QTableWidget>
#include <QMenu>
#include <QClipboard>
#include <QApplication>
#include <QVBoxLayout>
#include <algorithm>
#include <QHBoxLayout>

CFindResultsPage::CFindResultsPage(QWidget* a_pParent) : QWidget(a_pParent) {
    // 独立无模式窗口：Qt::Dialog 悬停于父链（主窗口）之上、不阻塞任何输入；
    //   点 X 只隐藏不销毁（未设 WA_DeleteOnClose），下次搜索 show/raise 复用。
    setWindowFlag(Qt::Dialog, true);
    setWindowTitle(QStringLiteral("查找结果"));
    resize(760, 480);
    BuildUi();
}

CFindResultsPage::~CFindResultsPage() {
    // 先置取消再 join——后台线程对 this 的使用止于 join 返回（投递的事件随
    // QObject 析构清队，不会在析构后触达成员）。
    m_bCancel.store(true, std::memory_order_relaxed);
    if (m_thrWorker.joinable()) {
        m_thrWorker.join();
    }
}

void CFindResultsPage::BuildUi() {
    QFont stFont = font();
    stFont.setPointSize(11);   // 界面缺省 11pt（2026 规则）
    setFont(stFont);

    // —— 结果表：两列（位置 / 文件名含相对路径）——
    m_pTable = new QTableWidget(0, 2, this);
    m_pTable->setFont(stFont);
    m_pTable->setHorizontalHeaderLabels(QStringList()
        << QStringLiteral("位置") << QStringLiteral("文件"));
    m_pTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    // 2026-11-07 多选（需求 4）：Ctrl/Shift/框选构选；多选态点击不自动定位
    //   （OnRowClicked 按选中数与修饰键判）。右键菜单：复制内容/复制路径（需求 5）。
    m_pTable->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_pTable->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_pTable, &QTableWidget::customContextMenuRequested,
            this, &CFindResultsPage::OnTableContextMenu);
    m_pTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_pTable->setAlternatingRowColors(true);
    m_pTable->verticalHeader()->setDefaultSectionSize(26);
    m_pTable->verticalHeader()->setVisible(false);
    m_pTable->horizontalHeader()->setStretchLastSection(true);
    m_pTable->setColumnWidth(0, 110);
    // 点击行 → 打开定位（用户需求 9：单击列表记录即在内置 Tab 打开并定位）。
    connect(m_pTable, &QTableWidget::cellClicked,
            this, &CFindResultsPage::OnRowClicked);
    // 回车/双击同效（键盘可达性）。
    connect(m_pTable, &QTableWidget::cellActivated,
            this, &CFindResultsPage::OnRowClicked);

    // —— 顶栏：状态行（左）+ 替换按钮组（右，替换模式可见）——
    m_pLblStat = new QLabel(this);
    m_pLblStat->setWordWrap(true);   // 长文案折行，不撑宽顶层窗口（历史教训）
    m_pBtnReplace = new QPushButton(QStringLiteral("替换"), this);
    m_pBtnReplace->setToolTip(QStringLiteral(
        "替换当前选中的条目（未选中时替换第一条）"));
    connect(m_pBtnReplace, &QPushButton::clicked, this, &CFindResultsPage::OnReplaceOne);
    m_pBtnReplaceAll = new QPushButton(QStringLiteral("替换全部"), this);
    m_pBtnReplaceAll->setToolTip(QStringLiteral(
        "替换列表中的全部条目（按文件分组写回，保持原编码与行尾）"));
    connect(m_pBtnReplaceAll, &QPushButton::clicked, this,
            &CFindResultsPage::OnReplaceAll);
    auto* pRowTop = new QHBoxLayout();
    pRowTop->addWidget(m_pLblStat, 1);
    pRowTop->addWidget(m_pBtnReplace);
    pRowTop->addWidget(m_pBtnReplaceAll);

    auto* pMain = new QVBoxLayout(this);
    pMain->addLayout(pRowTop);
    pMain->addWidget(m_pTable, 1);

    m_pLblStat->setText(QStringLiteral("尚未搜索（编辑 → 查找/替换 发起）"));
    RefreshButtons();
}

void CFindResultsPage::RefreshButtons() {
    const bool bReplace = m_stTask.m_bReplaceMode;
    const bool bHasHits = !m_aHits.isEmpty();
    const bool bRunning = m_thrWorker.joinable();
    m_pBtnReplace->setVisible(bReplace);
    m_pBtnReplaceAll->setVisible(bReplace);
    m_pBtnReplace->setEnabled(bReplace && bHasHits && !bRunning);
    m_pBtnReplaceAll->setEnabled(bReplace && bHasHits && !bRunning);
}

void CFindResultsPage::StartSearch(const SFindTask& a_stTask) {
    // 收上一轮（若在飞）：换序号 + 取消 + join 后再起新轮（一页一时刻至多一个
    //   搜索线程；旧轮已投递的完成事件被序号拦截丢弃，不会覆盖本轮/误 join
    //   新线程阻塞 UI）。
    ++m_iTaskSeq;
    m_bCancel.store(true, std::memory_order_relaxed);
    if (m_thrWorker.joinable()) {
        m_thrWorker.join();
    }
    m_bCancel.store(false, std::memory_order_relaxed);
    m_stTask = a_stTask;
    m_aHits.clear();
    m_iReplacedCount = 0;
    m_pTable->setRowCount(0);
    m_pLblStat->setText(QStringLiteral("正在搜索“%1” ...").arg(a_stTask.m_sFind));
    setWindowTitle(QStringLiteral("查找结果 - %1").arg(a_stTask.m_sFind));
    RefreshButtons();
    // 无模式窗口弹出：show（首次/上次点 X 隐藏后）+ 置顶激活——列表与文件预览
    //   同屏可见，后续点行开文件本窗不动。
    show();
    // 2026-11-07 落位防遮（用户实害：标题栏拖不动）：本窗恒悬于父链主窗之上，
    //   若盖住主窗自绘标题带（CTitleBar 高 36px，DIP），点标题带实际落在本窗 →
    //   主窗拖不动（探针 probe_findwin 重叠实验实锤）。每次弹出时压边即下移让位
    //   （仅默认/居中落位修正；用户手动拖去盖住后下次搜索自愈）。
    if (QWidget* pHost = parentWidget()) {
        QWidget* pMain = pHost->window();
        if (pMain != nullptr && pMain != this) {
            const int iTitleBottom = pMain->y() + 36;
            if (y() < iTitleBottom && y() + height() > pMain->y()) {
                move(x(), iTitleBottom);
            }
        }
    }
    raise();
    activateWindow();

    auto pHits = std::make_shared<QList<SFindHit>>();
    auto pStats = std::make_shared<SFileSearchStats>();
    const qint64 iSeq = m_iTaskSeq.load(std::memory_order_relaxed);
    const SFindTask stTask = a_stTask;   // 拷贝进线程（不依赖 this 成员可变性）
    // this 生命周期由析构 join 保证（见析构注）；投递经 Qt::QueuedConnection，
    // 若页面已析构，事件随 QObject 清队丢弃，shared_ptr 自清无泄漏。
    m_thrWorker = std::thread([this, iSeq, stTask, pHits, pStats]() {
        SFileSearchQuery stQuery;
        stQuery.m_sPattern = stTask.m_sFind;
        stQuery.m_bCase = stTask.m_bCase;
        stQuery.m_bWord = stTask.m_bWord;
        stQuery.m_bRegex = stTask.m_bRegex;
        stQuery.m_aRoots = stTask.m_aRoots;
        stQuery.m_aLogRoots = stTask.m_aLogRoots;
        stQuery.m_aLogFileNames = stTask.m_aLogFileNames;
        stQuery.m_aExcludeDirs = stTask.m_aExcludeDirs;
        CFileSearcher::Search(stQuery, *pHits, *pStats, &m_bCancel);
        const bool bCancelled = m_bCancel.load(std::memory_order_relaxed);
        QMetaObject::invokeMethod(this, [this, iSeq, pHits, pStats, bCancelled]() {
            if (iSeq != m_iTaskSeq.load(std::memory_order_relaxed)) {
                return;   // 旧轮投递：新一轮已接管，丢弃防覆盖/误 join
            }
            ApplyResults(pHits, pStats, bCancelled);
        }, Qt::QueuedConnection);
    });
}

void CFindResultsPage::ApplyResults(
        const std::shared_ptr<QList<SFindHit>>& a_pHits,
        const std::shared_ptr<SFileSearchStats>& a_pStats, bool a_bCancelled) {
    // 收线：worker 投递完事件后即将退出，join 归位线程句柄——否则 joinable()
    //   恒真，替换按钮会被误禁（当作搜索仍在跑）。此刻 join 至多等函数返回一步。
    if (m_thrWorker.joinable()) {
        m_thrWorker.join();
    }
    m_aHits = *a_pHits;
    QString sStat;
    if (a_bCancelled) {
        sStat = QStringLiteral("搜索已取消。");
    } else {
        QSet<QString> aHitFiles;
        for (const SFindHit& stHit : std::as_const(m_aHits)) {
            aHitFiles.insert(stHit.m_sFilePath);
        }
        const SFileSearchStats* pSt = a_pStats.get();
        sStat = QStringLiteral("“%1”：找到 %2 个匹配（%3 个文件）· 已扫描 %4 个文件")
                    .arg(m_stTask.m_sFind,
                         QString::number(m_aHits.size()),
                         QString::number(aHitFiles.size()),
                         QString::number(pSt != nullptr ? pSt->m_iFilesScanned : 0));
        if (pSt != nullptr && pSt->m_iFilesSkipped > 0) {
            sStat += QStringLiteral("（%1 个按二进制/超限跳过）")
                           .arg(QString::number(pSt->m_iFilesSkipped));
        }
    }
    m_pLblStat->setText(sStat);
    FillTable();
    RefreshButtons();
    if (!m_aHits.isEmpty()) {
        m_pTable->selectRow(0);
    }
}

void CFindResultsPage::FillTable() {
    m_pTable->setRowCount(0);
    for (int iRow = 0; iRow < m_aHits.size(); ++iRow) {
        const SFindHit& stHit = m_aHits.at(iRow);
        m_pTable->insertRow(iRow);
        auto* pItemPos = new QTableWidgetItem(
            QStringLiteral("%1:%2").arg(QString::number(stHit.m_iLine),
                                        QString::number(stHit.m_iCol)));
        pItemPos->setTextAlignment(Qt::AlignCenter);
        auto* pItemFile = new QTableWidgetItem(DisplayPath(stHit.m_sFilePath));
        // 行文本截断兑底：压缩/单行巨型文件的 tooltip 不能整行寒进去（Qt 富文本
        //   弹窗会卡 UI）。
        QString sLineTip = stHit.m_sLineText;
        if (sLineTip.size() > 400) {
            sLineTip = sLineTip.left(400) + QStringLiteral("…");
        }
        pItemFile->setToolTip(QStringLiteral("%1\n行 %2：%3\n%4")
                                  .arg(stHit.m_sFilePath,
                                       QString::number(stHit.m_iLine),
                                       QString::number(stHit.m_iCol),
                                       sLineTip));
        pItemPos->setFlags(pItemPos->flags() & ~Qt::ItemIsEditable);
        pItemFile->setFlags(pItemFile->flags() & ~Qt::ItemIsEditable);
        m_pTable->setItem(iRow, 0, pItemPos);
        m_pTable->setItem(iRow, 1, pItemFile);
    }
}

QString CFindResultsPage::DisplayPath(const QString& a_sAbsPath) const {
    // 工程根下 → 工程相对路径；关联根下 → 「根目录名/相对」；都不沾 → 绝对路径。
    //   relativeFilePath 对根外路径回 "../.." 形态，startsWith("..") 一并拦下。
    const QString sClean = QDir::cleanPath(a_sAbsPath);
    const QString sProj = QDir::cleanPath(m_stTask.m_sProjectDir);
    if (!sProj.isEmpty()) {
        const QString sRel = QDir(sProj).relativeFilePath(sClean);
        if (!sRel.startsWith(QStringLiteral("..")) && sRel != QStringLiteral(".")) {
            return sRel;
        }
    }
    for (const QString& sRoot : std::as_const(m_stTask.m_aRoots)) {
        if (sRoot.isEmpty()) {
            continue;
        }
        const QString sRel = QDir(QDir::cleanPath(sRoot)).relativeFilePath(sClean);
        if (!sRel.startsWith(QStringLiteral("..")) && sRel != QStringLiteral(".")) {
            return QFileInfo(QDir::cleanPath(sRoot)).fileName()
                + QLatin1Char('/') + sRel;
        }
    }
    // 2026-11-07 日志通道根（范围=任务和助手日志时 m_aRoots 为空）：关联根下
    //   日志同样以「根目录名/相对」呈现（工程根下已由上面的工程根分支覆盖）。
    for (const QString& sLogRoot : std::as_const(m_stTask.m_aLogRoots)) {
        if (sLogRoot.isEmpty()) {
            continue;
        }
        const QString sRel = QDir(QDir::cleanPath(sLogRoot)).relativeFilePath(sClean);
        if (!sRel.startsWith(QStringLiteral("..")) && sRel != QStringLiteral(".")) {
            return QFileInfo(QDir::cleanPath(sLogRoot)).fileName()
                + QLatin1Char('/') + sRel;
        }
    }
    return sClean;
}

void CFindResultsPage::OnRowClicked(int a_iRow) {
    // 2026-11-07 多选（需求 4）：正在构选（Ctrl/Shift 按住）或已选多行时不自动
    //   定位（多选的目的是批量复制/审阅）；单选点击才打开文件定位。
    if (a_iRow < 0 || a_iRow >= m_aHits.size()) {
        return;
    }
    const QItemSelectionModel* pSel = m_pTable->selectionModel();
    if (pSel != nullptr && pSel->selectedRows().size() > 1) {
        return;
    }
    if ((QApplication::keyboardModifiers()
            & (Qt::ControlModifier | Qt::ShiftModifier)) != 0) {
        return;
    }
    const SFindHit& stHit = m_aHits.at(a_iRow);
    // 同文同行重复序（md 行锚点定位用）：同文件里此前出现过几次【相同行文本且
    //   不同源行】的命中——同源行多命中不计（同一渲染块内按列出现序区分）。
    int iLineDup = 0;
    for (int j = 0; j < a_iRow && j < m_aHits.size(); ++j) {
        const SFindHit& stPrev = m_aHits.at(j);
        if (stPrev.m_sFilePath == stHit.m_sFilePath
                && stPrev.m_sLineText == stHit.m_sLineText
                && stPrev.m_iLine < stHit.m_iLine) {
            ++iLineDup;
        }
    }
    emit sigOpenHit(stHit.m_sFilePath, stHit.m_iLine, stHit.m_iCol, stHit.m_iLen,
                    stHit.m_sMatched, stHit.m_iMatchOrd,
                    stHit.m_sLineText, iLineDup);
}

QList<int> CFindResultsPage::SelectedRowsSorted() const {
    QList<int> aRows;
    const QItemSelectionModel* pSel = m_pTable->selectionModel();
    if (pSel == nullptr) {
        return aRows;
    }
    const QModelIndexList aIdx = pSel->selectedRows();
    for (const QModelIndex& stIdx : aIdx) {
        aRows.append(stIdx.row());
    }
    std::sort(aRows.begin(), aRows.end());
    return aRows;
}

void CFindResultsPage::OnTableContextMenu(const QPoint& a_stPos) {
    // 2026-11-07 右键菜单（需求 5）：复制内容（相对路径:行:列，每选中行一条）/
    //   复制路径（选中文件去重绝对路径）。无选中时置灰。
    const QPoint stGlobal = m_pTable->viewport()->mapToGlobal(a_stPos);
    const QList<int> aRows = SelectedRowsSorted();
    QMenu stMenu(this);
    QAction* pActCopyContent = stMenu.addAction(QStringLiteral("复制内容"));
    QAction* pActCopyPath = stMenu.addAction(QStringLiteral("复制路径"));
    pActCopyContent->setEnabled(!aRows.isEmpty());
    pActCopyPath->setEnabled(!aRows.isEmpty());
    QAction* pAct = stMenu.exec(stGlobal);
    if (pAct == nullptr) {
        return;
    }
    if (pAct == pActCopyContent) {
        QStringList aLines;
        for (int iRow : aRows) {
            if (iRow < 0 || iRow >= m_aHits.size()) {
                continue;
            }
            const SFindHit& stHit = m_aHits.at(iRow);
            aLines << QStringLiteral("%1:%2:%3")
                          .arg(DisplayPath(stHit.m_sFilePath),
                               QString::number(stHit.m_iLine),
                               QString::number(stHit.m_iCol));
        }
        QApplication::clipboard()->setText(aLines.join(QLatin1Char('\n')));
    } else if (pAct == pActCopyPath) {
        // 去重（同文件多命中只复制一次路径），保持选中行序。
        QStringList aPaths;
        for (int iRow : aRows) {
            if (iRow < 0 || iRow >= m_aHits.size()) {
                continue;
            }
            const QString sPath = m_aHits.at(iRow).m_sFilePath;
            if (!aPaths.contains(sPath)) {
                aPaths << sPath;
            }
        }
        QApplication::clipboard()->setText(aPaths.join(QLatin1Char('\n')));
    }
}

void CFindResultsPage::OnReplaceOne() {
    if (m_thrWorker.joinable() || m_aHits.isEmpty()) {
        return;
    }
    // 未选中 = 第一条（用户需求 10）。
    int iRow = m_pTable->currentRow();
    if (iRow < 0 || iRow >= m_aHits.size()) {
        iRow = 0;
    }
    const SFindHit stHit = m_aHits.at(iRow);
    QString sErr;
    if (!CFileSearcher::ReplaceOne(stHit, m_stTask.m_sReplace, sErr)) {
        m_pLblStat->setText(QStringLiteral("替换失败：%1").arg(sErr));
        return;
    }
    ++m_iReplacedCount;
    m_aHits.removeAt(iRow);
    m_pTable->removeRow(iRow);
    if (iRow < m_pTable->rowCount()) {
        m_pTable->selectRow(iRow);
    } else if (m_pTable->rowCount() > 0) {
        m_pTable->selectRow(m_pTable->rowCount() - 1);
    }
    m_pLblStat->setText(QStringLiteral(
        "已替换 %1 处（本任务累计 %2 处），剩余 %3 个匹配")
        .arg(QStringLiteral("1"), QString::number(m_iReplacedCount),
             QString::number(m_aHits.size())));
    emit sigFileRewritten(stHit.m_sFilePath);
    RefreshButtons();
}

void CFindResultsPage::OnReplaceAll() {
    if (m_thrWorker.joinable() || m_aHits.isEmpty()) {
        return;
    }
    QStringList aErrs;
    const int nDone = CFileSearcher::ReplaceAll(m_aHits, m_stTask.m_sReplace, &aErrs);
    QSet<QString> aRewritten;
    for (const SFindHit& stHit : std::as_const(m_aHits)) {
        aRewritten.insert(stHit.m_sFilePath);
    }
    m_iReplacedCount += nDone;
    m_aHits.clear();
    m_pTable->setRowCount(0);
    QString sStat = QStringLiteral("替换全部完成：共替换 %1 处").arg(nDone);
    if (!aErrs.isEmpty()) {
        sStat += QStringLiteral("，%1 个文件失败（详见下方）\n%2")
                     .arg(QString::number(aErrs.size()),
                          aErrs.join(QLatin1Char('\n')));
    }
    m_pLblStat->setText(sStat);
    for (const QString& sFile : std::as_const(aRewritten)) {
        emit sigFileRewritten(sFile);
    }
    RefreshButtons();
}
